// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::collections::{HashMap, HashSet};
use std::sync::{Arc, OnceLock, RwLock as StdRwLock};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use anyhow::Result;
use futures_util::StreamExt;
use matrix_sdk::latest_events::LatestEventValue;
use matrix_sdk::ruma::events::room::message::{AudioMessageEventContent, MessageType};
use matrix_sdk::ruma::events::tag::TagName;
use matrix_sdk::ruma::OwnedRoomId;
use matrix_sdk::{Client, Room, RoomMemberships, RoomState};
use tokio::sync::RwLock;
use tracing::warn;

use crate::folder_service::FolderService;
use crate::local_cache_service::LocalCacheService;
use crate::types::{
    MembershipState, MessageContent, RoomNotificationMode, RoomSummary, SendState, TimelineItem,
};
use crate::unread_count_service::UnreadCountService;

pub(crate) struct RoomSummaryService;

pub(crate) struct RoomSummaryRefreshContext<'a> {
    pub(crate) timeline_cache: &'a RwLock<HashMap<String, Vec<TimelineItem>>>,
    pub(crate) rooms_cache: &'a Arc<RwLock<Vec<RoomSummary>>>,
    pub(crate) notification_overrides: &'a HashMap<String, RoomNotificationMode>,
    pub(crate) presence_snapshot: &'a HashMap<String, u32>,
    pub(crate) local_cache: &'a LocalCacheService,
}

/// Stable sort timestamp for an invited room. Invited rooms have stripped
/// state with no real event timestamp, so stamp each invite once on first sight
/// and reuse it. Using `SystemTime::now()` on every rebuild made pending invites
/// constantly re-sort to the top of the room list.
fn invite_sort_timestamp(room_id: &str) -> SystemTime {
    static SEEN: std::sync::OnceLock<
        std::sync::Mutex<std::collections::HashMap<String, SystemTime>>,
    > = std::sync::OnceLock::new();
    let map = SEEN.get_or_init(|| std::sync::Mutex::new(std::collections::HashMap::new()));
    let mut guard = map.lock().unwrap_or_else(|e| e.into_inner());
    *guard
        .entry(room_id.to_string())
        .or_insert_with(SystemTime::now)
}

/// Outcome of scanning a room's cached timeline for the rooms-list row.
/// See [`RoomSummaryService::select_cached_preview`].
#[derive(Debug, Clone, PartialEq)]
pub(crate) enum CachePreviewOutcome {
    /// Newest previewable (non-deleted, non-system) message — drives the row's
    /// text, sender, timestamp, and send/outgoing state.
    Previewable {
        text: String,
        sender: String,
        timestamp: SystemTime,
        is_outgoing: bool,
        send_state: SendState,
    },
    /// No previewable message (system-only, all redacted, or all UTD). Only the
    /// newest item's row state is meaningful; text/timestamp stay unset.
    SystemOnly {
        is_outgoing: bool,
        send_state: SendState,
        is_last_service: bool,
    },
    /// The cache is empty.
    Empty,
}

/// Whether a room belongs in the chat list. Spaces are joined rooms as far as the SDK is concerned,
/// but they hold no timeline — without this they render as permanently empty rows.
pub(crate) fn belongs_in_chat_list(room: &matrix_sdk::Room, state: RoomState) -> bool {
    room.state() == state && !room.is_space()
}

/// Process-global reverse index `room_id -> [space_id]` (a room's joined-space
/// membership, recursive through nested joined sub-spaces). Rebuilt on each full
/// `build_rooms_cache`; read per-room in `room_to_summary`. Same
/// no-plumbing-needed pattern as `invite_sort_timestamp` / the folder registry.
fn space_membership() -> &'static StdRwLock<HashMap<String, Vec<String>>> {
    static MEMBERSHIP: OnceLock<StdRwLock<HashMap<String, Vec<String>>>> = OnceLock::new();
    MEMBERSHIP.get_or_init(|| StdRwLock::new(HashMap::new()))
}

fn space_ids_for_room(room_id: &str) -> Vec<String> {
    space_membership()
        .read()
        .map(|m| m.get(room_id).cloned().unwrap_or_default())
        .unwrap_or_default()
}

/// Recompute the joined-space → descendant-room index and publish it globally.
///
/// A room belongs to space `S` if it is reachable from `S` through `m.space.child`
/// edges, descending only into **joined** sub-spaces (an unjoined sub-space has
/// no local child state, so recursion naturally stops there). Every joined space
/// that can reach a room lists it, so a room nested under several ancestor spaces
/// appears under each — Element-like "all descendants".
async fn recompute_space_membership(client: &Client) {
    use matrix_sdk::ruma::events::space::child::SpaceChildEventContent;

    // Direct children of each joined space (removed children — empty `via` — are
    // skipped per the Matrix convention).
    let mut joined_spaces: HashSet<OwnedRoomId> = HashSet::new();
    let mut children: HashMap<OwnedRoomId, Vec<OwnedRoomId>> = HashMap::new();
    for room in client.rooms() {
        if room.state() != RoomState::Joined || !room.is_space() {
            continue;
        }
        let space_id = room.room_id().to_owned();
        joined_spaces.insert(space_id.clone());
        let mut kids = Vec::new();
        let events = room
            .get_state_events_static::<SpaceChildEventContent>()
            .await
            .unwrap_or_default();
        for raw in events {
            let Ok(ev) = raw.deserialize() else {
                continue;
            };
            let child_id = ev.state_key().to_owned();
            let Some(original) = ev.as_sync().and_then(|s| s.as_original()) else {
                continue; // redacted/stripped -> treat as removed
            };
            if original.content.via.is_empty() {
                continue; // removed child
            }
            kids.push(child_id);
        }
        children.insert(space_id, kids);
    }

    // DFS descendants from each space; collect the reachable non-space rooms.
    let mut reverse: HashMap<String, Vec<String>> = HashMap::new();
    for space in &joined_spaces {
        let mut visited: HashSet<OwnedRoomId> = HashSet::new();
        let mut stack: Vec<OwnedRoomId> = children.get(space).cloned().unwrap_or_default();
        while let Some(node) = stack.pop() {
            if !visited.insert(node.clone()) {
                continue;
            }
            if joined_spaces.contains(&node) {
                // A joined sub-space: descend, but a space is not itself a chat-list row.
                if let Some(kids) = children.get(&node) {
                    stack.extend(kids.iter().cloned());
                }
            } else {
                reverse
                    .entry(node.to_string())
                    .or_default()
                    .push(space.to_string());
            }
        }
    }

    if let Ok(mut guard) = space_membership().write() {
        *guard = reverse;
    }
}

impl RoomSummaryService {
    pub(crate) async fn build_rooms_cache(
        client: &Client,
        timeline_cache: &RwLock<HashMap<String, Vec<TimelineItem>>>,
        notification_overrides: &HashMap<String, RoomNotificationMode>,
        presence_snapshot: &HashMap<String, u32>,
    ) -> Result<Vec<RoomSummary>> {
        let rooms = client.rooms();

        // Refresh the joined-space membership index before building summaries so
        // each room's space_ids reflect the current space graph.
        recompute_space_membership(client).await;

        // Clone the entire timeline cache snapshot and immediately release
        // the read lock.  room_to_summary makes async network calls
        // (display_name, is_direct, notification_mode) which would hold the
        // lock across .await, blocking writers (and then Qt block_on readers).
        let tc_snapshot = {
            let tc = timeline_cache.read().await;
            tc.clone()
        }; // read lock released

        // Parallelize room summary construction for speed.
        use futures_util::stream::FuturesUnordered;

        let futs: FuturesUnordered<_> = rooms
            .iter()
            .filter(|room| belongs_in_chat_list(room, RoomState::Joined))
            .map(|room| {
                let tc = &tc_snapshot;
                let overrides = notification_overrides;
                let presence = presence_snapshot;
                async move { Self::room_to_summary(room, tc, overrides, presence).await }
            })
            .collect();

        let mut summaries: Vec<RoomSummary> = futs
            .filter_map(|result| async { result.ok() })
            .collect()
            .await;

        // Also include invited rooms.
        let invite_futs: FuturesUnordered<_> = rooms
            .iter()
            .filter(|room| belongs_in_chat_list(room, RoomState::Invited))
            .map(|room| async move { Self::invited_room_to_summary(room).await })
            .collect();

        let mut invite_summaries: Vec<RoomSummary> = invite_futs
            .filter_map(|result| async { result.ok() })
            .collect()
            .await;

        summaries.append(&mut invite_summaries);

        summaries.sort_by_key(|b| std::cmp::Reverse(b.last_event_timestamp));
        Ok(summaries)
    }

    pub(crate) async fn refresh_rooms_cache_by_ids(
        client: &Client,
        room_ids: &HashSet<String>,
        context: &RoomSummaryRefreshContext<'_>,
    ) -> Result<()> {
        if room_ids.is_empty() {
            return Ok(());
        }

        // Clone only the needed rooms' timeline data and release the read
        // lock before calling room_to_summary (which makes async network
        // calls).  Same pattern as build_rooms_cache and
        // refresh_room_summary_cache.
        let tc_snapshot = {
            let tc = context.timeline_cache.read().await;
            let mut snapshot = HashMap::with_capacity(room_ids.len());
            for room_id in room_ids {
                if let Some(items) = tc.get(room_id) {
                    snapshot.insert(room_id.clone(), items.clone());
                }
            }
            snapshot
        }; // read lock released

        let mut updated = Vec::<RoomSummary>::new();
        let mut removed = HashSet::<String>::new();

        for room_id in room_ids {
            let parsed_room_id: OwnedRoomId = match room_id.as_str().try_into() {
                Ok(id) => id,
                Err(_) => {
                    removed.insert(room_id.clone());
                    continue;
                }
            };

            let Some(room) = client.get_room(&parsed_room_id) else {
                removed.insert(room_id.clone());
                continue;
            };

            // A space falls through to the else arm and is evicted, so one that slipped into an
            // older cache gets cleaned out on the next refresh.
            if belongs_in_chat_list(&room, RoomState::Joined) {
                if let Ok(summary) = Self::room_to_summary(
                    &room,
                    &tc_snapshot,
                    context.notification_overrides,
                    context.presence_snapshot,
                )
                .await
                {
                    updated.push(summary);
                }
            } else if belongs_in_chat_list(&room, RoomState::Invited) {
                if let Ok(summary) = Self::invited_room_to_summary(&room).await {
                    updated.push(summary);
                }
            } else {
                removed.insert(room_id.clone());
                continue;
            }
        }

        if updated.is_empty() && removed.is_empty() {
            return Ok(());
        }

        {
            let mut cache = context.rooms_cache.write().await;
            if !removed.is_empty() {
                cache.retain(|room| !removed.contains(&room.room_id));
            }

            for mut summary in updated {
                if let Some(existing) = cache
                    .iter_mut()
                    .find(|room| room.room_id == summary.room_id)
                {
                    Self::preserve_preview_if_blank(&mut summary, existing);
                    *existing = summary;
                } else {
                    cache.push(summary);
                }
            }

            cache.sort_by_key(|b| std::cmp::Reverse(b.last_event_timestamp));
        }
        context.local_cache.schedule_rooms_snapshot();
        Ok(())
    }

    pub(crate) async fn refresh_room_summary_cache(
        room: &Room,
        room_id: &str,
        timeline_cache: &Arc<RwLock<HashMap<String, Vec<TimelineItem>>>>,
        rooms_cache: &Arc<RwLock<Vec<RoomSummary>>>,
        notification_overrides: &HashMap<String, RoomNotificationMode>,
        presence_snapshot: &HashMap<String, u32>,
    ) {
        if !belongs_in_chat_list(room, RoomState::Joined) {
            let mut rooms = rooms_cache.write().await;
            rooms.retain(|r| r.room_id != room_id);
            return;
        }

        // Clone only this room's timeline data out of the read lock so we
        // don't hold it across the async network calls in room_to_summary
        // (display_name, is_direct, notification_mode).  Holding the lock
        // across .await causes writer starvation -> blocks the Qt main
        // thread's block_on(get_timeline) behind a queued write lock.
        let snapshot = {
            let tc = timeline_cache.read().await;
            tc.get(room_id).cloned().unwrap_or_default()
        }; // read lock released here
        let mut snapshot_map = HashMap::with_capacity(1);
        snapshot_map.insert(room_id.to_string(), snapshot);

        let mut summary = match Self::room_to_summary(
            room,
            &snapshot_map,
            notification_overrides,
            presence_snapshot,
        )
        .await
        {
            Ok(summary) => summary,
            Err(e) => {
                warn!("Failed to refresh room summary for {room_id}: {e}");
                return;
            }
        };

        let mut rooms = rooms_cache.write().await;
        if let Some(existing) = rooms.iter_mut().find(|r| r.room_id == room_id) {
            Self::preserve_preview_if_blank(&mut summary, existing);
            *existing = summary;
        } else {
            rooms.push(summary);
        }
        rooms.sort_by_key(|b| std::cmp::Reverse(b.last_event_timestamp));
    }

    async fn room_to_summary(
        room: &Room,
        timeline_cache: &HashMap<String, Vec<TimelineItem>>,
        notification_overrides: &HashMap<String, RoomNotificationMode>,
        presence_snapshot: &HashMap<String, u32>,
    ) -> Result<RoomSummary> {
        let room_id = room.room_id().to_string();
        // Use cached display name (instant, no network) with fallback.
        let display_name = room
            .cached_display_name()
            .map(|dn| dn.to_string())
            .unwrap_or_else(|| room_id.clone());

        // Use sync is_direct check (instant, reads cached DM targets).
        let is_direct = room.direct_targets_length() > 0;

        let (avatar_url, avatar_entity_id) = Self::resolve_room_list_avatar(room, is_direct).await;

        let sdk_unread = UnreadCountService::room_counts_from_sdk(room);
        let unread_count = sdk_unread.unread_count;

        let notification_mode =
            Self::notification_mode(room, &room_id, notification_overrides).await;
        let is_muted = notification_mode == RoomNotificationMode::Mute;
        let highlight_count = sdk_unread.highlight_count;

        // Try SDK's latest_event first, fall back to timeline cache.
        let (mut last_text, mut last_sender, mut last_timestamp, last_event_outgoing) =
            Self::extract_last_event(room).await;
        let mut is_outgoing = false;
        let mut is_last_service = false;
        let mut last_send_state = SendState::Sent;
        if last_text.is_empty() {
            if let Some(items) = timeline_cache.get(&room_id) {
                match Self::select_cached_preview(items) {
                    CachePreviewOutcome::Previewable {
                        text,
                        sender,
                        timestamp,
                        is_outgoing: outgoing,
                        send_state,
                    } => {
                        last_text = text;
                        last_sender = sender;
                        last_timestamp = timestamp;
                        is_outgoing = outgoing;
                        last_send_state = send_state;
                        is_last_service = false;
                    }
                    // Cache holds only system/deleted/UTD events (no previewable
                    // message): reflect the newest item's send/outgoing state for
                    // the row but leave the text/timestamp unset (UNIX_EPOCH) so the
                    // room is not ordered or labelled by a system event's time.
                    CachePreviewOutcome::SystemOnly {
                        is_outgoing: outgoing,
                        send_state,
                        is_last_service: service,
                    } => {
                        is_outgoing = outgoing;
                        last_send_state = send_state;
                        is_last_service = service;
                    }
                    CachePreviewOutcome::Empty => {}
                }
            }
        } else {
            // SDK latest_event supplied the preview text; its sender determines the
            // outgoing/"You:" state — correct even before the room's timeline is
            // cached (first load), which the cache-only derivation got wrong.
            is_outgoing = last_event_outgoing;
            // Still reflect real receipt (send) state from the newest cached item
            // when the timeline is already cached.
            if let Some(items) = timeline_cache.get(&room_id) {
                if let Some(last) = items.last() {
                    last_send_state = last.send_state;
                }
            }
        }

        let member_count = room.joined_members_count();

        // Check if the current user can pin/unpin messages in this room.
        let can_pin_messages = {
            let own_uid = room.own_user_id().to_owned();
            match room.get_member_no_sync(&own_uid).await {
                Ok(Some(member)) => member.can_pin_or_unpin_event(),
                _ => false,
            }
        };

        // Folder membership = the room's `u.*` tags, mapped to runtime handles.
        let filter_ids = FolderService::folder_handles_for_room(room).await;
        let space_ids = space_ids_for_room(&room_id);

        let peer_presence = if is_direct {
            let target = room.direct_targets();
            let uid = target
                .iter()
                .next()
                .map(|u| u.to_string())
                .unwrap_or_default();
            presence_snapshot.get(&uid).copied().unwrap_or(0)
        } else {
            0
        };

        // `is_favourite()` is a cached bool, but the ordering the user chose lives in
        // the tag's `order` field, which needs a store read — so only pay for it on
        // rooms that are actually pinned.
        let is_pinned = room.is_favourite();
        let pinned_order = if is_pinned {
            room.tags()
                .await
                .ok()
                .flatten()
                .and_then(|tags| tags.get(&TagName::Favorite).and_then(|info| info.order))
        } else {
            None
        };

        // One read: two calls could straddle a join-rule sync and yield the
        // known-but-private combination, which merge_sticky_previews trusts.
        let join_rule_public = room.is_public();

        Ok(RoomSummary {
            room_id,
            display_name,
            canonical_alias: room.canonical_alias().map(|alias| alias.to_string()),
            avatar_url,
            avatar_entity_id,
            last_event_text: last_text,
            last_event_sender: last_sender,
            last_event_timestamp: last_timestamp,
            unread_count,
            highlight_count,
            notification_mode,
            is_muted,
            is_pinned,
            pinned_order,
            is_marked_unread: room.is_marked_unread(),
            is_direct,
            is_public: join_rule_public.unwrap_or(false),
            is_public_known: join_rule_public.is_some(),
            filter_ids,
            space_ids,
            is_last_event_outgoing: is_outgoing,
            is_last_event_service: is_last_service,
            last_event_send_state: last_send_state,
            member_count,
            can_pin_messages,
            peer_presence,
            membership: MembershipState::Join,
            inviter_user_id: String::new(),
            inviter_display_name: String::new(),
            inviter_avatar_url: String::new(),
            room_topic: room.topic().unwrap_or_default(),
        })
    }

    async fn notification_mode(
        room: &Room,
        room_id: &str,
        notification_overrides: &HashMap<String, RoomNotificationMode>,
    ) -> RoomNotificationMode {
        if let Some(&mode) = notification_overrides.get(room_id) {
            return mode;
        }
        let sdk_mode = room.notification_mode().await;
        match sdk_mode {
            Some(
                matrix_sdk::notification_settings::RoomNotificationMode::MentionsAndKeywordsOnly,
            ) => RoomNotificationMode::MentionsOnly,
            Some(matrix_sdk::notification_settings::RoomNotificationMode::Mute) => {
                RoomNotificationMode::Mute
            }
            _ => RoomNotificationMode::AllMessages,
        }
    }

    /// Build a RoomSummary for an invited (not-yet-joined) room.
    async fn invited_room_to_summary(room: &Room) -> Result<RoomSummary> {
        let room_id = room.room_id().to_string();

        let display_name = room
            .cached_display_name()
            .map(|dn| dn.to_string())
            .unwrap_or_else(|| room_id.clone());

        let is_direct = room.direct_targets_length() > 0;

        let avatar_url = room.avatar_url().map(|u| u.to_string());

        let avatar_entity_id = if is_direct {
            room.direct_targets()
                .iter()
                .next()
                .map(|u| u.to_string())
                .unwrap_or_else(|| room_id.clone())
        } else {
            room_id.clone()
        };

        // Extract inviter info from room members.
        let mut inviter_user_id = String::new();
        let mut inviter_display_name = String::new();
        let mut inviter_avatar_url = String::new();

        if let Ok(members) = room.members(RoomMemberships::empty()).await {
            for member in &members {
                if member.user_id() != room.own_user_id()
                    && *member.membership()
                        == matrix_sdk::ruma::events::room::member::MembershipState::Join
                {
                    inviter_user_id = member.user_id().to_string();
                    inviter_display_name = member
                        .display_name()
                        .unwrap_or_else(|| member.user_id().as_str())
                        .to_string();
                    inviter_avatar_url = member
                        .avatar_url()
                        .map(|u| u.to_string())
                        .unwrap_or_default();
                    break;
                }
            }
        }

        let topic = room.topic().unwrap_or_default();
        let member_count = room.joined_members_count();
        let invite_timestamp = invite_sort_timestamp(&room_id);
        let join_rule_public = room.is_public();

        Ok(RoomSummary {
            room_id,
            display_name,
            canonical_alias: room.canonical_alias().map(|a| a.to_string()),
            avatar_url,
            avatar_entity_id,
            last_event_text: if inviter_display_name.is_empty() {
                String::from("Invitation")
            } else {
                format!("{} invited you", inviter_display_name)
            },
            last_event_sender: inviter_display_name.clone(),
            last_event_timestamp: invite_timestamp,
            unread_count: 0,
            highlight_count: 1,
            notification_mode: RoomNotificationMode::AllMessages,
            is_muted: false,
            is_pinned: false,
            pinned_order: None,
            is_marked_unread: false,
            is_direct,
            is_public: join_rule_public.unwrap_or(false),
            is_public_known: join_rule_public.is_some(),
            filter_ids: vec![],
            space_ids: Vec::new(),
            is_last_event_outgoing: false,
            is_last_event_service: true,
            last_event_send_state: SendState::Sent,
            member_count,
            can_pin_messages: false,
            peer_presence: 0,
            membership: MembershipState::Invite,
            inviter_user_id,
            inviter_display_name,
            inviter_avatar_url,
            room_topic: topic,
        })
    }

    /// Resolve the avatar URL and entity ID for a room in the chat list.
    ///
    /// For non-DM rooms: uses the room avatar (`m.room.avatar`).
    /// For DM rooms without a room avatar: falls back to the other member's
    /// profile avatar.
    ///
    /// Returns `(avatar_url, avatar_entity_id)` where `avatar_entity_id` is
    /// the user ID (for DM fallback) or room ID, used for stable placeholder
    /// color selection.
    async fn resolve_room_list_avatar(room: &Room, is_direct: bool) -> (Option<String>, String) {
        let room_id = room.room_id().to_string();

        // For DMs, seed the placeholder colour from the peer's user id. We read
        // it from direct_targets() (m.direct account data) which is instant and
        // cached — unlike room.members(), which on a cold room timed out and
        // fell back to the room-id seed, so the chat-list avatar flickered to a
        // different colour (and lagged the real avatar) on first open. A stable
        // seed keeps the chat-list avatar matching the conversation.
        if is_direct {
            let targets = room.direct_targets();
            if let Some(peer) = targets.iter().next() {
                let peer_id = peer.to_string();
                if room.avatar_url().is_some() {
                    return (room.avatar_url().map(|u| u.to_string()), peer_id);
                }
                // No room avatar: use the peer's profile avatar from the local
                // store (get_member_no_sync — cached, no network/timeout). Stays
                // None (generated, stable seed) until the member is cached.
                // direct_targets() yields DirectUserIdentifier (may be a 3pid);
                // only a real user id can resolve a member.
                let peer_avatar = match peer.as_user_id() {
                    Some(user_id) => match room.get_member_no_sync(user_id).await {
                        Ok(Some(member)) => member.avatar_url().map(|u| u.to_string()),
                        _ => None,
                    },
                    None => None,
                };
                return (peer_avatar, peer_id);
            }
        }

        // Room avatar (or no avatar for non-DM).
        let avatar_url = room.avatar_url().map(|u| u.to_string());
        (avatar_url, room_id)
    }

    /// Extract last event info from the room's latest event value.
    async fn extract_last_event(room: &Room) -> (String, String, SystemTime, bool) {
        let default = (String::new(), String::new(), UNIX_EPOCH, false);

        // 0.18's Latest Event API returns an enum. NOTE: `Remote(_)` does NOT imply
        // a previewable message — the SDK considers some state events (e.g. the
        // user's own `m.room.member` join) "latest-worthy". A bodyless state event
        // yields a blank row here; the deterministic event-cache scan that recovers
        // those lives in the THROTTLED backfill path (`backfill_blank_preview`), NOT
        // here — `extract_last_event` runs for every room in the fully-parallel
        // `build_rooms_cache`, where a per-room event-cache load would stall the
        // whole rooms-list build on cold start.
        let LatestEventValue::Remote(timeline_event) = room.latest_event() else {
            return default;
        };
        let Ok(event) = timeline_event.raw().deserialize() else {
            return default;
        };
        let timestamp_ms: u64 = event.origin_server_ts().get().into();
        let timestamp = UNIX_EPOCH + Duration::from_millis(timestamp_ms);
        // Own-message detection for the "You:" rooms-list prefix.
        let is_outgoing = event.sender() == room.own_user_id();

        if let Some(text) = Self::extract_event_body(&event) {
            let sender_name = Self::resolve_sender_name(room, event.sender()).await;
            return (text, sender_name, timestamp, is_outgoing);
        }
        // Calls are room-specific activity worth surfacing (excluded from
        // extract_event_body); label them with the call's real timestamp.
        if Self::is_call_event(&event) {
            return (
                Self::call_label(is_outgoing),
                String::new(),
                timestamp,
                false,
            );
        }
        default
    }

    /// Resolve a sender's display name from the room member store (no network);
    /// falls back to the raw MXID.
    async fn resolve_sender_name(room: &Room, sender: &matrix_sdk::ruma::UserId) -> String {
        match room.get_member_no_sync(sender).await {
            Ok(Some(member)) => member
                .display_name()
                .unwrap_or_else(|| sender.as_str())
                .to_string(),
            _ => sender.as_str().to_string(),
        }
    }

    /// Newest previewable message in the room's in-memory event cache, scanning
    /// backward. Bypasses `latest_event()` (which can pick a bodyless state event
    /// like the own-join and shadow a real message). Returns None if the cache has
    /// no previewable message in memory (then a backward-pagination backfill can
    /// pull older ones in and a later scan will find them).
    pub(crate) async fn scan_event_cache_for_preview(
        room: &Room,
    ) -> Option<(String, String, SystemTime, bool)> {
        let (event_cache, _drop_handles) = room.event_cache().await.ok()?;
        let events = event_cache.events().await.ok()?;
        for event in events.iter().rev() {
            let Ok(any) = event.raw().deserialize() else {
                continue;
            };
            if let Some(text) = Self::extract_event_body(&any) {
                let timestamp_ms: u64 = any.origin_server_ts().get().into();
                let timestamp = UNIX_EPOCH + Duration::from_millis(timestamp_ms);
                let is_outgoing = any.sender() == room.own_user_id();
                let sender_name = Self::resolve_sender_name(room, any.sender()).await;
                return Some((text, sender_name, timestamp, is_outgoing));
            }
        }
        None
    }

    /// Write a backfilled preview straight into a room's summary in `rooms_cache`,
    /// then re-sort by timestamp so the now-dated room leaves the blank bottom.
    /// Used by the LatestEvents updater when the event-cache scan found a message
    /// that `latest_event()` shadowed with a state event.
    pub(crate) async fn set_room_preview(
        rooms_cache: &Arc<RwLock<Vec<RoomSummary>>>,
        room_id: &str,
        text: String,
        sender: String,
        timestamp: SystemTime,
        is_outgoing: bool,
    ) {
        let mut rooms = rooms_cache.write().await;
        if let Some(room) = rooms.iter_mut().find(|r| r.room_id == room_id) {
            room.last_event_text = text;
            room.last_event_sender = sender;
            room.last_event_timestamp = timestamp;
            room.is_last_event_outgoing = is_outgoing;
            room.is_last_event_service = false;
        }
        rooms.sort_by_key(|b| std::cmp::Reverse(b.last_event_timestamp));
    }

    /// True if `event` is a call the room list should surface — a call invite or
    /// an MSC4075 RTC notification (the two call kinds the timeline also renders
    /// as "started a call"). Excluded from `extract_event_body` (and thus from
    /// notifications), so the rooms-list path labels them explicitly.
    fn is_call_event(event: &matrix_sdk::ruma::events::AnySyncTimelineEvent) -> bool {
        use matrix_sdk::ruma::events::{AnySyncMessageLikeEvent, AnySyncTimelineEvent};
        matches!(
            event,
            AnySyncTimelineEvent::MessageLike(
                AnySyncMessageLikeEvent::CallInvite(_)
                    | AnySyncMessageLikeEvent::RtcNotification(_)
            )
        )
    }

    /// Rooms-list preview label for a call, by direction.
    fn call_label(is_outgoing: bool) -> String {
        if is_outgoing {
            String::from("Outgoing call")
        } else {
            String::from("Incoming call")
        }
    }

    /// Keep a room's last real preview when a freshly built summary came back
    /// blank. A system event (membership/avatar/name/topic change) as a room's
    /// newest event yields no preview text and an epoch timestamp; on its own
    /// that blanks the row and sorts the room to the bottom despite it having
    /// real messages. The resident-window LRU makes this routine — it evicts the
    /// in-memory timeline the blank-preview fallback relies on. Preserving the
    /// previously computed preview keeps the last real message and sort position
    /// until a newer previewable message (or a reopen) supplies fresh data. A
    /// non-blank `fresh` always wins.
    pub(crate) fn preserve_preview_if_blank(fresh: &mut RoomSummary, prior: &RoomSummary) {
        if fresh.last_event_text.is_empty() && !prior.last_event_text.is_empty() {
            fresh.last_event_text = prior.last_event_text.clone();
            fresh.last_event_sender = prior.last_event_sender.clone();
            fresh.last_event_timestamp = prior.last_event_timestamp;
            fresh.is_last_event_outgoing = prior.is_last_event_outgoing;
            fresh.last_event_send_state = prior.last_event_send_state;
        }
    }

    /// Keep a room's last known join-rule publicness when the freshly built
    /// summary could not answer. `Room::is_public()` reads local state, so before
    /// a room's state has synced it is None and `is_public` flattens to false —
    /// on a cold start that overwrites a cached `true` and silently disables
    /// "hide system messages in public rooms" (it also persists, outliving the
    /// sync that would have fixed it). A room that genuinely answered private
    /// still wins: only an unknown fresh value is filled in.
    pub(crate) fn preserve_publicness_if_unknown(fresh: &mut RoomSummary, prior: &RoomSummary) {
        if !fresh.is_public_known && prior.is_public_known {
            fresh.is_public = prior.is_public;
            fresh.is_public_known = true;
        }
    }

    /// Apply [`preserve_preview_if_blank`] and [`preserve_publicness_if_unknown`]
    /// across a wholesale-rebuilt room list using the prior list as the source,
    /// then re-sort (restored timestamps change ordering). No-op when there is no
    /// prior list.
    pub(crate) fn merge_sticky_previews(fresh: &mut [RoomSummary], prior: &[RoomSummary]) {
        if prior.is_empty() {
            return;
        }
        let prior_by_id: HashMap<&str, &RoomSummary> =
            prior.iter().map(|r| (r.room_id.as_str(), r)).collect();
        for summary in fresh.iter_mut() {
            if let Some(p) = prior_by_id.get(summary.room_id.as_str()) {
                Self::preserve_preview_if_blank(summary, p);
                Self::preserve_publicness_if_unknown(summary, p);
            }
        }
        fresh.sort_by_key(|b| std::cmp::Reverse(b.last_event_timestamp));
    }

    /// Pick the rooms-list row from a room's cached timeline (oldest→newest).
    ///
    /// Pins two past regressions:
    /// - **Skip deleted/redacted** items: the preview shows the latest *real*
    ///   message (Telegram-style), never "[Message deleted]". Also covers the
    ///   transient decryption case where newer messages are still UTD (no preview)
    ///   and an older redacted message would otherwise leak through.
    /// - **System-only → epoch**: when no previewable message exists, the row
    ///   reflects the newest item's send/outgoing/service state but leaves the
    ///   timestamp at `UNIX_EPOCH`, so a profile/avatar change landing in many
    ///   rooms at once does not stamp them all with one identical, misleading time.
    pub(crate) fn select_cached_preview(items: &[TimelineItem]) -> CachePreviewOutcome {
        for item in items.iter().rev() {
            if item.is_deleted {
                continue;
            }
            if let Some(preview) = Self::content_to_preview(&item.content) {
                return CachePreviewOutcome::Previewable {
                    text: preview,
                    sender: item.sender.display_name.clone(),
                    timestamp: item.timestamp,
                    is_outgoing: item.is_outgoing,
                    send_state: item.send_state,
                };
            }
        }
        match items.last() {
            Some(latest) => CachePreviewOutcome::SystemOnly {
                is_outgoing: latest.is_outgoing,
                send_state: latest.send_state,
                is_last_service: matches!(latest.content, MessageContent::Service { .. }),
            },
            None => CachePreviewOutcome::Empty,
        }
    }

    /// Convert a MessageContent to a short preview string for the chat list.
    /// Returns None for service/system messages that should not appear in preview.
    pub(crate) fn content_to_preview(content: &MessageContent) -> Option<String> {
        match content {
            MessageContent::Text { body, .. } => Some(body.clone()),
            MessageContent::Image { caption, .. } => {
                Some(caption.clone().unwrap_or_else(|| "Photo".to_string()))
            }
            MessageContent::Video { caption, .. } => {
                Some(caption.clone().unwrap_or_else(|| "Video".to_string()))
            }
            MessageContent::File { filename, .. } => Some(filename.clone()),
            MessageContent::Audio { info } => {
                Some(Self::audio_preview_text(info.is_voice, &info.filename))
            }
            MessageContent::Poll { info } => Some(format!("Poll: {}", info.question)),
            MessageContent::Service { .. } => None,
            MessageContent::UnableToDecrypt { .. } => None,
        }
    }

    fn audio_preview_text(is_voice: bool, filename: &str) -> String {
        if is_voice {
            String::from("Voice message")
        } else if filename.trim().is_empty() {
            String::from("Audio")
        } else {
            filename.to_string()
        }
    }

    pub(crate) fn detect_voice_message(audio: &AudioMessageEventContent) -> bool {
        audio.voice.is_some()
    }

    /// Extract a display-friendly body string from a deserialized sync event.
    /// Returns None for system/state/call/encryption events that should not
    /// appear in the chat list preview.
    pub(crate) fn extract_event_body(
        event: &matrix_sdk::ruma::events::AnySyncTimelineEvent,
    ) -> Option<String> {
        use matrix_sdk::ruma::events::{AnySyncMessageLikeEvent, AnySyncTimelineEvent};
        match event {
            AnySyncTimelineEvent::MessageLike(msg) => match msg {
                AnySyncMessageLikeEvent::RoomMessage(m) => {
                    if let Some(original) = m.as_original() {
                        match &original.content.msgtype {
                            MessageType::Text(t) => Some(t.body.clone()),
                            MessageType::Image(img) => {
                                let mime = img.info.as_ref().and_then(|i| i.mimetype.as_deref());
                                let fname = img.filename();
                                if mime == Some("application/pdf")
                                    || fname.to_lowercase().ends_with(".pdf")
                                {
                                    Some(fname.to_string())
                                } else {
                                    Some(String::from("Photo"))
                                }
                            }
                            MessageType::Video(_) => Some(String::from("Video")),
                            MessageType::File(_) => Some(String::from("File")),
                            MessageType::Audio(audio) => Some(Self::audio_preview_text(
                                Self::detect_voice_message(audio),
                                audio.filename(),
                            )),
                            MessageType::Emote(e) => Some(format!("* {}", e.body)),
                            MessageType::Notice(n) => Some(n.body.clone()),
                            _ => None,
                        }
                    } else {
                        None
                    }
                }
                // System-like events: no preview
                AnySyncMessageLikeEvent::RoomEncrypted(_)
                | AnySyncMessageLikeEvent::CallInvite(_)
                | AnySyncMessageLikeEvent::RtcNotification(_) => None,
                _ => None,
            },
            AnySyncTimelineEvent::State(_) => None,
        }
    }
}

pub(crate) fn room_unread_count(room: &Room) -> u32 {
    let server_unread = room.unread_notification_counts().notification_count;
    let client_unread = room.num_unread_messages();
    client_unread
        .max(server_unread)
        .try_into()
        .unwrap_or(u32::MAX)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::types::{AudioInfo, PollInfo, PollKind, UserProfile};
    use std::time::Duration;

    fn audio(is_voice: bool, filename: &str) -> AudioInfo {
        AudioInfo {
            url: "mxc://x/y".into(),
            mime_type: "audio/ogg".into(),
            filename: filename.into(),
            size: 0,
            duration_ms: 0,
            is_voice,
            waveform: Vec::new(),
        }
    }

    fn poll(question: &str) -> PollInfo {
        PollInfo {
            question: question.into(),
            kind: PollKind::Disclosed,
            max_selections: 1,
            is_closed: false,
            is_quiz: false,
            total_voters: 0,
            options: Vec::new(),
            has_voted: false,
        }
    }

    /// Build a timeline item with the given content; `secs` is the offset from
    /// the epoch so ordering is explicit, `deleted`/`outgoing`/`state` cover the
    /// fields the rooms-list cascade reads.
    fn item(
        content: MessageContent,
        secs: u64,
        deleted: bool,
        outgoing: bool,
        send_state: SendState,
    ) -> TimelineItem {
        TimelineItem {
            event_id: format!("$e{secs}"),
            transaction_id: None,
            sender: UserProfile {
                user_id: "@a:x".into(),
                display_name: "Alice".into(),
                avatar_url: None,
            },
            timestamp: UNIX_EPOCH + Duration::from_secs(secs),
            content,
            reply_to_event_id: None,
            reply_preview: None,
            forwarded_from: None,
            is_edited: false,
            is_pinned: false,
            reactions: Vec::new(),
            send_state,
            upload_progress: 0.0,
            is_outgoing: outgoing,
            is_deleted: deleted,
            url_preview: None,
            is_encrypted: false,
            decryption_error: None,
        }
    }

    fn text(body: &str) -> MessageContent {
        MessageContent::Text {
            body: body.into(),
            formatted_body: None,
        }
    }

    // ---- content_to_preview: all 8 MessageContent variants -------------------

    #[test]
    fn preview_text_uses_body() {
        assert_eq!(
            RoomSummaryService::content_to_preview(&text("hello")),
            Some("hello".to_string())
        );
    }

    #[test]
    fn preview_image_uses_caption_else_photo() {
        let with = MessageContent::Image {
            url: String::new(),
            mime_type: String::new(),
            filename: "p.png".into(),
            caption: Some("beach".into()),
            thumbnail_url: None,
            blurhash: None,
            size: 0,
            width: 0,
            height: 0,
        };
        assert_eq!(
            RoomSummaryService::content_to_preview(&with),
            Some("beach".to_string())
        );
        let no_caption = MessageContent::Image {
            url: String::new(),
            mime_type: String::new(),
            filename: "p.png".into(),
            caption: None,
            thumbnail_url: None,
            blurhash: None,
            size: 0,
            width: 0,
            height: 0,
        };
        assert_eq!(
            RoomSummaryService::content_to_preview(&no_caption),
            Some("Photo".to_string())
        );
    }

    #[test]
    fn preview_video_uses_caption_else_video() {
        let with = MessageContent::Video {
            url: String::new(),
            mime_type: String::new(),
            filename: "v.mp4".into(),
            caption: Some("clip".into()),
            thumbnail_url: None,
            blurhash: None,
            size: 0,
            width: 0,
            height: 0,
            duration_ms: 0,
        };
        let without = MessageContent::Video {
            url: String::new(),
            mime_type: String::new(),
            filename: "v.mp4".into(),
            caption: None,
            thumbnail_url: None,
            blurhash: None,
            size: 0,
            width: 0,
            height: 0,
            duration_ms: 0,
        };
        assert_eq!(
            RoomSummaryService::content_to_preview(&with),
            Some("clip".to_string())
        );
        assert_eq!(
            RoomSummaryService::content_to_preview(&without),
            Some("Video".to_string())
        );
    }

    #[test]
    fn preview_file_uses_filename() {
        let f = MessageContent::File {
            url: String::new(),
            mime_type: String::new(),
            filename: "report.pdf".into(),
            caption: None,
            size: 0,
            duration_ms: 0,
        };
        assert_eq!(
            RoomSummaryService::content_to_preview(&f),
            Some("report.pdf".to_string())
        );
    }

    #[test]
    fn preview_audio_voice_vs_file() {
        let voice = MessageContent::Audio {
            info: audio(true, "ignored.ogg"),
        };
        let named = MessageContent::Audio {
            info: audio(false, "song.mp3"),
        };
        let unnamed = MessageContent::Audio {
            info: audio(false, "   "),
        };
        assert_eq!(
            RoomSummaryService::content_to_preview(&voice),
            Some("Voice message".to_string())
        );
        assert_eq!(
            RoomSummaryService::content_to_preview(&named),
            Some("song.mp3".to_string())
        );
        assert_eq!(
            RoomSummaryService::content_to_preview(&unnamed),
            Some("Audio".to_string())
        );
    }

    #[test]
    fn preview_poll_prefixes_question() {
        let p = MessageContent::Poll {
            info: poll("Lunch?"),
        };
        assert_eq!(
            RoomSummaryService::content_to_preview(&p),
            Some("Poll: Lunch?".to_string())
        );
    }

    #[test]
    fn preview_service_and_utd_have_no_preview() {
        assert_eq!(
            RoomSummaryService::content_to_preview(&MessageContent::Service {
                body: "Alice joined".into()
            }),
            None
        );
        assert_eq!(
            RoomSummaryService::content_to_preview(&MessageContent::UnableToDecrypt {
                body: String::new(),
                cause: 0,
                utd_state: 0,
                session_id: None
            }),
            None
        );
    }

    // ---- audio_preview_text --------------------------------------------------

    #[test]
    fn audio_preview_text_branches() {
        assert_eq!(
            RoomSummaryService::audio_preview_text(true, "x.ogg"),
            "Voice message"
        );
        assert_eq!(RoomSummaryService::audio_preview_text(false, ""), "Audio");
        assert_eq!(
            RoomSummaryService::audio_preview_text(false, "   "),
            "Audio"
        );
        assert_eq!(
            RoomSummaryService::audio_preview_text(false, "track.flac"),
            "track.flac"
        );
    }

    // ---- select_cached_preview: the rooms-list cascade -----------------------

    #[test]
    fn cascade_empty_cache() {
        assert_eq!(
            RoomSummaryService::select_cached_preview(&[]),
            CachePreviewOutcome::Empty
        );
    }

    #[test]
    fn cascade_single_message() {
        let items = [item(text("hi"), 10, false, true, SendState::Read)];
        assert_eq!(
            RoomSummaryService::select_cached_preview(&items),
            CachePreviewOutcome::Previewable {
                text: "hi".into(),
                sender: "Alice".into(),
                timestamp: UNIX_EPOCH + Duration::from_secs(10),
                is_outgoing: true,
                send_state: SendState::Read,
            }
        );
    }

    #[test]
    fn cascade_skips_deleted_latest_lands_on_real() {
        // Regression: a redacted latest message must NOT show "[Message deleted]";
        // the scan lands on the previous real message instead.
        let items = [
            item(text("real"), 10, false, false, SendState::Sent),
            item(text("gone"), 20, true, false, SendState::Sent),
        ];
        let out = RoomSummaryService::select_cached_preview(&items);
        match out {
            CachePreviewOutcome::Previewable {
                text, timestamp, ..
            } => {
                assert_eq!(text, "real");
                assert_eq!(timestamp, UNIX_EPOCH + Duration::from_secs(10));
            }
            other => panic!("expected Previewable, got {other:?}"),
        }
    }

    #[test]
    fn cascade_skips_utd_latest_lands_on_real() {
        // During decryption the newest items are still UTD (no preview); the scan
        // must fall through to the latest real message, not stall on nothing.
        let items = [
            item(text("decrypted"), 10, false, false, SendState::Sent),
            item(
                MessageContent::UnableToDecrypt {
                    body: String::new(),
                    cause: 0,
                    utd_state: 0,
                    session_id: None,
                },
                20,
                false,
                false,
                SendState::Sent,
            ),
        ];
        match RoomSummaryService::select_cached_preview(&items) {
            CachePreviewOutcome::Previewable { text, .. } => assert_eq!(text, "decrypted"),
            other => panic!("expected Previewable, got {other:?}"),
        }
    }

    #[test]
    fn cascade_picks_newest_previewable() {
        let items = [
            item(text("older"), 10, false, false, SendState::Sent),
            item(text("newer"), 20, false, true, SendState::Sending),
        ];
        match RoomSummaryService::select_cached_preview(&items) {
            CachePreviewOutcome::Previewable {
                text,
                is_outgoing,
                send_state,
                timestamp,
                ..
            } => {
                assert_eq!(text, "newer");
                assert!(is_outgoing);
                assert_eq!(send_state, SendState::Sending);
                assert_eq!(timestamp, UNIX_EPOCH + Duration::from_secs(20));
            }
            other => panic!("expected Previewable, got {other:?}"),
        }
    }

    #[test]
    fn cascade_service_only_is_system_with_service_flag() {
        // Regression: a system/service-only room must use epoch time (SystemOnly),
        // not stamp the row with the service event's timestamp.
        let items = [item(
            MessageContent::Service {
                body: "Alice changed avatar".into(),
            },
            99,
            false,
            true,
            SendState::Sent,
        )];
        assert_eq!(
            RoomSummaryService::select_cached_preview(&items),
            CachePreviewOutcome::SystemOnly {
                is_outgoing: true,
                send_state: SendState::Sent,
                is_last_service: true,
            }
        );
    }

    #[test]
    fn cascade_all_deleted_is_system_without_service_flag() {
        // All items redacted: no previewable text; the newest item is a (deleted)
        // text message, so the service flag is false.
        let items = [
            item(text("gone1"), 10, true, false, SendState::Sent),
            item(text("gone2"), 20, true, true, SendState::Failed),
        ];
        assert_eq!(
            RoomSummaryService::select_cached_preview(&items),
            CachePreviewOutcome::SystemOnly {
                is_outgoing: true,
                send_state: SendState::Failed,
                is_last_service: false,
            }
        );
    }

    /// Minimal room summary for the sticky-preview tests; an empty `text` models
    /// a blank (system-event) summary, `secs` its sort timestamp.
    fn summary(room_id: &str, text: &str, secs: u64) -> RoomSummary {
        RoomSummary {
            room_id: room_id.into(),
            display_name: room_id.into(),
            canonical_alias: None,
            avatar_url: None,
            avatar_entity_id: String::new(),
            last_event_text: text.into(),
            last_event_sender: String::new(),
            last_event_timestamp: UNIX_EPOCH + Duration::from_secs(secs),
            unread_count: 0,
            highlight_count: 0,
            notification_mode: RoomNotificationMode::AllMessages,
            is_muted: false,
            is_pinned: false,
            pinned_order: None,
            is_marked_unread: false,
            is_direct: false,
            is_public: false,
            is_public_known: false,
            filter_ids: Vec::new(),
            space_ids: Vec::new(),
            is_last_event_outgoing: false,
            is_last_event_service: false,
            last_event_send_state: SendState::Sent,
            member_count: 0,
            can_pin_messages: false,
            peer_presence: 0,
            membership: MembershipState::Join,
            inviter_user_id: String::new(),
            inviter_display_name: String::new(),
            inviter_avatar_url: String::new(),
            room_topic: String::new(),
        }
    }

    #[test]
    fn call_label_is_directional() {
        assert_eq!(RoomSummaryService::call_label(true), "Outgoing call");
        assert_eq!(RoomSummaryService::call_label(false), "Incoming call");
    }

    #[test]
    fn blank_summary_keeps_prior_preview_and_time() {
        // A non-call system event blanks the fresh summary (epoch time); the last
        // real message must survive for both preview text and sorting.
        let prior = summary("!r:x", "hi there", 100);
        let mut fresh = summary("!r:x", "", 0);
        RoomSummaryService::preserve_preview_if_blank(&mut fresh, &prior);
        assert_eq!(fresh.last_event_text, "hi there");
        assert_eq!(
            fresh.last_event_timestamp,
            UNIX_EPOCH + Duration::from_secs(100)
        );
    }

    #[test]
    fn nonblank_summary_overrides_prior() {
        // A newer real message always wins over the stale preview.
        let prior = summary("!r:x", "old", 100);
        let mut fresh = summary("!r:x", "new", 200);
        RoomSummaryService::preserve_preview_if_blank(&mut fresh, &prior);
        assert_eq!(fresh.last_event_text, "new");
        assert_eq!(
            fresh.last_event_timestamp,
            UNIX_EPOCH + Duration::from_secs(200)
        );
    }

    #[test]
    fn merge_sticky_previews_restores_and_resorts() {
        let prior = [summary("!a:x", "alpha", 100), summary("!b:x", "beta", 50)];
        // Wholesale rebuild: !a came back blank (system event on top); !b has a
        // newer message. !a must be restored to alpha@100 and the list re-sorted.
        let mut fresh = [summary("!b:x", "beta2", 200), summary("!a:x", "", 0)];
        RoomSummaryService::merge_sticky_previews(&mut fresh, &prior);
        assert_eq!(fresh[0].room_id, "!b:x");
        assert_eq!(fresh[1].room_id, "!a:x");
        assert_eq!(fresh[1].last_event_text, "alpha");
        assert_eq!(
            fresh[1].last_event_timestamp,
            UNIX_EPOCH + Duration::from_secs(100)
        );
    }

    #[test]
    fn unknown_publicness_keeps_prior_answer() {
        // The cold-start shape: the cache knows the room is public, but the rebuilt
        // summary was built before the join rule synced (unknown → flattened false).
        // Clobbering here disables "hide system messages in public rooms" for the
        // session AND persists, outliving the sync that would have fixed it.
        let mut prior = summary("!r:x", "hi", 100);
        prior.is_public = true;
        prior.is_public_known = true;
        let mut fresh = summary("!r:x", "hi", 100); // is_public_known: false
        RoomSummaryService::preserve_publicness_if_unknown(&mut fresh, &prior);
        assert!(fresh.is_public);
        assert!(fresh.is_public_known);
    }

    #[test]
    fn known_publicness_overrides_prior() {
        // A room that actually answered wins in both directions — no sticky true.
        let mut prior = summary("!r:x", "hi", 100);
        prior.is_public = true;
        prior.is_public_known = true;
        let mut fresh = summary("!r:x", "hi", 100);
        fresh.is_public_known = true; // answered: private
        RoomSummaryService::preserve_publicness_if_unknown(&mut fresh, &prior);
        assert!(
            !fresh.is_public,
            "a known-private answer must not be undone"
        );

        let mut prior_private = summary("!r:x", "hi", 100);
        prior_private.is_public_known = true;
        let mut fresh_public = summary("!r:x", "hi", 100);
        fresh_public.is_public = true;
        fresh_public.is_public_known = true;
        RoomSummaryService::preserve_publicness_if_unknown(&mut fresh_public, &prior_private);
        assert!(fresh_public.is_public, "a newly public room must go public");
    }

    #[test]
    fn unknown_publicness_stays_unknown_without_prior_answer() {
        // Neither side knows: nothing to restore, and the room must stay flagged
        // unknown so a later rebuild can still fill it in.
        let prior = summary("!r:x", "hi", 100);
        let mut fresh = summary("!r:x", "hi", 100);
        RoomSummaryService::preserve_publicness_if_unknown(&mut fresh, &prior);
        assert!(!fresh.is_public);
        assert!(!fresh.is_public_known);
    }

    #[test]
    fn merge_sticky_previews_carries_publicness() {
        // The merge is where the cold-start rebuild meets the cached list, so the
        // preservation must be wired into it, not just available as a helper.
        let mut prior_public = summary("!a:x", "alpha", 100);
        prior_public.is_public = true;
        prior_public.is_public_known = true;
        let prior = [prior_public];
        let mut fresh = [summary("!a:x", "", 0)];
        RoomSummaryService::merge_sticky_previews(&mut fresh, &prior);
        assert!(
            fresh[0].is_public,
            "cold rebuild must not un-public the room"
        );
        assert!(fresh[0].is_public_known);
    }
}

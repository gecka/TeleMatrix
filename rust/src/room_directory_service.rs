// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Public-room discovery: directory search, space children, room preview, and joining.
//!
//! Two asymmetries drive the shape of this module:
//!   * The **directory** searches server-side — `filter.generic_search_term` is matched against a
//!     room's name, topic and canonical alias by the homeserver.
//!   * A **space** cannot be searched at all. `/hierarchy` takes no search term (its whole parameter
//!     set is `from`/`limit`/`max_depth`/`suggested_only`), so callers page in the children and
//!     filter them locally. There is no server-side alternative.

use anyhow::{anyhow, Result};
use matrix_sdk::config::RequestConfig;
use matrix_sdk::ruma::api::client::directory::get_public_rooms_filtered;
use matrix_sdk::ruma::api::client::filter::LazyLoadOptions;
use matrix_sdk::ruma::api::client::message::get_message_events;
use matrix_sdk::ruma::api::client::peeking::get_current_state;
use matrix_sdk::ruma::api::client::space::{get_hierarchy, SpaceHierarchyRoomsChunk};
use matrix_sdk::ruma::directory::{Filter, PublicRoomsChunk, RoomTypeFilter};
use matrix_sdk::ruma::events::room::MediaSource;
use matrix_sdk::ruma::events::{
    AnyMessageLikeEvent, AnyStateEvent, AnyTimelineEvent, MessageLikeEvent,
};
use matrix_sdk::ruma::room::{JoinRuleKind, JoinRuleSummary, RoomType};
use matrix_sdk::ruma::serde::Raw;
use matrix_sdk::ruma::{OwnedRoomId, OwnedServerName, RoomId, RoomOrAliasId, UInt};
use matrix_sdk::{Client, RoomState};
use std::collections::HashMap;
use std::sync::{Arc, RwLock as StdRwLock};
use std::time::{Duration, UNIX_EPOCH};

use crate::timeline_conversion_service::TimelineConversionService;
use crate::timeline_window_service::{
    TimelineChangedFactory, TimelineRuntime, TimelineWindowService,
};
use crate::types::{
    MessageContent, RoomDirectoryEntry, RoomDirectoryJoinRule, RoomDirectoryPage,
    RoomDirectoryRequest, RoomMembershipState, RoomPreviewInfo, SendState, SpaceHierarchyRequest,
    TimelineItem, UserProfile,
};

pub(crate) struct RoomDirectoryService;

impl RoomDirectoryService {
    pub(crate) async fn search_public_rooms(
        client: Client,
        request: RoomDirectoryRequest,
    ) -> Result<RoomDirectoryPage> {
        let mut filter = Filter::new();
        let term = request.query.trim();
        if !term.is_empty() {
            filter.generic_search_term = Some(term.to_owned());
        }
        // Empty `room_types` means "no filter", so spaces come back alongside rooms and the UI can
        // offer to drill into them.
        filter.room_types = Vec::<RoomTypeFilter>::new();

        let mut req = get_public_rooms_filtered::v3::Request::new();
        req.filter = filter;
        req.limit = Some(UInt::from(request.limit));
        req.since = request.next_token.clone();
        // `server` stays unset: we only search our own homeserver's directory.

        let response = client
            .public_rooms_filtered(req)
            .await
            .map_err(|e| anyhow!("Failed to search the room directory: {e}"))?;

        let entries = response
            .chunk
            .iter()
            .map(|chunk| {
                let membership = membership_of(&client, &chunk.room_id);
                entry_from_public_chunk(chunk, membership)
            })
            .collect();

        Ok(RoomDirectoryPage {
            request_id: request.request_id,
            entries,
            total_approx: response
                .total_room_count_estimate
                .map(|count| i64::from(count).try_into().unwrap_or(i32::MAX))
                .unwrap_or(-1),
            done: response.next_batch.is_none(),
            next_token: response.next_batch,
        })
    }

    pub(crate) async fn space_children(
        client: Client,
        request: SpaceHierarchyRequest,
    ) -> Result<RoomDirectoryPage> {
        let space_id: OwnedRoomId = request
            .space_id
            .as_str()
            .try_into()
            .map_err(|_| anyhow!("Invalid space ID"))?;

        let mut req = get_hierarchy::v1::Request::new(space_id.clone());
        // Immediate children only; sub-spaces are drilled into one level at a time.
        req.max_depth = Some(UInt::from(1u32));
        req.limit = Some(UInt::from(request.limit));
        req.from = request.next_token.clone();

        let response = client
            .send(req)
            .await
            .map_err(|e| anyhow!("Failed to load the space: {e}"))?;

        // The server returns the queried space as a member of its own `rooms` array. Left in, every
        // space would list itself as its own first child.
        let (parents, children): (Vec<_>, Vec<_>) = response
            .rooms
            .into_iter()
            .partition(|room| room.summary.room_id == space_id);

        let via_hints = parents
            .first()
            .map(via_hints_from_children_state)
            .unwrap_or_default();

        let entries = children
            .iter()
            .map(|chunk| {
                let room_id = &chunk.summary.room_id;
                let via = via_hints
                    .get(room_id.as_str())
                    .cloned()
                    .unwrap_or_else(|| via_fallback(room_id));
                let membership = membership_of(&client, room_id);
                entry_from_hierarchy_chunk(chunk, via, membership)
            })
            .collect();

        Ok(RoomDirectoryPage {
            request_id: request.request_id,
            entries,
            total_approx: -1,
            done: response.next_batch.is_none(),
            next_token: response.next_batch,
        })
    }

    pub(crate) async fn room_preview(
        client: Client,
        room_id_or_alias: &str,
        via: &[String],
    ) -> Result<RoomPreviewInfo> {
        let target = <&RoomOrAliasId>::try_from(room_id_or_alias)
            .map_err(|_| anyhow!("Invalid room ID or alias"))?;

        // `get_room_preview` walks three fallbacks (summary endpoint → directory search → full
        // `/state` download), each on the client's default config = unlimited retries. On a flaky
        // remote server that can hang forever, leaving the top bar stuck on "Loading…". Bound it;
        // the timeline itself loads on the independent peek path regardless of this result.
        let preview = tokio::time::timeout(
            Duration::from_secs(30),
            client.get_room_preview(target, parse_via(via)),
        )
        .await
        .map_err(|_| anyhow!("Timed out loading the room"))?
        .map_err(|e| anyhow!("Failed to load the room: {e}"))?;

        let name = display_name_for(
            preview.name.as_deref(),
            preview.canonical_alias.as_ref().map(|a| a.as_str()),
            preview.room_id.as_str(),
        );

        Ok(RoomPreviewInfo {
            room_id: preview.room_id.to_string(),
            name,
            topic: preview.topic.unwrap_or_default(),
            canonical_alias: preview
                .canonical_alias
                .map(|a| a.to_string())
                .unwrap_or_default(),
            avatar_url: preview
                .avatar_url
                .map(|url| url.to_string())
                .unwrap_or_default(),
            member_count: preview.num_joined_members.try_into().unwrap_or(u32::MAX),
            is_space: matches!(preview.room_type, Some(RoomType::Space)),
            join_rule: preview
                .join_rule
                .as_ref()
                .map(map_join_rule_summary)
                .unwrap_or(RoomDirectoryJoinRule::Unknown),
            membership: map_membership(preview.state),
            world_readable: preview.is_world_readable.unwrap_or(false),
        })
    }

    /// Read recent history for a room the user has **not** joined. Only works for `world_readable`
    /// rooms — the homeserver authorizes `/messages` for a non-member solely when the room's history
    /// visibility is world-readable, and returns 403 otherwise (surfaced here as an error, so the
    /// caller falls back to the name+topic placeholder).
    ///
    /// This is deliberately low-level: there is no joined `Room`, so no rich `Timeline`, no
    /// decryption, no live updates, no reactions/receipts. It maps the raw event chunk into
    /// read-only `TimelineItem`s using the same content converter the live timeline uses, so media
    /// (via the shared `media_sources` map) and text render identically.
    ///
    /// Paginates backward like a joined timeline's scrollback: `from` is `None` for the newest page,
    /// then the returned `next_token` for each older page. Returns the mapped items (oldest→newest)
    /// plus the token for the next-older page, or `None` once the start of history is reached.
    ///
    /// The FIRST page goes through `/rooms/{id}/initialSync`, NOT `/messages`. This matters for
    /// **remote** world-readable rooms (e.g. `#kde:kde.org`): our homeserver holds none of their
    /// events, so a bare `/messages` returns nothing. `initialSync` makes the server set up a peek
    /// (fetch the room's state + recent messages over federation) and hands them back in one shot —
    /// exactly what Element's `peekInRoom` does. Once the peek is live, `/messages` back-pagination
    /// works for the older pages.
    pub(crate) async fn preview_messages(
        client: Client,
        room_id: &str,
        from: Option<String>,
        limit: u32,
        media_sources: &Arc<StdRwLock<HashMap<String, MediaSource>>>,
    ) -> Result<(Vec<TimelineItem>, Option<String>)> {
        let parsed: OwnedRoomId = room_id.try_into().map_err(|_| anyhow!("Invalid room ID"))?;

        // Setting up a peek makes the server fetch a remote room's state + messages over federation,
        // which is far slower than a local read — the SDK's 30s default times out on big rooms like
        // KDE via matrix.org. Give it room, but a single attempt: on a persistent timeout we fall
        // back to the placeholder rather than retrying (the default) for minutes.
        let peek_config = RequestConfig::new()
            .timeout(Duration::from_secs(60))
            .disable_retry();

        let Some(from) = from else {
            // First page: establish the peek and read the recent messages.
            let req = get_current_state::v3::Request::new(parsed);
            let response = client
                .send(req)
                .with_request_config(peek_config)
                .await
                .map_err(|e| anyhow!("Failed to load room history: {e}"))?;

            let profiles = member_profiles_from_state(&response.state);
            let (items, next_token) = match response.messages {
                // `initialSync` returns the chunk oldest→newest (chronological), and `start` is the
                // token for paginating further back — feed it to `/messages` for the next page.
                Some(chunk) => {
                    let items =
                        aggregate_preview_items(chunk.chunk.iter(), &profiles, media_sources);
                    (items, chunk.start)
                }
                None => (Vec::new(), None),
            };
            return Ok((items, next_token));
        };

        // Older pages: the peek is active, so `/messages` back-pagination works — but federated
        // backfill can still be slow, so keep the generous timeout.
        let mut req = get_message_events::v3::Request::backward(parsed);
        req.limit = UInt::from(limit);
        req.from = Some(from);
        // Without lazy-load the `state` block comes back empty, so senders would have no display
        // name or avatar (only page 1 via initialSync gets full room state). This backfills the
        // membership events for the page's senders.
        req.filter.lazy_load_options = LazyLoadOptions::Enabled {
            include_redundant_members: true,
        };

        let response = client
            .send(req)
            .with_request_config(peek_config)
            .await
            .map_err(|e| anyhow!("Failed to load room history: {e}"))?;

        // An empty chunk means we've paginated past the start of history; stop so the caller
        // doesn't loop forever. Otherwise `end` is the token for the next-older page.
        let next_token = if response.chunk.is_empty() {
            None
        } else {
            response.end
        };

        let profiles = member_profiles_from_state(&response.state);

        // `backward` yields newest→oldest; the UI (and edit aggregation) wants oldest→newest.
        let items = aggregate_preview_items(response.chunk.iter().rev(), &profiles, media_sources);

        Ok((items, next_token))
    }

    /// Join, then bootstrap the timeline window exactly as invite-acceptance does — without it the
    /// freshly joined room opens to an empty timeline.
    pub(crate) async fn join_room(
        client: Client,
        room_id_or_alias: &str,
        via: &[String],
        runtime: TimelineRuntime,
        make_on_changed: TimelineChangedFactory,
    ) -> Result<String> {
        let target = <&RoomOrAliasId>::try_from(room_id_or_alias)
            .map_err(|_| anyhow!("Invalid room ID or alias"))?;

        let room = client
            .join_room_by_id_or_alias(target, &parse_via(via))
            .await
            .map_err(|e| anyhow!("Failed to join: {e}"))?;

        let room_id = room.room_id().to_string();

        // A space has no timeline to open.
        if room.is_space() {
            return Ok(room_id);
        }

        {
            let wins = runtime.windows.read().await;
            if wins.contains_key(&room_id) {
                return Ok(room_id);
            }
        }

        TimelineWindowService::create_window_after_join(
            room,
            room_id.clone(),
            runtime,
            make_on_changed,
        )
        .await;

        Ok(room_id)
    }

    /// Knock on a room (request to join). Knock-only rooms reject a plain join, so the "Ask to join"
    /// button routes here. No timeline is opened — membership stays `Knocked` until accepted.
    pub(crate) async fn knock_room(
        client: Client,
        room_id_or_alias: &str,
        via: &[String],
    ) -> Result<String> {
        let target = <&RoomOrAliasId>::try_from(room_id_or_alias)
            .map_err(|_| anyhow!("Invalid room ID or alias"))?
            .to_owned();

        let room = client
            .knock(target, None, parse_via(via))
            .await
            .map_err(|e| anyhow!("Failed to knock: {e}"))?;

        Ok(room.room_id().to_string())
    }
}

fn parse_via(via: &[String]) -> Vec<OwnedServerName> {
    via.iter()
        .filter_map(|server| server.as_str().try_into().ok())
        .collect()
}

/// Build a `user_id → profile` map from the `m.room.member` events the `/messages` endpoint returns
/// alongside the chunk. A sender missing here falls back to their bare MXID.
fn member_profiles_from_state(state: &[Raw<AnyStateEvent>]) -> HashMap<String, UserProfile> {
    use matrix_sdk::ruma::events::StateEvent;

    let mut profiles = HashMap::new();
    for raw in state {
        // Only original (non-redacted) membership events carry a profile.
        let Ok(AnyStateEvent::RoomMember(StateEvent::Original(ev))) = raw.deserialize() else {
            continue;
        };
        let user_id = ev.state_key.to_string();
        let display_name = ev
            .content
            .displayname
            .clone()
            .unwrap_or_else(|| user_id.clone());
        let avatar_url = ev.content.avatar_url.as_ref().map(|u| u.to_string());
        profiles.insert(
            user_id.clone(),
            UserProfile {
                user_id,
                display_name,
                avatar_url,
            },
        );
    }
    profiles
}

/// Aggregate a chronological run of raw events into read-only preview `TimelineItem`s, the way a
/// joined timeline would: `m.room.message` and `m.sticker` become bubbles, and `m.replace` edits are
/// folded into the message they replace (rather than showing a duplicate `* new text` bubble).
///
/// Deliberately NOT handled (a preview is a read-only glimpse, and there is no crypto for an unjoined
/// room): replies keep their quoted fallback body, reactions, threads, polls, and state/service
/// events are dropped, and encrypted events can't be decrypted.
fn aggregate_preview_items<'a>(
    raws: impl Iterator<Item = &'a Raw<AnyTimelineEvent>>,
    profiles: &HashMap<String, UserProfile>,
    media_sources: &Arc<StdRwLock<HashMap<String, MediaSource>>>,
) -> Vec<TimelineItem> {
    use matrix_sdk::ruma::events::room::message::Relation;

    let mut items: Vec<TimelineItem> = Vec::new();
    let mut index_by_id: HashMap<String, usize> = HashMap::new();
    // (target_event_id, replacement_content). Applied after the pass so an edit that arrives before
    // its target in the chunk still lands.
    let mut pending_edits: Vec<(String, MessageContent)> = Vec::new();

    for raw in raws {
        let Ok(AnyTimelineEvent::MessageLike(msg_like)) = raw.deserialize() else {
            continue;
        };
        match msg_like {
            AnyMessageLikeEvent::RoomMessage(MessageLikeEvent::Original(msg)) => {
                // An edit replaces an earlier message; render the new content on the original, not a
                // second bubble. The top-level msgtype is only the `* fallback`, so use new_content.
                if let Some(Relation::Replacement(repl)) = &msg.content.relates_to {
                    let content = TimelineConversionService::convert_message_type(
                        &repl.new_content.msgtype,
                        media_sources,
                    );
                    pending_edits.push((repl.event_id.to_string(), content));
                    continue;
                }
                let content = TimelineConversionService::convert_message_type(
                    &msg.content.msgtype,
                    media_sources,
                );
                let item = build_preview_item(
                    msg.sender.as_str(),
                    msg.event_id.as_str(),
                    msg.origin_server_ts.get().into(),
                    content,
                    profiles,
                );
                index_by_id.insert(item.event_id.clone(), items.len());
                items.push(item);
            }
            AnyMessageLikeEvent::Sticker(MessageLikeEvent::Original(sticker)) => {
                let content = sticker_content(&sticker.content, media_sources);
                let item = build_preview_item(
                    sticker.sender.as_str(),
                    sticker.event_id.as_str(),
                    sticker.origin_server_ts.get().into(),
                    content,
                    profiles,
                );
                index_by_id.insert(item.event_id.clone(), items.len());
                items.push(item);
            }
            _ => {}
        }
    }

    for (target_id, content) in pending_edits {
        if let Some(&idx) = index_by_id.get(&target_id) {
            items[idx].content = content;
            items[idx].is_edited = true;
        }
        // An edit whose target is outside this window is dropped (the original updates whenever it
        // loads), exactly like a joined timeline.
    }

    items
}

fn build_preview_item(
    sender: &str,
    event_id: &str,
    timestamp_ms: u64,
    content: MessageContent,
    profiles: &HashMap<String, UserProfile>,
) -> TimelineItem {
    let profile = profiles
        .get(sender)
        .cloned()
        .unwrap_or_else(|| UserProfile {
            user_id: sender.to_owned(),
            display_name: sender.to_owned(),
            avatar_url: None,
        });

    TimelineItem {
        event_id: event_id.to_owned(),
        transaction_id: None,
        sender: profile,
        timestamp: UNIX_EPOCH + Duration::from_millis(timestamp_ms),
        content,
        reply_to_event_id: None,
        reply_preview: None,
        forwarded_from: None,
        is_edited: false,
        is_pinned: false,
        reactions: Vec::new(),
        send_state: SendState::Sent,
        upload_progress: 0.0,
        is_outgoing: false,
        is_deleted: false,
        url_preview: None,
        is_encrypted: false,
        decryption_error: None,
    }
}

/// A sticker renders as an image (that is how the joined timeline treats it too).
fn sticker_content(
    sticker: &matrix_sdk::ruma::events::sticker::StickerEventContent,
    media_sources: &Arc<StdRwLock<HashMap<String, MediaSource>>>,
) -> MessageContent {
    let source: MediaSource = sticker.source.clone().into();
    let url = TimelineConversionService::remember_media_source(&source, media_sources);
    let info = &sticker.info;
    MessageContent::Image {
        url,
        mime_type: info
            .mimetype
            .clone()
            .unwrap_or_else(|| String::from("image/png")),
        filename: sticker.body.clone(),
        caption: None,
        thumbnail_url: info
            .thumbnail_source
            .as_ref()
            .map(|s| TimelineConversionService::remember_media_source(s, media_sources)),
        blurhash: None,
        size: info.size.map(u64::from).unwrap_or(0),
        width: info.width.map(|v| u64::from(v) as u32).unwrap_or(0),
        height: info.height.map(|v| u64::from(v) as u32).unwrap_or(0),
    }
}

fn membership_of(client: &Client, room_id: &RoomId) -> RoomMembershipState {
    // Under sliding sync this only knows rooms the window has materialised, so a "not joined"
    // answer is best-effort. Re-joining an already-joined room is a server-side no-op.
    client
        .get_room(room_id)
        .map(|room| map_membership(Some(room.state())))
        .unwrap_or(RoomMembershipState::None)
}

/// The `via` server hints live on the space's own `m.space.child` events, which only arrive on the
/// first hierarchy page.
fn via_hints_from_children_state(space: &SpaceHierarchyRoomsChunk) -> HashMap<String, Vec<String>> {
    let mut hints = HashMap::new();
    for raw in &space.children_state {
        let Ok(child) = raw.deserialize() else {
            continue;
        };
        hints.insert(
            child.state_key.to_string(),
            child
                .content
                .via
                .iter()
                .map(|server| server.to_string())
                .collect(),
        );
    }
    hints
}

/// Any server in the room can bootstrap a join; the room ID's own server is the best guess we have
/// when the space gave us no hint.
fn via_fallback(room_id: &RoomId) -> Vec<String> {
    room_id
        .server_name()
        .map(|server| vec![server.to_string()])
        .unwrap_or_default()
}

fn display_name_for(name: Option<&str>, alias: Option<&str>, room_id: &str) -> String {
    name.filter(|n| !n.trim().is_empty())
        .or(alias.filter(|a| !a.trim().is_empty()))
        .unwrap_or(room_id)
        .to_owned()
}

fn map_join_rule(kind: &JoinRuleKind) -> RoomDirectoryJoinRule {
    match kind {
        JoinRuleKind::Public => RoomDirectoryJoinRule::Public,
        JoinRuleKind::Knock => RoomDirectoryJoinRule::Knock,
        JoinRuleKind::Invite => RoomDirectoryJoinRule::Invite,
        JoinRuleKind::Restricted => RoomDirectoryJoinRule::Restricted,
        JoinRuleKind::KnockRestricted => RoomDirectoryJoinRule::KnockRestricted,
        JoinRuleKind::Private => RoomDirectoryJoinRule::Private,
        // `#[non_exhaustive]`: an exhaustive match would compile today and break on a ruma bump.
        _ => RoomDirectoryJoinRule::Unknown,
    }
}

fn map_join_rule_summary(summary: &JoinRuleSummary) -> RoomDirectoryJoinRule {
    map_join_rule(&summary.kind())
}

fn map_membership(state: Option<RoomState>) -> RoomMembershipState {
    match state {
        None => RoomMembershipState::None,
        Some(RoomState::Joined) => RoomMembershipState::Joined,
        Some(RoomState::Invited) => RoomMembershipState::Invited,
        Some(RoomState::Left) => RoomMembershipState::Left,
        Some(RoomState::Knocked) => RoomMembershipState::Knocked,
        Some(RoomState::Banned) => RoomMembershipState::Banned,
    }
}

fn entry_from_public_chunk(
    chunk: &PublicRoomsChunk,
    membership: RoomMembershipState,
) -> RoomDirectoryEntry {
    RoomDirectoryEntry {
        room_id: chunk.room_id.to_string(),
        name: display_name_for(
            chunk.name.as_deref(),
            chunk.canonical_alias.as_ref().map(|a| a.as_str()),
            chunk.room_id.as_str(),
        ),
        topic: chunk.topic.clone().unwrap_or_default(),
        canonical_alias: chunk
            .canonical_alias
            .as_ref()
            .map(|a| a.to_string())
            .unwrap_or_default(),
        avatar_url: chunk
            .avatar_url
            .as_ref()
            .map(|url| url.to_string())
            .unwrap_or_default(),
        member_count: i64::from(chunk.num_joined_members)
            .try_into()
            .unwrap_or(u32::MAX),
        children_count: 0,
        is_space: matches!(chunk.room_type, Some(RoomType::Space)),
        world_readable: chunk.world_readable,
        guest_can_join: chunk.guest_can_join,
        join_rule: map_join_rule(&chunk.join_rule),
        membership,
        via: Vec::new(),
    }
}

fn entry_from_hierarchy_chunk(
    chunk: &SpaceHierarchyRoomsChunk,
    via: Vec<String>,
    membership: RoomMembershipState,
) -> RoomDirectoryEntry {
    let summary = &chunk.summary;
    RoomDirectoryEntry {
        room_id: summary.room_id.to_string(),
        name: display_name_for(
            summary.name.as_deref(),
            summary.canonical_alias.as_ref().map(|a| a.as_str()),
            summary.room_id.as_str(),
        ),
        topic: summary.topic.clone().unwrap_or_default(),
        canonical_alias: summary
            .canonical_alias
            .as_ref()
            .map(|a| a.to_string())
            .unwrap_or_default(),
        avatar_url: summary
            .avatar_url
            .as_ref()
            .map(|url| url.to_string())
            .unwrap_or_default(),
        member_count: i64::from(summary.num_joined_members)
            .try_into()
            .unwrap_or(u32::MAX),
        children_count: chunk.children_state.len().try_into().unwrap_or(u32::MAX),
        is_space: matches!(summary.room_type, Some(RoomType::Space)),
        world_readable: summary.world_readable,
        guest_can_join: summary.guest_can_join,
        join_rule: map_join_rule_summary(&summary.join_rule),
        membership,
        via,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use matrix_sdk::ruma::room_id;

    #[test]
    fn display_name_prefers_name_then_alias_then_id() {
        assert_eq!(
            display_name_for(Some("Matrix HQ"), Some("#matrix:x.org"), "!abc:x.org"),
            "Matrix HQ"
        );
        assert_eq!(
            display_name_for(None, Some("#matrix:x.org"), "!abc:x.org"),
            "#matrix:x.org"
        );
        assert_eq!(display_name_for(None, None, "!abc:x.org"), "!abc:x.org");
        // A blank name must not win over a usable alias.
        assert_eq!(
            display_name_for(Some("   "), Some("#matrix:x.org"), "!abc:x.org"),
            "#matrix:x.org"
        );
    }

    #[test]
    fn via_fallback_uses_the_room_ids_own_server() {
        assert_eq!(
            via_fallback(room_id!("!abc:example.org")),
            vec!["example.org"]
        );
    }

    #[test]
    fn join_rules_map_to_stable_discriminants() {
        // The C++ side mirrors these numbers; they are a contract, not an implementation detail.
        assert_eq!(map_join_rule(&JoinRuleKind::Public) as u32, 0);
        assert_eq!(map_join_rule(&JoinRuleKind::Knock) as u32, 1);
        assert_eq!(map_join_rule(&JoinRuleKind::Invite) as u32, 2);
        assert_eq!(map_join_rule(&JoinRuleKind::Restricted) as u32, 3);
        assert_eq!(map_join_rule(&JoinRuleKind::KnockRestricted) as u32, 4);
        assert_eq!(map_join_rule(&JoinRuleKind::Private) as u32, 5);
    }

    #[test]
    fn memberships_map_to_stable_discriminants() {
        assert_eq!(map_membership(None) as u32, 0);
        assert_eq!(map_membership(Some(RoomState::Invited)) as u32, 1);
        assert_eq!(map_membership(Some(RoomState::Joined)) as u32, 2);
        assert_eq!(map_membership(Some(RoomState::Left)) as u32, 3);
        assert_eq!(map_membership(Some(RoomState::Knocked)) as u32, 4);
        assert_eq!(map_membership(Some(RoomState::Banned)) as u32, 5);
    }
}

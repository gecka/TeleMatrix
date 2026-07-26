// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::collections::{HashMap, HashSet};
use std::hash::{Hash, Hasher};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::UNIX_EPOCH;

use matrix_sdk::ruma::events::room::MediaSource;
use matrix_sdk_ui::timeline::{
    EventTimelineItem, MsgLikeKind, Timeline as SdkTimeline, TimelineItemContent,
};
use tokio::sync::RwLock;
use tracing::warn;

use crate::timeline_conversion_service::TimelineConversionService;
use crate::timeline_service::TimelineRefreshState;
use crate::timeline_window::TimelineDiff;
use crate::types::{
    MessageContent, ReplyPreview, SendState, TimelineItem, TimelineSlice, TimelineUpdateKind,
    UrlPreview, UserProfile,
};

type TimelineCache = Arc<RwLock<HashMap<String, Vec<TimelineItem>>>>;
type TimelineUpdateCache = Arc<RwLock<HashMap<String, TimelineSlice>>>;
type ReplyPreviewCache = Arc<RwLock<HashMap<(String, String), ReplyPreview>>>;
type PendingReactionOverrides = Arc<RwLock<HashMap<(String, String, String), bool>>>;
type MediaSources = Arc<std::sync::RwLock<HashMap<String, MediaSource>>>;
type UrlPreviewCache = Arc<RwLock<HashMap<String, Option<UrlPreview>>>>;
type SenderAvatarCache = Arc<RwLock<HashMap<String, HashMap<String, Option<String>>>>>;
type SearchIndex = Arc<std::sync::Mutex<Option<crate::search_index::SearchIndex>>>;
type SearchIndexFingerprints = Arc<RwLock<HashMap<(String, String), u64>>>;
type TimelineRefreshStateMap = Arc<Mutex<HashMap<String, Arc<TimelineRefreshState>>>>;

/// txid/event-id → forward metadata recorded at send time. In an E2EE room the
/// local echo has no raw JSON and the remote echo of our own send keeps the
/// encrypted envelope, so `extract_forwarded_from` finds nothing for the rest
/// of the session — this side channel restores it. Session-scoped.
pub(crate) type PendingForwardMeta =
    Arc<std::sync::Mutex<HashMap<String, crate::types::ForwardedFrom>>>;

pub(crate) struct TimelineCacheContext {
    pub(crate) cache: TimelineCache,
    pub(crate) update_cache: TimelineUpdateCache,
    pub(crate) reply_preview_cache: ReplyPreviewCache,
    pub(crate) pending_reaction_overrides: PendingReactionOverrides,
    pub(crate) media_sources: MediaSources,
    pub(crate) preview_cache: UrlPreviewCache,
    pub(crate) sender_avatar_cache: SenderAvatarCache,
    pub(crate) search_index: SearchIndex,
    pub(crate) search_index_fingerprints: SearchIndexFingerprints,
    pub(crate) pending_forward_meta: PendingForwardMeta,
}

/// Fill `forwarded_from` for freshly sent forwards whose raw JSON cannot carry
/// the metadata yet (see [`PendingForwardMeta`]). Learns the event id on a
/// txid hit so the remote echo (which has no transaction id) still matches.
fn apply_pending_forward_meta(items: &mut [TimelineItem], pending: &PendingForwardMeta) {
    let mut map = match pending.lock() {
        Ok(guard) => guard,
        Err(poisoned) => poisoned.into_inner(),
    };
    if map.is_empty() {
        return;
    }
    for item in items.iter_mut() {
        if item.forwarded_from.is_some() {
            continue;
        }
        let txn = item.transaction_id.as_deref().unwrap_or_default();
        let key = if !txn.is_empty() && map.contains_key(txn) {
            txn.to_string()
        } else if !item.event_id.is_empty() && map.contains_key(&item.event_id) {
            item.event_id.clone()
        } else {
            continue;
        };
        if let Some(meta) = map.get(&key).cloned() {
            if !item.event_id.is_empty() && item.event_id != key {
                map.insert(item.event_id.clone(), meta.clone());
            }
            item.forwarded_from = Some(meta);
        }
    }
}

fn lock_timeline_mutex<'a, T>(mutex: &'a Mutex<T>, name: &str) -> MutexGuard<'a, T> {
    match mutex.lock() {
        Ok(guard) => guard,
        Err(poisoned) => {
            warn!("Recovering poisoned Matrix mutex: {name}");
            poisoned.into_inner()
        }
    }
}

async fn reconcile_pending_reaction_overrides(
    room_id: &str,
    items: &[TimelineItem],
    pending_reaction_overrides: &PendingReactionOverrides,
) {
    let mut pending = pending_reaction_overrides.write().await;
    pending.retain(
        |(pending_room_id, pending_event_id, pending_key), desired_state| {
            if pending_room_id != room_id {
                return true;
            }
            let Some(item) = items.iter().find(|item| item.event_id == *pending_event_id) else {
                return true;
            };
            let actual_state = item
                .reactions
                .iter()
                .any(|reaction| reaction.key == *pending_key && reaction.is_self);
            actual_state != *desired_state
        },
    );
}

/// Drop one account's memoized profiles. Called on logout / new session: the memo
/// is a process-wide static, so without this the next account on this context would
/// serve display names and avatars fetched by the previous one (and inherit its
/// cached absences). Other accounts' partitions are left alone.
pub(crate) async fn clear_global_profile_cache(owner: &str) {
    crate::profile_cache_store::clear(owner).await;
}

/// True when a `/profile` failure means the user definitively HAS no profile (404 —
/// deleted account, or never set one). Everything else (timeout, 429, 5xx, offline)
/// is transient and must not be mistaken for an absence.
fn profile_error_is_absent(e: &matrix_sdk::Error) -> bool {
    e.as_client_api_error()
        .map(|api| api.status_code.as_u16() == 404)
        .unwrap_or(false)
}

/// Bound concurrent global `/profile` fetches. The prefetch would otherwise fire one
/// request per unresolvable sender at once (dozens in a room full of departed users) —
/// the surest way to earn the 429s that this classification then has to survive.
fn profile_fetch_semaphore() -> &'static tokio::sync::Semaphore {
    static SEM: std::sync::OnceLock<tokio::sync::Semaphore> = std::sync::OnceLock::new();
    SEM.get_or_init(|| tokio::sync::Semaphore::new(4))
}

/// Bound concurrent member-store probes during the prefetch scan: each is a SQLite
/// read through the shared pool, and firing one per sender at once starves that pool
/// for the timeline build running alongside it.
fn member_probe_semaphore() -> &'static tokio::sync::Semaphore {
    static SEM: std::sync::OnceLock<tokio::sync::Semaphore> = std::sync::OnceLock::new();
    SEM.get_or_init(|| tokio::sync::Semaphore::new(6))
}

pub(crate) struct TimelineCacheService;

impl TimelineCacheService {
    pub(crate) fn refresh_state(
        states: &TimelineRefreshStateMap,
        room_id: &str,
    ) -> Arc<TimelineRefreshState> {
        let mut guard = lock_timeline_mutex(states, "timeline_update_states");
        guard
            .entry(room_id.to_string())
            .or_insert_with(|| Arc::new(TimelineRefreshState::default()))
            .clone()
    }

    pub(crate) fn queue_diffs(state: &TimelineRefreshState, diffs: Vec<TimelineDiff>) {
        let mut guard = lock_timeline_mutex(&state.pending_diffs, "timeline_pending_diffs");
        if diffs.is_empty() {
            guard.push(TimelineDiff::Full);
        } else {
            guard.extend(diffs);
        }
    }

    pub(crate) fn take_diffs(state: &TimelineRefreshState) -> Vec<TimelineDiff> {
        let mut guard = lock_timeline_mutex(&state.pending_diffs, "timeline_pending_diffs");
        if guard.is_empty() {
            vec![TimelineDiff::Full]
        } else {
            std::mem::take(&mut *guard)
        }
    }

    fn item_fingerprint(event: &EventTimelineItem) -> Option<(String, String)> {
        let id = event
            .event_id()
            .map(|id| id.to_string())
            .or_else(|| event.transaction_id().map(|t| t.to_string()))?;
        let tag = match event.content() {
            TimelineItemContent::MsgLike(msg) => match &msg.kind {
                MsgLikeKind::UnableToDecrypt(_) => "utd".to_string(),
                MsgLikeKind::Redacted => "redacted".to_string(),
                MsgLikeKind::Message(m) if m.is_edited() => {
                    let mut hasher = std::collections::hash_map::DefaultHasher::new();
                    m.body().hash(&mut hasher);
                    format!("msg:e{}", hasher.finish())
                }
                _ => "msg".to_string(),
            },
            TimelineItemContent::MembershipChange(_) => "membership".to_string(),
            TimelineItemContent::ProfileChange(_) => "profile".to_string(),
            TimelineItemContent::OtherState(_) => "state".to_string(),
            TimelineItemContent::CallInvite => "call".to_string(),
            TimelineItemContent::RtcNotification { .. } => "rtc".to_string(),
            _ => "other".to_string(),
        };
        Some((id, tag))
    }

    fn search_index_fingerprint(item: &TimelineItem, body: &str, timestamp: i64) -> u64 {
        let mut hasher = std::collections::hash_map::DefaultHasher::new();
        item.event_id.hash(&mut hasher);
        item.sender.user_id.hash(&mut hasher);
        item.sender.display_name.hash(&mut hasher);
        body.hash(&mut hasher);
        timestamp.hash(&mut hasher);
        hasher.finish()
    }

    fn timeline_item_key(item: &TimelineItem) -> &str {
        if !item.event_id.is_empty() {
            item.event_id.as_str()
        } else {
            item.transaction_id.as_deref().unwrap_or_default()
        }
    }

    fn timeline_items_render_equal(left: &TimelineItem, right: &TimelineItem) -> bool {
        left == right
    }

    fn timeline_delta_slice(previous: &[TimelineItem], next: &[TimelineItem]) -> TimelineSlice {
        let mut slice = TimelineSlice::empty_live();

        if previous.is_empty() || next.is_empty() {
            slice.items = next.to_vec();
            return slice;
        }

        let first_key = Self::timeline_item_key(&previous[0]);
        let last_key = Self::timeline_item_key(&previous[previous.len() - 1]);
        if first_key.is_empty() || last_key.is_empty() {
            slice.items = next.to_vec();
            return slice;
        }

        let mut first_match = None;
        let mut last_match = None;
        for (index, item) in next.iter().enumerate() {
            let key = Self::timeline_item_key(item);
            if key == first_key {
                first_match = Some(index);
            }
            if key == last_key {
                last_match = Some(index);
            }
            if first_match.is_some() && last_match.is_some() {
                break;
            }
        }

        let Some(first_match) = first_match else {
            slice.items = next.to_vec();
            return slice;
        };
        let Some(last_match) = last_match else {
            slice.items = next.to_vec();
            return slice;
        };

        if last_match < first_match || last_match - first_match + 1 != previous.len() {
            slice.items = next.to_vec();
            return slice;
        }

        for index in 0..previous.len() {
            if Self::timeline_item_key(&previous[index])
                != Self::timeline_item_key(&next[first_match + index])
                || !Self::timeline_items_render_equal(&previous[index], &next[first_match + index])
            {
                slice.items = next.to_vec();
                return slice;
            }
        }

        let prepend_count = first_match;
        let append_start = last_match + 1;
        let append_count = next.len().saturating_sub(append_start);
        match (prepend_count, append_count) {
            (0, 0) => {
                slice.update_kind = TimelineUpdateKind::MetadataOnly;
                slice.update_index = 0;
            }
            (0, _) => {
                slice.update_kind = TimelineUpdateKind::Append;
                slice.update_index = append_start as u32;
                slice.items = next[append_start..].to_vec();
            }
            (_, 0) => {
                slice.update_kind = TimelineUpdateKind::Prepend;
                slice.update_index = 0;
                slice.items = next[..prepend_count].to_vec();
            }
            _ => {
                slice.items = next.to_vec();
            }
        }
        slice
    }

    fn full_timeline_update(items: &[TimelineItem]) -> TimelineSlice {
        let mut full = TimelineSlice::empty_live();
        full.items = items.to_vec();
        full
    }

    fn coalesce_timeline_update(
        pending: Option<&TimelineSlice>,
        update: TimelineSlice,
        full_items: &[TimelineItem],
    ) -> TimelineSlice {
        let Some(pending) = pending else {
            return update;
        };

        match (pending.update_kind, update.update_kind) {
            (TimelineUpdateKind::MetadataOnly, _) => update,
            (_, TimelineUpdateKind::MetadataOnly) => pending.clone(),
            (TimelineUpdateKind::Append, TimelineUpdateKind::Append)
                if update.update_index
                    == pending
                        .update_index
                        .saturating_add(pending.items.len() as u32) =>
            {
                let mut merged = pending.clone();
                merged.items.extend(update.items);
                merged
            }
            (TimelineUpdateKind::Prepend, TimelineUpdateKind::Prepend)
                if pending.update_index == 0 && update.update_index == 0 =>
            {
                let mut merged = update;
                merged.items.extend(pending.items.clone());
                merged
            }
            _ => Self::full_timeline_update(full_items),
        }
    }

    /// Fold a `Full` snapshot of `items` into the room's pending `update_cache`
    /// entry so the next incremental pull carries it. Lets an out-of-band item
    /// patch (e.g. a late URL-preview attach) reach the UI deterministically
    /// instead of relying on the drained→`Full` fallback, which a concurrent
    /// `MetadataOnly`/`Append` diff pending in `update_cache` would preempt.
    pub(crate) async fn stage_full_update(
        update_cache: &TimelineUpdateCache,
        room_id: &str,
        items: &[TimelineItem],
    ) {
        let mut guard = update_cache.write().await;
        let merged = Self::coalesce_timeline_update(
            guard.get(room_id),
            Self::full_timeline_update(items),
            items,
        );
        guard.insert(room_id.to_string(), merged);
    }

    pub(crate) async fn cache_timeline_diffs(
        room_id: &str,
        timeline: &SdkTimeline,
        diffs: Vec<TimelineDiff>,
        context: &TimelineCacheContext,
    ) -> bool {
        let mut update_kind = TimelineUpdateKind::MetadataOnly;
        let mut edge_items: Vec<Arc<matrix_sdk_ui::timeline::TimelineItem>> = Vec::new();
        // Same-event in-place replacements (edits/reactions/receipts) carried by
        // TimelineDiff::Changed; handled after the loop by convert-and-compare.
        let mut changed_items: Vec<Arc<matrix_sdk_ui::timeline::TimelineItem>> = Vec::new();

        for diff in diffs {
            match diff {
                TimelineDiff::Append(values) => {
                    if update_kind == TimelineUpdateKind::Prepend {
                        return false;
                    }
                    update_kind = TimelineUpdateKind::Append;
                    edge_items.extend(values);
                }
                TimelineDiff::PushBack(value) => {
                    if update_kind == TimelineUpdateKind::Prepend {
                        return false;
                    }
                    update_kind = TimelineUpdateKind::Append;
                    edge_items.push(value);
                }
                TimelineDiff::PushFront(value) => {
                    if update_kind == TimelineUpdateKind::Append {
                        return false;
                    }
                    update_kind = TimelineUpdateKind::Prepend;
                    edge_items.insert(0, value);
                }
                TimelineDiff::Prepend(values) => {
                    if update_kind == TimelineUpdateKind::Append {
                        return false;
                    }
                    update_kind = TimelineUpdateKind::Prepend;
                    // Newer prepend batches in the same flush go further front, so
                    // splice these before the already-collected front edge items.
                    let mut combined = values;
                    combined.extend(std::mem::take(&mut edge_items));
                    edge_items = combined;
                }
                TimelineDiff::Changed(values) => changed_items.extend(values),
                TimelineDiff::Full => return false,
            }
        }

        let previous_items = {
            let guard = context.cache.read().await;
            guard.get(room_id).cloned().unwrap_or_default()
        };
        if previous_items.is_empty() {
            return false;
        }

        // In-place replacements: convert each changed event and compare to its
        // cached converted form. If every one is byte-identical (e.g. a read
        // receipt we don't render), skip the whole-window reconvert entirely;
        // any real change (edit/reaction), an unconvertible item, or a batch that
        // also grew the window → fall back to the Full snapshot.
        if !changed_items.is_empty() {
            if !edge_items.is_empty() {
                return false;
            }
            let own_user_id = timeline.room().own_user_id().to_owned();
            let room_is_encrypted = timeline.room().encryption_state().is_encrypted();
            let pinned_event_ids: HashSet<String> = timeline
                .room()
                .pinned_event_ids()
                .unwrap_or_default()
                .into_iter()
                .map(|id| id.to_string())
                .collect();
            let room_reply_previews: HashMap<String, ReplyPreview> = {
                let guard = context.reply_preview_cache.read().await;
                guard
                    .iter()
                    .filter(|&((cached_room_id, _), _)| cached_room_id == room_id)
                    .map(|((_, event_id), preview)| (event_id.clone(), preview.clone()))
                    .collect()
            };
            for item in &changed_items {
                let Some(event) = item.as_event() else {
                    return false;
                };
                let key = event
                    .event_id()
                    .map(|id| id.to_string())
                    .or_else(|| event.transaction_id().map(|txid| txid.to_string()))
                    .unwrap_or_default();
                let mut converted = match TimelineConversionService::convert_timeline_item(
                    item.as_ref(),
                    &pinned_event_ids,
                    &context.media_sources,
                    &own_user_id,
                    room_is_encrypted,
                    room_reply_previews.get(&key),
                ) {
                    Some(c) => c,
                    None => return false,
                };
                apply_pending_forward_meta(
                    std::slice::from_mut(&mut converted),
                    &context.pending_forward_meta,
                );
                let converted = converted;
                let is_unchanged = match previous_items
                    .iter()
                    .find(|c| c.event_id == converted.event_id)
                {
                    Some(cached) => Self::converted_matches_cached(cached, converted),
                    None => false, // not cached → Full snapshot
                };
                if !is_unchanged {
                    return false; // real change → Full snapshot
                }
            }
            // Nothing rendered changed: emit a metadata-only refresh (pagination
            // flags etc.) without reconverting the window.
            let mut update = TimelineSlice::empty_live();
            update.update_kind = TimelineUpdateKind::MetadataOnly;
            let mut update_guard = context.update_cache.write().await;
            let update =
                Self::coalesce_timeline_update(update_guard.get(room_id), update, &previous_items);
            update_guard.insert(room_id.to_string(), update);
            return true;
        }

        if update_kind == TimelineUpdateKind::MetadataOnly || edge_items.is_empty() {
            let mut update = TimelineSlice::empty_live();
            update.update_kind = TimelineUpdateKind::MetadataOnly;
            let mut update_guard = context.update_cache.write().await;
            let update =
                Self::coalesce_timeline_update(update_guard.get(room_id), update, &previous_items);
            update_guard.insert(room_id.to_string(), update);
            return true;
        }

        let own_user_id = timeline.room().own_user_id().to_owned();
        let room_is_encrypted = timeline.room().encryption_state().is_encrypted();

        if edge_items
            .iter()
            .filter_map(|item| item.as_event())
            .any(|event| {
                event
                    .read_receipts()
                    .keys()
                    .any(|user_id| user_id.as_str() != own_user_id.as_str())
            })
        {
            return false;
        }

        let acknowledged_transaction_ids: HashSet<String> = edge_items
            .iter()
            .filter_map(|item| item.as_event())
            .filter_map(|event| {
                event
                    .event_id()
                    .map(|_| ())
                    .and(event.transaction_id().map(|txid| txid.to_string()))
            })
            .collect();
        if !acknowledged_transaction_ids.is_empty()
            && previous_items.iter().any(|item| {
                item.transaction_id
                    .as_ref()
                    .is_some_and(|txid| acknowledged_transaction_ids.contains(txid))
                    || acknowledged_transaction_ids.contains(&item.event_id)
            })
        {
            return false;
        }

        if TimelineConversionService::schedule_reply_details_prefetch(timeline, &edge_items).await {
            return false;
        }

        let pinned_event_ids: HashSet<String> = timeline
            .room()
            .pinned_event_ids()
            .unwrap_or_default()
            .into_iter()
            .map(|id| id.to_string())
            .collect();
        let room_reply_previews: HashMap<String, ReplyPreview> = {
            let guard = context.reply_preview_cache.read().await;
            guard
                .iter()
                .filter(|&((cached_room_id, _event_id), _preview)| cached_room_id == room_id)
                .map(|((cached_room_id, event_id), preview)| {
                    let _ = cached_room_id;
                    (event_id.clone(), preview.clone())
                })
                .collect()
        };

        let mut converted: Vec<TimelineItem> = edge_items
            .iter()
            .filter_map(|item| {
                let event = item.as_event()?;
                if Self::is_redundant_local_timeline_echo(
                    event.event_id().map(|id| id.as_str()),
                    event.transaction_id().map(|txid| txid.as_str()),
                    &acknowledged_transaction_ids,
                ) {
                    return None;
                }
                TimelineConversionService::convert_timeline_item(
                    item.as_ref(),
                    &pinned_event_ids,
                    &context.media_sources,
                    &own_user_id,
                    room_is_encrypted,
                    room_reply_previews.get(
                        &event
                            .event_id()
                            .map(|id| id.to_string())
                            .or_else(|| event.transaction_id().map(|txid| txid.to_string()))
                            .unwrap_or_default(),
                    ),
                )
            })
            .collect();
        apply_pending_forward_meta(&mut converted, &context.pending_forward_meta);

        let previous_keys: HashSet<&str> = previous_items
            .iter()
            .map(Self::timeline_item_key)
            .filter(|key| !key.is_empty())
            .collect();
        if converted
            .iter()
            .map(Self::timeline_item_key)
            .any(|key| !key.is_empty() && previous_keys.contains(key))
        {
            return false;
        }

        Self::cache_reply_previews(room_id, &converted, &context.reply_preview_cache).await;
        Self::apply_sender_avatar_cache(room_id, &mut converted, &context.sender_avatar_cache)
            .await;
        Self::apply_cached_url_previews(&mut converted, &context.preview_cache).await;
        Self::index_encrypted_messages(
            timeline,
            room_id,
            &converted,
            &context.search_index,
            &context.search_index_fingerprints,
        )
        .await;

        let update_index;
        let full_after = {
            let mut cache_guard = context.cache.write().await;
            let items = cache_guard.entry(room_id.to_string()).or_default();
            update_index = if update_kind == TimelineUpdateKind::Append {
                items.len() as u32
            } else {
                0
            };
            if update_kind == TimelineUpdateKind::Append {
                items.extend(converted.clone());
            } else {
                items.splice(0..0, converted.clone());
            }
            items.clone()
        };

        {
            let mut update = TimelineSlice::empty_live();
            update.update_kind = if converted.is_empty() {
                TimelineUpdateKind::MetadataOnly
            } else {
                update_kind
            };
            update.update_index = update_index;
            update.items = converted;

            let mut update_guard = context.update_cache.write().await;
            let update =
                Self::coalesce_timeline_update(update_guard.get(room_id), update, &full_after);
            update_guard.insert(room_id.to_string(), update);
        }

        reconcile_pending_reaction_overrides(
            room_id,
            &full_after,
            &context.pending_reaction_overrides,
        )
        .await;
        true
    }

    pub(crate) async fn cache_timeline_snapshot(
        room_id: &str,
        timeline: &SdkTimeline,
        context: &TimelineCacheContext,
    ) {
        let own_user_id = timeline.room().own_user_id().to_owned();
        let room_is_encrypted = timeline.room().encryption_state().is_encrypted();
        let pinned_event_ids: HashSet<String> = timeline
            .room()
            .pinned_event_ids()
            .unwrap_or_default()
            .into_iter()
            .map(|id| id.to_string())
            .collect();
        let mut all_items: Vec<_> = timeline.items().await.into_iter().collect();
        let acknowledged_transaction_ids: HashSet<String> = all_items
            .iter()
            .filter_map(|item| item.as_event())
            .filter_map(|event| {
                event
                    .event_id()
                    .map(|_| ())
                    .and(event.transaction_id().map(|txid| txid.to_string()))
            })
            .collect();

        let previous_items = {
            let guard = context.cache.read().await;
            guard.get(room_id).cloned().unwrap_or_default()
        };
        // Borrow into previous_items (no deep clone): the map lives only until the
        // `converted` build below finishes; previous_items is re-borrowed shared
        // for the delta slice afterwards, so NLL drops this first.
        let prev_items: HashMap<&str, &TimelineItem> = previous_items
            .iter()
            .map(|i| (i.event_id.as_str(), i))
            .collect();
        let room_reply_previews: HashMap<String, ReplyPreview> = {
            let guard = context.reply_preview_cache.read().await;
            guard
                .iter()
                .filter(|&((cached_room_id, _event_id), _preview)| cached_room_id == room_id)
                .map(|((cached_room_id, event_id), preview)| {
                    let _ = cached_room_id;
                    (event_id.clone(), preview.clone())
                })
                .collect()
        };

        if TimelineConversionService::schedule_reply_details_prefetch(timeline, &all_items).await {
            all_items = timeline.items().await.into_iter().collect();
        }

        let mut converted: Vec<TimelineItem> = all_items
            .iter()
            .filter_map(|item| {
                let event = item.as_event()?;
                if Self::is_redundant_local_timeline_echo(
                    event.event_id().map(|id| id.as_str()),
                    event.transaction_id().map(|txid| txid.as_str()),
                    &acknowledged_transaction_ids,
                ) {
                    return None;
                }
                if let Some((id, tag)) = Self::item_fingerprint(event) {
                    if let Some(&cached) = prev_items.get(id.as_str()) {
                        let cached_tag = match cached.content {
                            MessageContent::UnableToDecrypt { .. } => "utd",
                            MessageContent::Service { .. } => "service",
                            _ => "msg",
                        };
                        if cached_tag == tag {
                            let mut reused = cached.clone();
                            // The content fingerprint ignores the sender profile,
                            // so refresh it here — otherwise a name/avatar that
                            // fetch_members resolved after first render is dropped
                            // and the sender stays a raw MXID.
                            reused.sender =
                                TimelineConversionService::extract_sender_profile(event);
                            let (send_state, upload_progress) =
                                TimelineConversionService::derive_send_state(
                                    event.send_state(),
                                    event.is_own(),
                                    &reused.sender.user_id,
                                    event.read_receipts().keys().map(|u| u.as_str()),
                                );
                            reused.send_state = send_state;
                            reused.upload_progress = upload_progress;
                            reused.is_pinned = pinned_event_ids.contains(&id);
                            reused.is_encrypted = room_is_encrypted;
                            reused.reactions = match event.content() {
                                TimelineItemContent::MsgLike(msg_like) => {
                                    let reply_preview =
                                        TimelineConversionService::merge_reply_preview(
                                            TimelineConversionService::extract_reply_preview(
                                                msg_like,
                                                &context.media_sources,
                                            ),
                                            reused.reply_preview.as_ref(),
                                            room_reply_previews.get(&id),
                                        );
                                    reused.reply_to_event_id = msg_like
                                        .in_reply_to
                                        .as_ref()
                                        .map(|reply| reply.event_id.to_string());
                                    reused.reply_preview = reply_preview;
                                    TimelineConversionService::convert_reactions(
                                        msg_like,
                                        &own_user_id,
                                    )
                                }
                                _ => {
                                    reused.reply_to_event_id = None;
                                    reused.reply_preview = None;
                                    Vec::new()
                                }
                            };
                            return Some(reused);
                        }
                    }
                }
                TimelineConversionService::convert_timeline_item(
                    item.as_ref(),
                    &pinned_event_ids,
                    &context.media_sources,
                    &own_user_id,
                    room_is_encrypted,
                    room_reply_previews.get(
                        &item
                            .as_event()
                            .and_then(|event| {
                                event
                                    .event_id()
                                    .map(|id| id.to_string())
                                    .or_else(|| event.transaction_id().map(|txid| txid.to_string()))
                            })
                            .unwrap_or_default(),
                    ),
                )
            })
            .collect();

        Self::cache_reply_previews(room_id, &converted, &context.reply_preview_cache).await;
        Self::apply_sender_avatar_cache(room_id, &mut converted, &context.sender_avatar_cache)
            .await;
        Self::resolve_stuck_sender_profiles(timeline.room(), &mut converted).await;
        Self::propagate_read_receipt_frontier(&own_user_id, &all_items, &mut converted);
        Self::apply_cached_url_previews(&mut converted, &context.preview_cache).await;
        Self::index_encrypted_messages(
            timeline,
            room_id,
            &converted,
            &context.search_index,
            &context.search_index_fingerprints,
        )
        .await;

        {
            let update = Self::timeline_delta_slice(&previous_items, &converted);
            let mut update_guard = context.update_cache.write().await;
            let update =
                Self::coalesce_timeline_update(update_guard.get(room_id), update, &converted);
            update_guard.insert(room_id.to_string(), update);
        }

        // Run the reaction reconcile (reads &converted only — never context.cache)
        // BEFORE the cache insert so the insert can MOVE converted instead of
        // deep-cloning the whole window.
        reconcile_pending_reaction_overrides(
            room_id,
            &converted,
            &context.pending_reaction_overrides,
        )
        .await;

        {
            let mut cache_guard = context.cache.write().await;
            cache_guard.insert(room_id.to_string(), converted);
        }
    }

    /// Re-resolve senders the SDK left stuck as an empty `Ready(Profile::default())`
    /// — its `display_name` equals the raw MXID. Under lazy member loading the SDK
    /// stamps this at build time when `are_members_synced()` is already true but the
    /// specific member isn't in the store yet, and it NEVER revisits a `Ready`
    /// profile (both `fetch_members` passes skip `Ready`, and `force_update_sender_profiles`
    /// is `pub(super)`). So query the member store directly here — exactly what the
    /// user-info popup does via `room.get_member(...)` — which resolves once the store
    /// is complete. Deduped within the window; only stuck senders are queried, and once
    /// patched they persist in the cache (the reuse path clones them), so this is a
    /// no-op on subsequent snapshots except for genuinely name-less senders.
    async fn resolve_stuck_sender_profiles(
        room: &matrix_sdk::Room,
        converted: &mut [TimelineItem],
    ) {
        let mut seen = HashSet::new();
        let stuck: Vec<String> = converted
            .iter()
            .filter(|it| it.sender.display_name == it.sender.user_id)
            .map(|it| it.sender.user_id.clone())
            .filter(|uid| seen.insert(uid.clone()))
            .collect();
        if stuck.is_empty() {
            return;
        }

        let owner = crate::profile_cache_store::owner_key(&room.client());
        let mut resolved: HashMap<String, UserProfile> = HashMap::new();
        for uid_str in stuck {
            let Ok(uid) = matrix_sdk::ruma::UserId::parse(&uid_str) else {
                continue;
            };
            // 1. Room member store + prev_content — both LOCAL, no network.
            if let Ok(Some(member)) = room.get_member_no_sync(&uid).await {
                let name =
                    crate::room_member_service::RoomMemberService::member_display_name(&member);
                let avatar_url = member.avatar_url().map(|u| u.to_string());
                if name.is_some() || avatar_url.is_some() {
                    resolved.insert(
                        uid_str.clone(),
                        UserProfile {
                            display_name: name.unwrap_or_else(|| uid_str.clone()),
                            avatar_url,
                            user_id: uid_str,
                        },
                    );
                    continue;
                }
            }
            // 2. Global `/profile` CACHE lookup ONLY — never a network fetch here.
            // A miss leaves the MXID in place; the background prefetch
            // (`prefetch_global_profiles_for_timeline`) fetches it and forces a
            // re-snapshot, so the render path never blocks on the network.
            if let Some(Some(profile)) = Self::global_profile_lookup(&owner, &uid_str).await {
                resolved.insert(uid_str, profile);
            }
        }

        if resolved.is_empty() {
            return;
        }
        for item in converted.iter_mut() {
            if let Some(profile) = resolved.get(&item.sender.user_id) {
                item.sender = profile.clone();
            }
        }
    }

    /// Background (off the render path) prefetch of the global `/profile` for every
    /// distinct timeline sender that isn't locally resolvable, populating the shared
    /// cache. Driven from `trigger_member_fetch`, which forces a full re-snapshot
    /// afterwards so `resolve_stuck_sender_profiles` serves the names from cache —
    /// the render path never awaits the network. Remote/departed users whose
    /// `/profile` is gone (404) are cached as absent and not retried.
    pub(crate) async fn prefetch_global_profiles_for_timeline(timeline: &SdkTimeline) {
        let room = timeline.room();
        let client = room.client();
        let items = timeline.items().await;

        // Unique senders (cheap, in-memory).
        let mut seen = HashSet::new();
        let mut candidates: Vec<matrix_sdk::ruma::OwnedUserId> = Vec::new();
        for item in items.iter() {
            if let Some(event) = item.as_event() {
                let uid = event.sender();
                if seen.insert(uid.to_owned()) {
                    candidates.push(uid.to_owned());
                }
            }
        }

        // Which senders need a global fetch: locally unresolvable (no member-store /
        // prev_content name) AND not already cached. The member-store reads run
        // concurrently — sequentially they were ~90ms each under startup SQLite
        // contention (seconds for a busy room) — but BOUNDED, so this background scan
        // can't monopolise the shared pool against the timeline build beside it.
        let owner = crate::profile_cache_store::owner_key(&client);
        let owner = owner.as_str();
        let checks = candidates.into_iter().map(|uid| async move {
            let local_name = {
                let _permit = member_probe_semaphore().acquire().await.ok();
                room.get_member_no_sync(&uid)
                    .await
                    .ok()
                    .flatten()
                    .and_then(|m| {
                        crate::room_member_service::RoomMemberService::member_display_name(&m)
                    })
            };
            if local_name.is_some() {
                return None;
            }
            if Self::global_profile_lookup(owner, uid.as_str())
                .await
                .is_some()
            {
                return None;
            }
            Some(uid)
        });
        let needs: Vec<matrix_sdk::ruma::OwnedUserId> = futures_util::future::join_all(checks)
            .await
            .into_iter()
            .flatten()
            .collect();
        if !needs.is_empty() {
            let fetches = needs
                .iter()
                .map(|uid| Self::global_profile(&client, uid.as_str(), uid));
            futures_util::future::join_all(fetches).await;
        }
    }

    /// Read-only lookup of the remembered global `/profile` result: `Some(Some(p))`
    /// a profile, `Some(None)` a known absence (404 / nameless), `None` not resolved
    /// yet. No network, no disk — this is the timeline render path's read, and the
    /// memo is seeded from disk at session start.
    async fn global_profile_lookup(owner: &str, uid_str: &str) -> Option<Option<UserProfile>> {
        crate::profile_cache_store::lookup(owner, uid_str).await
    }

    /// A user's global profile (name + avatar), remembered across runs so each user is
    /// fetched at most once (and a name we resolved before an account was deleted
    /// survives). Call OFF the render path (network).
    async fn global_profile(
        client: &matrix_sdk::Client,
        uid_str: &str,
        uid: &matrix_sdk::ruma::UserId,
    ) -> Option<UserProfile> {
        if let Some(cached) = crate::profile_cache_store::lookup(
            &crate::profile_cache_store::owner_key(client),
            uid_str,
        )
        .await
        {
            return cached;
        }

        const MAX_ATTEMPTS: u32 = 3;
        for attempt in 0..MAX_ATTEMPTS {
            let result = {
                let _permit = profile_fetch_semaphore().acquire().await.ok();
                client.account().fetch_user_profile_of(uid).await
            };
            // Raw fields — `remember` builds the profile, so the sticky-positive rule
            // lives in exactly one place. Both `None` records a real absence.
            let (display_name, avatar_url) = match result {
                Ok(response) => (
                    response
                        .get("displayname")
                        .and_then(|v| v.as_str())
                        .filter(|s| !s.is_empty())
                        .map(str::to_string),
                    response
                        .get("avatar_url")
                        .and_then(|v| v.as_str())
                        .filter(|s| !s.is_empty())
                        .map(str::to_string),
                ),
                Err(e) if profile_error_is_absent(&e) => {
                    // 404: a departed / deleted user really has no profile.
                    (None, None)
                }
                Err(_) if attempt + 1 < MAX_ATTEMPTS => {
                    // Transient (timeout / 429 / 5xx / offline). Back off and retry.
                    tokio::time::sleep(std::time::Duration::from_millis(500u64 << attempt)).await;
                    continue;
                }
                Err(e) => {
                    // Still transient after retries. Do NOT remember: recording this as
                    // "no profile" would pin the sender to a raw MXID for the rest of
                    // the session (the prefetch runs once per room), which is exactly
                    // the cold-start-under-contention case this resolution exists for.
                    warn!(user = %uid_str, error = %e, "global /profile failed (transient — not cached)");
                    return None;
                }
            };
            // Only definitive outcomes reach here, so they are safe to remember.
            return crate::profile_cache_store::remember(client, uid_str, display_name, avatar_url)
                .await;
        }
        None
    }

    async fn cache_reply_previews(
        room_id: &str,
        converted: &[TimelineItem],
        reply_preview_cache: &ReplyPreviewCache,
    ) {
        let mut guard = reply_preview_cache.write().await;
        for item in converted.iter() {
            let Some(preview) = item.reply_preview.as_ref() else {
                continue;
            };
            if preview.is_unavailable || item.event_id.is_empty() {
                continue;
            }
            guard.insert(
                (room_id.to_string(), item.event_id.clone()),
                preview.clone(),
            );
        }
    }

    async fn apply_sender_avatar_cache(
        room_id: &str,
        converted: &mut [TimelineItem],
        sender_avatar_cache: &SenderAvatarCache,
    ) {
        let cached_sender_avatars = {
            let guard = sender_avatar_cache.read().await;
            guard.get(room_id).cloned().unwrap_or_default()
        };
        let mut sender_avatar_updates = HashMap::new();
        for item in converted.iter_mut() {
            if item.sender.user_id.is_empty() {
                continue;
            }
            if item.sender.avatar_url.is_none() {
                if let Some(cached_avatar) = cached_sender_avatars.get(&item.sender.user_id) {
                    item.sender.avatar_url = cached_avatar.clone();
                }
            }
            if item.sender.avatar_url.is_some()
                || !cached_sender_avatars.contains_key(&item.sender.user_id)
            {
                sender_avatar_updates
                    .insert(item.sender.user_id.clone(), item.sender.avatar_url.clone());
            }
        }
        if !sender_avatar_updates.is_empty() {
            let mut guard = sender_avatar_cache.write().await;
            let room_cache = guard.entry(room_id.to_string()).or_default();
            for (sender_id, avatar_url) in sender_avatar_updates {
                room_cache.insert(sender_id, avatar_url);
            }
        }
    }

    fn propagate_read_receipt_frontier(
        own_user_id: &matrix_sdk::ruma::UserId,
        all_items: &[Arc<matrix_sdk_ui::timeline::TimelineItem>],
        converted: &mut [TimelineItem],
    ) {
        let own_uid = own_user_id.as_str();
        let mut receipt_frontier: Option<usize> = None;
        for (idx, sdk_item) in all_items.iter().enumerate() {
            if let Some(event) = sdk_item.as_event() {
                let has_non_own_receipt = event
                    .read_receipts()
                    .keys()
                    .any(|uid| uid.as_str() != own_uid);
                if has_non_own_receipt {
                    receipt_frontier = Some(idx);
                }
            }
        }

        if let Some(frontier_sdk_idx) = receipt_frontier {
            let frontier_event_ids: HashSet<String> = all_items
                .iter()
                .take(frontier_sdk_idx + 1)
                .filter_map(|i| i.as_event())
                .filter_map(|e| e.event_id().map(|id| id.to_string()))
                .collect();

            let mut past_frontier = false;
            for item in converted.iter_mut().rev() {
                if !past_frontier && frontier_event_ids.contains(&item.event_id) {
                    past_frontier = true;
                }
                if past_frontier && item.is_outgoing && item.send_state == SendState::Sent {
                    item.send_state = SendState::Read;
                }
            }
        } else {
            let mut seen_read = false;
            for item in converted.iter_mut().rev() {
                if item.is_outgoing {
                    if item.send_state == SendState::Read {
                        seen_read = true;
                    } else if seen_read && item.send_state == SendState::Sent {
                        item.send_state = SendState::Read;
                    }
                }
            }
        }
    }

    /// Compare a freshly-converted item against its cached form for the
    /// receipt-noop fast path, ignoring fields that are only ever attached AFTER
    /// conversion. Today that is `url_preview`: the preview layer patches fetched
    /// OG cards into the cached item, but a fresh convert always has `None`
    /// ([`TimelineConversionService::convert_timeline_item`] never sets it). Without
    /// grafting the cached preview across, a link-bearing message whose card has
    /// arrived would never compare equal to its reconvert, forcing a full-window
    /// snapshot on every read receipt that touches it.
    pub(crate) fn converted_matches_cached(
        cached: &TimelineItem,
        mut converted: TimelineItem,
    ) -> bool {
        converted.url_preview = cached.url_preview.clone();
        *cached == converted
    }

    async fn apply_cached_url_previews(
        converted: &mut [TimelineItem],
        preview_cache: &UrlPreviewCache,
    ) {
        let pc = preview_cache.read().await;
        for item in converted.iter_mut() {
            if item.url_preview.is_some() {
                continue;
            }
            let (body, fmt) = match &item.content {
                MessageContent::Text {
                    body,
                    formatted_body,
                } => (body.as_str(), formatted_body.as_deref()),
                _ => continue,
            };
            if let Some(url) = Self::extract_url_with_formatted(body, fmt) {
                if let Some(cached) = pc.get(&url) {
                    item.url_preview = cached.clone();
                }
            }
        }
    }

    async fn index_encrypted_messages(
        timeline: &SdkTimeline,
        room_id: &str,
        converted: &[TimelineItem],
        search_index: &SearchIndex,
        search_index_fingerprints: &SearchIndexFingerprints,
    ) {
        // Live indexing is off while the user has disabled E2EE search.
        if !crate::search_service::e2ee_search_enabled() {
            return;
        }
        if !timeline.room().encryption_state().is_encrypted() {
            return;
        }

        let room_key = room_id.to_string();
        let (batch, fingerprint_updates) = {
            let guard = search_index_fingerprints.read().await;
            let mut batch = Vec::new();
            let mut fingerprint_updates = Vec::new();
            for item in converted.iter() {
                if item.event_id.is_empty() {
                    continue;
                }
                let body = match &item.content {
                    MessageContent::Text { body, .. } => body,
                    _ => continue,
                };
                let ts = item
                    .timestamp
                    .duration_since(UNIX_EPOCH)
                    .unwrap_or_default()
                    .as_secs() as i64;
                let fingerprint = Self::search_index_fingerprint(item, body, ts);
                let key = (room_key.clone(), item.event_id.clone());
                if guard.get(&key) == Some(&fingerprint) {
                    continue;
                }
                fingerprint_updates.push((item.event_id.clone(), fingerprint));
                batch.push((
                    item.event_id.clone(),
                    room_key.clone(),
                    item.sender.user_id.clone(),
                    item.sender.display_name.clone(),
                    body.clone(),
                    ts,
                ));
            }
            (batch, fingerprint_updates)
        };
        let mut indexed_batch = false;
        if !batch.is_empty() {
            // Index on a BLOCKING thread. index_batch is a synchronous SQLCipher FTS5
            // write taken while holding the std::sync index mutex, so running it here
            // stalled a tokio worker for its whole duration — exactly what the same
            // call in search_backfill.rs guards against with its own spawn_blocking.
            // This LIVE path is per timeline update, so a cold start (every encrypted
            // room snapshotting at once, all serialized on that one mutex) starved the
            // executor: a no-op yield_now measured 570ms, and every await in an
            // unrelated room's open paid it — that room's event_cache() alone hit 5.3s
            // despite the room being unencrypted and only 12 items long.
            let search_index = search_index.clone();
            let batch_ms = std::time::Instant::now();
            indexed_batch = tokio::task::spawn_blocking(move || {
                let Ok(mut idx_guard) = search_index.lock() else {
                    return false;
                };
                // Borrow ends with this `map` so we can drop the connection in
                // the error branch without a borrow conflict.
                let result = idx_guard.as_ref().map(|idx| idx.index_batch(&batch));
                match result {
                    Some(Ok(_)) => true,
                    Some(Err(e)) => {
                        warn!("Search index batch insert failed: {e}");
                        // The connection's file was moved/deleted out from under
                        // it (DBMOVED); drop it so the next
                        // ensure_search_index_open() reopens a fresh one instead
                        // of failing on every future insert.
                        *idx_guard = None;
                        false
                    }
                    None => false,
                }
            })
            .await
            .unwrap_or(false);
            let ms = batch_ms.elapsed().as_millis();
            if ms > 100 {
                tracing::info!(room = %room_id, ms, "live search index batch (now off-worker)");
            }
        }
        if indexed_batch {
            let mut guard = search_index_fingerprints.write().await;
            for (event_id, fingerprint) in fingerprint_updates {
                guard.insert((room_id.to_string(), event_id), fingerprint);
            }
        }
    }

    pub(crate) fn is_redundant_local_timeline_echo(
        event_id: Option<&str>,
        transaction_id: Option<&str>,
        acknowledged_transaction_ids: &HashSet<String>,
    ) -> bool {
        event_id.is_none()
            && transaction_id.is_some_and(|txid| acknowledged_transaction_ids.contains(txid))
    }

    pub(crate) fn extract_url(body: &str) -> Option<String> {
        Self::extract_url_with_formatted(body, None)
    }

    pub(crate) fn extract_url_with_formatted(
        body: &str,
        formatted_body: Option<&str>,
    ) -> Option<String> {
        if let Some(html) = formatted_body {
            if let Some(url) = Self::extract_url_from_html(html) {
                return Some(url);
            }
        }

        body.split_whitespace()
            .find_map(Self::clean_url_token)
            .map(str::to_string)
            .filter(|url| Self::is_previewable_url(url))
    }

    /// Clean a whitespace-delimited token into a bare http(s) URL, or `None` if
    /// it isn't one. Strips leading wrappers (so `(https://x)` extracts) then
    /// trailing punctuation with paren balancing (so a Wikipedia
    /// `..._(programming_language)` link keeps its closing paren while a stray
    /// `https://x/a).` loses it).
    fn clean_url_token(token: &str) -> Option<&str> {
        let token = token.trim_start_matches(['(', '<', '"', '\'']);
        if !(token.starts_with("https://") || token.starts_with("http://")) {
            return None;
        }
        Some(Self::trim_url_end(token))
    }

    /// Strip trailing punctuation from a URL, removing a trailing `)` only when
    /// it is unbalanced (more `)` than `(` in the candidate). Loops so mixed
    /// tails like `).,` fully unwind. All trimmed characters are ASCII, so the
    /// byte indexing stays on char boundaries.
    fn trim_url_end(url: &str) -> &str {
        let mut end = url.len();
        loop {
            let candidate = &url[..end];
            // Trailing punctuation that is never part of a URL (note: no ')').
            let trimmed = candidate.trim_end_matches(|c: char| ".,;:!?>\"'".contains(c));
            let mut new_end = trimmed.len();
            if trimmed.ends_with(')') && trimmed.matches(')').count() > trimmed.matches('(').count()
            {
                new_end -= 1; // drop one unbalanced ')' (1 ASCII byte)
            }
            if new_end == end {
                return candidate;
            }
            end = new_end;
        }
    }

    fn extract_url_from_html(html: &str) -> Option<String> {
        let lower = html.to_ascii_lowercase();
        let mut offset = 0usize;
        while let Some(rel_pos) = lower[offset..].find("href=") {
            offset += rel_pos + 5;
            let rest = html[offset..].trim_start();
            let url = if let Some(s) = rest.strip_prefix('"') {
                let end = s.find('"').unwrap_or(s.len());
                s[..end].trim().to_string()
            } else if let Some(s) = rest.strip_prefix('\'') {
                let end = s.find('\'').unwrap_or(s.len());
                s[..end].trim().to_string()
            } else {
                let end = rest
                    .find(|c: char| c.is_ascii_whitespace() || c == '>')
                    .unwrap_or(rest.len());
                rest[..end].trim().to_string()
            };
            if (url.starts_with("https://") || url.starts_with("http://"))
                && Self::is_previewable_url(&url)
            {
                return Some(url);
            }
        }
        None
    }

    fn is_previewable_url(url: &str) -> bool {
        !url.starts_with("https://matrix.to/") && !url.starts_with("http://matrix.to/")
    }
}

#[cfg(test)]
mod tests {
    use super::TimelineCacheService;
    use crate::types::{
        MessageContent, PreviewType, SendState, TimelineItem, UrlPreview, UserProfile,
    };
    use std::collections::HashSet;
    use std::time::{Duration, UNIX_EPOCH};

    fn text_item(event_id: &str, body: &str) -> TimelineItem {
        TimelineItem {
            event_id: event_id.into(),
            transaction_id: None,
            sender: UserProfile {
                user_id: "@alice:localhost".into(),
                display_name: "Alice".into(),
                avatar_url: None,
            },
            timestamp: UNIX_EPOCH + Duration::from_secs(1),
            content: MessageContent::Text {
                body: body.into(),
                formatted_body: None,
            },
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

    fn sample_preview() -> UrlPreview {
        UrlPreview {
            url: "https://example.com".into(),
            site_name: Some("Example".into()),
            title: Some("Title".into()),
            description: None,
            image_url: None,
            image_width: 0,
            image_height: 0,
            preview_type: PreviewType::Article,
            duration_secs: 0,
            author: None,
            has_large_media: false,
            site_name_canonical: Some("example".into()),
        }
    }

    // A cached item that has since gained a URL preview still matches its fresh
    // reconvert (url_preview is always None fresh), so a read receipt stays a
    // no-op instead of forcing a full-window snapshot (regression from M5).
    #[test]
    fn converted_matches_cached_ignores_added_preview() {
        let mut cached = text_item("$e1", "https://example.com hi");
        cached.url_preview = Some(sample_preview());
        let converted = text_item("$e1", "https://example.com hi"); // fresh: None
        assert!(TimelineCacheService::converted_matches_cached(
            &cached, converted
        ));
    }

    // A genuine content change is still detected even when both carry a preview.
    #[test]
    fn converted_matches_cached_detects_real_change() {
        let mut cached = text_item("$e1", "original");
        cached.url_preview = Some(sample_preview());
        let mut converted = text_item("$e1", "edited");
        converted.url_preview = Some(sample_preview());
        assert!(!TimelineCacheService::converted_matches_cached(
            &cached, converted
        ));
    }

    // Baseline: identical items with no preview on either side match.
    #[test]
    fn converted_matches_cached_identical_no_preview() {
        let cached = text_item("$e1", "hi");
        let converted = text_item("$e1", "hi");
        assert!(TimelineCacheService::converted_matches_cached(
            &cached, converted
        ));
    }

    // A Wikipedia-style link with a balanced `(...)` keeps its closing paren
    // (M3: the old blind trim dropped it, fetching a 404 URL that was then
    // negative-cached for 30 days).
    #[test]
    fn extract_url_keeps_balanced_parens() {
        assert_eq!(
            TimelineCacheService::extract_url(
                "see https://en.wikipedia.org/wiki/Rust_(programming_language) ok"
            )
            .as_deref(),
            Some("https://en.wikipedia.org/wiki/Rust_(programming_language)")
        );
    }

    // A parenthesized URL with trailing punctuation strips the wrappers and the
    // unbalanced trailing paren but nothing internal.
    #[test]
    fn extract_url_strips_unbalanced_paren_and_punct() {
        assert_eq!(
            TimelineCacheService::extract_url("look (https://example.com/a).").as_deref(),
            Some("https://example.com/a")
        );
    }

    // A fully-wrapped URL still extracts (leading '(' stripped).
    #[test]
    fn extract_url_unwraps_leading_paren() {
        assert_eq!(
            TimelineCacheService::extract_url("(https://example.com)").as_deref(),
            Some("https://example.com")
        );
    }

    // Balanced parens in the middle of a path are left untouched.
    #[test]
    fn extract_url_leaves_inner_balanced_parens() {
        assert_eq!(
            TimelineCacheService::extract_url("https://x.com/a(b)c").as_deref(),
            Some("https://x.com/a(b)c")
        );
    }

    #[test]
    fn redundant_local_timeline_echo_requires_acknowledged_txid() {
        let acknowledged = HashSet::from([String::from("tx-1")]);
        assert!(TimelineCacheService::is_redundant_local_timeline_echo(
            None,
            Some("tx-1"),
            &acknowledged,
        ));
        assert!(!TimelineCacheService::is_redundant_local_timeline_echo(
            None,
            Some("tx-2"),
            &acknowledged,
        ));
        assert!(!TimelineCacheService::is_redundant_local_timeline_echo(
            Some("$event:example.org"),
            Some("tx-1"),
            &acknowledged,
        ));
    }
}

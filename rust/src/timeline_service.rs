// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::collections::HashMap;
use std::sync::atomic::AtomicBool;
use std::sync::{Arc, Mutex};

use anyhow::{anyhow, Result};
use matrix_sdk::ruma::events::room::MediaSource;
use matrix_sdk::Room;
use matrix_sdk_ui::timeline::Timeline as SdkTimeline;
use tokio::sync::RwLock;

use crate::room_summary_service::room_unread_count;
use crate::timeline_window::{TimelineDiff, TimelineWindow};
use crate::timeline_window_service::{
    TimelineChangedFactory, TimelineRuntime, TimelineWindowService,
};
use crate::types::{MessageContent, ReplyPreview, TimelineItem, TimelineSlice};

type TimelineMap = Arc<RwLock<HashMap<String, Arc<SdkTimeline>>>>;
type WindowMap = Arc<RwLock<HashMap<String, TimelineWindow>>>;
type TimelineCache = Arc<RwLock<HashMap<String, Vec<TimelineItem>>>>;
type TimelineUpdateCache = Arc<RwLock<HashMap<String, TimelineSlice>>>;
type RefreshStateMap = Arc<Mutex<HashMap<String, Arc<TimelineRefreshState>>>>;
type ReplyPreviewCache = Arc<RwLock<HashMap<(String, String), ReplyPreview>>>;
type PendingReactionOverrides = Arc<RwLock<HashMap<(String, String, String), bool>>>;
type MediaSourceMap = Arc<std::sync::RwLock<HashMap<String, MediaSource>>>;
type TimelineCallbacks = Arc<Mutex<HashMap<String, Box<dyn Fn() + Send>>>>;
type SenderAvatarCache = Arc<RwLock<HashMap<String, HashMap<String, Option<String>>>>>;
// Most-recently-used room ids (front = newest), driving the resident-window LRU.
type RecentRooms = Arc<Mutex<std::collections::VecDeque<String>>>;
type CreatingWindows = Arc<Mutex<std::collections::HashSet<String>>>;
// room_id -> (own read-receipt target event ids, when they were loaded).
type ReceiptTargetsMemo =
    Arc<Mutex<HashMap<String, (std::collections::HashSet<String>, std::time::Instant)>>>;

/// Per-room guard for expensive timeline cache refreshes.
///
/// SDK timeline streams can deliver more changes while a full snapshot is still
/// being converted. Keep one worker per room and collapse those later changes
/// into a single follow-up refresh.
#[derive(Default)]
pub(crate) struct TimelineRefreshState {
    pub(crate) running: AtomicBool,
    pub(crate) dirty: AtomicBool,
    pub(crate) pending_diffs: Mutex<Vec<TimelineDiff>>,
}

#[derive(Clone)]
pub(crate) struct TimelineService {
    pub(crate) timelines: TimelineMap,
    pub(crate) windows: WindowMap,
    pub(crate) cache: TimelineCache,
    pub(crate) update_cache: TimelineUpdateCache,
    pub(crate) refresh_states: RefreshStateMap,
    pub(crate) reply_preview_cache: ReplyPreviewCache,
    pub(crate) pending_reaction_overrides: PendingReactionOverrides,
    pub(crate) media_sources: MediaSourceMap,
    pub(crate) pending_forward_meta: crate::timeline_cache_service::PendingForwardMeta,
    pub(crate) callbacks: TimelineCallbacks,
    /// room_id -> sender_id -> avatar URL last seen in timeline/event profiles.
    pub(crate) sender_avatar_cache: SenderAvatarCache,
    /// LRU order of opened rooms; caps the number of resident timeline windows.
    pub(crate) recent_rooms: RecentRooms,
    /// Rooms whose timeline window is being built right now. Dedups the repeated
    /// cold-room polls that would otherwise each start a full rebuild.
    pub(crate) creating_windows: CreatingWindows,
    /// Short-lived per-room cache of our own read-receipt target event ids, so a
    /// burst of timeline updates doesn't re-issue the 4 receipt loads each time.
    receipt_targets_memo: ReceiptTargetsMemo,
    /// Rooms whose full member list has been fetched this session (via the
    /// timeline's fetch_members). Gates the one-shot per open so we don't re-run
    /// it — and its profile-Pending flicker — on every slice request.
    members_fetched: Arc<Mutex<std::collections::HashSet<String>>>,
}

impl TimelineService {
    pub(crate) fn new() -> Self {
        Self {
            timelines: Arc::new(RwLock::new(HashMap::new())),
            windows: Arc::new(RwLock::new(HashMap::new())),
            cache: Arc::new(RwLock::new(HashMap::new())),
            update_cache: Arc::new(RwLock::new(HashMap::new())),
            refresh_states: Arc::new(Mutex::new(HashMap::new())),
            reply_preview_cache: Arc::new(RwLock::new(HashMap::new())),
            pending_reaction_overrides: Arc::new(RwLock::new(HashMap::new())),
            media_sources: Arc::new(std::sync::RwLock::new(HashMap::new())),
            pending_forward_meta: Arc::new(std::sync::Mutex::new(HashMap::new())),
            callbacks: Arc::new(Mutex::new(HashMap::new())),
            sender_avatar_cache: Arc::new(RwLock::new(HashMap::new())),
            recent_rooms: Arc::new(Mutex::new(std::collections::VecDeque::new())),
            creating_windows: Arc::new(Mutex::new(std::collections::HashSet::new())),
            receipt_targets_memo: Arc::new(Mutex::new(HashMap::new())),
            members_fetched: Arc::new(Mutex::new(std::collections::HashSet::new())),
        }
    }

    /// Returns true the first time it is called for `room_id` this session,
    /// false afterwards — the caller then fetches the room's members exactly
    /// once. See [`members_fetched`](Self::members_fetched).
    pub(crate) fn claim_members_fetch(&self, room_id: &str) -> bool {
        if let Ok(mut set) = self.members_fetched.lock() {
            set.insert(room_id.to_string())
        } else {
            false
        }
    }

    /// The first unread message's event id: the first countable item *after*
    /// the user's read marker. `unread_count` is used only as a 0/non-zero gate
    /// (a stable signal); the position is derived from the read marker, never
    /// from a count-relative index, so it does not jump when the count and the
    /// loaded item set drift apart (which is what slid the C++ delimiter). When
    /// the marker is not in the loaded window (older than it — e.g. an all-unread
    /// live tail) the first countable item is used.
    pub(crate) fn first_unread_event_id(
        items: &[TimelineItem],
        read_marker_event_id: Option<&str>,
        unread_count: u32,
    ) -> Option<String> {
        if unread_count == 0 {
            return None;
        }

        if let Some(marker) = read_marker_event_id {
            if items.iter().any(|item| item.event_id == marker) {
                let mut after_marker = false;
                for item in items {
                    if after_marker {
                        if Self::item_counts_towards_unread(item) {
                            return Some(item.event_id.clone());
                        }
                    } else if item.event_id == marker {
                        after_marker = true;
                    }
                }
                // Marker is the last countable item — everything loaded is read.
                return None;
            }
        }

        // No marker, or the marker is not loaded (older than the window): the
        // first countable item is the first unread.
        items
            .iter()
            .find(|item| Self::item_counts_towards_unread(item))
            .map(|item| item.event_id.clone())
    }

    fn item_counts_towards_unread(item: &TimelineItem) -> bool {
        !item.event_id.is_empty()
            && !item.is_outgoing
            && !matches!(item.content, MessageContent::Service { .. })
    }

    /// The user's read-marker event id as it appears in `items`: the latest
    /// loaded message any of our own read receipts (public/private ×
    /// unthreaded/main, from any device) points at. `None` when no such receipt
    /// falls inside the loaded window. Mirrors notification_service's
    /// own_read_receipt_targets so `unread_count` and the delimiter agree.
    /// Event ids our own read receipts (public/private × unthreaded/main, any
    /// device) point at. The 4 async loads are memoized per room for 2s so a
    /// burst of timeline updates doesn't re-issue them each time; the TTL bounds
    /// staleness, and send_read_receipt/mark_room_read invalidate eagerly.
    async fn own_receipt_targets(&self, room: &Room) -> std::collections::HashSet<String> {
        use matrix_sdk::ruma::events::receipt::{ReceiptThread, ReceiptType};
        const TTL: std::time::Duration = std::time::Duration::from_secs(2);
        let room_id = room.room_id().to_string();
        if let Ok(memo) = self.receipt_targets_memo.lock() {
            if let Some((targets, at)) = memo.get(&room_id) {
                if at.elapsed() < TTL {
                    return targets.clone();
                }
            }
        }
        let own = room.own_user_id().to_owned();
        let mut targets = std::collections::HashSet::new();
        for (receipt_type, thread) in [
            (ReceiptType::Read, ReceiptThread::Unthreaded),
            (ReceiptType::Read, ReceiptThread::Main),
            (ReceiptType::ReadPrivate, ReceiptThread::Unthreaded),
            (ReceiptType::ReadPrivate, ReceiptThread::Main),
        ] {
            if let Ok(Some((event_id, _))) =
                room.load_user_receipt(receipt_type, thread, &own).await
            {
                targets.insert(event_id.to_string());
            }
        }
        if let Ok(mut memo) = self.receipt_targets_memo.lock() {
            memo.insert(room_id, (targets.clone(), std::time::Instant::now()));
        }
        targets
    }

    /// Drop the memoized receipt targets for a room so the next read reloads
    /// them (call right after our own read receipt changes).
    pub(crate) fn invalidate_receipt_targets(&self, room_id: &str) {
        if let Ok(mut memo) = self.receipt_targets_memo.lock() {
            memo.remove(room_id);
        }
    }

    /// The effective read marker in `items`: several receipt types may resolve to
    /// different events; the one sitting latest in the loaded window wins.
    fn marker_in_items(
        targets: &std::collections::HashSet<String>,
        items: &[TimelineItem],
    ) -> Option<String> {
        if targets.is_empty() {
            return None;
        }
        items
            .iter()
            .rev()
            .find(|item| targets.contains(&item.event_id))
            .map(|item| item.event_id.clone())
    }

    pub(crate) async fn active_timeline_or_legacy(
        &self,
        room_id: &str,
    ) -> Option<Arc<SdkTimeline>> {
        {
            let wins = self.windows.read().await;
            if let Some(window) = wins.get(room_id) {
                return Some(window.active_timeline().clone());
            }
        }
        {
            let timelines = self.timelines.read().await;
            if let Some(timeline) = timelines.get(room_id) {
                return Some(timeline.clone());
            }
        }
        None
    }

    pub(crate) async fn active_window_timeline(&self, room_id: &str) -> Option<Arc<SdkTimeline>> {
        let wins = self.windows.read().await;
        wins.get(room_id)
            .map(|window| window.active_timeline().clone())
    }

    pub(crate) async fn create_missing_window_timeline(
        &self,
        room: &Room,
        room_id: &str,
        runtime: TimelineRuntime,
        make_on_changed: TimelineChangedFactory,
    ) -> Result<Arc<SdkTimeline>> {
        // Double-check no race created it while the caller awaited room lookup.
        if let Some(timeline) = self.active_window_timeline(room_id).await {
            return Ok(timeline);
        }

        TimelineWindowService::create_window_for_room(room, room_id, runtime, make_on_changed)
            .await?;

        self.active_window_timeline(room_id)
            .await
            .ok_or_else(|| anyhow!("Timeline window was not created for room {room_id}"))
    }

    pub(crate) async fn take_timeline_update_snapshot(&self, room_id: &str) -> TimelineSlice {
        let mut update = {
            let mut updates = self.update_cache.write().await;
            updates.remove(room_id)
        }
        .unwrap_or_else(TimelineSlice::empty_live);

        if update.items.is_empty() && update.update_kind == crate::types::TimelineUpdateKind::Full {
            update.items = {
                let cache = self.cache.read().await;
                cache.get(room_id).cloned().unwrap_or_default()
            };
        }

        // The read marker / first-unread is now resolved against the cache guard
        // directly in finish_timeline_update, so we no longer clone the whole
        // window here just to hand it back.
        update
    }

    pub(crate) async fn take_timeline_slice_snapshot(&self, room_id: &str) -> Vec<TimelineItem> {
        {
            let mut updates = self.update_cache.write().await;
            updates.remove(room_id);
        }

        let cache = self.cache.read().await;
        cache.get(room_id).cloned().unwrap_or_default()
    }

    pub(crate) async fn finish_timeline_slice(
        &self,
        room_id: &str,
        items: Vec<TimelineItem>,
        room: Option<Room>,
    ) -> TimelineSlice {
        let unread_state_known = room.is_some();
        let pinned_event_ids = room
            .as_ref()
            .and_then(|r| r.pinned_event_ids())
            .map(|ids| ids.into_iter().map(|id| id.to_string()).collect())
            .unwrap_or_default();
        let unread_count = room.as_ref().map(room_unread_count).unwrap_or(0);
        let (first_unread_event_id, read_marker_loaded) = match room.as_ref() {
            Some(r) => {
                let targets = self.own_receipt_targets(r).await;
                let read_marker = Self::marker_in_items(&targets, &items);
                let loaded = read_marker.is_some();
                (
                    Self::first_unread_event_id(&items, read_marker.as_deref(), unread_count),
                    loaded,
                )
            }
            None => (None, false),
        };

        let windows = self.windows.read().await;
        if let Some(window) = windows.get(room_id) {
            TimelineSlice {
                items,
                update_kind: crate::types::TimelineUpdateKind::Full,
                update_index: 0,
                can_paginate_back: window.can_paginate_back(),
                can_paginate_forward: window.can_paginate_forward(),
                hit_timeline_start: window.hit_timeline_start(),
                is_live: window.is_live(),
                focus_event_id: window.focus_event_id().map(String::from),
                pinned_event_ids,
                first_unread_event_id,
                read_marker_loaded,
                unread_count,
                unread_state_known,
            }
        } else {
            TimelineSlice {
                items,
                update_kind: crate::types::TimelineUpdateKind::Full,
                update_index: 0,
                pinned_event_ids,
                first_unread_event_id,
                read_marker_loaded,
                unread_count,
                unread_state_known,
                ..TimelineSlice::empty_live()
            }
        }
    }

    pub(crate) async fn finish_timeline_update(
        &self,
        room_id: &str,
        mut update: TimelineSlice,
        room: Option<Room>,
    ) -> TimelineSlice {
        update.unread_state_known = room.is_some();
        update.pinned_event_ids = room
            .as_ref()
            .and_then(|r| r.pinned_event_ids())
            .map(|ids| ids.into_iter().map(|id| id.to_string()).collect())
            .unwrap_or_default();
        update.unread_count = room.as_ref().map(room_unread_count).unwrap_or(0);
        let (first_unread, read_marker_loaded) = match room.as_ref() {
            Some(r) => {
                // Resolve receipt targets async (no lock held), then compute the
                // marker + first-unread against the cache read guard directly —
                // no full-window clone.
                let targets = self.own_receipt_targets(r).await;
                let cache = self.cache.read().await;
                let items = cache.get(room_id).map(Vec::as_slice).unwrap_or(&[]);
                let read_marker = Self::marker_in_items(&targets, items);
                let loaded = read_marker.is_some();
                (
                    Self::first_unread_event_id(items, read_marker.as_deref(), update.unread_count),
                    loaded,
                )
            }
            None => (None, false),
        };
        update.first_unread_event_id = first_unread;
        update.read_marker_loaded = read_marker_loaded;

        let windows = self.windows.read().await;
        if let Some(window) = windows.get(room_id) {
            update.can_paginate_back = window.can_paginate_back();
            update.can_paginate_forward = window.can_paginate_forward();
            update.hit_timeline_start = window.hit_timeline_start();
            update.is_live = window.is_live();
            update.focus_event_id = window.focus_event_id().map(String::from);
        }
        update
    }
}

impl Default for TimelineService {
    fn default() -> Self {
        Self::new()
    }
}

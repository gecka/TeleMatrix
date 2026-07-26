// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::collections::HashMap;
use std::sync::{Arc, Mutex};

use anyhow::Result;
use matrix_sdk::ruma::events::room::MediaSource;
use matrix_sdk::ruma::OwnedRoomId;
use matrix_sdk::{Client, Room};
use matrix_sdk_ui::timeline::Timeline as SdkTimeline;
use tokio::sync::RwLock;
use tracing::warn;

use crate::presence_typing_service::PresenceTypingService;
use crate::session_task_service::SessionTaskService;
use crate::timeline_service::TimelineRefreshState;
use crate::timeline_window::{OnTimelineChanged, TimelineWindow};
use crate::types::{ReplyPreview, RoomSummary, TimelineItem, TimelineSlice, UrlPreview};

type Timelines = Arc<RwLock<HashMap<String, Arc<SdkTimeline>>>>;
type Windows = Arc<RwLock<HashMap<String, TimelineWindow>>>;
type TimelineCache = Arc<RwLock<HashMap<String, Vec<TimelineItem>>>>;
type UpdateCache = Arc<RwLock<HashMap<String, TimelineSlice>>>;
type RefreshStates = Arc<Mutex<HashMap<String, Arc<TimelineRefreshState>>>>;
type ReplyPreviewCache = Arc<RwLock<HashMap<(String, String), ReplyPreview>>>;
type PendingReactionOverrides = Arc<RwLock<HashMap<(String, String, String), bool>>>;
type MediaSources = Arc<std::sync::RwLock<HashMap<String, MediaSource>>>;
type TimelineCallbacks = Arc<Mutex<HashMap<String, Box<dyn Fn() + Send>>>>;
type RecentRooms = Arc<Mutex<std::collections::VecDeque<String>>>;
type CreatingWindows = Arc<Mutex<std::collections::HashSet<String>>>;
type RoomListCallback = Arc<Mutex<Option<Box<dyn Fn() + Send>>>>;
type RoomsCache = Arc<RwLock<Vec<RoomSummary>>>;
type UrlPreviewCache = Arc<RwLock<HashMap<String, Option<UrlPreview>>>>;
type PreviewInflight = Arc<RwLock<std::collections::HashSet<String>>>;
type PreviewStore = Arc<std::sync::Mutex<Option<crate::preview_store::PreviewStore>>>;
type NotificationOverrides = Arc<RwLock<HashMap<String, crate::types::RoomNotificationMode>>>;
type SenderAvatarCache = Arc<RwLock<HashMap<String, HashMap<String, Option<String>>>>>;
type SearchIndex = Arc<std::sync::Mutex<Option<crate::search_index::SearchIndex>>>;
type SearchIndexFingerprints = Arc<RwLock<HashMap<(String, String), u64>>>;

#[derive(Clone)]
pub(crate) struct TimelineRuntime {
    pub(crate) timelines: Timelines,
    pub(crate) windows: Windows,
    pub(crate) cache: TimelineCache,
    pub(crate) update_cache: UpdateCache,
    pub(crate) refresh_states: RefreshStates,
    pub(crate) reply_preview_cache: ReplyPreviewCache,
    pub(crate) pending_reaction_overrides: PendingReactionOverrides,
    pub(crate) media_sources: MediaSources,
    pub(crate) pending_forward_meta: crate::timeline_cache_service::PendingForwardMeta,
    pub(crate) callbacks: TimelineCallbacks,
    pub(crate) room_list_callback: RoomListCallback,
    pub(crate) notification_callback: crate::notification_service::NotificationCallbackSlot,
    pub(crate) invite_notification_callback:
        crate::notification_service::InviteNotificationCallbackSlot,
    pub(crate) preview_fetch_callback: crate::preview_fetch_signal::PreviewFetchCallbackSlot,
    pub(crate) sync_state: Arc<std::sync::atomic::AtomicU32>,
    pub(crate) rooms_cache: RoomsCache,
    pub(crate) preview_cache: UrlPreviewCache,
    pub(crate) preview_inflight: PreviewInflight,
    pub(crate) preview_store: PreviewStore,
    pub(crate) notification_overrides: NotificationOverrides,
    pub(crate) sender_avatar_cache: SenderAvatarCache,
    pub(crate) search_index: SearchIndex,
    pub(crate) search_index_fingerprints: SearchIndexFingerprints,
    pub(crate) presence_typing: PresenceTypingService,
    pub(crate) session_tasks: SessionTaskService,
    pub(crate) recent_rooms: RecentRooms,
    pub(crate) creating_windows: CreatingWindows,
}

/// Frees a room's window-build slot on every exit path — success, build error, or
/// lost race — so a build that fails is retried by the next poll instead of
/// wedging the room shut for the rest of the session.
struct WindowBuildSlot {
    creating: CreatingWindows,
    room_id: String,
}

impl WindowBuildSlot {
    /// `None` if another task is already building this room's window.
    fn claim(creating: &CreatingWindows, room_id: &str) -> Option<Self> {
        let mut guard = creating.lock().ok()?;
        if !guard.insert(room_id.to_string()) {
            return None;
        }
        drop(guard);
        Some(Self {
            creating: creating.clone(),
            room_id: room_id.to_string(),
        })
    }
}

impl Drop for WindowBuildSlot {
    fn drop(&mut self) {
        if let Ok(mut guard) = self.creating.lock() {
            guard.remove(&self.room_id);
        }
    }
}

pub(crate) type TimelineChangedFactory =
    Arc<dyn Fn(TimelineRuntime) -> OnTimelineChanged + Send + Sync>;

pub(crate) struct TimelineWindowService;

impl TimelineWindowService {
    pub(crate) async fn create_window_for_room(
        room: &Room,
        room_id: &str,
        runtime: TimelineRuntime,
        make_on_changed: TimelineChangedFactory,
    ) -> Result<()> {
        {
            let wins = runtime.windows.read().await;
            if wins.contains_key(room_id) {
                return Ok(());
            }
        }

        let on_changed = make_on_changed(runtime.clone());
        let window = TimelineWindow::new(room.clone(), room_id.to_string(), on_changed).await?;
        let live_timeline = window.live_timeline.clone();

        // Insert into windows BEFORE starting watcher, so the on_changed
        // callback can find the window in the map when it fires.
        {
            let mut wins = runtime.windows.write().await;
            if wins.contains_key(room_id) {
                return Ok(());
            }
            wins.insert(room_id.to_string(), window);
            if let Some(w) = wins.get_mut(room_id) {
                w.start_live_watcher();
            }
        }

        // Insert the live timeline into the legacy `timelines` map for
        // backward compatibility (get_or_create_timeline, send_message, etc.)
        {
            let mut tls = runtime.timelines.write().await;
            tls.insert(room_id.to_string(), live_timeline);
        }

        // A new resident window: mark it most-recent and release older ones so a
        // long multi-room session's window count stays bounded.
        Self::touch_recent(&runtime, room_id);
        Self::enforce_window_cap(&runtime).await;

        Ok(())
    }

    /// Drop a room's resident timeline state so a long multi-room session stops
    /// growing per room. Safe to call for rooms the user is no longer viewing:
    /// removing the window runs its Drop, which aborts the live watcher and
    /// pagination tasks. Leaves rooms-list state (rooms_cache) and cross-room
    /// caches (media_sources / preview_cache) untouched, so the
    /// dialogs list and notifications are unaffected. Re-opening the room goes
    /// back through get_or_create_timeline and rebuilds everything.
    pub(crate) async fn release_room(runtime: &TimelineRuntime, room_id: &str) {
        {
            let mut wins = runtime.windows.write().await;
            wins.remove(room_id); // Drop aborts watcher + pagination tasks
        }
        {
            let mut tls = runtime.timelines.write().await;
            tls.remove(room_id);
        }
        {
            let mut cache = runtime.cache.write().await;
            cache.remove(room_id);
        }
        {
            let mut updates = runtime.update_cache.write().await;
            updates.remove(room_id);
        }
        if let Ok(mut states) = runtime.refresh_states.lock() {
            states.remove(room_id);
        }
        {
            let mut avatars = runtime.sender_avatar_cache.write().await;
            avatars.remove(room_id);
        }
        {
            // (room, event)-keyed — prune this room's entries only.
            let mut previews = runtime.reply_preview_cache.write().await;
            previews.retain(|k, _| k.0 != room_id);
        }
        {
            let mut fps = runtime.search_index_fingerprints.write().await;
            fps.retain(|k, _| k.0 != room_id);
        }
        if let Ok(mut cbs) = runtime.callbacks.lock() {
            cbs.remove(room_id);
        }
        if let Ok(mut recent) = runtime.recent_rooms.lock() {
            recent.retain(|r| r != room_id);
        }
    }

    /// Number of resident timeline windows kept live. The current room plus a
    /// couple of previous ones so back-switching stays instant; older rooms are
    /// released (they rebuild on reopen).
    const MAX_RESIDENT_WINDOWS: usize = 3;

    /// Mark `room_id` as most-recently used for the resident-window LRU.
    pub(crate) fn touch_recent(runtime: &TimelineRuntime, room_id: &str) {
        if let Ok(mut recent) = runtime.recent_rooms.lock() {
            recent.retain(|r| r != room_id);
            recent.push_front(room_id.to_string());
        }
    }

    /// Release the least-recently-used live windows until at most
    /// MAX_RESIDENT_WINDOWS remain. Skips rooms whose window is focused (a
    /// jump/focused view is loading) so an in-flight jump is never dropped.
    async fn enforce_window_cap(runtime: &TimelineRuntime) {
        loop {
            let victim = {
                let wins = runtime.windows.read().await;
                if wins.len() <= Self::MAX_RESIDENT_WINDOWS {
                    break;
                }
                let recent = match runtime.recent_rooms.lock() {
                    Ok(r) => r,
                    Err(_) => break,
                };
                // Back of the deque = least recent. Evict the oldest room that
                // still has a live (non-focused) window.
                recent
                    .iter()
                    .rev()
                    .find(|r| wins.get(*r).is_some_and(|w| w.is_live()))
                    .cloned()
            };
            match victim {
                Some(room) => Self::release_room(runtime, &room).await,
                None => break, // nothing evictable (all focused / untracked)
            }
        }
    }

    pub(crate) async fn create_window_from_client(
        client: Client,
        room_id_owned: String,
        runtime: TimelineRuntime,
        make_on_changed: TimelineChangedFactory,
    ) {
        let room_id: OwnedRoomId = match room_id_owned.as_str().try_into() {
            Ok(id) => id,
            Err(_) => return,
        };
        let room = match client.get_room(&room_id) {
            Some(r) => r,
            None => return,
        };

        // Double-check no race created it while we awaited
        {
            let wins = runtime.windows.read().await;
            if wins.contains_key(room_id.as_str()) {
                return;
            }
        }

        // Claim the build slot for the WHOLE build. The check above only covers
        // windows that already exist, but `get_timeline_slice` answers a cold room
        // with an empty slice and the UI re-polls while the window builds — so every
        // poll used to pass that check and spawn another multi-second
        // TimelineWindow::new for the same room, all contending for one SQLite pool.
        // Held until this function returns, so concurrent callers back off instead.
        let Some(_slot) = WindowBuildSlot::claim(&runtime.creating_windows, room_id.as_str())
        else {
            return;
        };

        let on_changed = make_on_changed(runtime.clone());

        match TimelineWindow::new(room, room_id.to_string(), on_changed).await {
            Ok(window) => {
                // Insert live timeline into legacy map
                {
                    let mut tls = runtime.timelines.write().await;
                    if tls.contains_key(room_id.as_str()) {
                        return; // Another task already created it.
                    }
                    tls.insert(room_id.to_string(), window.live_timeline.clone());
                }
                // Insert before starting watcher so callback can find window
                {
                    let mut wins = runtime.windows.write().await;
                    wins.insert(room_id.to_string(), window);
                    if let Some(w) = wins.get_mut(room_id.as_str()) {
                        w.start_live_watcher();
                    }
                }
                // Record the open for the LRU, but DO NOT enforce the cap here.
                //
                // Evicting from this path made every cold-start open slow (tried and
                // reverted): it is the path the UI's re-polls take, so a build for one
                // room would release another room's window while the UI was still
                // polling it, which spawned a fresh multi-second build, which evicted
                // the next one — thrash, with only MAX_RESIDENT_WINDOWS live at a time.
                // release_room also drops the window's Timeline and hence its event-
                // cache subscriber, queueing an SDK auto-shrink into the middle of the
                // concurrent rebuilds (see backfill_blank_preview for how that panics).
                //
                // Eviction stays on create_window_for_room (send/jump), which is driven
                // by a deliberate user action rather than by polling.
                Self::touch_recent(&runtime, room_id.as_str());
            }
            Err(e) => {
                warn!("Background TimelineWindow creation failed for {room_id}: {e}");
            }
        }
    }

    pub(crate) fn spawn_create_window_from_client(
        runtime_handle: tokio::runtime::Handle,
        client: Client,
        room_id_owned: String,
        runtime: TimelineRuntime,
        make_on_changed: TimelineChangedFactory,
    ) {
        runtime_handle.spawn(async move {
            Self::create_window_from_client(client, room_id_owned, runtime, make_on_changed).await;
        });
    }

    pub(crate) async fn create_window_after_join(
        room: Room,
        room_id: String,
        runtime: TimelineRuntime,
        make_on_changed: TimelineChangedFactory,
    ) {
        {
            let wins = runtime.windows.read().await;
            if wins.contains_key(&room_id) {
                return;
            }
        }

        let on_changed = make_on_changed(runtime.clone());

        match TimelineWindow::new(room.clone(), room_id.clone(), on_changed).await {
            Ok(window) => {
                {
                    let mut tls = runtime.timelines.write().await;
                    tls.insert(room_id.clone(), window.live_timeline.clone());
                }
                {
                    let mut wins = runtime.windows.write().await;
                    wins.insert(room_id.clone(), window);
                    if let Some(w) = wins.get_mut(&room_id) {
                        w.start_live_watcher();
                    }
                }

                runtime.presence_typing.subscribe_room_typing_notifications(
                    &room,
                    room_id,
                    &runtime.session_tasks,
                );
            }
            Err(e) => {
                warn!("Failed to create timeline window after join: {e}");
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicU32;

    impl TimelineRuntime {
        // Minimal runtime with empty maps for map-mechanics tests (no client, so
        // no real windows — window creation is exercised by integration tests).
        fn for_test() -> Self {
            TimelineRuntime {
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
                room_list_callback: Arc::new(Mutex::new(None)),
                notification_callback: Arc::new(Mutex::new(None)),
                invite_notification_callback: Arc::new(Mutex::new(None)),
                preview_fetch_callback: Arc::new(Mutex::new(None)),
                sync_state: Arc::new(AtomicU32::new(0)),
                rooms_cache: Arc::new(RwLock::new(Vec::new())),
                preview_cache: Arc::new(RwLock::new(HashMap::new())),
                preview_inflight: Arc::new(RwLock::new(std::collections::HashSet::new())),
                preview_store: Arc::new(std::sync::Mutex::new(None)),
                notification_overrides: Arc::new(RwLock::new(HashMap::new())),
                sender_avatar_cache: Arc::new(RwLock::new(HashMap::new())),
                search_index: Arc::new(std::sync::Mutex::new(None)),
                search_index_fingerprints: Arc::new(RwLock::new(HashMap::new())),
                presence_typing: PresenceTypingService::default(),
                session_tasks: SessionTaskService::new(tokio::runtime::Handle::current()),
                recent_rooms: Arc::new(Mutex::new(std::collections::VecDeque::new())),
                creating_windows: Arc::new(Mutex::new(std::collections::HashSet::new())),
            }
        }
    }

    #[tokio::test]
    async fn release_room_prunes_only_target_room() {
        let rt = TimelineRuntime::for_test();
        {
            let mut cache = rt.cache.write().await;
            cache.insert("a".into(), Vec::new());
            cache.insert("b".into(), Vec::new());
        }
        {
            let mut avatars = rt.sender_avatar_cache.write().await;
            avatars.insert("a".into(), HashMap::new());
            avatars.insert("b".into(), HashMap::new());
        }
        {
            // (room, event)-keyed — exercises the retain-by-key.0 pruning.
            let mut fps = rt.search_index_fingerprints.write().await;
            fps.insert(("a".into(), "e1".into()), 1);
            fps.insert(("a".into(), "e2".into()), 2);
            fps.insert(("b".into(), "e1".into()), 3);
        }

        TimelineWindowService::release_room(&rt, "a").await;

        let cache = rt.cache.read().await;
        assert!(!cache.contains_key("a"));
        assert!(cache.contains_key("b"));
        let avatars = rt.sender_avatar_cache.read().await;
        assert!(!avatars.contains_key("a"));
        assert!(avatars.contains_key("b"));
        let fps = rt.search_index_fingerprints.read().await;
        assert!(!fps.keys().any(|k| k.0 == "a"));
        assert_eq!(fps.keys().filter(|k| k.0 == "b").count(), 1);
    }

    #[tokio::test]
    async fn release_room_prunes_recent_order() {
        let rt = TimelineRuntime::for_test();
        TimelineWindowService::touch_recent(&rt, "a");
        TimelineWindowService::touch_recent(&rt, "b");
        TimelineWindowService::release_room(&rt, "a").await;
        let recent = rt.recent_rooms.lock().unwrap();
        let order: Vec<&str> = recent.iter().map(String::as_str).collect();
        assert_eq!(order, ["b"]);
    }

    #[tokio::test]
    async fn touch_recent_orders_newest_first_and_dedups() {
        let rt = TimelineRuntime::for_test();
        for r in ["a", "b", "c", "d"] {
            TimelineWindowService::touch_recent(&rt, r);
        }
        {
            let recent = rt.recent_rooms.lock().unwrap();
            let order: Vec<&str> = recent.iter().map(String::as_str).collect();
            assert_eq!(order, ["d", "c", "b", "a"]); // newest first
        }
        // Re-touching an existing room moves it to the front without duplicating.
        TimelineWindowService::touch_recent(&rt, "b");
        let recent = rt.recent_rooms.lock().unwrap();
        let order: Vec<&str> = recent.iter().map(String::as_str).collect();
        assert_eq!(order, ["b", "d", "c", "a"]);
    }

    // A cold room's build slot is exclusive while held: the repeated
    // get_timeline_slice polls that a cold room provokes must not each start
    // their own multi-second window build.
    #[test]
    fn a_claimed_build_slot_turns_concurrent_claims_away() {
        let creating: CreatingWindows = Arc::new(Mutex::new(std::collections::HashSet::new()));
        let first = WindowBuildSlot::claim(&creating, "!room:x").expect("first claim wins");
        assert!(WindowBuildSlot::claim(&creating, "!room:x").is_none());
        // A different room is unaffected.
        assert!(WindowBuildSlot::claim(&creating, "!other:x").is_some());
        drop(first);
    }

    // The slot must free on EVERY exit path, or a build that fails once would
    // wedge the room shut for the rest of the session.
    #[test]
    fn dropping_a_build_slot_lets_the_next_poll_retry() {
        let creating: CreatingWindows = Arc::new(Mutex::new(std::collections::HashSet::new()));
        drop(WindowBuildSlot::claim(&creating, "!room:x").expect("first claim wins"));
        assert!(creating.lock().unwrap().is_empty());
        assert!(WindowBuildSlot::claim(&creating, "!room:x").is_some());
    }
}

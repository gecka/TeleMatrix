// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Sliding-sync backend (matrix-sdk-ui `SyncService` + `RoomListService`).
//!
//! This is the only sync backend (see `docs/sliding-sync-plan.md`).
//!
//! Phase 1 scope (this file): drive our existing `RoomListService` cache +
//! `room_list_callback` from the sliding-sync room list, and map the loading
//! state onto the `SYNC_STATE_*` ints the FFI/C++ already consume. It reuses
//! `RoomSummaryService::build_rooms_cache` over `client.rooms()` — which, under
//! sliding sync, holds exactly the *materialized* (windowed) rooms — so all the
//! existing preview/avatar/folder/unread logic is unchanged. The VectorDiff-
//! precise apply, subscribe-on-open, presence-drop and notification model are
//! later phases.

use std::collections::{HashMap, HashSet};
use std::sync::atomic::Ordering;
use std::sync::{Arc, Mutex};

use anyhow::{anyhow, Result};
use eyeball_im::VectorDiff;
use futures_util::{pin_mut, StreamExt};
use matrix_sdk::{Client, Room, RoomState};
use matrix_sdk_ui::room_list_service::filters::new_filter_non_left;
use matrix_sdk_ui::room_list_service::{RoomListItem, State as RoomListState};
use matrix_sdk_ui::sync_service::{State as SyncServiceState, SyncService};
use tracing::{debug, info, warn, Instrument as _};

/// Shared slot holding the running sliding-sync `SyncService`, so the room-open
/// path can subscribe rooms (via its `RoomListService`) and logout/session-change
/// can stop it gracefully. `None` between sessions.
pub(crate) type SlidingSyncHandle = Arc<Mutex<Option<Arc<SyncService>>>>;

use crate::matrix::{SYNC_STATE_STORE_ERROR, SYNC_STATE_SYNCED, SYNC_STATE_SYNCING};
use crate::room_summary_service::{RoomSummaryRefreshContext, RoomSummaryService};
use crate::session_task_service::{SessionTaskService, SessionTasks};
use crate::sync_loop_service::SyncLoopRuntime;

/// Initial room-list window for a desktop client. The list is recency-sorted, so
/// the most-active rooms (where new messages land) are always near the top of
/// this window; `add_one_page()` grows it on scroll (a later phase).
const SLIDING_PAGE_SIZE: usize = 200;

/// Per-room timeline limit carried by the room list (previews + the
/// notification mini-timeline). Small but > 1 so a short burst isn't collapsed.
const SLIDING_LIST_TIMELINE_LIMIT: u32 = 10;

pub(crate) struct SlidingSyncService;

impl SlidingSyncService {
    /// Start the sliding-sync backend for this session generation. It is the
    /// only sync backend; it drives the shared `SyncLoopRuntime` handles.
    pub(crate) fn start(
        client: Client,
        runtime: SyncLoopRuntime,
        session_generation: u64,
        sync_handle: SlidingSyncHandle,
        reconnect_notify: Arc<tokio::sync::Notify>,
    ) {
        let session_tasks = runtime.session_tasks.clone();
        let tasks_handle = session_tasks.tasks();
        // Every account syncs in the same process, so untagged lines from two of
        // them are indistinguishable. WARN level because span creation obeys the
        // env filter: an INFO span would vanish under `RUST_LOG=warn` and take
        // the tag off the WARN lines that still print. The SDK's own
        // "supervisor task" span is WARN for the same reason — and since
        // `SyncService::start()` is awaited inside this span, that supervisor
        // (and its room-list/encryption children) inherits the account too.
        let span = tracing::span!(
            tracing::Level::WARN,
            "account",
            user = client
                .user_id()
                .map(|id| id.to_string())
                .unwrap_or_default()
        );
        session_tasks.spawn(
            async move {
                if let Err(e) = Self::run(
                    client,
                    runtime,
                    session_generation,
                    tasks_handle,
                    sync_handle,
                    reconnect_notify,
                )
                .await
                {
                    warn!("[sliding] sync service exited with error: {e}");
                }
            }
            .instrument(span),
        );
    }

    async fn run(
        client: Client,
        runtime: SyncLoopRuntime,
        session_generation: u64,
        tasks_handle: Arc<Mutex<SessionTasks>>,
        sync_handle: SlidingSyncHandle,
        reconnect_notify: Arc<tokio::sync::Notify>,
    ) -> Result<()> {
        set_sync_state(&runtime, SYNC_STATE_SYNCING);

        let sync_service = Arc::new(
            SyncService::builder(client.clone())
                .with_room_list_timeline_limit(SLIDING_LIST_TIMELINE_LIMIT)
                // Detect network drops and auto-retry when back online (go to the
                // Offline state instead of a terminal Error on connection loss,
                // e.g. WiFi drop / sleep-resume / AddrNotAvailable).
                .with_offline_mode()
                .build()
                .await
                .map_err(|e| anyhow!("failed to build SyncService: {e}"))?,
        );
        sync_service.start().await;
        info!("[sliding] SyncService started");

        // Publish the running service so the room-open path can subscribe rooms
        // (via its RoomListService) and logout/session-change can stop it.
        if let Ok(mut guard) = sync_handle.lock() {
            *guard = Some(sync_service.clone());
        }

        let room_list = sync_service
            .room_list_service()
            .all_rooms()
            .await
            .map_err(|e| anyhow!("failed to obtain all_rooms list: {e}"))?;

        // The stream only yields once a filter is set, and emits a `Reset`
        // followed by live diffs. We don't inspect the diffs yet — any batch is
        // a "something changed" signal to rebuild from the materialized window.
        let (stream, controller) = room_list.entries_with_dynamic_adapters(SLIDING_PAGE_SIZE);
        controller.set_filter(Box::new(new_filter_non_left()));
        pin_mut!(stream);
        // SyncService health stream (Phase 2 state mapping). Owned `Subscriber`,
        // independent of the borrowed room-list stream, so they coexist in one
        // select! loop.
        let mut state_sub = sync_service.state();
        // The connection heartbeat. The room-list state machine stores a state
        // only after a sliding-sync response comes back, and it stores
        // unconditionally (`set`, not `set_if_not_eq`), so this yields once per
        // successful response — including responses that change no rooms, which
        // the diff stream below stays silent for.
        let mut room_list_state_sub = sync_service.room_list_service().state();

        let mut announced_synced = false;
        // Pending one-shot rebuild to reflect favourite/pinned account-data tags
        // (see the SYNCED block below for why).
        let mut tag_rebuild_at: Option<tokio::time::Instant> = None;
        // Per-room last-seen latest event id, for notification dedup. Seeded from
        // the initial (pre-SYNCED) batches so the startup backlog never notifies.
        let mut last_event_ids: HashMap<String, String> = HashMap::new();
        // Room ids we've already handled as invites (notified or pre-SYNCED
        // seeded). Cleared when a room leaves the invite state, so a re-invite
        // notifies again. See notification_service::invite_action.
        let mut notified_invites: HashSet<String> = HashSet::new();
        // Backoff for restarting the SyncService after its terminal Error state
        // (network drop, sleep-resume, etc.). Reset on a healthy room-list batch.
        let mut error_backoff = std::time::Duration::from_secs(2);
        loop {
            if !SessionTaskService::is_generation_current(&tasks_handle, session_generation) {
                info!("[sliding] session generation changed; stopping consumer");
                break;
            }
            // Copy of the deadline so the timer branch's future owns it (avoids
            // borrowing `tag_rebuild_at`, which the other branches mutate).
            let tag_deadline = tag_rebuild_at;
            tokio::select! {
                maybe_diffs = stream.next() => {
                    match maybe_diffs {
                        // Any batch is a "something changed" signal; rebuild from
                        // the materialized window. (VectorDiff-precise apply later.)
                        Some(diffs) => {
                            debug!("[sliding] room-list batch: {} diff(s)", diffs.len());
                            error_backoff = std::time::Duration::from_secs(2); // healthy sync
                            // A batch means the connection is healthy again — clear
                            // any "Waiting for network" regression (idempotent once
                            // already SYNCED).
                            set_sync_state(&runtime, SYNC_STATE_SYNCED);

                            Self::process_notifications(
                                &diffs,
                                &runtime,
                                &mut last_event_ids,
                                &mut notified_invites,
                            )
                            .await;
                            if diffs.iter().any(|d| matches!(d, VectorDiff::Reset { .. })) {
                                // Initial load / filter reset: full rebuild over
                                // the materialized window.
                                Self::rebuild_room_list(&client, &runtime).await;
                            } else {
                                // Incremental: refresh only the rooms this batch
                                // touched, instead of rebuilding the whole list.
                                let changed: HashSet<String> = Self::affected_rooms(&diffs)
                                    .iter()
                                    .map(|room| room.room_id().to_string())
                                    .collect();
                                Self::refresh_changed_rooms(&client, &runtime, &changed).await;
                            }

                            // Announce SYNCED once, after the first batch is applied
                            // via EITHER path. The C++ folder-loading bar under the
                            // search field is stopped only by SYNCED (handleSyncSynced);
                            // gating the announcement on the `Reset` branch alone left
                            // it spinning forever whenever the initial population
                            // arrived as incremental diffs. Mirrors the classic backend,
                            // which announces SYNCED after its first successful refresh
                            // regardless of shape.
                            if !announced_synced {
                                announced_synced = true;
                                info!("[sliding] initial room-list populated; announcing SYNCED");
                                set_sync_state(&runtime, SYNC_STATE_SYNCED);
                                // Pinned state comes from m.favourite tags, which
                                // sync via the account-data extension AFTER this
                                // first room-list batch and do NOT emit their own
                                // room-list diff. Schedule one full rebuild so pinned
                                // rooms appear on a fresh login instead of only after
                                // an app restart (which reads already-cached tags).
                                tag_rebuild_at = Some(
                                    tokio::time::Instant::now()
                                        + std::time::Duration::from_secs(4),
                                );
                            }
                        }
                        None => break,
                    }
                }
                maybe_state = state_sub.next() => {
                    if let Some(state) = maybe_state {
                        map_service_state(&runtime, &state);
                        match &state {
                            // NOTE: `Running` is deliberately NOT treated as
                            // "connected" here. The SDK publishes it from
                            // `start()` without sending anything, so every failed
                            // retry would report SYNCED — which hid the connecting
                            // pill, reset its backoff, and opened the notification
                            // gate on a dead connection. The room-list heartbeat
                            // arm below is the evidence-based signal.
                            //
                            // The Error state is terminal — the service stopped.
                            // Restart it (capped exponential backoff) so the app
                            // reconnects after a transient failure offline mode
                            // didn't absorb. Store/crypto errors stay terminal.
                            SyncServiceState::Error(e)
                                if !error_is_store_crypto(e.as_ref()) =>
                            {
                                let delay = error_backoff;
                                error_backoff = (error_backoff * 2)
                                    .min(std::time::Duration::from_secs(30));
                                warn!("[sliding] SyncService errored; restarting in {delay:?}");
                                tokio::select! {
                                    _ = tokio::time::sleep(delay) => {}
                                    _ = reconnect_notify.notified() => {
                                        info!("[sliding] reconnect requested; skipping backoff");
                                    }
                                }
                                if SessionTaskService::is_generation_current(
                                    &tasks_handle,
                                    session_generation,
                                ) {
                                    sync_service.start().await;
                                    info!("[sliding] SyncService restarted after error");
                                }
                            }
                            _ => {}
                        }
                    }
                }
                maybe_room_list_state = room_list_state_sub.next() => {
                    if let Some(state) = maybe_room_list_state {
                        // A response came back, so the connection really works —
                        // clear any "Waiting for network" regression. Gated on
                        // `announced_synced` so the initial population still owns
                        // the first SYNCED (the batch paths above).
                        if announced_synced && room_list_state_is_connected(&state) {
                            set_sync_state(&runtime, SYNC_STATE_SYNCED);
                        }
                    }
                }
                _ = reconnect_notify.notified() => {
                    // The C++ network monitor saw the interface return — re-arm the
                    // sync now instead of waiting for the long-poll to time out.
                    // start() is idempotent if the service is already running.
                    info!("[sliding] reconnect requested; re-arming sync");
                    sync_service.start().await;
                }
                _ = async move {
                    match tag_deadline {
                        Some(at) => tokio::time::sleep_until(at).await,
                        None => std::future::pending::<()>().await,
                    }
                }, if tag_deadline.is_some() => {
                    tag_rebuild_at = None;
                    info!("[sliding] post-SYNCED rebuild to reflect account-data (favourite/pinned) tags");
                    Self::rebuild_room_list(&client, &runtime).await;
                }
            }
        }

        // Normal exit (generation changed at the loop top): clear the handle and
        // stop the service. The logout/session-change teardown stops it
        // explicitly *before* aborting this task (see
        // `MatrixProtocol::stop_sliding_backend`), since an aborted task never
        // reaches this point; `SyncService::stop` is idempotent, so a later
        // double-stop here is a no-op.
        if let Ok(mut guard) = sync_handle.lock() {
            *guard = None;
        }
        drop(controller);
        sync_service.stop().await;
        Ok(())
    }

    async fn rebuild_room_list(client: &Client, runtime: &SyncLoopRuntime) {
        let overrides = runtime.notification_overrides.read().await.clone();
        let presence = runtime.presence_typing.presence_snapshot();

        match RoomSummaryService::build_rooms_cache(
            client,
            &runtime.timeline_cache,
            &overrides,
            &presence,
        )
        .await
        {
            Ok(mut summaries) => {
                // First rebuild after a (re)start: seed the merge prior from the persisted
                // snapshot. Without this, a room whose newest event is a system event (typical
                // of churny public rooms) rebuilds with a blank preview + epoch timestamp —
                // merge would have nothing to restore from, blanking the row and sorting it to
                // the bottom until reopened. Load the snapshot (a synchronous SQLCipher read)
                // BEFORE taking the write lock, so it doesn't block every rooms_cache reader
                // while it runs. See code-review-2026-07-19 PERF-7.
                let seed = if runtime.rooms_cache.read().await.is_empty() {
                    Some(runtime.local_cache.load_rooms_snapshot())
                } else {
                    None
                };
                {
                    let mut cache = runtime.rooms_cache.write().await;
                    // Re-check under the write lock: another rebuild may have seeded in between.
                    if cache.is_empty() {
                        if let Some(seed) = seed {
                            *cache = seed;
                        }
                    }
                    RoomSummaryService::merge_sticky_previews(&mut summaries, cache.as_slice());
                    *cache = summaries;
                }
                fire_room_list_callback(runtime);
            }
            Err(e) => warn!("[sliding] build_rooms_cache failed: {e}"),
        }
    }

    /// Incrementally refresh only the rooms a diff batch touched, reusing the
    /// classic path's by-ids refresh (find-and-replace + re-sort, adds new rooms,
    /// drops rooms that have actually left). Avoids rebuilding all summaries on
    /// every batch.
    async fn refresh_changed_rooms(
        client: &Client,
        runtime: &SyncLoopRuntime,
        changed: &HashSet<String>,
    ) {
        if changed.is_empty() {
            return;
        }
        let overrides = runtime.notification_overrides.read().await.clone();
        let presence = runtime.presence_typing.presence_snapshot();
        let context = RoomSummaryRefreshContext {
            timeline_cache: &runtime.timeline_cache,
            rooms_cache: &runtime.rooms_cache,
            notification_overrides: &overrides,
            presence_snapshot: &presence,
            local_cache: &runtime.local_cache,
        };
        match RoomSummaryService::refresh_rooms_cache_by_ids(client, changed, &context).await {
            Ok(()) => fire_room_list_callback(runtime),
            Err(e) => warn!("[sliding] incremental room refresh failed: {e}"),
        }
    }

    /// Drive background notifications from the room-list diff stream. A new
    /// message bumps its room up the recency-sorted list, surfacing here as a
    /// changed latest event id; we dedup per room and only emit once SYNCED (the
    /// pre-SYNCED batches just seed the dedup map so the startup backlog is
    /// silent). Invited rooms have no latest event, so they take a parallel
    /// path (`notified_invites` + [`crate::notification_service::invite_action`]).
    /// See [`crate::notification_service::notify_room_latest_event`] /
    /// [`crate::notification_service::notify_room_invite`].
    async fn process_notifications(
        diffs: &[VectorDiff<RoomListItem>],
        runtime: &SyncLoopRuntime,
        last_event_ids: &mut HashMap<String, String>,
        notified_invites: &mut HashSet<String>,
    ) {
        use crate::notification_service::InviteAction;
        let caught_up = runtime.sync_state.load(Ordering::SeqCst) == SYNC_STATE_SYNCED;
        for room in Self::affected_rooms(diffs) {
            let room_id = room.room_id().to_string();
            let is_invited = room.state() == RoomState::Invited;

            // Invite path: independent of the message dedup below.
            match crate::notification_service::invite_action(
                is_invited,
                caught_up,
                notified_invites.contains(&room_id),
            ) {
                InviteAction::Notify => {
                    notified_invites.insert(room_id.clone());
                    crate::notification_service::notify_room_invite(
                        &room,
                        &runtime.timeline_runtime.invite_notification_callback,
                    )
                    .await;
                }
                InviteAction::Record => {
                    notified_invites.insert(room_id.clone());
                }
                InviteAction::Forget => {
                    notified_invites.remove(&room_id);
                }
                InviteAction::Ignore => {}
            }

            // Message path: only non-invite rooms carry a notifiable latest event.
            if is_invited {
                continue;
            }
            let Some(new_eid) = room.latest_event().event_id().map(|id| id.to_string()) else {
                continue;
            };
            let prev = last_event_ids.insert(room_id, new_eid.clone());
            let changed = prev.as_deref() != Some(new_eid.as_str());
            if changed && caught_up {
                crate::notification_service::notify_room_latest_event(
                    &room,
                    &runtime.timeline_runtime.notification_callback,
                )
                .await;
            }
        }
    }

    /// Collect the rooms added or updated by a diff batch (the only diffs that can
    /// carry a new latest event); removals/truncations have nothing to notify.
    fn affected_rooms(diffs: &[VectorDiff<RoomListItem>]) -> Vec<Room> {
        let mut rooms = Vec::new();
        for diff in diffs {
            match diff {
                VectorDiff::Append { values } | VectorDiff::Reset { values } => {
                    rooms.extend(values.iter().map(|item| item.clone().into_inner()));
                }
                VectorDiff::PushFront { value }
                | VectorDiff::PushBack { value }
                | VectorDiff::Insert { value, .. }
                | VectorDiff::Set { value, .. } => {
                    rooms.push(value.clone().into_inner());
                }
                VectorDiff::Remove { .. }
                | VectorDiff::PopFront
                | VectorDiff::PopBack
                | VectorDiff::Truncate { .. }
                | VectorDiff::Clear => {}
            }
        }
        rooms
    }
}

/// Map the `SyncService` health state onto our `SYNC_STATE_*` ints. The room-list
/// consumer owns the SYNCING→SYNCED progression; here we escalate a store/crypto
/// error to the unrecoverable `STORE_ERROR` (drives the C++ store-error dialog),
/// and regress to `SYNCING` on a network problem (Offline / transient Error) so
/// the C++ "Waiting for network…" banner shows until a healthy batch recovers.
fn map_service_state(runtime: &SyncLoopRuntime, state: &SyncServiceState) {
    match state {
        SyncServiceState::Error(error) => {
            if error_is_store_crypto(error.as_ref()) {
                warn!("[sliding] unrecoverable store/crypto error from sync service: {error}");
                set_sync_state(runtime, SYNC_STATE_STORE_ERROR);
            } else {
                // The room-list consumer loop restarts the service on this terminal
                // Error state (with backoff). Regress to SYNCING so the C++
                // "Waiting for network…" banner shows until a batch recovers.
                warn!("[sliding] sync service error (will restart): {error}");
                set_sync_state(runtime, SYNC_STATE_SYNCING);
            }
        }
        // Network is down — show the "Waiting for network…" banner (regress to
        // SYNCING); a healthy room-list batch resets it to SYNCED on recovery.
        SyncServiceState::Offline => {
            info!("[sliding] sync service is offline");
            set_sync_state(runtime, SYNC_STATE_SYNCING);
        }
        SyncServiceState::Idle | SyncServiceState::Running | SyncServiceState::Terminated => {}
    }
}

/// Whether a room-list state means a sliding-sync response actually came back.
///
/// The state machine stores a state only after the sync call resolves: success
/// stores the next state, failure stores `Error`/`Terminated`. So anything but
/// those two is proof the connection works — the one signal here that cannot be
/// produced by merely re-arming the service.
///
/// Any success state counts, not just `Running`: recovering from an error goes
/// `Error{Running}` → `Recovering` → `Running`, so waiting for `Running` would
/// hold the notification gate shut for an extra round trip.
fn room_list_state_is_connected(state: &RoomListState) -> bool {
    !matches!(
        state,
        RoomListState::Error { .. } | RoomListState::Terminated { .. }
    )
}

/// Walk an error's source chain for the store-cipher marker (mirrors the classic
/// loop's `is_store_crypto_error`).
fn error_is_store_crypto(err: &dyn std::error::Error) -> bool {
    let mut current: Option<&dyn std::error::Error> = Some(err);
    while let Some(e) = current {
        if e.to_string()
            .contains("Error encrypting or decrypting a value")
        {
            return true;
        }
        current = e.source();
    }
    false
}

/// Fire the FFI room-list-changed callback (poison-tolerant, like the classic loop).
fn fire_room_list_callback(runtime: &SyncLoopRuntime) {
    let guard = runtime
        .room_list_callback
        .lock()
        .unwrap_or_else(|e| e.into_inner());
    if let Some(callback) = guard.as_ref() {
        callback();
    }
}

/// Transition the sync state and notify the FFI only on a real change.
fn set_sync_state(runtime: &SyncLoopRuntime, state: u32) {
    let was = runtime.sync_state.swap(state, Ordering::SeqCst);
    if was != state {
        info!("[sliding] sync state: {was} -> {state}");
        let guard = runtime
            .sync_state_callback
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        if let Some(callback) = guard.as_ref() {
            callback(state);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // The failure stores. These are the two the SDK writes when the sync call
    // did NOT come back, so they must never report a working connection.
    #[test]
    fn a_failed_room_list_state_is_not_connected() {
        assert!(!room_list_state_is_connected(&RoomListState::Error {
            from: Box::new(RoomListState::Running)
        }));
        assert!(!room_list_state_is_connected(&RoomListState::Terminated {
            from: Box::new(RoomListState::Running)
        }));
    }

    // Every other state is only ever stored after a successful response.
    // `Recovering` in particular must count: it is what the first response
    // after an outage stores, and the notification gate waits on this.
    #[test]
    fn every_success_room_list_state_is_connected() {
        for state in [
            RoomListState::Init,
            RoomListState::SettingUp,
            RoomListState::Recovering,
            RoomListState::Running,
        ] {
            assert!(
                room_list_state_is_connected(&state),
                "{state:?} is a success store and must report connected"
            );
        }
    }
}

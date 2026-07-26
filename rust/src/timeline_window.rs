// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use anyhow::{anyhow, Result};
use eyeball_im::VectorDiff;
use futures_util::StreamExt;
use matrix_sdk::event_cache::PaginationStatus;
use matrix_sdk::Room;
use matrix_sdk_ui::timeline::{
    RoomExt, Timeline as SdkTimeline, TimelineEventFocusThreadMode, TimelineFocus,
    TimelineItem as SdkTimelineItem,
};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;
use tokio::task::JoinHandle;
use tracing::warn;

const INITIAL_BACK_PAGINATION_LIMIT: u16 = 50;
const PAGINATION_TIMEOUT: Duration = Duration::from_secs(30);

/// Reduced timeline diffs that the protocol layer can apply without asking the
/// SDK for the full item vector. Virtual-item churn (date dividers, read
/// markers) is filtered out so back-pagination stays an incremental Prepend;
/// edits/redactions/reactions/receipts collapse to Full so the exact snapshot
/// path keeps handling them.
#[derive(Clone)]
pub enum TimelineDiff {
    Append(Vec<Arc<SdkTimelineItem>>),
    Prepend(Vec<Arc<SdkTimelineItem>>),
    PushBack(Arc<SdkTimelineItem>),
    PushFront(Arc<SdkTimelineItem>),
    /// In-place replacement(s) of existing event items with the SAME event id
    /// (edits, reactions, receipts, delivery state). The consumer converts each
    /// and compares to the cached converted item: identical → no-op (skips the
    /// whole-window reconvert); different → falls back to a Full snapshot.
    Changed(Vec<Arc<SdkTimelineItem>>),
    Full,
}

/// Callback type: fired when the active timeline's items change.
pub type OnTimelineChanged = Arc<dyn Fn(&str, Vec<TimelineDiff>) + Send + Sync>;

#[derive(Debug, PartialEq, Eq)]
enum EventChange {
    Unchanged,
    Prepend(usize),
    Append(usize),
    Other,
}

/// Classify how the event-key sequence changed from `before` to `after`.
/// Detects clean front growth (Prepend) or back growth (Append); anything else
/// (edits, removals, reorders, both-ends growth) is Other.
fn classify_event_change(before: &[String], after: &[String]) -> EventChange {
    if before == after {
        return EventChange::Unchanged;
    }
    if after.len() > before.len() {
        let k = after.len() - before.len();
        if after[k..] == *before {
            return EventChange::Prepend(k);
        }
        if after[..before.len()] == *before {
            return EventChange::Append(k);
        }
    }
    EventChange::Other
}

/// Result of reducing one SDK diff batch to an event-cache-level operation.
#[derive(Debug, PartialEq, Eq)]
enum ReducedOp<T> {
    None,
    Prepend(Vec<T>),
    Append(Vec<T>),
    /// Same-event in-place replacements (no key-sequence change). Carried so the
    /// consumer can convert+compare and skip the reconvert when nothing rendered
    /// actually changed (e.g. a read receipt).
    Changed(Vec<T>),
    Full,
}

/// Apply one VectorDiff batch to `mirror` (a faithful copy of the SDK item
/// vector) and classify its net effect on the *event* subsequence. Virtual
/// items (date dividers / read markers) are transparent here, so back-pagination
/// — which interleaves divider churn with event inserts — reduces to a clean
/// Prepend. Set/Remove of an *event* (edit/redaction/reaction/receipt) forces
/// Full so the exact snapshot path keeps handling those. On any index that
/// would be out of range the mirror is treated as desynced and we fall back to
/// Full (the snapshot re-seeds it).
fn reduce_batch<T, F>(mirror: &mut Vec<T>, diffs: Vec<VectorDiff<T>>, event_key: F) -> ReducedOp<T>
where
    T: Clone,
    F: Fn(&T) -> Option<String>,
{
    let before: Vec<String> = mirror.iter().filter_map(&event_key).collect();
    let mut forced_full = false;
    let mut changed: Vec<T> = Vec::new();
    for diff in diffs {
        match diff {
            VectorDiff::Append { values } => mirror.extend(values),
            VectorDiff::PushBack { value } => mirror.push(value),
            VectorDiff::PushFront { value } => mirror.insert(0, value),
            VectorDiff::Insert { index, value } => {
                if index <= mirror.len() {
                    mirror.insert(index, value);
                } else {
                    forced_full = true;
                }
            }
            VectorDiff::Set { index, value } => {
                if index < mirror.len() {
                    let old_key = event_key(&mirror[index]);
                    let new_key = event_key(&value);
                    match (&old_key, &new_key) {
                        // Same event replaced in place (edit/reaction/receipt/
                        // delivery): don't reconvert the whole window — carry it
                        // for a convert-and-compare in the consumer.
                        (Some(a), Some(b)) if a == b => changed.push(value.clone()),
                        // Both virtual (date divider / read marker) — transparent.
                        (None, None) => {}
                        // Key changed or an event appeared/vanished at this index:
                        // the event sequence really moved; re-snapshot.
                        _ => forced_full = true,
                    }
                    mirror[index] = value;
                } else {
                    forced_full = true;
                }
            }
            VectorDiff::Remove { index } => {
                if index < mirror.len() {
                    if event_key(&mirror[index]).is_some() {
                        forced_full = true;
                    }
                    mirror.remove(index);
                } else {
                    forced_full = true;
                }
            }
            VectorDiff::PopFront => {
                if !mirror.is_empty() {
                    if event_key(&mirror[0]).is_some() {
                        forced_full = true;
                    }
                    mirror.remove(0);
                }
            }
            VectorDiff::PopBack => {
                if let Some(last) = mirror.last() {
                    if event_key(last).is_some() {
                        forced_full = true;
                    }
                    mirror.pop();
                }
            }
            VectorDiff::Truncate { length } => {
                if length < mirror.len() {
                    if mirror[length..].iter().any(|i| event_key(i).is_some()) {
                        forced_full = true;
                    }
                    mirror.truncate(length);
                }
            }
            VectorDiff::Clear => {
                if mirror.iter().any(|i| event_key(i).is_some()) {
                    forced_full = true;
                }
                mirror.clear();
            }
            VectorDiff::Reset { values } => {
                forced_full = true;
                *mirror = values.into_iter().collect();
            }
        }
    }
    let after_items: Vec<T> = mirror
        .iter()
        .filter(|i| event_key(i).is_some())
        .cloned()
        .collect();
    if forced_full {
        return ReducedOp::Full;
    }
    let after: Vec<String> = after_items.iter().filter_map(&event_key).collect();
    match classify_event_change(&before, &after) {
        // Pure in-place replacement batch (key sequence unchanged): carry the
        // changed items; nothing changed at all → None.
        EventChange::Unchanged => {
            if changed.is_empty() {
                ReducedOp::None
            } else {
                ReducedOp::Changed(changed)
            }
        }
        // Structural growth mixed with an in-place change in the same batch is
        // rare; take the safe whole-snapshot path rather than emit both.
        EventChange::Prepend(k) => {
            if changed.is_empty() {
                ReducedOp::Prepend(after_items[..k].to_vec())
            } else {
                ReducedOp::Full
            }
        }
        EventChange::Append(k) => {
            if changed.is_empty() {
                ReducedOp::Append(after_items[after_items.len() - k..].to_vec())
            } else {
                ReducedOp::Full
            }
        }
        EventChange::Other => ReducedOp::Full,
    }
}

/// Shared pagination state — accessible by spawned tasks via Arc
#[derive(Default)]
pub struct PaginationState {
    pub live_hit_start: AtomicBool,
    pub focused_hit_start: AtomicBool,
    pub focused_hit_live_edge: AtomicBool,
    pub is_paginating_back: AtomicBool,
    pub is_paginating_forward: AtomicBool,
}

pub struct TimelineWindow {
    pub room_id: String,
    room: Room,
    pub live_timeline: Arc<SdkTimeline>,
    live_watcher_handle: Option<JoinHandle<()>>,
    live_pagination_status_handle: Option<JoinHandle<()>>,
    focused_timeline: Option<Arc<SdkTimeline>>,
    focused_watcher_handle: Option<JoinHandle<()>>,
    focus_target_event_id: Option<String>,
    is_live: AtomicBool,
    pub state: Arc<PaginationState>,
    on_changed: OnTimelineChanged,
}

impl TimelineWindow {
    fn update_live_hit_start_from_status(state: &PaginationState, status: PaginationStatus) {
        if let PaginationStatus::Idle {
            hit_timeline_start: true,
        } = status
        {
            state.live_hit_start.store(true, Ordering::Release);
        }
    }

    /// Stable per-item key for sequence comparison: event id, else txn id.
    fn event_key(item: &Arc<SdkTimelineItem>) -> Option<String> {
        item.as_event().map(|event| {
            event
                .event_id()
                .map(|id| id.to_string())
                .or_else(|| event.transaction_id().map(|txid| txid.to_string()))
                .unwrap_or_default()
        })
    }

    /// Append one reduced batch op to the pending diff list.
    fn push_reduced(pending: &mut Vec<TimelineDiff>, op: ReducedOp<Arc<SdkTimelineItem>>) {
        match op {
            ReducedOp::None => {}
            ReducedOp::Prepend(values) => pending.push(TimelineDiff::Prepend(values)),
            ReducedOp::Append(values) => pending.push(TimelineDiff::Append(values)),
            ReducedOp::Changed(values) => pending.push(TimelineDiff::Changed(values)),
            ReducedOp::Full => pending.push(TimelineDiff::Full),
        }
    }

    async fn watch_timeline_changes(
        timeline: Arc<SdkTimeline>,
        room_id: String,
        on_changed: OnTimelineChanged,
        focused: bool,
    ) {
        const CHANGE_DEBOUNCE: Duration = Duration::from_millis(40);
        const RESUBSCRIBE_DELAY: Duration = Duration::from_millis(250);

        'resubscribe: loop {
            let (initial, mut stream) = timeline.subscribe().await;
            // Keep a faithful copy of the SDK item vector so each diff batch can
            // be reduced to an event-cache-level op (Prepend/Append/Full) without
            // re-snapshotting; virtual-item churn becomes transparent. Re-seeded
            // on every (re)subscribe.
            let mut mirror: Vec<Arc<SdkTimelineItem>> = initial.into_iter().collect();
            // Render whatever is already in the local (persisted) event cache
            // immediately, instead of discarding it and waiting for the network
            // pagination below. `Full` re-snapshots from `timeline.items()`,
            // which the SDK has already loaded from matrix-sdk-event-cache.sqlite3
            // — this is what makes the timeline appear instantly at cold start.
            on_changed(&room_id, vec![TimelineDiff::Full]);
            while let Some(diffs) = stream.next().await {
                let mut pending = Vec::new();
                Self::push_reduced(
                    &mut pending,
                    reduce_batch(&mut mirror, diffs, Self::event_key),
                );
                let delay = tokio::time::sleep(CHANGE_DEBOUNCE);
                tokio::pin!(delay);
                loop {
                    tokio::select! {
                        _ = &mut delay => {
                            break;
                        }
                        maybe_diff = stream.next() => {
                            if let Some(diffs) = maybe_diff {
                                Self::push_reduced(&mut pending, reduce_batch(&mut mirror, diffs, Self::event_key));
                            } else {
                                on_changed(&room_id, vec![TimelineDiff::Full]);
                                if focused {
                                    warn!("Focused timeline stream ended for {room_id}, re-subscribing");
                                } else {
                                    warn!("Timeline stream ended for {room_id}, re-subscribing");
                                }
                                tokio::time::sleep(RESUBSCRIBE_DELAY).await;
                                continue 'resubscribe;
                            }
                        }
                    }
                }
                if pending.is_empty() {
                    pending.push(TimelineDiff::Full);
                }
                on_changed(&room_id, pending);
            }
            if focused {
                warn!("Focused timeline stream ended for {room_id}, re-subscribing");
            } else {
                warn!("Timeline stream ended for {room_id}, re-subscribing");
            }
            tokio::time::sleep(RESUBSCRIBE_DELAY).await;
        }
    }

    pub async fn new(room: Room, room_id: String, on_changed: OnTimelineChanged) -> Result<Self> {
        let build_t0 = std::time::Instant::now();

        // Split room.timeline()'s cost across the steps we can reach, so a slow build is
        // attributed instead of guessed at. Each of these is called by the builder
        // anyway (builder.rs:164-172) and is idempotent/cached, so doing it here first
        // makes the builder's own call short-circuit: this divides the same work rather
        // than adding any. NB: do NOT probe room_event_cache.subscribe() — creating and
        // dropping a subscriber queues an auto-shrink that panics the SDK's observers
        // (see backfill_blank_preview).
        let state_before = room.encryption_state();

        // Scheduler latency: how long a no-op yield takes to be re-polled. Large means
        // the runtime is saturated, and then EVERY await in the build pays it — which
        // would explain a multi-second build of a handful of items.
        let sched_t0 = std::time::Instant::now();
        tokio::task::yield_now().await;
        let sched_us = sched_t0.elapsed().as_micros();

        // Event-cache init: loads the room's last chunk out of the SQLite event-cache
        // store, so it pays for any contention on that store's connection pool.
        let ec_t0 = std::time::Instant::now();
        let _ = room.event_cache().await;
        let ec_ms = ec_t0.elapsed().as_millis();

        let enc_t0 = std::time::Instant::now();
        let _ = room.latest_encryption_state().await;
        let enc_ms = enc_t0.elapsed().as_millis();

        let rest_t0 = std::time::Instant::now();
        let live_timeline = Arc::new(room.timeline().await?);
        let rest_ms = rest_t0.elapsed().as_millis();
        let build_ms = build_t0.elapsed().as_millis();
        if build_ms > 60 {
            let items = live_timeline.items().await.len();
            tracing::info!(
                room = %room_id,
                build_ms,
                sched_us,
                ec_ms,
                enc_ms,
                rest_ms,
                items,
                alive_tasks = tokio::runtime::Handle::current().metrics().num_alive_tasks(),
                backfills_inflight = crate::matrix::BACKFILLS_INFLIGHT
                    .load(std::sync::atomic::Ordering::Relaxed),
                state_before = ?state_before,
                "TimelineWindow::new: room.timeline() build cost"
            );
        }

        let window = Self {
            room_id,
            room,
            live_timeline,
            live_watcher_handle: None,
            live_pagination_status_handle: None,
            focused_timeline: None,
            focused_watcher_handle: None,
            focus_target_event_id: None,
            is_live: AtomicBool::new(true),
            state: Arc::new(PaginationState::default()),
            on_changed,
        };

        Ok(window)
    }

    /// Start the live timeline watcher tasks.
    /// Must be called after construction.
    pub fn start_live_watcher(&mut self) {
        if let Some(handle) = self.live_watcher_handle.take() {
            handle.abort();
        }
        if let Some(handle) = self.live_pagination_status_handle.take() {
            handle.abort();
        }

        let status_timeline = self.live_timeline.clone();
        let status_state = self.state.clone();
        let status_handle = tokio::spawn(async move {
            if let Some((status, mut status_stream)) =
                status_timeline.live_back_pagination_status().await
            {
                Self::update_live_hit_start_from_status(&status_state, status);
                while let Some(status) = status_stream.next().await {
                    Self::update_live_hit_start_from_status(&status_state, status);
                }
            }
        });
        self.live_pagination_status_handle = Some(status_handle);

        let timeline = self.live_timeline.clone();
        let room_id = self.room_id.clone();
        let on_changed = self.on_changed.clone();
        let state = self.state.clone();

        let handle = tokio::spawn(async move {
            // Fill older history in the background. This runs CONCURRENTLY with
            // the live watcher (not before it) so the first render comes from the
            // local event cache instantly, instead of waiting on this network
            // round-trip. Both futures live under this single task handle, so
            // logout's abort tears down the pagination too (it holds Client/store
            // refs via the timeline).
            let paginate = {
                let timeline = timeline.clone();
                let room_id = room_id.clone();
                let on_changed = on_changed.clone();
                async move {
                    state.is_paginating_back.store(true, Ordering::Relaxed);
                    let pagination_started = std::time::Instant::now();
                    match tokio::time::timeout(
                        PAGINATION_TIMEOUT,
                        timeline.paginate_backwards(INITIAL_BACK_PAGINATION_LIMIT),
                    )
                    .await
                    {
                        Ok(Ok(_)) => {}
                        Ok(Err(e)) => warn!(
                            "Initial paginate_backwards failed for {room_id} after {:?}: {e}",
                            pagination_started.elapsed()
                        ),
                        Err(_) => warn!(
                            "Initial paginate_backwards timed out for {room_id} after {:?}",
                            pagination_started.elapsed()
                        ),
                    }
                    state.is_paginating_back.store(false, Ordering::Relaxed);
                    on_changed(&room_id, vec![TimelineDiff::Full]);
                }
            };

            tokio::join!(
                paginate,
                Self::watch_timeline_changes(timeline, room_id, on_changed, false),
            );
        });

        self.live_watcher_handle = Some(handle);
    }

    pub fn paginate_back(&self, count: u16) {
        if self.state.is_paginating_back.swap(true, Ordering::Relaxed) {
            return; // already paginating
        }

        let timeline = if self.is_live.load(Ordering::Relaxed) {
            self.live_timeline.clone()
        } else {
            match &self.focused_timeline {
                Some(t) => t.clone(),
                None => {
                    self.state
                        .is_paginating_back
                        .store(false, Ordering::Relaxed);
                    return;
                }
            }
        };

        let is_live = self.is_live.load(Ordering::Relaxed);
        let state = self.state.clone();
        let room_id = self.room_id.clone();

        tokio::spawn(async move {
            let page_t0 = std::time::Instant::now();
            let mut reached_start = false;
            match tokio::time::timeout(PAGINATION_TIMEOUT, timeline.paginate_backwards(count)).await
            {
                Ok(Ok(hit_start)) => {
                    reached_start = hit_start;
                    if hit_start {
                        if is_live {
                            state.live_hit_start.store(true, Ordering::Relaxed);
                        } else {
                            state.focused_hit_start.store(true, Ordering::Relaxed);
                        }
                    }
                }
                Ok(Err(e)) => {
                    warn!("timeline pagination failed for {room_id}: {e}");
                }
                Err(_) => {
                    warn!("timeline pagination timed out for {room_id}");
                }
            }
            let page_ms = page_t0.elapsed().as_millis();

            // Scheduler latency, same probe as TimelineWindow::new — but that
            // one only fires on room open, so it never observes the runtime
            // under a sustained back-scroll, which is exactly when the SQLite
            // pools and store decryption are hammering the blocking pool. A
            // large value here means every await in the app is paying it.
            let sched_t0 = std::time::Instant::now();
            tokio::task::yield_now().await;
            let sched_us = sched_t0.elapsed().as_micros();

            tracing::info!(
                room = %room_id,
                page_ms,
                sched_us,
                count,
                reached_start,
                alive_tasks = tokio::runtime::Handle::current().metrics().num_alive_tasks(),
                "paginate_back: page landed"
            );
            state.is_paginating_back.store(false, Ordering::Relaxed);
            // Don't call on_changed here — the live watcher subscription
            // already fires when the SDK adds items to the timeline.
            // Calling it again causes double delivery which can disrupt
            // scroll anchoring during backward pagination.
        });
    }

    pub fn paginate_forward(&self, count: u16) {
        if self.is_live.load(Ordering::Relaxed) {
            return;
        } // only in Event mode
        if self
            .state
            .is_paginating_forward
            .swap(true, Ordering::Relaxed)
        {
            return;
        }

        let timeline = match &self.focused_timeline {
            Some(t) => t.clone(),
            None => {
                self.state
                    .is_paginating_forward
                    .store(false, Ordering::Relaxed);
                return;
            }
        };

        let state = self.state.clone();
        let room_id = self.room_id.clone();

        tokio::spawn(async move {
            match tokio::time::timeout(PAGINATION_TIMEOUT, timeline.paginate_forwards(count)).await
            {
                Ok(Ok(hit_live_edge)) => {
                    if hit_live_edge {
                        state.focused_hit_live_edge.store(true, Ordering::Relaxed);
                    }
                }
                Ok(Err(e)) => {
                    tracing::error!("paginate_forwards failed for {room_id}: {e}");
                }
                Err(_) => {
                    tracing::error!("paginate_forwards timed out for {room_id}");
                }
            }
            state.is_paginating_forward.store(false, Ordering::Relaxed);
            // Same as paginate_back: live/focused watcher handles notification.
        });
    }

    pub fn is_live(&self) -> bool {
        self.is_live.load(Ordering::Relaxed)
    }

    pub fn can_paginate_back(&self) -> bool {
        if self.is_live() {
            !self.state.live_hit_start.load(Ordering::Relaxed)
        } else {
            !self.state.focused_hit_start.load(Ordering::Relaxed)
        }
    }

    pub fn can_paginate_forward(&self) -> bool {
        if self.is_live() {
            false
        } else {
            !self.state.focused_hit_live_edge.load(Ordering::Relaxed)
        }
    }

    pub fn hit_timeline_start(&self) -> bool {
        if self.is_live() {
            self.state.live_hit_start.load(Ordering::Relaxed)
        } else {
            self.state.focused_hit_start.load(Ordering::Relaxed)
        }
    }

    pub fn focus_event_id(&self) -> Option<&str> {
        self.focus_target_event_id.as_deref()
    }

    pub fn active_timeline(&self) -> &Arc<SdkTimeline> {
        if self.is_live() {
            &self.live_timeline
        } else {
            self.focused_timeline
                .as_ref()
                .unwrap_or(&self.live_timeline)
        }
    }

    pub fn should_auto_return_to_live(&self) -> bool {
        !self.is_live() && self.state.focused_hit_live_edge.load(Ordering::Relaxed)
    }

    pub async fn build_focused_timeline(
        room: &Room,
        event_id: &str,
    ) -> Result<(String, Arc<SdkTimeline>)> {
        let target: matrix_sdk::ruma::OwnedEventId = event_id
            .parse()
            .map_err(|e| anyhow::anyhow!("Invalid event ID: {e}"))?;
        let target_id = target.to_string();

        // Create focused timeline via /context/{event_id}
        let focused = Arc::new(
            room.timeline_builder()
                .with_focus(TimelineFocus::Event {
                    target,
                    num_context_events: 20,
                    thread_mode: TimelineEventFocusThreadMode::Automatic {
                        hide_threaded_events: false,
                    },
                })
                .build()
                .await?,
        );

        let contains_target = focused.items().await.iter().any(|item| {
            item.as_event()
                .and_then(|event| event.event_id())
                .is_some_and(|id| id.as_str() == target_id.as_str())
        });
        if !contains_target {
            return Err(anyhow!(
                "Focused timeline for event {target_id} did not contain the target event"
            ));
        }

        Ok((target_id, focused))
    }

    pub async fn focus_on_event(&mut self, event_id: String) -> Result<()> {
        let (target_id, focused) = Self::build_focused_timeline(&self.room, &event_id).await?;
        self.apply_focused_timeline(target_id, focused);
        Ok(())
    }

    pub fn apply_focused_timeline(&mut self, event_id: String, focused: Arc<SdkTimeline>) {
        // Abort previous focused watcher if any
        if let Some(handle) = self.focused_watcher_handle.take() {
            handle.abort();
        }

        // Reset focused pagination state
        self.state.focused_hit_start.store(false, Ordering::Relaxed);
        self.state
            .focused_hit_live_edge
            .store(false, Ordering::Relaxed);

        // Start focused watcher (re-subscription loop)
        let timeline = focused.clone();
        let on_changed = self.on_changed.clone();
        let rid = self.room_id.clone();

        let handle = tokio::spawn(async move {
            Self::watch_timeline_changes(timeline, rid, on_changed, true).await;
        });

        // Atomically switch AFTER focused timeline is ready
        self.focused_timeline = Some(focused);
        self.focused_watcher_handle = Some(handle);
        self.focus_target_event_id = Some(event_id);
        self.is_live.store(false, Ordering::Relaxed);

        // Fire initial notification
        (self.on_changed)(&self.room_id, vec![TimelineDiff::Full]);
    }

    pub fn return_to_live(&mut self) {
        if let Some(handle) = self.focused_watcher_handle.take() {
            handle.abort();
        }
        self.focused_timeline = None;
        self.focus_target_event_id = None;
        self.state.focused_hit_start.store(false, Ordering::Relaxed);
        self.state
            .focused_hit_live_edge
            .store(false, Ordering::Relaxed);
        self.is_live.store(true, Ordering::Relaxed);

        // Fire notification — MatrixProtocol will snapshot the live timeline
        (self.on_changed)(&self.room_id, vec![TimelineDiff::Full]);
    }
}

impl Drop for TimelineWindow {
    fn drop(&mut self) {
        if let Some(h) = self.live_watcher_handle.take() {
            h.abort();
        }
        if let Some(h) = self.live_pagination_status_handle.take() {
            h.abort();
        }
        if let Some(h) = self.focused_watcher_handle.take() {
            h.abort();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{classify_event_change, reduce_batch, EventChange, ReducedOp};
    use eyeball_im::{Vector, VectorDiff};

    fn keys(list: &[&str]) -> Vec<String> {
        list.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn classify_equal_is_unchanged() {
        assert_eq!(
            classify_event_change(&keys(&["a", "b"]), &keys(&["a", "b"])),
            EventChange::Unchanged
        );
    }

    #[test]
    fn classify_front_growth_is_prepend() {
        assert_eq!(
            classify_event_change(&keys(&["c", "d"]), &keys(&["a", "b", "c", "d"])),
            EventChange::Prepend(2)
        );
    }

    #[test]
    fn classify_back_growth_is_append() {
        assert_eq!(
            classify_event_change(&keys(&["a", "b"]), &keys(&["a", "b", "c"])),
            EventChange::Append(1)
        );
    }

    #[test]
    fn classify_both_ends_is_other() {
        assert_eq!(
            classify_event_change(&keys(&["b"]), &keys(&["a", "b", "c"])),
            EventChange::Other
        );
    }

    #[test]
    fn classify_same_len_changed_is_other() {
        assert_eq!(
            classify_event_change(&keys(&["a", "b"]), &keys(&["a", "x"])),
            EventChange::Other
        );
    }

    #[test]
    fn classify_shorter_is_other() {
        assert_eq!(
            classify_event_change(&keys(&["a", "b", "c"]), &keys(&["a", "c"])),
            EventChange::Other
        );
    }

    #[derive(Clone, Debug, PartialEq, Eq)]
    enum Item {
        Event(String),
        Virtual,
    }

    fn ev(id: &str) -> Item {
        Item::Event(id.to_string())
    }

    fn key(i: &Item) -> Option<String> {
        match i {
            Item::Event(k) => Some(k.clone()),
            Item::Virtual => None,
        }
    }

    fn vec_of(values: &[Item]) -> Vector<Item> {
        values.iter().cloned().collect()
    }

    #[test]
    fn reduce_push_front_event_is_prepend() {
        let mut mirror = vec![ev("a")];
        let op = reduce_batch(
            &mut mirror,
            vec![VectorDiff::PushFront { value: ev("z") }],
            key,
        );
        assert_eq!(op, ReducedOp::Prepend(vec![ev("z")]));
        assert_eq!(mirror, vec![ev("z"), ev("a")]);
    }

    #[test]
    fn reduce_back_pagination_newday_is_prepend() {
        // [a] --paginate back--> [dayDivider, e0, a] via front inserts.
        let mut mirror = vec![ev("a")];
        let op = reduce_batch(
            &mut mirror,
            vec![
                VectorDiff::Insert {
                    index: 0,
                    value: Item::Virtual,
                },
                VectorDiff::Insert {
                    index: 1,
                    value: ev("e0"),
                },
            ],
            key,
        );
        assert_eq!(op, ReducedOp::Prepend(vec![ev("e0")]));
    }

    #[test]
    fn reduce_back_pagination_sameday_after_divider_is_prepend() {
        // [div, a] --paginate back same day--> [div, e0, a]: event inserted at 1.
        let mut mirror = vec![Item::Virtual, ev("a")];
        let op = reduce_batch(
            &mut mirror,
            vec![VectorDiff::Insert {
                index: 1,
                value: ev("e0"),
            }],
            key,
        );
        assert_eq!(op, ReducedOp::Prepend(vec![ev("e0")]));
    }

    #[test]
    fn reduce_back_pagination_with_divider_removal_is_prepend() {
        // Old leading divider removed, new divider + event inserted. The mirror
        // knows the removed item was virtual, so this stays a clean prepend.
        let mut mirror = vec![Item::Virtual, ev("a")];
        let op = reduce_batch(
            &mut mirror,
            vec![
                VectorDiff::Remove { index: 0 },
                VectorDiff::Insert {
                    index: 0,
                    value: Item::Virtual,
                },
                VectorDiff::Insert {
                    index: 1,
                    value: ev("e0"),
                },
            ],
            key,
        );
        assert_eq!(op, ReducedOp::Prepend(vec![ev("e0")]));
    }

    #[test]
    fn reduce_multi_event_prepend_keeps_order() {
        let mut mirror = vec![ev("a")];
        let op = reduce_batch(
            &mut mirror,
            vec![
                VectorDiff::Insert {
                    index: 0,
                    value: Item::Virtual,
                },
                VectorDiff::Insert {
                    index: 1,
                    value: ev("e0"),
                },
                VectorDiff::Insert {
                    index: 2,
                    value: ev("e1"),
                },
            ],
            key,
        );
        assert_eq!(op, ReducedOp::Prepend(vec![ev("e0"), ev("e1")]));
    }

    #[test]
    fn reduce_live_append_is_append() {
        let mut mirror = vec![ev("a")];
        let op = reduce_batch(
            &mut mirror,
            vec![VectorDiff::PushBack { value: ev("b") }],
            key,
        );
        assert_eq!(op, ReducedOp::Append(vec![ev("b")]));
    }

    #[test]
    fn reduce_same_key_set_is_changed() {
        // Set replacing an event item with the SAME event id (edit / reaction /
        // read-receipt / delivery): carried as Changed so the consumer can
        // convert+compare and skip the reconvert when nothing rendered changed.
        let mut mirror = vec![ev("a"), ev("b")];
        let op = reduce_batch(
            &mut mirror,
            vec![VectorDiff::Set {
                index: 0,
                value: ev("a"),
            }],
            key,
        );
        assert_eq!(op, ReducedOp::Changed(vec![ev("a")]));
    }

    #[test]
    fn reduce_different_key_set_is_full() {
        // Set that swaps one event id for another really moves the sequence.
        let mut mirror = vec![ev("a"), ev("b")];
        let op = reduce_batch(
            &mut mirror,
            vec![VectorDiff::Set {
                index: 0,
                value: ev("c"),
            }],
            key,
        );
        assert_eq!(op, ReducedOp::Full);
    }

    #[test]
    fn reduce_same_key_set_mixed_with_prepend_is_full() {
        // A same-key Set combined with structural growth in one batch takes the
        // safe whole-snapshot path rather than emitting both.
        let mut mirror = vec![ev("a"), ev("b")];
        let op = reduce_batch(
            &mut mirror,
            vec![
                VectorDiff::Set {
                    index: 0,
                    value: ev("a"),
                },
                VectorDiff::PushFront { value: ev("z") },
            ],
            key,
        );
        assert_eq!(op, ReducedOp::Full);
    }

    #[test]
    fn reduce_event_redaction_is_full() {
        let mut mirror = vec![ev("a"), ev("b")];
        let op = reduce_batch(&mut mirror, vec![VectorDiff::Remove { index: 0 }], key);
        assert_eq!(op, ReducedOp::Full);
    }

    #[test]
    fn reduce_virtual_only_churn_is_none() {
        // Read marker moves (virtual Set) with no event change -> no event update.
        let mut mirror = vec![Item::Virtual, ev("a")];
        let op = reduce_batch(
            &mut mirror,
            vec![VectorDiff::Set {
                index: 0,
                value: Item::Virtual,
            }],
            key,
        );
        assert_eq!(op, ReducedOp::None);
    }

    #[test]
    fn reduce_mid_list_event_insert_is_full() {
        let mut mirror = vec![ev("a"), ev("b")];
        let op = reduce_batch(
            &mut mirror,
            vec![VectorDiff::Insert {
                index: 1,
                value: ev("x"),
            }],
            key,
        );
        assert_eq!(op, ReducedOp::Full);
    }

    #[test]
    fn reduce_reset_is_full() {
        let mut mirror = vec![ev("a")];
        let op = reduce_batch(
            &mut mirror,
            vec![VectorDiff::Reset {
                values: vec_of(&[ev("a"), ev("b")]),
            }],
            key,
        );
        assert_eq!(op, ReducedOp::Full);
        assert_eq!(mirror, vec![ev("a"), ev("b")]);
    }

    #[test]
    fn reduce_append_values_batch_is_append() {
        let mut mirror = vec![ev("a")];
        let op = reduce_batch(
            &mut mirror,
            vec![VectorDiff::Append {
                values: vec_of(&[ev("b"), ev("c")]),
            }],
            key,
        );
        assert_eq!(op, ReducedOp::Append(vec![ev("b"), ev("c")]));
    }
}

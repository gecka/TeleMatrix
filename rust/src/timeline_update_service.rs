// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::collections::{HashMap, HashSet};
use std::sync::atomic::{AtomicU8, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::Duration;

use futures_util::StreamExt;
use matrix_sdk::Client;
use tokio::sync::RwLock;
use tokio::task::JoinHandle;
use tracing::{debug, info, warn};

use crate::link_preview_rules;
use crate::room_summary_service::RoomSummaryService;
use crate::timeline_cache_service::{TimelineCacheContext, TimelineCacheService};
use crate::timeline_window::{OnTimelineChanged, TimelineDiff};
use crate::timeline_window_service::TimelineRuntime;
use crate::types::{MessageContent, TimelineItem, TimelineSlice, UrlPreview};

enum UrlPreviewOutcome {
    /// Got preview data, or a definitive "no preview" from the server. Cached
    /// (and persisted) so it is fetched once.
    Success(Option<UrlPreview>),
    /// The server definitively rejected the URL (4xx other than 404/429) or it
    /// is on the ignore list. Negative-cached + persisted: never retried.
    Terminal,
    /// The request timed out. Counts toward [`MAX_PREVIEW_TIMEOUTS`]; the URL is
    /// given up on (negative-cached for the session) once the cap is reached.
    Timeout,
    /// Transient transport failure (network, 5xx, rate-limit). Not cached, so it
    /// is retried on a later timeline diff.
    Transient,
}

enum FetchRawResult {
    Success(Box<serde_json::value::RawValue>),
    ServerNoData,
    /// Endpoint not implemented (404) — the caller probes the other endpoint.
    Unsupported,
    /// Definitive 4xx rejection of the URL (e.g. 403 "IP address blocked").
    Terminal,
    /// The request timed out.
    Timeout,
    /// Transient transport failure — retry later.
    Transient,
}

type TimelineCache = Arc<RwLock<HashMap<String, Vec<TimelineItem>>>>;
type TimelineUpdateCache = Arc<RwLock<HashMap<String, TimelineSlice>>>;
type TimelineCallbacks = Arc<Mutex<HashMap<String, Box<dyn Fn() + Send>>>>;
type RoomListCallback = Arc<Mutex<Option<Box<dyn Fn() + Send>>>>;
type UrlPreviewCache = Arc<RwLock<HashMap<String, Option<UrlPreview>>>>;
type PreviewInflight = Arc<RwLock<HashSet<String>>>;
type PreviewStore = Arc<std::sync::Mutex<Option<crate::preview_store::PreviewStore>>>;

#[derive(Clone)]
pub(crate) struct TimelineUrlPreviewContext {
    pub(crate) room_id: String,
    pub(crate) client: Client,
    pub(crate) cache: TimelineCache,
    pub(crate) preview_cache: UrlPreviewCache,
    pub(crate) preview_inflight: PreviewInflight,
    pub(crate) preview_store: PreviewStore,
    pub(crate) callbacks: TimelineCallbacks,
    pub(crate) room_list_callback: RoomListCallback,
    pub(crate) update_cache: TimelineUpdateCache,
    pub(crate) preview_fetch_callback: crate::preview_fetch_signal::PreviewFetchCallbackSlot,
}

fn lock_update_mutex<'a, T>(mutex: &'a Mutex<T>, name: &str) -> MutexGuard<'a, T> {
    match mutex.lock() {
        Ok(guard) => guard,
        Err(poisoned) => {
            warn!("Recovering poisoned Matrix mutex: {name}");
            poisoned.into_inner()
        }
    }
}

/// Process-wide cap on concurrent outbound URL-preview HTTP fetches. Each
/// timeline diff spawns a preview task, and each task may fetch several URLs;
/// without a shared limit a burst of diffs across many rooms (initial sync,
/// backfill) could open ~100 simultaneous connections. The permit is held for
/// the duration of each individual fetch, bounding total in-flight requests.
fn preview_fetch_semaphore() -> &'static tokio::sync::Semaphore {
    static SEM: std::sync::OnceLock<tokio::sync::Semaphore> = std::sync::OnceLock::new();
    SEM.get_or_init(|| tokio::sync::Semaphore::new(6))
}

/// Give up previewing a URL after this many timeouts in a row, so a chronically
/// unreachable host is not re-fetched on every timeline diff forever.
const MAX_PREVIEW_TIMEOUTS: u32 = 3;

/// Request config for preview fetches.
///
/// The SDK default is *unbounded* retries (`retry_limit: None`), so a 502 from a
/// preview endpoint turned one logical attempt into ~5 HTTP requests, each held
/// inside our timeout wrapper while occupying one of the 6 preview-fetch permits
/// — starving other previews and inflating the live task count during a scroll.
/// Previews already have their own retry policy above this layer (negative-cache
/// on terminal 4xx, give up after MAX_PREVIEW_TIMEOUTS), so one retry is enough
/// to ride out a blip and the policy decides the rest.
fn preview_request_config() -> matrix_sdk::config::RequestConfig {
    matrix_sdk::config::RequestConfig::default().retry_limit(1)
}

/// Rooms with a preview scan in flight, mapped to "another diff landed while it
/// ran". The scan walks the whole resident window, and a task was spawned per
/// diff batch — so a fast back-scroll queued one full walk per landed page, all
/// competing for the same 6 permits. One scan per room at a time, re-run once if
/// anything changed underneath it.
fn preview_scan_state() -> &'static std::sync::Mutex<HashMap<String, bool>> {
    static STATE: std::sync::OnceLock<std::sync::Mutex<HashMap<String, bool>>> =
        std::sync::OnceLock::new();
    STATE.get_or_init(Default::default)
}

/// Releases the scan claim even if the task is aborted mid-fetch (logout), which
/// would otherwise strand the room with a claim nobody ever clears.
struct PreviewScanClaim {
    room_id: String,
}

impl Drop for PreviewScanClaim {
    fn drop(&mut self) {
        if let Ok(mut state) = preview_scan_state().lock() {
            state.remove(&self.room_id);
        }
    }
}

/// Concurrent per-room backup key downloads in the session-wide sweep. Kept low
/// on purpose — see `spawn_backup_bulk_key_prefetch`.
const BULK_KEY_FETCH_CONCURRENCY: usize = 4;

/// When to re-sweep for rooms the previous pass could not see. `client.rooms()`
/// only holds what sliding sync has materialized, and a single pass right after
/// the backup becomes usable misses everything that appears afterwards — those
/// rooms would then stay undecryptable for the whole session. Already-fetched
/// rooms are skipped, so the later passes cost one `client.rooms()` walk.
const BULK_KEY_SWEEP_SCHEDULE: &[Duration] = &[
    Duration::from_secs(0),
    Duration::from_secs(15),
    Duration::from_secs(60),
    Duration::from_secs(240),
];

/// Per-URL consecutive-timeout counter backing [`MAX_PREVIEW_TIMEOUTS`].
/// Process-global (like the fetch semaphore) and self-cleaning: an entry is
/// removed as soon as the URL succeeds, errors terminally, or hits the cap
/// (after which the negative preview-cache entry stops further fetches). Only
/// URLs with 1..MAX still-pending timeouts are ever retained, so it stays tiny;
/// surviving counts across a session change are harmless.
fn preview_timeout_counts() -> &'static Mutex<HashMap<String, u32>> {
    static COUNTS: std::sync::OnceLock<Mutex<HashMap<String, u32>>> = std::sync::OnceLock::new();
    COUNTS.get_or_init(|| Mutex::new(HashMap::new()))
}

/// Hosts we never request link previews for: IP-literal hosts (which the
/// homeserver blocks anyway — "M_UNKNOWN IP address blocked") and
/// loopback/internal names. Skips a doomed request (and its retry loop) for
/// URLs that can never yield a public OG card.
fn url_ignored_for_preview(url: &str) -> bool {
    let Some(host) = preview_url_host(url) else {
        return true;
    };
    // IP literals (v4/v6), including loopback/private ranges. OG cards are for
    // named sites; an IP host is exactly what the server refuses.
    if host.parse::<std::net::IpAddr>().is_ok() {
        return true;
    }
    host == "localhost"
        || host.ends_with(".localhost")
        || host.ends_with(".local")
        || host.ends_with(".internal")
}

/// Lowercased host of an `http(s)` URL (the only schemes the extractor yields).
/// Strips userinfo and port and unwraps an IPv6 literal's brackets so it parses
/// as an address.
fn preview_url_host(url: &str) -> Option<String> {
    let authority = url.split_once("://")?.1.split(['/', '?', '#']).next()?;
    let host_port = match authority.rsplit_once('@') {
        Some((_, hp)) => hp,
        None => authority,
    };
    if let Some(rest) = host_port.strip_prefix('[') {
        // IPv6 literal: [::1]:8080 -> ::1
        return rest.split_once(']').map(|(h, _)| h.to_ascii_lowercase());
    }
    let host = host_port.split(':').next()?;
    (!host.is_empty()).then(|| host.to_ascii_lowercase())
}

/// Classify a failed preview `client.send`. A 404 means the endpoint is not
/// implemented (probe the other one); any other 4xx except 429 is a definitive
/// rejection of the URL; everything else (429, 5xx, transport) is transient.
fn classify_preview_send_error(e: &matrix_sdk::HttpError) -> FetchRawResult {
    match e.as_client_api_error().map(|api| api.status_code.as_u16()) {
        Some(404) => FetchRawResult::Unsupported,
        Some(429) => FetchRawResult::Transient,
        Some(code) if (400..500).contains(&code) => FetchRawResult::Terminal,
        _ => FetchRawResult::Transient,
    }
}

pub(crate) struct TimelineUpdateService;

impl TimelineUpdateService {
    pub(crate) fn make_on_changed_callback(runtime: TimelineRuntime) -> OnTimelineChanged {
        Arc::new(move |room_id: &str, diffs: Vec<TimelineDiff>| {
            let rid = room_id.to_string();
            let refresh_state = TimelineCacheService::refresh_state(&runtime.refresh_states, &rid);
            TimelineCacheService::queue_diffs(&refresh_state, diffs);
            refresh_state.dirty.store(true, Ordering::Release);
            if refresh_state.running.swap(true, Ordering::AcqRel) {
                return;
            }

            let runtime = runtime.clone();
            let timeline_cache_context = TimelineCacheContext {
                cache: runtime.cache.clone(),
                update_cache: runtime.update_cache.clone(),
                reply_preview_cache: runtime.reply_preview_cache.clone(),
                pending_reaction_overrides: runtime.pending_reaction_overrides.clone(),
                media_sources: runtime.media_sources.clone(),
                preview_cache: runtime.preview_cache.clone(),
                sender_avatar_cache: runtime.sender_avatar_cache.clone(),
                search_index: runtime.search_index.clone(),
                search_index_fingerprints: runtime.search_index_fingerprints.clone(),
                pending_forward_meta: runtime.pending_forward_meta.clone(),
            };
            // Registered in session tasks: this loop holds SdkTimeline
            // (and thus Client/store) references, so it must die on
            // logout or it keeps sqlite connections open through the
            // store wipe.
            let worker_tasks = runtime.session_tasks.clone();
            worker_tasks.spawn(async move {
                loop {
                    refresh_state.dirty.store(false, Ordering::Release);

                    let (active_timeline, is_live) = {
                        let wins = runtime.windows.read().await;
                        match wins.get(&rid) {
                            Some(w) => (w.active_timeline().clone(), w.is_live()),
                            None => {
                                refresh_state.running.store(false, Ordering::Release);
                                return;
                            }
                        }
                    };

                    let diffs = TimelineCacheService::take_diffs(&refresh_state);
                    // Capture genuinely-new bottom-edge items (live appends)
                    // before the diffs are consumed, for per-event notification.
                    let new_arrivals: Vec<std::sync::Arc<matrix_sdk_ui::timeline::TimelineItem>> =
                        if is_live {
                            diffs
                                .iter()
                                .flat_map(|d| match d {
                                    TimelineDiff::Append(items) => items.clone(),
                                    TimelineDiff::PushBack(item) => vec![item.clone()],
                                    TimelineDiff::PushFront(_)
                                    | TimelineDiff::Prepend(_)
                                    | TimelineDiff::Changed(_)
                                    | TimelineDiff::Full => Vec::new(),
                                })
                                .collect()
                        } else {
                            Vec::new()
                        };
                    // A back-pagination page adds only OLDER events, so the room's
                    // newest event — and with it the rooms-list preview, timestamp
                    // and sort position — cannot change. Refreshing the summary
                    // anyway rebuilt it and re-sorted the ENTIRE rooms list on
                    // every landed page of a back-scroll. `Changed` still counts:
                    // a redecrypt/edit/redaction can land on the newest event.
                    let touches_newest = diffs.iter().any(|d| {
                        !matches!(d, TimelineDiff::PushFront(_) | TimelineDiff::Prepend(_))
                    });
                    let applied_incremental = is_live
                        && TimelineCacheService::cache_timeline_diffs(
                            &rid,
                            &active_timeline,
                            diffs,
                            &timeline_cache_context,
                        )
                        .await;
                    if !applied_incremental {
                        TimelineCacheService::cache_timeline_snapshot(
                            &rid,
                            &active_timeline,
                            &timeline_cache_context,
                        )
                        .await;
                    }
                    // R2D2 + the SDK timeline redecrypt UTDs reactively; we only
                    // recache and refresh the rooms-list preview so a redecrypted
                    // last message replaces stale UTD text on the room's own diff.
                    if is_live && touches_newest {
                        let no = runtime.notification_overrides.read().await;
                        let presence_snap = runtime.presence_typing.presence_snapshot();
                        RoomSummaryService::refresh_room_summary_cache(
                            active_timeline.room(),
                            &rid,
                            &runtime.cache,
                            &runtime.rooms_cache,
                            &no,
                            &presence_snap,
                        )
                        .await;
                    }

                    if is_live {
                        crate::notification_service::evaluate_and_emit(
                            &rid,
                            active_timeline.room(),
                            &new_arrivals,
                            &runtime.cache,
                            &runtime.sync_state,
                            &runtime.notification_callback,
                        )
                        .await;
                    }

                    Self::notify_timeline_update(
                        &rid,
                        &runtime.callbacks,
                        &runtime.room_list_callback,
                    );

                    let url_preview_context = TimelineUrlPreviewContext {
                        room_id: rid.clone(),
                        client: active_timeline.room().client(),
                        cache: runtime.cache.clone(),
                        preview_cache: runtime.preview_cache.clone(),
                        preview_inflight: runtime.preview_inflight.clone(),
                        preview_store: runtime.preview_store.clone(),
                        callbacks: runtime.callbacks.clone(),
                        room_list_callback: runtime.room_list_callback.clone(),
                        update_cache: runtime.update_cache.clone(),
                        preview_fetch_callback: runtime.preview_fetch_callback.clone(),
                    };
                    runtime
                        .session_tasks
                        .spawn(Self::fetch_url_previews_coalesced(
                            url_preview_context.clone(),
                        ));

                    if !refresh_state.dirty.swap(false, Ordering::AcqRel) {
                        refresh_state.running.store(false, Ordering::Release);
                        if !refresh_state.dirty.load(Ordering::Acquire)
                            || refresh_state.running.swap(true, Ordering::AcqRel)
                        {
                            break;
                        }
                    }
                }
            });
        })
    }

    /// One preview scan per room at a time; a diff arriving mid-scan asks the
    /// running scan to repeat once rather than queueing another full walk.
    async fn fetch_url_previews_coalesced(context: TimelineUrlPreviewContext) {
        let room_id = context.room_id.clone();
        {
            let Ok(mut state) = preview_scan_state().lock() else {
                return;
            };
            if let Some(dirty) = state.get_mut(&room_id) {
                *dirty = true;
                return;
            }
            state.insert(room_id.clone(), false);
        }
        let _claim = PreviewScanClaim {
            room_id: room_id.clone(),
        };
        loop {
            Self::fetch_url_previews_and_notify(context.clone()).await;
            let Ok(mut state) = preview_scan_state().lock() else {
                return;
            };
            match state.get_mut(&room_id) {
                // Something landed while we scanned — go round once more.
                Some(dirty) if *dirty => *dirty = false,
                // Nothing new: _claim's Drop releases the room.
                _ => return,
            }
        }
    }

    async fn fetch_url_previews_and_notify(context: TimelineUrlPreviewContext) {
        let room_id = context.room_id.clone();

        // One pass over the cached items. For every text item still lacking a
        // preview, map its extracted URL to ALL event ids that reference it, and
        // split into: already positive-cached (patch now — heals an item whose
        // conversion missed a since-cached preview, e.g. the same link posted in
        // a second message or seen first in another room) vs. needs-fetch.
        let mut url_events: HashMap<String, Vec<String>> = HashMap::new();
        let mut to_fetch: Vec<String> = Vec::new();
        let mut cached_patch: Vec<(String, UrlPreview)> = Vec::new();
        {
            let guard = context.cache.read().await;
            let items = match guard.get(&room_id) {
                Some(items) => items,
                None => return,
            };
            let pc = context.preview_cache.read().await;
            let inflight = context.preview_inflight.read().await;
            let mut fetch_seen = HashSet::new();
            for item in items.iter() {
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
                // Cheap prefilter: with no scheme substring anywhere there is no
                // URL to extract, so skip the (allocating) extraction that would
                // otherwise run for every plain-text message on every diff.
                if !body.contains("http") && !fmt.is_some_and(|f| f.contains("http")) {
                    continue;
                }
                let Some(url) = TimelineCacheService::extract_url_with_formatted(body, fmt) else {
                    continue;
                };
                if let Some(cached) = pc.get(&url) {
                    if let Some(preview) = cached {
                        cached_patch.push((item.event_id.clone(), preview.clone()));
                    }
                    continue; // negative-cached -> nothing to render
                }
                if inflight.contains(&url) {
                    continue; // another task owns this fetch; heals on a later diff
                }
                url_events
                    .entry(url.clone())
                    .or_default()
                    .push(item.event_id.clone());
                if fetch_seen.insert(url.clone()) {
                    to_fetch.push(url);
                }
            }
        }

        let mut any_patched = false;

        // Patch items whose preview was already cached (no fetch needed).
        if !cached_patch.is_empty() {
            let mut guard = context.cache.write().await;
            if let Some(items) = guard.get_mut(&room_id) {
                for (event_id, preview) in &cached_patch {
                    if let Some(item) = items.iter_mut().find(|i| &i.event_id == event_id) {
                        if item.url_preview.is_none() {
                            item.url_preview = Some(preview.clone());
                            any_patched = true;
                        }
                    }
                }
            }
        }

        if to_fetch.is_empty() {
            if any_patched {
                Self::notify_timeline_update(
                    &room_id,
                    &context.callbacks,
                    &context.room_list_callback,
                );
            }
            return;
        }

        {
            let mut inflight = context.preview_inflight.write().await;
            for url in &to_fetch {
                inflight.insert(url.clone());
            }
            // Glow every event referencing a URL now being fetched.
            for event_ids in url_events.values() {
                for event_id in event_ids {
                    crate::preview_fetch_signal::emit(
                        &context.preview_fetch_callback,
                        &room_id,
                        event_id,
                        true,
                    );
                }
            }
        }

        use futures_util::stream::FuturesUnordered;
        let futs: FuturesUnordered<_> = to_fetch
            .into_iter()
            .map(|url| {
                let client = context.client.clone();
                async move {
                    // Hold a permit for the whole fetch so the global semaphore
                    // bounds how many preview requests run at once across rooms.
                    let _permit = preview_fetch_semaphore().acquire().await;
                    let outcome = Self::fetch_url_preview_outcome(&client, &url).await;
                    (url, outcome)
                }
            })
            .collect();

        let mut stream = futs;
        let mut results: Vec<(String, UrlPreviewOutcome)> = Vec::new();
        while let Some(result) = stream.next().await {
            results.push(result);
        }

        {
            let mut inflight = context.preview_inflight.write().await;
            for (url, _) in &results {
                inflight.remove(url);
                // Stop the glow on every event of this URL — success or failure.
                if let Some(event_ids) = url_events.get(url) {
                    for event_id in event_ids {
                        crate::preview_fetch_signal::emit(
                            &context.preview_fetch_callback,
                            &room_id,
                            event_id,
                            false,
                        );
                    }
                }
            }
        }

        if results.is_empty() {
            if any_patched {
                Self::notify_timeline_update(
                    &room_id,
                    &context.callbacks,
                    &context.room_list_callback,
                );
            }
            return;
        }

        {
            let mut pc = context.preview_cache.write().await;
            let mut guard = context.cache.write().await;
            let mut items = guard.get_mut(&room_id);
            let mut timeouts =
                lock_update_mutex(preview_timeout_counts(), "preview_timeout_counts");
            for (url, outcome) in &results {
                tracing::info!(
                    "url_preview: {} -> {}",
                    url,
                    match outcome {
                        UrlPreviewOutcome::Success(Some(_)) => "success",
                        UrlPreviewOutcome::Success(None) => "success(no-data)",
                        UrlPreviewOutcome::Terminal => "terminal",
                        UrlPreviewOutcome::Timeout => "timeout",
                        UrlPreviewOutcome::Transient => "transient",
                    }
                );
                match outcome {
                    UrlPreviewOutcome::Success(preview) => {
                        timeouts.remove(url);
                        pc.insert(url.clone(), preview.clone());
                        // Patch EVERY event that referenced this URL, not just the
                        // first — otherwise a duplicate link stays cardless until an
                        // unrelated full reconvert (M4).
                        if let (Some(preview), Some(items)) = (preview, items.as_mut()) {
                            if let Some(event_ids) = url_events.get(url) {
                                for event_id in event_ids {
                                    if let Some(item) =
                                        items.iter_mut().find(|i| &i.event_id == event_id)
                                    {
                                        item.url_preview = Some(preview.clone());
                                        any_patched = true;
                                    }
                                }
                            }
                        }
                    }
                    UrlPreviewOutcome::Terminal => {
                        // Definitive rejection / ignored host: negative-cache so it
                        // is never retried (also persisted below).
                        timeouts.remove(url);
                        pc.insert(url.clone(), None);
                    }
                    UrlPreviewOutcome::Timeout => {
                        let count = {
                            let c = timeouts.entry(url.clone()).or_insert(0);
                            *c += 1;
                            *c
                        };
                        if count >= MAX_PREVIEW_TIMEOUTS {
                            timeouts.remove(url);
                            // Give up for this session; not persisted, so a restart
                            // gives a possibly-recovered host a fresh chance.
                            pc.insert(url.clone(), None);
                        }
                        // Otherwise leave it uncached so the next diff retries.
                    }
                    UrlPreviewOutcome::Transient => {}
                }
            }
        }

        if let Ok(store) = context.preview_store.lock() {
            if let Some(store) = store.as_ref() {
                for (url, outcome) in &results {
                    match outcome {
                        UrlPreviewOutcome::Success(preview) => {
                            let _ = store.save(url, preview);
                        }
                        // Persist definitive rejections so a restart doesn't refetch
                        // them either (bounded by the store's expiry). Timeouts and
                        // transient errors are intentionally not persisted.
                        UrlPreviewOutcome::Terminal => {
                            let _ = store.save(url, &None);
                        }
                        UrlPreviewOutcome::Timeout | UrlPreviewOutcome::Transient => {}
                    }
                }
            }
        }

        if any_patched {
            // The patch above only touched context.cache. A bare notify then
            // races the C++ incremental pull, which a concurrent MetadataOnly/
            // Append diff can preempt — stranding the card until an unrelated
            // later refresh. Fold a Full snapshot of the resident window into
            // update_cache so the next pull carries the preview regardless
            // (coalescing keeps it Full against any pending diff).
            let items = {
                let cache = context.cache.read().await;
                cache.get(&room_id).cloned().unwrap_or_default()
            };
            if !items.is_empty() {
                TimelineCacheService::stage_full_update(&context.update_cache, &room_id, &items)
                    .await;
            }
            Self::notify_timeline_update(&room_id, &context.callbacks, &context.room_list_callback);
        }
    }

    async fn fetch_url_preview_outcome(client: &Client, url: &str) -> UrlPreviewOutcome {
        // Never even attempt previews for IP-literal / loopback / internal hosts.
        if url_ignored_for_preview(url) {
            return UrlPreviewOutcome::Terminal;
        }

        static ENDPOINT: AtomicU8 = AtomicU8::new(0);
        const AUTH: u8 = 1;
        const LEGACY: u8 = 2;

        let timeout = Duration::from_secs(15);
        let pref = ENDPOINT.load(Ordering::Relaxed);

        let raw_result = if pref == LEGACY {
            Self::fetch_preview_legacy_result(client, url, timeout).await
        } else if pref == AUTH {
            Self::fetch_preview_auth_result(client, url, timeout).await
        } else {
            // Probe legacy (`/_matrix/media/v3/preview_url`) first, then fall back
            // to the authenticated endpoint ONLY when legacy reports the endpoint
            // is unimplemented (404). Synapse advertises Matrix 1.11 but does NOT
            // implement the authenticated `/_matrix/client/v1/media/preview_url`
            // (404 M_UNRECOGNIZED) while the legacy endpoint works; legacy-first
            // avoids that doomed request. A terminal rejection or timeout on legacy
            // would recur on auth (same server/policy/network), so we don't double
            // the work — those are returned directly.
            let legacy_result = Self::fetch_preview_legacy_result(client, url, timeout).await;
            match legacy_result {
                FetchRawResult::Success(_) | FetchRawResult::ServerNoData => {
                    ENDPOINT.store(LEGACY, Ordering::Relaxed);
                    legacy_result
                }
                FetchRawResult::Unsupported => {
                    let auth_result = Self::fetch_preview_auth_result(client, url, timeout).await;
                    if matches!(
                        auth_result,
                        FetchRawResult::Success(_) | FetchRawResult::ServerNoData
                    ) {
                        ENDPOINT.store(AUTH, Ordering::Relaxed);
                    }
                    auth_result
                }
                FetchRawResult::Terminal => FetchRawResult::Terminal,
                FetchRawResult::Timeout => FetchRawResult::Timeout,
                FetchRawResult::Transient => FetchRawResult::Transient,
            }
        };

        match raw_result {
            FetchRawResult::Success(raw_json) => match Self::parse_preview_json(&raw_json, url) {
                // Valid JSON: either a preview or a definitive "no OG data".
                Ok(preview) => UrlPreviewOutcome::Success(preview),
                // Malformed JSON is a transient server glitch, not a real "no
                // preview" — retry later rather than negative-cache it for 30 days.
                Err(()) => UrlPreviewOutcome::Transient,
            },
            FetchRawResult::ServerNoData => UrlPreviewOutcome::Success(None),
            FetchRawResult::Unsupported => {
                if pref != 0 {
                    // A previously-working endpoint now 404s; unpin and re-probe
                    // next time instead of giving up on the URL.
                    ENDPOINT.store(0, Ordering::Relaxed);
                    UrlPreviewOutcome::Transient
                } else {
                    // Neither endpoint implements preview_url — stop trying.
                    UrlPreviewOutcome::Terminal
                }
            }
            FetchRawResult::Terminal => UrlPreviewOutcome::Terminal,
            FetchRawResult::Timeout => UrlPreviewOutcome::Timeout,
            FetchRawResult::Transient => UrlPreviewOutcome::Transient,
        }
    }

    /// Parse a preview-endpoint JSON body. `Err(())` means the body did not parse
    /// as JSON at all (a transient server glitch — the caller retries). `Ok(None)`
    /// means valid JSON with no usable OG fields (a real "no preview").
    fn parse_preview_json(
        raw_json: &serde_json::value::RawValue,
        url: &str,
    ) -> Result<Option<UrlPreview>, ()> {
        let json: serde_json::Value = serde_json::from_str(raw_json.get()).map_err(|_| ())?;

        let og_title = json
            .get("og:title")
            .and_then(|v| v.as_str())
            .map(str::to_string);
        let og_description = json
            .get("og:description")
            .and_then(|v| v.as_str())
            .map(str::to_string);
        let og_site_name = json
            .get("og:site_name")
            .and_then(|v| v.as_str())
            .map(str::to_string);
        let og_type = json
            .get("og:type")
            .and_then(|v| v.as_str())
            .unwrap_or("article");
        // Only an mxc:// image is renderable: a spec-compliant homeserver rewrites
        // og:image to mxc://, and our thumbnail resolver only understands mxc. A
        // raw http(s) og:image (hostile/buggy server) would otherwise never load
        // and leave the card's skeleton glowing forever — drop it and zero its
        // dimensions so the card renders text-only.
        let og_image = json
            .get("og:image")
            .and_then(|v| v.as_str())
            .filter(|u| u.starts_with("mxc://"))
            .map(str::to_string);
        let (og_image_width, og_image_height) = if og_image.is_some() {
            let w = json
                .get("og:image:width")
                .and_then(|v| {
                    v.as_u64()
                        .or_else(|| v.as_str().and_then(|s| s.parse().ok()))
                })
                .unwrap_or(0) as u32;
            let h = json
                .get("og:image:height")
                .and_then(|v| {
                    v.as_u64()
                        .or_else(|| v.as_str().and_then(|s| s.parse().ok()))
                })
                .unwrap_or(0) as u32;
            (w, h)
        } else {
            (0, 0)
        };

        if og_title.is_none() && og_description.is_none() && og_image.is_none() {
            return Ok(None);
        }

        let preview_type = link_preview_rules::preview_type_from_og_type(og_type);

        let mut preview = UrlPreview {
            url: url.to_string(),
            site_name: og_site_name,
            title: og_title,
            description: og_description,
            image_url: og_image,
            image_width: og_image_width,
            image_height: og_image_height,
            preview_type,
            duration_secs: 0,
            author: None,
            has_large_media: false,
            site_name_canonical: None,
        };

        link_preview_rules::apply_provider_rules(&mut preview);
        Ok(Some(preview))
    }

    async fn fetch_preview_auth_result(
        client: &Client,
        url: &str,
        timeout: Duration,
    ) -> FetchRawResult {
        use matrix_sdk::ruma::api::client::authenticated_media::get_media_preview;
        let request = get_media_preview::v1::Request::new(url.to_owned());
        let send = client
            .send(request)
            .with_request_config(preview_request_config());
        match tokio::time::timeout(timeout, send).await {
            Ok(Ok(r)) => match r.data {
                Some(data) => FetchRawResult::Success(data),
                None => FetchRawResult::ServerNoData,
            },
            Ok(Err(e)) => {
                warn!("Preview fetch (auth) failed for {url}: {e}");
                classify_preview_send_error(&e)
            }
            Err(_) => {
                warn!("Preview fetch (auth) timed out for {url}");
                FetchRawResult::Timeout
            }
        }
    }

    #[allow(deprecated)]
    async fn fetch_preview_legacy_result(
        client: &Client,
        url: &str,
        timeout: Duration,
    ) -> FetchRawResult {
        use matrix_sdk::ruma::api::client::media::get_media_preview;
        let request = get_media_preview::v3::Request::new(url.to_owned());
        let send = client
            .send(request)
            .with_request_config(preview_request_config());
        match tokio::time::timeout(timeout, send).await {
            Ok(Ok(r)) => match r.data {
                Some(data) => FetchRawResult::Success(data),
                None => FetchRawResult::ServerNoData,
            },
            Ok(Err(e)) => {
                warn!("Preview fetch (legacy) failed for {url}: {e}");
                classify_preview_send_error(&e)
            }
            Err(_) => {
                warn!("Preview fetch (legacy) timed out for {url}");
                FetchRawResult::Timeout
            }
        }
    }

    #[allow(deprecated)]
    pub(crate) async fn fetch_url_preview(client: &Client, url: &str) -> Option<UrlPreview> {
        match Self::fetch_url_preview_outcome(client, url).await {
            UrlPreviewOutcome::Success(preview) => preview,
            UrlPreviewOutcome::Terminal
            | UrlPreviewOutcome::Timeout
            | UrlPreviewOutcome::Transient => None,
        }
    }

    pub(crate) fn notify_timeline_update(
        room_id: &str,
        callbacks: &TimelineCallbacks,
        room_list_callback: &RoomListCallback,
    ) {
        Self::notify_timeline_callback(room_id, callbacks);
        Self::notify_room_list_update(room_list_callback);
    }

    pub(crate) fn notify_timeline_callback(room_id: &str, callbacks: &TimelineCallbacks) {
        let guard = lock_update_mutex(callbacks, "timeline_callbacks");
        if let Some(cb) = guard.get(room_id) {
            cb();
        }
    }

    pub(crate) fn notify_room_list_update(room_list_callback: &RoomListCallback) {
        let guard = lock_update_mutex(room_list_callback, "room_list_callback");
        if let Some(cb) = guard.as_ref() {
            cb();
        }
    }

    /// One-shot bulk room-key prefetch from key backup, fired once the backup is
    /// usable. `BackupDownloadStrategy::AfterDecryptionFailure` fetches a single
    /// megolm session per UTD (a ~100ms-debounced trickle), so a whole account
    /// decrypts very slowly and the per-room "decrypting" indicator never settles
    /// (it re-arms on every key wave). `download_room_keys_for_room` is one
    /// server-paginated request per room; a few in parallel burst key acquisition.
    /// Complements — does not replace — AfterDecryptionFailure (which still catches
    /// keys backed up later). Returns the handle so the caller aborts it on session
    /// change.
    ///
    /// Concurrency is deliberately modest: the same `BackupState::Enabled`
    /// transition this waits on also makes the SDK retry every in-memory UTD and
    /// enqueue a per-session key download for each failure, so a wide sweep here
    /// lands on top of that storm and earns 429s — and a 429 puts that session in
    /// the SDK's 24-hour `FailuresCache`, disabling the per-UTD fallback too.
    pub(crate) fn spawn_backup_bulk_key_prefetch(
        client: Client,
        runtime_handle: tokio::runtime::Handle,
    ) -> JoinHandle<()> {
        runtime_handle.spawn(async move {
            let backups = client.encryption().backups();
            // `state_stream()` does NOT replay the current state, so check readiness
            // FIRST — an already-verified returning user has the backup enabled at
            // startup and would otherwise wait forever for a transition that already
            // happened.
            if !backups.are_enabled().await {
                let mut stream = Box::pin(backups.state_stream());
                loop {
                    match stream.next().await {
                        Some(_) => {
                            if backups.are_enabled().await {
                                break;
                            }
                        }
                        None => return,
                    }
                }
            }
            // Sweep repeatedly, not once. `client.rooms()` only holds the rooms
            // sliding sync has materialized so far, and right after a verification
            // the client is still filling that in — a single pass permanently skips
            // every room that shows up afterwards, leaving it undecryptable until
            // the user happens to open it. Already-fetched rooms are skipped inside
            // the sweep, so the later passes are nearly free.
            let mut seen: std::collections::HashSet<matrix_sdk::ruma::OwnedRoomId> =
                std::collections::HashSet::new();
            for delay in BULK_KEY_SWEEP_SCHEDULE {
                tokio::time::sleep(*delay).await;
                Self::bulk_download_room_keys(&client, &mut seen).await;
            }
        })
    }

    async fn bulk_download_room_keys(
        client: &Client,
        already_fetched: &mut std::collections::HashSet<matrix_sdk::ruma::OwnedRoomId>,
    ) {
        let room_ids: Vec<matrix_sdk::ruma::OwnedRoomId> = client
            .rooms()
            .iter()
            .filter(|room| room.state() == matrix_sdk::RoomState::Joined && !room.is_space())
            .map(|room| room.room_id().to_owned())
            .filter(|room_id| !already_fetched.contains(room_id))
            .collect();
        if room_ids.is_empty() {
            return;
        }
        let total = room_ids.len();
        already_fetched.extend(room_ids.iter().cloned());
        info!("[keys] backup usable — bulk-prefetching keys for {total} room(s)");
        futures_util::stream::iter(room_ids)
            .for_each_concurrent(BULK_KEY_FETCH_CONCURRENCY, |room_id| {
                let client = client.clone();
                async move {
                    if let Err(e) = client
                        .encryption()
                        .backups()
                        .download_room_keys_for_room(&room_id)
                        .await
                    {
                        debug!("[keys] bulk prefetch for {room_id} failed: {e}");
                    }
                }
            })
            .await;
        info!("[keys] bulk backup-key prefetch complete ({total} room(s))");
    }
}

#[cfg(test)]
mod tests {
    use super::{preview_url_host, url_ignored_for_preview, TimelineUpdateService};

    fn raw(json: &str) -> Box<serde_json::value::RawValue> {
        serde_json::value::RawValue::from_string(json.to_string()).unwrap()
    }

    // A non-mxc og:image is dropped (with its dimensions) so the card renders
    // text-only instead of glowing forever on an unresolvable image (M2).
    #[test]
    fn parse_preview_drops_non_mxc_image() {
        let json = r#"{"og:title":"T","og:image":"https://cdn.example.com/x.jpg","og:image:width":800,"og:image:height":600}"#;
        let preview = TimelineUpdateService::parse_preview_json(&raw(json), "https://example.com")
            .unwrap()
            .expect("title present -> Some");
        assert_eq!(preview.image_url, None);
        assert_eq!(preview.image_width, 0);
        assert_eq!(preview.image_height, 0);
        assert_eq!(preview.title.as_deref(), Some("T"));
    }

    // An mxc:// og:image (and its dimensions) is kept.
    #[test]
    fn parse_preview_keeps_mxc_image() {
        let json = r#"{"og:title":"T","og:image":"mxc://server/abc","og:image:width":800,"og:image:height":600}"#;
        let preview = TimelineUpdateService::parse_preview_json(&raw(json), "https://example.com")
            .unwrap()
            .expect("some");
        assert_eq!(preview.image_url.as_deref(), Some("mxc://server/abc"));
        assert_eq!(preview.image_width, 800);
        assert_eq!(preview.image_height, 600);
    }

    // Valid JSON with no OG fields is a real "no preview" (Ok(None)), distinct
    // from a parse failure (Err) which is transient (L3).
    #[test]
    fn parse_preview_no_og_fields_is_ok_none() {
        assert_eq!(
            TimelineUpdateService::parse_preview_json(&raw(r#"{"foo":"bar"}"#), "https://x.com"),
            Ok(None)
        );
    }

    #[test]
    fn host_extraction_handles_userinfo_port_and_ipv6() {
        assert_eq!(
            preview_url_host("https://example.com/a?b#c").as_deref(),
            Some("example.com")
        );
        assert_eq!(
            preview_url_host("https://Example.COM:8443/x").as_deref(),
            Some("example.com")
        );
        assert_eq!(
            preview_url_host("http://user:pass@example.com/").as_deref(),
            Some("example.com")
        );
        assert_eq!(
            preview_url_host("http://127.0.0.1:8889/").as_deref(),
            Some("127.0.0.1")
        );
        assert_eq!(
            preview_url_host("http://[::1]:8080/p").as_deref(),
            Some("::1")
        );
        assert_eq!(preview_url_host("http:///nohost").as_deref(), None);
    }

    #[test]
    fn ignores_ip_literals_and_local_hosts() {
        for u in [
            "http://127.0.0.1:8889/", // the reported case
            "http://192.168.1.5/",
            "https://8.8.8.8/", // public IP literal still skipped
            "http://[::1]:8080/path",
            "http://[fe80::1]/",
            "http://localhost/",
            "https://localhost:3000/x",
            "https://dev.localhost/",
            "https://printer.local/",
            "http://svc.internal/api",
        ] {
            assert!(url_ignored_for_preview(u), "expected ignored: {u}");
        }
    }

    #[test]
    fn allows_normal_public_urls() {
        for u in [
            "https://example.com/article",
            "http://news.bbc.co.uk/story",
            "https://sub.localhost.example.com/p", // host is not a localhost
            "https://example.com:8443/page",
            "https://my-localhost-site.com/", // "localhost" only as a substring
        ] {
            assert!(!url_ignored_for_preview(u), "expected allowed: {u}");
        }
    }
}

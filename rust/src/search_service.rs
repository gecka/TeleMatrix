// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::collections::{HashMap, HashSet};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use anyhow::{anyhow, Result};
use matrix_sdk::ruma::api::client::{filter::RoomEventFilter, search::search_events};
use matrix_sdk::ruma::{OwnedRoomId, OwnedUserId, UInt};
use matrix_sdk::Client;
use serde::{Deserialize, Serialize};
use tokio::sync::RwLock;
use tracing::warn;

use crate::local_cache_service::LocalCacheService;
use crate::room_summary_service::RoomSummaryService;
use crate::types::{SearchHit, SearchPage, SearchRequest, SearchScope};

#[derive(Clone)]
pub(crate) struct SearchService {
    /// Encrypted local FTS5 search index for E2EE room messages.
    pub(crate) index: Arc<Mutex<Option<crate::search_index::SearchIndex>>>,
    /// (room_id, event_id) -> text/search metadata fingerprint last written to FTS.
    pub(crate) fingerprints: Arc<RwLock<HashMap<(String, String), u64>>>,
}

/// When false, E2EE-room search indexing is disabled: the backfill + live
/// indexers stop, the local FTS index is wiped, and encrypted-room queries
/// return a "disabled" page instead of touching the index. Default true; set
/// from the app setting via the FFI. A process-global consulted by every
/// worker/query, mirroring `cache_manager`'s media-limit global.
static E2EE_SEARCH_ENABLED: AtomicBool = AtomicBool::new(true);

pub(crate) fn set_e2ee_search_enabled(enabled: bool) {
    E2EE_SEARCH_ENABLED.store(enabled, Ordering::Relaxed);
}

pub(crate) fn e2ee_search_enabled() -> bool {
    E2EE_SEARCH_ENABLED.load(Ordering::Relaxed)
}

impl SearchService {
    pub(crate) fn new() -> Self {
        Self {
            index: Arc::new(Mutex::new(None)),
            fingerprints: Arc::new(RwLock::new(HashMap::new())),
        }
    }

    pub(crate) fn search_local(
        &self,
        ensure_search_index_open: impl FnOnce(Option<&str>),
        key_material: Option<&str>,
        request: &SearchRequest,
    ) -> Result<SearchPage> {
        ensure_search_index_open(key_material);
        let idx_guard = self
            .index
            .lock()
            .map_err(|e| anyhow!("Search index lock: {e}"))?;
        let idx = idx_guard
            .as_ref()
            .ok_or_else(|| anyhow!("Search index not available"))?;

        // Room-scoped E2EE search whose history backfill isn't finished yet: the
        // local index is still filling, so flag it (the UI shows an "indexing"
        // message so a thin/empty result reads as "still indexing", not "no hits").
        let indexing = request
            .room_id
            .as_deref()
            .map(|rid| !idx.is_backfill_done(rid).unwrap_or(true))
            .unwrap_or(false);

        let (hits, total) = idx.search(
            &request.query,
            request.room_id.as_deref(),
            request.sender_filter.as_deref(),
            50,
            0,
        )?;

        Ok(SearchPage {
            request_id: request.request_id,
            hits: hits
                .into_iter()
                .map(|h| SearchHit {
                    event_id: h.event_id,
                    room_id: h.room_id,
                    sender_id: h.sender_id,
                    sender_name: h.sender_name,
                    timestamp: h.timestamp as u64,
                    snippet: h.body,
                    rank: 0,
                    local_only: true,
                })
                .collect(),
            total_approx: total as i32,
            next_token: None,
            done: true,
            e2ee_disabled: false,
            indexing,
        })
    }

    pub(crate) async fn search_messages(
        &self,
        client: &Client,
        local_cache: &LocalCacheService,
        request: SearchRequest,
    ) -> Result<SearchPage> {
        // Global search merges the server's index (non-E2EE rooms) with the local
        // FTS index (E2EE rooms), so encrypted-room hits are included too.
        if request.scope == SearchScope::AllRooms {
            return self.search_global(client, local_cache, request).await;
        }

        // Room-scoped search of an E2EE room: the server can't read ciphertext, so
        // query the local index instead.
        if let Some(ref room_id_str) = request.room_id {
            if let Ok(room_id) = OwnedRoomId::try_from(room_id_str.as_str()) {
                if let Some(room) = client.get_room(&room_id) {
                    if room.encryption_state().is_encrypted() {
                        // Encrypted-room search is served by the local FTS index.
                        // When the user disabled E2EE search, don't open/query it
                        // (it may be deleted) — return a "disabled" page so the UI
                        // shows a message instead of a bare "no results".
                        if !e2ee_search_enabled() {
                            return Ok(SearchPage {
                                request_id: request.request_id,
                                hits: Vec::new(),
                                total_approx: 0,
                                next_token: None,
                                done: true,
                                e2ee_disabled: true,
                                indexing: false,
                            });
                        }
                        let key_material = LocalCacheService::search_index_key_material(client);
                        return self.search_local(
                            |key_material| local_cache.ensure_search_index_open(key_material),
                            key_material.as_deref(),
                            &request,
                        );
                    }
                }
            }
        }

        self.search_server(client, request).await
    }

    /// All-rooms search: run the server-side and local-index searches together,
    /// merge them by recency, and carry a composite [`GlobalSearchCursor`] so the
    /// UI can page through both sources behind a single opaque `next_token`.
    pub(crate) async fn search_global(
        &self,
        client: &Client,
        local_cache: &LocalCacheService,
        request: SearchRequest,
    ) -> Result<SearchPage> {
        let prev = GlobalSearchCursor::parse(request.next_token.as_deref());
        // Clamp the UI-supplied limit so a buggy/hostile caller can't ask the
        // local index for an unbounded number of rows in one shot.
        let page_size = (request.limit as usize).clamp(1, MAX_GLOBAL_SEARCH_PAGE);

        // The local index must be open before we read it.
        let key_material = LocalCacheService::search_index_key_material(client);
        local_cache.ensure_search_index_open(key_material.as_deref());

        // --- Server side (skip once exhausted) ---
        let server_fut = async {
            if prev.server_exhausted() {
                None
            } else {
                let mut req = request.clone();
                req.scope = SearchScope::AllRooms;
                req.room_id = None;
                req.next_token = prev.server.clone();
                Some(self.search_server(client, req).await)
            }
        };

        // --- Local side (skip once drained), off the async thread ---
        let index = self.index.clone();
        let query = request.query.clone();
        let sender = request.sender_filter.clone();
        // Skip the local (E2EE) index entirely when E2EE search is disabled, so
        // all-rooms search silently omits encrypted-room results (no error, no
        // per-room "disabled" message — that's only for room-scoped search).
        let local_drained = prev.local_done || !e2ee_search_enabled();
        // The match-count is the same for every page of a query, so only compute
        // it on the first local page and carry it forward in the cursor.
        let need_count = prev.local_total.is_none();
        let before = prev.local_before().map(|(ts, ev)| (ts, ev.to_string()));
        let local_fut = tokio::task::spawn_blocking(move || -> Result<LocalOutcome> {
            if local_drained {
                return Ok(LocalOutcome::default());
            }
            let guard = index
                .lock()
                .map_err(|e| anyhow!("Search index lock: {e}"))?;
            // A `None` guard means the index failed to open (e.g. missing key
            // material). Surface it as an error rather than a silent empty page,
            // so a concurrent server failure isn't masked as a "no results" page.
            let idx = guard
                .as_ref()
                .ok_or_else(|| anyhow!("search index unavailable"))?;
            let before_ref = before.as_ref().map(|(ts, ev)| (*ts, ev.as_str()));
            // Over-fetch by one to detect exhaustion without emitting a spurious
            // empty page when the result count is an exact multiple of page_size.
            let mut hits =
                idx.search_global(&query, sender.as_deref(), before_ref, page_size + 1)?;
            let has_more = hits.len() > page_size;
            hits.truncate(page_size);
            let total = if need_count {
                Some(idx.count_global(&query, sender.as_deref())?)
            } else {
                None
            };
            Ok(LocalOutcome {
                hits,
                total,
                has_more,
            })
        });

        let (server_res, local_res) = tokio::join!(server_fut, local_fut);

        let mut next = prev.clone();
        let mut server_hits: Vec<SearchHit> = Vec::new();
        let mut server_total: i32 = 0;
        let mut any_success = false;
        let mut last_err: Option<anyhow::Error> = None;

        match server_res {
            Some(Ok(page)) => {
                any_success = true;
                server_total = page.total_approx;
                server_hits = page.hits;
                next.server_started = true;
                next.server = page.next_token; // None => exhausted
            }
            Some(Err(e)) => {
                // Network/server failure: don't sink the whole search — keep local
                // results and stop server paging (avoids retrying a bad token).
                warn!("Global search: server side failed: {e}");
                last_err = Some(e);
                next.server_started = true;
                next.server = None;
            }
            None => {
                // Already exhausted before this page; a valid, successful state.
                any_success = true;
            }
        }

        let mut local_hits: Vec<SearchHit> = Vec::new();
        let mut local_total: i32 = 0;
        match local_res {
            Ok(Ok(outcome)) => {
                any_success = true;
                // Freshly counted on the first page, otherwise carried forward.
                local_total = outcome
                    .total
                    .map(|t| t.min(i32::MAX as u32) as i32)
                    .or(prev.local_total)
                    .unwrap_or(0);
                next.local_total = Some(local_total);
                if !outcome.has_more {
                    next.local_done = true;
                }
                if let Some(last) = outcome.hits.last() {
                    next.local_ts = Some(last.timestamp);
                    next.local_event = Some(last.event_id.clone());
                }
                local_hits = outcome
                    .hits
                    .into_iter()
                    .map(|h| SearchHit {
                        event_id: h.event_id,
                        room_id: h.room_id,
                        sender_id: h.sender_id,
                        sender_name: h.sender_name,
                        timestamp: h.timestamp as u64,
                        snippet: h.body,
                        rank: 0,
                        local_only: true,
                    })
                    .collect();
            }
            Ok(Err(e)) => {
                warn!("Global search: local side failed: {e}");
                last_err = Some(e);
                // Stop local paging so the search can terminate rather than loop.
                next.local_done = true;
            }
            Err(join_err) => {
                warn!("Global search: local task panicked: {join_err}");
                last_err = Some(anyhow!("local search task failed: {join_err}"));
                next.local_done = true;
            }
        }

        if !any_success {
            return Err(last_err.unwrap_or_else(|| anyhow!("search failed")));
        }

        let hits = merge_hits(server_hits, local_hits);
        let total_approx = server_total.saturating_add(local_total);

        Ok(SearchPage {
            request_id: request.request_id,
            hits,
            total_approx,
            done: next.done(),
            next_token: next.to_token(),
            e2ee_disabled: false,
            indexing: false,
        })
    }

    pub(crate) async fn search_server(
        &self,
        client: &Client,
        request: SearchRequest,
    ) -> Result<SearchPage> {
        let mut criteria = search_events::v3::Criteria::new(request.query.clone());
        criteria.keys = Some(vec![search_events::v3::SearchKeys::ContentBody]);
        criteria.order_by = Some(search_events::v3::OrderBy::Recent);

        let mut filter = RoomEventFilter::default();
        // Don't set filter.limit -- it limits events scanned, not results returned.
        // Don't set filter.types -- let server search all event types.

        if request.scope == SearchScope::Room {
            let room_id = request
                .room_id
                .as_deref()
                .ok_or_else(|| anyhow!("Room-scoped search requires a room ID"))?;
            let room_id = OwnedRoomId::try_from(room_id)
                .map_err(|_| anyhow!("Invalid room ID for search: {room_id}"))?;
            filter.rooms = Some(vec![room_id]);
        }
        if let Some(sender) = request.sender_filter.as_deref() {
            let sender = OwnedUserId::try_from(sender)
                .map_err(|_| anyhow!("Invalid sender filter for search: {sender}"))?;
            filter.senders = Some(vec![sender]);
        }
        criteria.filter = filter;

        let mut context = search_events::v3::EventContext::new();
        context.before_limit =
            UInt::new(0).ok_or_else(|| anyhow!("Invalid Matrix search before limit"))?;
        context.after_limit =
            UInt::new(0).ok_or_else(|| anyhow!("Invalid Matrix search after limit"))?;
        context.include_profile = true;
        criteria.event_context = context;

        let mut categories = search_events::v3::Categories::new();
        categories.room_events = Some(criteria);

        let mut search_request = search_events::v3::Request::new(categories);
        search_request.next_batch = request.next_token.clone();

        let response = client.send(search_request).await.map_err(|e| {
            warn!("Matrix search failed: {e}");
            anyhow!("Matrix search failed: {e}")
        })?;
        let room_events = response.search_categories.room_events;

        let hits = room_events
            .results
            .into_iter()
            .filter_map(|result| {
                let raw = result.result?;
                let event: matrix_sdk::ruma::events::AnyTimelineEvent = raw.deserialize().ok()?;
                let sender_id = event.sender().to_string();
                let sender_name = result
                    .context
                    .profile_info
                    .get(event.sender())
                    .and_then(|profile| profile.displayname.clone())
                    .unwrap_or_else(|| sender_id.clone());
                let snippet = RoomSummaryService::extract_event_body(
                    &matrix_sdk::ruma::events::AnySyncTimelineEvent::from(event.clone()),
                )
                .unwrap_or_default();
                Some(SearchHit {
                    room_id: event.room_id().to_string(),
                    event_id: event.event_id().to_string(),
                    sender_id,
                    sender_name,
                    timestamp: event.origin_server_ts().as_secs().into(),
                    snippet,
                    rank: result.rank.unwrap_or_default().round() as i32,
                    local_only: false,
                })
            })
            .collect::<Vec<_>>();

        let total_approx = room_events
            .count
            .map(|count| {
                let count_u64: u64 = count.into();
                count_u64.min(i32::MAX as u64) as i32
            })
            .unwrap_or_else(|| hits.len().min(i32::MAX as usize) as i32);
        let next_token = room_events.next_batch.clone();

        Ok(SearchPage {
            request_id: request.request_id,
            hits,
            total_approx,
            next_token,
            done: room_events.next_batch.is_none(),
            e2ee_disabled: false,
            indexing: false,
        })
    }
}

impl Default for SearchService {
    fn default() -> Self {
        Self::new()
    }
}

/// Upper bound on how many rows the local index is asked for per page, regardless
/// of the limit the UI requests, to bound per-call memory.
const MAX_GLOBAL_SEARCH_PAGE: usize = 200;

/// Result of one local-index page fetch inside the blocking task.
#[derive(Default)]
struct LocalOutcome {
    hits: Vec<crate::search_index::SearchHit>,
    /// Total match count, present only when (re)computed this page.
    total: Option<u32>,
    /// Whether more local rows exist beyond this page (from the N+1 over-fetch).
    has_more: bool,
}

/// Opaque continuation cursor for global (all-rooms) search.
///
/// Global search merges two independently-paginated sources — the homeserver's
/// `/search` (token-based, encrypted rooms invisible to it) and the local FTS
/// index (keyset-based, E2EE rooms only). This struct tracks both cursors and is
/// serialized into the `next_token` the UI round-trips back to us. The UI treats
/// it as an opaque string.
#[derive(Debug, Default, Clone, PartialEq, Eq, Serialize, Deserialize)]
struct GlobalSearchCursor {
    /// Server-side `next_batch`; meaningful only once `server_started` is true.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    server: Option<String>,
    /// Whether a server page was already fetched. Distinguishes the fresh state
    /// (`server == None`, must still query) from exhaustion (`server == None`).
    #[serde(default)]
    server_started: bool,
    /// Local keyset upper bound (exclusive): the last emitted local hit's timestamp.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    local_ts: Option<i64>,
    /// Local keyset upper bound (exclusive): the last emitted local hit's event id.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    local_event: Option<String>,
    /// Whether the local index has been fully drained for this query.
    #[serde(default)]
    local_done: bool,
    /// Local match count, cached after the first page so we don't recount on every
    /// "load more".
    #[serde(default, skip_serializing_if = "Option::is_none")]
    local_total: Option<i32>,
}

impl GlobalSearchCursor {
    /// Parse an incoming token. A missing/empty/garbage token yields the fresh
    /// state (start both sources from the newest results).
    fn parse(token: Option<&str>) -> Self {
        match token {
            Some(t) if !t.is_empty() => serde_json::from_str(t).unwrap_or_default(),
            _ => Self::default(),
        }
    }

    fn server_exhausted(&self) -> bool {
        self.server_started && self.server.is_none()
    }

    fn local_before(&self) -> Option<(i64, &str)> {
        match (self.local_ts, self.local_event.as_deref()) {
            (Some(ts), Some(ev)) => Some((ts, ev)),
            _ => None,
        }
    }

    fn done(&self) -> bool {
        self.server_exhausted() && self.local_done
    }

    /// Serialize to an opaque token, or `None` once both sources are exhausted so
    /// the UI (which treats an empty token as "no more pages") stops paginating.
    fn to_token(&self) -> Option<String> {
        if self.done() {
            None
        } else {
            serde_json::to_string(self).ok()
        }
    }
}

/// Merge server and local hits into one recency-ordered page.
///
/// Deduplicates by `event_id` (the server's copy wins, since it carries rank and
/// profile context), then orders by `(timestamp DESC, event_id DESC)` to match
/// both the server's `OrderBy::Recent` and the local index's keyset order.
///
/// Note: dedup is WITHIN a page only. The two sources page independently, so a
/// room that switched encryption on mid-history (its plaintext-era events are
/// both server-visible and locally indexed) can in principle yield the same event
/// on different pages. This is the documented known limitation, accepted to keep
/// the merge stateless across the opaque `next_token`.
fn merge_hits(mut server: Vec<SearchHit>, local: Vec<SearchHit>) -> Vec<SearchHit> {
    let seen: HashSet<&str> = server.iter().map(|h| h.event_id.as_str()).collect();
    let extra: Vec<SearchHit> = local
        .into_iter()
        .filter(|h| !seen.contains(h.event_id.as_str()))
        .collect();
    server.extend(extra);
    server.sort_by(|a, b| {
        b.timestamp
            .cmp(&a.timestamp)
            .then_with(|| b.event_id.cmp(&a.event_id))
    });
    server
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hit(event_id: &str, ts: u64, local_only: bool) -> SearchHit {
        SearchHit {
            room_id: "!r:s".to_string(),
            event_id: event_id.to_string(),
            sender_id: "@a:s".to_string(),
            sender_name: "a".to_string(),
            timestamp: ts,
            snippet: if local_only { "local" } else { "server" }.to_string(),
            rank: 0,
            local_only,
        }
    }

    #[test]
    fn merge_orders_by_timestamp_desc() {
        let server = vec![hit("$s1", 100, false)];
        let local = vec![hit("$l1", 300, true), hit("$l2", 200, true)];
        let merged = merge_hits(server, local);
        let ids: Vec<&str> = merged.iter().map(|h| h.event_id.as_str()).collect();
        assert_eq!(ids, vec!["$l1", "$l2", "$s1"]);
    }

    #[test]
    fn merge_dedups_by_event_id_server_wins() {
        // Same event indexed locally and returned by the server (a room that
        // enabled encryption mid-history). It must appear once, server's copy.
        let server = vec![hit("$dup", 200, false)];
        let local = vec![hit("$dup", 200, true), hit("$l1", 100, true)];
        let merged = merge_hits(server, local);
        assert_eq!(merged.len(), 2);
        let dup = merged.iter().find(|h| h.event_id == "$dup").unwrap();
        assert!(!dup.local_only, "server copy should win dedup");
    }

    #[test]
    fn merge_breaks_ties_by_event_id_desc() {
        let server = vec![hit("$a", 500, false)];
        let local = vec![hit("$b", 500, true)];
        let merged = merge_hits(server, local);
        let ids: Vec<&str> = merged.iter().map(|h| h.event_id.as_str()).collect();
        assert_eq!(ids, vec!["$b", "$a"]);
    }

    #[test]
    fn cursor_fresh_token_is_not_exhausted() {
        let c = GlobalSearchCursor::parse(None);
        assert!(!c.server_exhausted());
        assert!(!c.done());
        assert_eq!(c.local_before(), None);
    }

    #[test]
    fn cursor_round_trips_through_token() {
        let c = GlobalSearchCursor {
            server: Some("batch-2".to_string()),
            server_started: true,
            local_ts: Some(1234),
            local_event: Some("$e9".to_string()),
            local_done: false,
            local_total: Some(42),
        };
        let token = c.to_token().expect("not done -> some token");
        let parsed = GlobalSearchCursor::parse(Some(&token));
        assert_eq!(parsed, c);
        assert_eq!(parsed.local_before(), Some((1234, "$e9")));
    }

    #[test]
    fn cursor_exhausted_when_server_started_with_no_batch_and_local_done() {
        let c = GlobalSearchCursor {
            server: None,
            server_started: true,
            local_ts: None,
            local_event: None,
            local_done: true,
            local_total: Some(0),
        };
        assert!(c.server_exhausted());
        assert!(c.done());
        assert_eq!(c.to_token(), None, "exhausted cursor yields no token");
    }

    #[test]
    fn cursor_not_done_when_only_one_source_exhausted() {
        let server_left = GlobalSearchCursor {
            server: Some("batch-2".to_string()),
            server_started: true,
            local_done: true,
            ..Default::default()
        };
        assert!(!server_left.done());
        assert!(server_left.to_token().is_some());

        let local_left = GlobalSearchCursor {
            server: None,
            server_started: true,
            local_done: false,
            ..Default::default()
        };
        assert!(!local_left.done());
        assert!(local_left.to_token().is_some());
    }
}

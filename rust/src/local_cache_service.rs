// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use anyhow::{anyhow, Result};
use matrix_sdk::Client;
use tokio::sync::{Notify, RwLock};
use tracing::{info, warn};

use crate::session_storage_service::SessionStorageService;
use crate::types::{RoomSummary, UrlPreview};

/// Diagnostics for the rooms-snapshot writer (see run_snapshot_writer).
static SNAPSHOT_WRITES: AtomicU64 = AtomicU64::new(0);
static SNAPSHOT_MS_TOTAL: AtomicU64 = AtomicU64::new(0);

#[derive(Clone)]
pub(crate) struct LocalCacheService {
    data_dir: PathBuf,
    runtime_handle: tokio::runtime::Handle,
    app_cache_store: Arc<Mutex<Option<crate::app_cache_store::AppCacheStore>>>,
    preview_cache: Arc<RwLock<HashMap<String, Option<UrlPreview>>>>,
    preview_store: Arc<Mutex<Option<crate::preview_store::PreviewStore>>>,
    search_index: Arc<Mutex<Option<crate::search_index::SearchIndex>>>,
    /// The canonical in-memory rooms list. The snapshot writer reads it at write
    /// time, so a persist request carries no data — it just marks this dirty.
    rooms_cache: Arc<RwLock<Vec<RoomSummary>>>,
    snapshot_dirty: Arc<AtomicBool>,
    snapshot_notify: Arc<Notify>,
}

impl LocalCacheService {
    pub(crate) fn new(
        data_dir: PathBuf,
        runtime_handle: tokio::runtime::Handle,
        app_cache_store: Arc<Mutex<Option<crate::app_cache_store::AppCacheStore>>>,
        preview_cache: Arc<RwLock<HashMap<String, Option<UrlPreview>>>>,
        preview_store: Arc<Mutex<Option<crate::preview_store::PreviewStore>>>,
        search_index: Arc<Mutex<Option<crate::search_index::SearchIndex>>>,
        rooms_cache: Arc<RwLock<Vec<RoomSummary>>>,
    ) -> Self {
        Self {
            data_dir,
            runtime_handle,
            app_cache_store,
            preview_cache,
            preview_store,
            search_index,
            rooms_cache,
            snapshot_dirty: Arc::new(AtomicBool::new(false)),
            snapshot_notify: Arc::new(Notify::new()),
        }
    }

    pub(crate) fn app_cache_store(
        &self,
    ) -> Arc<Mutex<Option<crate::app_cache_store::AppCacheStore>>> {
        self.app_cache_store.clone()
    }

    pub(crate) fn open_from_local_secrets(&self) {
        let dir_ns = crate::session_storage_service::dir_namespace(&self.data_dir);
        if let Some(key) = SessionStorageService::load_local_secret(&dir_ns, "app_cache_passphrase")
        {
            if let Err(err) = self.open_app_cache_store(key.as_str()) {
                warn!("Encrypted app cache unavailable: {err}");
            }
        }
        if let Some(key) =
            SessionStorageService::load_local_secret(&dir_ns, "preview_cache_passphrase")
        {
            if let Err(err) = self.open_preview_store(key.as_str()) {
                warn!("Encrypted preview cache unavailable: {err}");
            }
        }
    }

    pub(crate) fn ensure_local_cache_stores_open(
        &self,
        app_cache_passphrase: &str,
        preview_cache_passphrase: &str,
    ) -> Result<()> {
        self.open_app_cache_store(app_cache_passphrase)?;
        self.open_preview_store(preview_cache_passphrase)?;
        Ok(())
    }

    /// Request that the rooms snapshot be persisted. O(1), non-blocking, no SQLite:
    /// it marks the shared rooms cache dirty and wakes the writer. A burst of these
    /// (one per room at cold start) coalesces into a single whole-list write of the
    /// latest state — see [`run_snapshot_writer`]. The caller must already have
    /// applied its change to `rooms_cache`; the writer reads that, not a payload.
    ///
    /// This replaced a per-call synchronous rusqlite whole-list write (DELETE-all +
    /// INSERT-all behind a std Mutex) that pinned a tokio worker for its full
    /// duration on every room update — the same executor-starving anti-pattern as
    /// the live FTS `index_batch`.
    pub(crate) fn schedule_rooms_snapshot(&self) {
        self.snapshot_dirty.store(true, Ordering::Release);
        self.snapshot_notify.notify_one();
    }

    /// The single rooms-snapshot writer. Spawn once per session via `session_tasks`
    /// so logout's generation-abort tears it down before the store is wiped — a
    /// long-lived writer could otherwise read one session's rooms and write them
    /// into the next session's freshly-opened store.
    ///
    /// Coalescing: it reads the canonical `rooms_cache` at write time, so any number
    /// of `schedule_rooms_snapshot` calls between two writes collapse to one write of
    /// the freshest list, and no caller can persist a stale list (there is no queued
    /// payload to lose a race). The debounce batches a cold-start burst — persistence
    /// lags a change by up to that window, invisible for a self-healing startup cache
    /// that the next sync rewrites.
    pub(crate) async fn run_snapshot_writer(self) {
        const DEBOUNCE: std::time::Duration = std::time::Duration::from_millis(500);
        loop {
            // Skip the wait when a change is already pending — one queued before this
            // writer spawned (prepopulate at restore) or carried on the shared dirty
            // flag across a generation restart, whose notify permit this new task
            // never received. A concurrent schedule between the load and the wait
            // still wakes us via the stored permit, so this only ever flushes sooner.
            if !self.snapshot_dirty.load(Ordering::Acquire) {
                self.snapshot_notify.notified().await;
            }
            tokio::time::sleep(DEBOUNCE).await;
            // Swap the flag before the read so a change landing during the write
            // re-arms us (its notify_one stores a permit, so the next notified()
            // returns immediately and we write again).
            if !self.snapshot_dirty.swap(false, Ordering::AcqRel) {
                continue;
            }
            let rooms = self.rooms_cache.read().await.clone();
            let store = self.app_cache_store.clone();
            let count = rooms.len();
            let t0 = std::time::Instant::now();
            let _ = tokio::task::spawn_blocking(move || {
                let Ok(guard) = store.lock() else {
                    warn!("App cache store lock poisoned while saving rooms snapshot");
                    return;
                };
                let Some(store) = guard.as_ref() else {
                    return; // Store closed (logged out / not yet open) — skip.
                };
                if let Err(err) = store.save_rooms(&rooms) {
                    warn!("Failed to save rooms snapshot to app cache: {err}");
                }
            })
            .await;
            let ms = t0.elapsed().as_millis() as u64;
            let n = SNAPSHOT_WRITES.fetch_add(1, Ordering::Relaxed) + 1;
            let total = SNAPSHOT_MS_TOTAL.fetch_add(ms, Ordering::Relaxed) + ms;
            if ms > 20 {
                tracing::info!(
                    rooms = count,
                    ms,
                    writes = n,
                    total_ms = total,
                    "rooms snapshot: coalesced whole-list write"
                );
            }
        }
    }

    /// Load the persisted rooms snapshot (empty if unavailable). Used to seed the in-memory cache on
    /// the first rebuild after a restart so `merge_sticky_previews` has a prior to restore previews
    /// from (rooms whose newest event is a system event carry no fresh preview).
    pub(crate) fn load_rooms_snapshot(&self) -> Vec<RoomSummary> {
        let Ok(guard) = self.app_cache_store.lock() else {
            warn!("App cache store lock poisoned while loading rooms snapshot");
            return Vec::new();
        };
        let Some(store) = guard.as_ref() else {
            return Vec::new();
        };
        store.load_rooms().unwrap_or_else(|err| {
            warn!("Failed to load rooms snapshot from app cache: {err}");
            Vec::new()
        })
    }

    pub(crate) fn search_index_key_material(client: &Client) -> Option<String> {
        SessionStorageService::search_index_key_material(client)
    }

    pub(crate) async fn cache_stats(&self) -> crate::cache_manager::CacheStats {
        crate::cache_manager::calculate_stats(&self.data_dir).await
    }

    pub(crate) fn ensure_search_index_open(&self, key_material: Option<&str>) {
        let mut guard = match self.search_index.lock() {
            Ok(guard) => guard,
            Err(e) => {
                warn!("Search index lock poisoned: {e}");
                return;
            }
        };
        if guard.is_some() {
            return;
        }
        match crate::search_index::SearchIndex::open(&self.data_dir, key_material) {
            Ok(idx) => {
                *guard = Some(idx);
            }
            Err(e) => {
                warn!("Failed to open search index: {e}");
            }
        }
    }

    fn open_app_cache_store(&self, key_material: &str) -> Result<()> {
        let mut guard = self
            .app_cache_store
            .lock()
            .map_err(|_| anyhow!("App cache store lock poisoned"))?;
        if guard.is_some() {
            return Ok(());
        }
        *guard = Some(crate::app_cache_store::AppCacheStore::open(
            &self.data_dir,
            key_material,
        )?);
        Ok(())
    }

    fn open_preview_store(&self, key_material: &str) -> Result<()> {
        let mut guard = self
            .preview_store
            .lock()
            .map_err(|_| anyhow!("Preview store lock poisoned"))?;
        if guard.is_some() {
            return Ok(());
        }
        let store = crate::preview_store::PreviewStore::open(&self.data_dir, key_material)?;
        let _ = store.delete_expired(30);
        // One-time heal for URLs the old extractor mangled by dropping a balanced
        // trailing ')' (see delete_unbalanced_paren_negatives).
        let _ = store.delete_unbalanced_paren_negatives();
        if let Ok(loaded) = store.load_all() {
            if !loaded.is_empty() {
                info!("Loaded {} cached URL previews", loaded.len());
                let pc = self.preview_cache.clone();
                self.runtime_handle.spawn(async move {
                    let mut cache = pc.write().await;
                    *cache = loaded;
                });
            }
        }
        *guard = Some(store);
        Ok(())
    }
}

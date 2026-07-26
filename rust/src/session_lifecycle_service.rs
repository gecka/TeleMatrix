// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::path::{Path, PathBuf};
use std::sync::{Mutex, MutexGuard};
use std::time::Duration;

use anyhow::Result;
use matrix_sdk::Client;
use tracing::{error, info, warn};

use crate::local_cache_service::LocalCacheService;
use crate::media_transfer_service::MediaTransferService;
use crate::presence_typing_service::PresenceTypingService;
use crate::preview_service::PreviewService;
use crate::room_list_service::RoomListService;
use crate::room_summary_service::RoomSummaryService;
use crate::search_service::SearchService;
use crate::session_storage_service::SessionStorageService;
use crate::timeline_service::TimelineService;
use crate::types::UserProfile;

fn lock_session_mutex<'a, T>(mutex: &'a Mutex<T>, name: &str) -> MutexGuard<'a, T> {
    match mutex.lock() {
        Ok(guard) => guard,
        Err(poisoned) => {
            warn!("Recovering poisoned session lifecycle mutex: {name}");
            poisoned.into_inner()
        }
    }
}

#[derive(Clone)]
pub(crate) struct SessionLifecycleService {
    data_dir: PathBuf,
    local_cache: LocalCacheService,
    media_transfer: MediaTransferService,
    session_storage: SessionStorageService,
    room_list: RoomListService,
    timeline: TimelineService,
    preview: PreviewService,
    search: SearchService,
    presence_typing: PresenceTypingService,
}

pub(crate) struct SessionLifecycleComponents {
    pub(crate) data_dir: PathBuf,
    pub(crate) local_cache: LocalCacheService,
    pub(crate) media_transfer: MediaTransferService,
    pub(crate) session_storage: SessionStorageService,
    pub(crate) room_list: RoomListService,
    pub(crate) timeline: TimelineService,
    pub(crate) preview: PreviewService,
    pub(crate) search: SearchService,
    pub(crate) presence_typing: PresenceTypingService,
}

impl SessionLifecycleService {
    pub(crate) fn new(components: SessionLifecycleComponents) -> Self {
        Self {
            data_dir: components.data_dir,
            local_cache: components.local_cache,
            media_transfer: components.media_transfer,
            session_storage: components.session_storage,
            room_list: components.room_list,
            timeline: components.timeline,
            preview: components.preview,
            search: components.search,
            presence_typing: components.presence_typing,
        }
    }

    /// Delete the SDK store directory and verify it is actually gone.
    ///
    /// A single `remove_dir_all` is not enough: a straggler task holding a
    /// live sqlite connection can recreate `-wal`/`-shm` files between the
    /// unlink pass and the final rmdir (ENOTEMPTY), and a stale orphan WAL
    /// left next to a freshly created database gets replayed into it by
    /// sqlite — resurrecting old, differently-encrypted values inside a
    /// "fresh" store (aead::Error on read).
    pub(crate) async fn wipe_store_dir_verified(&self) -> Result<()> {
        let store_path = self.data_dir.join("store");
        let _store_lock = crate::store_guard::lock_store_dir(&self.data_dir).await;
        self.wipe_store_dir_inner(&store_path).await
    }

    /// Verified in-place wipe of `store_path` (the 8-attempt loop). The caller
    /// MUST already hold the store-dir lock (`store_guard::lock_store_dir`);
    /// this does not re-acquire it, so it can run under a lock already held by
    /// the rename-aside path without deadlocking.
    async fn wipe_store_dir_inner(&self, store_path: &Path) -> Result<()> {
        if !store_path.exists() {
            info!("SDK store directory already absent; nothing to wipe");
            return Ok(());
        }
        for attempt in 1..=8u32 {
            if let Err(e) = tokio::fs::remove_dir_all(store_path).await {
                if e.kind() == std::io::ErrorKind::NotFound {
                    info!("SDK store directory wiped (attempt {attempt})");
                    return Ok(());
                }
                warn!("SDK store wipe attempt {attempt} failed: {e}");
            }
            if !store_path.exists() {
                info!("SDK store directory wiped (attempt {attempt})");
                return Ok(());
            }
            // Give straggler sqlite writers a beat before retrying.
            tokio::time::sleep(Duration::from_millis(150)).await;
        }
        let mut survivors = Vec::new();
        if let Ok(mut entries) = tokio::fs::read_dir(store_path).await {
            while let Ok(Some(entry)) = entries.next_entry().await {
                survivors.push(entry.file_name().to_string_lossy().into_owned());
            }
        }
        error!("SDK store wipe FAILED after 8 attempts; surviving entries: {survivors:?}");
        Err(anyhow::anyhow!(
            "SDK store wipe failed after 8 attempts; surviving entries: {survivors:?}"
        ))
    }

    /// Move every local store aside into a fresh `.trash/<unique>/` directory
    /// with atomic renames (O(1) regardless of size), freeing the canonical
    /// paths instantly so the next login starts clean. The renamed-aside data
    /// is unreadable without the keychain keys (cleared earlier in logout), so
    /// the actual recursive delete is left to a background thread / startup
    /// sweep. Returns the trash directory to delete, or `None` if a trash dir
    /// could not be created and the stores were deleted in place instead.
    ///
    /// Holds the store-dir lock across the whole batch so a concurrent
    /// fresh-login build cannot open `store/` mid-rename.
    async fn move_stores_to_trash(&self) -> Result<Option<PathBuf>> {
        let _store_lock = crate::store_guard::lock_store_dir(&self.data_dir).await;
        let trash = match crate::trash::new_trash_subdir(&self.data_dir) {
            Ok(dir) => Some(dir),
            Err(e) => {
                warn!("logout: cannot create trash dir ({e}); deleting stores in place");
                None
            }
        };
        let trash_ref = trash.as_deref();

        self.relocate_or_delete(&self.data_dir.join("store"), trash_ref)
            .await;
        self.relocate_or_delete(&self.data_dir.join("media_cache"), trash_ref)
            .await;
        for name in ["preview_cache.db", "search_index.db", "app_cache.db"] {
            for suffix in ["", "-wal", "-shm"] {
                let file = self.data_dir.join(format!("{name}{suffix}"));
                self.relocate_or_delete(&file, trash_ref).await;
            }
        }

        // Correctness gate: the next fresh login reopens `store/` at the
        // canonical path; a surviving `-wal`/`-shm` there is replayed by sqlite
        // into the new, differently-encrypted store (aead::Error). If both the
        // rename and the in-place fallback failed, run the verified in-place
        // wipe and surface its failure so logout cannot report success on a
        // damaged store.
        let store_path = self.data_dir.join("store");
        if store_path.exists() {
            self.wipe_store_dir_inner(&store_path).await?;
        }
        Ok(trash)
    }

    /// Rename `src` into `trash` (preferred), falling back to an in-place
    /// recursive delete when there is no trash dir or the rename fails (a
    /// same-filesystem rename should not fail). A missing `src` is a no-op.
    async fn relocate_or_delete(&self, src: &Path, trash: Option<&Path>) {
        if !src.exists() {
            return;
        }
        if let (Some(trash), Some(name)) = (trash, src.file_name()) {
            match tokio::fs::rename(src, trash.join(name)).await {
                Ok(()) => return,
                Err(e) => warn!(
                    "logout: rename {} aside failed ({e}); deleting in place",
                    src.display()
                ),
            }
        }
        let result = if src.is_dir() {
            tokio::fs::remove_dir_all(src).await
        } else {
            tokio::fs::remove_file(src).await
        };
        if let Err(e) = result {
            if e.kind() != std::io::ErrorKind::NotFound {
                warn!("logout: in-place delete of {} failed: {e}", src.display());
            }
        }
    }

    /// `previous_owner` is the user id this context last held, if any, so its
    /// profile memo is dropped before the new session populates its own.
    pub(crate) async fn prepare_fresh_login(&self, previous_owner: &str) -> Result<()> {
        // Clear any existing store -- a fresh login creates a new device ID
        // which would conflict with crypto store data from a previous session.
        // Fail the login rather than build a new session on a non-wiped store.
        self.wipe_store_dir_verified().await?;
        self.clear_media_sources();
        self.reset_local_cache_stores_for_new_session(previous_owner)
            .await;
        if let Ok(mut idx) = self.search.index.lock() {
            *idx = None;
        }
        Ok(())
    }

    pub(crate) fn clear_media_sources(&self) {
        if let Ok(mut media_sources) = self.timeline.media_sources.write() {
            media_sources.clear();
        }
    }

    pub(crate) async fn reset_local_cache_stores_for_new_session(&self, previous_owner: &str) {
        let app_cache_store = self.local_cache.app_cache_store();
        if let Ok(mut guard) = app_cache_store.lock() {
            *guard = None;
        }
        if let Ok(mut guard) = self.preview.store.lock() {
            *guard = None;
        }
        {
            let mut pc = self.preview.cache.write().await;
            pc.clear();
        }
        {
            let mut updates = self.timeline.update_cache.write().await;
            updates.clear();
        }
        {
            let mut avatars = self.timeline.sender_avatar_cache.write().await;
            avatars.clear();
        }
        // Process-wide static: without this the next account serves display names and
        // avatars fetched by the previous one (and inherits its cached absences).
        crate::timeline_cache_service::clear_global_profile_cache(previous_owner).await;
        {
            let mut fingerprints = self.search.fingerprints.write().await;
            fingerprints.clear();
        }
        if let Ok(mut guard) = self.search.index.lock() {
            *guard = None;
            // Delete the search index DB while still holding the lock so the
            // sync loop's ensure_search_index_open() cannot reopen+recreate it
            // in the gap (it would then be unlinked under the new connection ->
            // SQLITE_READONLY_DBMOVED on the next insert).
            let _ = crate::encrypted_sqlite::delete_database_files(
                &self.data_dir.join("search_index.db"),
            );
        }
        for name in ["app_cache.db", "preview_cache.db"] {
            let path = self.data_dir.join(name);
            let _ = crate::encrypted_sqlite::delete_database_files(&path);
        }
        self.media_transfer.delete_cache_dir().await;
    }

    pub(crate) fn prepare_restore_session(
        &self,
        homeserver: &str,
        user_id: &str,
        device_id: &str,
    ) -> Result<String> {
        let store_passphrase = SessionStorageService::load_session_secret(
            "sdk_store_passphrase",
            homeserver,
            user_id,
            device_id,
        )?;
        let dir_ns = crate::session_storage_service::dir_namespace(&self.data_dir);
        let app_cache_passphrase =
            SessionStorageService::load_required_local_secret(&dir_ns, "app_cache_passphrase")?;
        let preview_cache_passphrase =
            SessionStorageService::load_required_local_secret(&dir_ns, "preview_cache_passphrase")?;
        self.media_transfer.require_cache_key()?;
        self.local_cache
            .ensure_local_cache_stores_open(&app_cache_passphrase, &preview_cache_passphrase)?;
        Ok(store_passphrase)
    }

    pub(crate) async fn prepopulate_restored_rooms(&self, client: &Client) {
        let no = self.room_list.notification_overrides.read().await;
        let presence_snap = self.presence_typing.presence_snapshot();
        let prepopulate_started = std::time::Instant::now();
        match RoomSummaryService::build_rooms_cache(
            client,
            &self.timeline.cache,
            &no,
            &presence_snap,
        )
        .await
        {
            Ok(mut rooms) => {
                info!(
                    "Pre-populated {} rooms from local store in {:?}",
                    rooms.len(),
                    prepopulate_started.elapsed()
                );
                if !rooms.is_empty() {
                    {
                        let mut cache = self.room_list.rooms.write().await;
                        RoomSummaryService::merge_sticky_previews(&mut rooms, cache.as_slice());
                        *cache = rooms;
                    }
                    self.local_cache.schedule_rooms_snapshot();
                }
            }
            Err(e) => {
                warn!("Failed to pre-populate rooms from local store: {e}");
            }
        }
        // Notify UI so room list appears immediately.
        let cb = lock_session_mutex(&self.room_list.callback, "room_list_callback");
        if let Some(ref f) = *cb {
            f();
        }
    }

    pub(crate) async fn restored_profile(&self, client: &Client, user_id: String) -> UserProfile {
        // Profile fetches are non-critical -- use short timeout, fall back to user ID.
        let display_name =
            tokio::time::timeout(Duration::from_secs(5), client.account().get_display_name())
                .await
                .ok()
                .and_then(|r| r.ok())
                .flatten()
                .unwrap_or_else(|| user_id.clone());
        let avatar_url =
            tokio::time::timeout(Duration::from_secs(5), client.account().get_avatar_url())
                .await
                .ok()
                .and_then(|r| r.ok())
                .flatten()
                .map(|u| u.to_string());

        UserProfile {
            user_id,
            display_name,
            avatar_url,
        }
    }

    /// `owner` is the departing account's user id (captured before the client is
    /// taken), so only its profile memo is dropped — other live accounts keep theirs.
    pub(crate) async fn clear_runtime_state_for_logout(&self, owner: &str) {
        {
            let mut rooms = self.room_list.rooms.write().await;
            rooms.clear();
        }
        {
            let mut tl = self.timeline.cache.write().await;
            tl.clear();
        }
        {
            let mut updates = self.timeline.update_cache.write().await;
            updates.clear();
        }
        {
            let mut states =
                lock_session_mutex(&self.timeline.refresh_states, "timeline_update_states");
            states.clear();
        }
        {
            let mut pending = self.timeline.pending_reaction_overrides.write().await;
            pending.clear();
        }
        {
            let mut reply_previews = self.timeline.reply_preview_cache.write().await;
            reply_previews.clear();
        }
        {
            let mut wins = self.timeline.windows.write().await;
            wins.clear();
        }
        {
            let mut timelines = self.timeline.timelines.write().await;
            timelines.clear();
        }
        self.clear_media_sources();
        crate::upload_seed_store::clear();
        {
            let mut pc = self.preview.cache.write().await;
            pc.clear();
        }
        {
            let mut avatars = self.timeline.sender_avatar_cache.write().await;
            avatars.clear();
        }
        // Process-wide static: logout must not leave the next account serving the
        // previous one's display names / avatars (or its cached absences).
        crate::timeline_cache_service::clear_global_profile_cache(owner).await;
        {
            let mut fingerprints = self.search.fingerprints.write().await;
            fingerprints.clear();
        }
        if let Ok(store) = self.preview.store.lock() {
            if let Some(store) = store.as_ref() {
                let _ = store.delete_all();
            }
        }
        if let Ok(idx_guard) = self.search.index.lock() {
            if let Some(ref idx) = *idx_guard {
                let _ = idx.clear_all();
            }
        }
        let app_cache_store = self.local_cache.app_cache_store();
        if let Ok(guard) = app_cache_store.lock() {
            if let Some(store) = guard.as_ref() {
                let _ = store.clear_all();
            }
        };
    }

    pub(crate) async fn logout_remote_best_effort(&self, client: &Client) {
        match tokio::time::timeout(Duration::from_secs(3), client.matrix_auth().logout()).await {
            Ok(Ok(_)) => {}
            Ok(Err(e)) => {
                warn!("Server logout failed, continuing local logout cleanup: {e}");
            }
            Err(_) => {
                warn!("Server logout timed out, continuing local logout cleanup");
            }
        }
    }

    pub(crate) async fn finish_logout_local_cleanup(&self) -> Result<()> {
        if let Ok(mut guard) = self.preview.store.lock() {
            *guard = None;
        }
        let app_cache_store = self.local_cache.app_cache_store();
        if let Ok(mut guard) = app_cache_store.lock() {
            *guard = None;
        }
        if let Ok(mut guard) = self.search.index.lock() {
            *guard = None;
        }
        self.session_storage.clear_pending_auth_store_passphrase();

        // Move every local store aside with atomic renames (O(1) regardless of
        // size) so logout finishes promptly and the canonical paths are clear
        // for the next login. The encrypted data left behind is unreadable
        // without the keychain keys (cleared earlier in logout); reclaim it
        // off the critical path.
        let trash = self.move_stores_to_trash().await?;

        // Plaintext temp media is the only unencrypted sensitive data and lives
        // outside the data dir (not in `.trash`), so scrub it synchronously now.
        self.media_transfer.cleanup_plaintext();

        if let Some(trash) = trash {
            crate::trash::spawn_background_delete(trash);
        }
        Ok(())
    }

    pub(crate) async fn clear_cache_data(&self) -> u64 {
        let stats_before = crate::cache_manager::calculate_stats(&self.data_dir).await;

        // The E2EE search index is intentionally NOT cleared here: it's the only
        // source of E2EE-room search, small relative to media, and rebuilding it
        // needs a slow throttled re-backfill. "Clear local cache" frees the real
        // space hogs (media/preview) below and leaves E2EE search intact.

        // Clear our own caches (media files dir, preview DB).
        // SDK media database (matrix-sdk-media.sqlite3) is managed by the SDK's
        // own MediaRetentionPolicy (400 MiB max, 60-day expiry, daily cleanup).
        let _ = crate::cache_manager::clear_all(&self.data_dir).await;
        self.media_transfer.cleanup_plaintext();

        // Keep timeline_media_sources -- they hold encryption keys needed to
        // re-download E2EE media after cache clear.  They get repopulated on
        // each timeline sync anyway, but clearing them creates a window where
        // re-downloads of encrypted media fail (produces unreadable blobs).
        {
            let mut pc = self.preview.cache.write().await;
            pc.clear();
        }
        if let Ok(store) = self.preview.store.lock() {
            if let Some(store) = store.as_ref() {
                let _ = store.delete_all();
            }
        }
        let app_cache_store = self.local_cache.app_cache_store();
        if let Ok(store) = app_cache_store.lock() {
            if let Some(store) = store.as_ref() {
                let _ = store.clear_all();
            }
        }

        let stats_after = crate::cache_manager::calculate_stats(&self.data_dir).await;
        stats_before
            .total_bytes
            .saturating_sub(stats_after.total_bytes)
    }

    /// Evict media files if over the size limit. Spawned + generation-tracked by
    /// the caller (`MatrixProtocol::auto_cleanup`) so logout aborts it cleanly
    /// rather than letting an untracked task race the logout cache wipe.
    pub(crate) async fn run_auto_cleanup(&self, size_limit_bytes: u64) {
        let stats = crate::cache_manager::calculate_stats(&self.data_dir).await;
        if stats.media_files_bytes > size_limit_bytes {
            info!(
                "Auto-cleanup: media {}MB > limit {}MB, evicting...",
                stats.media_files_bytes / (1024 * 1024),
                size_limit_bytes / (1024 * 1024)
            );
            let _ =
                crate::cache_manager::clear_media_files(&self.data_dir, 0, size_limit_bytes).await;
            if let Ok(mut sources) = self.timeline.media_sources.write() {
                sources.clear();
            }
        }
    }
}

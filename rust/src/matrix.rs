// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::Duration;

use anyhow::{anyhow, Result};
use async_trait::async_trait;
use matrix_sdk::ruma::OwnedRoomId;
use matrix_sdk::{Client, Room};
use matrix_sdk_ui::timeline::Timeline as SdkTimeline;
use tokio::sync::RwLock;
use tracing::{debug, error, info, warn};

use crate::account_service::AccountService;
use crate::auth_service::AuthService;
use crate::encryption_service::{EncryptionService, RecoverySetupError};
use crate::folder_service::FolderService;
use crate::local_cache_service::LocalCacheService;
use crate::media_cache_service::MediaCacheService;
use crate::media_transfer_service::MediaTransferService;
use crate::message_action_service::MessageActionService;
use crate::presence_typing_service::PresenceTypingService;
use crate::preview_service::PreviewService;
use crate::protocol::ProtocolClient;
use crate::recent_emoji_service::RecentEmojiService;
use crate::room_action_service::RoomActionService;
use crate::room_creation_service::RoomCreationService;
use crate::room_directory_service::RoomDirectoryService;
use crate::room_invite_service::RoomInviteService;
use crate::room_list_service::RoomListService;
use crate::room_member_service::RoomMemberService;
use crate::room_summary_service::{RoomSummaryRefreshContext, RoomSummaryService};
use crate::search_service::SearchService;
use crate::session_lifecycle_service::{SessionLifecycleComponents, SessionLifecycleService};
use crate::session_storage_service::SessionStorageService;
use crate::session_task_service::SessionTaskService;
use crate::sync_loop_service::SyncLoopRuntime;
use crate::timeline_navigation_service::TimelineNavigationService;
use crate::timeline_service::TimelineService;
use crate::timeline_update_service::TimelineUpdateService;
use crate::timeline_window::TimelineDiff;
use crate::timeline_window_service::{
    TimelineChangedFactory, TimelineRuntime, TimelineWindowService,
};
use crate::types::QrCodeImage;
use crate::types::{
    AccountActionResult, AccountSummary, CreateRoomRequest, DeleteDevicesResult, DeviceSessionList,
    EncryptionOverview, FolderMeta, ImportKeysResult, MessageContent, RegistrationRequest,
    RegistrationResult, ResetIdentityResult, RoomAccess, RoomDirectoryPage, RoomDirectoryRequest,
    RoomMembersSnapshot, RoomPreviewInfo, RoomSettingsSnapshot, RoomSummary, SasEmoji, SearchPage,
    SearchRequest, SessionInfo, SpaceHierarchyRequest, ThreePid, ThreePidMedium,
    ThreePidTokenResponse, TimelineItem, TimelineSlice, UserProfile, UserProfileDetails,
    UserTrustState, UsernameAvailability, VerificationCapabilities,
};
use crate::verification_service::VerificationService;
type DeviceVerifiedCallback = Box<dyn Fn(bool) + Send>;
type SyncStateCallback = Box<dyn Fn(u32) + Send>;
type PresenceChangedCallback = Box<dyn Fn(&str, u32, u64) + Send>;
type TypingChangedCallback = Box<dyn Fn(&str, Vec<String>) + Send>;
type VerificationIncomingRequestCallback = Box<dyn Fn(&str, &str, &str) + Send>;
type VerificationStateCallback = Box<dyn Fn(u32, &str) + Send>;
type UserTrustChangedCallback = Box<dyn Fn(&str, u32) + Send>;
type VerificationIncomingUserRequestCallback = Box<dyn Fn(&str, &str, &str) + Send>;
type VerificationRequestClosedCallback = Box<dyn Fn(&str) + Send>;
type VerificationSasEmojisCallback = Box<dyn Fn(&str, &[SasEmoji]) + Send>;
type VerificationQrDataCallback = Box<dyn Fn(&str, &QrCodeImage) + Send>;
type VerificationCancelInfoCallback = Box<dyn Fn(&str, &str, bool) + Send>;

/// Runtime handles the per-room latest-event preview-refresh tasks need. All
/// fields are cheap `Arc`/service clones; bundled so the subscribe helper has one
/// `Clone`-able parameter instead of seven.
/// Preview backfills currently running. Read by `TimelineWindow::new` so a slow
/// room-open reports whether it was competing with them (they hold matrix-sdk's
/// client-global event-cache lock and hammer the same SQLite pool).
pub(crate) static BACKFILLS_INFLIGHT: AtomicU64 = AtomicU64::new(0);

/// Decrements on every exit path of the backfill, including its early returns.
pub(crate) struct BackfillInflightGuard;

impl BackfillInflightGuard {
    fn enter() -> Self {
        BACKFILLS_INFLIGHT.fetch_add(1, Ordering::Relaxed);
        Self
    }
}

impl Drop for BackfillInflightGuard {
    fn drop(&mut self) {
        BACKFILLS_INFLIGHT.fetch_sub(1, Ordering::Relaxed);
    }
}

#[derive(Clone)]
struct LatestEventPreviewDeps {
    timeline_cache: Arc<RwLock<HashMap<String, Vec<TimelineItem>>>>,
    rooms_cache: Arc<RwLock<Vec<RoomSummary>>>,
    notification_overrides: Arc<RwLock<HashMap<String, crate::types::RoomNotificationMode>>>,
    presence_typing: PresenceTypingService,
    local_cache: LocalCacheService,
    session_tasks: SessionTaskService,
    /// Pushes rooms-list refreshes to C++ — the preview updater edits rooms_cache
    /// out of band (esp. after a blank-preview backfill), so without this the new
    /// last message sits in the cache until some later full rebuild.
    room_list_callback: crate::room_list_service::RoomListCallbackSlot,
    /// Caps concurrent blank-preview backfills so a cold start with many
    /// system-tail rooms walks the local event DB a few rooms at a time.
    preview_backfill_semaphore: Arc<tokio::sync::Semaphore>,
}

/// Outcome of `backfill_blank_preview`.
enum PreviewBackfill {
    /// Room not synced yet (empty event cache) — caller retries on a later emit.
    NotSynced,
    /// Newest previewable message: (text, sender_name, timestamp, is_outgoing).
    Found(String, String, std::time::SystemTime, bool),
    /// Room synced but no previewable message found — caller latches (stays blank).
    Exhausted,
}

/// Sync state constants exposed via FFI.
pub const SYNC_STATE_NOT_STARTED: u32 = 0;
pub const SYNC_STATE_SYNCING: u32 = 1;
pub const SYNC_STATE_SYNCED: u32 = 2;
/// The local encrypted store cannot decrypt its own values (store cipher
/// mismatch). Deterministic — sync retries can never heal it; the user must
/// sign out and back in (or repair the store).
pub const SYNC_STATE_STORE_ERROR: u32 = 3;
pub use crate::media_transfer_service::{
    MEDIA_DOWNLOAD_PHASE_DECRYPTING, MEDIA_DOWNLOAD_PHASE_DOWNLOADING,
};

fn lock_matrix_mutex<'a, T>(mutex: &'a Mutex<T>, name: &str) -> MutexGuard<'a, T> {
    match mutex.lock() {
        Ok(guard) => guard,
        Err(poisoned) => {
            warn!("Recovering poisoned Matrix mutex: {name}");
            poisoned.into_inner()
        }
    }
}

/// True if `err` (or anything in its chain) indicates the local store is corrupt
/// or unreadable at OPEN time — sqlite corruption or a store-cipher mismatch —
/// as opposed to a transient/network failure. Used to force a logout instead of
/// silently dropping to the login screen when the store can't be opened.
pub(crate) fn is_store_corruption_error(err: &anyhow::Error) -> bool {
    err.chain().any(|e| {
        let message = e.to_string();
        message.contains("Error encrypting or decrypting a value")
            || message.contains("is not a database")
            || message.contains("malformed")
    })
}

/// Callback invoked with `(room_id, in_progress)` around a room's one-shot
/// member fetch. Boxed so the FFI can install a C trampoline.
pub(crate) type MemberSyncFn = Box<dyn Fn(&str, bool) + Send>;
pub(crate) type MemberSyncCallbackSlot = Arc<Mutex<Option<MemberSyncFn>>>;

fn emit_member_sync(slot: &MemberSyncCallbackSlot, room_id: &str, in_progress: bool) {
    if let Ok(guard) = slot.lock() {
        if let Some(cb) = guard.as_ref() {
            cb(room_id, in_progress);
        }
    }
}

/// Real Matrix protocol implementation using matrix-rust-sdk.
pub struct MatrixProtocol {
    client: Arc<RwLock<Option<Client>>>,
    stream_server: Arc<tokio::sync::Mutex<Option<crate::media_stream::server::MediaStreamServer>>>,
    session_tasks: SessionTaskService,
    auth: AuthService,
    account: AccountService,
    room_list: RoomListService,
    /// Running sliding-sync service handle (set while the sliding backend runs);
    /// used by the room-open path to subscribe rooms and by teardown to stop it.
    sliding_sync: crate::sliding_sync_service::SlidingSyncHandle,
    /// Notified to force an immediate sliding-sync reconnect (e.g. when the C++
    /// network monitor sees the interface return), short-circuiting backoff.
    reconnect_notify: Arc<tokio::sync::Notify>,
    timeline: TimelineService,
    last_timeline_watch_room: Arc<Mutex<Option<String>>>,
    device_verified_callback: Arc<Mutex<Option<DeviceVerifiedCallback>>>,
    sync_state: Arc<AtomicU32>,
    sync_state_callback: Arc<Mutex<Option<SyncStateCallback>>>,
    /// Bumped whenever a sign-in begins (login / register / restore). `logout`
    /// snapshots it and refuses to run its destructive tail if it moved — a
    /// teardown must never vandalise a session that started underneath it.
    auth_generation: Arc<AtomicU64>,
    /// Rooms with a live backup-key retry chain (see `retry_room_keys_from_backup`).
    backup_retry_rooms: BackupRetryRooms,
    /// Rooms whose backup keys we have already bulk-fetched this session. Unlike
    /// `backup_retry_rooms` this is claim-ONLY (never released), so opening a room
    /// repeatedly cannot re-download keys the crypto store already holds — that
    /// re-download is what produced the "received a room key we already have …
    /// discarding" bursts. Cleared with the session so a re-login re-fetches.
    backup_prefetched_rooms: BackupRetryRooms,
    notification_callback: crate::notification_service::NotificationCallbackSlot,
    invite_notification_callback: crate::notification_service::InviteNotificationCallbackSlot,
    /// Fires once when a new, unverified session appears on the account.
    new_login_callback: crate::new_login_service::NewLoginCallbackSlot,
    /// Fires once when the homeserver rejects our access token (`M_UNKNOWN_TOKEN`),
    /// i.e. this session was signed out from somewhere else. See
    /// [`crate::session_invalidation`].
    session_invalidated_callback: crate::session_invalidation::SessionInvalidatedCallbackSlot,
    /// Fires (room_id, in_progress) around a room's one-shot member fetch, so the
    /// UI can show a "syncing members" indicator while lazy profiles resolve.
    member_sync_callback: MemberSyncCallbackSlot,
    /// Fires (room_id, event_id, fetching) around a message's URL-preview fetch.
    preview_fetch_callback: crate::preview_fetch_signal::PreviewFetchCallbackSlot,
    /// Byte progress for direct (send-queue-bypassing) uploads, keyed by txn id.
    upload_progress_callback: crate::upload_progress::UploadProgressCallbackSlot,
    /// Server-side recent-emoji list changes (startup hydrate + cross-device).
    recent_emoji_callback: crate::recent_emoji::RecentEmojiCallbackSlot,
    /// This homeserver's `m.upload.size`; 0 until loaded.
    max_upload_size: crate::upload_limit::MaxUploadSizeSlot,
    presence_typing: PresenceTypingService,
    preview: PreviewService,
    media_transfer: MediaTransferService,
    /// App/preview/search local cache store wiring.
    local_cache: LocalCacheService,
    folders: FolderService,
    recent_emoji: RecentEmojiService,
    saved_messages: crate::saved_messages_service::SavedMessagesService,
    /// Active sign-in verification coordinator.
    verification: VerificationService,
    session_storage: SessionStorageService,
    session_lifecycle: SessionLifecycleService,
    /// Encryption and session-management operations with shared security state.
    encryption: EncryptionService,
    /// Focused timeline navigation and pagination actions.
    timeline_navigation: TimelineNavigationService,
    search: SearchService,
    runtime_handle: tokio::runtime::Handle,
    /// Session data dir root (e.g. for the media-stream cache directory).
    data_dir: PathBuf,
}

impl MatrixProtocol {
    pub fn new(runtime_handle: tokio::runtime::Handle, data_dir: PathBuf) -> Self {
        let media_cache = MediaCacheService::new(data_dir.clone());
        media_cache.cleanup_plaintext();
        // Reclaim any stores a previous logout renamed aside but did not finish
        // deleting (e.g. the app was killed mid-delete). Cheap and non-blocking.
        crate::trash::sweep(&data_dir);
        let timeline = TimelineService::new();
        let timeline_navigation = TimelineNavigationService::new(timeline.windows.clone());
        let media_transfer = MediaTransferService::new(media_cache, timeline.media_sources.clone());
        let app_cache_store = Arc::new(std::sync::Mutex::new(None));

        let preview_store = Arc::new(std::sync::Mutex::new(None));
        let preview_cache = Arc::new(RwLock::new(HashMap::new()));
        let search = SearchService::new();
        let session_storage = SessionStorageService::new();
        // Built before local_cache so the snapshot writer can share its rooms list.
        let room_list = RoomListService::new();
        let local_cache = LocalCacheService::new(
            data_dir.clone(),
            runtime_handle.clone(),
            app_cache_store.clone(),
            preview_cache.clone(),
            preview_store.clone(),
            search.index.clone(),
            room_list.rooms.clone(),
        );
        local_cache.open_from_local_secrets();

        let sync_state = Arc::new(AtomicU32::new(SYNC_STATE_NOT_STARTED));
        let presence_typing = PresenceTypingService::new();
        let preview = PreviewService::new(preview_cache, preview_store);
        let session_lifecycle = SessionLifecycleService::new(SessionLifecycleComponents {
            data_dir: data_dir.clone(),
            local_cache: local_cache.clone(),
            media_transfer: media_transfer.clone(),
            session_storage: session_storage.clone(),
            room_list: room_list.clone(),
            timeline: timeline.clone(),
            preview: preview.clone(),
            search: search.clone(),
            presence_typing: presence_typing.clone(),
        });
        Self {
            client: Arc::new(RwLock::new(None)),
            stream_server: Arc::new(tokio::sync::Mutex::new(None)),
            session_tasks: SessionTaskService::new(runtime_handle.clone()),
            auth: AuthService::new(
                data_dir.clone(),
                local_cache.clone(),
                session_storage.clone(),
            ),
            account: AccountService::new(),
            room_list,
            sliding_sync: Arc::new(Mutex::new(None)),
            reconnect_notify: Arc::new(tokio::sync::Notify::new()),
            timeline,
            last_timeline_watch_room: Arc::new(Mutex::new(None)),
            device_verified_callback: Arc::new(Mutex::new(None)),
            sync_state: sync_state.clone(),
            sync_state_callback: Arc::new(Mutex::new(None)),
            auth_generation: Arc::new(AtomicU64::new(0)),
            backup_retry_rooms: BackupRetryRooms::default(),
            backup_prefetched_rooms: BackupRetryRooms::default(),
            notification_callback: Arc::new(Mutex::new(None)),
            invite_notification_callback: Arc::new(Mutex::new(None)),
            member_sync_callback: Arc::new(Mutex::new(None)),
            new_login_callback: Arc::new(Mutex::new(None)),
            session_invalidated_callback: Arc::new(Mutex::new(None)),
            preview_fetch_callback: Arc::new(Mutex::new(None)),
            upload_progress_callback: Arc::new(Mutex::new(None)),
            recent_emoji_callback: Arc::new(Mutex::new(None)),
            max_upload_size: Arc::new(AtomicU64::new(0)),
            presence_typing,
            preview,
            media_transfer,
            folders: FolderService::new(local_cache.app_cache_store()),
            recent_emoji: RecentEmojiService::new(local_cache.app_cache_store()),
            saved_messages: crate::saved_messages_service::SavedMessagesService::new(),
            local_cache,
            verification: VerificationService::new(),
            session_storage,
            session_lifecycle,
            encryption: EncryptionService::new(),
            timeline_navigation,
            search,
            runtime_handle,
            data_dir,
        }
    }

    pub fn cleanup_plaintext_media_cache(&self) {
        self.media_transfer.cleanup_plaintext();
    }

    fn stream_deps(&self) -> crate::media_stream::server::StreamDeps {
        crate::media_stream::server::StreamDeps {
            client: self.client.clone(),
            media_sources: self.timeline.media_sources.clone(),
            runtime: self.runtime_handle.clone(),
            cache: Arc::new(crate::media_stream::cache::MediaCache::new(
                self.data_dir.join("media-stream-cache"),
            )),
        }
    }

    pub async fn video_stream_url(&self, mxc: &str) -> Option<String> {
        if self.client.read().await.is_none() {
            return None;
        }
        let mut guard = self.stream_server.lock().await;
        if guard.is_none() {
            match crate::media_stream::server::MediaStreamServer::start(self.stream_deps()).await {
                Ok(s) => *guard = Some(s),
                Err(e) => {
                    warn!("media stream server failed to start: {e}");
                    return None;
                }
            }
        }
        guard.as_ref().map(|s| s.stream_url(mxc))
    }

    /// Fraction of `mxc` downloaded by the streaming proxy (0.0–1.0). Returns 0.0
    /// when nothing is known yet (no entry / size not received) — i.e. a stream
    /// just starting. The C++ caller treats a non-proxy-streamed (local-file)
    /// video as fully available on its own, so this never needs a 1.0 default.
    pub async fn video_stream_progress(&self, mxc: &str) -> f32 {
        if let Some(server) = self.stream_server.lock().await.as_ref() {
            if let Some((written, total)) = server.progress(mxc).await {
                if total > 0 {
                    return (written as f64 / total as f64).clamp(0.0, 1.0) as f32;
                }
            }
        }
        0.0
    }

    /// Raw (downloaded, total) bytes for a proxy-streamed video, or None if it
    /// isn't currently streamed. Lets the UI show accurate "X / Y" progress even
    /// when the event carries no file size.
    pub async fn video_stream_progress_bytes(&self, mxc: &str) -> Option<(u64, u64)> {
        if let Some(server) = self.stream_server.lock().await.as_ref() {
            return server.progress(mxc).await;
        }
        None
    }

    /// Whether the proxy's current download for `mxc` has failed. Lets the C++
    /// retry loop drop to its local fallback promptly instead of waiting out its
    /// stall window. False when there's no running server / no entry.
    pub async fn video_stream_errored(&self, mxc: &str) -> bool {
        if let Some(server) = self.stream_server.lock().await.as_ref() {
            return server.errored(mxc).await;
        }
        false
    }

    /// Whether `mxc` can be played progressively: 0 unknown, 1 faststart,
    /// 2 moov-at-end (the whole file downloads before the first frame). Read from
    /// the process-global mirror, which the classifier updates synchronously — so
    /// this needs no runtime and no lock on the stream server.
    pub fn video_stream_container(&self, mxc: &str) -> u8 {
        crate::container_store::cached(mxc) as u8
    }

    /// Profile-memo partition key for the client this context currently holds, if
    /// any. The memo is process-wide but partitioned per account, so every drop of
    /// it must name whose entries to drop.
    async fn current_profile_owner(&self) -> String {
        self.client
            .read()
            .await
            .as_ref()
            .map(crate::profile_cache_store::owner_key)
            .unwrap_or_default()
    }

    fn timeline_runtime(&self) -> TimelineRuntime {
        TimelineRuntime {
            timelines: self.timeline.timelines.clone(),
            windows: self.timeline.windows.clone(),
            cache: self.timeline.cache.clone(),
            update_cache: self.timeline.update_cache.clone(),
            refresh_states: self.timeline.refresh_states.clone(),
            reply_preview_cache: self.timeline.reply_preview_cache.clone(),
            pending_reaction_overrides: self.timeline.pending_reaction_overrides.clone(),
            media_sources: self.timeline.media_sources.clone(),
            pending_forward_meta: self.timeline.pending_forward_meta.clone(),
            callbacks: self.timeline.callbacks.clone(),
            room_list_callback: self.room_list.callback.clone(),
            notification_callback: self.notification_callback.clone(),
            invite_notification_callback: self.invite_notification_callback.clone(),
            preview_fetch_callback: self.preview_fetch_callback.clone(),
            sync_state: self.sync_state.clone(),
            rooms_cache: self.room_list.rooms.clone(),
            preview_cache: self.preview.cache.clone(),
            preview_inflight: self.preview.inflight.clone(),
            preview_store: self.preview.store.clone(),
            notification_overrides: self.room_list.notification_overrides.clone(),
            sender_avatar_cache: self.timeline.sender_avatar_cache.clone(),
            search_index: self.search.index.clone(),
            search_index_fingerprints: self.search.fingerprints.clone(),
            presence_typing: self.presence_typing.clone(),
            session_tasks: self.session_tasks.clone(),
            recent_rooms: self.timeline.recent_rooms.clone(),
            creating_windows: self.timeline.creating_windows.clone(),
        }
    }

    /// Fire the per-room timeline + room-list callbacks so C++ re-pulls this
    /// room's summary. This is the same "room changed, re-fetch" signal the
    /// live-timeline path uses (TimelineUpdateService::notify_timeline_update);
    /// safe to call from any runtime task — the callbacks marshal onto the Qt
    /// event loop.
    fn emit_room_summary_changed(&self, room_id: &str) {
        TimelineUpdateService::notify_timeline_update(
            room_id,
            &self.timeline.callbacks,
            &self.room_list.callback,
        );
    }

    fn timeline_changed_factory() -> TimelineChangedFactory {
        Arc::new(TimelineUpdateService::make_on_changed_callback)
    }

    /// Execute a registration request (initial or UIA continuation).
    async fn do_register(
        &self,
        client: Client,
        request: &RegistrationRequest,
        store_passphrase: &str,
    ) -> Result<RegistrationResult> {
        let outcome = self
            .auth
            .register(client, request, store_passphrase)
            .await?;
        if let Some(client) = outcome.client_for_sync {
            let mut c = self.client.write().await;
            *c = Some(client.clone());
            drop(c);
            self.start_sync(client);
        }
        Ok(outcome.result)
    }

    /// Register a callback that fires when sync state changes.
    pub fn on_sync_state_changed(&self, callback: SyncStateCallback) {
        let mut cb = lock_matrix_mutex(&self.sync_state_callback, "sync_state_callback");
        *cb = Some(callback);
    }

    /// Force the sliding-sync consumer to reconnect immediately, short-circuiting
    /// any backoff/long-poll wait. Safe to call when already connected — a stored
    /// permit just makes the next wait return early.
    pub fn reconnect_now(&self) {
        self.reconnect_notify.notify_one();
    }

    /// Force the store-error sync state so the UI forces a logout. Used when the
    /// local store is found corrupt at startup, before the sync loop runs.
    pub fn report_store_corruption(&self) {
        let prev = self
            .sync_state
            .swap(SYNC_STATE_STORE_ERROR, std::sync::atomic::Ordering::SeqCst);
        if prev != SYNC_STATE_STORE_ERROR {
            let guard = lock_matrix_mutex(&self.sync_state_callback, "sync_state_callback");
            if let Some(callback) = guard.as_ref() {
                callback(SYNC_STATE_STORE_ERROR);
            }
        }
    }

    /// Register the per-message desktop-notification callback.
    pub fn on_notification(&self, callback: crate::notification_service::NotificationFn) {
        let mut cb = lock_matrix_mutex(&self.notification_callback, "notification_callback");
        *cb = Some(callback);
    }

    /// Register the room-invite desktop-notification callback.
    pub fn on_invite_notification(
        &self,
        callback: crate::notification_service::InviteNotificationFn,
    ) {
        let mut cb = lock_matrix_mutex(
            &self.invite_notification_callback,
            "invite_notification_callback",
        );
        *cb = Some(callback);
    }

    /// Register the "new login" (new-device) banner + notification callback.
    pub fn on_new_login(&self, callback: crate::new_login_service::NewLoginFn) {
        let mut cb = lock_matrix_mutex(&self.new_login_callback, "new_login_callback");
        *cb = Some(callback);
    }

    /// Register the "this session was signed out remotely" callback. Fires at
    /// most once per session; the C++ side signs the owning account out.
    pub fn on_session_invalidated(
        &self,
        callback: crate::session_invalidation::SessionInvalidatedFn,
    ) {
        let mut cb = lock_matrix_mutex(
            &self.session_invalidated_callback,
            "session_invalidated_callback",
        );
        *cb = Some(callback);
    }

    /// Register a callback fired with `(room_id, in_progress)` around a room's
    /// one-shot member fetch.
    pub fn on_member_sync(&self, callback: MemberSyncFn) {
        let mut cb = lock_matrix_mutex(&self.member_sync_callback, "member_sync_callback");
        *cb = Some(callback);
    }

    /// Register the URL-preview "fetching" callback (glows a message's URL while
    /// its link preview is being fetched).
    pub fn on_preview_fetching(&self, callback: crate::preview_fetch_signal::PreviewFetchFn) {
        let mut cb = lock_matrix_mutex(&self.preview_fetch_callback, "preview_fetch_callback");
        *cb = Some(callback);
    }

    /// Register the byte-progress callback for direct media uploads.
    pub fn on_upload_progress(&self, callback: crate::upload_progress::ProgressFn) {
        let mut cb = lock_matrix_mutex(&self.upload_progress_callback, "upload_progress_callback");
        *cb = Some(callback);
    }

    /// Register the recent-emoji list callback (`io.element.recent_emoji`).
    pub fn on_recent_emoji(&self, callback: crate::recent_emoji::RecentEmojiFn) {
        let mut cb = lock_matrix_mutex(&self.recent_emoji_callback, "recent_emoji_callback");
        *cb = Some(callback);
    }

    /// This homeserver's max upload size in bytes, or 0 if not yet loaded.
    pub fn max_upload_size(&self) -> u64 {
        self.max_upload_size
            .load(std::sync::atomic::Ordering::Relaxed)
    }

    /// Fetch the full member list for `room_id` once and re-resolve lazy-loaded
    /// sender profiles on the timeline (older senders otherwise render as raw
    /// MXIDs — `m.room.member` is `$LAZY` in required_state). Bracketed by
    /// member-sync callbacks so the UI can show a syncing indicator. No-op after
    /// the first call per room (guarded by `claim_members_fetch`).
    ///
    /// Fetches the full list FIRST (`sync_members`, retried — a busy cold start
    /// can fail/time out the first `/members` request) and only calls the
    /// timeline's `fetch_members()` once the store is complete. That ordering
    /// matters: `fetch_members()` against an incomplete store resolves the
    /// missing senders to an EMPTY `Ready(default)` profile that the SDK never
    /// retries (`set_non_ready`/`update_missing` both skip `Ready`), leaving them
    /// stuck as MXIDs; keeping them `Unavailable` until the store is full avoids
    /// that. `sync_members()` touches only the member store, not the timeline, so
    /// the retries don't flash resolved names to MXID.
    fn trigger_member_fetch(&self, room_id: &str, timeline: Arc<SdkTimeline>) {
        if !self.timeline.claim_members_fetch(room_id) {
            return;
        }
        let callback = self.member_sync_callback.clone();
        let runtime = self.timeline_runtime();
        let room_id = room_id.to_string();
        self.session_tasks.spawn(async move {
            emit_member_sync(&callback, &room_id, true);
            let room = timeline.room();
            const MAX_ATTEMPTS: u32 = 4;
            for attempt in 0..MAX_ATTEMPTS {
                if room.are_members_synced() {
                    break;
                }
                let _ = room.sync_members().await;
                if room.are_members_synced() {
                    break;
                }
                // Backoff (2, 4, 8s) before retrying the full member fetch.
                if attempt + 1 < MAX_ATTEMPTS {
                    tokio::time::sleep(Duration::from_secs(2u64 << attempt)).await;
                }
            }
            // Store is now (hopefully) complete. fetch_members re-resolves items
            // that were still Unavailable, but emits NO diff for senders the SDK
            // already stamped as an empty Ready(default) (it never revisits Ready).
            // So force one full re-snapshot: cache_timeline_snapshot's
            // resolve_stuck_sender_profiles re-queries those from the member store.
            timeline.fetch_members().await;
            // Prefetch the global /profile for departed/remote senders the member
            // store can't resolve, so the forced re-snapshot below fills their names
            // from cache — keeping the network entirely off the timeline render path
            // (resolve_stuck_sender_profiles only reads local sources + this cache).
            crate::timeline_cache_service::TimelineCacheService::prefetch_global_profiles_for_timeline(
                &timeline,
            )
            .await;
            let on_changed = TimelineUpdateService::make_on_changed_callback(runtime);
            on_changed(&room_id, vec![TimelineDiff::Full]);
            emit_member_sync(&callback, &room_id, false);
        });
    }

    /// Register a callback that fires when this device's verification status changes.
    pub fn on_device_verified_changed(&self, callback: DeviceVerifiedCallback) {
        let mut cb = lock_matrix_mutex(&self.device_verified_callback, "device_verified_callback");
        *cb = Some(callback);
    }

    /// Register a callback that fires when a user's presence changes.
    pub fn on_presence_changed_matrix(&self, callback: PresenceChangedCallback) {
        self.presence_typing.on_presence_changed(callback);
    }

    /// Register a callback that fires when the typing users list changes for a room.
    /// The callback receives the room_id and a list of user IDs currently typing.
    pub fn on_typing_changed(&self, callback: TypingChangedCallback) {
        self.presence_typing.on_typing_changed(callback);
    }

    pub fn clear_callbacks(&self) {
        {
            let mut cb = lock_matrix_mutex(&self.room_list.callback, "room_list_callback");
            *cb = None;
        }
        {
            let mut cb = lock_matrix_mutex(&self.sync_state_callback, "sync_state_callback");
            *cb = None;
        }
        {
            let mut cb = lock_matrix_mutex(&self.notification_callback, "notification_callback");
            *cb = None;
        }
        {
            let mut cb = lock_matrix_mutex(
                &self.invite_notification_callback,
                "invite_notification_callback",
            );
            *cb = None;
        }
        {
            let mut cb =
                lock_matrix_mutex(&self.device_verified_callback, "device_verified_callback");
            *cb = None;
        }
        {
            let mut cb = lock_matrix_mutex(&self.preview_fetch_callback, "preview_fetch_callback");
            *cb = None;
        }
        {
            let mut cb =
                lock_matrix_mutex(&self.upload_progress_callback, "upload_progress_callback");
            *cb = None;
        }
        {
            let mut cb = lock_matrix_mutex(&self.recent_emoji_callback, "recent_emoji_callback");
            *cb = None;
        }
        {
            let mut callbacks = lock_matrix_mutex(&self.timeline.callbacks, "timeline_callbacks");
            callbacks.clear();
        }
        {
            let mut last =
                lock_matrix_mutex(&self.last_timeline_watch_room, "last_timeline_watch_room");
            *last = None;
        }
        self.presence_typing.clear_callbacks();
        self.verification.clear_callbacks();
    }

    /// Send a typing notice for the given room.
    pub async fn send_typing(&self, room_id: &str, typing: bool) -> Result<()> {
        let client = self.require_client().await?;
        self.presence_typing
            .send_typing(client, room_id, typing)
            .await
    }

    /// Subscribe to typing notifications for a single room.
    /// Spawns a background task that calls `typing_callback` whenever the
    /// typing user list changes.
    pub fn subscribe_room_typing_notifications(&self, room: &matrix_sdk::Room, room_id: String) {
        self.presence_typing.subscribe_room_typing_notifications(
            room,
            room_id,
            &self.session_tasks,
        );
    }

    /// Push SDK verification-state changes to the device-verified callback.
    fn spawn_device_verified_watch(&self, client: Client) {
        let callback = self.device_verified_callback.clone();
        self.session_tasks.spawn(async move {
            let mut sub = client.encryption().verification_state();
            let mut last: Option<bool> = None;
            let mut emit = |state: matrix_sdk::encryption::VerificationState| {
                let verified = match state {
                    matrix_sdk::encryption::VerificationState::Verified => true,
                    matrix_sdk::encryption::VerificationState::Unverified => false,
                    matrix_sdk::encryption::VerificationState::Unknown => return,
                };
                if last == Some(verified) {
                    return;
                }
                last = Some(verified);
                info!("Device verification status changed: verified={verified}");
                let cb = lock_matrix_mutex(&callback, "device_verified_callback");
                if let Some(cb) = cb.as_ref() {
                    cb(verified);
                }
            };
            emit(sub.get());
            while let Some(state) = sub.next().await {
                emit(state);
            }
        });
    }

    /// Push SDK user-identity trust changes to the user-trust callback so trust
    /// shields for other users stay live (mirrors `spawn_device_verified_watch`).
    fn spawn_user_trust_watch(&self, client: Client) {
        let verification = self.verification.clone();
        self.session_tasks.spawn(async move {
            use futures_util::{pin_mut, StreamExt};
            let stream = match client.encryption().user_identities_stream().await {
                Ok(stream) => stream,
                Err(e) => {
                    warn!("failed to subscribe to user identity updates: {e}");
                    return;
                }
            };
            pin_mut!(stream);
            let own_user_id = client.user_id().map(|id| id.to_owned());
            while let Some(updates) = stream.next().await {
                for identity in updates.new.values().chain(updates.changed.values()) {
                    // Skip our own identity — trust shields are for other users;
                    // our own device/cross-signing status has a separate signal.
                    if own_user_id.as_deref() == Some(identity.user_id()) {
                        continue;
                    }
                    // Device-aware, matching the on-demand query: a verified
                    // identity with an unverified session surfaces a warning.
                    let state = verification
                        .resolve_user_trust(&client, identity.user_id())
                        .await;
                    verification.emit_user_trust_changed(identity.user_id().as_str(), state as u32);
                }
            }
        });
    }

    /// Watch for the homeserver rejecting our access token — this session was
    /// signed out from somewhere else (device deleted from another client, an
    /// admin removed it, a password change revoked it, a denied OAuth refresh).
    ///
    /// Without this the session died in place: every request 401s, the sliding
    /// sync service restarts forever against the dead token, and the UI sits on
    /// "Waiting for network…". Unlike a failed sync, `M_UNKNOWN_TOKEN` is
    /// positive proof from the server — see [`crate::session_invalidation`].
    fn spawn_session_invalidated_watch(&self, client: Client) {
        // Subscribe HERE, not inside the task: the broadcast does not replay, so
        // a 401 landing before the task's first poll would otherwise be missed.
        let mut changes = client.subscribe_to_session_changes();
        let callback = self.session_invalidated_callback.clone();
        let sliding = self.sliding_sync.clone();
        self.session_tasks.spawn(async move {
            let Some(soft_logout) =
                crate::session_invalidation::wait_for_invalidation(&mut changes).await
            else {
                return;
            };
            warn!("homeserver rejected our access token (soft_logout={soft_logout}); this session was signed out remotely");

            // Stop sync before handing over to C++. The consumer loop treats the
            // resulting terminal Error as transient and would keep restarting the
            // service against a token the server has already rejected, for the
            // whole duration of the teardown.
            let service = match sliding.lock() {
                Ok(mut guard) => guard.take(),
                Err(_) => None,
            };
            if let Some(service) = service {
                service.stop().await;
            }

            crate::session_invalidation::emit(&callback, soft_logout);
        });
    }

    /// Watch the account's own device list and alert once for a newly-appeared,
    /// unverified session — the "New login. Was this you?" alert. Mirrors
    /// `spawn_user_trust_watch`, driven by `Encryption::devices_stream`. The
    /// baseline of sessions present at startup is never alerted (the crypto store
    /// also replays pre-existing devices as `new` the first time it sees them).
    fn spawn_new_login_watch(&self, client: Client) {
        let callback = self.new_login_callback.clone();
        let encryption = self.encryption.clone();
        self.session_tasks.spawn(async move {
            use futures_util::{pin_mut, StreamExt};
            use std::collections::HashSet;

            let own_user_id = match client.user_id() {
                Some(id) => id.to_owned(),
                None => return,
            };

            let mut known: HashSet<String> = match encryption.get_own_devices(client.clone()).await
            {
                Ok(list) => list.sessions.into_iter().map(|s| s.device_id).collect(),
                Err(e) => {
                    warn!("new-login watch: failed to seed device baseline: {e}");
                    HashSet::new()
                }
            };

            let stream = match client.encryption().devices_stream().await {
                Ok(stream) => stream,
                Err(e) => {
                    warn!("failed to subscribe to device updates: {e}");
                    return;
                }
            };
            pin_mut!(stream);

            while let Some(updates) = stream.next().await {
                let Some(own_devices) = updates.new.get(&own_user_id) else {
                    continue;
                };
                let candidates: Vec<String> = own_devices
                    .keys()
                    .map(|id| id.to_string())
                    .filter(|id| !known.contains(id))
                    .collect();
                if candidates.is_empty() {
                    continue;
                }

                // Rare path (only a genuinely new device): one overview drives the
                // new-login gate, one device-list fetch supplies the name/ip/last-seen.
                let overview = encryption
                    .get_encryption_overview(client.clone())
                    .await
                    .ok();
                let cross_ready = overview
                    .as_ref()
                    .map(|o| o.cross_signing_ready)
                    .unwrap_or(false);
                let current_verified = overview
                    .as_ref()
                    .map(|o| o.is_current_device_verified)
                    .unwrap_or(false);
                let sessions = encryption.get_own_devices(client.clone()).await.ok();
                let current_device_id = client.device_id().map(|d| d.to_string());

                for id in candidates {
                    known.insert(id.clone());
                    let session = sessions
                        .as_ref()
                        .and_then(|l| l.sessions.iter().find(|s| s.device_id == id));
                    let is_current = current_device_id.as_deref() == Some(id.as_str());
                    let is_verified = session
                        .map(|s| {
                            matches!(
                                s.verification_state,
                                crate::types::DeviceVerificationState::Verified
                            )
                        })
                        .unwrap_or(false);
                    if !crate::new_login_service::should_alert_new_login(
                        is_current,
                        is_verified,
                        false,
                        cross_ready,
                        current_verified,
                    ) {
                        continue;
                    }
                    let name = session
                        .and_then(|s| s.display_name.clone())
                        .unwrap_or_default();
                    let ip = session
                        .and_then(|s| s.last_seen_ip.clone())
                        .unwrap_or_default();
                    let ts = session.and_then(|s| s.last_seen_ts).unwrap_or(0);
                    info!("[new-login] alerting for new device {id}");
                    let cb = lock_matrix_mutex(&callback, "new_login_callback");
                    if let Some(cb) = cb.as_ref() {
                        cb(&id, &name, &ip, ts);
                    }
                }
            }
        });
    }

    /// Register a client-level handler for the folders GLOBAL account-data event,
    /// so cross-device folder-definition changes (create/rename/reorder/delete made
    /// on another device) propagate live. On sliding sync there is no classic
    /// `/sync` account_data to drive `FolderService::apply_sync_account_data_jsons`,
    /// but the account-data extension is still enabled by RoomListService, so the
    /// event is delivered to client-level handlers — this is the missing consumer.
    ///
    /// The handler takes `Raw<AnyGlobalAccountDataEvent>` (KIND=GlobalAccountData,
    /// TYPE=None), so it fires for every global account-data event; we filter to the
    /// custom folders type by its JSON `type` field. On a folders event we update
    /// the folder cache and fire the room-list callback so the UI refreshes — the
    /// same effect the classic loop's `has_folder_event` -> full refresh had.
    fn register_folder_account_data_handler(&self, client: &Client) {
        use matrix_sdk::ruma::events::AnyGlobalAccountDataEvent;
        use matrix_sdk::ruma::serde::Raw;

        let folders = self.folders.clone();
        let room_list_callback = self.room_list.callback.clone();
        client.add_event_handler(move |raw: Raw<AnyGlobalAccountDataEvent>| {
            let folders = folders.clone();
            let room_list_callback = room_list_callback.clone();
            async move {
                let json = raw.json().get();
                // Cheap pre-filter: folder state lives in Element's settings event
                // (section names/order) and our own sidebar-order event.
                if !json.contains(crate::room_folders::ELEMENT_SETTINGS_EVENT_TYPE)
                    && !json.contains(crate::room_folders::FOLDERS_EVENT_TYPE)
                {
                    return;
                }
                let applied = folders.apply_sync_account_data_jsons([json]).await;
                if applied {
                    info!("[folders] remote folder account-data change applied; refreshing UI");
                    let cb = lock_matrix_mutex(&room_list_callback, "room_list_callback");
                    if let Some(callback) = cb.as_ref() {
                        callback();
                    }
                }
            }
        });
    }

    /// Folders are native `element.io.section.*` room tags, so a section
    /// created/removed on a room
    /// by ANY client (or this one on another device) surfaces only as an `m.tag`
    /// change. Sliding sync delivers those via the account-data extension but
    /// never as a room-list diff (see `sliding_sync_service`), so without this
    /// handler an Element-created section wouldn't appear until restart. On any
    /// `m.tag` change we refresh that room's summary (recomputing its folder
    /// membership) and fire the room-list callback so C++ re-fetches rooms +
    /// folders.
    fn register_tag_account_data_handler(&self, client: &Client) {
        use matrix_sdk::ruma::events::tag::TagEvent;

        let timeline_cache = self.timeline.cache.clone();
        let rooms_cache = self.room_list.rooms.clone();
        let notification_overrides = self.room_list.notification_overrides.clone();
        let presence_typing = self.presence_typing.clone();
        let local_cache = self.local_cache.clone();
        let room_list_callback = self.room_list.callback.clone();

        // `client` is injected per invocation, never captured: the handler store
        // lives inside `ClientInner`, so a captured `Client` is a reference cycle
        // that keeps the sqlite stores open forever. See
        // `integration_tests::session_teardown`.
        client.add_event_handler(move |_ev: TagEvent, room: Room, client: Client| {
            let timeline_cache = timeline_cache.clone();
            let rooms_cache = rooms_cache.clone();
            let notification_overrides = notification_overrides.clone();
            let presence_typing = presence_typing.clone();
            let local_cache = local_cache.clone();
            let room_list_callback = room_list_callback.clone();
            async move {
                let room_id = room.room_id().to_string();
                let overrides = notification_overrides.read().await.clone();
                let presence = presence_typing.presence_snapshot();
                let context = RoomSummaryRefreshContext {
                    timeline_cache: &timeline_cache,
                    rooms_cache: &rooms_cache,
                    notification_overrides: &overrides,
                    presence_snapshot: &presence,
                    local_cache: &local_cache,
                };
                let mut changed = std::collections::HashSet::new();
                changed.insert(room_id);
                let _ = RoomSummaryService::refresh_rooms_cache_by_ids(&client, &changed, &context)
                    .await;
                let cb = lock_matrix_mutex(&room_list_callback, "room_list_callback");
                if let Some(callback) = cb.as_ref() {
                    callback();
                }
            }
        });
    }

    /// Keep the rooms-list (dialogs) preview current for E2EE rooms the user has
    /// NOT opened. `room.latest_event()` is only kept decrypted + reactively
    /// recomputed by the SDK once the room is REGISTERED with the `LatestEvents`
    /// subsystem; this app never opened a per-room timeline for unopened rooms, so
    /// those previews were blank/stale. `listen_and_subscribe_to_room` registers
    /// the room (event cache, already subscribed in `start_sync`, is its
    /// prerequisite) and hands back a `Subscriber` that emits every future
    /// `LatestEventValue` — including when a UTD's latest event later
    /// re-decrypts. One task per joined room; aborted with the session.
    ///
    /// `joined_rooms()` only covers rooms already in the store (session restore);
    /// on a FRESH login it is empty at `start_sync` and rooms arrive via the first
    /// sync. So we also watch `subscribe_to_all_room_updates()` and subscribe
    /// rooms as they appear (and as new rooms are joined mid-session), deduped so
    /// no room is ever subscribed twice.
    fn spawn_latest_event_preview_updater(&self, client: Client) {
        let deps = LatestEventPreviewDeps {
            timeline_cache: self.timeline.cache.clone(),
            rooms_cache: self.room_list.rooms.clone(),
            notification_overrides: self.room_list.notification_overrides.clone(),
            presence_typing: self.presence_typing.clone(),
            local_cache: self.local_cache.clone(),
            session_tasks: self.session_tasks.clone(),
            room_list_callback: self.room_list.callback.clone(),
            preview_backfill_semaphore: Arc::new(tokio::sync::Semaphore::new(4)),
        };

        self.session_tasks.spawn(async move {
            // Subscribe BEFORE the initial pass so a sync landing between the two
            // can't slip a room through the gap; the dedup set absorbs the overlap.
            let mut updates = client.subscribe_to_all_room_updates();
            let subscribed: Arc<tokio::sync::Mutex<std::collections::HashSet<OwnedRoomId>>> =
                Arc::new(tokio::sync::Mutex::new(std::collections::HashSet::new()));

            // Initial pass: covers session restore (rooms already in the store).
            for room in client.joined_rooms() {
                Self::subscribe_room_preview(&client, &room, &deps, &subscribed).await;
            }

            // Discovery: fresh-login rooms + rooms joined during the session arrive
            // via sync. `Lagged` just means we dropped some batches under load — the
            // store is still authoritative, so re-scan joined_rooms() and continue.
            loop {
                match updates.recv().await {
                    Ok(room_updates) => {
                        for room_id in room_updates.iter_all_room_ids() {
                            if let Some(room) = client.get_room(room_id) {
                                Self::subscribe_room_preview(&client, &room, &deps, &subscribed)
                                    .await;
                            }
                        }
                    }
                    Err(tokio::sync::broadcast::error::RecvError::Lagged(_)) => {
                        for room in client.joined_rooms() {
                            Self::subscribe_room_preview(&client, &room, &deps, &subscribed).await;
                        }
                    }
                    Err(tokio::sync::broadcast::error::RecvError::Closed) => return,
                }
            }
        });
    }

    /// Register one joined room with `LatestEvents` and spawn its preview-refresh
    /// task, exactly once (deduped). No-op for non-joined rooms or rooms already
    /// subscribed. The spawned task handle is registered with `session_tasks` so it
    /// aborts on session teardown.
    async fn subscribe_room_preview(
        client: &Client,
        room: &Room,
        deps: &LatestEventPreviewDeps,
        subscribed: &Arc<tokio::sync::Mutex<std::collections::HashSet<OwnedRoomId>>>,
    ) {
        if room.state() != matrix_sdk::RoomState::Joined {
            return;
        }
        let room_id = room.room_id().to_owned();

        // Check-and-insert FIRST so two near-simultaneous discoveries can't both
        // pass and double-subscribe.
        if !subscribed.lock().await.insert(room_id.clone()) {
            return;
        }

        // Registers the room (keeps `room.latest_event()` decrypted + reactive) and
        // returns the subscriber. `Ok(None)` => room gone; back it out of the set.
        let mut subscriber = match client
            .latest_events()
            .await
            .listen_and_subscribe_to_room(&room_id)
            .await
        {
            Ok(Some(sub)) => sub,
            Ok(None) => {
                subscribed.lock().await.remove(&room_id);
                return;
            }
            Err(e) => {
                warn!("listen_and_subscribe_to_room({room_id}) failed: {e}");
                subscribed.lock().await.remove(&room_id);
                return;
            }
        };

        let room = room.clone();
        let deps = deps.clone();
        // Cloned out before the task because the closure now captures all of
        // `deps` (backfill_blank_preview borrows it whole), so `deps` is no
        // longer available for register() after the move.
        let session_tasks = deps.session_tasks.clone();
        let handle = tokio::spawn(async move {
            let room_id_str = room.room_id().to_string();
            // The async-lock `subscribe()` does NOT replay the current value, so
            // refresh once now (post-registration the value is decrypted), then on
            // every subsequent emit.
            let mut backfill_tried = false;
            // The LatestEvents subscriber only fires when the latest event CHANGES,
            // so a room whose newest event is a stable state event (e.g. the own
            // join) never re-wakes this loop — the first backfill can run too early
            // (before deep history is paginable) and then never retry. Drive bounded
            // retries with a delay, independent of the subscriber.
            let mut backfill_attempts: u32 = 0;
            const MAX_BACKFILL_ATTEMPTS: u32 = 6;
            let mut last_preview: Option<String> = None;
            loop {
                let overrides_snap = deps.notification_overrides.read().await.clone();
                let presence_snap = deps.presence_typing.presence_snapshot();
                RoomSummaryService::refresh_room_summary_cache(
                    &room,
                    &room_id_str,
                    &deps.timeline_cache,
                    &deps.rooms_cache,
                    &overrides_snap,
                    &presence_snap,
                )
                .await;
                let mut snapshot = deps.rooms_cache.read().await.clone();

                // A blank preview means `latest_event()` found no previewable
                // message in its in-memory window — typically a run of system
                // messages at the tail. Walk the local event DB backward (once)
                // to surface the last real message, then refresh so it becomes
                // the preview. See `backfill_blank_preview`.
                let in_cache = snapshot.iter().any(|r| r.room_id == room_id_str);
                let preview_blank = snapshot
                    .iter()
                    .find(|r| r.room_id == room_id_str)
                    .is_some_and(|r| r.last_event_text.is_empty());
                debug!(target: "tm_preview", room = %room_id_str, in_cache, preview_blank, backfill_tried, backfill_attempts, "preview updater tick");
                if !backfill_tried && preview_blank && backfill_attempts < MAX_BACKFILL_ATTEMPTS {
                    let outcome = Self::backfill_blank_preview(&room, &deps).await;
                    // NotSynced must NOT spend an attempt: the room's events simply
                    // hadn't arrived, so there was nothing to page through. A big
                    // account can take longer to deliver a low-priority room than the
                    // whole retry budget lasts, and the counter never resets — charging
                    // for those left the preview blank for the rest of the session.
                    if !matches!(outcome, PreviewBackfill::NotSynced) {
                        backfill_attempts += 1;
                    }
                    match outcome {
                        // Not synced yet, or paginated some but found no message yet —
                        // a later retry paginates deeper (the cache's token advances).
                        // Don't latch; the bounded retry below re-attempts.
                        PreviewBackfill::NotSynced | PreviewBackfill::Exhausted => {}
                        // Found the newest real message: write it straight into the
                        // summary (extract_last_event won't surface it, since it can't
                        // see past latest_event()'s state-event pick on the hot path).
                        PreviewBackfill::Found(text, sender, ts, is_outgoing) => {
                            backfill_tried = true;
                            RoomSummaryService::set_room_preview(
                                &deps.rooms_cache,
                                &room_id_str,
                                text,
                                sender,
                                ts,
                                is_outgoing,
                            )
                            .await;
                            snapshot = deps.rooms_cache.read().await.clone();
                        }
                    }
                }

                // Persist + push a rooms-list refresh to C++ when this room's non-blank
                // preview text first appears or changes — the updater edits
                // rooms_cache out of band (esp. after a backfill), so without an
                // explicit notify the new last message would sit in the cache
                // until some later full rebuild.
                //
                // Scheduling only on an actual preview change (not per ~4s tick)
                // still matters even though the write is now coalesced: it keeps
                // idle blank-preview rooms from dirtying the snapshot every tick.
                let current_preview = snapshot
                    .iter()
                    .find(|r| r.room_id == room_id_str)
                    .map(|r| r.last_event_text.clone())
                    .unwrap_or_default();
                if !current_preview.is_empty()
                    && last_preview.as_deref() != Some(current_preview.as_str())
                {
                    last_preview = Some(current_preview);
                    deps.local_cache.schedule_rooms_snapshot();
                    TimelineUpdateService::notify_room_list_update(&deps.room_list_callback);
                }

                // Wait for the next LatestEvents emit. But if a blank preview still
                // has retries left, also wake on a short delay — the subscriber won't
                // fire again for a stable state-event room, so this is the only way
                // the backfill retries (and paginates deeper) until it finds a
                // message or exhausts its attempts. `None` => observable dropped
                // (session torn down); exit.
                let retry_pending =
                    !backfill_tried && preview_blank && backfill_attempts < MAX_BACKFILL_ATTEMPTS;
                if retry_pending {
                    tokio::select! {
                        next = subscriber.next() => {
                            if next.is_none() {
                                break;
                            }
                        }
                        _ = tokio::time::sleep(Duration::from_secs(4)) => {}
                    }
                } else if subscriber.next().await.is_none() {
                    break;
                }
            }
        });
        session_tasks.register(handle);
    }

    /// Walk `room`'s local event-cache DB backward to surface the last
    /// previewable message when `latest_event()`'s in-memory window holds only
    /// system messages (the rooms-list preview would otherwise be blank).
    ///
    /// Disk-first: `run_backwards_once` serves persisted events from
    /// `matrix-sdk-event-cache.sqlite3` before ever reaching the network, and
    /// only hits a gap (network) for rooms with no stored history past the
    /// system tail. Bounded to a few small batches, throttled by a shared
    /// semaphore, and the caller runs it at most once per room. Returns true if
    /// any pagination ran, so the caller re-refreshes the summary to pick up the
    /// now-in-memory message.
    /// Find a blank-preview room's newest previewable message by scanning its
    /// event cache directly, bypassing `latest_event()` (which can be a bodyless
    /// state event, e.g. the own-join, that shadows the real message). Paginates
    /// backward if the synced window has none. Throttled, per-room, once — the
    /// expensive event-cache work stays OFF the parallel `build_rooms_cache` path;
    /// the caller writes the returned preview straight into `rooms_cache` (the
    /// hot-path `extract_last_event` deliberately does not do this scan).
    async fn backfill_blank_preview(room: &Room, deps: &LatestEventPreviewDeps) -> PreviewBackfill {
        let _permit = match deps
            .preview_backfill_semaphore
            .clone()
            .acquire_owned()
            .await
        {
            Ok(permit) => permit,
            Err(_) => return PreviewBackfill::NotSynced,
        };

        // Counted for the whole backfill (permit acquired above, guard dropped on every
        // exit path) so a slow room-open can say whether backfills were in flight while
        // it waited — see TimelineWindow::new's backfills_inflight.
        let _inflight = BackfillInflightGuard::enter();

        // Timed because this is the suspect: a room's FIRST event_cache() takes
        // matrix-sdk's client-global `by_room` WRITE lock and loads the room's chunk
        // from SQLite while holding it (event_cache/mod.rs:669-689). Up to
        // `preview_backfill_semaphore` of these run at once, against the room the user
        // is trying to open.
        let ec_t0 = std::time::Instant::now();
        let ec_result = room.event_cache().await;
        let ec_ms = ec_t0.elapsed().as_millis();
        if ec_ms > 200 {
            tracing::info!(
                room = %room.room_id(),
                ec_ms,
                inflight = BACKFILLS_INFLIGHT.load(Ordering::Relaxed),
                "backfill: slow room.event_cache()"
            );
        }
        let (event_cache, _drop_handles) = match ec_result {
            Ok(handles) => handles,
            Err(e) => {
                warn!(
                    "preview backfill: event_cache() failed for {}: {e}",
                    room.room_id()
                );
                return PreviewBackfill::NotSynced;
            }
        };

        // DO NOT hold a RoomEventCacheSubscriber across this backfill to trigger the
        // SDK's auto-shrink. It looks like the fix for the inflation this pagination
        // causes, but it crashes the app (tried and reverted):
        //
        // Dropping the last subscriber only QUEUES a shrink on an mpsc channel, which
        // a separate SDK task services later (event_cache/tasks.rs). If the user opens
        // the room in that gap, matrix-sdk-ui's Timeline subscribes and snapshots the
        // chunk list into an AsVector — and the queued shrink then runs underneath it,
        // leaving the observer's mirror stale: "The chunk is not found" /
        // "Inserting new chunk at the end: The previous chunk is invalid"
        // (matrix-sdk-common linked_chunk/as_vector.rs), and panic=abort kills us.
        // Cold start runs this backfill across every blank-preview room at exactly the
        // moment rooms are being opened, so the collision is routine, not theoretical.
        //
        // Without a subscriber the count stays 0, the 1->0 transition never happens,
        // and no shrink is ever queued for a room nobody has open. The chunk stays
        // inflated (see MAX_BACKFILL_* below, which bound how much we pull in) —
        // that is the price of not racing the SDK's observers.

        let rid = room.room_id().to_string();
        // Not synced yet (no cached events): don't burn the one-shot — a later
        // emit, once the room's events arrive, retries.
        let initial_count = match event_cache.events().await {
            Ok(events) if events.is_empty() => {
                debug!(target: "tm_preview", room = %rid, "backfill: NotSynced (0 cached events)");
                return PreviewBackfill::NotSynced;
            }
            Ok(events) => events.len(),
            Err(e) => {
                debug!(target: "tm_preview", room = %rid, "backfill: NotSynced (events() err: {e})");
                return PreviewBackfill::NotSynced;
            }
        };

        // Shadow case: a real message is already in the synced window but
        // latest_event() picked a state event over it — no pagination needed.
        if let Some((text, sender, ts, is_outgoing)) =
            RoomSummaryService::scan_event_cache_for_preview(room).await
        {
            debug!(target: "tm_preview", room = %rid, cached = initial_count, "backfill: Found in synced window");
            return PreviewBackfill::Found(text, sender, ts, is_outgoing);
        }

        // Beyond-window: paginate backward until a batch pulls in a message.
        let pagination = event_cache.pagination();
        const MAX_BATCHES: usize = 4;
        const BATCH_SIZE: u16 = 40;
        let mut batches = 0;
        let mut total_paginated = 0usize;
        for _ in 0..MAX_BATCHES {
            match tokio::time::timeout(
                Duration::from_secs(15),
                pagination.run_backwards_once(BATCH_SIZE),
            )
            .await
            {
                Ok(Ok(outcome)) => {
                    batches += 1;
                    total_paginated += outcome.events.len();
                    let found = outcome.events.iter().any(|e| {
                        e.raw()
                            .deserialize()
                            .ok()
                            .and_then(|any| RoomSummaryService::extract_event_body(&any))
                            .is_some()
                    });
                    if found || outcome.reached_start {
                        debug!(target: "tm_preview", room = %rid, batches, total_paginated, reached_start = outcome.reached_start, found_in_batch = found, "backfill: pagination stop");
                        break;
                    }
                }
                Ok(Err(e)) => {
                    debug!(target: "tm_preview", room = %rid, batches, "backfill: pagination err: {e}");
                    break;
                }
                Err(_) => {
                    debug!(target: "tm_preview", room = %rid, batches, "backfill: pagination timeout");
                    break;
                }
            }
        }

        match RoomSummaryService::scan_event_cache_for_preview(room).await {
            Some((text, sender, ts, is_outgoing)) => {
                debug!(target: "tm_preview", room = %rid, batches, total_paginated, "backfill: Found after pagination");
                PreviewBackfill::Found(text, sender, ts, is_outgoing)
            }
            None => {
                debug!(target: "tm_preview", room = %rid, batches, total_paginated, "backfill: Exhausted (no previewable message)");
                PreviewBackfill::Exhausted
            }
        }
    }

    /// Start the background sync loop with automatic reconnection.
    fn start_sync(&self, client: Client) {
        // Subscribe the event cache + its R2D2 redecryptor HERE — after auth —
        // not in build_client. The redecryptor's startup subscribes to the
        // OlmMachine's room-key stream; if the OlmMachine does not exist yet
        // (pre-login), `subscribe_to_room_key_stream` returns None and the task
        // PERMANENTLY exits (redecryption_loop -> false), so arriving room keys
        // never re-decrypt cached UTDs ("redecryption task has been shut down").
        // `subscribe()` is idempotent (get_or_init), and every caller of
        // start_sync (login/restore/register) has already established the session.
        if let Err(e) = client.event_cache().subscribe() {
            warn!("event cache subscribe failed: {e}");
        }

        // Load persisted audio durations for this session into the in-memory
        // mirror the (sync) timeline conversion reads to fill missing durations.
        {
            let client = client.clone();
            self.runtime_handle.spawn(async move {
                crate::audio_duration_store::load(&client).await;
            });
        }

        // Same, for the per-video "can this stream?" verdicts the players read to
        // decide between a spinner and a determinate download bar.
        {
            let client = client.clone();
            self.runtime_handle.spawn(async move {
                crate::container_store::load(&client).await;
            });
        }

        // Remembered global display names / avatars. Seeding the memo from disk is
        // what lets a departed user whose account was later deleted (permanent 404 on
        // /profile, and absent from every member store) still render under the name we
        // resolved for them in an earlier run, instead of collapsing to a raw MXID.
        {
            let client = client.clone();
            self.runtime_handle.spawn(async move {
                crate::profile_cache_store::load(&client).await;
            });
        }

        // Report media-upload progress: it is opt-in and off by default in
        // matrix-sdk. With it on, the send queue feeds upload progress onto the
        // local echo's EventSendState::NotSentYet { progress }, which our
        // timeline conversion turns into the bubble's progress bar (otherwise it
        // shows only a generic spinner).
        client.send_queue().enable_upload_progress(true);

        // Cache the homeserver's max upload size so the UI can reject oversized
        // files before starting an upload (instead of a silent server 413).
        {
            let client = client.clone();
            let slot = self.max_upload_size.clone();
            self.runtime_handle.spawn(async move {
                match client.load_or_fetch_max_upload_size().await {
                    Ok(size) => {
                        slot.store(u64::from(size), std::sync::atomic::Ordering::Relaxed);
                    }
                    Err(e) => warn!("failed to load max upload size: {e}"),
                }
            });
        }

        let session_generation = self.session_tasks.start_generation();
        // One coalescing rooms-snapshot writer for this session; aborted with the
        // generation on logout, before the store is wiped.
        self.session_tasks
            .spawn(self.local_cache.clone().run_snapshot_writer());
        self.spawn_device_verified_watch(client.clone());
        self.spawn_user_trust_watch(client.clone());
        self.spawn_new_login_watch(client.clone());
        self.spawn_session_invalidated_watch(client.clone());

        // Register the incoming self-verification request handler once, post-auth.
        // It is a client-level to-device handler (sync-backend-independent) that
        // prompts the UI when ANOTHER of our devices requests verification. It was
        // previously only wired by the now-deleted classic sync loop.
        self.verification.register_incoming_request_handler(&client);
        // In-room cross-user verification requests (other people asking to verify
        // with us) arrive as room-message events, not to-device.
        self.verification
            .register_incoming_user_request_handler(&client);

        // Propagate cross-device folder-definition changes (create/rename/reorder/
        // delete made on another device). On sliding sync there is no classic
        // `/sync` account_data, so install a client-level GLOBAL account-data
        // handler for the folders event. The account-data extension is enabled by
        // RoomListService, so these events arrive; this is the missing consumer.
        self.register_folder_account_data_handler(&client);

        // Folders are `u.*` tags: a section added/removed on a room (here or in
        // Element) arrives only as an `m.tag` change, which sliding sync does not
        // surface as a room-list diff. This handler is the missing consumer.
        self.register_tag_account_data_handler(&client);

        // Recent emojis: keep the picker in sync with `io.element.recent_emoji`
        // account data (server-authoritative, cross-device). Mirrors the folders
        // handler — live changes (this or another device) arrive here; the spawn
        // below hydrates once from the server at startup.
        {
            let recent = self.recent_emoji.clone();
            let slot = self.recent_emoji_callback.clone();
            client.add_event_handler(
                move |raw: matrix_sdk::ruma::serde::Raw<
                    matrix_sdk::ruma::events::AnyGlobalAccountDataEvent,
                >| {
                    let recent = recent.clone();
                    let slot = slot.clone();
                    async move {
                        let json = raw.json().get();
                        if !json.contains(crate::recent_emoji::RECENT_EMOJI_EVENT_TYPE) {
                            return;
                        }
                        if let Some(pairs) = recent.apply_sync_json(json).await {
                            crate::recent_emoji::emit(&slot, &pairs);
                        }
                    }
                },
            );
        }
        {
            let recent = self.recent_emoji.clone();
            let slot = self.recent_emoji_callback.clone();
            let client = client.clone();
            self.session_tasks.spawn(async move {
                if let Some(pairs) = recent.hydrate_from_server(&client).await {
                    crate::recent_emoji::emit(&slot, &pairs);
                }
            });
        }

        // Bulk-prefetch backup keys once the backup is usable, so the account
        // decrypts in a burst instead of the SDK's per-UTD trickle — which
        // otherwise keeps the "decrypting" indicator armed for minutes and is what
        // the user sees as "decrypting never ends". Aborted with the session.
        self.session_tasks
            .register(TimelineUpdateService::spawn_backup_bulk_key_prefetch(
                client.clone(),
                self.runtime_handle.clone(),
            ));

        self.session_tasks.register(spawn_key_readiness_probe(
            client.clone(),
            self.timeline.cache.clone(),
            &self.runtime_handle,
        ));

        // Open the local FTS search index up front. Both the backfill worker below
        // and live indexing (timeline_cache_service) no-op silently against a
        // `None` index, and nothing else opens it at session start since the
        // classic loop's opener was removed with that backend. Without this the
        // index only opened lazily on the first query — by which point backfill had
        // already exited and live indexing had been a no-op all session, so
        // E2EE-room search returned nothing (non-E2EE rooms use the server search).
        // Skipped when the user disabled E2EE-room search: the index stays closed
        // and no indexing workers run. `set_e2ee_search_enabled(true)` opens it
        // and spawns the backfill later if the user re-enables it mid-session.
        if crate::search_service::e2ee_search_enabled() {
            let search_key_material = LocalCacheService::search_index_key_material(&client);
            self.local_cache
                .ensure_search_index_open(search_key_material.as_deref());

            // Backfill the local FTS search index for E2EE rooms (non-E2EE rooms use
            // the server `/search` API). On sliding sync there is no warmup, so an
            // unopened encrypted room is never live-indexed; this worker enumerates
            // materialized rooms and pages their history via `/messages`, re-scanning
            // periodically to pick up rooms that slide into the window after startup.
            // Aborted with the session.
            self.session_tasks
                .register(self.runtime_handle.spawn(crate::search_backfill::run(
                    client.clone(),
                    self.search.index.clone(),
                )));
        }

        // Keep the app media cache within the configured size budget. Only
        // downloads grow `media_cache/`, and nothing else trims it during a
        // session, so debounce on store signals and evict LRU to the low
        // watermark. Tracked by session_tasks so logout aborts it.
        {
            let data_dir = self.data_dir.clone();
            self.session_tasks.spawn(async move {
                loop {
                    crate::cache_manager::media_stored_notified().await;
                    tokio::time::sleep(Duration::from_secs(5)).await;
                    let _ = crate::cache_manager::enforce_media_cache_limit(&data_dir).await;
                }
            });
        }

        // Keep rooms-list previews live for unopened E2EE rooms (registers each
        // joined room with the SDK's LatestEvents so `room.latest_event()` is
        // decrypted + reactive). Aborted with the session.
        self.spawn_latest_event_preview_updater(client.clone());

        // Backup-access precondition (off the room-list-blocking path): once the
        // SDK's E2EE init tasks settle, surface whether historical UTDs are
        // actually recoverable (verified && backup usable). `UtdCause` reads this
        // live at item build, so it is a correctness prerequisite for the glow to
        // mean anything; the UI prompts Verify / Enter-recovery-key when false.
        {
            let client = client.clone();
            self.session_tasks.spawn(async move {
                client
                    .encryption()
                    .wait_for_e2ee_initialization_tasks()
                    .await;
                crate::encryption_service::log_e2ee_diagnostics(&client, "init-settled").await;
            });
        }

        let runtime = SyncLoopRuntime {
            session_tasks: self.session_tasks.clone(),
            verification: self.verification.clone(),
            presence_typing: self.presence_typing.clone(),
            rooms_cache: self.room_list.rooms.clone(),
            room_list_callback: self.room_list.callback.clone(),
            notification_overrides: self.room_list.notification_overrides.clone(),
            timeline_cache: self.timeline.cache.clone(),
            timeline_runtime: self.timeline_runtime(),
            sync_state: self.sync_state.clone(),
            sync_state_callback: self.sync_state_callback.clone(),
            folders: self.folders.clone(),
            local_cache: self.local_cache.clone(),
            search_index: self.search.index.clone(),
        };

        // Sliding-sync is the only backend. It shares main's reactive decryption
        // pipeline set up above (event-cache subscribe + R2D2 redecryptor, bulk
        // backup-key prefetch, LatestEvents rooms-list previews); no custom
        // redecryption worker is spawned.
        info!("[sync] starting sliding-sync backend");
        crate::sliding_sync_service::SlidingSyncService::start(
            client,
            runtime,
            session_generation,
            self.sliding_sync.clone(),
            self.reconnect_notify.clone(),
        );
    }

    async fn require_client(&self) -> Result<Client> {
        self.client
            .read()
            .await
            .clone()
            .ok_or_else(|| anyhow!("Not logged in"))
    }

    async fn get_room(&self, room_id: &str) -> Result<Room> {
        let client = self.require_client().await?;
        let room_id: OwnedRoomId = room_id
            .try_into()
            .map_err(|_| anyhow!("Invalid room ID: {room_id}"))?;
        client
            .get_room(&room_id)
            .ok_or_else(|| anyhow!("Room not found: {room_id}"))
    }

    /// Sliding sync: subscribe a room so its full timeline/state streams into the
    /// event cache that the timeline windows read from. Best-effort; a no-op on
    /// the classic backend (the handle is `None`).
    async fn sliding_subscribe(&self, room_id: &str) {
        let service = match self.sliding_sync.lock() {
            Ok(guard) => guard.clone(),
            Err(_) => None,
        };
        let Some(service) = service else {
            return;
        };
        match matrix_sdk::ruma::RoomId::parse(room_id) {
            Ok(rid) => {
                debug!("[sliding] subscribing to opened room '{room_id}'");
                // subscribe_to_rooms awaits per-room latest-event listeners
                // (`listen_to_room`), which can take SEVERAL SECONDS for a large
                // room. This runs on the room-switch path (get_timeline_slice), so
                // awaiting it froze the switch for that whole time. Spawn it: the
                // subscription still lands, the switch returns immediately, and the
                // window's change callback re-polls once data arrives.
                let service_for_subscribe = service.clone();
                let rid_for_subscribe = rid.clone();
                self.session_tasks.spawn(async move {
                    let id: &matrix_sdk::ruma::RoomId = &rid_for_subscribe;
                    service_for_subscribe
                        .room_list_service()
                        .subscribe_to_rooms(&[id])
                        .await;
                });
                // Chase this room's backup keys while it shows undecryptable
                // messages: a short first step (once the timeline has populated)
                // then a decaying schedule, because the device that owns those keys
                // uploads them to backup on its own timetable and nothing tells us
                // when they land. Gated on the room actually having UTDs, so a
                // fully-decrypted room never re-downloads keys it already holds.
                if let Ok(client) = self.require_client().await {
                    let cache = self.timeline.cache.clone();
                    let room = rid.clone();
                    // Fetch THIS room's keys right away, newest sessions first. The
                    // room the user just opened is the one whose decryption they are
                    // waiting on, and nothing else prioritises it: the session-wide
                    // bulk sweep walks `client.rooms()` in arbitrary order and only
                    // covers rooms materialized when it ran. Claim-only, so this is
                    // at most one bulk download per room per session.
                    if self.backup_prefetched_rooms.claim(room.clone()) {
                        let prefetch_client = client.clone();
                        let prefetch_cache = cache.clone();
                        let prefetch_room = room.clone();
                        self.session_tasks.spawn(async move {
                            prefetch_room_keys_prioritized(
                                &prefetch_client,
                                &prefetch_room,
                                &prefetch_cache,
                            )
                            .await;
                        });
                    }
                    // One chase per room: reopening a room must not stack them. This
                    // tops up sessions that reach the backup later (see the schedule).
                    if self.backup_retry_rooms.claim(room.clone()) {
                        let retry_rooms = self.backup_retry_rooms.clone();
                        self.session_tasks.spawn(async move {
                            retry_room_keys_from_backup(client, room.clone(), cache).await;
                            retry_rooms.release(&room);
                        });
                    }
                }
            }
            Err(e) => warn!("[sliding] cannot parse room id '{room_id}': {e}"),
        }
    }

    /// Stop the running sliding-sync service gracefully and clear the handle.
    /// Must run before `abort_current_generation` on logout/session-change: the
    /// sliding consumer task is hard-aborted there (so it never runs its own
    /// stop), and the SDK's sync tasks are detach-on-drop — not abort-on-drop —
    /// so without this they keep syncing against a logged-out/wiped store.
    async fn stop_sliding_backend(&self) {
        let service = match self.sliding_sync.lock() {
            Ok(mut guard) => guard.take(),
            Err(_) => None,
        };
        if let Some(service) = service {
            info!("[sliding] stopping SyncService for teardown");
            service.stop().await;
        }
    }

    async fn get_or_create_timeline(&self, room_id: &str) -> Result<Arc<SdkTimeline>> {
        // Fast path: check windows first (preferred), then legacy timelines map
        let timeline =
            if let Some(timeline) = self.timeline.active_timeline_or_legacy(room_id).await {
                // Mark most-recently used so the resident-window LRU never evicts the
                // room being used (create_window_for_room handles the slow path).
                if let Ok(mut recent) = self.timeline.recent_rooms.lock() {
                    recent.retain(|r| r != room_id);
                    recent.push_front(room_id.to_string());
                }
                timeline
            } else {
                // Slow path: create a TimelineWindow
                let room = self.get_room(room_id).await?;

                self.sliding_subscribe(room_id).await;

                self.timeline
                    .create_missing_window_timeline(
                        &room,
                        room_id,
                        self.timeline_runtime(),
                        Self::timeline_changed_factory(),
                    )
                    .await?
            };

        // Sliding sync lazy-loads members only for recent senders ($LAZY
        // required_state), so older messages render with a raw MXID and no
        // avatar until the full list is known. Fetch it once per room (this
        // covers the send/edit/react paths; the display path is handled in
        // get_timeline_slice). Guarded + backgrounded so it never re-runs or
        // blocks the first render.
        self.trigger_member_fetch(room_id, timeline.clone());

        Ok(timeline)
    }

    /// Resolve an avatar via a server thumbnail (see MediaTransferService::resolve_avatar).
    pub async fn resolve_avatar(&self, mxc_url: &str, size: u32) -> Result<PathBuf> {
        let client = self.require_client().await?;
        self.media_transfer
            .resolve_avatar(&client, mxc_url, size)
            .await
    }

    /// Resolve an mxc:// URL to a local file path with local progress updates.
    pub async fn resolve_media_with_progress<F>(
        &self,
        mxc_url: &str,
        progress: F,
    ) -> Result<PathBuf>
    where
        F: Fn(u64, u64, u32) + Send + Sync,
    {
        let client = self.require_client().await?;
        self.media_transfer
            .resolve_media_with_progress(&client, mxc_url, progress)
            .await
    }

    /// Resolve an mxc:// URL into decrypted bytes without writing plaintext to
    /// the temporary media directory.
    pub async fn resolve_media_bytes_with_progress<F>(
        &self,
        mxc_url: &str,
        progress: F,
    ) -> Result<Vec<u8>>
    where
        F: Fn(u64, u64, u32) + Send + Sync,
    {
        let client = self.require_client().await?;
        self.media_transfer
            .resolve_media_bytes_with_progress(&client, mxc_url, progress)
            .await
    }

    /// Resolve a server-generated thumbnail for a media URL. `allow_partial_video`
    /// enables the 2MB partial-download fallback (video frame extraction); pass
    /// false for OG-card / image thumbnails.
    pub async fn resolve_media_thumbnail(
        &self,
        mxc_url: &str,
        width: u32,
        height: u32,
        allow_partial_video: bool,
    ) -> Result<PathBuf> {
        let client = self.require_client().await?;
        self.media_transfer
            .resolve_media_thumbnail(&client, mxc_url, width, height, allow_partial_video)
            .await
    }

    /// Resolve a server-generated thumbnail into decrypted bytes.
    pub async fn resolve_media_thumbnail_bytes(
        &self,
        mxc_url: &str,
        width: u32,
        height: u32,
    ) -> Result<Vec<u8>> {
        let client = self.require_client().await?;
        self.media_transfer
            .resolve_media_thumbnail_bytes(&client, mxc_url, width, height)
            .await
    }

    /// Produce a locally-decoded JPEG thumbnail for a video, cached per event.
    pub async fn get_video_thumbnail(
        &self,
        event_id: &str,
        mxc_url: &str,
        width: u32,
        height: u32,
    ) -> Result<Vec<u8>> {
        let client = self.require_client().await?;
        self.media_transfer
            .get_video_thumbnail(&client, event_id, mxc_url, width, height)
            .await
    }

    /// Export media directly to a user-selected path. This avoids keeping a
    /// decrypted temp file solely to support "Save As".
    pub async fn export_media_to_path(&self, mxc_url: &str, target_path: &str) -> Result<()> {
        self.media_transfer
            .export_media_to_path(|| self.require_client(), mxc_url, target_path)
            .await
    }

    /// Get session info for persistence after login.
    pub async fn get_session_info(&self) -> Result<SessionInfo> {
        let client = self.require_client().await?;
        SessionStorageService::current_session_info(&client)
    }

    /// Restore a session from saved tokens.
    pub async fn restore_session(&self, info: &SessionInfo) -> Result<UserProfile> {
        let restore_started = std::time::Instant::now();
        self.auth_generation.fetch_add(1, Ordering::SeqCst);
        self.stop_sliding_backend().await;
        self.session_tasks.abort_current_generation();
        // Those tasks own the retry-chain claims; an aborted chain never releases
        // its own, so a stale claim would silently disable retries for that room.
        self.backup_retry_rooms.clear();
        self.backup_prefetched_rooms.clear();
        self.session_lifecycle.clear_media_sources();
        let store_passphrase = self.session_lifecycle.prepare_restore_session(
            &info.homeserver,
            &info.user_id,
            &info.device_id,
        )?;
        debug!(
            "restore_session: secrets + cache stores ready in {:?}",
            restore_started.elapsed()
        );
        let build_started = std::time::Instant::now();
        let client = match self
            .auth
            .build_client(&info.homeserver, &store_passphrase)
            .await
        {
            Ok(client) => client,
            Err(e) => {
                // A store that won't open (sqlite corruption / cipher mismatch)
                // can't be repaired in place — signal the UI to force a logout
                // rather than silently dropping to the login screen.
                if is_store_corruption_error(&e) {
                    warn!("restore_session: local store is corrupt at open: {e}");
                    self.report_store_corruption();
                }
                return Err(e);
            }
        };
        debug!(
            "restore_session: SDK client built in {:?}",
            build_started.elapsed()
        );

        let session = matrix_sdk::authentication::matrix::MatrixSession {
            meta: matrix_sdk::SessionMeta {
                user_id: info
                    .user_id
                    .as_str()
                    .try_into()
                    .map_err(|_| anyhow!("Invalid user_id"))?,
                device_id: info.device_id.as_str().into(),
            },
            tokens: matrix_sdk::authentication::SessionTokens {
                access_token: info.access_token.clone(),
                refresh_token: None,
            },
        };

        let sdk_restore_started = std::time::Instant::now();
        tokio::time::timeout(Duration::from_secs(15), client.restore_session(session))
            .await
            .map_err(|_| anyhow!("Session restore timed out"))??;

        info!(
            "Session restored for {} on {} (SDK restore took {:?})",
            info.user_id,
            info.homeserver,
            sdk_restore_started.elapsed()
        );

        {
            let mut c = self.client.write().await;
            *c = Some(client.clone());
        }

        self.session_lifecycle
            .prepopulate_restored_rooms(&client)
            .await;

        // Start the sync loop BEFORE the own-profile fetch below. The profile
        // fetch is a ~2s network round-trip whose result is only used for the
        // self avatar/display name; gating sync (and therefore the timeline)
        // on it delayed the first messages by ~2s for no reason. Client clones
        // cheaply (Arc inside), so sync runs concurrently with the fetch.
        self.start_sync(client.clone());

        let profile_started = std::time::Instant::now();
        let user_id = info.user_id.clone();
        let profile = self
            .session_lifecycle
            .restored_profile(&client, user_id)
            .await;
        debug!(
            "restore_session: profile fetch took {:?}",
            profile_started.elapsed()
        );

        info!(
            "restore_session: complete in {:?}; sync loop started before profile fetch",
            restore_started.elapsed()
        );
        Ok(profile)
    }

    /// Logout and clear state.
    pub async fn logout(&self) -> Result<()> {
        // Snapshot what a *newer* sign-in would move. The C++ leftover-data cleanup
        // re-enables the login form on a 15s safety timer even when this teardown is
        // still running, so a sign-in can complete underneath us; everything at the
        // tail of this function is destructive, and none of it may touch a session
        // that started after we did.
        let generation = self.auth_generation.load(Ordering::SeqCst);

        // Take the client (if any) instead of requiring one: the local wipe
        // must run even when no session is active (e.g. restore failed), or
        // a damaged store would survive the logout.
        let client = {
            let mut c = self.client.write().await;
            c.take()
        };
        // Captured before teardown: the profile memo is partitioned by account, and
        // only the departing one may be dropped.
        let owner = client
            .as_ref()
            .map(crate::profile_cache_store::owner_key)
            .unwrap_or_default();
        // Capture this account's session identity before the client is consumed by
        // the match below; the scoped keychain delete at the tail needs it to name
        // the departing account's session-scoped keys. See MA-1.
        let logout_session = client
            .as_ref()
            .and_then(|c| SessionStorageService::current_session_info(c).ok());
        self.stop_sliding_backend().await;
        self.session_tasks.abort_current_generation();
        // Those tasks own the retry-chain claims; an aborted chain never releases
        // its own, so a stale claim would silently disable retries for that room.
        self.backup_retry_rooms.clear();
        self.backup_prefetched_rooms.clear();

        // Stream server holds the access token; stop it with the session. Take the
        // handle out from under the lock first, then stop (which awaits the download
        // cancellation) without holding the stream_server mutex across the await.
        let stream_server = self.stream_server.lock().await.take();
        if let Some(s) = stream_server {
            s.stop().await;
        }

        self.session_lifecycle
            .clear_runtime_state_for_logout(&owner)
            .await;
        self.encryption.clear_pending_recovery_key().await;
        match client {
            Some(client) => {
                // Null the C++-facing callback slots before tearing the client
                // down, so a late SDK event handler (verification/presence) firing
                // during the remote-logout window can't marshal into a UI being
                // disposed. Only when there IS an active session: the no-session
                // startup leftover-data cleanup (startUnauthorisedCleanup) calls
                // logout() on the SAME bridge it then reuses for the next login,
                // and registerCallbacks() runs only once in the C++ ctor — so
                // clearing here would leave its sync-state/room-list callbacks
                // permanently dead (stuck loading bar, no folders/pinned) until an
                // app restart.
                self.clear_callbacks();
                self.session_lifecycle
                    .logout_remote_best_effort(&client)
                    .await;
                // Release our store handles before the wipe below.
                drop(client);
            }
            None => {
                warn!("logout: no active client; skipping server-side logout");
            }
        }
        // A sign-in that began while we were tearing down owns the store, the cache
        // handles and the pending store passphrase that the local wipe below would
        // destroy — leave all of it alone. (The runtime teardown above ran when
        // logout was called, before any new session could exist.)
        if self.auth_generation.load(Ordering::SeqCst) != generation {
            warn!(
                "logout: a new sign-in started during teardown; \
                 skipping the local wipe so it cannot destroy the new session"
            );
            return Ok(());
        }

        // Run the full local wipe; capture its result so a failed store wipe is
        // surfaced rather than masked by an unconditional Ok below.
        let cleanup_result = self.session_lifecycle.finish_logout_local_cleanup().await;

        // Defense in depth: delete THIS account's Keychain secrets from the Rust
        // side too, so a C++ caller that forgets to scrub them can't leave the
        // access token / store passphrases behind. Scoped to the departing account
        // only (its six keys) — a sibling account that stays signed in must keep
        // working; the wholesale/last-account wipe is the C++ side's job, guarded by
        // !hasAnySession(). See code-review-2026-07-19 MA-1.
        let keychain_result =
            SessionStorageService::delete_account_secrets(logout_session.as_ref(), &self.data_dir);
        if let Err(ref e) = keychain_result {
            error!("logout: failed to delete this account's keychain secrets: {e}");
        }

        cleanup_result?;
        keychain_result?;

        info!("Logged out and cleared all local data");
        Ok(())
    }

    /// Return cache size statistics for the data directory.
    pub async fn get_cache_stats(&self) -> crate::cache_manager::CacheStats {
        self.local_cache.cache_stats().await
    }

    /// Evict media cache files using age + LRU policy. Returns bytes freed.
    pub async fn clear_media_cache(&self, max_age_days: u32, size_limit_bytes: u64) -> Result<u64> {
        self.media_transfer
            .clear_media_files(max_age_days, size_limit_bytes)
            .await
    }

    /// Clear all caches: media files, preview DB, app cache, and the progressive
    /// video-stream cache. The E2EE search index is preserved (it's the only E2EE
    /// search source, and rebuilding it needs a slow re-backfill).
    pub async fn clear_all_caches(&self) -> Result<u64> {
        let mut freed = self.session_lifecycle.clear_cache_data().await;
        // Drop the live video-stream entries first (so they don't reference the
        // about-to-be-deleted files), then wipe + recreate the on-disk cache dir.
        if let Some(server) = self.stream_server.lock().await.as_ref() {
            server.clear_cache_entries().await;
        }
        freed += crate::cache_manager::clear_stream_cache(&self.data_dir).await;
        Ok(freed)
    }

    /// Persist the recent-emoji list to the server (and local cache). Called when
    /// the user picks an emoji; `pairs` is the full ordered `[emoji, count]` list.
    pub async fn set_recent_emoji(&self, pairs: Vec<(String, u32)>) -> Result<()> {
        let client = self.require_client().await?;
        self.recent_emoji.save(&client, pairs).await
    }

    /// Locally-cached recent emojis (app_cache.db) for instant startup display.
    pub fn recent_emoji_local(&self) -> Vec<(String, u32)> {
        self.recent_emoji.local()
    }

    /// Spawn a background task that evicts media files if over the size limit.
    pub fn auto_cleanup(&self, size_limit_bytes: u64) {
        // Spawn via session_tasks so logout's abort_current_generation cancels the
        // eviction instead of letting it race the logout cache wipe.
        let lifecycle = self.session_lifecycle.clone();
        self.session_tasks.spawn(async move {
            lifecycle.run_auto_cleanup(size_limit_bytes).await;
        });
    }

    /// Push the media-cache size budget from the app settings: bound the SDK
    /// thumbnail store via its retention policy, and immediately enforce the app
    /// media-cache + video-stream budgets (so lowering the limit shrinks now).
    pub fn set_media_cache_limit(&self, limit_bytes: u64) {
        crate::cache_manager::set_media_cache_limit_bytes(limit_bytes);
        let data_dir = self.data_dir.clone();
        let client = self.client.clone();
        let stream_server = self.stream_server.clone();
        self.session_tasks.spawn(async move {
            // Bound the SDK thumbnail store (matrix-sdk-media.sqlite3): it isn't
            // covered by our file-cache eviction and is otherwise unbounded.
            if let Some(client) = client.read().await.clone() {
                let policy = matrix_sdk::media::MediaRetentionPolicy::new()
                    .with_max_cache_size(Some(crate::cache_manager::sdk_thumb_reserve_bytes()))
                    .with_max_file_size(Some(2 * 1024 * 1024))
                    .with_last_access_expiry(Some(Duration::from_secs(30 * 24 * 60 * 60)))
                    .with_cleanup_frequency(Some(Duration::from_secs(24 * 60 * 60)));
                match client.media().set_media_retention_policy(policy).await {
                    Ok(()) => {
                        let _ = client.media().clean().await;
                    }
                    Err(e) => warn!("set media retention policy failed: {e}"),
                }
            }
            // App media_cache/ — evict to the 50% low watermark right now.
            let _ = crate::cache_manager::enforce_media_cache_limit(&data_dir).await;
            // Video stream cache — enforce its (now setting-derived) budget too.
            if let Some(server) = stream_server.lock().await.as_ref() {
                server.enforce_stream_budget().await;
            }
        });
    }

    /// Enable/disable local search indexing of E2EE rooms (from the app setting).
    ///
    /// Off: stop the backfill + live indexers and wipe the local FTS index
    /// (reclaiming disk). On: reopen the index and (re)spawn the backfill worker;
    /// live indexing resumes on its own. No-op when the state is unchanged. The
    /// global flag is set synchronously so queries observe it immediately; the
    /// store/worker work runs on the session runtime.
    pub fn set_e2ee_search_enabled(&self, enabled: bool) {
        let was = crate::search_service::e2ee_search_enabled();
        crate::search_service::set_e2ee_search_enabled(enabled);
        if enabled == was {
            return;
        }
        let client = self.client.clone();
        let local_cache = self.local_cache.clone();
        let session_tasks = self.session_tasks.clone();
        let index = self.search.index.clone();
        let fingerprints = self.search.fingerprints.clone();
        let data_dir = self.data_dir.clone();
        self.session_tasks.spawn(async move {
            if enabled {
                // Open the index and spawn the backfill worker (re-indexes E2EE
                // history from scratch; live indexing resumes automatically).
                if let Some(client) = client.read().await.clone() {
                    let key = LocalCacheService::search_index_key_material(&client);
                    local_cache.ensure_search_index_open(key.as_deref());
                    session_tasks.spawn(crate::search_backfill::run(client, index));
                }
            } else {
                // Drop the index connection and delete its files under the lock so
                // a concurrent ensure_search_index_open() can't reopen+recreate it
                // in the gap. The backfill + live indexers self-exit on a `None`
                // index (and the global flag already short-circuits them).
                {
                    let mut guard = fingerprints.write().await;
                    guard.clear();
                }
                if let Ok(mut guard) = index.lock() {
                    *guard = None;
                    let _ = crate::encrypted_sqlite::delete_database_files(
                        &data_dir.join("search_index.db"),
                    );
                }
            }
        });
    }

    /// Abort all background session tasks (sync loop, watchers).
    pub fn abort_background_tasks(&self) {
        self.session_tasks.abort_current_generation();
        // Those tasks own the retry-chain claims; an aborted chain never releases
        // its own, so a stale claim would silently disable retries for that room.
        self.backup_retry_rooms.clear();
        self.backup_prefetched_rooms.clear();
    }
}

// ----- Verification helper methods -----

impl MatrixProtocol {
    /// Register a callback that fires when the verification state changes.
    /// The callback receives the state discriminant and the flow id it pertains
    /// to (empty when there is no specific flow).
    pub fn on_verification_state_changed(&self, callback: VerificationStateCallback) {
        self.verification.on_state_changed(callback);
    }

    /// Register a callback that fires when another own device requests verification.
    pub fn on_incoming_verification_request(&self, callback: VerificationIncomingRequestCallback) {
        self.verification.on_incoming_request(callback);
    }

    /// Register a callback that fires when another user's identity trust changes.
    pub fn on_user_trust_changed(&self, callback: UserTrustChangedCallback) {
        self.verification.on_user_trust_changed(callback);
    }

    /// Register a callback that fires when another user requests to verify with us.
    pub fn on_incoming_user_verification_request(
        &self,
        callback: VerificationIncomingUserRequestCallback,
    ) {
        self.verification.on_incoming_user_request(callback);
    }

    /// Register a callback that fires when an incoming verification request can
    /// no longer be answered, so the UI can take its banner down.
    pub fn on_verification_request_closed(&self, callback: VerificationRequestClosedCallback) {
        self.verification.on_request_closed(callback);
    }

    /// Register a callback that fires when a SAS flow's emojis become available,
    /// including a SAS the other device started.
    pub fn on_sas_emojis(&self, callback: VerificationSasEmojisCallback) {
        self.verification.on_sas_emojis(callback);
    }

    /// Register a callback that fires when a QR code has been generated.
    pub fn on_qr_data(&self, callback: VerificationQrDataCallback) {
        self.verification.on_qr_data(callback);
    }

    /// Register a callback that fires with a flow's SDK cancel code just
    /// before its `Cancelled` state, so the UI can pick a failure severity.
    pub fn on_verification_cancel_info(&self, callback: VerificationCancelInfoCallback) {
        self.verification.on_cancel_info(callback);
    }

    /// Read another user's cross-signing trust state (for trust shields).
    pub async fn user_trust_state(&self, user_id: &str) -> Result<UserTrustState> {
        let client = self.require_client().await?;
        self.verification.user_trust_state(&client, user_id).await
    }

    /// Start an interactive SAS (emoji) verification of another user's identity.
    /// Initiate-only; the emojis arrive via `on_sas_emojis`.
    pub async fn start_user_verification(&self, user_id: &str) -> Result<()> {
        let client = self.require_client().await?;
        self.verification
            .start_user_verification(client, user_id)
            .await
    }

    /// Withdraw our verification of another user's identity.
    pub async fn withdraw_user_verification(&self, user_id: &str) -> Result<()> {
        let client = self.require_client().await?;
        self.verification
            .withdraw_user_verification(&client, user_id)
            .await
    }

    pub async fn start_sas_verification_for(&self, expected_flow_id: &str) -> Result<()> {
        self.verification
            .start_sas_verification_for(expected_flow_id)
            .await
    }

    pub async fn start_qr_verification_for(&self, expected_flow_id: &str) -> Result<()> {
        self.verification
            .start_qr_verification_for(expected_flow_id)
            .await
    }

    pub async fn cancel_verification_for(&self, expected_flow_id: &str) -> Result<()> {
        self.verification
            .cancel_verification_for(expected_flow_id)
            .await
    }

    /// Re-deliver a still-answerable pending incoming request to whichever
    /// incoming-request callback is currently attached. Call after attaching a
    /// consumer (main window, intro) that may have missed the original signal.
    pub async fn replay_pending_incoming_request(&self) {
        self.verification.replay_pending_incoming_request().await;
    }
}

/// When to re-ask the backup for a room's keys while some of its messages are still
/// undecryptable. Nothing tells a client that *new* keys reached the backup — the
/// device that owns them uploads on its own schedule (and only once it is online),
/// so the newest messages are routinely missing at the moment we first look. Both
/// key-fetch paths are one-shot: the SDK's per-UTD `BackupDownloadTask` caches the
/// session in its downloaded/failure caches and backs off. Without these retries a
/// key that lands in backup a minute later goes unnoticed until the room is
/// reopened or the app restarts. Delays are cumulative from room-open; the schedule
/// gives up after ~9 minutes, by which point the keys are almost certainly not
/// coming.
///
/// The short first step stands in for the old immediate whole-room prefetch: it
/// lets the just-opened room's timeline populate the cache so the UTD gate below
/// can tell whether a backup fetch is even warranted. A fully-decrypted room (all
/// keys already received live) then never hits the backup — which is what stops
/// `download_room_keys_for_room` from re-decrypting keys we already hold and the
/// crypto store logging "received a room key we already have … discarding".
const BACKUP_RETRY_SCHEDULE: &[Duration] = &[
    Duration::from_secs(2),
    Duration::from_secs(20),
    Duration::from_secs(40),
    Duration::from_secs(120),
    Duration::from_secs(360),
];

/// The rendered timeline cache (room id -> items) — the same handle the UI reads.
type TimelineCacheRef = Arc<RwLock<HashMap<String, Vec<TimelineItem>>>>;

/// Rooms that already have a backup-key retry chain running, so reopening a room
/// doesn't stack a second one on top.
#[derive(Clone, Default)]
struct BackupRetryRooms(Arc<Mutex<std::collections::HashSet<OwnedRoomId>>>);

impl BackupRetryRooms {
    /// True when the room had no chain yet — the caller now owns it.
    fn claim(&self, room: OwnedRoomId) -> bool {
        self.0
            .lock()
            .map(|mut rooms| rooms.insert(room))
            .unwrap_or(false)
    }

    fn release(&self, room: &OwnedRoomId) {
        if let Ok(mut rooms) = self.0.lock() {
            rooms.remove(room);
        }
    }

    fn clear(&self) {
        if let Ok(mut rooms) = self.0.lock() {
            rooms.clear();
        }
    }
}

/// UTD state of a room's cached timeline, distinguishing "not in the cache yet"
/// from "in the cache with nothing stuck".
///
/// The difference matters: the cache is populated by the UI as a room
/// materializes, so a chase that starts at room-open can easily look at it
/// before the slice lands. Treating that (`None`) the same as "finished"
/// permanently abandons a room that in fact needs its keys — the bug that left
/// rooms undecryptable after verification.
async fn room_utd_state(cache: &TimelineCacheRef, room_id: &str) -> Option<bool> {
    let guard = cache.read().await;
    guard.get(room_id).map(|items| {
        items
            .iter()
            .any(|item| matches!(item.content, MessageContent::UnableToDecrypt { .. }))
    })
}

/// Distinct megolm session ids still undecryptable, NEWEST FIRST.
///
/// The cached vector is oldest→newest, so this walks it in reverse. Order is the
/// point: an arriving room key makes the SDK redecrypt exactly that session's
/// events (`redecryptor::retry_decryption` is session-scoped), so importing the
/// newest sessions first decrypts what the user is actually looking at first,
/// instead of whatever order the backup happens to return.
async fn undecrypted_sessions_newest_first(
    cache: &TimelineCacheRef,
    room_id: &str,
    limit: usize,
) -> Vec<String> {
    let guard = cache.read().await;
    let Some(items) = guard.get(room_id) else {
        return Vec::new();
    };
    let mut seen = std::collections::HashSet::new();
    let mut out = Vec::new();
    for item in items.iter().rev() {
        if let MessageContent::UnableToDecrypt {
            session_id: Some(sid),
            ..
        } = &item.content
        {
            if seen.insert(sid.clone()) {
                out.push(sid.clone());
                if out.len() >= limit {
                    break;
                }
            }
        }
    }
    out
}

/// The distinct megolm session ids still undecryptable in the room's cached
/// timeline — the keys a backup fetch could actually supply. UTDs without a
/// session id (non-megolm) are omitted: the backup is keyed by session id, so
/// re-downloading it cannot resolve them.
async fn undecrypted_session_ids(
    cache: &TimelineCacheRef,
    room_id: &str,
) -> std::collections::HashSet<String> {
    let guard = cache.read().await;
    guard
        .get(room_id)
        .map(|items| {
            items
                .iter()
                .filter_map(|item| match &item.content {
                    MessageContent::UnableToDecrypt {
                        session_id: Some(sid),
                        ..
                    } => Some(sid.clone()),
                    _ => None,
                })
                .collect()
        })
        .unwrap_or_default()
}

/// How many of the newest stuck sessions to fetch individually before the
/// whole-room download. Small: these are per-session round trips, and their only
/// job is to get the screenful the user is staring at decrypted first.
const NEWEST_FIRST_SESSION_FETCHES: usize = 10;

const KEY_READINESS_PROBE_INTERVAL: Duration = Duration::from_secs(15);
/// 40 minutes of ticks — a post-verification backlog is the slow case we are
/// trying to time, so the probe has to outlive it.
const KEY_READINESS_PROBE_TICKS: usize = 160;

/// Rooms with UTDs, distinct stuck megolm sessions, and stuck items across the
/// whole UI cache.
async fn utd_totals(cache: &TimelineCacheRef) -> (usize, usize, usize) {
    let guard = cache.read().await;
    let mut rooms = 0usize;
    let mut items = 0usize;
    let mut sessions = std::collections::HashSet::new();
    for room_items in guard.values() {
        let mut room_stuck = false;
        for item in room_items {
            if let MessageContent::UnableToDecrypt { session_id, .. } = &item.content {
                items += 1;
                room_stuck = true;
                if let Some(sid) = session_id {
                    sessions.insert(sid.clone());
                }
            }
        }
        if room_stuck {
            rooms += 1;
        }
    }
    (rooms, sessions.len(), items)
}

/// Log-only. Reports whether a UTD can *ever* resolve on this device.
///
/// "Decryption never finishes" has three causes that are indistinguishable from
/// the UI and from the SDK's own logs, because every one of them ends in the same
/// `Can't find the room key` warning: (1) the backup key never reached this device,
/// so `are_enabled()` stays false and every fetch path we have is a silent no-op;
/// (2) the backup key is here but the backup does not hold those sessions; (3) keys
/// are arriving, just slowly. Only (3) is a speed problem, and only this tells them
/// apart.
fn spawn_key_readiness_probe(
    client: Client,
    cache: TimelineCacheRef,
    runtime_handle: &tokio::runtime::Handle,
) -> tokio::task::JoinHandle<()> {
    runtime_handle.spawn(async move {
        let backups = client.encryption().backups();
        match backups.exists_on_server().await {
            Ok(exists) => info!("[keys] backup exists on server: {exists}"),
            Err(e) => warn!("[keys] could not ask the server whether a backup exists: {e}"),
        }
        // Every tick, unconditionally: the drain *curve* is the measurement. A
        // linear cost per session drains at a steady rate; a per-success sweep of
        // everything still stuck costs time proportional to what remains, so it
        // crawls at first and visibly accelerates as it empties.
        let started = std::time::Instant::now();
        for _ in 0..KEY_READINESS_PROBE_TICKS {
            let enabled = backups.are_enabled().await;
            let (rooms, sessions, items) = utd_totals(&cache).await;
            info!(
                "[keys] t={}s backup={:?} usable={} recovery={:?} rooms_with_utd={} utd_sessions={} utd_items={} failed_decrypts={}",
                started.elapsed().as_secs(),
                backups.state(),
                enabled,
                client.encryption().recovery().state(),
                rooms,
                sessions,
                items,
                crate::log_noise::suppressed_count()
            );
            if enabled && items == 0 {
                info!(
                    "[keys] all cached rooms decrypted after {}s",
                    started.elapsed().as_secs()
                );
                return;
            }
            tokio::time::sleep(KEY_READINESS_PROBE_INTERVAL).await;
        }
    })
}

/// Fetch a freshly-opened room's keys, newest sessions first, then the rest.
///
/// Order is the whole point. An arriving room key makes the SDK redecrypt exactly
/// that session's events, so fetching the newest stuck sessions individually makes
/// the visible bottom of the timeline resolve first; the whole-room download then
/// sweeps up everything older in one request. Without this the only ordering is
/// whatever order the backup returns keys in, which is why decryption appeared to
/// start at random points in the past.
///
/// Keys only — decryption stays SDK-reactive (do not grow this into a decryption
/// loop; that fights R2D2).
async fn prefetch_room_keys_prioritized(
    client: &Client,
    room_id: &OwnedRoomId,
    cache: &TimelineCacheRef,
) {
    let backups = client.encryption().backups();
    if !backups.are_enabled().await {
        // Nothing to fetch from yet; the per-room chase retries on its schedule.
        debug!("[keys] on-open fetch for {room_id} skipped: backup not usable yet");
        return;
    }

    // Newest-first, best effort: a failure here is not terminal because the
    // whole-room download below covers the same sessions.
    let newest =
        undecrypted_sessions_newest_first(cache, room_id.as_str(), NEWEST_FIRST_SESSION_FETCHES)
            .await;
    for session_id in &newest {
        if let Err(e) = backups.download_room_key(room_id, session_id).await {
            debug!("[keys] newest-first key fetch for {room_id}/{session_id} failed: {e}");
        }
    }
    if !newest.is_empty() {
        debug!(
            "[keys] fetched {} newest session(s) for {room_id} before the bulk sweep",
            newest.len()
        );
    }

    match backups.download_room_keys_for_room(room_id).await {
        Ok(()) => {
            let left = undecrypted_session_ids(cache, room_id.as_str()).await.len();
            debug!("[keys] on-open bulk fetch for {room_id} done; {left} session(s) still stuck");
        }
        Err(e) => debug!("[keys] on-open bulk key fetch for {room_id} failed: {e}"),
    }
}

/// Fetch a room's keys from the backup while it still shows undecryptable messages,
/// on the schedule in [`BACKUP_RETRY_SCHEDULE`] (the short first step doubles as the
/// initial on-open fetch). Skips entirely when the room has no UTDs, and never asks
/// twice for the same sessions — that is what stops the whole-room download from
/// re-decrypting keys we already hold.
///
/// This fetches KEYS only. Decryption itself stays reactive: the imported sessions
/// land on the OlmMachine's room-key stream and the SDK's redecryptor re-runs the
/// affected events. Do not grow this into a decryption loop — that fights R2D2.
async fn retry_room_keys_from_backup(
    client: Client,
    room_id: OwnedRoomId,
    cache: TimelineCacheRef,
) {
    // Session ids we have already asked the backup for. Re-downloading the whole
    // room for the SAME sessions just re-decrypts (curve25519) keys we already
    // imported and the crypto store discards them ("received a room key we
    // already have … discarding") — pure waste that showed up as CPU bursts
    // while lingering in a UTD-heavy room. A session the backup lacks now it
    // will still lack on a bare re-poll, and any key that lands in backup later
    // is caught per-session by the AfterDecryptionFailure download strategy.
    let mut requested: std::collections::HashSet<String> = std::collections::HashSet::new();
    for delay in BACKUP_RETRY_SCHEDULE {
        tokio::time::sleep(*delay).await;

        match room_utd_state(&cache, room_id.as_str()).await {
            // Genuinely finished: everything decrypted. Stop asking.
            Some(false) => return,
            // Still stuck — carry on to the fetch below.
            Some(true) => {}
            // Not in the UI cache yet. The room is still materializing (the first
            // tick fires seconds after open), so this is "unknown", NOT "done" —
            // returning here is what abandoned rooms permanently.
            None => continue,
        }
        if !client.encryption().backups().are_enabled().await {
            // Backup is not usable *yet*. After a QR verification it becomes usable
            // once secret storage opens and the backup key is imported, which is
            // routinely later than the first tick — so wait for a later tick rather
            // than killing the chain.
            continue;
        }

        // Only hit the backup when a session we have not tried yet is still
        // stuck — e.g. the user scrolled fresh UTD history into view. One
        // whole-room download still fetches every missing session at once (one
        // request, not one per session), it just no longer fires when nothing
        // new appeared since the last round.
        let missing = undecrypted_session_ids(&cache, room_id.as_str()).await;
        if missing.is_subset(&requested) {
            continue;
        }

        debug!("[keys] retrying backup-key fetch for {room_id} (new UTD sessions)");
        match client
            .encryption()
            .backups()
            .download_room_keys_for_room(&room_id)
            .await
        {
            // Mark as tried only on success: a transient failure should be
            // retried next round, not skipped for the rest of the chain.
            Ok(()) => requested.extend(missing),
            Err(e) => debug!("[keys] backup-key retry for {room_id} failed: {e}"),
        }
    }
    // Schedule exhausted. If the room is still stuck here, no further backup fetch
    // will happen for it this session — say so rather than going quiet.
    if room_utd_state(&cache, room_id.as_str()).await == Some(true) {
        let left = undecrypted_session_ids(&cache, room_id.as_str())
            .await
            .len();
        warn!("[keys] giving up on {room_id}: {left} session(s) still undecryptable");
    }
}

// ----- ProtocolClient trait implementation -----

#[async_trait]
impl ProtocolClient for MatrixProtocol {
    async fn login(&self, homeserver: &str, user: &str, pass: &str) -> Result<UserProfile> {
        self.auth_generation.fetch_add(1, Ordering::SeqCst);
        self.stop_sliding_backend().await;
        self.session_tasks.abort_current_generation();
        // Those tasks own the retry-chain claims; an aborted chain never releases
        // its own, so a stale claim would silently disable retries for that room.
        self.backup_retry_rooms.clear();
        self.backup_prefetched_rooms.clear();
        let previous_owner = self.current_profile_owner().await;
        self.session_lifecycle
            .prepare_fresh_login(&previous_owner)
            .await?;

        let store_passphrase = self.session_storage.pending_auth_store_passphrase()?;
        let client = self
            .auth
            .build_client(homeserver, &store_passphrase)
            .await?;

        let profile = self
            .auth
            .login(&client, user, pass, &store_passphrase)
            .await?;

        {
            let mut c = self.client.write().await;
            *c = Some(client.clone());
        }

        self.start_sync(client);

        Ok(profile)
    }

    async fn get_rooms(&self) -> Result<Vec<RoomSummary>> {
        Ok(self
            .room_list
            .current_rooms_or_cached(self.local_cache.app_cache_store())
            .await)
    }

    async fn get_room_members(&self, room_id: &str) -> Result<Vec<UserProfile>> {
        let room = self.get_room(room_id).await?;
        RoomMemberService::get_room_members(room, room_id).await
    }

    async fn send_message(
        &self,
        room_id: &str,
        body: &str,
        formatted_body: Option<&str>,
        reply_to_event_id: Option<&str>,
    ) -> Result<String> {
        let timeline = self.get_or_create_timeline(room_id).await?;
        MessageActionService::send_message(timeline, body, formatted_body, reply_to_event_id).await
    }

    async fn edit_message(
        &self,
        room_id: &str,
        event_id: &str,
        body: &str,
        formatted_body: Option<&str>,
        as_media_caption: bool,
    ) -> Result<String> {
        let timeline = self.get_or_create_timeline(room_id).await?;
        MessageActionService::edit_message(
            timeline,
            event_id,
            body,
            formatted_body,
            as_media_caption,
        )
        .await
    }

    async fn delete_message(&self, room_id: &str, event_id: &str) -> Result<()> {
        let timeline = self.get_or_create_timeline(room_id).await?;
        MessageActionService::delete_message(timeline, event_id).await
    }

    async fn get_pinned_messages(&self, room_id: &str) -> Result<Vec<TimelineItem>> {
        let room = self.get_room(room_id).await?;
        let client = self.require_client().await?;
        MessageActionService::get_pinned_messages(
            room,
            room_id,
            client,
            self.timeline.reply_preview_cache.clone(),
            self.preview.cache.clone(),
        )
        .await
    }

    async fn set_audio_duration(&self, mxc: &str, duration_ms: u64) -> Result<()> {
        let client = self.require_client().await?;
        crate::audio_duration_store::store(&client, mxc.to_string(), duration_ms).await;
        Ok(())
    }

    async fn pin_message(&self, room_id: &str, event_id: &str, pinned: bool) -> Result<()> {
        let room = self.get_room(room_id).await?;
        MessageActionService::pin_message(room, event_id, pinned).await
    }

    async fn unpin_all_messages(&self, room_id: &str) -> Result<()> {
        let room = self.get_room(room_id).await?;
        MessageActionService::unpin_all_messages(room).await
    }

    async fn pin_room(&self, room_id: &str, pinned: bool, order: Option<f64>) -> Result<()> {
        let room = self.get_room(room_id).await?;
        let client = if pinned {
            None
        } else {
            Some(self.require_client().await?)
        };
        RoomActionService::pin_room(room, client, pinned, order).await
    }

    async fn set_pinned_order(&self, room_ids: Vec<String>) -> Result<()> {
        let client = self.require_client().await?;
        RoomActionService::set_pinned_order(client, room_ids).await
    }

    async fn set_room_notification_mode(
        &self,
        room_id: &str,
        mode: crate::types::RoomNotificationMode,
    ) -> Result<()> {
        let client = self.require_client().await?;
        let room = self.get_room(room_id).await?;
        RoomActionService::set_room_notification_mode(
            client,
            room,
            room_id,
            mode,
            self.room_list.notification_overrides.clone(),
            self.room_list.rooms.clone(),
            self.local_cache.clone(),
        )
        .await
    }

    async fn get_notification_settings(
        &self,
    ) -> Result<crate::notification_settings_service::NotificationSettings> {
        let client = self.require_client().await?;
        crate::notification_settings_service::NotificationSettingsService::get(&client).await
    }

    async fn set_category_notification_level(
        &self,
        category: crate::notification_settings_service::ChatCategory,
        level: crate::types::RoomNotificationMode,
    ) -> Result<()> {
        let client = self.require_client().await?;
        crate::notification_settings_service::NotificationSettingsService::set_category_level(
            &client, category, level,
        )
        .await
    }

    async fn set_keywords_setting(&self, csv: &str) -> Result<()> {
        let client = self.require_client().await?;
        let desired = crate::notification_settings_service::parse_keywords(csv);
        crate::notification_settings_service::NotificationSettingsService::set_keywords(
            &client, desired,
        )
        .await
    }

    async fn set_notification_toggle(
        &self,
        toggle: crate::notification_settings_service::NotificationToggle,
        enabled: bool,
    ) -> Result<()> {
        let client = self.require_client().await?;
        crate::notification_settings_service::NotificationSettingsService::set_toggle(
            &client, toggle, enabled,
        )
        .await
    }

    async fn mark_room_read(&self, room_id: &str, read: bool) -> Result<()> {
        let room = self.get_room(room_id).await?;
        RoomActionService::mark_room_read(
            room,
            room_id,
            read,
            self.room_list.rooms.clone(),
            self.local_cache.clone(),
        )
        .await?;
        // Our read state just changed — drop the memoized receipt targets so the
        // next timeline update recomputes the read marker / first-unread.
        self.timeline.invalidate_receipt_targets(room_id);
        // Tell C++ to re-pull this room's summary so the rooms-list badge
        // converges without a room reopen. mark_room_read has already written the
        // authoritative unread_count (0 for `read`) into rooms_cache, and
        // get_rooms() returns that cache verbatim.
        //
        // We deliberately do NOT recompute via refresh_room_summary_cache here:
        // matrix-sdk `Room::send_single_receipt` only sends to the server, so the
        // SDK's local counts (num_unread_messages / unread_notification_counts)
        // stay stale-high until the server echoes the receipt on a later sync — a
        // recompute now would stomp the authoritative 0.
        self.emit_room_summary_changed(room_id);
        Ok(())
    }

    async fn send_read_receipt(&self, room_id: &str, event_id: &str) -> Result<()> {
        let room = self.get_room(room_id).await?;
        RoomActionService::send_read_receipt(room, event_id).await?;
        self.timeline.invalidate_receipt_targets(room_id);
        // Tell C++ to re-pull. For the open room the per-room timeline callback
        // routes to get_room_unread_snapshot, which recomputes from the SDK and
        // so converges to the server-fresh count once the receipt echo lands.
        // (No recompute here for the same reason as mark_room_read — the SDK's
        // counts do not reflect our own just-sent receipt until that echo.)
        self.emit_room_summary_changed(room_id);
        Ok(())
    }

    async fn create_room(&self, request: CreateRoomRequest) -> Result<String> {
        let client = self.require_client().await?;
        RoomCreationService::create_room(client, request).await
    }

    async fn upload_room_avatar(
        &self,
        room_id: &str,
        data: Vec<u8>,
        content_type: &str,
    ) -> Result<String> {
        let client = self.require_client().await?;
        let room = self.get_room(room_id).await?;
        RoomActionService::upload_room_avatar(client, room, room_id, data, content_type).await
    }

    async fn delete_room_avatar(&self, room_id: &str) -> Result<()> {
        let room = self.get_room(room_id).await?;
        RoomActionService::delete_room_avatar(room, room_id).await
    }

    async fn set_room_name(&self, room_id: &str, name: &str) -> Result<()> {
        let room = self.get_room(room_id).await?;
        RoomActionService::set_room_name(room, room_id, name).await
    }

    async fn set_room_topic(&self, room_id: &str, topic: &str) -> Result<()> {
        let room = self.get_room(room_id).await?;
        RoomActionService::set_room_topic(room, room_id, topic).await
    }

    async fn leave_room(&self, room_id: &str) -> Result<()> {
        let room = self.get_room(room_id).await?;
        RoomActionService::leave_room(
            room,
            room_id,
            self.room_list.rooms.clone(),
            self.local_cache.clone(),
        )
        .await?;
        // Nudge C++ to re-pull the (now-trimmed) list, in case the caller relies on the
        // room-list callback rather than the leave callback's refreshRooms.
        self.emit_room_summary_changed(room_id);
        Ok(())
    }

    async fn release_room_timeline(&self, room_id: &str) {
        TimelineWindowService::release_room(&self.timeline_runtime(), room_id).await;
    }

    async fn accept_invite(&self, room_id: &str) -> Result<()> {
        let client = self.require_client().await?;
        RoomInviteService::accept_invite(
            client,
            room_id,
            self.timeline_runtime(),
            Self::timeline_changed_factory(),
        )
        .await
    }

    async fn search_public_rooms(
        &self,
        request: RoomDirectoryRequest,
    ) -> Result<RoomDirectoryPage> {
        let client = self.require_client().await?;
        RoomDirectoryService::search_public_rooms(client, request).await
    }

    async fn space_children(&self, request: SpaceHierarchyRequest) -> Result<RoomDirectoryPage> {
        let client = self.require_client().await?;
        RoomDirectoryService::space_children(client, request).await
    }

    async fn room_preview(
        &self,
        room_id_or_alias: &str,
        via: Vec<String>,
    ) -> Result<RoomPreviewInfo> {
        let client = self.require_client().await?;
        RoomDirectoryService::room_preview(client, room_id_or_alias, &via).await
    }

    async fn preview_messages(
        &self,
        room_id: &str,
        from: Option<String>,
        limit: u32,
    ) -> Result<(Vec<TimelineItem>, Option<String>)> {
        let client = self.require_client().await?;
        RoomDirectoryService::preview_messages(
            client,
            room_id,
            from,
            limit,
            &self.timeline.media_sources,
        )
        .await
    }

    async fn join_room(&self, room_id_or_alias: &str, via: Vec<String>) -> Result<String> {
        let client = self.require_client().await?;
        RoomDirectoryService::join_room(
            client,
            room_id_or_alias,
            &via,
            self.timeline_runtime(),
            Self::timeline_changed_factory(),
        )
        .await
    }

    async fn knock_room(&self, room_id_or_alias: &str, via: Vec<String>) -> Result<String> {
        let client = self.require_client().await?;
        RoomDirectoryService::knock_room(client, room_id_or_alias, &via).await
    }

    async fn add_room_to_folder(&self, room_id: &str, tag_key: &str) -> Result<()> {
        let room = self.get_room(room_id).await?;
        self.folders.add_room_to_folder(room, tag_key).await
    }

    async fn create_folder(&self, name: &str) -> Result<String> {
        let client = self.require_client().await?;
        self.folders.create_folder(client, name).await
    }

    async fn edit_folder(&self, tag_key: &str, name: &str) -> Result<String> {
        let client = self.require_client().await?;
        self.folders.rename_folder(client, tag_key, name).await
    }

    async fn delete_folder(&self, tag_key: &str) -> Result<()> {
        let client = self.require_client().await?;
        self.folders.delete_folder(client, tag_key).await
    }

    async fn set_sidebar_order(&self, order: Vec<crate::room_folders::SidebarRef>) -> Result<()> {
        let client = self.require_client().await?;
        self.folders.set_sidebar_order(client, order).await
    }

    async fn get_folders(&self) -> Result<Vec<FolderMeta>> {
        let client = self.client.read().await.clone();
        self.folders.get_folders(client).await
    }

    async fn get_sidebar_order(&self) -> Result<Vec<crate::room_folders::SidebarRef>> {
        Ok(self.folders.cached_sidebar_order().await)
    }

    async fn get_joined_spaces(&self) -> Result<Vec<crate::types::SpaceInfo>> {
        let client = self.require_client().await?;
        let mut spaces = Vec::new();
        for room in client.rooms() {
            if room.state() != matrix_sdk::RoomState::Joined || !room.is_space() {
                continue;
            }
            let name = room
                .cached_display_name()
                .map(|dn| dn.to_string())
                .unwrap_or_else(|| room.room_id().to_string());
            spaces.push(crate::types::SpaceInfo {
                room_id: room.room_id().to_string(),
                name,
                avatar_url: room.avatar_url().map(|u| u.to_string()),
                topic: room.topic().unwrap_or_default(),
                member_count: room.joined_members_count(),
                canonical_alias: room.canonical_alias().map(|a| a.to_string()),
            });
        }
        spaces.sort_by_key(|s| s.name.to_lowercase());
        Ok(spaces)
    }

    async fn forward_message(
        &self,
        src_room_id: &str,
        event_id: &str,
        dst_room_id: &str,
    ) -> Result<String> {
        let payload = MessageActionService::prepare_forward_message(
            src_room_id,
            event_id,
            self.timeline.cache.clone(),
        )
        .await?;
        let dst_timeline = self.get_or_create_timeline(dst_room_id).await?;
        MessageActionService::send_forwarded_message(
            dst_timeline,
            payload,
            &self.timeline.media_sources,
            &self.timeline.pending_forward_meta,
        )
        .await
    }

    async fn send_media(
        &self,
        room_id: &str,
        content: MessageContent,
        transaction_id: Option<String>,
    ) -> Result<String> {
        let timeline = self.get_or_create_timeline(room_id).await?;

        // Remember the source file BEFORE the upload starts, so its echo can
        // seed the media cache rather than download back what we just sent.
        let txn_id = transaction_id.clone().unwrap_or_default();
        if let Some(path) = content.media_url() {
            self.media_transfer
                .register_upload_seed(&txn_id, Path::new(path));
        }

        let result = MessageActionService::send_media(
            timeline,
            content,
            transaction_id,
            self.upload_progress_callback.clone(),
        )
        .await;
        if result.is_err() {
            crate::upload_seed_store::remove(&txn_id);
        }
        result
    }

    async fn set_reaction(
        &self,
        room_id: &str,
        event_id: &str,
        key: &str,
        active: bool,
    ) -> Result<()> {
        let timeline = self.get_or_create_timeline(room_id).await?;
        MessageActionService::set_reaction(
            timeline,
            room_id,
            event_id,
            key,
            active,
            self.timeline.cache.clone(),
            self.timeline.pending_reaction_overrides.clone(),
        )
        .await
    }

    async fn send_poll_vote(
        &self,
        room_id: &str,
        poll_event_id: &str,
        option_ids: Vec<String>,
    ) -> Result<String> {
        let timeline = self.get_or_create_timeline(room_id).await?;
        MessageActionService::send_poll_vote(timeline, poll_event_id, option_ids).await
    }

    async fn get_room_settings(&self, room_id: &str) -> Result<RoomSettingsSnapshot> {
        let client = self.require_client().await?;
        RoomMemberService::get_room_settings_for_client(
            client,
            room_id,
            self.room_list.notification_overrides.clone(),
        )
        .await
    }

    async fn enable_room_encryption(&self, room_id: &str) -> Result<()> {
        let room = self.get_room(room_id).await?;
        RoomMemberService::enable_room_encryption(room).await
    }

    async fn set_room_access(&self, room_id: &str, access: RoomAccess) -> Result<()> {
        let room = self.get_room(room_id).await?;
        RoomMemberService::set_room_access(room, access).await
    }

    async fn set_room_history_visibility(
        &self,
        room_id: &str,
        visibility: crate::types::HistoryVisibility,
    ) -> Result<()> {
        let room = self.get_room(room_id).await?;
        RoomMemberService::set_room_history_visibility(room, visibility).await
    }

    async fn search_messages(&self, request: SearchRequest) -> anyhow::Result<SearchPage> {
        let client = self.require_client().await?;
        self.search
            .search_messages(&client, &self.local_cache, request)
            .await
    }

    // ----- Verification trait methods -----

    async fn start_sas_verification(&self) -> Result<()> {
        let client = self.require_client().await?;
        self.verification
            .start_sas_verification_checked(client, None)
            .await
    }

    async fn confirm_sas_match(&self) -> Result<()> {
        self.verification.confirm_sas_match().await?;
        Ok(())
    }

    async fn start_qr_verification(&self) -> Result<()> {
        let client = self.require_client().await?;
        self.verification
            .start_qr_verification_checked(client, None)
            .await
    }

    async fn confirm_qr_scanned(&self) -> Result<()> {
        self.verification.confirm_qr_scanned().await?;
        Ok(())
    }

    async fn verify_with_recovery_key(&self, key: &str) -> Result<()> {
        let client = self.require_client().await?;
        self.verification
            .verify_with_recovery_key(client, key)
            .await?;
        Ok(())
    }

    async fn cancel_verification(&self) -> Result<()> {
        self.verification.cancel_verification().await
    }

    async fn mismatch_sas(&self) -> Result<()> {
        self.verification.mismatch_sas().await
    }

    async fn skip_verification(&self) -> Result<()> {
        self.verification.skip_verification().await?;
        Ok(())
    }

    async fn get_verification_capabilities(&self) -> Result<VerificationCapabilities> {
        let client = self.require_client().await?;
        self.verification.capabilities(client).await
    }

    async fn get_user_profile_details(
        &self,
        room_id: &str,
        user_id: &str,
    ) -> Result<UserProfileDetails> {
        let client = self.require_client().await?;
        // The room-member path needs a joined/synced room. In preview mode the
        // room is unjoined (often not even a local Room), so fall back to the
        // global profile for name + avatar — the timeline already shows the
        // avatar from the peeked member state, so the popup must too.
        if let Ok(room) = self.get_room(room_id).await {
            if let Ok(details) = RoomMemberService::get_user_profile_details(
                client.clone(),
                room,
                room_id,
                user_id,
                &self.presence_typing,
            )
            .await
            {
                return Ok(details);
            }
        }
        RoomMemberService::get_user_profile_global(client, room_id, user_id, &self.presence_typing)
            .await
    }

    async fn get_room_members_snapshot(
        &self,
        room_id: &str,
        force_refresh: bool,
    ) -> Result<RoomMembersSnapshot> {
        let room = self.get_room(room_id).await?;
        RoomMemberService::get_room_members_snapshot(room, room_id, force_refresh).await
    }

    async fn set_user_power_level(
        &self,
        room_id: &str,
        user_id: &str,
        power_level: i64,
    ) -> Result<()> {
        let room = self.get_room(room_id).await?;
        RoomMemberService::set_user_power_level(room, user_id, power_level).await
    }

    async fn create_direct_room(&self, user_id: &str) -> Result<String> {
        let client = self.require_client().await?;
        RoomMemberService::create_direct_room(client, user_id).await
    }

    async fn ensure_saved_messages_room(&self, create: bool) -> Result<Option<String>> {
        let client = self.require_client().await?;
        let Some((room_id, created)) = self.saved_messages.ensure_room(&client, create).await?
        else {
            // No saved room and this caller may not create one (passive
            // session start). Nothing to adopt.
            return Ok(None);
        };
        if created {
            // Self-messages must never notify; failure is cosmetic, not fatal.
            if let Err(e) = self
                .set_room_notification_mode(&room_id, crate::types::RoomNotificationMode::Mute)
                .await
            {
                tracing::warn!("[saved-messages] mute-on-create failed: {e:?}");
            }
        }
        Ok(Some(room_id))
    }

    async fn delete_saved_messages_room(&self) -> Result<Option<String>> {
        let client = self.require_client().await?;
        // Clear the marker first (permanent — no hidden-but-kept state), then
        // leave + forget the room like any other. take_marker returns the id
        // the marker held so we know what to leave.
        let Some(room_id) = self.saved_messages.take_marker(&client).await? else {
            return Ok(None);
        };
        if let Ok(room) = self.get_room(&room_id).await {
            RoomActionService::leave_room(
                room,
                &room_id,
                self.room_list.rooms.clone(),
                self.local_cache.clone(),
            )
            .await?;
        }
        // Forget after leaving so the room drops off this account entirely
        // (best-effort; leaving already removed it from the list).
        if let Ok(id) = matrix_sdk::ruma::OwnedRoomId::try_from(room_id.as_str()) {
            if let Some(room) = client.get_room(&id) {
                if let Err(e) = room.forget().await {
                    tracing::warn!("[saved-messages] forget failed: {e:?}");
                }
            }
        }
        self.emit_room_summary_changed(&room_id);
        Ok(Some(room_id))
    }

    async fn kick_user(&self, room_id: &str, user_id: &str, reason: Option<&str>) -> Result<()> {
        let room = self.get_room(room_id).await?;
        RoomMemberService::kick_user(room, user_id, reason).await
    }

    async fn ban_user(&self, room_id: &str, user_id: &str, reason: Option<&str>) -> Result<()> {
        let room = self.get_room(room_id).await?;
        RoomMemberService::ban_user(room, user_id, reason).await
    }

    async fn unban_user(&self, room_id: &str, user_id: &str) -> Result<()> {
        let room = self.get_room(room_id).await?;
        RoomMemberService::unban_user(room, user_id).await
    }

    async fn invite_user(&self, room_id: &str, user_id: &str) -> Result<()> {
        let room = self.get_room(room_id).await?;
        RoomMemberService::invite_user(room, user_id).await
    }

    async fn search_user_directory(
        &self,
        query: &str,
        limit: u64,
    ) -> Result<(Vec<UserProfile>, bool)> {
        let client = self.require_client().await?;
        RoomMemberService::search_user_directory(client, query, limit).await
    }

    async fn set_user_ignored(&self, user_id: &str, ignored: bool) -> Result<()> {
        let client = self.require_client().await?;
        RoomMemberService::set_user_ignored(client, user_id, ignored).await
    }

    // --- Account settings ---

    async fn get_account_summary(&self) -> Result<AccountSummary> {
        let client = self.require_client().await?;
        self.account.get_summary(client).await
    }

    async fn set_display_name(&self, name: &str) -> Result<()> {
        let client = self.require_client().await?;
        self.account.set_display_name(client, name).await
    }

    async fn set_avatar_url(&self, mxc_url: Option<&str>) -> Result<()> {
        let client = self.require_client().await?;
        self.account.set_avatar_url(client, mxc_url).await
    }

    async fn upload_avatar(&self, data: Vec<u8>, content_type: &str) -> Result<String> {
        let client = self.require_client().await?;
        self.account.upload_avatar(client, data, content_type).await
    }

    async fn get_3pids(&self) -> Result<Vec<ThreePid>> {
        let client = self.require_client().await?;
        self.account.get_3pids(client).await
    }

    async fn request_3pid_token(
        &self,
        medium: ThreePidMedium,
        address: &str,
        country: &str,
        client_secret: &str,
        send_attempt: u32,
    ) -> Result<ThreePidTokenResponse> {
        let client = self.require_client().await?;
        self.account
            .request_3pid_token(
                client,
                medium,
                address,
                country,
                client_secret,
                send_attempt,
            )
            .await
    }

    async fn add_3pid(
        &self,
        client_secret: &str,
        sid: &str,
        auth_json: Option<&str>,
    ) -> Result<AccountActionResult> {
        let client = self.require_client().await?;
        self.account
            .add_3pid(client, client_secret, sid, auth_json)
            .await
    }

    async fn delete_3pid(&self, medium: ThreePidMedium, address: &str) -> Result<()> {
        let client = self.require_client().await?;
        self.account.delete_3pid(client, medium, address).await
    }

    async fn change_password(
        &self,
        new_password: &str,
        auth_json: Option<&str>,
    ) -> Result<AccountActionResult> {
        let client = self.require_client().await?;
        self.account
            .change_password(client, new_password, auth_json)
            .await
    }

    async fn deactivate_account(
        &self,
        erase_data: bool,
        auth_json: Option<&str>,
    ) -> Result<AccountActionResult> {
        let client = self.require_client().await?;
        self.account
            .deactivate_account(client, erase_data, auth_json)
            .await
    }

    // --- Sessions + Encryption ---

    async fn get_own_devices(&self) -> Result<DeviceSessionList> {
        let client = self.require_client().await?;
        self.encryption.get_own_devices(client).await
    }

    async fn rename_device(&self, device_id: &str, display_name: &str) -> Result<()> {
        let client = self.require_client().await?;
        self.encryption
            .rename_device(client, device_id, display_name)
            .await
    }

    async fn delete_devices(
        &self,
        device_ids: &[String],
        auth_json: &str,
    ) -> Result<DeleteDevicesResult> {
        let client = self.require_client().await?;
        self.encryption
            .delete_devices(client, device_ids, auth_json)
            .await
    }

    async fn get_encryption_overview(&self) -> Result<EncryptionOverview> {
        let client = self.require_client().await?;
        self.encryption.get_encryption_overview(client).await
    }

    async fn set_key_storage_enabled(&self, enabled: bool) -> Result<()> {
        let client = self.require_client().await?;
        self.encryption
            .set_key_storage_enabled(client, enabled)
            .await
    }

    async fn setup_recovery(&self) -> std::result::Result<String, RecoverySetupError> {
        let client = self
            .require_client()
            .await
            .map_err(|e| RecoverySetupError::Other(e.to_string()))?;
        self.encryption.setup_recovery(client).await
    }

    async fn reset_recovery(&self) -> std::result::Result<String, RecoverySetupError> {
        let client = self
            .require_client()
            .await
            .map_err(|e| RecoverySetupError::Other(e.to_string()))?;
        self.encryption.reset_recovery(client).await
    }

    async fn enter_recovery_key(&self, recovery_key: &str) -> Result<()> {
        let client = self.require_client().await?;
        self.encryption
            .enter_recovery_key(client, recovery_key)
            .await
    }

    async fn create_recovery_key(&self) -> Result<String> {
        let _ = self.require_client().await?;
        self.encryption.create_recovery_key().await
    }

    async fn commit_recovery_key(&self, recovery_key: &str) -> Result<()> {
        let client = self.require_client().await?;
        self.encryption
            .commit_recovery_key(client, recovery_key)
            .await
    }

    async fn reset_identity(&self, auth_json: &str) -> Result<ResetIdentityResult> {
        let client = self.require_client().await?;
        self.encryption.reset_identity(client, auth_json).await
    }

    async fn export_e2e_keys(&self, path: &str, passphrase: &str) -> Result<()> {
        let client = self.require_client().await?;
        self.encryption
            .export_e2e_keys(client, path, passphrase)
            .await
    }

    async fn import_e2e_keys(&self, path: &str, passphrase: &str) -> Result<ImportKeysResult> {
        let client = self.require_client().await?;
        self.encryption
            .import_e2e_keys(client, path, passphrase)
            .await
    }

    fn on_room_list_changed(&self, callback: Box<dyn Fn() + Send>) {
        let mut cb = lock_matrix_mutex(&self.room_list.callback, "room_list_callback");
        *cb = Some(callback);
    }

    fn on_timeline_changed(&self, room_id: &str, callback: Box<dyn Fn() + Send>) {
        let mut last =
            lock_matrix_mutex(&self.last_timeline_watch_room, "last_timeline_watch_room");
        *last = Some(room_id.to_string());
        drop(last);

        let mut cbs = lock_matrix_mutex(&self.timeline.callbacks, "timeline_callbacks");
        cbs.insert(room_id.to_string(), callback);
    }

    async fn register(&self, request: RegistrationRequest) -> Result<RegistrationResult> {
        self.auth_generation.fetch_add(1, Ordering::SeqCst);
        self.stop_sliding_backend().await;
        self.session_tasks.abort_current_generation();
        // Those tasks own the retry-chain claims; an aborted chain never releases
        // its own, so a stale claim would silently disable retries for that room.
        self.backup_retry_rooms.clear();
        self.backup_prefetched_rooms.clear();
        let previous_owner = self.current_profile_owner().await;
        self.session_lifecycle
            .prepare_fresh_login(&previous_owner)
            .await?;

        let store_passphrase = self.session_storage.pending_auth_store_passphrase()?;
        let client = self
            .auth
            .build_client(&request.homeserver, &store_passphrase)
            .await?;
        self.do_register(client, &request, &store_passphrase).await
    }

    async fn submit_registration_auth(
        &self,
        request: RegistrationRequest,
    ) -> Result<RegistrationResult> {
        let store_passphrase = self.session_storage.pending_auth_store_passphrase()?;
        let client = self
            .auth
            .build_client(&request.homeserver, &store_passphrase)
            .await?;
        self.do_register(client, &request, &store_passphrase).await
    }

    async fn check_username_available(
        &self,
        homeserver: &str,
        username: &str,
    ) -> Result<UsernameAvailability> {
        AuthService::check_username_available(homeserver, username).await
    }

    fn on_presence_changed(&self, cb: Box<dyn Fn(&str, u32, u64) + Send>) {
        self.on_presence_changed_matrix(cb);
    }

    async fn get_timeline_slice(&self, room_id: &str) -> Result<TimelineSlice> {
        // This is the C++ display path, and it only reads self.timeline.cache.
        // The sliding backend has no warmup, so create the timeline window (and
        // subscribe the room) lazily on first access. The window populates the
        // cache and fires the change callback, prompting the UI to re-poll.
        // create_window_from_client claims a per-room build slot for the whole
        // build, so the re-polls this empty slice provokes are safe.
        match self.timeline.active_timeline_or_legacy(room_id).await {
            None => {
                self.sliding_subscribe(room_id).await;
                if let Ok(client) = self.require_client().await {
                    TimelineWindowService::spawn_create_window_from_client(
                        self.runtime_handle.clone(),
                        client,
                        room_id.to_string(),
                        self.timeline_runtime(),
                        Self::timeline_changed_factory(),
                    );
                }
                // Cold room: the cache is empty (no window yet) and the window is
                // being built in the background. Return an EMPTY slice IMMEDIATELY —
                // building slice metadata (finish_timeline_slice: pinned / unread /
                // read-receipts) is SQLite-heavy, useless for an empty timeline, and
                // contends with the concurrent window build, which otherwise froze the
                // switch. The window's change callback re-polls once it's resident and
                // delivers the real slice. C++ shows the loading pill meanwhile.
                return Ok(TimelineSlice::empty_live());
            }
            Some(timeline) => {
                // Displaying a room whose timeline is resident: fetch the full
                // member list ONCE (this is the open path — get_timeline_slice
                // does not go through get_or_create_timeline). Sliding sync
                // lazy-loads members only for recent senders, so without this,
                // older messages keep a raw MXID name.
                self.trigger_member_fetch(room_id, timeline);
            }
        }

        let items = self.timeline.take_timeline_slice_snapshot(room_id).await;
        let room = self.get_room(room_id).await.ok();
        Ok(self
            .timeline
            .finish_timeline_slice(room_id, items, room)
            .await)
    }

    async fn get_timeline_update(&self, room_id: &str) -> Result<TimelineSlice> {
        let update = self.timeline.take_timeline_update_snapshot(room_id).await;
        let room = self.get_room(room_id).await.ok();
        Ok(self
            .timeline
            .finish_timeline_update(room_id, update, room)
            .await)
    }

    fn paginate_back(&self, room_id: &str, count: u16) {
        self.timeline_navigation.paginate_back(room_id, count);
    }

    fn paginate_forward(&self, room_id: &str, count: u16) {
        self.timeline_navigation.paginate_forward(room_id, count);
    }

    async fn focus_on_event(&self, room_id: &str, event_id: &str) -> Result<()> {
        let generation = self.timeline_navigation.begin_focus_request(room_id).await;

        let room = self.get_room(room_id).await?;
        if self.timeline_navigation.window_missing(room_id).await {
            TimelineWindowService::create_window_for_room(
                &room,
                room_id,
                self.timeline_runtime(),
                Self::timeline_changed_factory(),
            )
            .await?;
        }

        self.timeline_navigation
            .focus_on_event(room_id, event_id, generation, &room)
            .await
    }

    fn return_to_live(&self, room_id: &str) {
        self.timeline_navigation.return_to_live(room_id);
    }

    async fn cancel_upload(&self, room_id: &str, transaction_id: &str) -> Result<()> {
        let timeline = self.get_or_create_timeline(room_id).await?;
        MessageActionService::cancel_upload(timeline, transaction_id).await
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::timeline_conversion_service::TimelineConversionService;
    use crate::types::{AudioInfo, SendState};
    use matrix_sdk::ruma::OwnedTransactionId;
    use matrix_sdk_ui::timeline::EventSendState;
    use matrix_sdk_ui::timeline::TimelineEventItemId;
    use std::sync::Arc;

    fn make_not_sent_yet() -> EventSendState {
        EventSendState::NotSentYet { progress: None }
    }

    fn make_sending_failed() -> EventSendState {
        EventSendState::SendingFailed {
            error: Arc::new(matrix_sdk::Error::AuthenticationRequired),
            is_recoverable: false,
        }
    }

    fn make_sent() -> EventSendState {
        use matrix_sdk::ruma::owned_event_id;
        EventSendState::Sent {
            event_id: owned_event_id!("$test:example.org"),
        }
    }

    #[test]
    fn local_not_sent_yet_maps_to_sending() {
        let state = make_not_sent_yet();
        let result = TimelineConversionService::derive_send_state(
            Some(&state),
            true,
            "@alice:example.org",
            std::iter::empty(),
        );
        assert_eq!(result, (SendState::Sending, -1.0));
    }

    #[test]
    fn local_upload_progress_maps_to_fraction() {
        let state = EventSendState::NotSentYet {
            progress: Some(matrix_sdk_ui::timeline::MediaUploadProgress {
                index: 0,
                progress: matrix_sdk::send_queue::AbstractProgress {
                    current: 25,
                    total: 100,
                },
            }),
        };
        let result = TimelineConversionService::derive_send_state(
            Some(&state),
            true,
            "@alice:example.org",
            std::iter::empty(),
        );
        assert_eq!(result, (SendState::Sending, 0.25));
    }

    #[test]
    fn voice_message_preview_uses_tdesktop_label() {
        let preview = crate::room_summary_service::RoomSummaryService::content_to_preview(
            &MessageContent::Audio {
                info: AudioInfo {
                    url: String::new(),
                    mime_type: String::from("audio/ogg"),
                    filename: String::from("voice-message.ogg"),
                    size: 0,
                    duration_ms: 0,
                    is_voice: true,
                    waveform: Vec::new(),
                },
            },
        );
        assert_eq!(preview.as_deref(), Some("Voice message"));
    }

    #[test]
    fn local_sending_failed_maps_to_failed() {
        let state = make_sending_failed();
        let result = TimelineConversionService::derive_send_state(
            Some(&state),
            true,
            "@alice:example.org",
            std::iter::empty(),
        );
        assert_eq!(result, (SendState::Failed, -1.0));
    }

    #[test]
    fn local_sent_maps_to_sent() {
        let state = make_sent();
        let result = TimelineConversionService::derive_send_state(
            Some(&state),
            true,
            "@alice:example.org",
            std::iter::empty(),
        );
        assert_eq!(result, (SendState::Sent, -1.0));
    }

    #[test]
    fn remote_own_no_receipts_maps_to_sent() {
        let result = TimelineConversionService::derive_send_state(
            None,
            true,
            "@alice:example.org",
            std::iter::empty(),
        );
        assert_eq!(result, (SendState::Sent, -1.0));
    }

    #[test]
    fn resolve_timeline_item_id_accepts_event_ids() {
        let item_id =
            MessageActionService::resolve_timeline_item_id("$test:example.org", "delete").unwrap();
        let TimelineEventItemId::EventId(event_id) = item_id else {
            panic!("expected event id");
        };
        assert_eq!(event_id.as_str(), "$test:example.org");
    }

    #[test]
    fn resolve_timeline_item_id_accepts_transaction_ids() {
        let transaction_id: OwnedTransactionId = "local-media-upload".into();
        let item_id =
            MessageActionService::resolve_timeline_item_id(transaction_id.as_str(), "delete")
                .unwrap();
        let TimelineEventItemId::TransactionId(parsed) = item_id else {
            panic!("expected transaction id");
        };
        assert_eq!(parsed.as_str(), transaction_id.as_str());
    }

    #[test]
    fn remote_own_only_author_receipt_maps_to_sent() {
        let receipts = vec!["@alice:example.org"];
        let result = TimelineConversionService::derive_send_state(
            None,
            true,
            "@alice:example.org",
            receipts.into_iter(),
        );
        assert_eq!(result, (SendState::Sent, -1.0));
    }

    #[test]
    fn remote_own_author_and_other_receipt_maps_to_read() {
        let receipts = vec!["@alice:example.org", "@bob:example.org"];
        let result = TimelineConversionService::derive_send_state(
            None,
            true,
            "@alice:example.org",
            receipts.into_iter(),
        );
        assert_eq!(result, (SendState::Read, -1.0));
    }

    #[test]
    fn remote_own_only_other_receipt_maps_to_read() {
        let receipts = vec!["@bob:example.org"];
        let result = TimelineConversionService::derive_send_state(
            None,
            true,
            "@alice:example.org",
            receipts.into_iter(),
        );
        assert_eq!(result, (SendState::Read, -1.0));
    }

    #[test]
    fn remote_non_own_with_receipts_maps_to_sent() {
        let receipts = vec!["@alice:example.org", "@bob:example.org"];
        let result = TimelineConversionService::derive_send_state(
            None,
            false,
            "@charlie:example.org",
            receipts.into_iter(),
        );
        assert_eq!(result, (SendState::Sent, -1.0));
    }

    fn make_timeline_item(
        event_id: &str,
        content: MessageContent,
        is_outgoing: bool,
    ) -> TimelineItem {
        TimelineItem {
            event_id: event_id.to_string(),
            transaction_id: None,
            sender: UserProfile {
                user_id: "@alice:example.org".to_string(),
                display_name: "Alice".to_string(),
                avatar_url: None,
            },
            timestamp: std::time::SystemTime::UNIX_EPOCH,
            content,
            reply_to_event_id: None,
            reply_preview: None,
            forwarded_from: None,
            is_edited: false,
            is_pinned: false,
            reactions: Vec::new(),
            send_state: SendState::Sent,
            upload_progress: -1.0,
            is_outgoing,
            is_deleted: false,
            url_preview: None,
            is_encrypted: false,
            decryption_error: None,
        }
    }

    #[tokio::test]
    async fn backup_retries_run_only_while_a_room_still_has_undecrypted_items() {
        let cache: TimelineCacheRef = Arc::new(RwLock::new(HashMap::new()));
        let room = "!room:example.org";

        // Unknown room: NOT "finished". The chase must keep waiting — treating
        // this as done is what abandoned rooms whose timeline had not populated
        // yet, leaving them permanently undecryptable after a verification.
        assert_eq!(room_utd_state(&cache, room).await, None);

        cache.write().await.insert(
            room.to_string(),
            vec![
                text_item("$decrypted", false),
                make_timeline_item(
                    "$utd",
                    MessageContent::UnableToDecrypt {
                        body: "Waiting for this message".to_string(),
                        cause: 0,
                        utd_state: 0,
                        session_id: None,
                    },
                    false,
                ),
            ],
        );
        assert_eq!(room_utd_state(&cache, room).await, Some(true));

        // The key arrived and the SDK re-decrypted the event: the chain must stop.
        cache
            .write()
            .await
            .insert(room.to_string(), vec![text_item("$utd", false)]);
        assert_eq!(room_utd_state(&cache, room).await, Some(false));
    }

    #[tokio::test]
    async fn undecrypted_session_ids_collects_megolm_sessions_and_skips_the_rest() {
        let cache: TimelineCacheRef = Arc::new(RwLock::new(HashMap::new()));
        let room = "!room:example.org";

        let utd = |event_id: &str, session_id: Option<&str>| {
            make_timeline_item(
                event_id,
                MessageContent::UnableToDecrypt {
                    body: String::new(),
                    cause: 0,
                    utd_state: 0,
                    session_id: session_id.map(str::to_owned),
                },
                false,
            )
        };

        // Unknown room: nothing to fetch.
        assert!(undecrypted_session_ids(&cache, room).await.is_empty());

        cache.write().await.insert(
            room.to_string(),
            vec![
                text_item("$plain", false),
                utd("$a", Some("sess-A")),
                utd("$b", Some("sess-B")),
                utd("$c", Some("sess-A")), // same session as $a -> deduped
                utd("$d", None),           // non-megolm -> the backup can't help
            ],
        );

        let sessions = undecrypted_session_ids(&cache, room).await;
        assert_eq!(sessions.len(), 2);
        assert!(sessions.contains("sess-A"));
        assert!(sessions.contains("sess-B"));

        // The dedup the retry relies on: once every stuck session has been
        // requested, a further round has nothing new (is_subset) and must skip.
        let requested = sessions.clone();
        assert!(undecrypted_session_ids(&cache, room)
            .await
            .is_subset(&requested));
    }

    #[test]
    fn a_room_gets_one_backup_retry_chain_at_a_time() {
        let rooms = BackupRetryRooms::default();
        let room: OwnedRoomId = "!room:example.org".try_into().unwrap();
        let other: OwnedRoomId = "!other:example.org".try_into().unwrap();

        assert!(rooms.claim(room.clone()));
        // Reopening the room while its chain runs must not stack a second one.
        assert!(!rooms.claim(room.clone()));
        assert!(rooms.claim(other.clone()));

        // Once the chain finishes, a later open starts a fresh one.
        rooms.release(&room);
        assert!(rooms.claim(room.clone()));

        // Logout aborts the chains, so their claims are dropped wholesale.
        rooms.clear();
        assert!(rooms.claim(room));
        assert!(rooms.claim(other));
    }

    fn text_item(event_id: &str, is_outgoing: bool) -> TimelineItem {
        make_timeline_item(
            event_id,
            MessageContent::Text {
                body: event_id.to_string(),
                formatted_body: None,
            },
            is_outgoing,
        )
    }

    fn unread_sample_items() -> Vec<TimelineItem> {
        vec![
            make_timeline_item(
                "$service:example.org",
                MessageContent::Service {
                    body: "joined".to_string(),
                },
                false,
            ),
            text_item("$outgoing:example.org", true),
            text_item("$incoming-1:example.org", false),
            text_item("$incoming-2:example.org", false),
        ]
    }

    #[test]
    fn timeline_first_unread_without_marker_is_first_incoming() {
        // No read marker: the first countable item (service + outgoing skipped).
        let items = unread_sample_items();
        assert_eq!(
            TimelineService::first_unread_event_id(&items, None, 2).as_deref(),
            Some("$incoming-1:example.org")
        );
    }

    #[test]
    fn timeline_first_unread_after_read_marker() {
        // Marker on incoming-1 → first unread is the next countable item.
        let items = unread_sample_items();
        assert_eq!(
            TimelineService::first_unread_event_id(&items, Some("$incoming-1:example.org"), 1)
                .as_deref(),
            Some("$incoming-2:example.org")
        );
    }

    #[test]
    fn timeline_first_unread_is_stable_as_count_changes() {
        // The whole point of the fix: with a fixed read marker the anchor does
        // NOT move when only the unread count changes (the old Nth-from-bottom
        // logic returned a different event for each count).
        let items = unread_sample_items();
        let marker = Some("$incoming-1:example.org");
        let with_high = TimelineService::first_unread_event_id(&items, marker, 5);
        let with_low = TimelineService::first_unread_event_id(&items, marker, 1);
        assert_eq!(with_high, with_low);
        assert_eq!(with_high.as_deref(), Some("$incoming-2:example.org"));
    }

    #[test]
    fn timeline_first_unread_is_stable_on_append() {
        // Appending a newer message must not move the anchor while the read
        // marker is unchanged.
        let mut items = unread_sample_items();
        let marker = Some("$incoming-1:example.org");
        let before = TimelineService::first_unread_event_id(&items, marker, 1);
        items.push(text_item("$incoming-3:example.org", false));
        let after = TimelineService::first_unread_event_id(&items, marker, 2);
        assert_eq!(before, after);
        assert_eq!(after.as_deref(), Some("$incoming-2:example.org"));
    }

    #[test]
    fn timeline_first_unread_falls_back_when_marker_not_loaded() {
        // Marker older than the loaded window (all-unread live tail): the first
        // countable item is the first unread.
        let items = unread_sample_items();
        assert_eq!(
            TimelineService::first_unread_event_id(&items, Some("$not-loaded:example.org"), 4)
                .as_deref(),
            Some("$incoming-1:example.org")
        );
    }

    #[test]
    fn timeline_first_unread_none_when_count_zero() {
        let items = unread_sample_items();
        assert!(
            TimelineService::first_unread_event_id(&items, Some("$incoming-1:example.org"), 0)
                .is_none()
        );
    }

    #[test]
    fn timeline_first_unread_none_when_marker_is_last_countable() {
        // Read up to the last message: nothing countable after the marker.
        let items = unread_sample_items();
        assert!(
            TimelineService::first_unread_event_id(&items, Some("$incoming-2:example.org"), 1)
                .is_none()
        );
    }

    #[test]
    fn extract_mentions_detects_room_mention_token() {
        let mentions = MessageActionService::extract_mentions("Heads up @room", None).unwrap();
        assert!(mentions.room);
        assert!(mentions.user_ids.is_empty());
    }

    #[test]
    fn extract_mentions_ignores_embedded_room_substrings() {
        assert!(MessageActionService::extract_mentions("foo@roommate", None).is_none());
        assert!(MessageActionService::extract_mentions("@roommate", None).is_none());
    }

    #[test]
    fn extract_mentions_collects_user_mentions_from_html() {
        let mentions = MessageActionService::extract_mentions(
            "Alice and Bob",
            Some(
                r#"<a href="https://matrix.to/#/@alice:example.org">Alice</a> and <a href="https://matrix.to/#/@bob:example.org?via=example.org">Bob</a>"#,
            ),
        )
        .unwrap();

        let user_ids: Vec<_> = mentions.user_ids.iter().map(|id| id.as_str()).collect();
        assert_eq!(user_ids, vec!["@alice:example.org", "@bob:example.org"]);
        assert!(!mentions.room);
    }

    #[test]
    fn extract_mentions_combines_room_and_user_mentions() {
        let mentions = MessageActionService::extract_mentions(
            "Hi @room",
            Some(r#"<a href="https://matrix.to/#/@alice:example.org">Alice</a>"#),
        )
        .unwrap();

        assert!(mentions.room);
        assert!(mentions
            .user_ids
            .iter()
            .any(|id| id.as_str() == "@alice:example.org"));
    }
}

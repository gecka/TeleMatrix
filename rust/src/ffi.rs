// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#![allow(clippy::empty_line_after_doc_comments, clippy::doc_lazy_continuation)]
// Per-function `# Safety` sections are omitted (there are ~100 `extern "C"`
// entry points); the shared safety contract is documented in the module doc
// below instead. See `src/protocol/protocol_bridge.cpp` on the C++ side.
#![allow(clippy::missing_safety_doc)]

//! # FFI boundary — safety & ownership contract
//!
//! All `pub unsafe extern "C" fn tm_*` functions in this module share these
//! rules. The C++ side (`src/protocol/`) must uphold them:
//!
//! - **Handle.** `tm_create` returns a `*mut Handle`. Every other entry point
//!   takes that pointer and assumes it is non-null and valid until `tm_destroy`
//!   is called. After `tm_destroy` the pointer is dangling and must not be used.
//! - **String ownership.** Returned `*mut c_char` / heap structs are owned by
//!   C++ and must be released with the matching `tm_free_*` function (never
//!   `free()`/`delete`). Strings *passed in* (`*const c_char`) are borrowed for
//!   the duration of the call only and are copied if retained.
//! - **Callback pointers.** `*const`/byte-buffer pointers handed to a callback
//!   are valid only for the duration of that callback invocation; the C++ side
//!   must copy anything it needs to keep (see the `MediaDownloadCompletion`
//!   at-most-once contract for media bytes).
//! - **Callback threading & lifetime.** Callbacks fire on tokio worker threads.
//!   C++ must marshal to the Qt main thread before touching widgets, and must
//!   guarantee any `userdata` it passes outlives every possible invocation —
//!   persistent callbacks are guarded by `BridgeCallbackGuard` and cleared via
//!   `tm_clear_callbacks` before `tm_destroy`.
//! - **Panics.** Release builds (the shipped artifact) use `panic = "abort"`
//!   (see `Cargo.toml` `[profile.release]`), so a panic aborts the process
//!   rather than unwinding across the C boundary. Debug builds still unwind;
//!   only link the release staticlib into production.

use std::collections::{HashMap, HashSet};
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int};
use std::path::PathBuf;
use std::ptr;
use std::slice;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};

use tokio::runtime::Runtime;
use tokio::task::JoinHandle;
use tracing::{info, warn};
use zeroize::Zeroizing;

use crate::auth_service::AuthService;
use crate::encryption_service::RecoverySetupError;
use crate::matrix::MatrixProtocol;
use crate::protocol::ProtocolClient;
use crate::types::{
    AudioInfo, ContentType, CreateRoomRequest, EncryptionHealthState, FolderMeta, MessageContent,
    PollKind, ReactionInfo, RoomDirectoryPage, RoomDirectoryRequest, RoomSummary, SearchRequest,
    SearchScope, SpaceHierarchyRequest, ThreePidMedium, TimelineSlice, TimelineUpdateKind,
};

/// Opaque handle holding the Tokio runtime and protocol client.
pub struct Handle {
    runtime: Runtime,
    client: Arc<dyn ProtocolClient>,
    /// Reference to MatrixProtocol for non-trait methods (session, media, etc.).
    matrix: Arc<MatrixProtocol>,
    /// Active search request IDs for cancellation support.
    active_searches: Arc<Mutex<HashSet<u64>>>,
    /// Same, for room-directory / space-hierarchy requests. Debounced typing cancels these
    /// constantly, so a cancelled request must still fire its callback for C++ to free userdata.
    active_directory_requests: Arc<Mutex<HashSet<u64>>>,
    /// Active media tasks that can write plaintext temp files.
    active_media_downloads: Arc<Mutex<HashMap<String, ActiveMediaDownload>>>,
}

struct ActiveMediaDownload {
    task: JoinHandle<()>,
    completion: ActiveMediaCompletion,
}

static NEXT_ABORTED_MEDIA_TASK_ID: AtomicU64 = AtomicU64::new(1);

/// Per-Handle ordinal, used to name that runtime's threads (`a0-w7`).
static ACCOUNT_ORDINAL: AtomicU32 = AtomicU32::new(0);

fn lock_ffi_mutex<'a, T>(mutex: &'a Mutex<T>, name: &str) -> MutexGuard<'a, T> {
    match mutex.lock() {
        Ok(guard) => guard,
        Err(poisoned) => {
            warn!("[FFI] recovering poisoned mutex: {name}");
            poisoned.into_inner()
        }
    }
}

#[inline]
fn leak_boxed_slice<T>(items: Vec<T>) -> *mut T {
    let mut boxed = items.into_boxed_slice();
    let ptr = boxed.as_mut_ptr();
    std::mem::forget(boxed);
    ptr
}

#[inline]
unsafe fn reclaim_boxed_slice<T>(ptr: *mut T, len: usize) -> Box<[T]> {
    unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(ptr, len)) }
}

#[derive(Clone)]
enum ActiveMediaCompletion {
    Path(Arc<MediaDownloadCompletion>),
    Bytes(Arc<MediaBytesCompletion>),
}

impl ActiveMediaCompletion {
    fn call_cancelled(&self) {
        match self {
            ActiveMediaCompletion::Path(completion) => {
                completion.call_once(false, ptr::null(), false);
            }
            ActiveMediaCompletion::Bytes(completion) => {
                completion.call_once(false, ptr::null(), 0, ptr::null(), ptr::null(), false);
            }
        }
    }

    fn matches_path(&self, completion: &Arc<MediaDownloadCompletion>) -> bool {
        match self {
            ActiveMediaCompletion::Path(current) => Arc::ptr_eq(current, completion),
            ActiveMediaCompletion::Bytes(_) => false,
        }
    }

    fn matches_bytes(&self, completion: &Arc<MediaBytesCompletion>) -> bool {
        match self {
            ActiveMediaCompletion::Path(_) => false,
            ActiveMediaCompletion::Bytes(current) => Arc::ptr_eq(current, completion),
        }
    }
}

struct MediaDownloadCompletion {
    callback: TmMediaCallback,
    userdata: Userdata,
    callback_lock: Mutex<()>,
    completed: AtomicBool,
}

impl MediaDownloadCompletion {
    fn new(callback: TmMediaCallback, userdata: Userdata) -> Self {
        Self {
            callback,
            userdata,
            callback_lock: Mutex::new(()),
            completed: AtomicBool::new(false),
        }
    }

    fn call_once(&self, success: bool, local_path: *const c_char, terminal: bool) {
        let _guard = lock_ffi_mutex(&self.callback_lock, "media_download_callback");
        if !self.completed.swap(true, Ordering::AcqRel) {
            (self.callback)(success, local_path, terminal, self.userdata.as_ptr());
        }
    }

    fn progress(
        &self,
        callback: TmMediaProgressCallback,
        received_bytes: u64,
        total_bytes: u64,
        phase: u32,
    ) {
        let _guard = lock_ffi_mutex(&self.callback_lock, "media_download_callback");
        if !self.completed.load(Ordering::Acquire) {
            callback(received_bytes, total_bytes, phase, self.userdata.as_ptr());
        }
    }
}

struct MediaBytesCompletion {
    callback: TmMediaBytesCallback,
    userdata: Userdata,
    callback_lock: Mutex<()>,
    completed: AtomicBool,
}

impl MediaBytesCompletion {
    fn new(callback: TmMediaBytesCallback, userdata: Userdata) -> Self {
        Self {
            callback,
            userdata,
            callback_lock: Mutex::new(()),
            completed: AtomicBool::new(false),
        }
    }

    fn call_once(
        &self,
        success: bool,
        bytes: *const u8,
        bytes_len: usize,
        mime: *const c_char,
        filename: *const c_char,
        terminal: bool,
    ) {
        let _guard = lock_ffi_mutex(&self.callback_lock, "media_bytes_callback");
        if !self.completed.swap(true, Ordering::AcqRel) {
            (self.callback)(
                success,
                bytes,
                bytes_len,
                mime,
                filename,
                terminal,
                self.userdata.as_ptr(),
            );
        }
    }

    fn progress(
        &self,
        callback: TmMediaProgressCallback,
        received_bytes: u64,
        total_bytes: u64,
        phase: u32,
    ) {
        let _guard = lock_ffi_mutex(&self.callback_lock, "media_bytes_callback");
        if !self.completed.load(Ordering::Acquire) {
            callback(received_bytes, total_bytes, phase, self.userdata.as_ptr());
        }
    }
}

async fn abort_and_await_active_media_tasks(
    active_media_downloads: Arc<Mutex<HashMap<String, ActiveMediaDownload>>>,
) {
    let active = {
        let mut active = lock_ffi_mutex(&active_media_downloads, "active_media_downloads");
        active.drain().map(|(_, active)| active).collect::<Vec<_>>()
    };
    for active in &active {
        active.task.abort();
        active.completion.call_cancelled();
    }
    for active in active {
        let _ = active.task.await;
    }
}

fn track_aborted_media_task_locked(
    active_media_downloads: &mut HashMap<String, ActiveMediaDownload>,
    label: &str,
    active: ActiveMediaDownload,
) -> ActiveMediaCompletion {
    active.task.abort();
    let completion = active.completion.clone();
    if active.task.is_finished() {
        return completion;
    }
    let id = NEXT_ABORTED_MEDIA_TASK_ID.fetch_add(1, Ordering::Relaxed);
    let key = format!("aborted:{label}:{id}");
    active_media_downloads.insert(key, active);
    completion
}

// --- FFI-safe types ---

/// C-compatible room summary.
#[repr(C)]
pub struct FfiRoomSummary {
    pub room_id: *mut c_char,
    pub display_name: *mut c_char,
    pub canonical_alias: *mut c_char,
    pub avatar_url: *mut c_char,
    pub avatar_entity_id: *mut c_char,
    pub last_event_text: *mut c_char,
    pub last_event_sender: *mut c_char,
    pub last_event_timestamp: u64,
    pub unread_count: u32,
    pub highlight_count: u32,
    pub notification_mode: u32,
    pub is_muted: bool,
    pub is_pinned: bool,
    /// `order` from the `m.favourite` tag — where the user placed this room among
    /// their pinned ones. Negative when the server holds no order for it.
    pub pinned_order: f64,
    pub is_marked_unread: bool,
    pub is_direct: bool,
    pub is_public: bool,
    pub filter_ids: *mut i32,
    pub filter_ids_len: usize,
    /// Room ids of the joined spaces this room belongs to (string array).
    pub space_ids: *mut *mut c_char,
    pub space_ids_len: usize,
    pub is_last_event_outgoing: bool,
    pub is_last_event_service: bool,
    pub last_event_send_state: u32,
    pub member_count: u64,
    pub can_pin_messages: bool,
    pub peer_presence: u32,
    pub membership: u32,
    pub inviter_user_id: *mut c_char,
    pub inviter_display_name: *mut c_char,
    pub inviter_avatar_url: *mut c_char,
    pub room_topic: *mut c_char,
}

/// C-compatible room list.
#[repr(C)]
pub struct FfiRoomList {
    pub rooms: *mut FfiRoomSummary,
    pub len: usize,
}

pub type TmRoomListCallback =
    extern "C" fn(success: bool, list: FfiRoomList, userdata: *mut libc::c_void);

/// C-compatible user profile.
#[repr(C)]
pub struct FfiUserProfile {
    pub user_id: *mut c_char,
    pub display_name: *mut c_char,
    pub avatar_url: *mut c_char,
}

/// C-compatible timeline item.
#[repr(C)]
pub struct FfiTimelineItem {
    pub event_id: *mut c_char,
    pub transaction_id: *mut c_char,
    pub sender_user_id: *mut c_char,
    pub sender_display_name: *mut c_char,
    pub sender_avatar_url: *mut c_char,
    pub timestamp: u64,
    pub content_type: u32,
    pub body: *mut c_char,
    pub formatted_body: *mut c_char,
    pub media_url: *mut c_char,
    pub media_mime: *mut c_char,
    pub media_filename: *mut c_char,
    pub media_caption: *mut c_char,
    pub media_thumb_url: *mut c_char,
    pub media_blurhash: *mut c_char,
    pub media_size: u64,
    pub media_width: u32,
    pub media_height: u32,
    pub media_duration_ms: u64,
    pub reply_to_event_id: *mut c_char,
    pub reply_preview_sender_name: *mut c_char,
    pub reply_preview_text: *mut c_char,
    pub reply_preview_thumb_url: *mut c_char,
    pub reply_preview_has_thumb: bool,
    pub reply_preview_is_text_colorized: bool,
    pub reply_preview_is_deleted: bool,
    pub reply_preview_is_unavailable: bool,
    pub forwarded_from_sender_name: *mut c_char,
    /// Null/empty on forwards made before author identity was recorded.
    pub forwarded_from_sender_id: *mut c_char,
    pub forwarded_from_avatar_url: *mut c_char,
    pub is_edited: bool,
    pub is_pinned: bool,
    pub reactions: *mut c_char,
    pub send_state: u32,
    pub upload_progress: f64,
    pub is_outgoing: bool,
    pub is_deleted: bool,
    pub url_preview_url: *mut c_char,
    pub url_preview_site_name: *mut c_char,
    pub url_preview_title: *mut c_char,
    pub url_preview_description: *mut c_char,
    pub url_preview_image_url: *mut c_char,
    pub url_preview_image_width: u32,
    pub url_preview_image_height: u32,
    pub url_preview_type: u32,
    pub url_preview_duration: u32,
    pub url_preview_author: *mut c_char,
    pub url_preview_has_large_media: bool,
    pub url_preview_site_name_canonical: *mut c_char,
    // Encryption indicators.
    pub is_encrypted: bool,
    pub decryption_error: *mut c_char, // NULL if none
    pub utd_cause: u32,
    pub utd_state: u32, // 0 = decrypting/glow, 1 = terminal
    // Audio-specific fields.
    pub is_voice_message: bool,
    pub audio_waveform_json: *mut c_char,
    // Poll fields (content_type == 5)
    pub poll_question: *mut c_char,
    pub poll_subtitle: *mut c_char,
    pub poll_options_json: *mut c_char,
    pub poll_total_voters: u32,
    pub poll_max_selections: u32,
    pub poll_is_closed: bool,
    pub poll_is_multi_choice: bool,
    pub poll_is_quiz: bool,
    pub poll_has_voted: bool,
    pub poll_kind: u32,
}

/// C-compatible timeline.
#[repr(C)]
pub struct FfiTimeline {
    pub items: *mut FfiTimelineItem,
    pub len: usize,
}

/// C-compatible timeline slice with pagination metadata.
#[repr(C)]
pub struct FfiTimelineSlice {
    pub items: *mut FfiTimelineItem,
    pub items_count: u32,
    pub update_kind: u32,
    pub update_index: u32,
    pub can_paginate_back: bool,
    pub can_paginate_forward: bool,
    pub hit_timeline_start: bool,
    pub is_live: bool,
    pub focus_event_id: *mut c_char, // NULL when live; owned (freed by tm_free_timeline_slice)
    pub pinned_event_ids: *mut *mut c_char,
    pub pinned_event_ids_count: u32,
    pub first_unread_event_id: *mut c_char,
    pub read_marker_loaded: bool,
    pub unread_count: u32,
    pub unread_state_known: bool,
}

pub type TmTimelineSliceCallback =
    extern "C" fn(success: bool, slice: FfiTimelineSlice, userdata: *mut libc::c_void);

/// C-compatible search hit.
#[repr(C)]
pub struct FfiSearchHit {
    pub room_id: *mut c_char,
    pub event_id: *mut c_char,
    pub sender_id: *mut c_char,
    pub sender_name: *mut c_char,
    pub timestamp: u64,
    pub snippet: *mut c_char,
    pub rank: i32,
    pub local_only: bool,
}

/// C-compatible search results page.
#[repr(C)]
pub struct FfiSearchPage {
    pub request_id: u64,
    pub hits: *mut FfiSearchHit,
    pub hits_len: usize,
    pub total_approx: i32,
    pub next_token: *mut c_char,
    pub done: bool,
    /// True when the page is empty because the user disabled E2EE-room search.
    pub e2ee_disabled: bool,
    /// True for a room-scoped E2EE search whose local index is still being built
    /// (history backfill in progress) — the UI shows an "indexing" message.
    pub indexing: bool,
}

/// C-compatible user profile details.
#[repr(C)]
pub struct FfiUserProfileDetails {
    pub room_id: *mut c_char,
    pub user_id: *mut c_char,
    pub display_name: *mut c_char,
    pub avatar_url: *mut c_char,
    pub presence: u32,
    pub last_active_ts: u64,
    pub membership: u32,
    pub power_level: i64,
    pub role: u32,
    pub is_ignored: bool,
    pub dm_room_id: *mut c_char,
    pub can_invite: bool,
    pub can_kick: bool,
    pub can_ban: bool,
    pub can_mute: bool,
    pub can_change_power_level: bool,
    pub max_assignable_power_level: i64,
}

/// Callback type for async search results.
pub type TmSearchCallback = extern "C" fn(
    success: bool,
    page: FfiSearchPage,
    error: *const c_char,
    userdata: *mut libc::c_void,
);

// --- Helper functions ---

fn str_to_c(s: &str) -> *mut c_char {
    CString::new(s).unwrap_or_default().into_raw()
}

fn opt_str_to_c(s: &Option<String>) -> *mut c_char {
    match s {
        Some(s) => str_to_c(s),
        None => ptr::null_mut(),
    }
}

fn c_to_string(ptr: *const c_char) -> String {
    if ptr.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(ptr) }.to_string_lossy().to_string()
    }
}

fn opt_c_to_string(ptr: *const c_char) -> Option<String> {
    if ptr.is_null() {
        None
    } else {
        Some(unsafe { CStr::from_ptr(ptr) }.to_string_lossy().to_string())
    }
}

fn timestamp_to_epoch(ts: std::time::SystemTime) -> u64 {
    ts.duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}

fn content_type_from_ffi(value: u32) -> Option<ContentType> {
    match value {
        1 => Some(ContentType::Image),
        2 => Some(ContentType::File),
        3 => Some(ContentType::Video),
        7 => Some(ContentType::Audio),
        _ => None,
    }
}

fn reactions_to_json(reactions: &[ReactionInfo]) -> *mut c_char {
    if reactions.is_empty() {
        return ptr::null_mut();
    }
    match serde_json::to_string(reactions) {
        Ok(json) => str_to_c(&json),
        Err(_) => ptr::null_mut(),
    }
}

fn compute_poll_subtitle(info: &crate::types::PollInfo) -> String {
    if info.is_closed {
        return String::from("Final Results");
    }
    match (info.is_quiz, info.kind) {
        (true, PollKind::Undisclosed) => String::from("Quiz, results after end"),
        (true, PollKind::Disclosed) => String::from("Quiz"),
        (false, PollKind::Undisclosed) => String::from("Poll, results after end"),
        (false, PollKind::Disclosed) => String::from("Poll"),
    }
}

#[cfg(test)]
mod tests {
    use super::compute_poll_subtitle;
    use crate::types::{PollInfo, PollKind};

    fn sample_poll(kind: PollKind) -> PollInfo {
        PollInfo {
            question: String::from("Question"),
            kind,
            max_selections: 1,
            is_closed: false,
            is_quiz: false,
            total_voters: 0,
            options: Vec::new(),
            has_voted: false,
        }
    }

    #[test]
    fn disclosed_poll_subtitle_stays_plain() {
        assert_eq!(
            compute_poll_subtitle(&sample_poll(PollKind::Disclosed)),
            "Poll"
        );
    }

    #[test]
    fn undisclosed_poll_subtitle_explains_reveal_timing() {
        assert_eq!(
            compute_poll_subtitle(&sample_poll(PollKind::Undisclosed)),
            "Poll, results after end"
        );
    }
}

/// # Safety
/// `ptr` must be a valid C string pointer returned by this library, or null.
unsafe fn free_c_string(ptr: *mut c_char) {
    if !ptr.is_null() {
        drop(unsafe { CString::from_raw(ptr) });
    }
}

/// Convert a single `TimelineItem` to an `FfiTimelineItem`.
/// All returned string pointers are heap-allocated C strings that must be freed
/// (either individually or via `tm_free_timeline` / `tm_free_timeline_slice`).
fn timeline_item_to_ffi(item: &crate::types::TimelineItem) -> FfiTimelineItem {
    let (
        content_type,
        body,
        formatted_body,
        media_url,
        media_mime,
        media_filename,
        media_caption,
        media_thumb_url,
        media_blurhash,
        media_size,
        media_width,
        media_height,
        media_duration_ms,
        is_voice_message,
        audio_waveform_json,
    ) = match &item.content {
        MessageContent::Text {
            body: text_body,
            formatted_body: text_formatted,
        } => (
            ContentType::Text as u32,
            text_body.clone(),
            text_formatted.clone(),
            None,
            None,
            None,
            None,
            None,
            None,
            0_u64,
            0_u32,
            0_u32,
            0_u64,
            false,
            None::<String>,
        ),
        MessageContent::Image {
            url,
            mime_type,
            filename,
            caption,
            thumbnail_url,
            blurhash,
            size,
            width,
            height,
        } => (
            ContentType::Image as u32,
            caption.clone().unwrap_or_default(),
            None,
            Some(url.clone()),
            Some(mime_type.clone()),
            Some(filename.clone()),
            caption.clone(),
            thumbnail_url.clone(),
            blurhash.clone(),
            *size,
            *width,
            *height,
            0_u64,
            false,
            None::<String>,
        ),
        MessageContent::File {
            url,
            mime_type,
            filename,
            caption,
            size,
            duration_ms,
        } => (
            ContentType::File as u32,
            caption.clone().unwrap_or_else(|| filename.clone()),
            None,
            Some(url.clone()),
            Some(mime_type.clone()),
            Some(filename.clone()),
            caption.clone(),
            None,
            None,
            *size,
            0_u32,
            0_u32,
            *duration_ms,
            false,
            None::<String>,
        ),
        MessageContent::Video {
            url,
            mime_type,
            filename,
            caption,
            thumbnail_url,
            blurhash,
            size,
            width,
            height,
            duration_ms,
        } => (
            ContentType::Video as u32,
            caption.clone().unwrap_or_default(),
            None,
            Some(url.clone()),
            Some(mime_type.clone()),
            Some(filename.clone()),
            caption.clone(),
            thumbnail_url.clone(),
            blurhash.clone(),
            *size,
            *width,
            *height,
            *duration_ms,
            false,
            None::<String>,
        ),
        MessageContent::Audio { info } => (
            ContentType::Audio as u32,
            info.filename.clone(),
            None,
            Some(info.url.clone()),
            Some(info.mime_type.clone()),
            Some(info.filename.clone()),
            None,
            None,
            None,
            info.size,
            0_u32,
            0_u32,
            info.duration_ms,
            info.is_voice,
            if info.waveform.is_empty() {
                None
            } else {
                Some(serde_json::to_string(&info.waveform).unwrap_or_default())
            },
        ),
        MessageContent::Service { body: service_body } => (
            ContentType::Service as u32,
            service_body.clone(),
            None,
            None,
            None,
            None,
            None,
            None,
            None,
            0_u64,
            0_u32,
            0_u32,
            0_u64,
            false,
            None::<String>,
        ),
        MessageContent::Poll { info } => (
            ContentType::Poll as u32,
            info.question.clone(),
            None,
            None,
            None,
            None,
            None,
            None,
            None,
            0_u64,
            0_u32,
            0_u32,
            0_u64,
            false,
            None::<String>,
        ),
        MessageContent::UnableToDecrypt { body: utd_body, .. } => (
            ContentType::UnableToDecrypt as u32,
            utd_body.clone(),
            None,
            None,
            None,
            None,
            None,
            None,
            None,
            0_u64,
            0_u32,
            0_u32,
            0_u64,
            false,
            None::<String>,
        ),
    };

    let reactions = reactions_to_json(&item.reactions);

    FfiTimelineItem {
        event_id: str_to_c(&item.event_id),
        transaction_id: opt_str_to_c(&item.transaction_id),
        sender_user_id: str_to_c(&item.sender.user_id),
        sender_display_name: str_to_c(&item.sender.display_name),
        sender_avatar_url: opt_str_to_c(&item.sender.avatar_url),
        timestamp: timestamp_to_epoch(item.timestamp),
        content_type,
        body: str_to_c(&body),
        formatted_body: opt_str_to_c(&formatted_body),
        media_url: opt_str_to_c(&media_url),
        media_mime: opt_str_to_c(&media_mime),
        media_filename: opt_str_to_c(&media_filename),
        media_caption: opt_str_to_c(&media_caption),
        media_thumb_url: opt_str_to_c(&media_thumb_url),
        media_blurhash: opt_str_to_c(&media_blurhash),
        media_size,
        media_width,
        media_height,
        media_duration_ms,
        reply_to_event_id: opt_str_to_c(&item.reply_to_event_id),
        reply_preview_sender_name: opt_str_to_c(
            &item
                .reply_preview
                .as_ref()
                .map(|preview| preview.sender_display_name.clone()),
        ),
        reply_preview_text: opt_str_to_c(
            &item
                .reply_preview
                .as_ref()
                .map(|preview| preview.text.clone()),
        ),
        reply_preview_thumb_url: opt_str_to_c(
            &item
                .reply_preview
                .as_ref()
                .and_then(|preview| preview.thumb_url.clone()),
        ),
        reply_preview_has_thumb: item
            .reply_preview
            .as_ref()
            .map(|preview| preview.has_thumb)
            .unwrap_or(false),
        reply_preview_is_text_colorized: item
            .reply_preview
            .as_ref()
            .map(|preview| preview.is_text_colorized)
            .unwrap_or(false),
        reply_preview_is_deleted: item
            .reply_preview
            .as_ref()
            .map(|preview| preview.is_deleted)
            .unwrap_or(false),
        reply_preview_is_unavailable: item
            .reply_preview
            .as_ref()
            .map(|preview| preview.is_unavailable)
            .unwrap_or(false),
        forwarded_from_sender_name: opt_str_to_c(
            &item
                .forwarded_from
                .as_ref()
                .map(|f| f.sender_display_name.clone()),
        ),
        forwarded_from_sender_id: opt_str_to_c(
            &item.forwarded_from.as_ref().map(|f| f.sender_id.clone()),
        ),
        forwarded_from_avatar_url: opt_str_to_c(
            &item.forwarded_from.as_ref().map(|f| f.avatar_url.clone()),
        ),
        is_edited: item.is_edited,
        is_pinned: item.is_pinned,
        reactions,
        send_state: item.send_state as u32,
        upload_progress: item.upload_progress,
        is_outgoing: item.is_outgoing,
        is_deleted: item.is_deleted,
        url_preview_url: opt_str_to_c(&item.url_preview.as_ref().map(|p| p.url.clone())),
        url_preview_site_name: opt_str_to_c(
            &item.url_preview.as_ref().and_then(|p| p.site_name.clone()),
        ),
        url_preview_title: opt_str_to_c(&item.url_preview.as_ref().and_then(|p| p.title.clone())),
        url_preview_description: opt_str_to_c(
            &item
                .url_preview
                .as_ref()
                .and_then(|p| p.description.clone()),
        ),
        url_preview_image_url: opt_str_to_c(
            &item.url_preview.as_ref().and_then(|p| p.image_url.clone()),
        ),
        url_preview_image_width: item.url_preview.as_ref().map_or(0, |p| p.image_width),
        url_preview_image_height: item.url_preview.as_ref().map_or(0, |p| p.image_height),
        url_preview_type: item
            .url_preview
            .as_ref()
            .map_or(0, |p| p.preview_type as u32),
        url_preview_duration: item.url_preview.as_ref().map_or(0, |p| p.duration_secs),
        url_preview_author: opt_str_to_c(&item.url_preview.as_ref().and_then(|p| p.author.clone())),
        url_preview_has_large_media: item.url_preview.as_ref().is_some_and(|p| p.has_large_media),
        url_preview_site_name_canonical: opt_str_to_c(
            &item
                .url_preview
                .as_ref()
                .and_then(|p| p.site_name_canonical.clone()),
        ),
        is_encrypted: item.is_encrypted,
        decryption_error: opt_str_to_c(&item.decryption_error),
        utd_cause: match &item.content {
            MessageContent::UnableToDecrypt { cause, .. } => *cause as u32,
            _ => 0,
        },
        utd_state: match &item.content {
            MessageContent::UnableToDecrypt { utd_state, .. } => *utd_state as u32,
            _ => 0,
        },
        is_voice_message,
        audio_waveform_json: opt_str_to_c(&audio_waveform_json),
        poll_question: match &item.content {
            MessageContent::Poll { info } => str_to_c(&info.question),
            _ => ptr::null_mut(),
        },
        poll_subtitle: match &item.content {
            MessageContent::Poll { info } => str_to_c(&compute_poll_subtitle(info)),
            _ => ptr::null_mut(),
        },
        poll_options_json: match &item.content {
            MessageContent::Poll { info } => match serde_json::to_string(&info.options) {
                Ok(json) => str_to_c(&json),
                Err(_) => ptr::null_mut(),
            },
            _ => ptr::null_mut(),
        },
        poll_total_voters: match &item.content {
            MessageContent::Poll { info } => info.total_voters,
            _ => 0,
        },
        poll_max_selections: match &item.content {
            MessageContent::Poll { info } => info.max_selections,
            _ => 1,
        },
        poll_is_closed: match &item.content {
            MessageContent::Poll { info } => info.is_closed,
            _ => false,
        },
        poll_is_multi_choice: match &item.content {
            MessageContent::Poll { info } => info.max_selections > 1,
            _ => false,
        },
        poll_is_quiz: match &item.content {
            MessageContent::Poll { info } => info.is_quiz,
            _ => false,
        },
        poll_has_voted: match &item.content {
            MessageContent::Poll { info } => info.has_voted,
            _ => false,
        },
        poll_kind: match &item.content {
            MessageContent::Poll { info } => info.kind as u32,
            _ => PollKind::Disclosed as u32,
        },
    }
}

/// Convert a `Vec<TimelineItem>` to a raw (ptr, len) pair suitable for FFI.
/// The caller is responsible for freeing each item's strings and then the slice itself.
fn timeline_items_to_ffi_raw(items: &[crate::types::TimelineItem]) -> (*mut FfiTimelineItem, u32) {
    if items.is_empty() {
        return (ptr::null_mut(), 0);
    }
    let ffi_items: Vec<FfiTimelineItem> = items.iter().map(timeline_item_to_ffi).collect();
    let len = ffi_items.len() as u32;
    let ptr = leak_boxed_slice(ffi_items);
    (ptr, len)
}

fn empty_room_list() -> FfiRoomList {
    FfiRoomList {
        rooms: ptr::null_mut(),
        len: 0,
    }
}

fn room_summary_to_ffi(room: &RoomSummary) -> FfiRoomSummary {
    let filter_ids = room.filter_ids.clone();
    let filter_ids_len = filter_ids.len();
    let filter_ids_ptr = if filter_ids_len > 0 {
        leak_boxed_slice(filter_ids)
    } else {
        ptr::null_mut()
    };

    let space_ids_vec: Vec<*mut c_char> = room.space_ids.iter().map(|s| str_to_c(s)).collect();
    let space_ids_len = space_ids_vec.len();
    let space_ids_ptr = if space_ids_len > 0 {
        leak_boxed_slice(space_ids_vec)
    } else {
        ptr::null_mut()
    };

    FfiRoomSummary {
        room_id: str_to_c(&room.room_id),
        display_name: str_to_c(&room.display_name),
        canonical_alias: opt_str_to_c(&room.canonical_alias),
        avatar_url: opt_str_to_c(&room.avatar_url),
        avatar_entity_id: str_to_c(&room.avatar_entity_id),
        last_event_text: str_to_c(&room.last_event_text),
        last_event_sender: str_to_c(&room.last_event_sender),
        last_event_timestamp: timestamp_to_epoch(room.last_event_timestamp),
        unread_count: room.unread_count,
        highlight_count: room.highlight_count,
        notification_mode: room.notification_mode as u32,
        is_muted: room.is_muted,
        is_pinned: room.is_pinned,
        pinned_order: room.pinned_order.unwrap_or(-1.0),
        is_marked_unread: room.is_marked_unread,
        is_direct: room.is_direct,
        is_public: room.is_public,
        filter_ids: filter_ids_ptr,
        filter_ids_len,
        space_ids: space_ids_ptr,
        space_ids_len,
        is_last_event_outgoing: room.is_last_event_outgoing,
        is_last_event_service: room.is_last_event_service,
        last_event_send_state: room.last_event_send_state as u32,
        member_count: room.member_count,
        can_pin_messages: room.can_pin_messages,
        peer_presence: room.peer_presence,
        membership: room.membership as u32,
        inviter_user_id: str_to_c(&room.inviter_user_id),
        inviter_display_name: str_to_c(&room.inviter_display_name),
        inviter_avatar_url: str_to_c(&room.inviter_avatar_url),
        room_topic: str_to_c(&room.room_topic),
    }
}

fn rooms_to_ffi_raw(rooms: &[RoomSummary]) -> FfiRoomList {
    if rooms.is_empty() {
        return empty_room_list();
    }
    let ffi_rooms: Vec<FfiRoomSummary> = rooms.iter().map(room_summary_to_ffi).collect();
    let len = ffi_rooms.len();
    let ptr = leak_boxed_slice(ffi_rooms);
    FfiRoomList { rooms: ptr, len }
}

fn empty_timeline_slice() -> FfiTimelineSlice {
    FfiTimelineSlice {
        items: ptr::null_mut(),
        items_count: 0,
        update_kind: TimelineUpdateKind::Full as u32,
        update_index: 0,
        can_paginate_back: false,
        can_paginate_forward: false,
        hit_timeline_start: false,
        is_live: true,
        focus_event_id: ptr::null_mut(),
        pinned_event_ids: ptr::null_mut(),
        pinned_event_ids_count: 0,
        first_unread_event_id: ptr::null_mut(),
        read_marker_loaded: false,
        unread_count: 0,
        unread_state_known: false,
    }
}

fn timeline_slice_to_ffi(slice: &TimelineSlice) -> FfiTimelineSlice {
    let (items_ptr, items_count) = timeline_items_to_ffi_raw(&slice.items);
    let focus_event_id = slice
        .focus_event_id
        .as_deref()
        .and_then(|s| CString::new(s).ok())
        .map(|cs| cs.into_raw())
        .unwrap_or(ptr::null_mut());
    let pinned_count = slice.pinned_event_ids.len() as u32;
    let pinned_ptr = if slice.pinned_event_ids.is_empty() {
        ptr::null_mut()
    } else {
        let ptrs: Vec<*mut c_char> = slice
            .pinned_event_ids
            .iter()
            .map(|id| CString::new(id.as_str()).unwrap_or_default().into_raw())
            .collect();
        leak_boxed_slice(ptrs)
    };

    FfiTimelineSlice {
        items: items_ptr,
        items_count,
        update_kind: slice.update_kind as u32,
        update_index: slice.update_index,
        can_paginate_back: slice.can_paginate_back,
        can_paginate_forward: slice.can_paginate_forward,
        hit_timeline_start: slice.hit_timeline_start,
        is_live: slice.is_live,
        focus_event_id,
        pinned_event_ids: pinned_ptr,
        pinned_event_ids_count: pinned_count,
        first_unread_event_id: opt_str_to_c(&slice.first_unread_event_id),
        read_marker_loaded: slice.read_marker_loaded,
        unread_count: slice.unread_count,
        unread_state_known: slice.unread_state_known,
    }
}

// --- Public FFI API ---

/// Install the global tracing subscriber once. Without it every
/// `tracing::warn!`/`info!` in this crate (sync errors, refresh failures,
/// cache diagnostics) is silently dropped. Logs go to stderr; override the
/// filter with RUST_LOG (e.g. `RUST_LOG=telematrix_protocol=trace`).
fn init_tracing() {
    static INIT: std::sync::Once = std::sync::Once::new();
    INIT.call_once(|| {
        use tracing_subscriber::layer::SubscriberExt as _;
        use tracing_subscriber::util::SubscriberInitExt as _;
        use tracing_subscriber::Layer as _;

        let filter = tracing_subscriber::EnvFilter::try_from_default_env().unwrap_or_else(|_| {
            tracing_subscriber::EnvFilter::new("warn,telematrix_protocol=debug")
        });
        let _ = tracing_subscriber::registry()
            .with(filter)
            .with(
                tracing_subscriber::fmt::layer()
                    .with_writer(std::io::stderr)
                    .with_filter(crate::log_noise::DropExpectedUtdWarnings::from_env()),
            )
            .try_init();
    });
}

/// Raise the open-file soft limit toward the hard cap at startup, returning the
/// resulting soft limit (0 if unknown). A Matrix client legitimately holds a lot of
/// descriptors at once: ~7-8 WAL-mode SQLite DBs (state, crypto, event-cache, media,
/// app-cache, preview-cache, search-index — each ≈3 fds via db/-wal/-shm) behind
/// connection pools, plus sliding-sync/download sockets, the media proxy, and open
/// plaintext media files — a baseline near the ~200 that Element/Telegram also use.
/// macOS starts GUI apps at a low soft limit (often 256), which a burst of that work
/// exhausts, surfacing as EMFILE ("Too many open files") in unrelated places such as
/// media decryption. Logging old→new makes a recurrence at a high limit identifiable
/// as a genuine descriptor leak rather than a too-low ceiling.
#[cfg(unix)]
fn raise_fd_limit() -> u64 {
    // SAFETY: get/setrlimit called with a valid, fully-initialized rlimit pointer.
    unsafe {
        let mut lim = libc::rlimit {
            rlim_cur: 0,
            rlim_max: 0,
        };
        if libc::getrlimit(libc::RLIMIT_NOFILE, &mut lim) != 0 {
            warn!("getrlimit(RLIMIT_NOFILE) failed; leaving fd limit unchanged");
            return 0;
        }
        let old = lim.rlim_cur;
        // macOS reports rlim_max as RLIM_INFINITY but effectively caps at
        // kern.maxfilesperproc, and setrlimit EINVALs if rlim_cur is INFINITY — so
        // use a generous concrete target, clamped to the real hard limit.
        const TARGET: libc::rlim_t = 16_384;
        let hard = if lim.rlim_max == libc::RLIM_INFINITY {
            TARGET
        } else {
            lim.rlim_max
        };
        let target = TARGET.min(hard);
        // libc::rlim_t is u64 on every target we build (macOS/Linux), so the fields
        // and the u64 return need no cast.
        if target <= old {
            info!(old_limit = old, "RLIMIT_NOFILE already ample; unchanged");
            return old;
        }
        lim.rlim_cur = target;
        if libc::setrlimit(libc::RLIMIT_NOFILE, &lim) == 0 {
            info!(old_limit = old, new_limit = target, "raised RLIMIT_NOFILE");
            target
        } else {
            warn!(
                old_limit = old,
                attempted = target,
                "setrlimit(RLIMIT_NOFILE) failed"
            );
            old
        }
    }
}

#[cfg(not(unix))]
fn raise_fd_limit() -> u64 {
    0
}

/// Best-effort count of open file descriptors held by this process, for leak
/// diagnostics. macOS exposes them under /dev/fd, Linux under /proc/self/fd; the
/// read_dir handle itself is subtracted. `None` if neither is readable.
fn open_fd_count() -> Option<usize> {
    for dir in ["/proc/self/fd", "/dev/fd"] {
        if let Ok(entries) = std::fs::read_dir(dir) {
            return Some(entries.count().saturating_sub(1));
        }
    }
    None
}

/// Create a new protocol handle with the Matrix backend.
/// `data_dir` is the path for persistent storage (sqlite store, media cache).
/// If null, defaults to `~/.telematrix`.
///
/// # Safety
/// Returns a heap-allocated Handle. Caller must call `tm_destroy` to free it.
/// `data_dir` must be a valid null-terminated C string, or null.
#[no_mangle]
pub unsafe extern "C" fn tm_create(data_dir: *const c_char) -> *mut Handle {
    init_tracing();
    let fd_limit = raise_fd_limit();
    if let Some(n) = open_fd_count() {
        info!(open_fds = n, soft_limit = fd_limit, "fd usage at startup");
    }
    // DEFAULT worker_threads on purpose: trimming them halved decryption's async
    // parallelism and stuck fresh-login decryption in a reverted attempt. Only the
    // blocking-pool ceiling is capped here — that is where the per-account-runtime
    // (up to 6) thread explosion lives (default 512 each), and it does not affect
    // decryption throughput. 64 stays well above this Handle's total SQLite pool
    // size. See docs/thread-count-review-2026-07-08.md and code-review-2026-07-19 PERF-1.
    // Name the threads. Multiaccount means up to six runtimes, and tokio's
    // default leaves every one of ~74 threads called "tokio-runtime-worker" —
    // which makes a profiler's thread list unreadable and impossible to
    // attribute to an account. `a0-w7` = account 0, worker 7.
    let account = ACCOUNT_ORDINAL.fetch_add(1, Ordering::Relaxed);
    let worker_seq = std::sync::Arc::new(AtomicUsize::new(0));
    let runtime = match tokio::runtime::Builder::new_multi_thread()
        .thread_name_fn(move || {
            let n = worker_seq.fetch_add(1, Ordering::Relaxed);
            format!("a{account}-w{n}")
        })
        .max_blocking_threads(64)
        .enable_all()
        .build()
    {
        Ok(rt) => rt,
        Err(_) => return ptr::null_mut(),
    };
    crate::runtime_stats::spawn(&runtime, account);

    let dir = if data_dir.is_null() {
        dirs::data_dir()
            .unwrap_or_else(|| PathBuf::from("."))
            .join("telematrix")
    } else {
        PathBuf::from(c_to_string(data_dir))
    };

    // Count this account's Handle so the media-cache budget is split across live
    // accounts (paired with note_account_stopped in tm_destroy). See PERF-4.
    crate::cache_manager::note_account_started();
    let matrix = Arc::new(MatrixProtocol::new(runtime.handle().clone(), dir));
    let client: Arc<dyn ProtocolClient> = matrix.clone();
    Box::into_raw(Box::new(Handle {
        runtime,
        client,
        matrix,
        active_searches: Arc::new(Mutex::new(HashSet::new())),
        active_directory_requests: Arc::new(Mutex::new(HashSet::new())),
        active_media_downloads: Arc::new(Mutex::new(HashMap::new())),
    }))
}

// --- Session management FFI types ---

/// C-compatible session info for persistence.
#[repr(C)]
pub struct FfiSessionInfo {
    pub homeserver: *mut c_char,
    pub user_id: *mut c_char,
    pub device_id: *mut c_char,
    pub access_token: *mut c_char,
}

/// Callback type for session restore result. On failure `error` says why, which
/// the caller needs: a restore can fail for reasons that have nothing to do with
/// the token being dead (a timeout, an unreachable homeserver, a slow cold start),
/// and those must not cost the user their session.
pub type TmSessionCallback = extern "C" fn(
    success: bool,
    user_id: *const c_char,
    display_name: *const c_char,
    avatar_url: *const c_char,
    error: *const c_char,
    userdata: *mut libc::c_void,
);

/// Callback type for homeserver discovery result.
pub type TmDiscoverCallback =
    extern "C" fn(success: bool, homeserver_url: *const c_char, userdata: *mut libc::c_void);

/// Callback type for the auth-delegation probe result. `account_url` is the
/// server's account-management website when `delegated`, else null.
pub type TmAuthDelegationCallback =
    extern "C" fn(delegated: bool, account_url: *const c_char, userdata: *mut libc::c_void);

/// Callback for the registration-capability classification. `status`: 0 = not a
/// Matrix server (`url` null), 1 = password registration (`url` = resolved base
/// URL), 2 = delegated auth/OIDC (`url` = registration website).
pub type TmClassifyRegistrationCallback =
    extern "C" fn(status: i32, url: *const c_char, userdata: *mut libc::c_void);

/// Callback type for a per-medium 3PID support probe. `known` is false when the
/// server's answer settles nothing, and `supported` is then meaningless.
pub type TmThreepidSupportCallback =
    extern "C" fn(known: bool, supported: bool, userdata: *mut libc::c_void);

/// Callback type for media resolution result.
/// `terminal` is true when the failure is permanent (HTTP 4xx except 429) and the
/// caller should stop retrying; it is always false on success or a transient failure.
pub type TmMediaCallback = extern "C" fn(
    success: bool,
    local_path: *const c_char,
    terminal: bool,
    userdata: *mut libc::c_void,
);

/// Callback type for in-memory media resolution result. See [`TmMediaCallback`] for
/// the `terminal` flag semantics.
pub type TmMediaBytesCallback = extern "C" fn(
    success: bool,
    bytes: *const u8,
    bytes_len: usize,
    mime: *const c_char,
    filename: *const c_char,
    terminal: bool,
    userdata: *mut libc::c_void,
);

/// Callback type for incremental media download progress.
pub type TmMediaProgressCallback =
    extern "C" fn(received_bytes: u64, total_bytes: u64, phase: u32, userdata: *mut libc::c_void);

/// Get the current session info after login (for persistence).
/// Returns a FfiSessionInfo with all fields set, or all null pointers if not logged in.
///
/// # Safety
/// `h` must be a valid Handle pointer.
/// Caller must call `tm_free_session_info` on the returned struct.
#[no_mangle]
pub unsafe extern "C" fn tm_get_session_info(h: *mut Handle) -> FfiSessionInfo {
    let handle = unsafe { &*h };
    let empty = FfiSessionInfo {
        homeserver: ptr::null_mut(),
        user_id: ptr::null_mut(),
        device_id: ptr::null_mut(),
        access_token: ptr::null_mut(),
    };

    let matrix = &handle.matrix;

    match handle
        .runtime
        .block_on(async { matrix.get_session_info().await })
    {
        Ok(info) => FfiSessionInfo {
            homeserver: str_to_c(&info.homeserver),
            user_id: str_to_c(&info.user_id),
            device_id: str_to_c(&info.device_id),
            access_token: str_to_c(&info.access_token),
        },
        Err(_) => empty,
    }
}

/// Free a session info struct returned by `tm_get_session_info`.
///
/// # Safety
/// `info` must have been returned by `tm_get_session_info`.
#[no_mangle]
pub unsafe extern "C" fn tm_free_session_info(info: FfiSessionInfo) {
    unsafe {
        free_c_string(info.homeserver);
        free_c_string(info.user_id);
        free_c_string(info.device_id);
        free_c_string(info.access_token);
    }
}

/// Asynchronously restore a session from saved tokens (skip login).
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `homeserver`, `user_id`, `device_id`, `access_token` must be valid C strings.
/// - `callback` will be invoked on a background thread.
#[no_mangle]
pub unsafe extern "C" fn tm_restore_session(
    h: *mut Handle,
    homeserver: *const c_char,
    user_id: *const c_char,
    device_id: *const c_char,
    access_token: *const c_char,
    callback: TmSessionCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);

    let matrix = handle.matrix.clone();

    let info = crate::types::SessionInfo {
        homeserver: c_to_string(homeserver),
        user_id: c_to_string(user_id),
        device_id: c_to_string(device_id),
        access_token: c_to_string(access_token),
    };

    handle.runtime.spawn(async move {
        match matrix.restore_session(&info).await {
            Ok(profile) => {
                let user_id = CString::new(profile.user_id).unwrap_or_default();
                let display_name = CString::new(profile.display_name).unwrap_or_default();
                let avatar_url = profile.avatar_url.and_then(|url| CString::new(url).ok());
                callback(
                    true,
                    user_id.as_ptr(),
                    display_name.as_ptr(),
                    avatar_url.as_ref().map_or(ptr::null(), |s| s.as_ptr()),
                    ptr::null(),
                    ud.as_ptr(),
                );
            }
            Err(e) => {
                tracing::error!("Session restore failed: {e}");
                let error = CString::new(e.to_string()).unwrap_or_default();
                callback(
                    false,
                    ptr::null(),
                    ptr::null(),
                    ptr::null(),
                    error.as_ptr(),
                    ud.as_ptr(),
                );
            }
        }
    });
}

/// Asynchronously discover the homeserver URL for a domain via .well-known.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `domain` must be a valid null-terminated C string (e.g. "matrix.org").
#[no_mangle]
pub unsafe extern "C" fn tm_discover_homeserver(
    h: *mut Handle,
    domain: *const c_char,
    callback: TmDiscoverCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let domain_str = c_to_string(domain);
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match AuthService::discover_homeserver(&domain_str).await {
            Ok(url) => {
                let url_c = CString::new(url).unwrap_or_default();
                callback(true, url_c.as_ptr(), ud.as_ptr());
            }
            Err(_) => callback(false, ptr::null(), ud.as_ptr()),
        }
    });
}

/// Asynchronously classify a homeserver's registration capability for the
/// two-step register flow: resolves the base URL, validates it is a Matrix
/// server, then checks for OIDC/MAS auth delegation. See
/// `TmClassifyRegistrationCallback` for the status codes.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `input` must be a valid null-terminated C string (a bare domain or URL).
#[no_mangle]
pub unsafe extern "C" fn tm_classify_registration(
    h: *mut Handle,
    input: *const c_char,
    callback: TmClassifyRegistrationCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let input_str = c_to_string(input);
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let (status, url) = AuthService::classify_registration(&input_str).await;
        if url.is_empty() {
            callback(status, ptr::null(), ud.as_ptr());
        } else {
            let url_c = CString::new(url).unwrap_or_default();
            callback(status, url_c.as_ptr(), ud.as_ptr());
        }
    });
}

/// Asynchronously probe whether a homeserver delegates auth to OIDC/MAS
/// (MSC2965 auth_metadata) — such servers reject legacy registration and
/// create accounts on their website instead.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `base_url` must be a valid null-terminated C string (a resolved
///   homeserver base URL, e.g. "https://matrix-client.matrix.org").
#[no_mangle]
pub unsafe extern "C" fn tm_probe_auth_delegation(
    h: *mut Handle,
    base_url: *const c_char,
    callback: TmAuthDelegationCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let base = c_to_string(base_url);
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match AuthService::probe_auth_delegation(&base).await {
            Some(url) => {
                let url_c = CString::new(url).unwrap_or_default();
                callback(true, url_c.as_ptr(), ud.as_ptr());
            }
            None => callback(false, ptr::null(), ud.as_ptr()),
        }
    });
}

/// Asynchronously fetch the page where a delegated-auth (OIDC/MAS) homeserver
/// lets someone reset a forgotten password. `available` is false on servers that
/// handle password reset themselves, where there is nothing to link to.
///
/// Unlike `tm_probe_account_management` this takes the RAW homeserver the user
/// typed ("matrix.org") and resolves it itself, because the forgot-password
/// screen never resolves one.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `homeserver` must be a valid null-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn tm_probe_password_reset_page(
    h: *mut Handle,
    homeserver: *const c_char,
    callback: TmAuthDelegationCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let input = c_to_string(homeserver);
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match AuthService::probe_password_reset_page(&input).await {
            Some(url) => {
                let url_c = CString::new(url).unwrap_or_default();
                callback(true, url_c.as_ptr(), ud.as_ptr());
            }
            None => callback(false, ptr::null(), ud.as_ptr()),
        }
    });
}

/// Asynchronously fetch the website where a delegated-auth (OIDC/MAS) homeserver
/// manages account details — email addresses and phone numbers included, since
/// such servers disable the legacy 3PID API. `available` is false on servers that
/// manage those themselves (nothing to link to).
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `base_url` must be a valid null-terminated C string (a resolved
///   homeserver base URL, e.g. "https://matrix-client.matrix.org").
#[no_mangle]
pub unsafe extern "C" fn tm_probe_account_management(
    h: *mut Handle,
    base_url: *const c_char,
    callback: TmAuthDelegationCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let base = c_to_string(base_url);
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match AuthService::probe_account_management(&base).await {
            Some(url) => {
                let url_c = CString::new(url).unwrap_or_default();
                callback(true, url_c.as_ptr(), ud.as_ptr());
            }
            None => callback(false, ptr::null(), ud.as_ptr()),
        }
    });
}

/// Asynchronously ask whether the homeserver can verify email addresses at all.
/// `known` is false when the server's answer settles nothing, in which case the
/// caller must assume email works.
#[no_mangle]
pub unsafe extern "C" fn tm_probe_email_threepid_support(
    h: *mut Handle,
    base_url: *const c_char,
    callback: TmThreepidSupportCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let base = c_to_string(base_url);
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match AuthService::probe_email_threepid_support(&base).await {
            Some(supported) => callback(true, supported, ud.as_ptr()),
            None => callback(false, false, ud.as_ptr()),
        }
    });
}

/// Asynchronously logout and clear session state.
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_logout(
    h: *mut Handle,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);

    let matrix = handle.matrix.clone();

    handle.runtime.spawn(async move {
        let success = matrix.logout().await.is_ok();
        callback(success, ud.as_ptr());
    });
}

/// Asynchronously resolve an avatar via a server thumbnail (`size`×`size`), stored
/// under the same key a full resolve would use so the result arrives on the same
/// `mediaResolved` path. Falls back to the full image if the server can't thumbnail.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `mxc_url` must be a valid null-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn tm_resolve_avatar(
    h: *mut Handle,
    mxc_url: *const c_char,
    size: u32,
    callback: TmMediaCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let mxc = c_to_string(mxc_url);
    let ud = Userdata::new(userdata);

    let matrix = handle.matrix.clone();
    let active_downloads = handle.active_media_downloads.clone();
    let completion = Arc::new(MediaDownloadCompletion::new(callback, ud));
    let completion_for_task = completion.clone();
    let active_for_task = active_downloads.clone();
    let active_key = format!("media:{mxc}");

    let task = handle.runtime.spawn(async move {
        match matrix.resolve_avatar(&mxc, size).await {
            Ok(path) => {
                let path_str = CString::new(path.to_string_lossy().as_ref()).unwrap_or_default();
                completion_for_task.call_once(true, path_str.as_ptr(), false);
            }
            Err(e) => {
                warn!("[RESOLVE-ERR] avatar {mxc}: {e}");
                let terminal = crate::media_transfer_service::is_permanent_media_error(&e);
                completion_for_task.call_once(false, ptr::null(), terminal);
            }
        }

        let mut active = lock_ffi_mutex(&active_for_task, "active_media_downloads");
        active.retain(|_, current| !current.completion.matches_path(&completion_for_task));
    });

    let previous_completion = {
        let mut active = lock_ffi_mutex(&active_downloads, "active_media_downloads");
        let previous = active.insert(
            active_key,
            ActiveMediaDownload {
                task,
                completion: ActiveMediaCompletion::Path(completion.clone()),
            },
        );
        if completion.completed.load(Ordering::Acquire) {
            active.retain(|_, current| !current.completion.matches_path(&completion));
        }
        previous.map(|previous| {
            track_aborted_media_task_locked(&mut active, "media-replaced", previous)
        })
    };
    if let Some(completion) = previous_completion {
        completion.call_cancelled();
    }
}

/// Asynchronously resolve an mxc:// URL to a local file path with progress.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `mxc_url` must be a valid null-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn tm_resolve_media_with_progress(
    h: *mut Handle,
    mxc_url: *const c_char,
    progress_callback: TmMediaProgressCallback,
    callback: TmMediaCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let mxc = c_to_string(mxc_url);
    let ud = Userdata::new(userdata);

    let matrix = handle.matrix.clone();
    let active_downloads = handle.active_media_downloads.clone();
    let completion = Arc::new(MediaDownloadCompletion::new(callback, ud));
    let completion_for_task = completion.clone();
    let completion_for_progress = completion.clone();
    let active_for_task = active_downloads.clone();
    let mxc_for_request = mxc.clone();

    let task = handle.runtime.spawn(async move {
        match matrix
            .resolve_media_with_progress(&mxc_for_request, |received, total, phase| {
                completion_for_progress.progress(progress_callback, received, total, phase);
            })
            .await
        {
            Ok(path) => {
                let path_str = CString::new(path.to_string_lossy().as_ref()).unwrap_or_default();
                completion_for_task.call_once(true, path_str.as_ptr(), false);
            }
            Err(e) => {
                warn!("[RESOLVE-ERR] {mxc_for_request}: {e}");
                let terminal = crate::media_transfer_service::is_permanent_media_error(&e);
                completion_for_task.call_once(false, ptr::null(), terminal);
            }
        }

        let mut active = lock_ffi_mutex(&active_for_task, "active_media_downloads");
        active.retain(|_, current| !current.completion.matches_path(&completion_for_task));
    });

    let previous_completion = {
        let mut active = lock_ffi_mutex(&active_downloads, "active_media_downloads");
        let previous = active.insert(
            mxc,
            ActiveMediaDownload {
                task,
                completion: ActiveMediaCompletion::Path(completion.clone()),
            },
        );
        if completion.completed.load(Ordering::Acquire) {
            active.retain(|_, current| !current.completion.matches_path(&completion));
        }
        previous.map(|previous| {
            track_aborted_media_task_locked(&mut active, "progress-replaced", previous)
        })
    };
    if let Some(completion) = previous_completion {
        completion.call_cancelled();
    }
}

/// Asynchronously resolve an mxc:// URL into decrypted bytes without writing
/// plaintext media to a temp file.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `mxc_url` must be a valid null-terminated C string.
/// - `callback` receives a byte pointer that is valid only for the duration of
///   the callback.
#[no_mangle]
pub unsafe extern "C" fn tm_resolve_media_bytes_with_progress(
    h: *mut Handle,
    mxc_url: *const c_char,
    progress_callback: TmMediaProgressCallback,
    callback: TmMediaBytesCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let mxc = c_to_string(mxc_url);
    let ud = Userdata::new(userdata);

    let matrix = handle.matrix.clone();
    let active_downloads = handle.active_media_downloads.clone();
    let completion = Arc::new(MediaBytesCompletion::new(callback, ud));
    let completion_for_task = completion.clone();
    let completion_for_progress = completion.clone();
    let active_for_task = active_downloads.clone();
    let mxc_for_request = mxc.clone();
    let active_key = format!("bytes:{mxc}");

    let task = handle.runtime.spawn(async move {
        match matrix
            .resolve_media_bytes_with_progress(&mxc_for_request, |received, total, phase| {
                completion_for_progress.progress(progress_callback, received, total, phase);
            })
            .await
        {
            Ok(bytes) => {
                let mime = CString::new("").unwrap_or_default();
                let filename = CString::new("").unwrap_or_default();
                completion_for_task.call_once(
                    true,
                    bytes.as_ptr(),
                    bytes.len(),
                    mime.as_ptr(),
                    filename.as_ptr(),
                    false,
                );
            }
            Err(e) => {
                warn!("[RESOLVE-ERR] bytes {mxc_for_request}: {e}");
                let terminal = crate::media_transfer_service::is_permanent_media_error(&e);
                completion_for_task.call_once(
                    false,
                    ptr::null(),
                    0,
                    ptr::null(),
                    ptr::null(),
                    terminal,
                );
            }
        }

        let mut active = lock_ffi_mutex(&active_for_task, "active_media_downloads");
        active.retain(|_, current| !current.completion.matches_bytes(&completion_for_task));
    });

    let previous_completion = {
        let mut active = lock_ffi_mutex(&active_downloads, "active_media_downloads");
        let previous = active.insert(
            active_key,
            ActiveMediaDownload {
                task,
                completion: ActiveMediaCompletion::Bytes(completion.clone()),
            },
        );
        if completion.completed.load(Ordering::Acquire) {
            active.retain(|_, current| !current.completion.matches_bytes(&completion));
        }
        previous.map(|previous| {
            track_aborted_media_task_locked(&mut active, "bytes-replaced", previous)
        })
    };
    if let Some(completion) = previous_completion {
        completion.call_cancelled();
    }
}

/// Cancel an in-flight media download started by `tm_resolve_media_with_progress`.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `mxc_url` must be a valid null-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn tm_cancel_media_download(h: *mut Handle, mxc_url: *const c_char) {
    if h.is_null() || mxc_url.is_null() {
        return;
    }
    let handle = unsafe { &*h };
    let mxc = c_to_string(mxc_url);
    let bytes_key = format!("bytes:{mxc}");
    let completions = {
        let mut active = lock_ffi_mutex(&handle.active_media_downloads, "active_media_downloads");
        [mxc, bytes_key]
            .into_iter()
            .filter_map(|key| {
                active
                    .remove(&key)
                    .map(|item| track_aborted_media_task_locked(&mut active, "cancelled", item))
            })
            .collect::<Vec<_>>()
    };
    for completion in completions {
        completion.call_cancelled();
    }
}

/// Resolve a server-generated thumbnail for a media URL.
/// Uses the Matrix thumbnail API — much faster than downloading full video.
/// `allow_partial_video` enables the 2MB partial-download fallback for video
/// frame extraction; image / OG-card callers pass false (see
/// `resolve_media_thumbnail`).
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `mxc_url` must be a valid null-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn tm_resolve_media_thumbnail(
    h: *mut Handle,
    mxc_url: *const c_char,
    width: u32,
    height: u32,
    allow_partial_video: bool,
    callback: TmMediaCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let mxc = c_to_string(mxc_url);
    let ud = Userdata::new(userdata);

    let matrix = handle.matrix.clone();
    let active_downloads = handle.active_media_downloads.clone();
    let completion = Arc::new(MediaDownloadCompletion::new(callback, ud));
    let completion_for_task = completion.clone();
    let active_for_task = active_downloads.clone();
    let active_key = format!("thumb:{mxc}:{width}x{height}");

    let task = handle.runtime.spawn(async move {
        match matrix
            .resolve_media_thumbnail(&mxc, width, height, allow_partial_video)
            .await
        {
            Ok(path) => {
                let path_str = CString::new(path.to_string_lossy().as_ref()).unwrap_or_default();
                completion_for_task.call_once(true, path_str.as_ptr(), false);
            }
            Err(e) => {
                warn!("[RESOLVE-ERR] thumbnail {mxc}: {e}");
                let terminal = crate::media_transfer_service::is_permanent_media_error(&e);
                completion_for_task.call_once(false, ptr::null(), terminal);
            }
        }

        let mut active = lock_ffi_mutex(&active_for_task, "active_media_downloads");
        active.retain(|_, current| !current.completion.matches_path(&completion_for_task));
    });

    let previous_completion = {
        let mut active = lock_ffi_mutex(&active_downloads, "active_media_downloads");
        let previous = active.insert(
            active_key,
            ActiveMediaDownload {
                task,
                completion: ActiveMediaCompletion::Path(completion.clone()),
            },
        );
        if completion.completed.load(Ordering::Acquire) {
            active.retain(|_, current| !current.completion.matches_path(&completion));
        }
        previous.map(|previous| {
            track_aborted_media_task_locked(&mut active, "thumb-replaced", previous)
        })
    };
    if let Some(completion) = previous_completion {
        completion.call_cancelled();
    }
}

/// Resolve a server-generated thumbnail into decrypted bytes.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `mxc_url` must be a valid null-terminated C string.
/// - `callback` receives a byte pointer that is valid only for the duration of
///   the callback.
#[no_mangle]
pub unsafe extern "C" fn tm_resolve_media_thumbnail_bytes(
    h: *mut Handle,
    mxc_url: *const c_char,
    width: u32,
    height: u32,
    callback: TmMediaBytesCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let mxc = c_to_string(mxc_url);
    let ud = Userdata::new(userdata);

    let matrix = handle.matrix.clone();
    let active_downloads = handle.active_media_downloads.clone();
    let completion = Arc::new(MediaBytesCompletion::new(callback, ud));
    let completion_for_task = completion.clone();
    let active_for_task = active_downloads.clone();
    let active_key = format!("thumb-bytes:{mxc}:{width}x{height}");
    let mxc_for_request = mxc.clone();

    let task = handle.runtime.spawn(async move {
        match matrix
            .resolve_media_thumbnail_bytes(&mxc_for_request, width, height)
            .await
        {
            Ok(bytes) => {
                let mime = CString::new("").unwrap_or_default();
                let filename = CString::new("").unwrap_or_default();
                completion_for_task.call_once(
                    true,
                    bytes.as_ptr(),
                    bytes.len(),
                    mime.as_ptr(),
                    filename.as_ptr(),
                    false,
                );
            }
            Err(e) => {
                warn!("[RESOLVE-ERR] thumbnail bytes {mxc_for_request}: {e}");
                let terminal = crate::media_transfer_service::is_permanent_media_error(&e);
                completion_for_task.call_once(
                    false,
                    ptr::null(),
                    0,
                    ptr::null(),
                    ptr::null(),
                    terminal,
                );
            }
        }

        let mut active = lock_ffi_mutex(&active_for_task, "active_media_downloads");
        active.retain(|_, current| !current.completion.matches_bytes(&completion_for_task));
    });

    let previous_completion = {
        let mut active = lock_ffi_mutex(&active_downloads, "active_media_downloads");
        let previous = active.insert(
            active_key,
            ActiveMediaDownload {
                task,
                completion: ActiveMediaCompletion::Bytes(completion.clone()),
            },
        );
        if completion.completed.load(Ordering::Acquire) {
            active.retain(|_, current| !current.completion.matches_bytes(&completion));
        }
        previous.map(|previous| {
            track_aborted_media_task_locked(&mut active, "thumb-bytes-replaced", previous)
        })
    };
    if let Some(completion) = previous_completion {
        completion.call_cancelled();
    }
}

/// Produce a locally-decoded JPEG thumbnail for a video.
///
/// Returns the thumbnail from the encrypted on-disk cache when available
/// (keyed by `event_id`), otherwise resolves the video, decodes one frame via
/// ffmpeg, scales it to `width`×`height`, caches it, and returns the JPEG
/// bytes. The bytes are delivered through `callback` as a `TmMediaBytesCallback`
/// (success, bytes ptr, length, mime, filename, userdata); `mime` is
/// `"image/jpeg"` on success. The byte pointer is valid only for the duration
/// of the callback.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `event_id` and `mxc_url` must be valid null-terminated C strings.
/// - `callback` receives a byte pointer that is valid only for the duration of
///   the callback.
#[no_mangle]
pub unsafe extern "C" fn tm_get_video_thumbnail(
    h: *mut Handle,
    event_id: *const c_char,
    mxc_url: *const c_char,
    width: u32,
    height: u32,
    callback: TmMediaBytesCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let event_id = c_to_string(event_id);
    let mxc = c_to_string(mxc_url);
    let ud = Userdata::new(userdata);

    let matrix = handle.matrix.clone();
    let active_downloads = handle.active_media_downloads.clone();
    let completion = Arc::new(MediaBytesCompletion::new(callback, ud));
    let completion_for_task = completion.clone();
    let active_for_task = active_downloads.clone();
    let active_key = format!("vidthumb:{event_id}:{width}x{height}");
    let event_id_for_request = event_id.clone();
    let mxc_for_request = mxc.clone();

    let task = handle.runtime.spawn(async move {
        match matrix
            .get_video_thumbnail(&event_id_for_request, &mxc_for_request, width, height)
            .await
        {
            Ok(bytes) => {
                let mime = CString::new("image/jpeg").unwrap_or_default();
                let filename = CString::new("").unwrap_or_default();
                completion_for_task.call_once(
                    true,
                    bytes.as_ptr(),
                    bytes.len(),
                    mime.as_ptr(),
                    filename.as_ptr(),
                    false,
                );
            }
            Err(e) => {
                warn!(
                    "[RESOLVE-ERR] video thumbnail {event_id_for_request} ({mxc_for_request}): {e}"
                );
                let terminal = crate::media_transfer_service::is_permanent_media_error(&e);
                completion_for_task.call_once(
                    false,
                    ptr::null(),
                    0,
                    ptr::null(),
                    ptr::null(),
                    terminal,
                );
            }
        }

        let mut active = lock_ffi_mutex(&active_for_task, "active_media_downloads");
        active.retain(|_, current| !current.completion.matches_bytes(&completion_for_task));
    });

    let previous_completion = {
        let mut active = lock_ffi_mutex(&active_downloads, "active_media_downloads");
        let previous = active.insert(
            active_key,
            ActiveMediaDownload {
                task,
                completion: ActiveMediaCompletion::Bytes(completion.clone()),
            },
        );
        if completion.completed.load(Ordering::Acquire) {
            active.retain(|_, current| !current.completion.matches_bytes(&completion));
        }
        previous.map(|previous| {
            track_aborted_media_task_locked(&mut active, "vidthumb-replaced", previous)
        })
    };
    if let Some(completion) = previous_completion {
        completion.call_cancelled();
    }
}

/// Export an mxc:// media item directly to a target path.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `mxc_url` and `target_path` must be valid null-terminated C strings.
#[no_mangle]
pub unsafe extern "C" fn tm_export_media_to_path(
    h: *mut Handle,
    mxc_url: *const c_char,
    target_path: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let mxc = c_to_string(mxc_url);
    let target = c_to_string(target_path);
    let ud = Userdata::new(userdata);

    let matrix = handle.matrix.clone();
    handle.runtime.spawn(async move {
        match matrix.export_media_to_path(&mxc, &target).await {
            Ok(()) => callback(true, ud.as_ptr()),
            Err(e) => {
                warn!("[EXPORT-ERR] {mxc} -> {target}: {e}");
                callback(false, ud.as_ptr());
            }
        }
    });
}

/// Clear persistent callbacks registered with the protocol handle.
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_clear_callbacks(h: *mut Handle) {
    if h.is_null() {
        return;
    }
    let handle = unsafe { &*h };
    handle.matrix.clear_callbacks();
}

/// Destroy a protocol handle and free all associated resources.
///
/// # Safety
/// `h` must be a valid pointer returned by `tm_create`, and must not be used after this call.
#[no_mangle]
pub unsafe extern "C" fn tm_destroy(h: *mut Handle, max_drain_ms: u64) {
    if h.is_null() {
        return;
    }
    let handle = unsafe { Box::from_raw(h) };
    // Paired with note_account_started in tm_create: this account's dirs no longer
    // count toward the shared media budget. See PERF-4.
    crate::cache_manager::note_account_stopped();
    // Drop the client (and matrix) inside the Tokio runtime context so that
    // deadpool/SQLite connection pool cleanup can spawn blocking tasks.
    let Handle {
        runtime,
        client,
        matrix,
        active_searches: _,
        active_directory_requests: _,
        active_media_downloads,
    } = *handle;
    runtime.block_on(abort_and_await_active_media_tasks(active_media_downloads));
    matrix.cleanup_plaintext_media_cache();
    matrix.abort_background_tasks();
    {
        let _guard = runtime.enter();
        drop(matrix);
        drop(client);
    }
    // Bounded drain before shutdown. The real hazard is cancelling an in-flight
    // deadpool spawn_blocking (sqlite connection close) — that aborts under
    // panic=abort. Those blocking tasks finish in well under the cap. We do NOT
    // expect num_alive_tasks() (async tasks) to reach 0: matrix-sdk's
    // sync/sliding-sync background tasks stay parked on long-poll network I/O and
    // never exit on client drop, so the loop normally runs to `max_drain_ms` and
    // then lets shutdown_timeout abort those async tasks (safe — only blocking
    // cancellation panics). Keep max_drain_ms small (callers pass ~1.5-2s).
    let drain_start = std::time::Instant::now();
    let initial_alive = runtime.metrics().num_alive_tasks();
    let drain_deadline = drain_start + std::time::Duration::from_millis(max_drain_ms);
    while runtime.metrics().num_alive_tasks() > 0 {
        if std::time::Instant::now() >= drain_deadline {
            warn!(
                "tm_destroy: {} of {initial_alive} task(s) still alive after {:?}; shutting down anyway",
                runtime.metrics().num_alive_tasks(),
                drain_start.elapsed()
            );
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(25));
    }
    info!(
        "tm_destroy: drained {initial_alive} task(s) in {:?}",
        drain_start.elapsed()
    );
    runtime.shutdown_timeout(std::time::Duration::from_secs(2));
}

/// Callback type for login result.
pub type TmLoginCallback = extern "C" fn(
    success: bool,
    user_id: *const c_char,
    display_name: *const c_char,
    avatar_url: *const c_char,
    userdata: *mut libc::c_void,
);

/// Callback type for send message result.
pub type TmSendCallback =
    extern "C" fn(success: bool, event_id: *const c_char, userdata: *mut libc::c_void);

/// Callback type for simple success/failure result.
pub type TmSimpleCallback = extern "C" fn(success: bool, userdata: *mut libc::c_void);

/// Callback carrying success plus an owned error message (null on success). The
/// C++ side takes ownership of `error` and frees it. Used where the UI must show
/// the server's failure reason (e.g. folder operations).
pub type TmResultCallback =
    extern "C" fn(success: bool, error: *mut c_char, userdata: *mut libc::c_void);

/// Invoke a `TmResultCallback` from a `Result`, passing the error text on failure.
fn deliver_result(callback: TmResultCallback, result: anyhow::Result<()>, ud: Userdata) {
    match result {
        Ok(()) => callback(true, ptr::null_mut(), ud.as_ptr()),
        Err(e) => callback(false, str_to_c(&e.to_string()), ud.as_ptr()),
    }
}

/// Callback type for room list change notifications.
pub type TmRoomChangeCallback = extern "C" fn(userdata: *mut libc::c_void);

/// Callback type for timeline change notifications.
pub type TmTimelineChangeCallback = extern "C" fn(userdata: *mut libc::c_void);

/// Callback type for sync state change notifications.
/// state: 0 = not started, 1 = syncing, 2 = synced.
pub type TmSyncStateCallback = extern "C" fn(state: u32, userdata: *mut libc::c_void);

/// Callback type for per-message desktop notifications. All `*const c_char` are
/// valid only for the duration of the call; copy before retaining.
pub type TmNotificationCallback = extern "C" fn(
    room_id: *const c_char,
    event_id: *const c_char,
    sender_display_name: *const c_char,
    sender_avatar_url: *const c_char,
    room_display_name: *const c_char,
    body: *const c_char,
    is_direct: bool,
    is_mention: bool,
    timestamp: u64,
    userdata: *mut libc::c_void,
);

/// Callback type for room-invite desktop notifications. Unlike messages an invite
/// has no event id / mention / timestamp; `is_direct` distinguishes a DM invite.
pub type TmInviteNotificationCallback = extern "C" fn(
    room_id: *const c_char,
    inviter_display_name: *const c_char,
    inviter_avatar_url: *const c_char,
    room_display_name: *const c_char,
    is_direct: bool,
    userdata: *mut libc::c_void,
);

/// Callback type for a "new login" — a newly-appeared, unverified session on the
/// account (the "New login. Was this you?" alert). `last_seen_ts` is UNIX secs
/// (0 if unknown); `last_seen_ip` may be empty.
pub type TmNewLoginCallback = extern "C" fn(
    device_id: *const c_char,
    display_name: *const c_char,
    last_seen_ip: *const c_char,
    last_seen_ts: u64,
    userdata: *mut libc::c_void,
);

/// Callback type for a remote sign-out: the homeserver rejected this session's
/// access token (`M_UNKNOWN_TOKEN`), so the account must be signed out locally.
/// `soft_logout` mirrors the spec flag — the device survived and the session
/// could in principle be re-authenticated, rather than being gone for good.
/// Fires at most once per session.
pub type TmSessionInvalidatedCallback =
    extern "C" fn(soft_logout: bool, userdata: *mut libc::c_void);

/// Callback fired around a room's member fetch: `in_progress` true when it
/// starts, false when it finishes.
pub type TmMemberSyncCallback =
    extern "C" fn(room_id: *const c_char, in_progress: bool, userdata: *mut libc::c_void);

/// Callback type for presence change notifications.
/// state: 0 = offline, 1 = online, 2 = unavailable.
/// last_active_ts: UNIX epoch seconds of last activity, or 0 if unknown.
pub type TmPresenceCallback = extern "C" fn(
    user_id: *const c_char,
    state: u32,
    last_active_ts: u64,
    userdata: *mut libc::c_void,
);

/// Callback type for typing notification updates.
/// room_id: the room where typing changed.
/// user_ids: array of user ID strings currently typing (may be empty).
/// user_count: number of entries in user_ids.
pub type TmTypingCallback = extern "C" fn(
    room_id: *const c_char,
    user_ids: *const *const c_char,
    user_count: usize,
    userdata: *mut libc::c_void,
);

// --- Verification FFI types ---

/// C-compatible SAS emoji.
#[repr(C)]
pub struct FfiSasEmoji {
    pub emoji: *mut c_char,
    pub label: *mut c_char,
}

/// C-compatible SAS emoji list (always 7 entries on success).
#[repr(C)]
pub struct FfiSasEmojiList {
    pub emojis: *mut FfiSasEmoji,
    pub len: usize,
}

/// Callback for SAS emoji result.
pub type TmSasCallback =
    extern "C" fn(success: bool, list: FfiSasEmojiList, userdata: *mut libc::c_void);

/// Asynchronously log in to the protocol backend.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `homeserver`, `user`, `pass` must be valid null-terminated C strings.
/// - `callback` will be invoked on a background thread when login completes.
/// - `userdata` is passed through to the callback unchanged.
#[no_mangle]
pub unsafe extern "C" fn tm_login(
    h: *mut Handle,
    homeserver: *const c_char,
    user: *const c_char,
    pass: *const c_char,
    callback: TmLoginCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let homeserver = c_to_string(homeserver);
    let user = c_to_string(user);
    let pass = c_to_string(pass);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client.login(&homeserver, &user, &pass).await {
            Ok(profile) => {
                let user_id = CString::new(profile.user_id).unwrap_or_default();
                let display_name = CString::new(profile.display_name).unwrap_or_default();
                let avatar_url = profile.avatar_url.and_then(|url| CString::new(url).ok());
                callback(
                    true,
                    user_id.as_ptr(),
                    display_name.as_ptr(),
                    avatar_url.as_ref().map_or(ptr::null(), |s| s.as_ptr()),
                    ud.as_ptr(),
                );
            }
            Err(e) => {
                tracing::error!("Login failed: {e:?}");
                callback(false, ptr::null(), ptr::null(), ptr::null(), ud.as_ptr());
            }
        }
    });
}

/// Get the room list synchronously (blocks until complete).
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - Caller must call `tm_free_rooms` on the returned FfiRoomList.
#[no_mangle]
pub unsafe extern "C" fn tm_get_rooms(h: *mut Handle) -> FfiRoomList {
    let handle = unsafe { &*h };
    let client = handle.client.clone();

    match handle.runtime.block_on(async { client.get_rooms().await }) {
        Ok(rooms) => rooms_to_ffi_raw(&rooms),
        Err(_) => empty_room_list(),
    }
}

/// Get the current room list asynchronously.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - Caller must call `tm_free_rooms` on the FfiRoomList delivered to the callback.
#[no_mangle]
pub unsafe extern "C" fn tm_get_rooms_async(
    h: *mut Handle,
    callback: TmRoomListCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client.get_rooms().await {
            Ok(rooms) => callback(true, rooms_to_ffi_raw(&rooms), ud.as_ptr()),
            Err(_) => callback(false, empty_room_list(), ud.as_ptr()),
        }
    });
}

/// Free a room list returned by `tm_get_rooms`.
///
/// # Safety
/// `list` must have been returned by `tm_get_rooms` and must not be used after this call.
#[no_mangle]
pub unsafe extern "C" fn tm_free_rooms(list: FfiRoomList) {
    if list.rooms.is_null() || list.len == 0 {
        return;
    }
    let rooms = unsafe { reclaim_boxed_slice(list.rooms, list.len) };
    for room in rooms {
        unsafe {
            free_c_string(room.room_id);
            free_c_string(room.display_name);
            free_c_string(room.canonical_alias);
            free_c_string(room.avatar_url);
            free_c_string(room.avatar_entity_id);
            free_c_string(room.last_event_text);
            free_c_string(room.last_event_sender);
            free_c_string(room.inviter_user_id);
            free_c_string(room.inviter_display_name);
            free_c_string(room.inviter_avatar_url);
            free_c_string(room.room_topic);
            if !room.filter_ids.is_null() && room.filter_ids_len > 0 {
                drop(reclaim_boxed_slice(room.filter_ids, room.filter_ids_len));
            }
            if !room.space_ids.is_null() && room.space_ids_len > 0 {
                for s in reclaim_boxed_slice(room.space_ids, room.space_ids_len) {
                    free_c_string(s);
                }
            }
        }
    }
}

/// Free a timeline returned by `tm_get_timeline`.
///
/// # Safety
/// `tl` must have been returned by `tm_get_timeline` and must not be used after this call.
#[no_mangle]
pub unsafe extern "C" fn tm_free_timeline(tl: FfiTimeline) {
    if tl.items.is_null() || tl.len == 0 {
        return;
    }
    let items = unsafe { reclaim_boxed_slice(tl.items, tl.len) };
    for item in items {
        unsafe {
            free_c_string(item.event_id);
            free_c_string(item.transaction_id);
            free_c_string(item.sender_user_id);
            free_c_string(item.sender_display_name);
            free_c_string(item.sender_avatar_url);
            free_c_string(item.body);
            free_c_string(item.formatted_body);
            free_c_string(item.media_url);
            free_c_string(item.media_mime);
            free_c_string(item.media_filename);
            free_c_string(item.media_caption);
            free_c_string(item.media_thumb_url);
            free_c_string(item.media_blurhash);
            free_c_string(item.reply_to_event_id);
            free_c_string(item.reply_preview_sender_name);
            free_c_string(item.reply_preview_text);
            free_c_string(item.reply_preview_thumb_url);
            free_c_string(item.forwarded_from_sender_name);
            free_c_string(item.forwarded_from_sender_id);
            free_c_string(item.forwarded_from_avatar_url);
            free_c_string(item.reactions);
            free_c_string(item.url_preview_url);
            free_c_string(item.url_preview_site_name);
            free_c_string(item.url_preview_title);
            free_c_string(item.url_preview_description);
            free_c_string(item.url_preview_image_url);
            free_c_string(item.url_preview_author);
            free_c_string(item.url_preview_site_name_canonical);
            free_c_string(item.decryption_error);
            free_c_string(item.audio_waveform_json);
            free_c_string(item.poll_question);
            free_c_string(item.poll_subtitle);
            free_c_string(item.poll_options_json);
        }
    }
}

/// Asynchronously send a message to a room.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `room_id` and `body` must be valid null-terminated C strings.
/// - `formatted_body` may be null (plain text) or a valid null-terminated C string (HTML).
/// - `callback` will be invoked on a background thread when send completes.
/// - `userdata` is passed through to the callback unchanged.
#[no_mangle]
pub unsafe extern "C" fn tm_send_message(
    h: *mut Handle,
    room_id: *const c_char,
    body: *const c_char,
    formatted_body: *const c_char,
    reply_to_event_id: *const c_char,
    callback: TmSendCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let body_str = c_to_string(body);
    let formatted_body = opt_c_to_string(formatted_body);
    let reply_to_event_id = opt_c_to_string(reply_to_event_id);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client
            .send_message(
                &room_id_str,
                &body_str,
                formatted_body.as_deref(),
                reply_to_event_id.as_deref(),
            )
            .await
        {
            Ok(event_id) => {
                let event_id = CString::new(event_id).unwrap_or_default();
                callback(true, event_id.as_ptr(), ud.as_ptr());
            }
            Err(_) => callback(false, ptr::null(), ud.as_ptr()),
        }
    });
}

#[no_mangle]
pub unsafe extern "C" fn tm_edit_message(
    h: *mut Handle,
    room_id: *const c_char,
    event_id: *const c_char,
    body: *const c_char,
    formatted_body: *const c_char,
    as_media_caption: bool,
    callback: TmSendCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let event_id_str = c_to_string(event_id);
    let body_str = c_to_string(body);
    let formatted_body = opt_c_to_string(formatted_body);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client
            .edit_message(
                &room_id_str,
                &event_id_str,
                &body_str,
                formatted_body.as_deref(),
                as_media_caption,
            )
            .await
        {
            Ok(new_event_id) => {
                let event_id = CString::new(new_event_id).unwrap_or_default();
                callback(true, event_id.as_ptr(), ud.as_ptr());
            }
            Err(_) => callback(false, ptr::null(), ud.as_ptr()),
        }
    });
}

#[no_mangle]
pub unsafe extern "C" fn tm_delete_message(
    h: *mut Handle,
    room_id: *const c_char,
    event_id: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let event_id_str = c_to_string(event_id);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let success = client
            .delete_message(&room_id_str, &event_id_str)
            .await
            .is_ok();
        callback(success, ud.as_ptr());
    });
}

#[no_mangle]
pub unsafe extern "C" fn tm_pin_message(
    h: *mut Handle,
    room_id: *const c_char,
    event_id: *const c_char,
    pinned: bool,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let event_id_str = c_to_string(event_id);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let result = client
            .pin_message(&room_id_str, &event_id_str, pinned)
            .await;
        if let Err(ref e) = result {
            warn!("[PIN] pin_message FAILED for {event_id_str} in {room_id_str}: {e:#}");
        }
        callback(result.is_ok(), ud.as_ptr());
    });
}

#[no_mangle]
pub unsafe extern "C" fn tm_pin_room(
    h: *mut Handle,
    room_id: *const c_char,
    pinned: bool,
    order: f64,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);
    // Negative means "no position to record" — the tag is written without an order.
    let order = (order >= 0.0).then_some(order);

    handle.runtime.spawn(async move {
        let result = client.pin_room(&room_id_str, pinned, order).await;
        if let Err(ref e) = result {
            warn!("[PIN_ROOM] pin_room FAILED for {room_id_str}: {e:#}");
        }
        callback(result.is_ok(), ud.as_ptr());
    });
}

/// Record the pinned rooms' order (top first) in their `m.favourite` tags, so the
/// arrangement survives a re-login and reaches the user's other devices.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `room_ids` must point to `count` valid C strings.
#[no_mangle]
pub unsafe extern "C" fn tm_set_pinned_order(
    h: *mut Handle,
    room_ids: *const *const c_char,
    count: usize,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ids: Vec<String> = if room_ids.is_null() {
        Vec::new()
    } else {
        (0..count)
            .map(|i| c_to_string(unsafe { *room_ids.add(i) }))
            .collect()
    };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let result = client.set_pinned_order(ids).await;
        if let Err(ref e) = result {
            warn!("[PIN_ROOM] set_pinned_order FAILED: {e:#}");
        }
        callback(result.is_ok(), ud.as_ptr());
    });
}

#[no_mangle]
pub unsafe extern "C" fn tm_set_room_notification_mode(
    h: *mut Handle,
    room_id: *const c_char,
    mode: u32,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    let mode = match mode {
        0 => crate::types::RoomNotificationMode::AllMessages,
        1 => crate::types::RoomNotificationMode::MentionsOnly,
        2 => crate::types::RoomNotificationMode::Mute,
        _ => {
            callback(false, ud.as_ptr());
            return;
        }
    };

    handle.runtime.spawn(async move {
        let result = client.set_room_notification_mode(&room_id_str, mode).await;
        if let Err(ref e) = result {
            warn!("[tm_set_room_notification_mode] error: {e:?}");
        }
        callback(result.is_ok(), ud.as_ptr());
    });
}

/// Callback for `tm_get_notification_settings`. `keywords_csv` is a ", "-joined
/// list of the user's keyword rules (empty when there are none / on failure).
/// `dm_level` / `room_level`: 0 = AllMessages, 1 = MentionsOnly. The four bools are
/// the "Mentions & keywords" master toggles.
pub type TmNotificationSettingsCallback = extern "C" fn(
    success: bool,
    dm_level: u32,
    room_level: u32,
    mention_display_name: bool,
    mention_username: bool,
    mention_room: bool,
    keywords_enabled: bool,
    keywords_csv: *const c_char,
    userdata: *mut libc::c_void,
);

/// Read the account-global "Notifications for chats" settings (DM/room default
/// levels + keyword rules).
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `callback` is invoked once on a background thread; pointers are valid only
///   for that call.
/// - `userdata` is passed through unchanged.
#[no_mangle]
pub unsafe extern "C" fn tm_get_notification_settings(
    h: *mut Handle,
    callback: TmNotificationSettingsCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);
    handle.runtime.spawn(async move {
        match client.get_notification_settings().await {
            Ok(s) => {
                let csv = CString::new(s.keywords.join(", ")).unwrap_or_default();
                callback(
                    true,
                    s.dm_level as u32,
                    s.room_level as u32,
                    s.mention_display_name,
                    s.mention_username,
                    s.mention_room,
                    s.keywords_enabled,
                    csv.as_ptr(),
                    ud.as_ptr(),
                );
            }
            Err(e) => {
                warn!("[tm_get_notification_settings] error: {e:?}");
                let empty = CString::new("").unwrap_or_default();
                callback(
                    false,
                    0,
                    0,
                    false,
                    false,
                    false,
                    false,
                    empty.as_ptr(),
                    ud.as_ptr(),
                );
            }
        }
    });
}

/// Set a chat category's default notification level.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `callback` is invoked once on a background thread.
/// - `userdata` is passed through unchanged.
#[no_mangle]
pub unsafe extern "C" fn tm_set_category_notification_level(
    h: *mut Handle,
    category: u32,
    level: u32,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);
    let category = match category {
        0 => crate::notification_settings_service::ChatCategory::Dm,
        1 => crate::notification_settings_service::ChatCategory::Room,
        _ => {
            callback(false, ud.as_ptr());
            return;
        }
    };
    let level = match level {
        0 => crate::types::RoomNotificationMode::AllMessages,
        1 => crate::types::RoomNotificationMode::MentionsOnly,
        _ => {
            callback(false, ud.as_ptr());
            return;
        }
    };
    handle.runtime.spawn(async move {
        let result = client
            .set_category_notification_level(category, level)
            .await;
        if let Err(ref e) = result {
            warn!("[tm_set_category_notification_level] error: {e:?}");
        }
        callback(result.is_ok(), ud.as_ptr());
    });
}

/// Reconcile the user's keyword rules to `keywords_csv` (comma-separated).
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `keywords_csv` must be a valid NUL-terminated C string.
/// - `callback` is invoked once on a background thread.
/// - `userdata` is passed through unchanged.
#[no_mangle]
pub unsafe extern "C" fn tm_set_keywords(
    h: *mut Handle,
    keywords_csv: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let csv = c_to_string(keywords_csv);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);
    handle.runtime.spawn(async move {
        let result = client.set_keywords_setting(&csv).await;
        if let Err(ref e) = result {
            warn!("[tm_set_keywords] error: {e:?}");
        }
        callback(result.is_ok(), ud.as_ptr());
    });
}

/// Toggle a "Mentions & keywords" master switch.
/// `toggle`: 0 = display name, 1 = username, 2 = @room, 3 = keywords.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `callback` is invoked once on a background thread.
/// - `userdata` is passed through unchanged.
#[no_mangle]
pub unsafe extern "C" fn tm_set_notification_toggle(
    h: *mut Handle,
    toggle: u32,
    enabled: bool,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);
    let toggle = match toggle {
        0 => crate::notification_settings_service::NotificationToggle::DisplayName,
        1 => crate::notification_settings_service::NotificationToggle::Username,
        2 => crate::notification_settings_service::NotificationToggle::Room,
        3 => crate::notification_settings_service::NotificationToggle::Keywords,
        _ => {
            callback(false, ud.as_ptr());
            return;
        }
    };
    handle.runtime.spawn(async move {
        let result = client.set_notification_toggle(toggle, enabled).await;
        if let Err(ref e) = result {
            warn!("[tm_set_notification_toggle] error: {e:?}");
        }
        callback(result.is_ok(), ud.as_ptr());
    });
}

#[no_mangle]
pub unsafe extern "C" fn tm_mark_room_read(
    h: *mut Handle,
    room_id: *const c_char,
    read: bool,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let success = client.mark_room_read(&room_id_str, read).await.is_ok();
        callback(success, ud.as_ptr());
    });
}

#[no_mangle]
pub unsafe extern "C" fn tm_send_read_receipt(
    h: *mut Handle,
    room_id: *const c_char,
    event_id: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let event_id_str = c_to_string(event_id);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let success = client
            .send_read_receipt(&room_id_str, &event_id_str)
            .await
            .is_ok();
        callback(success, ud.as_ptr());
    });
}

#[no_mangle]
pub unsafe extern "C" fn tm_leave_room(
    h: *mut Handle,
    room_id: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let success = client.leave_room(&room_id_str).await.is_ok();
        callback(success, ud.as_ptr());
    });
}

/// Drop a no-longer-viewed room's resident timeline state (window + per-room
/// caches) to bound memory across a long multi-room session. Fire-and-forget;
/// rooms-list state and notifications are unaffected, and re-opening the room
/// rebuilds its timeline.
#[no_mangle]
pub unsafe extern "C" fn tm_release_room_timeline(h: *mut Handle, room_id: *const c_char) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let client = handle.client.clone();

    handle.runtime.spawn(async move {
        client.release_room_timeline(&room_id_str).await;
    });
}

#[no_mangle]
pub unsafe extern "C" fn tm_accept_invite(
    h: *mut Handle,
    room_id: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let success = client.accept_invite(&room_id_str).await.is_ok();
        callback(success, ud.as_ptr());
    });
}

#[no_mangle]
pub unsafe extern "C" fn tm_add_room_to_folder(
    h: *mut Handle,
    room_id: *const c_char,
    tag_key: *const c_char,
    callback: TmResultCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let tag_key_str = c_to_string(tag_key);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let result = client.add_room_to_folder(&room_id_str, &tag_key_str).await;
        deliver_result(callback, result, ud);
    });
}

/// Callback for folder creation: returns the new runtime handle AND tag key
/// (both valid only on success), plus the error text on failure. The C++ UI
/// keys optimistic updates on the handle before the folder list re-fetch, and
/// cannot derive it from the key locally, so both are returned.
pub type TmCreateFolderCallback = extern "C" fn(
    success: bool,
    folder_id: i32,
    tag_key: *mut c_char,
    error: *mut c_char,
    userdata: *mut libc::c_void,
);
pub type TmFolderListCallback =
    extern "C" fn(success: bool, list: FfiFolderList, userdata: *mut libc::c_void);
pub type TmSidebarOrderCallback =
    extern "C" fn(success: bool, list: FfiSidebarOrder, userdata: *mut libc::c_void);

/// C-compatible folder metadata. `id` is the per-session runtime handle; the
/// durable identity is `tag_key` (`u.<name>`).
#[repr(C)]
pub struct FfiFolderMeta {
    pub id: i32,
    pub tag_key: *mut c_char,
    pub name: *mut c_char,
}

/// C-compatible folder list.
#[repr(C)]
pub struct FfiFolderList {
    pub folders: *mut FfiFolderMeta,
    pub len: usize,
}

/// One entry in the unified sidebar order. `kind`: 0 = folder (key is the tag
/// key), 1 = space (key is the space room id).
#[repr(C)]
pub struct FfiSidebarRef {
    pub kind: u32,
    pub key: *mut c_char,
}

#[repr(C)]
pub struct FfiSidebarOrder {
    pub refs: *mut FfiSidebarRef,
    pub len: usize,
}

fn folder_list_to_ffi(folders: &[FolderMeta]) -> FfiFolderList {
    // Return a null pointer for empty lists (like rooms_to_ffi_raw): leaking a
    // zero-length boxed slice would give a non-null dangling pointer that
    // tm_free_folders skips on `len == 0`, leaking it.
    if folders.is_empty() {
        return FfiFolderList {
            folders: ptr::null_mut(),
            len: 0,
        };
    }
    let ffi_folders: Vec<FfiFolderMeta> = folders
        .iter()
        .map(|f| FfiFolderMeta {
            id: f.id,
            tag_key: str_to_c(&f.tag_key),
            name: str_to_c(&f.name),
        })
        .collect();
    let len = ffi_folders.len();
    let ptr = leak_boxed_slice(ffi_folders);
    FfiFolderList { folders: ptr, len }
}

fn sidebar_order_to_ffi(order: &[crate::room_folders::SidebarRef]) -> FfiSidebarOrder {
    if order.is_empty() {
        return FfiSidebarOrder {
            refs: ptr::null_mut(),
            len: 0,
        };
    }
    let ffi_refs: Vec<FfiSidebarRef> = order
        .iter()
        .map(|r| FfiSidebarRef {
            kind: match r.kind {
                crate::room_folders::SidebarKind::Folder => 0,
                crate::room_folders::SidebarKind::Space => 1,
            },
            key: str_to_c(&r.key),
        })
        .collect();
    let len = ffi_refs.len();
    let ptr = leak_boxed_slice(ffi_refs);
    FfiSidebarOrder { refs: ptr, len }
}

#[no_mangle]
pub unsafe extern "C" fn tm_create_folder(
    h: *mut Handle,
    name: *const c_char,
    callback: TmCreateFolderCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let name_str = c_to_string(name);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client.create_folder(&name_str).await {
            Ok(tag_key) => {
                let handle = crate::room_folders::section_handle(&tag_key);
                callback(
                    true,
                    handle,
                    str_to_c(&tag_key),
                    ptr::null_mut(),
                    ud.as_ptr(),
                );
            }
            Err(e) => callback(
                false,
                -1,
                ptr::null_mut(),
                str_to_c(&e.to_string()),
                ud.as_ptr(),
            ),
        }
    });
}

/// Rename a folder (updates its name in Element's section settings; the tag is a
/// stable UUID, so no room re-tagging). The C++ side re-fetches the folder list.
#[no_mangle]
pub unsafe extern "C" fn tm_edit_folder(
    h: *mut Handle,
    tag_key: *const c_char,
    name: *const c_char,
    callback: TmResultCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let tag_key_str = c_to_string(tag_key);
    let name_str = c_to_string(name);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let result = client
            .edit_folder(&tag_key_str, &name_str)
            .await
            .map(|_| ());
        deliver_result(callback, result, ud);
    });
}

#[no_mangle]
pub unsafe extern "C" fn tm_delete_folder(
    h: *mut Handle,
    tag_key: *const c_char,
    callback: TmResultCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let tag_key_str = c_to_string(tag_key);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let result = client.delete_folder(&tag_key_str).await;
        deliver_result(callback, result, ud);
    });
}

/// Replace the unified sidebar order (folders + spaces).
#[no_mangle]
pub unsafe extern "C" fn tm_set_sidebar_order(
    h: *mut Handle,
    refs: *const FfiSidebarRef,
    refs_len: usize,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let order: Vec<crate::room_folders::SidebarRef> = if refs.is_null() || refs_len == 0 {
        Vec::new()
    } else {
        (0..refs_len)
            .map(|i| {
                let r = unsafe { &*refs.add(i) };
                crate::room_folders::SidebarRef {
                    kind: if r.kind == 1 {
                        crate::room_folders::SidebarKind::Space
                    } else {
                        crate::room_folders::SidebarKind::Folder
                    },
                    key: c_to_string(r.key),
                }
            })
            .collect()
    };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let success = client.set_sidebar_order(order).await.is_ok();
        callback(success, ud.as_ptr());
    });
}

/// Fetch the unified sidebar order (folders + spaces).
#[no_mangle]
pub unsafe extern "C" fn tm_get_sidebar_order_async(
    h: *mut Handle,
    callback: TmSidebarOrderCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client.get_sidebar_order().await {
            Ok(order) => callback(true, sidebar_order_to_ffi(&order), ud.as_ptr()),
            Err(_) => callback(
                false,
                FfiSidebarOrder {
                    refs: ptr::null_mut(),
                    len: 0,
                },
                ud.as_ptr(),
            ),
        }
    });
}

/// Free a sidebar-order list returned by `tm_get_sidebar_order_async`.
#[no_mangle]
pub unsafe extern "C" fn tm_free_sidebar_order(list: FfiSidebarOrder) {
    if list.refs.is_null() || list.len == 0 {
        return;
    }
    let refs = unsafe { reclaim_boxed_slice(list.refs, list.len) };
    for r in refs {
        unsafe {
            free_c_string(r.key);
        }
    }
}

/// C-compatible joined-space entry.
#[repr(C)]
pub struct FfiSpaceInfo {
    pub room_id: *mut c_char,
    pub name: *mut c_char,
    pub avatar_url: *mut c_char, // null if none
    pub topic: *mut c_char,
    pub member_count: u64,
    pub canonical_alias: *mut c_char, // null if none
}

#[repr(C)]
pub struct FfiSpaceList {
    pub spaces: *mut FfiSpaceInfo,
    pub len: usize,
}

pub type TmSpaceListCallback =
    extern "C" fn(success: bool, list: FfiSpaceList, userdata: *mut libc::c_void);

fn space_list_to_ffi(spaces: &[crate::types::SpaceInfo]) -> FfiSpaceList {
    if spaces.is_empty() {
        return FfiSpaceList {
            spaces: ptr::null_mut(),
            len: 0,
        };
    }
    let ffi_spaces: Vec<FfiSpaceInfo> = spaces
        .iter()
        .map(|s| FfiSpaceInfo {
            room_id: str_to_c(&s.room_id),
            name: str_to_c(&s.name),
            avatar_url: s
                .avatar_url
                .as_deref()
                .map(str_to_c)
                .unwrap_or(ptr::null_mut()),
            topic: str_to_c(&s.topic),
            member_count: s.member_count,
            canonical_alias: s
                .canonical_alias
                .as_deref()
                .map(str_to_c)
                .unwrap_or(ptr::null_mut()),
        })
        .collect();
    let len = ffi_spaces.len();
    let ptr = leak_boxed_slice(ffi_spaces);
    FfiSpaceList { spaces: ptr, len }
}

/// Fetch the joined Matrix spaces (folder-like tabs).
#[no_mangle]
pub unsafe extern "C" fn tm_get_joined_spaces_async(
    h: *mut Handle,
    callback: TmSpaceListCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client.get_joined_spaces().await {
            Ok(spaces) => callback(true, space_list_to_ffi(&spaces), ud.as_ptr()),
            Err(_) => callback(
                false,
                FfiSpaceList {
                    spaces: ptr::null_mut(),
                    len: 0,
                },
                ud.as_ptr(),
            ),
        }
    });
}

#[no_mangle]
pub unsafe extern "C" fn tm_free_spaces(list: FfiSpaceList) {
    if list.spaces.is_null() || list.len == 0 {
        return;
    }
    let spaces = unsafe { reclaim_boxed_slice(list.spaces, list.len) };
    for s in spaces {
        unsafe {
            free_c_string(s.room_id);
            free_c_string(s.name);
            if !s.avatar_url.is_null() {
                free_c_string(s.avatar_url);
            }
            free_c_string(s.topic);
            if !s.canonical_alias.is_null() {
                free_c_string(s.canonical_alias);
            }
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn tm_get_folders(h: *mut Handle) -> FfiFolderList {
    let handle = unsafe { &*h };
    let client = handle.client.clone();

    match handle
        .runtime
        .block_on(async { client.get_folders().await })
    {
        Ok(folders) => folder_list_to_ffi(&folders),
        Err(_) => FfiFolderList {
            folders: ptr::null_mut(),
            len: 0,
        },
    }
}

#[no_mangle]
pub unsafe extern "C" fn tm_get_folders_async(
    h: *mut Handle,
    callback: TmFolderListCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client.get_folders().await {
            Ok(folders) => callback(true, folder_list_to_ffi(&folders), ud.as_ptr()),
            Err(_) => callback(
                false,
                FfiFolderList {
                    folders: ptr::null_mut(),
                    len: 0,
                },
                ud.as_ptr(),
            ),
        }
    });
}

#[no_mangle]
pub unsafe extern "C" fn tm_free_folders(list: FfiFolderList) {
    if list.folders.is_null() || list.len == 0 {
        return;
    }
    let folders = unsafe { reclaim_boxed_slice(list.folders, list.len) };
    for f in folders {
        unsafe {
            free_c_string(f.tag_key);
            free_c_string(f.name);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn tm_forward_message(
    h: *mut Handle,
    src_room_id: *const c_char,
    event_id: *const c_char,
    dst_room_id: *const c_char,
    callback: TmSendCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let src_room_id_str = c_to_string(src_room_id);
    let event_id_str = c_to_string(event_id);
    let dst_room_id_str = c_to_string(dst_room_id);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client
            .forward_message(&src_room_id_str, &event_id_str, &dst_room_id_str)
            .await
        {
            Ok(new_event_id) => {
                let event_id = CString::new(new_event_id).unwrap_or_default();
                callback(true, event_id.as_ptr(), ud.as_ptr());
            }
            Err(_) => callback(false, ptr::null(), ud.as_ptr()),
        }
    });
}

#[no_mangle]
pub unsafe extern "C" fn tm_send_media(
    h: *mut Handle,
    room_id: *const c_char,
    content_type: u32,
    url: *const c_char,
    mime: *const c_char,
    filename: *const c_char,
    caption: *const c_char,
    thumb_url: *const c_char,
    size: u64,
    width: u32,
    height: u32,
    duration_ms: u64,
    transaction_id: *const c_char,
    is_voice: bool,
    waveform: *const u8,
    waveform_len: usize,
    callback: TmSendCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let content_type = match content_type_from_ffi(content_type) {
        Some(value) => value,
        None => {
            callback(false, ptr::null(), userdata);
            return;
        }
    };

    let url = c_to_string(url);
    let mime = c_to_string(mime);
    let filename = c_to_string(filename);
    let caption = opt_c_to_string(caption);
    let thumb_url = opt_c_to_string(thumb_url);
    let transaction_id = opt_c_to_string(transaction_id);
    let waveform = if !waveform.is_null() && waveform_len > 0 {
        unsafe { slice::from_raw_parts(waveform, waveform_len) }.to_vec()
    } else {
        Vec::new()
    };

    let content = match content_type {
        ContentType::Image => MessageContent::Image {
            url,
            mime_type: mime,
            filename,
            caption,
            thumbnail_url: thumb_url,
            blurhash: None,
            size,
            width,
            height,
        },
        ContentType::File => MessageContent::File {
            url,
            mime_type: mime,
            filename,
            caption,
            size,
            duration_ms,
        },
        ContentType::Video => MessageContent::Video {
            url,
            mime_type: mime,
            filename,
            caption,
            thumbnail_url: thumb_url,
            blurhash: None,
            size,
            width,
            height,
            duration_ms,
        },
        ContentType::Audio => MessageContent::Audio {
            info: AudioInfo {
                url,
                mime_type: mime,
                filename,
                size,
                duration_ms,
                is_voice,
                waveform,
            },
        },
        ContentType::Text => {
            callback(false, ptr::null(), userdata);
            return;
        }
        ContentType::Service => {
            callback(false, ptr::null(), userdata);
            return;
        }
        ContentType::Poll => {
            callback(false, ptr::null(), userdata);
            return;
        }
        ContentType::UnableToDecrypt => {
            callback(false, ptr::null(), userdata);
            return;
        }
    };

    let client = handle.client.clone();
    let ud = Userdata::new(userdata);
    let txn_for_registry = transaction_id.clone().unwrap_or_default();
    let txn_for_cleanup = txn_for_registry.clone();

    let join = handle.runtime.spawn(async move {
        let callback_id = transaction_id.clone().unwrap_or_default();
        let result = client
            .send_media(&room_id_str, content, transaction_id)
            .await;
        if !txn_for_cleanup.is_empty() {
            crate::upload_tasks::remove(&txn_for_cleanup);
        }
        match result {
            Ok(event_id) => {
                let event_id = CString::new(event_id).unwrap_or_default();
                callback(true, event_id.as_ptr(), ud.as_ptr());
            }
            Err(e) => {
                tracing::error!("send_media failed for {callback_id}: {e:?}");
                let event_id = CString::new(callback_id).unwrap_or_default();
                callback(false, event_id.as_ptr(), ud.as_ptr());
            }
        }
    });

    // Track the task so a cancel can abort an in-flight direct upload at any
    // point. (Voice still uses the send queue and enqueues quickly, so its task
    // finishes fast; cancel then falls through to the send-queue/redact path.)
    if !txn_for_registry.is_empty() {
        crate::upload_tasks::register(txn_for_registry.clone(), join.abort_handle());
        if join.is_finished() {
            crate::upload_tasks::remove(&txn_for_registry);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn tm_set_reaction(
    h: *mut Handle,
    room_id: *const c_char,
    event_id: *const c_char,
    key: *const c_char,
    active: bool,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let event_id_str = c_to_string(event_id);
    let key_str = c_to_string(key);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let success = client
            .set_reaction(&room_id_str, &event_id_str, &key_str, active)
            .await
            .is_ok();
        callback(success, ud.as_ptr());
    });
}

#[no_mangle]
pub unsafe extern "C" fn tm_send_poll_vote(
    h: *mut Handle,
    room_id: *const c_char,
    poll_event_id: *const c_char,
    option_ids_json: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let poll_event_id_str = c_to_string(poll_event_id);
    let option_ids_str = c_to_string(option_ids_json);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    let option_ids: Vec<String> = serde_json::from_str(&option_ids_str).unwrap_or_default();

    handle.runtime.spawn(async move {
        let success = client
            .send_poll_vote(&room_id_str, &poll_event_id_str, option_ids)
            .await
            .is_ok();
        callback(success, ud.as_ptr());
    });
}

/// C-compatible member list.
#[repr(C)]
pub struct FfiMemberList {
    pub members: *mut FfiUserProfile,
    pub len: usize,
}

pub type TmMemberListCallback =
    extern "C" fn(success: bool, list: FfiMemberList, userdata: *mut libc::c_void);

/// C-compatible user directory search results. `limited` is true when the
/// homeserver capped the results and more matches exist (the directory endpoint
/// has no offset pagination).
#[repr(C)]
pub struct FfiUserDirectoryResults {
    pub members: *mut FfiUserProfile,
    pub len: usize,
    pub limited: bool,
}

pub type TmUserDirectorySearchCallback =
    extern "C" fn(success: bool, results: FfiUserDirectoryResults, userdata: *mut libc::c_void);

pub type TmRoomMembersSnapshotCallback =
    extern "C" fn(success: bool, snapshot: FfiRoomMembersSnapshot, userdata: *mut libc::c_void);

pub type TmUserProfileDetailsCallback =
    extern "C" fn(success: bool, details: FfiUserProfileDetails, userdata: *mut libc::c_void);

/// Get room members asynchronously via callback.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `room_id` must be a valid null-terminated C string.
/// - Caller must call `tm_free_room_members` on the FfiMemberList delivered to the callback.
#[no_mangle]
pub unsafe extern "C" fn tm_get_room_members_async(
    h: *mut Handle,
    room_id: *const c_char,
    callback: TmMemberListCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let ud = Userdata::new(userdata);
    let client = handle.client.clone();

    handle.runtime.spawn(async move {
        match client.get_room_members(&room_id_str).await {
            Ok(members) => {
                let ffi_members: Vec<FfiUserProfile> = members
                    .iter()
                    .map(|m| FfiUserProfile {
                        user_id: str_to_c(&m.user_id),
                        display_name: str_to_c(&m.display_name),
                        avatar_url: opt_str_to_c(&m.avatar_url),
                    })
                    .collect();
                let len = ffi_members.len();
                let ptr = leak_boxed_slice(ffi_members);
                callback(true, FfiMemberList { members: ptr, len }, ud.as_ptr());
            }
            Err(_) => callback(
                false,
                FfiMemberList {
                    members: ptr::null_mut(),
                    len: 0,
                },
                ud.as_ptr(),
            ),
        }
    });
}

/// Search the homeserver user directory asynchronously.
///
/// # Safety
/// `h` and `query` must be valid.
#[no_mangle]
pub unsafe extern "C" fn tm_search_user_directory(
    h: *mut Handle,
    query: *const c_char,
    limit: u64,
    callback: TmUserDirectorySearchCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let query_str = c_to_string(query);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client.search_user_directory(&query_str, limit).await {
            Ok((members, limited)) => {
                let ffi_members: Vec<FfiUserProfile> = members
                    .iter()
                    .map(|m| FfiUserProfile {
                        user_id: str_to_c(&m.user_id),
                        display_name: str_to_c(&m.display_name),
                        avatar_url: opt_str_to_c(&m.avatar_url),
                    })
                    .collect();
                let len = ffi_members.len();
                let ptr = leak_boxed_slice(ffi_members);
                callback(
                    true,
                    FfiUserDirectoryResults {
                        members: ptr,
                        len,
                        limited,
                    },
                    ud.as_ptr(),
                );
            }
            Err(_) => callback(
                false,
                FfiUserDirectoryResults {
                    members: ptr::null_mut(),
                    len: 0,
                    limited: false,
                },
                ud.as_ptr(),
            ),
        }
    });
}

/// Free a member list returned by `tm_get_room_members`.
///
/// # Safety
/// `list` must have been returned by `tm_get_room_members` and must not be used after this call.
#[no_mangle]
pub unsafe extern "C" fn tm_free_room_members(list: FfiMemberList) {
    if list.members.is_null() || list.len == 0 {
        return;
    }
    let members = unsafe { reclaim_boxed_slice(list.members, list.len) };
    for m in members {
        unsafe {
            free_c_string(m.user_id);
            free_c_string(m.display_name);
            free_c_string(m.avatar_url);
        }
    }
}

/// Free user directory search results delivered to `tm_search_user_directory`.
///
/// # Safety
/// `results` must have been delivered by `tm_search_user_directory` and must not be used after this call.
#[no_mangle]
pub unsafe extern "C" fn tm_free_user_directory_results(results: FfiUserDirectoryResults) {
    if results.members.is_null() || results.len == 0 {
        return;
    }
    let members = unsafe { reclaim_boxed_slice(results.members, results.len) };
    for m in members {
        unsafe {
            free_c_string(m.user_id);
            free_c_string(m.display_name);
            free_c_string(m.avatar_url);
        }
    }
}

// --- Room Members Snapshot FFI ---

/// C-compatible room member info.
#[repr(C)]
pub struct FfiRoomMemberInfo {
    pub user_id: *mut c_char,
    pub display_name: *mut c_char,
    pub avatar_url: *mut c_char,
    pub membership: u32,
    pub power_level: i64,
    pub role: u32,
    pub is_self: bool,
    pub can_be_removed_by_me: bool,
    pub can_be_banned_by_me: bool,
    pub can_be_unbanned_by_me: bool,
}

/// C-compatible room members snapshot.
#[repr(C)]
pub struct FfiRoomMembersSnapshot {
    pub room_id: *mut c_char,
    pub my_user_id: *mut c_char,
    pub can_invite: bool,
    pub can_remove_any: bool,
    pub members: *mut FfiRoomMemberInfo,
    pub members_len: usize,
}

fn ffi_room_members_snapshot_from_snapshot(
    snap: &crate::types::RoomMembersSnapshot,
) -> FfiRoomMembersSnapshot {
    let ffi_members: Vec<FfiRoomMemberInfo> = snap
        .members
        .iter()
        .map(|m| FfiRoomMemberInfo {
            user_id: str_to_c(&m.user_id),
            display_name: str_to_c(&m.display_name),
            avatar_url: opt_str_to_c(&m.avatar_url),
            membership: m.membership as u32,
            power_level: m.power_level,
            role: m.role as u32,
            is_self: m.is_self,
            can_be_removed_by_me: m.can_be_removed_by_me,
            can_be_banned_by_me: m.can_be_banned_by_me,
            can_be_unbanned_by_me: m.can_be_unbanned_by_me,
        })
        .collect();
    let members_len = ffi_members.len();
    let members_ptr = leak_boxed_slice(ffi_members);

    FfiRoomMembersSnapshot {
        room_id: str_to_c(&snap.room_id),
        my_user_id: str_to_c(&snap.my_user_id),
        can_invite: snap.can_invite,
        can_remove_any: snap.can_remove_any,
        members: members_ptr,
        members_len,
    }
}

fn empty_room_members_snapshot() -> FfiRoomMembersSnapshot {
    FfiRoomMembersSnapshot {
        room_id: ptr::null_mut(),
        my_user_id: ptr::null_mut(),
        can_invite: false,
        can_remove_any: false,
        members: ptr::null_mut(),
        members_len: 0,
    }
}

/// Get a detailed member snapshot with permission flags asynchronously.
///
/// # Safety
/// - `h` and `room_id` must be valid pointers.
/// - Caller must call `tm_free_room_members_snapshot` on the value delivered to the callback.
#[no_mangle]
pub unsafe extern "C" fn tm_get_room_members_snapshot_async(
    h: *mut Handle,
    room_id: *const c_char,
    force_refresh: bool,
    callback: TmRoomMembersSnapshotCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client
            .get_room_members_snapshot(&room_id_str, force_refresh)
            .await
        {
            Ok(snap) => callback(
                true,
                ffi_room_members_snapshot_from_snapshot(&snap),
                ud.as_ptr(),
            ),
            Err(_) => callback(false, empty_room_members_snapshot(), ud.as_ptr()),
        }
    });
}

/// Free a room members snapshot.
///
/// # Safety
/// `snap` must have been returned by `tm_get_room_members_snapshot`.
#[no_mangle]
pub unsafe extern "C" fn tm_free_room_members_snapshot(snap: FfiRoomMembersSnapshot) {
    unsafe {
        free_c_string(snap.room_id);
        free_c_string(snap.my_user_id);
    }
    if !snap.members.is_null() && snap.members_len > 0 {
        let members = unsafe { reclaim_boxed_slice(snap.members, snap.members_len) };
        for m in members {
            unsafe {
                free_c_string(m.user_id);
                free_c_string(m.display_name);
                free_c_string(m.avatar_url);
            }
        }
    }
}

/// Register a callback that fires when the room list changes.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `callback` will be invoked on a background thread.
/// - `userdata` is passed through to the callback unchanged.
#[no_mangle]
pub unsafe extern "C" fn tm_set_room_change_callback(
    h: *mut Handle,
    callback: TmRoomChangeCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);
    handle.client.on_room_list_changed(Box::new(move || {
        callback(ud.as_ptr());
    }));
}

/// Register a callback that fires when the sync state changes.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `callback` will be invoked on a background thread.
/// - `userdata` is passed through to the callback unchanged.
#[no_mangle]
pub unsafe extern "C" fn tm_set_sync_state_callback(
    h: *mut Handle,
    callback: TmSyncStateCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);
    handle.matrix.on_sync_state_changed(Box::new(move |state| {
        callback(state, ud.as_ptr());
    }));
}

/// Force an immediate sliding-sync reconnect, short-circuiting backoff. Call when
/// the OS network monitor reports the interface came back, so the app doesn't wait
/// for the long-poll to time out before reconnecting.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_sync_reconnect(h: *mut Handle) {
    let handle = unsafe { &*h };
    handle.matrix.reconnect_now();
}

/// Register a callback that fires when a user's presence changes.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `callback` will be invoked on a background thread.
/// - `userdata` is passed through to the callback unchanged.
#[no_mangle]
pub unsafe extern "C" fn tm_set_presence_callback(
    h: *mut Handle,
    callback: TmPresenceCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);
    handle
        .client
        .on_presence_changed(Box::new(move |user_id, state, last_active_ts| {
            if let Ok(c_user_id) = CString::new(user_id) {
                callback(c_user_id.as_ptr(), state, last_active_ts, ud.as_ptr());
            }
        }));
}

/// Register a callback that fires when the typing user list changes for a room.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `callback` will be invoked on a background thread.
/// - `userdata` is passed through to the callback unchanged.
#[no_mangle]
pub unsafe extern "C" fn tm_set_typing_callback(
    h: *mut Handle,
    callback: TmTypingCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);
    handle
        .matrix
        .on_typing_changed(Box::new(move |room_id, user_ids| {
            if let Ok(c_room_id) = CString::new(room_id) {
                let c_strings: Vec<CString> = user_ids
                    .iter()
                    .filter_map(|u| CString::new(u.as_str()).ok())
                    .collect();
                let ptrs: Vec<*const c_char> = c_strings.iter().map(|s| s.as_ptr()).collect();
                callback(c_room_id.as_ptr(), ptrs.as_ptr(), ptrs.len(), ud.as_ptr());
            }
        }));
}

/// Send a typing notice for a room.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `room_id` must be a valid null-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn tm_send_typing(h: *mut Handle, room_id: *const c_char, typing: bool) {
    let handle = unsafe { &*h };
    let rid = c_to_string(room_id);
    let matrix = handle.matrix.clone();
    handle.runtime.spawn(async move {
        if let Err(e) = matrix.send_typing(&rid, typing).await {
            tracing::warn!("tm_send_typing failed for {rid}: {e}");
        }
    });
}

/// Register a callback that fires when a room's timeline changes.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `room_id` must be a valid null-terminated C string.
/// - `callback` will be invoked on a background thread.
/// - `userdata` is passed through to the callback unchanged.
#[no_mangle]
pub unsafe extern "C" fn tm_set_timeline_change_callback(
    h: *mut Handle,
    room_id: *const c_char,
    callback: TmTimelineChangeCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let ud = Userdata::new(userdata);
    handle.client.on_timeline_changed(
        &room_id_str,
        Box::new(move || {
            callback(ud.as_ptr());
        }),
    );
}

/// Register the per-message desktop-notification callback.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `callback` will be invoked on a background thread.
/// - `userdata` is passed through to the callback unchanged.
#[no_mangle]
pub unsafe extern "C" fn tm_set_notification_callback(
    h: *mut Handle,
    callback: TmNotificationCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);
    handle.matrix.on_notification(Box::new(
        move |room_id, event_id, sender, avatar, room_name, body, is_direct, is_mention, timestamp| {
            let (Ok(c_room), Ok(c_event), Ok(c_sender), Ok(c_avatar), Ok(c_room_name), Ok(c_body)) = (
                CString::new(room_id),
                CString::new(event_id),
                CString::new(sender),
                CString::new(avatar),
                CString::new(room_name),
                CString::new(body),
            ) else {
                return;
            };
            callback(
                c_room.as_ptr(),
                c_event.as_ptr(),
                c_sender.as_ptr(),
                c_avatar.as_ptr(),
                c_room_name.as_ptr(),
                c_body.as_ptr(),
                is_direct,
                is_mention,
                timestamp,
                ud.as_ptr(),
            );
        },
    ));
}

/// Register a callback fired around a room's member fetch (in_progress
/// true/false), so the UI can show a "syncing members" indicator.
///
/// # Safety
/// `h` must be a valid Handle pointer; `callback`/`userdata` must outlive the session.
#[no_mangle]
pub unsafe extern "C" fn tm_set_member_sync_callback(
    h: *mut Handle,
    callback: TmMemberSyncCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);
    handle
        .matrix
        .on_member_sync(Box::new(move |room_id, in_progress| {
            let Ok(c_room) = CString::new(room_id) else {
                return;
            };
            callback(c_room.as_ptr(), in_progress, ud.as_ptr());
        }));
}

/// Register the room-invite desktop-notification callback.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `callback` will be invoked on a background thread.
/// - `userdata` is passed through to the callback unchanged.
#[no_mangle]
pub unsafe extern "C" fn tm_set_invite_notification_callback(
    h: *mut Handle,
    callback: TmInviteNotificationCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);
    handle.matrix.on_invite_notification(Box::new(
        move |room_id, inviter, avatar, room_name, is_direct| {
            let (Ok(c_room), Ok(c_inviter), Ok(c_avatar), Ok(c_room_name)) = (
                CString::new(room_id),
                CString::new(inviter),
                CString::new(avatar),
                CString::new(room_name),
            ) else {
                return;
            };
            callback(
                c_room.as_ptr(),
                c_inviter.as_ptr(),
                c_avatar.as_ptr(),
                c_room_name.as_ptr(),
                is_direct,
                ud.as_ptr(),
            );
        },
    ));
}

/// Register the "new login" callback (process-global; single consumer). Fires
/// once for each newly-appeared, unverified session on the account.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `callback` will be invoked on a background thread.
/// - `userdata` is passed through to the callback unchanged.
#[no_mangle]
pub unsafe extern "C" fn tm_set_new_login_callback(
    h: *mut Handle,
    callback: TmNewLoginCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);
    handle.matrix.on_new_login(Box::new(
        move |device_id, display_name, last_seen_ip, ts| {
            let (Ok(c_device), Ok(c_name), Ok(c_ip)) = (
                CString::new(device_id),
                CString::new(display_name),
                CString::new(last_seen_ip),
            ) else {
                return;
            };
            callback(
                c_device.as_ptr(),
                c_name.as_ptr(),
                c_ip.as_ptr(),
                ts,
                ud.as_ptr(),
            );
        },
    ));
}

/// Register the remote-sign-out callback. Fires at most once per session, when
/// the homeserver rejects this session's access token.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `callback` will be invoked on a background thread.
/// - `userdata` is passed through to the callback unchanged.
#[no_mangle]
pub unsafe extern "C" fn tm_set_session_invalidated_callback(
    h: *mut Handle,
    callback: TmSessionInvalidatedCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);
    handle
        .matrix
        .on_session_invalidated(Box::new(move |soft_logout| {
            callback(soft_logout, ud.as_ptr());
        }));
}

/// Callback fired when a message's URL link-preview fetch starts/stops, so the UI
/// can glow the URL only while a fetch is active. `fetching` is true on start,
/// false on completion (success or failure).
pub type TmPreviewFetchingCallback = extern "C" fn(
    room_id: *const c_char,
    event_id: *const c_char,
    fetching: bool,
    userdata: *mut libc::c_void,
);

/// Register this session's URL-preview "fetching" callback.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `callback` will be invoked on background threads.
/// - `userdata` is passed through to the callback unchanged.
#[no_mangle]
pub unsafe extern "C" fn tm_set_preview_fetching_callback(
    h: *mut Handle,
    callback: TmPreviewFetchingCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);
    handle.matrix.on_preview_fetching(Box::new(
        move |room_id: &str, event_id: &str, fetching: bool| {
            let (Ok(c_room), Ok(c_event)) = (CString::new(room_id), CString::new(event_id)) else {
                return;
            };
            callback(c_room.as_ptr(), c_event.as_ptr(), fetching, ud.as_ptr());
        },
    ));
}

// --- Verification FFI functions ---

/// Free an FfiSasEmojiList returned via TmSasCallback.
///
/// # Safety
/// `list` must have been returned by a TmSasCallback invocation.
#[no_mangle]
pub unsafe extern "C" fn tm_free_sas_emojis(list: FfiSasEmojiList) {
    if list.emojis.is_null() || list.len == 0 {
        return;
    }
    let emojis = unsafe { reclaim_boxed_slice(list.emojis, list.len) };
    for e in emojis {
        unsafe {
            free_c_string(e.emoji);
            free_c_string(e.label);
        }
    }
}

/// C-compatible owned QR code module grid (`size`*`size` bytes; 1 = dark, 0 = light).
#[repr(C)]
pub struct FfiQrCode {
    pub modules: *mut u8,
    pub size: usize,
}

/// Callback for a generated QR code.
pub type TmQrCodeCallback =
    extern "C" fn(success: bool, qr: FfiQrCode, userdata: *mut libc::c_void);

/// Free an FfiQrCode returned via TmQrCodeCallback.
///
/// # Safety
/// `qr` must have been returned by a TmQrCodeCallback invocation.
#[no_mangle]
pub unsafe extern "C" fn tm_free_qr_code(qr: FfiQrCode) {
    if qr.modules.is_null() || qr.size == 0 {
        return;
    }
    let _ = unsafe { reclaim_boxed_slice(qr.modules, qr.size * qr.size) };
}

/// Start SAS emoji verification asynchronously. `success` = the flow was
/// initiated; the emoji list here is always empty — emojis arrive via
/// `tm_set_sas_emojis_callback`.
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_start_sas_verification(
    h: *mut Handle,
    callback: TmSasCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let started = client.start_sas_verification().await.is_ok();
        let empty = FfiSasEmojiList {
            emojis: ptr::null_mut(),
            len: 0,
        };
        callback(started, empty, ud.as_ptr());
    });
}

/// Start SAS emoji verification for a specific verification request.
/// `success` = the flow was initiated; emojis arrive via
/// `tm_set_sas_emojis_callback`.
///
/// # Safety
/// `h` must be a valid Handle pointer and `transaction_id` must be a valid
/// null-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn tm_start_sas_verification_for(
    h: *mut Handle,
    transaction_id: *const c_char,
    callback: TmSasCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let matrix = handle.matrix.clone();
    let tx = c_to_string(transaction_id);
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let started = matrix.start_sas_verification_for(&tx).await.is_ok();
        let empty = FfiSasEmojiList {
            emojis: ptr::null_mut(),
            len: 0,
        };
        callback(started, empty, ud.as_ptr());
    });
}

/// Show a QR code for device verification (no specific request). `success` =
/// the flow was initiated; the module grid here is always empty — it arrives
/// via `tm_set_qr_data_callback`.
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_start_qr_verification(
    h: *mut Handle,
    callback: TmQrCodeCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let started = client.start_qr_verification().await.is_ok();
        let empty = FfiQrCode {
            modules: ptr::null_mut(),
            size: 0,
        };
        callback(started, empty, ud.as_ptr());
    });
}

/// Show a QR code for a specific verification request. `success` = the flow
/// was initiated; the module grid arrives via `tm_set_qr_data_callback`.
///
/// # Safety
/// `h` must be a valid Handle pointer and `transaction_id` a valid C string.
#[no_mangle]
pub unsafe extern "C" fn tm_start_qr_verification_for(
    h: *mut Handle,
    transaction_id: *const c_char,
    callback: TmQrCodeCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let matrix = handle.matrix.clone();
    let tx = c_to_string(transaction_id);
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let started = matrix.start_qr_verification_for(&tx).await.is_ok();
        let empty = FfiQrCode {
            modules: ptr::null_mut(),
            size: 0,
        };
        callback(started, empty, ud.as_ptr());
    });
}

/// Confirm the other device scanned our QR code.
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_confirm_qr_scanned(
    h: *mut Handle,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let success = client.confirm_qr_scanned().await.is_ok();
        callback(success, ud.as_ptr());
    });
}

/// Confirm that the SAS emojis match on both sides.
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_confirm_sas(
    h: *mut Handle,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let success = client.confirm_sas_match().await.is_ok();
        callback(success, ud.as_ptr());
    });
}

/// Verify the session with a recovery key string.
///
/// # Safety
/// `h` must be a valid Handle pointer. `key` must be a valid C string.
#[no_mangle]
pub unsafe extern "C" fn tm_verify_recovery_key(
    h: *mut Handle,
    key: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let key_str = c_to_string(key);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let success = client.verify_with_recovery_key(&key_str).await.is_ok();
        callback(success, ud.as_ptr());
    });
}

/// Skip session verification — cancels any in-flight request and resets state.
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_skip_verification(
    h: *mut Handle,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let success = client.skip_verification().await.is_ok();
        callback(success, ud.as_ptr());
    });
}

/// Callback for verification state changes. `flow_id` identifies the
/// verification flow the state belongs to (empty when there is no specific
/// flow), so the UI can ignore stale states from a torn-down flow.
pub type TmVerificationStateCallback =
    extern "C" fn(state: u32, flow_id: *const c_char, userdata: *mut libc::c_void);

/// Callback for device verification status changes.
pub type TmDeviceVerifiedCallback = extern "C" fn(verified: bool, userdata: *mut libc::c_void);

/// Callback for incoming self-verification requests.
pub type TmIncomingVerificationRequestCallback = extern "C" fn(
    transaction_id: *const c_char,
    device_id: *const c_char,
    device_name: *const c_char,
    userdata: *mut libc::c_void,
);

/// Callback for an incoming IN-ROOM verification request from ANOTHER user.
/// `flow_id` is the verification flow id; `user_id` / `display_name` identify
/// the requesting user.
pub type TmIncomingUserVerificationRequestCallback = extern "C" fn(
    flow_id: *const c_char,
    user_id: *const c_char,
    display_name: *const c_char,
    userdata: *mut libc::c_void,
);

/// Callback for an incoming verification request that can no longer be
/// answered (accepted on another session, withdrawn, or expired).
pub type TmVerificationRequestClosedCallback =
    extern "C" fn(flow_id: *const c_char, userdata: *mut libc::c_void);

/// Why a verification flow was cancelled; fires just before the Cancelled state.
pub type TmVerificationCancelInfoCallback = extern "C" fn(
    flow_id: *const c_char,
    cancel_code: *const c_char,
    cancelled_by_us: bool,
    userdata: *mut libc::c_void,
);

/// C-compatible verification capabilities.
#[repr(C)]
pub struct FfiVerificationCapabilities {
    pub can_verify_with_device: bool,
    pub can_verify_with_recovery: bool,
    pub sas_supported: bool,
    pub qr_supported: bool,
}

/// Callback for verification capabilities.
pub type TmVerificationCapabilitiesCallback =
    extern "C" fn(success: bool, caps: FfiVerificationCapabilities, userdata: *mut libc::c_void);

/// Result callback for `tm_user_trust_state`: `state` is a `UserTrustState`
/// discriminant (0 = unverified, 1 = verified, 2 = violation).
pub type TmUserTrustStateCallback =
    extern "C" fn(success: bool, state: u32, userdata: *mut libc::c_void);

/// Callback for another user's trust state changing. `state` is a
/// `UserTrustState` discriminant.
pub type TmUserTrustChangedCallback =
    extern "C" fn(user_id: *const c_char, state: u32, userdata: *mut libc::c_void);

/// Cancel active verification flow.
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_cancel_verification(
    h: *mut Handle,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let success = client.cancel_verification().await.is_ok();
        callback(success, ud.as_ptr());
    });
}

/// Reject SAS verification due to an emoji mismatch (sends `MismatchedSas`).
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_mismatch_sas(
    h: *mut Handle,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let success = client.mismatch_sas().await.is_ok();
        callback(success, ud.as_ptr());
    });
}

/// Cancel active verification flow only if it matches the expected request.
///
/// # Safety
/// `h` must be a valid Handle pointer and `transaction_id` must be a valid
/// null-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn tm_cancel_verification_for(
    h: *mut Handle,
    transaction_id: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let matrix = handle.matrix.clone();
    let tx = c_to_string(transaction_id);
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let success = matrix.cancel_verification_for(&tx).await.is_ok();
        callback(success, ud.as_ptr());
    });
}

/// Get verification capabilities asynchronously.
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_get_verification_capabilities(
    h: *mut Handle,
    callback: TmVerificationCapabilitiesCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client.get_verification_capabilities().await {
            Ok(caps) => {
                let ffi_caps = FfiVerificationCapabilities {
                    can_verify_with_device: caps.can_verify_with_device,
                    can_verify_with_recovery: caps.can_verify_with_recovery,
                    sas_supported: caps.sas_supported,
                    qr_supported: caps.qr_supported,
                };
                callback(true, ffi_caps, ud.as_ptr());
            }
            Err(_) => {
                let empty = FfiVerificationCapabilities {
                    can_verify_with_device: false,
                    can_verify_with_recovery: false,
                    sas_supported: false,
                    qr_supported: false,
                };
                callback(false, empty, ud.as_ptr());
            }
        }
    });
}

/// Register a callback for verification state changes.
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_set_verification_state_callback(
    h: *mut Handle,
    callback: TmVerificationStateCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);
    handle
        .matrix
        .on_verification_state_changed(Box::new(move |state, flow_id| {
            // Marshal flow_id as a C string; fall back to empty on interior NUL.
            let c_flow_id = CString::new(flow_id).unwrap_or_default();
            callback(state, c_flow_id.as_ptr(), ud.as_ptr());
        }));
}

/// Emojis for a SAS flow, delivered whenever they become available —
/// including a SAS the peer started (adopted `Transitioned`).
pub type TmSasEmojisAvailableCallback =
    extern "C" fn(flow_id: *const c_char, emojis: FfiSasEmojiList, userdata: *mut libc::c_void);

/// Register a callback for SAS emojis becoming available.
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_set_sas_emojis_callback(
    h: *mut Handle,
    callback: TmSasEmojisAvailableCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);
    handle
        .matrix
        .on_sas_emojis(Box::new(move |flow_id, emojis| {
            let c_flow_id = CString::new(flow_id).unwrap_or_default();
            let ffi_emojis: Vec<FfiSasEmoji> = emojis
                .iter()
                .map(|e| FfiSasEmoji {
                    emoji: str_to_c(&e.emoji),
                    label: str_to_c(&e.label),
                })
                .collect();
            let len = ffi_emojis.len();
            let ptr = leak_boxed_slice(ffi_emojis);
            callback(
                c_flow_id.as_ptr(),
                FfiSasEmojiList { emojis: ptr, len },
                ud.as_ptr(),
            );
        }));
}

/// QR code modules for a flow, delivered when generation completes.
pub type TmQrDataCallback =
    extern "C" fn(flow_id: *const c_char, qr: FfiQrCode, userdata: *mut libc::c_void);

/// Register a callback for a generated QR code's module grid.
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_set_qr_data_callback(
    h: *mut Handle,
    callback: TmQrDataCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);
    handle.matrix.on_qr_data(Box::new(move |flow_id, image| {
        let c_flow_id = CString::new(flow_id).unwrap_or_default();
        let size = image.size;
        let modules = leak_boxed_slice(image.modules.clone());
        callback(c_flow_id.as_ptr(), FfiQrCode { modules, size }, ud.as_ptr());
    }));
}

/// Register a callback for device verification status changes.
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_set_device_verified_callback(
    h: *mut Handle,
    callback: TmDeviceVerifiedCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);
    handle
        .matrix
        .on_device_verified_changed(Box::new(move |verified| {
            callback(verified, ud.as_ptr());
        }));
}

/// Register a callback for incoming self-verification requests.
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_set_incoming_verification_request_callback(
    h: *mut Handle,
    callback: TmIncomingVerificationRequestCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);
    handle.matrix.on_incoming_verification_request(Box::new(
        move |transaction_id, device_id, device_name| {
            let Ok(c_transaction_id) = CString::new(transaction_id) else {
                return;
            };
            let Ok(c_device_id) = CString::new(device_id) else {
                return;
            };
            let Ok(c_device_name) = CString::new(device_name) else {
                return;
            };
            callback(
                c_transaction_id.as_ptr(),
                c_device_id.as_ptr(),
                c_device_name.as_ptr(),
                ud.as_ptr(),
            );
        },
    ));
}

/// Register the callback for an incoming verification request going away.
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_set_verification_request_closed_callback(
    h: *mut Handle,
    callback: TmVerificationRequestClosedCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);
    handle
        .matrix
        .on_verification_request_closed(Box::new(move |flow_id| {
            let Ok(c_flow_id) = CString::new(flow_id) else {
                return;
            };
            callback(c_flow_id.as_ptr(), ud.as_ptr());
        }));
}

/// Register the callback for why a verification flow was cancelled. Fires
/// before the corresponding `Cancelled` state (see
/// `tm_set_verification_state_callback`).
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_set_verification_cancel_info_callback(
    h: *mut Handle,
    callback: TmVerificationCancelInfoCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);
    handle
        .matrix
        .on_verification_cancel_info(Box::new(move |flow_id, code, by_us| {
            let c_flow = CString::new(flow_id).unwrap_or_default();
            let c_code = CString::new(code).unwrap_or_default();
            callback(c_flow.as_ptr(), c_code.as_ptr(), by_us, ud.as_ptr());
        }));
}

/// Query another user's cross-signing trust state (for trust shields).
///
/// # Safety
/// `h` must be a valid Handle pointer; `user_id` a valid C string.
#[no_mangle]
pub unsafe extern "C" fn tm_user_trust_state(
    h: *mut Handle,
    user_id: *const c_char,
    callback: TmUserTrustStateCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let matrix = handle.matrix.clone();
    let uid = c_to_string(user_id);
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match matrix.user_trust_state(&uid).await {
            Ok(state) => callback(true, state as u32, ud.as_ptr()),
            Err(_) => callback(false, 0, ud.as_ptr()),
        }
    });
}

/// Register a callback for another user's trust-state changes.
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_set_user_trust_changed_callback(
    h: *mut Handle,
    callback: TmUserTrustChangedCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);
    handle
        .matrix
        .on_user_trust_changed(Box::new(move |user_id, state| {
            let Ok(c_user_id) = CString::new(user_id) else {
                return;
            };
            callback(c_user_id.as_ptr(), state, ud.as_ptr());
        }));
}

/// Start an interactive SAS (emoji) verification of another user's identity.
/// `success` = the flow was initiated; emojis arrive via
/// `tm_set_sas_emojis_callback`.
///
/// # Safety
/// `h` must be a valid Handle pointer; `user_id` a valid C string.
#[no_mangle]
pub unsafe extern "C" fn tm_start_user_verification(
    h: *mut Handle,
    user_id: *const c_char,
    callback: TmSasCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let matrix = handle.matrix.clone();
    let uid = c_to_string(user_id);
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let started = matrix.start_user_verification(&uid).await.is_ok();
        let empty = FfiSasEmojiList {
            emojis: ptr::null_mut(),
            len: 0,
        };
        callback(started, empty, ud.as_ptr());
    });
}

/// Withdraw our verification of another user's identity.
///
/// # Safety
/// `h` must be a valid Handle pointer; `user_id` a valid C string.
#[no_mangle]
pub unsafe extern "C" fn tm_withdraw_user_verification(
    h: *mut Handle,
    user_id: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let matrix = handle.matrix.clone();
    let uid = c_to_string(user_id);
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match matrix.withdraw_user_verification(&uid).await {
            Ok(()) => callback(true, ud.as_ptr()),
            Err(_) => callback(false, ud.as_ptr()),
        }
    });
}

/// Register a callback for incoming in-room verification requests from other users.
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_set_incoming_user_verification_request_callback(
    h: *mut Handle,
    callback: TmIncomingUserVerificationRequestCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);
    handle
        .matrix
        .on_incoming_user_verification_request(Box::new(move |flow_id, user_id, display_name| {
            let Ok(c_flow_id) = CString::new(flow_id) else {
                return;
            };
            let Ok(c_user_id) = CString::new(user_id) else {
                return;
            };
            let Ok(c_display_name) = CString::new(display_name) else {
                return;
            };
            callback(
                c_flow_id.as_ptr(),
                c_user_id.as_ptr(),
                c_display_name.as_ptr(),
                ud.as_ptr(),
            );
        }));
}

/// Wrapper to safely pass a raw userdata pointer across thread boundaries.
/// Stores the pointer as a usize so that closures capturing it are automatically
/// Send + Sync. The C++ caller is responsible for ensuring the pointed-to data
/// outlives the async operation and is accessed in a thread-safe manner.
#[derive(Clone, Copy)]
struct Userdata(usize);

impl Userdata {
    fn new(ptr: *mut libc::c_void) -> Self {
        Self(ptr as usize)
    }

    fn as_ptr(self) -> *mut libc::c_void {
        self.0 as *mut libc::c_void
    }
}

// --- Search FFI ---

/// Asynchronously search messages. Results delivered via callback.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `query` must be a valid null-terminated C string.
/// - `room_id` may be null for all-rooms search.
/// - `next_token` may be null for first page.
/// - `sender_filter` may be null for no sender filtering.
/// - `date_from` 0 means no lower bound; `date_to` 0 means no upper bound.
/// - `callback` will be invoked on a background thread.
#[no_mangle]
pub unsafe extern "C" fn tm_search_messages(
    h: *mut Handle,
    request_id: u64,
    scope: u32,
    room_id: *const c_char,
    query: *const c_char,
    limit: u32,
    next_token: *const c_char,
    sender_filter: *const c_char,
    date_from: u64,
    date_to: u64,
    callback: TmSearchCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);

    let request = SearchRequest {
        request_id,
        scope: if scope == 1 {
            SearchScope::AllRooms
        } else {
            SearchScope::Room
        },
        room_id: opt_c_to_string(room_id),
        query: c_to_string(query),
        limit,
        next_token: opt_c_to_string(next_token),
        sender_filter: opt_c_to_string(sender_filter),
        date_from: if date_from == 0 {
            None
        } else {
            Some(date_from)
        },
        date_to: if date_to == 0 { None } else { Some(date_to) },
    };

    // Register as active.
    lock_ffi_mutex(&handle.active_searches, "active_searches").insert(request_id);

    let client = handle.client.clone();
    let active = handle.active_searches.clone();

    handle.runtime.spawn(async move {
        let result = client.search_messages(request).await;

        // Check if still active (not cancelled).
        let still_active = lock_ffi_mutex(&active, "active_searches").remove(&request_id);
        if !still_active {
            // Cancelled — invoke callback with empty page so C++ frees userdata.
            let empty = FfiSearchPage {
                request_id,
                hits: ptr::null_mut(),
                hits_len: 0,
                total_approx: 0,
                next_token: ptr::null_mut(),
                done: true,
                e2ee_disabled: false,
                indexing: false,
            };
            let err_msg = CString::new("Cancelled").unwrap_or_default();
            callback(false, empty, err_msg.as_ptr(), ud.as_ptr());
            return;
        }

        match result {
            Ok(page) => {
                let hits_len = page.hits.len();
                let hits_ptr = if hits_len > 0 {
                    let ffi_hits: Vec<FfiSearchHit> = page
                        .hits
                        .iter()
                        .map(|h| FfiSearchHit {
                            room_id: str_to_c(&h.room_id),
                            event_id: str_to_c(&h.event_id),
                            sender_id: str_to_c(&h.sender_id),
                            sender_name: str_to_c(&h.sender_name),
                            timestamp: h.timestamp,
                            snippet: str_to_c(&h.snippet),
                            rank: h.rank,
                            local_only: h.local_only,
                        })
                        .collect();
                    leak_boxed_slice(ffi_hits)
                } else {
                    ptr::null_mut()
                };

                let ffi_page = FfiSearchPage {
                    request_id: page.request_id,
                    hits: hits_ptr,
                    hits_len,
                    total_approx: page.total_approx,
                    next_token: opt_str_to_c(&page.next_token),
                    done: page.done,
                    e2ee_disabled: page.e2ee_disabled,
                    indexing: page.indexing,
                };
                callback(true, ffi_page, ptr::null(), ud.as_ptr());
            }
            Err(e) => {
                let msg = CString::new(e.to_string()).unwrap_or_default();
                let empty = FfiSearchPage {
                    request_id,
                    hits: ptr::null_mut(),
                    hits_len: 0,
                    total_approx: 0,
                    next_token: ptr::null_mut(),
                    done: true,
                    e2ee_disabled: false,
                    indexing: false,
                };
                callback(false, empty, msg.as_ptr(), ud.as_ptr());
            }
        }
    });
}

/// Cancel a pending search request.
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_cancel_search(h: *mut Handle, request_id: u64) {
    let handle = unsafe { &*h };
    lock_ffi_mutex(&handle.active_searches, "active_searches").remove(&request_id);
}

/// Free a search results page returned via callback.
///
/// # Safety
/// `page` must have been received from a `TmSearchCallback`.
#[no_mangle]
pub unsafe extern "C" fn tm_free_search_page(page: FfiSearchPage) {
    if !page.hits.is_null() && page.hits_len > 0 {
        let hits = unsafe { reclaim_boxed_slice(page.hits, page.hits_len) };
        for hit in hits {
            unsafe {
                free_c_string(hit.room_id);
                free_c_string(hit.event_id);
                free_c_string(hit.sender_id);
                free_c_string(hit.sender_name);
                free_c_string(hit.snippet);
            }
        }
    }
    unsafe {
        free_c_string(page.next_token);
    }
}

// --- Room directory / spaces / join FFI ---

/// One room or space in a directory search or a space's child list.
#[repr(C)]
pub struct FfiRoomDirectoryEntry {
    pub room_id: *mut c_char,
    pub name: *mut c_char,
    pub topic: *mut c_char,
    pub canonical_alias: *mut c_char,
    pub avatar_url: *mut c_char,
    pub member_count: u32,
    pub children_count: u32,
    pub is_space: bool,
    pub world_readable: bool,
    pub guest_can_join: bool,
    pub join_rule: u32,
    pub membership: u32,
    pub via: *mut *mut c_char,
    pub via_len: usize,
}

/// A page of directory or space-hierarchy results. `total_approx` is -1 when the server gave no
/// estimate.
#[repr(C)]
pub struct FfiRoomDirectoryPage {
    pub request_id: u64,
    pub entries: *mut FfiRoomDirectoryEntry,
    pub entries_len: usize,
    pub total_approx: i32,
    pub next_token: *mut c_char,
    pub done: bool,
}

/// What can be shown about a room before joining it.
#[repr(C)]
pub struct FfiRoomPreview {
    pub room_id: *mut c_char,
    pub name: *mut c_char,
    pub topic: *mut c_char,
    pub canonical_alias: *mut c_char,
    pub avatar_url: *mut c_char,
    pub member_count: u32,
    pub is_space: bool,
    pub join_rule: u32,
    pub membership: u32,
    pub world_readable: bool,
}

/// `page` is owned by the callee and must be released with `tm_free_room_directory_page`.
/// `error` is borrowed and valid only for the duration of the call.
pub type TmRoomDirectoryCallback = extern "C" fn(
    success: bool,
    page: FfiRoomDirectoryPage,
    error: *const c_char,
    userdata: *mut libc::c_void,
);

/// `preview` is owned by the callee and must be released with `tm_free_room_preview`.
pub type TmRoomPreviewCallback = extern "C" fn(
    success: bool,
    preview: FfiRoomPreview,
    error: *const c_char,
    userdata: *mut libc::c_void,
);

/// `room_id` (the resolved ID, which differs from the request when joining by alias) and `error`
/// are borrowed and valid only for the duration of the call.
pub type TmJoinRoomCallback = extern "C" fn(
    success: bool,
    room_id: *const c_char,
    error: *const c_char,
    userdata: *mut libc::c_void,
);

fn empty_directory_page(request_id: u64) -> FfiRoomDirectoryPage {
    FfiRoomDirectoryPage {
        request_id,
        entries: ptr::null_mut(),
        entries_len: 0,
        total_approx: -1,
        next_token: ptr::null_mut(),
        done: true,
    }
}

fn empty_room_preview() -> FfiRoomPreview {
    FfiRoomPreview {
        room_id: ptr::null_mut(),
        name: ptr::null_mut(),
        topic: ptr::null_mut(),
        canonical_alias: ptr::null_mut(),
        avatar_url: ptr::null_mut(),
        member_count: 0,
        is_space: false,
        join_rule: 6, // Unknown
        membership: 0,
        world_readable: false,
    }
}

fn ffi_directory_page(page: RoomDirectoryPage) -> FfiRoomDirectoryPage {
    let entries_len = page.entries.len();
    let entries = if entries_len > 0 {
        let converted: Vec<FfiRoomDirectoryEntry> = page
            .entries
            .iter()
            .map(|entry| {
                let via_len = entry.via.len();
                let via = if via_len > 0 {
                    let servers: Vec<*mut c_char> = entry.via.iter().map(|s| str_to_c(s)).collect();
                    leak_boxed_slice(servers)
                } else {
                    ptr::null_mut()
                };
                FfiRoomDirectoryEntry {
                    room_id: str_to_c(&entry.room_id),
                    name: str_to_c(&entry.name),
                    topic: str_to_c(&entry.topic),
                    canonical_alias: str_to_c(&entry.canonical_alias),
                    avatar_url: str_to_c(&entry.avatar_url),
                    member_count: entry.member_count,
                    children_count: entry.children_count,
                    is_space: entry.is_space,
                    world_readable: entry.world_readable,
                    guest_can_join: entry.guest_can_join,
                    join_rule: entry.join_rule as u32,
                    membership: entry.membership as u32,
                    via,
                    via_len,
                }
            })
            .collect();
        leak_boxed_slice(converted)
    } else {
        ptr::null_mut()
    };

    FfiRoomDirectoryPage {
        request_id: page.request_id,
        entries,
        entries_len,
        total_approx: page.total_approx,
        next_token: opt_str_to_c(&page.next_token),
        done: page.done,
    }
}

/// Deliver a directory/hierarchy outcome, honouring cancellation. A cancelled request still fires
/// the callback so C++ frees its heap-allocated userdata.
fn deliver_directory_page(
    result: Result<RoomDirectoryPage, anyhow::Error>,
    request_id: u64,
    active: &Arc<Mutex<HashSet<u64>>>,
    callback: TmRoomDirectoryCallback,
    ud: &Userdata,
) {
    let still_active = lock_ffi_mutex(active, "active_directory_requests").remove(&request_id);
    if !still_active {
        let msg = CString::new("Cancelled").unwrap_or_default();
        callback(
            false,
            empty_directory_page(request_id),
            msg.as_ptr(),
            ud.as_ptr(),
        );
        return;
    }

    match result {
        Ok(page) => callback(true, ffi_directory_page(page), ptr::null(), ud.as_ptr()),
        Err(e) => {
            let msg = CString::new(e.to_string()).unwrap_or_default();
            callback(
                false,
                empty_directory_page(request_id),
                msg.as_ptr(),
                ud.as_ptr(),
            );
        }
    }
}

/// Search the homeserver's public room directory. `query` may be null or empty to browse the whole
/// directory. Spaces are included in the results.
///
/// # Safety
/// `h` must be a valid Handle pointer. `query` and `next_token` may be null.
#[no_mangle]
pub unsafe extern "C" fn tm_search_public_rooms(
    h: *mut Handle,
    request_id: u64,
    query: *const c_char,
    limit: u32,
    next_token: *const c_char,
    callback: TmRoomDirectoryCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);

    let request = RoomDirectoryRequest {
        request_id,
        query: c_to_string(query),
        limit,
        next_token: opt_c_to_string(next_token),
    };

    lock_ffi_mutex(
        &handle.active_directory_requests,
        "active_directory_requests",
    )
    .insert(request_id);

    let client = handle.client.clone();
    let active = handle.active_directory_requests.clone();

    handle.runtime.spawn(async move {
        let result = client.search_public_rooms(request).await;
        deliver_directory_page(result, request_id, &active, callback, &ud);
    });
}

/// One page of a space's immediate children. There is no server-side search inside a space, so the
/// caller pages these in and filters them locally.
///
/// # Safety
/// `h` must be a valid Handle pointer. `space_id` must be a valid C string; `next_token` may be null.
#[no_mangle]
pub unsafe extern "C" fn tm_get_space_children(
    h: *mut Handle,
    request_id: u64,
    space_id: *const c_char,
    limit: u32,
    next_token: *const c_char,
    callback: TmRoomDirectoryCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);

    let request = SpaceHierarchyRequest {
        request_id,
        space_id: c_to_string(space_id),
        limit,
        next_token: opt_c_to_string(next_token),
    };

    lock_ffi_mutex(
        &handle.active_directory_requests,
        "active_directory_requests",
    )
    .insert(request_id);

    let client = handle.client.clone();
    let active = handle.active_directory_requests.clone();

    handle.runtime.spawn(async move {
        let result = client.space_children(request).await;
        deliver_directory_page(result, request_id, &active, callback, &ud);
    });
}

/// Cancel a pending directory/hierarchy request. Its callback still fires (with success=false).
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_cancel_room_directory_request(h: *mut Handle, request_id: u64) {
    let handle = unsafe { &*h };
    lock_ffi_mutex(
        &handle.active_directory_requests,
        "active_directory_requests",
    )
    .remove(&request_id);
}

/// Free a directory page returned via callback.
///
/// # Safety
/// `page` must have been received from a `TmRoomDirectoryCallback`.
#[no_mangle]
pub unsafe extern "C" fn tm_free_room_directory_page(page: FfiRoomDirectoryPage) {
    if !page.entries.is_null() && page.entries_len > 0 {
        let entries = unsafe { reclaim_boxed_slice(page.entries, page.entries_len) };
        for entry in entries {
            unsafe {
                free_c_string(entry.room_id);
                free_c_string(entry.name);
                free_c_string(entry.topic);
                free_c_string(entry.canonical_alias);
                free_c_string(entry.avatar_url);
                // Inner strings first, then the array itself — the other order is a use-after-free.
                if !entry.via.is_null() && entry.via_len > 0 {
                    let servers = reclaim_boxed_slice(entry.via, entry.via_len);
                    for server in servers {
                        free_c_string(server);
                    }
                }
            }
        }
    }
    unsafe {
        free_c_string(page.next_token);
    }
}

/// Free a room preview returned via callback.
///
/// # Safety
/// `preview` must have been received from a `TmRoomPreviewCallback`.
#[no_mangle]
pub unsafe extern "C" fn tm_free_room_preview(preview: FfiRoomPreview) {
    unsafe {
        free_c_string(preview.room_id);
        free_c_string(preview.name);
        free_c_string(preview.topic);
        free_c_string(preview.canonical_alias);
        free_c_string(preview.avatar_url);
    }
}

/// Fetch what can be shown about a room before joining it. Matrix serves no message history to a
/// non-member, so this is the whole content of the preview screen.
///
/// # Safety
/// `h` must be a valid Handle pointer. `room_id_or_alias` must be a valid C string.
/// `via` must point to `via_len` valid C strings, or be null when `via_len` is 0.
#[no_mangle]
pub unsafe extern "C" fn tm_get_room_preview(
    h: *mut Handle,
    room_id_or_alias: *const c_char,
    via: *const *const c_char,
    via_len: usize,
    callback: TmRoomPreviewCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);
    let target = c_to_string(room_id_or_alias);
    // Copy out of C memory before spawning — raw pointers are not Send.
    let servers = unsafe { c_string_array_to_vec(via, via_len) };

    let client = handle.client.clone();

    handle.runtime.spawn(async move {
        match client.room_preview(&target, servers).await {
            Ok(info) => {
                let ffi = FfiRoomPreview {
                    room_id: str_to_c(&info.room_id),
                    name: str_to_c(&info.name),
                    topic: str_to_c(&info.topic),
                    canonical_alias: str_to_c(&info.canonical_alias),
                    avatar_url: str_to_c(&info.avatar_url),
                    member_count: info.member_count,
                    is_space: info.is_space,
                    join_rule: info.join_rule as u32,
                    membership: info.membership as u32,
                    world_readable: info.world_readable,
                };
                callback(true, ffi, ptr::null(), ud.as_ptr());
            }
            Err(e) => {
                let msg = CString::new(e.to_string()).unwrap_or_default();
                callback(false, empty_room_preview(), msg.as_ptr(), ud.as_ptr());
            }
        }
    });
}

/// Join a room by ID or alias. `via` carries server hints needed to bootstrap a federated join.
///
/// # Safety
/// `h` must be a valid Handle pointer. `room_id_or_alias` must be a valid C string.
/// `via` must point to `via_len` valid C strings, or be null when `via_len` is 0.
#[no_mangle]
pub unsafe extern "C" fn tm_join_room(
    h: *mut Handle,
    room_id_or_alias: *const c_char,
    via: *const *const c_char,
    via_len: usize,
    callback: TmJoinRoomCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);
    let target = c_to_string(room_id_or_alias);
    let servers = unsafe { c_string_array_to_vec(via, via_len) };

    let client = handle.client.clone();

    handle.runtime.spawn(async move {
        match client.join_room(&target, servers).await {
            Ok(room_id) => {
                let c_room_id = CString::new(room_id).unwrap_or_default();
                callback(true, c_room_id.as_ptr(), ptr::null(), ud.as_ptr());
            }
            Err(e) => {
                let msg = CString::new(e.to_string()).unwrap_or_default();
                callback(false, ptr::null(), msg.as_ptr(), ud.as_ptr());
            }
        }
    });
}

/// Knock on a room by ID or alias (request to join). Same callback shape as `tm_join_room`.
///
/// # Safety
/// `h` must be a valid Handle pointer. `room_id_or_alias` must be a valid C string.
/// `via` must point to `via_len` valid C strings, or be null when `via_len` is 0.
#[no_mangle]
pub unsafe extern "C" fn tm_knock_room(
    h: *mut Handle,
    room_id_or_alias: *const c_char,
    via: *const *const c_char,
    via_len: usize,
    callback: TmJoinRoomCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);
    let target = c_to_string(room_id_or_alias);
    let servers = unsafe { c_string_array_to_vec(via, via_len) };

    let client = handle.client.clone();

    handle.runtime.spawn(async move {
        match client.knock_room(&target, servers).await {
            Ok(room_id) => {
                let c_room_id = CString::new(room_id).unwrap_or_default();
                callback(true, c_room_id.as_ptr(), ptr::null(), ud.as_ptr());
            }
            Err(e) => {
                let msg = CString::new(e.to_string()).unwrap_or_default();
                callback(false, ptr::null(), msg.as_ptr(), ud.as_ptr());
            }
        }
    });
}

/// Callback for `tm_preview_messages`. On success `timeline` carries the read-only history and the
/// C++ side owns it (must call `tm_free_timeline`); on failure `timeline` is empty and `error` is set.
/// `next_token` is the pagination token for the next-older page, or null at the start of history.
/// `room_id`, `next_token` and `error` are borrowed for the duration of the call.
pub type TmPreviewMessagesCallback = extern "C" fn(
    room_id: *const c_char,
    success: bool,
    timeline: FfiTimeline,
    next_token: *const c_char,
    error: *const c_char,
    userdata: *mut libc::c_void,
);

/// Read a page of history for an unjoined, world-readable room, paginating backward. `from` is null
/// for the newest page, else the previous callback's `next_token`. Non-world-readable rooms return
/// an error (the caller then keeps the name+topic placeholder).
///
/// # Safety
/// `h` must be a valid Handle pointer. `room_id` must be a valid C string. `from` may be null.
#[no_mangle]
pub unsafe extern "C" fn tm_preview_messages(
    h: *mut Handle,
    room_id: *const c_char,
    from: *const c_char,
    limit: u32,
    callback: TmPreviewMessagesCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud = Userdata::new(userdata);
    let target = c_to_string(room_id);
    let from_token = if from.is_null() {
        None
    } else {
        let s = c_to_string(from);
        (!s.is_empty()).then_some(s)
    };
    let client = handle.client.clone();

    handle.runtime.spawn(async move {
        // Borrowed by the callback only; dropped when this task ends, after the callback returns.
        let c_target = CString::new(target.clone()).unwrap_or_default();
        match client.preview_messages(&target, from_token, limit).await {
            Ok((items, next_token)) => {
                let (ptr, count) = timeline_items_to_ffi_raw(&items);
                // Ownership of the timeline transfers to C++, which frees it via tm_free_timeline.
                let timeline = FfiTimeline {
                    items: ptr,
                    len: count as usize,
                };
                let c_next = next_token.map(|t| CString::new(t).unwrap_or_default());
                let next_ptr = c_next.as_ref().map_or(ptr::null(), |c| c.as_ptr());
                callback(
                    c_target.as_ptr(),
                    true,
                    timeline,
                    next_ptr,
                    ptr::null(),
                    ud.as_ptr(),
                );
            }
            Err(e) => {
                let msg = CString::new(e.to_string()).unwrap_or_default();
                let empty = FfiTimeline {
                    items: ptr::null_mut(),
                    len: 0,
                };
                callback(
                    c_target.as_ptr(),
                    false,
                    empty,
                    ptr::null(),
                    msg.as_ptr(),
                    ud.as_ptr(),
                );
            }
        }
    });
}

/// # Safety
/// `array` must point to `len` valid C strings, or be null when `len` is 0.
unsafe fn c_string_array_to_vec(array: *const *const c_char, len: usize) -> Vec<String> {
    if array.is_null() || len == 0 {
        return Vec::new();
    }
    let slice = unsafe { std::slice::from_raw_parts(array, len) };
    slice.iter().map(|&s| c_to_string(s)).collect()
}

// --- User Profile Details FFI ---

/// Get detailed user profile for the profile popup.
///
/// # Safety
/// `h`, `room_id`, `user_id` must be valid pointers.
/// Caller must call `tm_free_user_profile_details` on the result.
fn ffi_user_profile_details_from_details(
    details: &crate::types::UserProfileDetails,
) -> FfiUserProfileDetails {
    FfiUserProfileDetails {
        room_id: str_to_c(&details.room_id),
        user_id: str_to_c(&details.user_id),
        display_name: str_to_c(&details.display_name),
        avatar_url: opt_str_to_c(&details.avatar_url),
        presence: details.presence as u32,
        last_active_ts: details.last_active_ts,
        membership: details.membership as u32,
        power_level: details.power_level,
        role: details.role as u32,
        is_ignored: details.is_ignored,
        dm_room_id: opt_str_to_c(&details.dm_room_id),
        can_invite: details.can_invite,
        can_kick: details.can_kick,
        can_ban: details.can_ban,
        can_mute: details.can_mute,
        can_change_power_level: details.can_change_power_level,
        max_assignable_power_level: details.max_assignable_power_level,
    }
}

fn fallback_user_profile_details(user_id: &str) -> FfiUserProfileDetails {
    FfiUserProfileDetails {
        room_id: ptr::null_mut(),
        user_id: str_to_c(user_id),
        display_name: str_to_c(user_id),
        avatar_url: ptr::null_mut(),
        presence: 0,
        last_active_ts: 0,
        membership: 2, // Leave
        power_level: 0,
        role: 2, // User
        is_ignored: false,
        dm_room_id: ptr::null_mut(),
        can_invite: false,
        can_kick: false,
        can_ban: false,
        can_mute: false,
        can_change_power_level: false,
        max_assignable_power_level: -1,
    }
}

/// Get detailed user profile for the profile popup asynchronously.
///
/// # Safety
/// `h`, `room_id`, `user_id` must be valid pointers.
/// Caller must call `tm_free_user_profile_details` on the value delivered to the callback.
#[no_mangle]
pub unsafe extern "C" fn tm_get_user_profile_details_async(
    h: *mut Handle,
    room_id: *const c_char,
    user_id: *const c_char,
    callback: TmUserProfileDetailsCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let user_id_str = c_to_string(user_id);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client
            .get_user_profile_details(&room_id_str, &user_id_str)
            .await
        {
            Ok(details) => callback(
                true,
                ffi_user_profile_details_from_details(&details),
                ud.as_ptr(),
            ),
            Err(_) => callback(
                false,
                fallback_user_profile_details(&user_id_str),
                ud.as_ptr(),
            ),
        }
    });
}

/// Free a user profile details struct.
///
/// # Safety
/// `details` must have been returned by `tm_get_user_profile_details`.
#[no_mangle]
pub unsafe extern "C" fn tm_free_user_profile_details(details: FfiUserProfileDetails) {
    unsafe {
        free_c_string(details.room_id);
        free_c_string(details.user_id);
        free_c_string(details.display_name);
        free_c_string(details.avatar_url);
        free_c_string(details.dm_room_id);
    }
}

/// Set a user's power level in a room.
///
/// # Safety
/// `h`, `room_id`, `user_id` must be valid.
#[no_mangle]
pub unsafe extern "C" fn tm_set_user_power_level(
    h: *mut Handle,
    room_id: *const c_char,
    user_id: *const c_char,
    power_level: i64,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let user_id_str = c_to_string(user_id);
    let client = handle.client.clone();

    let ud = Userdata::new(userdata);
    handle.runtime.spawn(async move {
        let result = client
            .set_user_power_level(&room_id_str, &user_id_str, power_level)
            .await;
        callback(result.is_ok(), ud.as_ptr());
    });
}

/// Open or create a direct-message room with a user.
///
/// # Safety
/// `h` and `user_id` must be valid.
#[no_mangle]
pub unsafe extern "C" fn tm_create_direct_room(
    h: *mut Handle,
    user_id: *const c_char,
    callback: TmCreateRoomCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let user_id_str = c_to_string(user_id);
    let client = handle.client.clone();

    let ud = Userdata::new(userdata);
    handle.runtime.spawn(async move {
        match client.create_direct_room(&user_id_str).await {
            Ok(room_id) => {
                let c_room_id = CString::new(room_id).unwrap_or_default();
                callback(true, c_room_id.as_ptr(), ud.as_ptr());
            }
            Err(_) => {
                callback(false, ptr::null(), ud.as_ptr());
            }
        }
    });
}

/// Callback for Saved Messages ensure. `room_id` is null when there is no saved
/// room (a passive ensure that found nothing and was not allowed to create).
pub type TmSavedMessagesCallback =
    extern "C" fn(success: bool, room_id: *const c_char, userdata: *mut libc::c_void);

/// Resolve the Saved Messages room. `create` (an explicit forward / open)
/// creates + mutes it on first use; without it (a passive session start) this
/// only adopts an existing room and never creates one.
///
/// # Safety
/// `h` must be a valid Handle.
#[no_mangle]
pub unsafe extern "C" fn tm_ensure_saved_messages_room(
    h: *mut Handle,
    create: bool,
    callback: TmSavedMessagesCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);
    handle.runtime.spawn(async move {
        match client.ensure_saved_messages_room(create).await {
            // Success with a room, or success with none (null room id).
            Ok(Some(room_id)) => {
                let c_room_id = CString::new(room_id).unwrap_or_default();
                callback(true, c_room_id.as_ptr(), ud.as_ptr());
            }
            Ok(None) => callback(true, ptr::null(), ud.as_ptr()),
            Err(_) => callback(false, ptr::null(), ud.as_ptr()),
        }
    });
}

/// Permanently delete Saved Messages: clear the marker and leave + forget the
/// room. The callback reports whether that succeeded.
///
/// # Safety
/// `h` must be a valid Handle.
#[no_mangle]
pub unsafe extern "C" fn tm_delete_saved_messages(
    h: *mut Handle,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);
    handle.runtime.spawn(async move {
        match client.delete_saved_messages_room().await {
            // Deleted, or nothing to delete — both are success for the UI.
            Ok(_) => callback(true, ud.as_ptr()),
            Err(e) => {
                tracing::warn!("[saved-messages] delete failed: {e:?}");
                callback(false, ud.as_ptr());
            }
        }
    });
}

/// Kick a user from a room.
///
/// # Safety
/// `h`, `room_id`, `user_id` must be valid. `reason` may be null.
#[no_mangle]
pub unsafe extern "C" fn tm_kick_user(
    h: *mut Handle,
    room_id: *const c_char,
    user_id: *const c_char,
    reason: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let user_id_str = c_to_string(user_id);
    let reason_str = opt_c_to_string(reason);
    let client = handle.client.clone();

    let ud = Userdata::new(userdata);
    handle.runtime.spawn(async move {
        let result = client
            .kick_user(&room_id_str, &user_id_str, reason_str.as_deref())
            .await;
        callback(result.is_ok(), ud.as_ptr());
    });
}

/// Ban a user from a room.
///
/// # Safety
/// `h`, `room_id`, `user_id` must be valid. `reason` may be null.
#[no_mangle]
pub unsafe extern "C" fn tm_ban_user(
    h: *mut Handle,
    room_id: *const c_char,
    user_id: *const c_char,
    reason: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let user_id_str = c_to_string(user_id);
    let reason_str = opt_c_to_string(reason);
    let client = handle.client.clone();

    let ud = Userdata::new(userdata);
    handle.runtime.spawn(async move {
        let result = client
            .ban_user(&room_id_str, &user_id_str, reason_str.as_deref())
            .await;
        callback(result.is_ok(), ud.as_ptr());
    });
}

/// Unban a user from a room.
///
/// # Safety
/// `h`, `room_id`, `user_id` must be valid.
#[no_mangle]
pub unsafe extern "C" fn tm_unban_user(
    h: *mut Handle,
    room_id: *const c_char,
    user_id: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let user_id_str = c_to_string(user_id);
    let client = handle.client.clone();

    let ud = Userdata::new(userdata);
    handle.runtime.spawn(async move {
        let result = client.unban_user(&room_id_str, &user_id_str).await;
        callback(result.is_ok(), ud.as_ptr());
    });
}

/// Invite a user to a room.
///
/// # Safety
/// `h`, `room_id`, `user_id` must be valid.
#[no_mangle]
pub unsafe extern "C" fn tm_invite_user(
    h: *mut Handle,
    room_id: *const c_char,
    user_id: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let user_id_str = c_to_string(user_id);
    let client = handle.client.clone();

    let ud = Userdata::new(userdata);
    handle.runtime.spawn(async move {
        let result = client.invite_user(&room_id_str, &user_id_str).await;
        callback(result.is_ok(), ud.as_ptr());
    });
}

/// Set or unset a user as ignored.
///
/// # Safety
/// `h` and `user_id` must be valid.
#[no_mangle]
pub unsafe extern "C" fn tm_set_user_ignored(
    h: *mut Handle,
    user_id: *const c_char,
    ignored: bool,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let user_id_str = c_to_string(user_id);
    let client = handle.client.clone();

    let ud = Userdata::new(userdata);
    handle.runtime.spawn(async move {
        let result = client.set_user_ignored(&user_id_str, ignored).await;
        callback(result.is_ok(), ud.as_ptr());
    });
}

// --- Room Settings Snapshot FFI ---

/// C-compatible room settings snapshot.
#[repr(C)]
pub struct FfiRoomSettingsSnapshot {
    pub room_id: *mut c_char,
    pub display_name: *mut c_char,
    pub canonical_alias: *mut c_char,
    pub notification_mode: u32,
    pub is_muted: bool,
    pub member_count: u64,
    pub is_encrypted: bool,
    pub encryption_algorithm: *mut c_char,
    pub access: u32,
    pub history_visibility: u32,
    pub new_members_can_see_history: bool,
    pub can_invite: bool,
    pub can_kick: bool,
    pub can_ban: bool,
    pub can_change_avatar: bool,
    pub can_change_name: bool,
    pub can_change_topic: bool,
    pub can_change_encryption: bool,
    pub can_change_access: bool,
    pub can_change_history_visibility: bool,
}

/// Callback for room settings snapshot.
pub type TmRoomSettingsCallback =
    extern "C" fn(success: bool, snapshot: FfiRoomSettingsSnapshot, userdata: *mut libc::c_void);

/// Get a snapshot of a room's settings and security state asynchronously.
///
/// # Safety
/// `h` and `room_id` must be valid pointers.
/// Caller must call `tm_free_room_settings` on the snapshot received in the callback.
#[no_mangle]
pub unsafe extern "C" fn tm_get_room_settings(
    h: *mut Handle,
    room_id: *const c_char,
    callback: TmRoomSettingsCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client.get_room_settings(&room_id_str).await {
            Ok(snap) => {
                let ffi = FfiRoomSettingsSnapshot {
                    room_id: str_to_c(&snap.room_id),
                    display_name: str_to_c(&snap.display_name),
                    canonical_alias: opt_str_to_c(&snap.canonical_alias),
                    notification_mode: snap.notification_mode as u32,
                    is_muted: snap.is_muted,
                    member_count: snap.member_count,
                    is_encrypted: snap.is_encrypted,
                    encryption_algorithm: opt_str_to_c(&snap.encryption_algorithm),
                    access: snap.access as u32,
                    history_visibility: snap.history_visibility as u32,
                    new_members_can_see_history: snap.new_members_can_see_history,
                    can_invite: snap.can_invite,
                    can_kick: snap.can_kick,
                    can_ban: snap.can_ban,
                    can_change_avatar: snap.can_change_avatar,
                    can_change_name: snap.can_change_name,
                    can_change_topic: snap.can_change_topic,
                    can_change_encryption: snap.can_change_encryption,
                    can_change_access: snap.can_change_access,
                    can_change_history_visibility: snap.can_change_history_visibility,
                };
                callback(true, ffi, ud.as_ptr());
            }
            Err(_) => {
                let empty = FfiRoomSettingsSnapshot {
                    room_id: str_to_c(&room_id_str),
                    display_name: ptr::null_mut(),
                    canonical_alias: ptr::null_mut(),
                    notification_mode: 0,
                    is_muted: false,
                    member_count: 0,
                    is_encrypted: false,
                    encryption_algorithm: ptr::null_mut(),
                    access: 6,             // Unknown
                    history_visibility: 4, // Unknown
                    new_members_can_see_history: false,
                    can_invite: false,
                    can_kick: false,
                    can_ban: false,
                    can_change_avatar: false,
                    can_change_name: false,
                    can_change_topic: false,
                    can_change_encryption: false,
                    can_change_access: false,
                    can_change_history_visibility: false,
                };
                callback(false, empty, ud.as_ptr());
            }
        }
    });
}

/// Free a room settings snapshot received from `tm_get_room_settings`.
///
/// # Safety
/// `snapshot` must have been received from a `tm_get_room_settings` callback.
#[no_mangle]
pub unsafe extern "C" fn tm_free_room_settings(snapshot: FfiRoomSettingsSnapshot) {
    unsafe {
        free_c_string(snapshot.room_id);
        free_c_string(snapshot.display_name);
        free_c_string(snapshot.canonical_alias);
        free_c_string(snapshot.encryption_algorithm);
    }
}

// --- Room Encryption FFI ---

/// Enable end-to-end encryption for a room (one-way operation).
///
/// # Safety
/// `h` and `room_id` must be valid pointers.
#[no_mangle]
pub unsafe extern "C" fn tm_enable_room_encryption(
    h: *mut Handle,
    room_id: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let success = client.enable_room_encryption(&room_id_str).await.is_ok();
        callback(success, ud.as_ptr());
    });
}

/// Set room access / join rule.
///
/// # Safety
/// `h` and `room_id` must be valid pointers.
#[no_mangle]
pub unsafe extern "C" fn tm_set_room_access(
    h: *mut Handle,
    room_id: *const c_char,
    access: u32,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let access = match access {
            0 => crate::types::RoomAccess::InviteOnly,
            1 => crate::types::RoomAccess::Public,
            2 => crate::types::RoomAccess::Knock,
            3 => crate::types::RoomAccess::Restricted,
            4 => crate::types::RoomAccess::KnockRestricted,
            5 => crate::types::RoomAccess::Private,
            _ => crate::types::RoomAccess::Unknown,
        };
        let success = client.set_room_access(&room_id_str, access).await.is_ok();
        callback(success, ud.as_ptr());
    });
}

/// Set the room name (m.room.name).
///
/// # Safety
/// `h`, `room_id`, and `name` must be valid pointers.
#[no_mangle]
pub unsafe extern "C" fn tm_set_room_name(
    h: *mut Handle,
    room_id: *const c_char,
    name: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let name_str = c_to_string(name);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let success = client.set_room_name(&room_id_str, &name_str).await.is_ok();
        callback(success, ud.as_ptr());
    });
}

/// Set the room topic (m.room.topic).
///
/// # Safety
/// `h`, `room_id`, and `topic` must be valid pointers.
#[no_mangle]
pub unsafe extern "C" fn tm_set_room_topic(
    h: *mut Handle,
    room_id: *const c_char,
    topic: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let topic_str = c_to_string(topic);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let success = client
            .set_room_topic(&room_id_str, &topic_str)
            .await
            .is_ok();
        callback(success, ud.as_ptr());
    });
}

/// Set room history visibility.
///
/// # Safety
/// `h` and `room_id` must be valid pointers.
#[no_mangle]
pub unsafe extern "C" fn tm_set_room_history_visibility(
    h: *mut Handle,
    room_id: *const c_char,
    visibility: u32,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let visibility = match visibility {
            0 => crate::types::HistoryVisibility::Joined,
            1 => crate::types::HistoryVisibility::Invited,
            2 => crate::types::HistoryVisibility::Shared,
            3 => crate::types::HistoryVisibility::WorldReadable,
            _ => crate::types::HistoryVisibility::Unknown,
        };
        let success = client
            .set_room_history_visibility(&room_id_str, visibility)
            .await
            .is_ok();
        callback(success, ud.as_ptr());
    });
}

// --- Registration FFI ---

/// Registration callback status codes.
#[repr(u32)]
pub enum TmRegStatus {
    Success = 0,
    Challenge = 1,
    Error = 2,
}

/// Callback type for registration result.
/// - status: 0=success, 1=uiaa_challenge, 2=error
/// - payload_json: JSON string with result data
pub type TmRegisterCallback =
    extern "C" fn(status: u32, payload_json: *const c_char, userdata: *mut libc::c_void);

/// Callback type for username availability check.
/// - status: 0=available, 1=unavailable, 2=invalid, 3=error
/// - message: error message (null if not error)
pub type TmUsernameCheckCallback =
    extern "C" fn(status: u32, message: *const c_char, userdata: *mut libc::c_void);

/// Asynchronously register a new account.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - All string params must be valid null-terminated C strings.
/// - `session` and `auth_json` may be null for initial registration.
#[no_mangle]
pub unsafe extern "C" fn tm_register(
    h: *mut Handle,
    homeserver: *const c_char,
    username: *const c_char,
    password: *const c_char,
    session: *const c_char,
    auth_json: *const c_char,
    callback: TmRegisterCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let request = crate::types::RegistrationRequest {
        homeserver: c_to_string(homeserver),
        username: c_to_string(username),
        password: c_to_string(password),
        session: opt_c_to_string(session),
        auth_json: opt_c_to_string(auth_json),
    };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);
    let is_continuation = request.session.is_some();

    handle.runtime.spawn(async move {
        let result = if is_continuation {
            client.submit_registration_auth(request).await
        } else {
            client.register(request).await
        };

        match result {
            Ok(crate::types::RegistrationResult::Success(profile)) => {
                let payload = serde_json::json!({
                    "user_id": profile.user_id,
                    "display_name": profile.display_name,
                    "avatar_url": profile.avatar_url,
                });
                let json = CString::new(payload.to_string()).unwrap_or_default();
                callback(TmRegStatus::Success as u32, json.as_ptr(), ud.as_ptr());
            }
            Ok(crate::types::RegistrationResult::Challenge(challenge)) => {
                let json_str = serde_json::to_string(&challenge).unwrap_or_default();
                let json = CString::new(json_str).unwrap_or_default();
                callback(TmRegStatus::Challenge as u32, json.as_ptr(), ud.as_ptr());
            }
            Err(e) => {
                let payload = serde_json::json!({
                    "error": format!("{e}"),
                });
                let json = CString::new(payload.to_string()).unwrap_or_default();
                callback(TmRegStatus::Error as u32, json.as_ptr(), ud.as_ptr());
            }
        }
    });
}

/// Asynchronously check username availability.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `homeserver` and `username` must be valid null-terminated C strings.
#[no_mangle]
pub unsafe extern "C" fn tm_check_username_available(
    h: *mut Handle,
    homeserver: *const c_char,
    username: *const c_char,
    callback: TmUsernameCheckCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let hs = c_to_string(homeserver);
    let user = c_to_string(username);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client.check_username_available(&hs, &user).await {
            Ok(crate::types::UsernameAvailability::Available) => {
                callback(0, ptr::null(), ud.as_ptr());
            }
            Ok(crate::types::UsernameAvailability::Unavailable) => {
                callback(1, ptr::null(), ud.as_ptr());
            }
            Ok(crate::types::UsernameAvailability::Invalid) => {
                callback(2, ptr::null(), ud.as_ptr());
            }
            Ok(crate::types::UsernameAvailability::Error(msg)) => {
                let c_msg = CString::new(msg).unwrap_or_default();
                callback(3, c_msg.as_ptr(), ud.as_ptr());
            }
            Err(e) => {
                let c_msg = CString::new(format!("{e}")).unwrap_or_default();
                callback(3, c_msg.as_ptr(), ud.as_ptr());
            }
        }
    });
}

/// Get the room list synchronously (blocks until complete).
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - Caller must call `tm_free_rooms` on the returned FfiRoomList.

/// Callback type for room creation result (returns room ID on success).
pub type TmCreateRoomCallback =
    extern "C" fn(success: bool, room_id: *const c_char, userdata: *mut libc::c_void);

#[no_mangle]
pub unsafe extern "C" fn tm_create_room(
    h: *mut Handle,
    name: *const c_char,
    topic: *const c_char,
    is_public: bool,
    encrypted: bool,
    alias: *const c_char,
    avatar_path: *const c_char,
    guest_access: i32,
    history_visibility: i32,
    federate: bool,
    callback: TmCreateRoomCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let name_str = c_to_string(name);
    let topic_str = if topic.is_null() {
        None
    } else {
        let s = c_to_string(topic);
        if s.is_empty() {
            None
        } else {
            Some(s)
        }
    };
    let alias_str = if alias.is_null() {
        None
    } else {
        let s = c_to_string(alias);
        if s.is_empty() {
            None
        } else {
            Some(s)
        }
    };
    let avatar_str = if avatar_path.is_null() {
        None
    } else {
        let s = c_to_string(avatar_path);
        if s.is_empty() {
            None
        } else {
            Some(s)
        }
    };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    use crate::types::{CreateRoomGuestAccess, CreateRoomHistoryVisibility};
    let ga = if guest_access == 1 {
        CreateRoomGuestAccess::CanJoin
    } else {
        CreateRoomGuestAccess::Forbidden
    };
    let hv = match history_visibility {
        1 => CreateRoomHistoryVisibility::Invited,
        2 => CreateRoomHistoryVisibility::Shared,
        3 => CreateRoomHistoryVisibility::WorldReadable,
        _ => CreateRoomHistoryVisibility::Joined,
    };

    let request = CreateRoomRequest {
        name: name_str,
        topic: topic_str,
        is_public,
        encrypted,
        alias: alias_str,
        avatar_path: avatar_str,
        guest_access: ga,
        history_visibility: hv,
        federate,
    };

    handle.runtime.spawn(async move {
        match client.create_room(request).await {
            Ok(room_id) => {
                let c_room_id = CString::new(room_id).unwrap_or_default();
                callback(true, c_room_id.as_ptr(), ud.as_ptr());
            }
            Err(_) => {
                callback(false, ptr::null(), ud.as_ptr());
            }
        }
    });
}

/// Upload and set a room avatar. Returns the new mxc:// URL in the send callback data field.
#[no_mangle]
pub unsafe extern "C" fn tm_upload_room_avatar(
    h: *mut Handle,
    room_id: *const c_char,
    data: *const u8,
    len: usize,
    content_type: *const c_char,
    callback: TmSendCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let mime_str = c_to_string(content_type);
    // Guard like the sibling upload FFI: from_raw_parts is UB with a null pointer
    // even at len 0. See code-review-2026-07-19 R4-3.
    let bytes = if data.is_null() || len == 0 {
        Vec::new()
    } else {
        unsafe { std::slice::from_raw_parts(data, len) }.to_vec()
    };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client
            .upload_room_avatar(&room_id_str, bytes, &mime_str)
            .await
        {
            Ok(mxc_url) => {
                let c_url = CString::new(mxc_url).unwrap_or_default();
                callback(true, c_url.as_ptr(), ud.as_ptr());
            }
            Err(_) => {
                callback(false, ptr::null(), ud.as_ptr());
            }
        }
    });
}

/// Delete a room avatar by sending an empty m.room.avatar state event.
#[no_mangle]
pub unsafe extern "C" fn tm_delete_room_avatar(
    h: *mut Handle,
    room_id: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let ok = client.delete_room_avatar(&room_id_str).await.is_ok();
        callback(ok, ud.as_ptr());
    });
}

// --- Account settings FFI types ---

/// C-compatible account capabilities.
#[repr(C)]
pub struct FfiAccountCapabilities {
    pub can_change_password: bool,
    pub can_change_3pid: bool,
    pub can_set_display_name: bool,
    pub can_set_avatar_url: bool,
}

/// C-compatible account summary.
#[repr(C)]
pub struct FfiAccountSummary {
    pub user_id: *mut c_char,
    pub display_name: *mut c_char,
    pub avatar_url: *mut c_char,
    pub capabilities: FfiAccountCapabilities,
}

/// C-compatible third-party identifier.
#[repr(C)]
pub struct FfiThreePid {
    pub medium: u32, // 0=email, 1=msisdn
    pub address: *mut c_char,
    pub validated_at: u64,
    pub added_at: u64,
}

/// C-compatible 3PID list.
#[repr(C)]
pub struct FfiThreePidList {
    pub items: *mut FfiThreePid,
    pub len: usize,
}

/// C-compatible 3PID token response.
#[repr(C)]
pub struct FfiThreePidTokenResponse {
    pub sid: *mut c_char,
    pub submit_url: *mut c_char,
}

/// C-compatible account action result (UIA-aware).
#[repr(C)]
pub struct FfiAccountActionResult {
    pub completed: bool,
    pub error_message: *mut c_char,
    pub uia_session: *mut c_char,
    pub uia_flows_json: *mut c_char,
}

/// Callback for account summary result.
pub type TmAccountSummaryCallback = extern "C" fn(
    success: bool,
    summary: FfiAccountSummary,
    error: *const c_char,
    userdata: *mut libc::c_void,
);

/// Callback for 3PID list result.
pub type TmThreePidListCallback = extern "C" fn(
    success: bool,
    list: FfiThreePidList,
    error: *const c_char,
    userdata: *mut libc::c_void,
);

/// Callback for 3PID token result.
pub type TmThreePidTokenCallback = extern "C" fn(
    success: bool,
    response: FfiThreePidTokenResponse,
    error: *const c_char,
    userdata: *mut libc::c_void,
);

/// Callback for account action result (UIA-aware).
pub type TmAccountActionCallback =
    extern "C" fn(result: FfiAccountActionResult, userdata: *mut libc::c_void);

// --- Account settings FFI exports ---

/// Asynchronously get the account summary.
#[no_mangle]
pub unsafe extern "C" fn tm_get_account_summary(
    h: *mut Handle,
    callback: TmAccountSummaryCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client.get_account_summary().await {
            Ok(summary) => {
                let ffi = FfiAccountSummary {
                    user_id: str_to_c(&summary.user_id),
                    display_name: str_to_c(&summary.display_name),
                    avatar_url: opt_str_to_c(&summary.avatar_url),
                    capabilities: FfiAccountCapabilities {
                        can_change_password: summary.capabilities.can_change_password,
                        can_change_3pid: summary.capabilities.can_change_3pid,
                        can_set_display_name: summary.capabilities.can_set_display_name,
                        can_set_avatar_url: summary.capabilities.can_set_avatar_url,
                    },
                };
                callback(true, ffi, ptr::null(), ud.as_ptr());
            }
            Err(e) => {
                let err_msg = CString::new(format!("{e}")).unwrap_or_default();
                let empty = FfiAccountSummary {
                    user_id: ptr::null_mut(),
                    display_name: ptr::null_mut(),
                    avatar_url: ptr::null_mut(),
                    capabilities: FfiAccountCapabilities {
                        can_change_password: true,
                        can_change_3pid: true,
                        can_set_display_name: true,
                        can_set_avatar_url: true,
                    },
                };
                callback(false, empty, err_msg.as_ptr(), ud.as_ptr());
            }
        }
    });
}

/// Free an account summary returned via callback.
#[no_mangle]
pub unsafe extern "C" fn tm_free_account_summary(summary: FfiAccountSummary) {
    unsafe {
        free_c_string(summary.user_id);
        free_c_string(summary.display_name);
        free_c_string(summary.avatar_url);
    }
}

/// Asynchronously set the display name.
#[no_mangle]
pub unsafe extern "C" fn tm_set_display_name(
    h: *mut Handle,
    name: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let name_str = c_to_string(name);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let ok = client.set_display_name(&name_str).await.is_ok();
        callback(ok, ud.as_ptr());
    });
}

/// Asynchronously set the avatar URL (pass null to remove).
#[no_mangle]
pub unsafe extern "C" fn tm_set_avatar_url(
    h: *mut Handle,
    mxc_url: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let url = opt_c_to_string(mxc_url);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let ok = client.set_avatar_url(url.as_deref()).await.is_ok();
        callback(ok, ud.as_ptr());
    });
}

/// Asynchronously upload avatar data and set it.
/// `data` and `data_len` describe the image bytes.
/// `content_type` is the MIME type (e.g. "image/png").
/// On success, the callback receives the new mxc:// URL as event_id.
#[no_mangle]
pub unsafe extern "C" fn tm_upload_avatar_and_set(
    h: *mut Handle,
    data: *const u8,
    data_len: usize,
    content_type: *const c_char,
    callback: TmSendCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let bytes = if data.is_null() || data_len == 0 {
        Vec::new()
    } else {
        unsafe { std::slice::from_raw_parts(data, data_len) }.to_vec()
    };
    let mime_str = c_to_string(content_type);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client.upload_avatar(bytes, &mime_str).await {
            Ok(mxc_url) => {
                let url_c = CString::new(mxc_url).unwrap_or_default();
                callback(true, url_c.as_ptr(), ud.as_ptr());
            }
            Err(_) => callback(false, ptr::null(), ud.as_ptr()),
        }
    });
}

/// Asynchronously get the list of 3PIDs.
#[no_mangle]
pub unsafe extern "C" fn tm_get_3pids(
    h: *mut Handle,
    callback: TmThreePidListCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client.get_3pids().await {
            Ok(pids) => {
                let ffi_pids: Vec<FfiThreePid> = pids
                    .iter()
                    .map(|p| FfiThreePid {
                        medium: p.medium as u32,
                        address: str_to_c(&p.address),
                        validated_at: p.validated_at.unwrap_or(0),
                        added_at: p.added_at.unwrap_or(0),
                    })
                    .collect();
                let len = ffi_pids.len();
                let items_ptr = if len > 0 {
                    leak_boxed_slice(ffi_pids)
                } else {
                    ptr::null_mut()
                };
                let list = FfiThreePidList {
                    items: items_ptr,
                    len,
                };
                callback(true, list, ptr::null(), ud.as_ptr());
            }
            Err(e) => {
                let err_msg = CString::new(format!("{e}")).unwrap_or_default();
                let empty = FfiThreePidList {
                    items: ptr::null_mut(),
                    len: 0,
                };
                callback(false, empty, err_msg.as_ptr(), ud.as_ptr());
            }
        }
    });
}

/// Free a 3PID list returned via callback.
#[no_mangle]
pub unsafe extern "C" fn tm_free_3pid_list(list: FfiThreePidList) {
    if !list.items.is_null() && list.len > 0 {
        let items = unsafe { reclaim_boxed_slice(list.items, list.len) };
        for item in items {
            unsafe {
                free_c_string(item.address);
            }
        }
    }
}

/// Asynchronously request a 3PID verification token.
#[no_mangle]
pub unsafe extern "C" fn tm_request_3pid_token(
    h: *mut Handle,
    medium: u32,
    address: *const c_char,
    country: *const c_char,
    client_secret: *const c_char,
    send_attempt: u32,
    callback: TmThreePidTokenCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let medium_enum = if medium == 0 {
        ThreePidMedium::Email
    } else {
        ThreePidMedium::Msisdn
    };
    let addr = c_to_string(address);
    let country = c_to_string(country);
    let secret = c_to_string(client_secret);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client
            .request_3pid_token(medium_enum, &addr, &country, &secret, send_attempt)
            .await
        {
            Ok(resp) => {
                let ffi = FfiThreePidTokenResponse {
                    sid: str_to_c(&resp.sid),
                    submit_url: opt_str_to_c(&resp.submit_url),
                };
                callback(true, ffi, ptr::null(), ud.as_ptr());
            }
            Err(e) => {
                let err_msg = CString::new(format!("{e}")).unwrap_or_default();
                let empty = FfiThreePidTokenResponse {
                    sid: ptr::null_mut(),
                    submit_url: ptr::null_mut(),
                };
                callback(false, empty, err_msg.as_ptr(), ud.as_ptr());
            }
        }
    });
}

/// Free a 3PID token response returned via callback.
#[no_mangle]
pub unsafe extern "C" fn tm_free_3pid_token_response(resp: FfiThreePidTokenResponse) {
    unsafe {
        free_c_string(resp.sid);
        free_c_string(resp.submit_url);
    }
}

/// Asynchronously add a 3PID to the account (after verification token was sent).
#[no_mangle]
pub unsafe extern "C" fn tm_add_3pid(
    h: *mut Handle,
    client_secret: *const c_char,
    sid: *const c_char,
    auth_json: *const c_char,
    callback: TmAccountActionCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let secret = c_to_string(client_secret);
    let session_id = c_to_string(sid);
    let auth = if auth_json.is_null() {
        None
    } else {
        Some(c_to_string(auth_json))
    };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let auth_ref = auth.as_deref();
        match client.add_3pid(&secret, &session_id, auth_ref).await {
            Ok(result) => {
                let ffi = FfiAccountActionResult {
                    completed: result.completed,
                    error_message: opt_str_to_c(&result.error_message),
                    uia_session: opt_str_to_c(&result.uia_session),
                    uia_flows_json: opt_str_to_c(&result.uia_flows_json),
                };
                callback(ffi, ud.as_ptr());
            }
            Err(e) => {
                let ffi = FfiAccountActionResult {
                    completed: false,
                    error_message: str_to_c(&format!("{e}")),
                    uia_session: ptr::null_mut(),
                    uia_flows_json: ptr::null_mut(),
                };
                callback(ffi, ud.as_ptr());
            }
        }
    });
}

/// Asynchronously delete a 3PID (email or phone) from the account.
#[no_mangle]
pub unsafe extern "C" fn tm_delete_3pid(
    h: *mut Handle,
    medium: u32,
    address: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let medium_enum = if medium == 0 {
        ThreePidMedium::Email
    } else {
        ThreePidMedium::Msisdn
    };
    let addr = c_to_string(address);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client.delete_3pid(medium_enum, &addr).await {
            Ok(()) => {
                callback(true, ud.as_ptr());
            }
            Err(e) => {
                warn!("tm_delete_3pid error: {e}");
                callback(false, ud.as_ptr());
            }
        }
    });
}

/// Asynchronously change the account password.
#[no_mangle]
pub unsafe extern "C" fn tm_change_password(
    h: *mut Handle,
    new_password: *const c_char,
    auth_json: *const c_char,
    callback: TmAccountActionCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let pw = c_to_string(new_password);
    let auth = opt_c_to_string(auth_json);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client.change_password(&pw, auth.as_deref()).await {
            Ok(result) => {
                let ffi = FfiAccountActionResult {
                    completed: result.completed,
                    error_message: opt_str_to_c(&result.error_message),
                    uia_session: opt_str_to_c(&result.uia_session),
                    uia_flows_json: opt_str_to_c(&result.uia_flows_json),
                };
                callback(ffi, ud.as_ptr());
            }
            Err(e) => {
                let ffi = FfiAccountActionResult {
                    completed: false,
                    error_message: str_to_c(&format!("{e}")),
                    uia_session: ptr::null_mut(),
                    uia_flows_json: ptr::null_mut(),
                };
                callback(ffi, ud.as_ptr());
            }
        }
    });
}

/// Asynchronously deactivate the account.
#[no_mangle]
pub unsafe extern "C" fn tm_deactivate_account(
    h: *mut Handle,
    erase_data: bool,
    auth_json: *const c_char,
    callback: TmAccountActionCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let auth = opt_c_to_string(auth_json);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client.deactivate_account(erase_data, auth.as_deref()).await {
            Ok(result) => {
                let ffi = FfiAccountActionResult {
                    completed: result.completed,
                    error_message: opt_str_to_c(&result.error_message),
                    uia_session: opt_str_to_c(&result.uia_session),
                    uia_flows_json: opt_str_to_c(&result.uia_flows_json),
                };
                callback(ffi, ud.as_ptr());
            }
            Err(e) => {
                let ffi = FfiAccountActionResult {
                    completed: false,
                    error_message: str_to_c(&format!("{e}")),
                    uia_session: ptr::null_mut(),
                    uia_flows_json: ptr::null_mut(),
                };
                callback(ffi, ud.as_ptr());
            }
        }
    });
}

/// Free an account action result returned via callback.
#[no_mangle]
pub unsafe extern "C" fn tm_free_account_action_result(result: FfiAccountActionResult) {
    unsafe {
        free_c_string(result.error_message);
        free_c_string(result.uia_session);
        free_c_string(result.uia_flows_json);
    }
}

// --- Sessions + Encryption FFI types ---

/// C-compatible device/session info.
#[repr(C)]
pub struct FfiDeviceSession {
    pub device_id: *mut c_char,
    pub display_name: *mut c_char,
    pub is_current: bool,
    pub is_dehydrated: bool,
    pub last_seen_ts: u64,
    pub has_last_seen_ts: bool,
    pub last_seen_ip: *mut c_char,
    pub last_seen_user_agent: *mut c_char,
    pub app_name: *mut c_char,
    pub app_version: *mut c_char,
    pub device_model: *mut c_char,
    pub os: *mut c_char,
    pub browser: *mut c_char,
    pub verification_state: u32,
}

/// C-compatible device session list.
#[repr(C)]
pub struct FfiDeviceSessionList {
    pub current_device_id: *mut c_char,
    pub sessions: *mut FfiDeviceSession,
    pub sessions_len: usize,
}

/// C-compatible result for device deletion (may include UIA challenge).
#[repr(C)]
pub struct FfiDeleteDevicesResult {
    pub completed: bool,
    pub challenge_json: *mut c_char,
    /// Web account-management URL for MAS/OAuth servers that reject the legacy
    /// device-delete endpoint; null otherwise.
    pub account_management_url: *mut c_char,
}

/// C-compatible encryption overview snapshot.
#[repr(C)]
pub struct FfiEncryptionOverview {
    pub device_id: *mut c_char,
    pub device_ed25519: *mut c_char,
    pub is_current_device_verified: bool,
    pub cross_signing_ready: bool,
    pub cross_signing_keys_cached_locally: bool,
    pub cross_signing_keys_in_secret_storage: bool,
    pub secret_storage_ready: bool,
    pub secret_storage_default_key_id: *mut c_char,
    pub key_backup_upload_active: bool,
    pub backup_key_cached: bool,
    pub backup_key_stored_in_4s: bool,
    pub backup_disabled_account_flag: bool,
    pub recovery_disabled_account_flag: bool,
    pub history_decryptable: bool,
    pub health_state: u32,
}

/// C-compatible result for identity reset.
#[repr(C)]
pub struct FfiResetIdentityResult {
    pub completed: bool,
    pub challenge_json: *mut c_char,
}

/// C-compatible result for key import.
#[repr(C)]
pub struct FfiImportKeysResult {
    pub imported_count: u32,
    pub total_count: u32,
}

/// Callback type for own-devices list.
pub type TmDeviceListCallback =
    extern "C" fn(success: bool, list: FfiDeviceSessionList, userdata: *mut libc::c_void);

/// Callback type for device deletion result.
pub type TmDeleteDevicesCallback =
    extern "C" fn(success: bool, result: FfiDeleteDevicesResult, userdata: *mut libc::c_void);

/// Callback type for encryption overview.
pub type TmEncryptionOverviewCallback =
    extern "C" fn(success: bool, overview: FfiEncryptionOverview, userdata: *mut libc::c_void);

/// Callback type for recovery key creation (returns the generated key string).
pub type TmRecoveryKeyCallback =
    extern "C" fn(success: bool, recovery_key: *const c_char, userdata: *mut libc::c_void);

/// Callback type for provisioning recovery. On success `recovery_key` is the new key and
/// `error_code` is 0; on failure `recovery_key` is null and `error_code` says what went wrong
/// (1 = an unusable key backup already exists on the homeserver, 2 = anything else). Both
/// strings are only valid for the duration of the call.
pub type TmRecoverySetupCallback = extern "C" fn(
    success: bool,
    recovery_key: *const c_char,
    error_code: u32,
    error: *const c_char,
    userdata: *mut libc::c_void,
);

/// Callback type for identity reset result.
pub type TmResetIdentityCallback =
    extern "C" fn(success: bool, result: FfiResetIdentityResult, userdata: *mut libc::c_void);

/// Callback type for E2E key import result.
pub type TmImportKeysCallback =
    extern "C" fn(success: bool, result: FfiImportKeysResult, userdata: *mut libc::c_void);

// --- Sessions + Encryption FFI functions ---

/// Get the list of own devices/sessions asynchronously.
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_get_own_devices(
    h: *mut Handle,
    callback: TmDeviceListCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client.get_own_devices().await {
            Ok(list) => {
                let ffi_sessions: Vec<FfiDeviceSession> = list
                    .sessions
                    .iter()
                    .map(|s| FfiDeviceSession {
                        device_id: str_to_c(&s.device_id),
                        display_name: opt_str_to_c(&s.display_name),
                        is_current: s.is_current,
                        is_dehydrated: s.is_dehydrated,
                        last_seen_ts: s.last_seen_ts.unwrap_or(0),
                        has_last_seen_ts: s.last_seen_ts.is_some(),
                        last_seen_ip: opt_str_to_c(&s.last_seen_ip),
                        last_seen_user_agent: opt_str_to_c(&s.last_seen_user_agent),
                        app_name: opt_str_to_c(&s.app_name),
                        app_version: opt_str_to_c(&s.app_version),
                        device_model: opt_str_to_c(&s.device_model),
                        os: opt_str_to_c(&s.os),
                        browser: opt_str_to_c(&s.browser),
                        verification_state: s.verification_state as u32,
                    })
                    .collect();
                let len = ffi_sessions.len();
                let ptr = leak_boxed_slice(ffi_sessions);

                let ffi_list = FfiDeviceSessionList {
                    current_device_id: str_to_c(&list.current_device_id),
                    sessions: ptr,
                    sessions_len: len,
                };
                callback(true, ffi_list, ud.as_ptr());
            }
            Err(_) => {
                let empty = FfiDeviceSessionList {
                    current_device_id: ptr::null_mut(),
                    sessions: ptr::null_mut(),
                    sessions_len: 0,
                };
                callback(false, empty, ud.as_ptr());
            }
        }
    });
}

/// Free a device session list returned via callback.
///
/// # Safety
/// `list` must have been received from a `TmDeviceListCallback`.
#[no_mangle]
pub unsafe extern "C" fn tm_free_device_session_list(list: FfiDeviceSessionList) {
    unsafe { free_c_string(list.current_device_id) };
    if !list.sessions.is_null() && list.sessions_len > 0 {
        let sessions = unsafe { reclaim_boxed_slice(list.sessions, list.sessions_len) };
        for s in sessions {
            unsafe {
                free_c_string(s.device_id);
                free_c_string(s.display_name);
                free_c_string(s.last_seen_ip);
                free_c_string(s.last_seen_user_agent);
                free_c_string(s.app_name);
                free_c_string(s.app_version);
                free_c_string(s.device_model);
                free_c_string(s.os);
                free_c_string(s.browser);
            }
        }
    }
}

/// Rename a device's display name asynchronously.
///
/// # Safety
/// `h` must be a valid Handle pointer.
/// `device_id` and `display_name` must be valid C strings.
#[no_mangle]
pub unsafe extern "C" fn tm_rename_device(
    h: *mut Handle,
    device_id: *const c_char,
    display_name: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);
    let dev_id = c_to_string(device_id);
    let name = c_to_string(display_name);

    handle.runtime.spawn(async move {
        let success = client.rename_device(&dev_id, &name).await.is_ok();
        callback(success, ud.as_ptr());
    });
}

/// Delete one or more devices asynchronously.
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_delete_devices(
    h: *mut Handle,
    device_ids: *const *const c_char,
    device_ids_len: usize,
    auth_json: *const c_char,
    callback: TmDeleteDevicesCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    let ids: Vec<String> = (0..device_ids_len)
        .map(|i| c_to_string(unsafe { *device_ids.add(i) }))
        .collect();
    let auth = if auth_json.is_null() {
        String::new()
    } else {
        c_to_string(auth_json)
    };

    handle.runtime.spawn(async move {
        match client.delete_devices(&ids, &auth).await {
            Ok(result) => {
                let ffi_result = FfiDeleteDevicesResult {
                    completed: result.completed,
                    challenge_json: opt_str_to_c(&result.challenge_json),
                    account_management_url: opt_str_to_c(&result.account_management_url),
                };
                callback(true, ffi_result, ud.as_ptr());
            }
            Err(_) => {
                let ffi_result = FfiDeleteDevicesResult {
                    completed: false,
                    challenge_json: ptr::null_mut(),
                    account_management_url: ptr::null_mut(),
                };
                callback(false, ffi_result, ud.as_ptr());
            }
        }
    });
}

/// Free a delete-devices result.
///
/// # Safety
/// `result` must have been received from a `TmDeleteDevicesCallback`.
#[no_mangle]
pub unsafe extern "C" fn tm_free_delete_devices_result(result: FfiDeleteDevicesResult) {
    unsafe { free_c_string(result.challenge_json) };
    unsafe { free_c_string(result.account_management_url) };
}

/// Get encryption overview/health snapshot asynchronously.
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_get_encryption_overview(
    h: *mut Handle,
    callback: TmEncryptionOverviewCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client.get_encryption_overview().await {
            Ok(ov) => {
                let ffi = FfiEncryptionOverview {
                    device_id: str_to_c(&ov.device_id),
                    device_ed25519: opt_str_to_c(&ov.device_ed25519),
                    is_current_device_verified: ov.is_current_device_verified,
                    cross_signing_ready: ov.cross_signing_ready,
                    cross_signing_keys_cached_locally: ov.cross_signing_keys_cached_locally,
                    cross_signing_keys_in_secret_storage: ov.cross_signing_keys_in_secret_storage,
                    secret_storage_ready: ov.secret_storage_ready,
                    secret_storage_default_key_id: opt_str_to_c(&ov.secret_storage_default_key_id),
                    key_backup_upload_active: ov.key_backup_upload_active,
                    backup_key_cached: ov.backup_key_cached,
                    backup_key_stored_in_4s: ov.backup_key_stored_in_4s,
                    backup_disabled_account_flag: ov.backup_disabled_account_flag,
                    recovery_disabled_account_flag: ov.recovery_disabled_account_flag,
                    history_decryptable: ov.history_decryptable,
                    health_state: ov.health_state as u32,
                };
                callback(true, ffi, ud.as_ptr());
            }
            Err(_) => {
                let empty = FfiEncryptionOverview {
                    device_id: ptr::null_mut(),
                    device_ed25519: ptr::null_mut(),
                    is_current_device_verified: false,
                    cross_signing_ready: false,
                    cross_signing_keys_cached_locally: false,
                    cross_signing_keys_in_secret_storage: false,
                    secret_storage_ready: false,
                    secret_storage_default_key_id: ptr::null_mut(),
                    key_backup_upload_active: false,
                    backup_key_cached: false,
                    backup_key_stored_in_4s: false,
                    backup_disabled_account_flag: false,
                    recovery_disabled_account_flag: false,
                    history_decryptable: false,
                    health_state: EncryptionHealthState::Ok as u32,
                };
                callback(false, empty, ud.as_ptr());
            }
        }
    });
}

/// Free an encryption overview.
///
/// # Safety
/// `overview` must have been received from a `TmEncryptionOverviewCallback`.
#[no_mangle]
pub unsafe extern "C" fn tm_free_encryption_overview(overview: FfiEncryptionOverview) {
    unsafe {
        free_c_string(overview.device_id);
        free_c_string(overview.device_ed25519);
        free_c_string(overview.secret_storage_default_key_id);
    }
}

/// Enable or disable key storage asynchronously.
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_set_key_storage_enabled(
    h: *mut Handle,
    enabled: bool,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let success = client.set_key_storage_enabled(enabled).await.is_ok();
        callback(success, ud.as_ptr());
    });
}

/// Submit a recovery key to unlock secret storage.
///
/// # Safety
/// `h` must be a valid Handle pointer.
/// `recovery_key` must be a valid C string.
#[no_mangle]
pub unsafe extern "C" fn tm_enter_recovery_key(
    h: *mut Handle,
    recovery_key: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);
    let key = c_to_string(recovery_key);

    handle.runtime.spawn(async move {
        let success = client.enter_recovery_key(&key).await.is_ok();
        callback(success, ud.as_ptr());
    });
}

/// Create a new recovery key asynchronously.
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_create_recovery_key(
    h: *mut Handle,
    callback: TmRecoveryKeyCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client.create_recovery_key().await {
            Ok(key) => {
                let c_key = str_to_c(&key);
                callback(true, c_key, ud.as_ptr());
            }
            Err(_) => {
                callback(false, ptr::null(), ud.as_ptr());
            }
        }
    });
}

/// Provision recovery (key backup + secret storage) on an account that has none, and hand back
/// the recovery key. This is the only moment the key exists in plaintext, so the caller must show
/// it to the user.
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_setup_recovery(
    h: *mut Handle,
    callback: TmRecoverySetupCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        deliver_recovery_setup(client.setup_recovery().await, callback, &ud);
    });
}

/// Replace an unusable server-side key backup with a fresh backup and recovery key. Destructive:
/// only call this once the user has confirmed, since it deletes the existing backup version.
///
/// # Safety
/// `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_reset_recovery(
    h: *mut Handle,
    callback: TmRecoverySetupCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        deliver_recovery_setup(client.reset_recovery().await, callback, &ud);
    });
}

/// Hand a recovery-setup outcome to C++, freeing the temporary C strings once it has copied them.
fn deliver_recovery_setup(
    outcome: std::result::Result<String, RecoverySetupError>,
    callback: TmRecoverySetupCallback,
    ud: &Userdata,
) {
    match outcome {
        Ok(key) => {
            let c_key = str_to_c(&key);
            callback(true, c_key, 0, ptr::null(), ud.as_ptr());
            unsafe { free_c_string(c_key) };
        }
        Err(error) => {
            let c_error = str_to_c(&error.message());
            callback(false, ptr::null(), error.code(), c_error, ud.as_ptr());
            unsafe { free_c_string(c_error) };
        }
    }
}

/// Commit (finalize) a new recovery key after user confirmation.
///
/// # Safety
/// `h` must be a valid Handle pointer.
/// `recovery_key` must be a valid C string.
#[no_mangle]
pub unsafe extern "C" fn tm_commit_recovery_key(
    h: *mut Handle,
    recovery_key: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);
    let key = c_to_string(recovery_key);

    handle.runtime.spawn(async move {
        let success = client.commit_recovery_key(&key).await.is_ok();
        callback(success, ud.as_ptr());
    });
}

/// Reset cryptographic identity asynchronously.
///
/// # Safety
/// `h` must be a valid Handle pointer.
/// `auth_json` may be null for first attempt.
#[no_mangle]
pub unsafe extern "C" fn tm_reset_identity(
    h: *mut Handle,
    auth_json: *const c_char,
    callback: TmResetIdentityCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);
    let auth = if auth_json.is_null() {
        String::new()
    } else {
        c_to_string(auth_json)
    };

    handle.runtime.spawn(async move {
        match client.reset_identity(&auth).await {
            Ok(result) => {
                let ffi_result = FfiResetIdentityResult {
                    completed: result.completed,
                    challenge_json: opt_str_to_c(&result.challenge_json),
                };
                callback(true, ffi_result, ud.as_ptr());
            }
            Err(_) => {
                let ffi_result = FfiResetIdentityResult {
                    completed: false,
                    challenge_json: ptr::null_mut(),
                };
                callback(false, ffi_result, ud.as_ptr());
            }
        }
    });
}

/// Free a reset-identity result.
///
/// # Safety
/// `result` must have been received from a `TmResetIdentityCallback`.
#[no_mangle]
pub unsafe extern "C" fn tm_free_reset_identity_result(result: FfiResetIdentityResult) {
    unsafe { free_c_string(result.challenge_json) };
}

/// Export E2E encryption keys to a file.
///
/// # Safety
/// `h` must be a valid Handle pointer.
/// `path` and `passphrase` must be valid C strings.
#[no_mangle]
pub unsafe extern "C" fn tm_export_e2e_keys(
    h: *mut Handle,
    path: *const c_char,
    passphrase: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);
    let p = c_to_string(path);
    let pp = c_to_string(passphrase);

    handle.runtime.spawn(async move {
        let success = client.export_e2e_keys(&p, &pp).await.is_ok();
        callback(success, ud.as_ptr());
    });
}

/// Import E2E encryption keys from a file.
///
/// # Safety
/// `h` must be a valid Handle pointer.
/// `path` and `passphrase` must be valid C strings.
#[no_mangle]
pub unsafe extern "C" fn tm_import_e2e_keys(
    h: *mut Handle,
    path: *const c_char,
    passphrase: *const c_char,
    callback: TmImportKeysCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);
    let p = c_to_string(path);
    let pp = c_to_string(passphrase);

    handle.runtime.spawn(async move {
        match client.import_e2e_keys(&p, &pp).await {
            Ok(result) => {
                let ffi_result = FfiImportKeysResult {
                    imported_count: result.imported_count,
                    total_count: result.total_count,
                };
                callback(true, ffi_result, ud.as_ptr());
            }
            Err(_) => {
                let ffi_result = FfiImportKeysResult {
                    imported_count: 0,
                    total_count: 0,
                };
                callback(false, ffi_result, ud.as_ptr());
            }
        }
    });
}

// --- Timeline navigation API ---

/// Get a timeline slice for a room asynchronously.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `room_id` must be a valid null-terminated C string.
/// - Caller must call `tm_free_timeline_slice` on the FfiTimelineSlice delivered to the callback.
#[no_mangle]
pub unsafe extern "C" fn tm_get_timeline_slice_async(
    h: *mut Handle,
    room_id: *const c_char,
    callback: TmTimelineSliceCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client.get_timeline_slice(&room_id_str).await {
            Ok(slice) => callback(true, timeline_slice_to_ffi(&slice), ud.as_ptr()),
            Err(_) => callback(false, empty_timeline_slice(), ud.as_ptr()),
        }
    });
}

/// Get the latest incremental timeline update for a room asynchronously.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `room_id` must be a valid null-terminated C string.
/// - Caller must call `tm_free_timeline_slice` on the FfiTimelineSlice delivered to the callback.
#[no_mangle]
pub unsafe extern "C" fn tm_get_timeline_update_async(
    h: *mut Handle,
    room_id: *const c_char,
    callback: TmTimelineSliceCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match client.get_timeline_update(&room_id_str).await {
            Ok(slice) => callback(true, timeline_slice_to_ffi(&slice), ud.as_ptr()),
            Err(_) => callback(false, empty_timeline_slice(), ud.as_ptr()),
        }
    });
}

/// Free a timeline slice returned by `tm_get_timeline_slice`.
///
/// # Safety
/// `slice` must have been returned by `tm_get_timeline_slice` and must not be used after this call.
#[no_mangle]
pub unsafe extern "C" fn tm_free_timeline_slice(slice: FfiTimelineSlice) {
    if !slice.items.is_null() && slice.items_count > 0 {
        let items = unsafe { reclaim_boxed_slice(slice.items, slice.items_count as usize) };
        for item in items {
            unsafe {
                free_c_string(item.event_id);
                free_c_string(item.transaction_id);
                free_c_string(item.sender_user_id);
                free_c_string(item.sender_display_name);
                free_c_string(item.sender_avatar_url);
                free_c_string(item.body);
                free_c_string(item.formatted_body);
                free_c_string(item.media_url);
                free_c_string(item.media_mime);
                free_c_string(item.media_filename);
                free_c_string(item.media_caption);
                free_c_string(item.media_thumb_url);
                free_c_string(item.media_blurhash);
                free_c_string(item.reply_to_event_id);
                free_c_string(item.reply_preview_sender_name);
                free_c_string(item.reply_preview_text);
                free_c_string(item.reply_preview_thumb_url);
                free_c_string(item.forwarded_from_sender_name);
                free_c_string(item.forwarded_from_sender_id);
                free_c_string(item.forwarded_from_avatar_url);
                free_c_string(item.reactions);
                free_c_string(item.url_preview_url);
                free_c_string(item.url_preview_site_name);
                free_c_string(item.url_preview_title);
                free_c_string(item.url_preview_description);
                free_c_string(item.url_preview_image_url);
                free_c_string(item.url_preview_author);
                free_c_string(item.url_preview_site_name_canonical);
                free_c_string(item.decryption_error);
                free_c_string(item.audio_waveform_json);
                free_c_string(item.poll_question);
                free_c_string(item.poll_subtitle);
                free_c_string(item.poll_options_json);
            }
        }
    }
    if !slice.focus_event_id.is_null() {
        unsafe {
            drop(CString::from_raw(slice.focus_event_id));
        }
    }
    if !slice.pinned_event_ids.is_null() && slice.pinned_event_ids_count > 0 {
        let ptrs = unsafe {
            reclaim_boxed_slice(
                slice.pinned_event_ids,
                slice.pinned_event_ids_count as usize,
            )
        };
        for ptr in ptrs {
            if !ptr.is_null() {
                unsafe {
                    drop(CString::from_raw(ptr));
                }
            }
        }
    }
    free_c_string(slice.first_unread_event_id);
}

/// Request backward pagination for a room (non-blocking fire-and-forget).
///
/// Fetch pinned messages for a room (blocking, fetches from server).
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `room_id` must be a valid null-terminated C string.
/// - Caller must call `tm_free_timeline` on the returned `FfiTimeline`.
/// Persist a locally-learned audio duration (milliseconds) for a media mxc URL.
/// Fire-and-forget; the value is stored in the matrix-sdk state store and reused
/// to fill the duration of audio events whose metadata omits it.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `mxc` must be a valid null-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn tm_set_audio_duration(
    h: *mut Handle,
    mxc: *const c_char,
    duration_ms: u64,
) {
    let handle = unsafe { &*h };
    let mxc_str = c_to_string(mxc);
    if mxc_str.is_empty() || duration_ms == 0 {
        return;
    }
    let client = handle.client.clone();
    handle.runtime.spawn(async move {
        let _ = client.set_audio_duration(&mxc_str, duration_ms).await;
    });
}

/// This session's homeserver max upload size in bytes, or 0 if not yet known.
/// Read from a per-session cache populated once at sync start; safe to call
/// synchronously (no blocking).
///
/// # Safety
/// - `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_max_upload_size(h: *mut Handle) -> u64 {
    let handle = unsafe { &*h };
    handle.matrix.max_upload_size()
}

/// Upload-progress callback: transaction_id, bytes sent so far, total bytes,
/// userdata.
pub type TmUploadProgressCallback =
    extern "C" fn(txn_id: *const c_char, current: u64, total: u64, userdata: *mut libc::c_void);

/// Install this session's media-upload progress callback. Invoked from a
/// runtime thread with byte progress for DIRECT uploads (which bypass the send
/// queue and so don't surface progress on a timeline item), keyed by transaction
/// id so the UI can update the matching optimistic echo. The C++ side marshals
/// to the main thread.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_set_upload_progress_callback(
    h: *mut Handle,
    callback: TmUploadProgressCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud_bits = userdata as usize;
    handle
        .matrix
        .on_upload_progress(Box::new(move |txn_id, current, total| {
            if let Ok(c_txn) = CString::new(txn_id) {
                callback(c_txn.as_ptr(), current, total, ud_bits as *mut libc::c_void);
            }
        }));
}

/// Recent-emoji change callback: JSON array string of `[emoji, count]` pairs,
/// userdata.
pub type TmRecentEmojiCallback =
    extern "C" fn(json_pairs: *const c_char, userdata: *mut libc::c_void);

/// Install this session's recent-emoji callback. Invoked from a runtime thread
/// whenever the server's `io.element.recent_emoji` account data changes (startup
/// hydrate or cross-device update); the C++ side marshals to the main thread and
/// refreshes the picker's list.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_set_recent_emoji_callback(
    h: *mut Handle,
    callback: TmRecentEmojiCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let ud_bits = userdata as usize;
    handle.matrix.on_recent_emoji(Box::new(move |json| {
        if let Ok(c_json) = CString::new(json) {
            callback(c_json.as_ptr(), ud_bits as *mut libc::c_void);
        }
    }));
}

/// Persist the recent-emoji list to the server (and local cache). `json_pairs` is
/// a JSON array of `[emoji, count]` pairs (the full ordered list).
#[no_mangle]
pub unsafe extern "C" fn tm_set_recent_emoji(h: *mut Handle, json_pairs: *const c_char) {
    let handle = unsafe { &*h };
    let json = c_to_string(json_pairs);
    let pairs = crate::recent_emoji::from_json_pairs(&json);
    let matrix = handle.matrix.clone();
    handle.runtime.spawn(async move {
        if let Err(e) = matrix.set_recent_emoji(pairs).await {
            warn!("[recent-emoji] set failed: {e}");
        }
    });
}

/// Blocking read of the locally-cached recent emojis (app_cache.db) for instant
/// startup display. Returns a JSON array string of `[emoji, count]` pairs; the
/// caller must free it with `tm_free_string`.
#[no_mangle]
pub unsafe extern "C" fn tm_get_recent_emoji(h: *mut Handle) -> *mut c_char {
    let handle = unsafe { &*h };
    let pairs = handle.matrix.recent_emoji_local();
    str_to_c(&crate::recent_emoji::to_json_pairs(&pairs))
}

/// Callback type for tm_get_pinned_messages_async.
pub type TmPinnedMessagesCallback =
    extern "C" fn(timeline: FfiTimeline, userdata: *mut libc::c_void);

/// Fetch pinned messages asynchronously. Result arrives via callback.
#[no_mangle]
pub unsafe extern "C" fn tm_get_pinned_messages_async(
    h: *mut Handle,
    room_id: *const c_char,
    callback: TmPinnedMessagesCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let result = client.get_pinned_messages(&room_id_str).await;
        let timeline = match result {
            Ok(items) => {
                let (ptr, count) = timeline_items_to_ffi_raw(&items);
                FfiTimeline {
                    items: ptr,
                    len: count as usize,
                }
            }
            Err(_) => FfiTimeline {
                items: ptr::null_mut(),
                len: 0,
            },
        };
        callback(timeline, ud.as_ptr());
    });
}

/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `room_id` must be a valid null-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn tm_paginate_back(h: *mut Handle, room_id: *const c_char, count: u16) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    // Enter the Tokio runtime context so tokio::spawn works inside paginate_back
    let _guard = handle.runtime.enter();
    handle.client.paginate_back(&room_id_str, count);
}

/// Request forward pagination for a room (non-blocking fire-and-forget).
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `room_id` must be a valid null-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn tm_paginate_forward(h: *mut Handle, room_id: *const c_char, count: u16) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    // Enter the Tokio runtime context so tokio::spawn works inside paginate_forward
    let _guard = handle.runtime.enter();
    handle.client.paginate_forward(&room_id_str, count);
}

/// Callback type for `tm_focus_on_event` result.
pub type TmFocusOnEventCallback = extern "C" fn(success: bool, userdata: *mut std::ffi::c_void);

/// Jump the timeline for `room_id` to the given `event_id` asynchronously.
/// The callback is invoked on a background thread with `success = true` on success.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `room_id` and `event_id` must be valid null-terminated C strings.
/// - `callback` will be invoked on a background thread.
/// - `userdata` is passed through unchanged and must remain valid until the callback fires.
#[no_mangle]
pub unsafe extern "C" fn tm_focus_on_event(
    h: *mut Handle,
    room_id: *const c_char,
    event_id: *const c_char,
    callback: TmFocusOnEventCallback,
    userdata: *mut std::ffi::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let event_id_str = c_to_string(event_id);
    let client = handle.client.clone();
    let userdata_val = userdata as usize;

    handle.runtime.spawn(async move {
        let result = client.focus_on_event(&room_id_str, &event_id_str).await;
        let success = result.is_ok();
        if let Err(ref e) = result {
            tracing::error!("focus_on_event failed: {e}");
        }
        callback(success, userdata_val as *mut std::ffi::c_void);
    });
}

/// Return the timeline for `room_id` to the live end (non-blocking fire-and-forget).
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `room_id` must be a valid null-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn tm_return_to_live(h: *mut Handle, room_id: *const c_char) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    // Enter the Tokio runtime context so tokio::spawn works inside return_to_live
    let _guard = handle.runtime.enter();
    handle.client.return_to_live(&room_id_str);
}

/// Unpin all messages in a room (single state event).
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `room_id` must be a valid null-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn tm_unpin_all_messages(
    h: *mut Handle,
    room_id: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let client = handle.client.clone();
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let success = client.unpin_all_messages(&room_id_str).await.is_ok();
        callback(success, ud.as_ptr());
    });
}

/// Cancel an in-progress media upload by transaction ID.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `room_id` and `transaction_id` must be valid null-terminated C strings.
#[no_mangle]
pub unsafe extern "C" fn tm_cancel_upload(
    h: *mut Handle,
    room_id: *const c_char,
    transaction_id: *const c_char,
) {
    let handle = unsafe { &*h };
    let room_id_str = c_to_string(room_id);
    let txn_id_str = c_to_string(transaction_id);
    let client = handle.client.clone();

    handle.runtime.spawn(async move {
        if let Err(e) = client.cancel_upload(&room_id_str, &txn_id_str).await {
            tracing::error!("cancel_upload failed: {e}");
        }
    });
}

// --- Cache management FFI ---

/// Callback for cache statistics retrieval.
pub type TmCacheStatsCallback = extern "C" fn(
    media_files_bytes: u64,
    preview_cache_bytes: u64,
    app_cache_bytes: u64,
    search_index_bytes: u64,
    total_bytes: u64,
    media_file_count: u64,
    userdata: *mut std::ffi::c_void,
);

/// Asynchronously compute cache statistics and deliver via callback.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_get_cache_stats(
    h: *mut Handle,
    callback: TmCacheStatsCallback,
    userdata: *mut std::ffi::c_void,
) {
    let handle = unsafe { &*h };
    let ud = userdata as usize;
    let matrix = handle.matrix.clone();
    handle.runtime.spawn(async move {
        let stats = matrix.get_cache_stats().await;
        callback(
            stats.media_files_bytes,
            stats.preview_cache_bytes,
            stats.app_cache_bytes,
            stats.search_index_bytes,
            stats.total_bytes,
            stats.media_file_count,
            ud as *mut std::ffi::c_void,
        );
    });
}

/// Callback for cache clear operations.
pub type TmCacheClearCallback =
    extern "C" fn(success: bool, freed_bytes: u64, userdata: *mut std::ffi::c_void);

/// Clear media cache files using age + LRU eviction policy.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_clear_media_cache(
    h: *mut Handle,
    max_age_days: u32,
    size_limit_bytes: u64,
    callback: TmCacheClearCallback,
    userdata: *mut std::ffi::c_void,
) {
    let handle = unsafe { &*h };
    let ud = userdata as usize;
    let matrix = handle.matrix.clone();
    let active_downloads = handle.active_media_downloads.clone();
    handle.runtime.spawn(async move {
        abort_and_await_active_media_tasks(active_downloads).await;
        match matrix
            .clear_media_cache(max_age_days, size_limit_bytes)
            .await
        {
            Ok(freed) => callback(true, freed, ud as *mut std::ffi::c_void),
            Err(_) => callback(false, 0, ud as *mut std::ffi::c_void),
        }
    });
}

/// Clear all caches (media files, preview DB, VACUUM SDK databases).
///
/// # Safety
/// - `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_clear_all_caches(
    h: *mut Handle,
    callback: TmCacheClearCallback,
    userdata: *mut std::ffi::c_void,
) {
    let handle = unsafe { &*h };
    let ud = userdata as usize;
    let matrix = handle.matrix.clone();
    let active_downloads = handle.active_media_downloads.clone();
    handle.runtime.spawn(async move {
        abort_and_await_active_media_tasks(active_downloads).await;
        match matrix.clear_all_caches().await {
            Ok(freed) => callback(true, freed, ud as *mut std::ffi::c_void),
            Err(_) => callback(false, 0, ud as *mut std::ffi::c_void),
        }
    });
}

/// Trigger background auto-cleanup of media cache if over size limit.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_auto_cleanup_cache(h: *mut Handle, size_limit_bytes: u64) {
    let handle = unsafe { &*h };
    let _guard = handle.runtime.enter();
    handle.matrix.auto_cleanup(size_limit_bytes);
}

/// Set the media-cache size budget (bytes) from the app settings. Bounds the SDK
/// thumbnail store's retention policy and immediately enforces the app media +
/// video-stream cache budgets.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_set_media_cache_limit(h: *mut Handle, limit_bytes: u64) {
    let handle = unsafe { &*h };
    let _guard = handle.runtime.enter();
    handle.matrix.set_media_cache_limit(limit_bytes);
}

/// Enable/disable local search indexing of E2EE (encrypted) rooms.
///
/// When disabled, the backfill + live indexers stop and the local FTS search DB
/// is deleted; encrypted-room search then reports "disabled". When enabled, the
/// index is reopened and re-backfilled. Push the persisted app setting here on
/// session start and whenever the user toggles it.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
#[no_mangle]
pub unsafe extern "C" fn tm_set_e2ee_search_enabled(h: *mut Handle, enabled: bool) {
    let handle = unsafe { &*h };
    let _guard = handle.runtime.enter();
    handle.matrix.set_e2ee_search_enabled(enabled);
}

// --- Password Reset FFI ---

/// Callback type for password reset token result.
/// - `success`: true if the token request succeeded.
/// - `sid`: session ID from the server (null on failure).
/// - `submit_url`: optional URL to submit validation token (null if omitted or on failure).
/// - `error`: error message (null on success).
/// - `userdata`: opaque pointer passed through from the caller.
pub type TmPasswordResetTokenCallback = extern "C" fn(
    success: bool,
    sid: *const c_char,
    submit_url: *const c_char,
    error: *const c_char,
    userdata: *mut libc::c_void,
);

/// Request a password reset token via email (unauthenticated).
///
/// Calls `POST /_matrix/client/v3/account/password/email/requestToken`.
/// The callback receives the session ID (`sid`) on success, or an error message on failure.
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `homeserver`, `email`, and `client_secret` must be valid null-terminated C strings.
#[no_mangle]
pub unsafe extern "C" fn tm_request_password_reset(
    h: *mut Handle,
    homeserver: *const c_char,
    email: *const c_char,
    client_secret: *const c_char,
    callback: TmPasswordResetTokenCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let hs = c_to_string(homeserver);
    let em = c_to_string(email);
    let secret = c_to_string(client_secret);
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        match AuthService::request_password_reset_token(&hs, &em, &secret).await {
            Ok((sid, submit_url)) => {
                let sid_c = CString::new(sid).unwrap_or_default();
                let submit_c = submit_url
                    .as_deref()
                    .map(|s| CString::new(s).unwrap_or_default());
                let submit_ptr = submit_c.as_ref().map_or(ptr::null(), |c| c.as_ptr());
                callback(true, sid_c.as_ptr(), submit_ptr, ptr::null(), ud.as_ptr());
            }
            Err(e) => {
                let err_msg = CString::new(format!("{e}")).unwrap_or_default();
                callback(
                    false,
                    ptr::null(),
                    ptr::null(),
                    err_msg.as_ptr(),
                    ud.as_ptr(),
                );
            }
        }
    });
}

/// Reset password using an email verification token (unauthenticated).
///
/// Calls `POST /_matrix/client/v3/account/password` with `m.login.email.identity` auth.
/// The callback receives success/failure. On failure, the `userdata` callback receives
/// `false` (the caller should inspect the error).
///
/// # Safety
/// - `h` must be a valid Handle pointer.
/// - `homeserver`, `new_password`, `sid`, and `client_secret` must be valid null-terminated C strings.
#[no_mangle]
pub unsafe extern "C" fn tm_reset_password(
    h: *mut Handle,
    homeserver: *const c_char,
    new_password: *const c_char,
    sid: *const c_char,
    client_secret: *const c_char,
    callback: TmSimpleCallback,
    userdata: *mut libc::c_void,
) {
    let handle = unsafe { &*h };
    let hs = c_to_string(homeserver);
    let pw = c_to_string(new_password);
    let sid_str = c_to_string(sid);
    let secret = c_to_string(client_secret);
    let ud = Userdata::new(userdata);

    handle.runtime.spawn(async move {
        let success = AuthService::reset_password(&hs, &pw, &sid_str, &secret)
            .await
            .is_ok();
        callback(success, ud.as_ptr());
    });
}

// --- Keychain FFI ---

/// Free a single C string returned by this library (e.g. from `tm_keychain_load`).
///
/// # Safety
/// `ptr` must be a string pointer returned by this library, or null.
/// Returns a loopback HTTP URL the media player can open to stream the given
/// Matrix media (`mxc://…`). The returned string is heap-allocated and must be
/// freed with `tm_free_string`. Returns NULL when streaming is unavailable (no
/// active session, server failed to bind, or invalid arguments).
///
/// # Safety
/// `h` must be a valid `Handle` pointer or null. `mxc` must be a valid UTF-8
/// C string pointer or null.
#[no_mangle]
pub unsafe extern "C" fn tm_video_stream_url(h: *mut Handle, mxc: *const c_char) -> *mut c_char {
    if h.is_null() || mxc.is_null() {
        return std::ptr::null_mut();
    }
    let handle = unsafe { &*h };
    let mxc = match unsafe { CStr::from_ptr(mxc) }.to_str() {
        Ok(s) => s.to_owned(),
        Err(_) => return std::ptr::null_mut(),
    };
    let matrix = handle.matrix.clone();
    match handle
        .runtime
        .block_on(async move { matrix.video_stream_url(&mxc).await })
    {
        Some(url) => CString::new(url)
            .map(|c| c.into_raw())
            .unwrap_or(std::ptr::null_mut()),
        None => std::ptr::null_mut(),
    }
}

#[no_mangle]
pub unsafe extern "C" fn tm_free_string(ptr: *mut c_char) {
    unsafe { free_c_string(ptr) };
}

/// Fraction (0.0–1.0) of a streaming video already downloaded by the loopback
/// proxy. Returns 0.0 for an mxc the running proxy doesn't know yet (a stream just
/// starting). Returns 1.0 only on the no-handle / invalid-argument guard paths,
/// where the C++ caller treats the video as unrestricted (local-file playback).
#[no_mangle]
pub unsafe extern "C" fn tm_video_stream_progress(h: *mut Handle, mxc: *const c_char) -> f32 {
    if h.is_null() || mxc.is_null() {
        return 1.0;
    }
    let handle = unsafe { &*h };
    let mxc = match unsafe { CStr::from_ptr(mxc) }.to_str() {
        Ok(s) => s.to_owned(),
        Err(_) => return 1.0,
    };
    let matrix = handle.matrix.clone();
    handle
        .runtime
        .block_on(async move { matrix.video_stream_progress(&mxc).await })
}

/// Output the raw (downloaded, total) byte counts for a proxy-streamed video.
/// Returns false (leaving the out-params untouched) if it isn't currently
/// streamed, so the UI can fall back to the event's file size.
///
/// # Safety
/// - `h` and `mxc` must be valid; `out_downloaded`/`out_total` may be null.
#[no_mangle]
pub unsafe extern "C" fn tm_video_stream_progress_bytes(
    h: *mut Handle,
    mxc: *const c_char,
    out_downloaded: *mut u64,
    out_total: *mut u64,
) -> bool {
    if h.is_null() || mxc.is_null() {
        return false;
    }
    let handle = unsafe { &*h };
    let mxc = match unsafe { CStr::from_ptr(mxc) }.to_str() {
        Ok(s) => s.to_owned(),
        Err(_) => return false,
    };
    let matrix = handle.matrix.clone();
    let result = handle
        .runtime
        .block_on(async move { matrix.video_stream_progress_bytes(&mxc).await });
    if let Some((downloaded, total)) = result {
        if !out_downloaded.is_null() {
            unsafe { *out_downloaded = downloaded };
        }
        if !out_total.is_null() {
            unsafe { *out_total = total };
        }
        true
    } else {
        false
    }
}

/// Whether the loopback proxy's current download for `mxc` has failed (upstream
/// error, stall, or supersede-cancel). Lets the C++ retry loop fail fast to its
/// local fallback instead of waiting out its stall window. Returns false on the
/// no-handle / invalid-argument guard paths and when there's no entry for `mxc`.
///
/// # Safety
/// - `h` and `mxc` must be valid.
#[no_mangle]
pub unsafe extern "C" fn tm_video_stream_errored(h: *mut Handle, mxc: *const c_char) -> bool {
    if h.is_null() || mxc.is_null() {
        return false;
    }
    let handle = unsafe { &*h };
    let mxc = match unsafe { CStr::from_ptr(mxc) }.to_str() {
        Ok(s) => s.to_owned(),
        Err(_) => return false,
    };
    let matrix = handle.matrix.clone();
    handle
        .runtime
        .block_on(async move { matrix.video_stream_errored(&mxc).await })
}

/// Whether a video can be played progressively, judged from its container header:
/// 0 = unknown (not yet classified, or an unrecognised container — the caller
/// should keep its own heuristic), 1 = faststart, 2 = moov-at-end (the whole file
/// downloads before the first frame, so show determinate download progress).
///
/// Reads a process-global mirror; no runtime, no blocking. Returns 0 on the
/// no-handle / invalid-argument guard paths.
///
/// # Safety
/// - `h` and `mxc` must be valid.
#[no_mangle]
pub unsafe extern "C" fn tm_video_stream_container(h: *mut Handle, mxc: *const c_char) -> u8 {
    if h.is_null() || mxc.is_null() {
        return 0;
    }
    let handle = unsafe { &*h };
    let mxc = match unsafe { CStr::from_ptr(mxc) }.to_str() {
        Ok(s) => s,
        Err(_) => return 0,
    };
    handle.matrix.video_stream_container(mxc)
}

/// Store a secret in the system keychain.
/// Returns true on success.
///
/// # Safety
/// `key` and `value` must be valid C strings.
#[no_mangle]
pub unsafe extern "C" fn tm_keychain_store(key: *const c_char, value: *const c_char) -> bool {
    let key = c_to_string(key);
    let value = c_to_string(value);
    match crate::keychain::store_secret(&key, &value) {
        Ok(()) => true,
        Err(e) => {
            warn!("[KEYCHAIN] failed to store {key}: {e}");
            false
        }
    }
}

/// Load a secret from the system keychain.
/// Returns a heap-allocated C string (caller must free with `tm_free_string`), or NULL.
///
/// NULL is ambiguous on its own — it means both "no such secret" and "the keychain
/// refused the read" — so `out_failed` (optional) separates them. The caller MUST
/// check it before concluding a secret is absent: treating a refused read as an
/// absent one, and clearing the session on the strength of it, destroys secrets
/// that were merely unreadable at that moment.
///
/// # Safety
/// `key` must be a valid C string. `out_failed` must be null or a valid `bool`.
#[no_mangle]
pub unsafe extern "C" fn tm_keychain_load(
    key: *const c_char,
    out_failed: *mut bool,
) -> *mut c_char {
    let key = c_to_string(key);
    let (value, failed) = match crate::keychain::load_secret(&key) {
        Ok(Some(value)) => (str_to_c(&value), false),
        Ok(None) => (ptr::null_mut(), false),
        Err(e) => {
            warn!("[KEYCHAIN] failed to load {key}: {e}");
            (ptr::null_mut(), true)
        }
    };
    if !out_failed.is_null() {
        unsafe { *out_failed = failed };
    }
    value
}

/// Drop the cached secret bundle so the next load goes back to the keychain.
/// Call before retrying a read that came back empty, or the retry just re-serves
/// the same cached answer.
#[no_mangle]
pub extern "C" fn tm_keychain_forget_cache() {
    crate::keychain::forget_cached_bundle();
}

/// Delete a secret from the system keychain.
/// Returns true on success.
///
/// # Safety
/// `key` must be a valid C string.
#[no_mangle]
pub unsafe extern "C" fn tm_keychain_delete(key: *const c_char) -> bool {
    let key = c_to_string(key);
    crate::keychain::delete_secret(&key).is_ok()
}

/// Clear all TeleMatrix secrets from the keychain (for logout).
#[no_mangle]
pub unsafe extern "C" fn tm_keychain_clear_all() -> bool {
    crate::keychain::clear_all_secrets().is_ok()
}

/// Configure the secret backend for this process. Call once at startup, before
/// any secret access (including the `tm_keychain_*` gate in the C++ constructor).
/// `backend`: 0 = OS keychain, 1 = master-password file vault (honored on every
/// platform — a user-selectable choice). `data_dir` is where the vault file lives.
///
/// # Safety
/// `data_dir` must be a valid C string.
#[no_mangle]
pub unsafe extern "C" fn tm_secret_store_init(data_dir: *const c_char, backend: c_int) {
    // The whole secret-store gate runs before the first ProtocolBridge is built, so
    // tm_create has not installed the subscriber yet and every keychain warning
    // would be dropped — exactly the diagnostics needed when a backend probe fails.
    init_tracing();
    let data_dir = c_to_string(data_dir);
    crate::keychain::init(
        std::path::PathBuf::from(data_dir),
        crate::keychain::SecretBackend::from_i32(backend),
    );
}

/// Current secret-store state:
/// 0 = KeychainReady, 1 = KeychainUnavailable, 2 = VaultLocked, 3 = VaultUnlocked,
/// 4 = VaultAbsent. Lets the startup gate defer (prompt/unlock) instead of wiping
/// when the backend is merely unreachable.
#[no_mangle]
pub extern "C" fn tm_secret_store_state() -> c_int {
    init_tracing();
    crate::keychain::state().as_i32() as c_int
}

/// Whether the OS Secret Service is reachable (Linux); always true elsewhere.
/// Used to choose keychain-vs-vault at fresh login.
#[no_mangle]
pub extern "C" fn tm_secret_service_available() -> bool {
    init_tracing();
    crate::keychain::service_available()
}

/// Granular Secret Service reachability for diagnostics: 0 = available,
/// 1 = no D-Bus session bus, 2 = D-Bus up but no provider. Always 0 off Linux.
#[no_mangle]
pub extern "C" fn tm_secret_service_status() -> c_int {
    init_tracing();
    crate::keychain::service_status() as c_int
}

/// Unlock the master-password file vault. Returns 0 on success; 1 = wrong
/// password (or corrupted ciphertext — indistinguishable under AEAD); 2 = not a
/// valid vault file; 3 = unreadable file / non-vault backend; 4 = vault decoded
/// but its contents are corrupt.
///
/// # Safety
/// `passphrase` must be a valid C string.
#[no_mangle]
pub unsafe extern "C" fn tm_secret_store_unlock(passphrase: *const c_char) -> c_int {
    let passphrase = Zeroizing::new(c_to_string(passphrase));
    match crate::keychain::unlock(passphrase.as_str()) {
        Ok(()) => 0,
        Err(f) => {
            warn!("[SECRET_STORE] vault unlock failed ({f:?})");
            f.as_i32() as c_int
        }
    }
}

/// Set or replace the vault master password and switch to the file-vault backend.
/// Returns true on success.
///
/// # Safety
/// `passphrase` must be a valid C string.
#[no_mangle]
pub unsafe extern "C" fn tm_secret_store_set_passphrase(passphrase: *const c_char) -> bool {
    let passphrase = Zeroizing::new(c_to_string(passphrase));
    match crate::keychain::set_passphrase(passphrase.as_str()) {
        Ok(()) => true,
        Err(e) => {
            warn!("[SECRET_STORE] set vault passphrase failed: {e}");
            false
        }
    }
}

/// Migrate all secrets to a different backend (`backend`: 0 = OS keychain, 1 =
/// file vault). For the vault target, `passphrase` is the new master password; for
/// the keychain target it must be null. Returns true on success.
///
/// # Safety
/// `passphrase` must be a valid C string or null.
#[no_mangle]
pub unsafe extern "C" fn tm_secret_store_switch_backend(
    backend: c_int,
    passphrase: *const c_char,
) -> bool {
    let target = crate::keychain::SecretBackend::from_i32(backend);
    let pass = opt_c_to_string(passphrase).map(Zeroizing::new);
    match crate::keychain::switch_backend(target, pass.as_ref().map(|p| p.as_str())) {
        Ok(()) => true,
        Err(e) => {
            warn!("[SECRET_STORE] backend switch failed: {e}");
            false
        }
    }
}

// ===========================================================================
// Auto-update (handle-free)
// ===========================================================================
//
// These are the first *async* handle-free entry points. Unlike every `Handle`
// call there is no `tm_destroy` to join the runtime, so C++ cannot drain the
// updater at shutdown. The contract that replaces it:
//
//   Each call delivers **exactly one** terminal callback (success or error),
//   after which `userdata` is never touched again. Progress callbacks only ever
//   fire *before* the terminal one — `UpdateCompletion` serialises the two so a
//   late progress tick can't land after C++ has freed its userdata.
//
// C++ keeps its callback guard alive via a shared_ptr stored *in* the userdata,
// so a callback arriving after the service is gone finds a live guard and a
// null service, and no-ops.

/// Result of an update check.
/// `status`: 0 = up to date, 1 = update available, 2 = check failed.
/// `version`/`page` are set when `status` is 1. The asset fields (`url`, `size`,
/// `sha256`, `minisig`) are set only when this platform has a downloadable
/// asset; an empty `url` means notify-only (unknown platform key, or an asset
/// that has not finished uploading yet). `error` is set only when `status` is 2.
/// All strings are borrowed for the duration of the call — copy, do not free.
pub type TmUpdateCheckCallback = extern "C" fn(
    status: u32,
    version: *const c_char,
    page: *const c_char,
    url: *const c_char,
    size: u64,
    sha256: *const c_char,
    minisig: *const c_char,
    error: *const c_char,
    userdata: *mut libc::c_void,
);

/// Byte progress of an update download. Always precedes the terminal callback.
pub type TmUpdateProgressCallback =
    extern "C" fn(received_bytes: u64, total_bytes: u64, userdata: *mut libc::c_void);

/// Terminal callback of an update download. `local_path` is the verified file on
/// success; `error` is human-readable text on failure. Both are borrowed for the
/// duration of the call — copy, do not free.
pub type TmUpdateDownloadCallback = extern "C" fn(
    success: bool,
    local_path: *const c_char,
    error: *const c_char,
    userdata: *mut libc::c_void,
);

/// Serialises a download's progress and terminal callbacks so the terminal one
/// fires exactly once and no progress tick can follow it. Mirrors
/// `MediaDownloadCompletion`; it is what makes "the terminal callback owns the
/// userdata" sound on the C++ side.
struct UpdateCompletion {
    callback: TmUpdateDownloadCallback,
    userdata: Userdata,
    callback_lock: Mutex<()>,
    completed: AtomicBool,
}

impl UpdateCompletion {
    fn new(callback: TmUpdateDownloadCallback, userdata: Userdata) -> Self {
        Self {
            callback,
            userdata,
            callback_lock: Mutex::new(()),
            completed: AtomicBool::new(false),
        }
    }

    fn finish(&self, success: bool, local_path: *const c_char, error: *const c_char) {
        let _guard = lock_ffi_mutex(&self.callback_lock, "update_download_callback");
        if !self.completed.swap(true, Ordering::AcqRel) {
            (self.callback)(success, local_path, error, self.userdata.as_ptr());
        }
    }

    fn progress(&self, callback: TmUpdateProgressCallback, received: u64, total: u64) {
        let _guard = lock_ffi_mutex(&self.callback_lock, "update_download_callback");
        if !self.completed.load(Ordering::Acquire) {
            callback(received, total, self.userdata.as_ptr());
        }
    }
}

/// Whether this build has an update signing key compiled in. When false the app
/// must not offer to download — only to open the release page.
#[no_mangle]
pub extern "C" fn tm_update_signing_configured() -> bool {
    crate::update_service::signing_configured()
}

/// Check for a newer release. `platform_key` selects the manifest entry (e.g.
/// `macos-universal`); Linux resolves it at runtime because deb/rpm/AppImage are
/// the same binary. Transient failures (404 during the publish window) report
/// "up to date" rather than an error.
///
/// # Safety
/// `current_version`, `manifest_url` and `platform_key` must be valid C strings.
/// `userdata` must stay valid until the callback fires.
#[no_mangle]
pub unsafe extern "C" fn tm_update_check(
    current_version: *const c_char,
    manifest_url: *const c_char,
    platform_key: *const c_char,
    callback: TmUpdateCheckCallback,
    userdata: *mut libc::c_void,
) {
    let current = c_to_string(current_version);
    let url = c_to_string(manifest_url);
    let key = c_to_string(platform_key);
    let ud = Userdata::new(userdata);

    let deliver_error = move |message: &str| {
        let error = CString::new(message).unwrap_or_default();
        callback(
            2,
            ptr::null(),
            ptr::null(),
            ptr::null(),
            0,
            ptr::null(),
            ptr::null(),
            error.as_ptr(),
            ud.as_ptr(),
        );
    };

    let spawned = crate::update_service::spawn(async move {
        match crate::update_service::check(&current, &url, &key).await {
            Ok(crate::update_service::CheckOutcome::UpToDate) => {
                callback(
                    0,
                    ptr::null(),
                    ptr::null(),
                    ptr::null(),
                    0,
                    ptr::null(),
                    ptr::null(),
                    ptr::null(),
                    ud.as_ptr(),
                );
            }
            Ok(crate::update_service::CheckOutcome::Available {
                version,
                page,
                asset,
            }) => {
                let version_c = CString::new(version).unwrap_or_default();
                let page_c = CString::new(page).unwrap_or_default();
                let (url_c, size, sha_c, sig_c) = match asset {
                    Some(a) => (
                        CString::new(a.url).unwrap_or_default(),
                        a.size,
                        CString::new(a.sha256).unwrap_or_default(),
                        CString::new(a.minisig).unwrap_or_default(),
                    ),
                    None => (
                        CString::default(),
                        0,
                        CString::default(),
                        CString::default(),
                    ),
                };
                callback(
                    1,
                    version_c.as_ptr(),
                    page_c.as_ptr(),
                    url_c.as_ptr(),
                    size,
                    sha_c.as_ptr(),
                    sig_c.as_ptr(),
                    ptr::null(),
                    ud.as_ptr(),
                );
            }
            Err(e) => {
                warn!("[UPDATE] check failed: {e}");
                deliver_error(&e.to_string());
            }
        }
    });
    if !spawned {
        deliver_error("updater runtime unavailable");
    }
}

/// Download and verify an update asset. Exactly one download runs at a time; a
/// second call fails immediately. `expected_version` is the manifest's version —
/// the signature's trusted comment must name it, and it must be newer than
/// `current_version`, or the download is rejected as a downgrade.
///
/// # Safety
/// All string arguments must be valid C strings. `userdata` must stay valid
/// until the terminal callback fires.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn tm_update_download(
    url: *const c_char,
    size: u64,
    sha256: *const c_char,
    minisig: *const c_char,
    expected_version: *const c_char,
    current_version: *const c_char,
    cache_dir: *const c_char,
    progress_callback: TmUpdateProgressCallback,
    callback: TmUpdateDownloadCallback,
    userdata: *mut libc::c_void,
) {
    let url = c_to_string(url);
    let sha = c_to_string(sha256);
    let sig = c_to_string(minisig);
    let expected = c_to_string(expected_version);
    let current = c_to_string(current_version);
    let dir = std::path::PathBuf::from(c_to_string(cache_dir));
    let ud = Userdata::new(userdata);

    let completion = Arc::new(UpdateCompletion::new(callback, ud));
    let deliver_error = |completion: &UpdateCompletion, message: &str| {
        let error = CString::new(message).unwrap_or_default();
        completion.finish(false, ptr::null(), error.as_ptr());
    };

    // Claimed here, on the calling thread, so a second call is refused
    // deterministically instead of racing the spawn.
    let Some(slot) = crate::update_service::DownloadSlot::acquire() else {
        deliver_error(&completion, "an update download is already in progress");
        return;
    };

    let completion_for_task = completion.clone();
    let completion_for_progress = completion.clone();
    let spawned = crate::update_service::spawn(async move {
        // Held for the whole download; released when this task ends, however it ends.
        let _slot = slot;
        let result = crate::update_service::download_and_verify(
            &url,
            size,
            &sha,
            &sig,
            &expected,
            &current,
            &dir,
            move |received, total| {
                completion_for_progress.progress(progress_callback, received, total);
            },
        )
        .await;
        match result {
            Ok(path) => {
                let path_c = CString::new(path.to_string_lossy().as_ref()).unwrap_or_default();
                completion_for_task.finish(true, path_c.as_ptr(), ptr::null());
            }
            Err(e) => {
                warn!("[UPDATE] download failed: {e}");
                let error = CString::new(e.to_string()).unwrap_or_default();
                completion_for_task.finish(false, ptr::null(), error.as_ptr());
            }
        }
    });
    if !spawned {
        deliver_error(&completion, "updater runtime unavailable");
    }
}

/// Ask the in-flight download to stop. Asynchronous: the download still delivers
/// its single terminal callback (as a failure), so C++ frees userdata as usual.
#[no_mangle]
pub extern "C" fn tm_update_cancel() {
    crate::update_service::request_cancel();
}

/// Re-verify a downloaded update immediately before applying it, closing the
/// window between "verified at download" and "executed at apply" during which
/// any same-user process could have swapped the file. Blocking — it re-hashes
/// the payload — but it runs on a deliberate click as the app is exiting.
///
/// Returns true when the file still matches its signature, version binding and
/// checksum. On false, `out_error` (when non-null) receives an owned message the
/// caller must release with `tm_free_string`.
///
/// # Safety
/// All string arguments must be valid C strings. `out_error` must be null or a
/// valid pointer to a writable `char *`.
#[no_mangle]
pub unsafe extern "C" fn tm_update_verify_file(
    path: *const c_char,
    sha256: *const c_char,
    minisig: *const c_char,
    expected_version: *const c_char,
    current_version: *const c_char,
    out_error: *mut *mut c_char,
) -> bool {
    let path = c_to_string(path);
    let sha = c_to_string(sha256);
    let sig = c_to_string(minisig);
    let expected = c_to_string(expected_version);
    let current = c_to_string(current_version);

    match crate::update_service::verify_file(
        std::path::Path::new(&path),
        &sha,
        &sig,
        &expected,
        &current,
    ) {
        Ok(()) => true,
        Err(e) => {
            warn!("[UPDATE] pre-apply verification failed: {e}");
            if !out_error.is_null() {
                unsafe { *out_error = str_to_c(&e.to_string()) };
            }
            false
        }
    }
}

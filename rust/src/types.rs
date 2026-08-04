// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::time::SystemTime;

/// A user's profile information.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UserProfile {
    pub user_id: String,
    pub display_name: String,
    pub avatar_url: Option<String>,
}

/// Session info for persistence across restarts.
pub struct SessionInfo {
    pub homeserver: String,
    pub user_id: String,
    pub device_id: String,
    pub access_token: String,
}

/// Presence state for a user.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum PresenceState {
    Offline = 0,
    Online = 1,
    Unavailable = 2,
}

/// Membership state within a room.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum MembershipState {
    Join = 0,
    Invite = 1,
    Leave = 2,
    Ban = 3,
    Knock = 4,
}

/// Role derived from power level.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum MemberRole {
    Administrator = 0,
    Moderator = 1,
    User = 2,
}

/// Detailed info about a room member, including permission flags.
#[derive(Debug, Clone)]
pub struct RoomMemberInfo {
    pub user_id: String,
    pub display_name: String,
    pub avatar_url: Option<String>,
    pub membership: MembershipState,
    pub power_level: i64,
    pub role: MemberRole,
    pub is_self: bool,
    pub can_be_removed_by_me: bool,
    pub can_be_banned_by_me: bool,
    pub can_be_unbanned_by_me: bool,
}

/// Snapshot of a room's members with actor permission flags.
#[derive(Debug, Clone)]
pub struct RoomMembersSnapshot {
    pub room_id: String,
    pub my_user_id: String,
    pub can_invite: bool,
    pub can_remove_any: bool,
    pub members: Vec<RoomMemberInfo>,
}

/// Detailed user profile for the profile popup.
#[derive(Debug, Clone)]
pub struct UserProfileDetails {
    pub room_id: String,
    pub user_id: String,
    pub display_name: String,
    pub avatar_url: Option<String>,
    pub presence: PresenceState,
    pub last_active_ts: u64,
    pub membership: MembershipState,
    pub power_level: i64,
    pub role: MemberRole,
    pub is_ignored: bool,
    pub dm_room_id: Option<String>,
    // Permission booleans.
    pub can_invite: bool,
    pub can_kick: bool,
    pub can_ban: bool,
    pub can_mute: bool,
    pub can_change_power_level: bool,
    pub max_assignable_power_level: i64,
}

/// Summary of a room for display in the chat list.
#[derive(Debug, Clone)]
pub struct RoomSummary {
    pub room_id: String,
    pub display_name: String,
    pub canonical_alias: Option<String>,
    pub avatar_url: Option<String>,
    /// Stable entity ID for avatar placeholder color selection.
    /// For DM rooms this is the peer's user ID; otherwise the room ID.
    pub avatar_entity_id: String,
    pub last_event_text: String,
    pub last_event_sender: String,
    pub last_event_timestamp: SystemTime,
    pub unread_count: u32,
    pub highlight_count: u32,
    pub notification_mode: RoomNotificationMode,
    pub is_muted: bool,
    pub is_pinned: bool,
    /// `order` from the room's `m.favourite` tag: where the user put this room among
    /// their pinned ones. Lives in server account data, so it survives a re-login and
    /// reaches other devices. None on rooms pinned before the app started writing it.
    pub pinned_order: Option<f64>,
    pub is_marked_unread: bool,
    pub is_direct: bool,
    /// The room's join rule is public. Known synchronously from room state, so the UI can decide
    /// whether to hide system messages at room-open time instead of waiting for the async settings
    /// snapshot (which caused a visible flicker).
    pub is_public: bool,
    /// Whether `is_public` is an answer or a default. `Room::is_public()` returns None until the
    /// room's join rule has synced, and `is_public` flattens that to false — so a rebuilt summary
    /// is indistinguishable from a genuinely private room and would clobber a known value (see
    /// `merge_sticky_previews`). Not exposed over FFI: C++ consumes the flattened bool.
    pub is_public_known: bool,
    pub filter_ids: Vec<i32>,
    /// Room ids of the joined spaces this room belongs to (recursively, through
    /// nested joined sub-spaces). Parallel to `filter_ids` but keyed by space id.
    pub space_ids: Vec<String>,
    pub is_last_event_outgoing: bool,
    pub is_last_event_service: bool,
    pub last_event_send_state: SendState,
    pub member_count: u64,
    pub can_pin_messages: bool,
    /// Presence state of the DM peer (0=offline, 1=online, 2=unavailable).
    pub peer_presence: u32,
    /// Room membership state (Join for normal rooms, Invite for pending invites).
    pub membership: MembershipState,
    /// User ID of the person who sent the invite (empty for joined rooms).
    pub inviter_user_id: String,
    /// Display name of the inviter (empty for joined rooms).
    pub inviter_display_name: String,
    /// Avatar URL of the inviter (empty for joined rooms).
    pub inviter_avatar_url: String,
    /// Room topic from state events.
    pub room_topic: String,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct RoomUnreadCounts {
    pub unread_count: u32,
    pub highlight_count: u32,
}

/// Notification mode for a room.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum RoomNotificationMode {
    AllMessages = 0,
    MentionsOnly = 1,
    Mute = 2,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum ContentType {
    Text = 0,
    Image = 1,
    File = 2,
    Video = 3,
    Service = 4,
    Poll = 5,
    Audio = 7,
    UnableToDecrypt = 8,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum SendState {
    Sending = 0,
    Sent = 1,
    Read = 2,
    Failed = 3,
}

#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
pub struct ReactionInfo {
    pub key: String,
    pub count: u32,
    pub is_self: bool,
}

/// Forwarding metadata for a forwarded message.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ForwardedFrom {
    pub sender_display_name: String,
    /// Original author identity, used by Saved Messages to present the
    /// forward as authored by them. Empty on forwards made before these
    /// fields existed (name-only metadata).
    pub sender_id: String,
    pub avatar_url: String,
}

/// Audio message metadata (voice messages and audio files).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AudioInfo {
    pub url: String,
    pub mime_type: String,
    pub filename: String,
    pub size: u64,
    pub duration_ms: u64,
    pub is_voice: bool,
    pub waveform: Vec<u8>,
}

/// Preview type for link cards (subset of web-page types relevant for
/// Matrix previews).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum PreviewType {
    None = 0,
    Article = 1,
    Photo = 2,
    Video = 3,
    Document = 4,
    Profile = 5,
    Group = 6,
    Channel = 7,
}

impl PreviewType {
    pub fn from_u32(v: u32) -> Self {
        match v {
            0 => Self::None,
            1 => Self::Article,
            2 => Self::Photo,
            3 => Self::Video,
            4 => Self::Document,
            5 => Self::Profile,
            6 => Self::Group,
            7 => Self::Channel,
            _ => Self::Article,
        }
    }
}

/// URL preview metadata (from OG tags / Matrix preview_url endpoint).
/// Extended beyond basic OG fields to support type-driven
/// rendering (article vs large-media mode, provider-specific rules).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UrlPreview {
    pub url: String,
    pub site_name: Option<String>,
    pub title: Option<String>,
    pub description: Option<String>,
    pub image_url: Option<String>,
    pub image_width: u32,
    pub image_height: u32,
    /// Computed preview type.
    pub preview_type: PreviewType,
    /// Content duration in seconds (YouTube videos, etc.).
    pub duration_secs: u32,
    /// Content author name.
    pub author: Option<String>,
    /// Whether to force large media display.
    /// When true, disables article (small thumbnail) mode.
    pub has_large_media: bool,
    /// Lowercase-normalized canonical site key (e.g. "youtube", "twitter").
    /// Used for provider-specific rendering rules.
    pub site_name_canonical: Option<String>,
}

/// Metadata for a custom chat folder (a native `u.*` room-list section).
/// `tag_key` is the durable identity; `id` is a per-session runtime handle
/// derived from it via `room_folders::section_handle` (the int the C++ UI keys
/// on). `name` is the tag's display name.
#[derive(Debug, Clone)]
pub struct FolderMeta {
    pub id: i32,
    pub tag_key: String,
    pub name: String,
}

/// A joined Matrix space, surfaced in the left rail as a folder-like tab.
#[derive(Debug, Clone)]
pub struct SpaceInfo {
    pub room_id: String,
    pub name: String,
    pub avatar_url: Option<String>,
    pub topic: String,
    pub member_count: u64,
    pub canonical_alias: Option<String>,
}

/// Reply preview data rendered in the quoted-reply header.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ReplyPreview {
    pub sender_display_name: String,
    pub text: String,
    pub thumb_url: Option<String>,
    pub has_thumb: bool,
    pub is_text_colorized: bool,
    pub is_deleted: bool,
    pub is_unavailable: bool,
}

/// A single message in a room timeline.
#[derive(Debug, Clone, PartialEq)]
pub struct TimelineItem {
    pub event_id: String,
    pub transaction_id: Option<String>,
    pub sender: UserProfile,
    pub timestamp: SystemTime,
    pub content: MessageContent,
    pub reply_to_event_id: Option<String>,
    pub reply_preview: Option<ReplyPreview>,
    pub forwarded_from: Option<ForwardedFrom>,
    pub is_edited: bool,
    pub is_pinned: bool,
    pub reactions: Vec<ReactionInfo>,
    pub send_state: SendState,
    pub upload_progress: f64,
    pub is_outgoing: bool,
    pub is_deleted: bool,
    pub url_preview: Option<UrlPreview>,
    // Encryption indicators.
    pub is_encrypted: bool,
    pub decryption_error: Option<String>,
}

/// A windowed view of the timeline with pagination metadata
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum TimelineUpdateKind {
    Full = 0,
    Append = 1,
    Prepend = 2,
    Replace = 3,
    MetadataOnly = 4,
}

#[derive(Debug, Clone, PartialEq)]
pub struct TimelineSlice {
    pub items: Vec<TimelineItem>,
    pub update_kind: TimelineUpdateKind,
    pub update_index: u32,
    pub can_paginate_back: bool,
    pub can_paginate_forward: bool,
    pub hit_timeline_start: bool,
    pub is_live: bool,
    pub focus_event_id: Option<String>,
    pub pinned_event_ids: Vec<String>,
    pub first_unread_event_id: Option<String>,
    /// Whether the user's read marker is present in this loaded window. When
    /// true the `first_unread_event_id` is the TRUE first-unread (the boundary
    /// is confirmed); when false it is a best-effort guess (marker older than
    /// the window). The C++ delimiter uses this to place immediately on a
    /// confirmed boundary instead of withholding until a visible read message
    /// precedes it — which the "hide system messages" filter can erase.
    pub read_marker_loaded: bool,
    pub unread_count: u32,
    pub unread_state_known: bool,
}

impl TimelineSlice {
    pub fn empty_live() -> Self {
        Self {
            items: Vec::new(),
            update_kind: TimelineUpdateKind::Full,
            update_index: 0,
            can_paginate_back: false,
            can_paginate_forward: false,
            hit_timeline_start: false,
            is_live: true,
            focus_event_id: None,
            pinned_event_ids: Vec::new(),
            first_unread_event_id: None,
            read_marker_loaded: false,
            unread_count: 0,
            unread_state_known: false,
        }
    }
}

/// Message content types.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum MessageContent {
    Text {
        body: String,
        /// HTML-formatted body (Matrix `org.matrix.custom.html` format).
        /// None for plain text messages.
        formatted_body: Option<String>,
    },
    Image {
        url: String,
        mime_type: String,
        filename: String,
        caption: Option<String>,
        thumbnail_url: Option<String>,
        blurhash: Option<String>,
        size: u64,
        width: u32,
        height: u32,
    },
    File {
        url: String,
        mime_type: String,
        filename: String,
        caption: Option<String>,
        size: u64,
        duration_ms: u64,
    },
    Video {
        url: String,
        mime_type: String,
        filename: String,
        caption: Option<String>,
        thumbnail_url: Option<String>,
        blurhash: Option<String>,
        size: u64,
        width: u32,
        height: u32,
        duration_ms: u64,
    },
    Audio {
        info: AudioInfo,
    },
    Service {
        body: String,
    },
    Poll {
        info: PollInfo,
    },
    UnableToDecrypt {
        body: String,
        cause: u8,
        utd_state: u8, // 0 = decrypting/glow (Unknown cause), 1 = terminal
        // Megolm session id of the missing key, when this is a MegolmV1 UTD.
        // Lets the backup-key retry ask only for the sessions it still lacks
        // instead of re-downloading (and re-decrypting) the whole room. None for
        // non-megolm UTDs, which the backup cannot resolve anyway.
        session_id: Option<String>,
    },
}

/// Poll kind — disclosed (results visible while open) or undisclosed
/// (results revealed once the poll is closed).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum PollKind {
    Disclosed = 0,
    Undisclosed = 1,
}

/// A single option/answer in a poll.
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
pub struct PollOption {
    pub id: String,
    pub text: String,
    pub vote_count: u32,
    pub is_chosen: bool,
    pub is_correct: bool,
}

/// Full poll metadata for rendering.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PollInfo {
    pub question: String,
    pub kind: PollKind,
    pub max_selections: u32,
    pub is_closed: bool,
    pub is_quiz: bool,
    pub total_voters: u32,
    pub options: Vec<PollOption>,
    pub has_voted: bool,
}

/// History visibility state for a room.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum HistoryVisibility {
    Joined = 0,
    Invited = 1,
    Shared = 2,
    WorldReadable = 3,
    Unknown = 4,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum RoomAccess {
    InviteOnly = 0,
    Public = 1,
    Knock = 2,
    Restricted = 3,
    KnockRestricted = 4,
    Private = 5,
    Unknown = 6,
}

/// A snapshot of a room's settings and security state.
#[derive(Debug, Clone)]
pub struct RoomSettingsSnapshot {
    pub room_id: String,
    pub display_name: String,
    pub canonical_alias: Option<String>,
    pub notification_mode: RoomNotificationMode,
    pub is_muted: bool,
    pub member_count: u64,
    pub is_encrypted: bool,
    pub encryption_algorithm: Option<String>,
    pub access: RoomAccess,
    pub history_visibility: HistoryVisibility,
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

/// Guest access policy for a room.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum CreateRoomGuestAccess {
    /// Guests cannot join (default for private rooms).
    Forbidden = 0,
    /// Guests can join without an account.
    CanJoin = 1,
}

/// Who can see room history.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum CreateRoomHistoryVisibility {
    /// Only members from the point they joined.
    Joined = 0,
    /// Only members from the point they were invited.
    Invited = 1,
    /// All members (including future members).
    Shared = 2,
    /// Anyone (world-readable).
    WorldReadable = 3,
}

/// Request to create a new room.
#[derive(Debug, Clone)]
pub struct CreateRoomRequest {
    pub name: String,
    pub topic: Option<String>,
    pub is_public: bool,
    pub encrypted: bool,
    /// Room alias local part (e.g. "my-room" → #my-room:server).
    /// Empty = no alias.
    pub alias: Option<String>,
    /// Path to an avatar image file to upload after creation.
    /// Empty = no avatar.
    pub avatar_path: Option<String>,
    /// Guest access policy.
    pub guest_access: CreateRoomGuestAccess,
    /// History visibility.
    pub history_visibility: CreateRoomHistoryVisibility,
    /// Whether users on other homeservers are allowed to join.
    pub federate: bool,
}

/// A single SAS verification emoji (character + display label).
#[derive(Debug, Clone)]
pub struct SasEmoji {
    pub emoji: String,
    pub label: String,
}

/// Verification state phases communicated to the UI.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum VerificationState {
    /// Initial: creating verification request.
    RequestingVerification = 0,
    /// Waiting for the other device to accept the request.
    WaitingForReady = 1,
    /// Request is ready; user can start SAS.
    Ready = 2,
    /// SAS started, waiting for key exchange.
    SasStarted = 3,
    /// SAS emojis are available for comparison.
    SasEmojisAvailable = 4,
    /// Waiting for the other side to confirm SAS.
    SasWaitingForConfirm = 5,
    /// QR code generated and shown; waiting for the other device to scan it.
    QrCodeReady = 6,
    /// The other device scanned our QR; waiting for the user to confirm.
    QrCodeScanned = 7,
    /// Verification completed successfully.
    Done = 8,
    /// Verification was cancelled.
    Cancelled = 9,
    /// The user deliberately skipped verification. Distinct from `Done` so the
    /// UI never mistakes a skip for a verified device: treating a skip as
    /// `Done` latches the session "verified", which arms the decrypting-glow
    /// skeleton for permanently-UTD messages that will never resolve.
    Skipped = 10,
}

/// Trust state of *another* user's cross-signed identity, communicated to the UI
/// for trust shields. `#[repr(u32)]` so the discriminant crosses the FFI boundary.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum UserTrustState {
    /// Not cross-signed by us (or the user has no cross-signing identity).
    Unverified = 0,
    /// We have signed this user's master key with our user-signing key.
    Verified = 1,
    /// Was verified before, but their identity changed (verification violation).
    Violation = 2,
    /// Identity verified, but the user has at least one active session that is
    /// not cross-signed by their own identity — messages from that session
    /// can't be trusted, so the shield shows a caution rather than a clean
    /// verified check.
    VerifiedWithWarning = 3,
}

impl UserTrustState {
    /// Map an identity's `(is_verified, has_verification_violation)` flags to a
    /// trust state. Violation takes precedence: a once-verified identity whose
    /// keys later changed reports `is_verified() == false` yet must surface as a
    /// violation, not a plain "unverified".
    pub(crate) fn from_flags(is_verified: bool, has_violation: bool) -> Self {
        if has_violation {
            Self::Violation
        } else if is_verified {
            Self::Verified
        } else {
            Self::Unverified
        }
    }
}

/// Capabilities for the verify choice screen.
#[derive(Debug, Clone)]
pub struct VerificationCapabilities {
    /// Whether there are other verified devices to verify against.
    pub can_verify_with_device: bool,
    /// Whether secret storage is set up (recovery key/passphrase).
    pub can_verify_with_recovery: bool,
    /// Whether SAS is supported.
    pub sas_supported: bool,
    /// Whether showing a QR code for verification is possible.
    pub qr_supported: bool,
}

/// A rendered QR code as a square module grid (1 = dark, 0 = light, row-major).
#[derive(Debug, Clone)]
pub struct QrCodeImage {
    pub size: usize,
    pub modules: Vec<u8>,
}

/// Scope of a search request.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum SearchScope {
    Room = 0,
    AllRooms = 1,
}

/// A search request from the UI layer.
#[derive(Debug, Clone)]
pub struct SearchRequest {
    pub request_id: u64,
    pub scope: SearchScope,
    pub room_id: Option<String>,
    pub query: String,
    pub limit: u32,
    pub next_token: Option<String>,
    /// Filter results to messages from this sender (user ID).
    pub sender_filter: Option<String>,
    /// Filter results to messages on or after this UNIX epoch timestamp (seconds).
    pub date_from: Option<u64>,
    /// Filter results to messages on or before this UNIX epoch timestamp (seconds).
    pub date_to: Option<u64>,
}

/// A single search result hit.
#[derive(Debug, Clone)]
pub struct SearchHit {
    pub room_id: String,
    pub event_id: String,
    pub sender_id: String,
    pub sender_name: String,
    pub timestamp: u64,
    pub snippet: String,
    pub rank: i32,
    pub local_only: bool,
}

/// A page of search results.
#[derive(Debug, Clone)]
pub struct SearchPage {
    pub request_id: u64,
    pub hits: Vec<SearchHit>,
    pub total_approx: i32,
    pub next_token: Option<String>,
    pub done: bool,
    /// True when this page is empty because the user disabled E2EE-room search
    /// (room-scoped search of an encrypted room). The UI shows a "disabled"
    /// message instead of a bare "no results".
    pub e2ee_disabled: bool,
    /// True for a room-scoped E2EE search whose local index isn't fully built yet
    /// (history backfill still running). The UI surfaces an "indexing" message so
    /// an empty/partial result reads as "still indexing" rather than "no results".
    pub indexing: bool,
}

/// How a room may be entered. Mirrors ruma's `JoinRuleKind`, which is `#[non_exhaustive]`, so
/// anything we don't know maps to `Unknown` rather than failing.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum RoomDirectoryJoinRule {
    Public = 0,
    Knock = 1,
    Invite = 2,
    Restricted = 3,
    KnockRestricted = 4,
    Private = 5,
    Unknown = 6,
}

/// Our membership in a room we found in the directory.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum RoomMembershipState {
    None = 0,
    Invited = 1,
    Joined = 2,
    Left = 3,
    Knocked = 4,
    Banned = 5,
}

/// A public-directory search request from the UI layer.
#[derive(Debug, Clone)]
pub struct RoomDirectoryRequest {
    pub request_id: u64,
    /// Matched server-side against the room's name, topic and canonical alias. Empty browses the
    /// whole directory.
    pub query: String,
    pub limit: u32,
    pub next_token: Option<String>,
}

/// A request for one page of a space's children.
#[derive(Debug, Clone)]
pub struct SpaceHierarchyRequest {
    pub request_id: u64,
    pub space_id: String,
    pub limit: u32,
    pub next_token: Option<String>,
}

/// One room (or space) in a directory search or a space's child list. The two sources render
/// identically, so they share a row type.
#[derive(Debug, Clone)]
pub struct RoomDirectoryEntry {
    pub room_id: String,
    /// Falls back to the canonical alias, then the room ID.
    pub name: String,
    pub topic: String,
    pub canonical_alias: String,
    pub avatar_url: String,
    pub member_count: u32,
    /// Only meaningful for a space; 0 otherwise.
    pub children_count: u32,
    pub is_space: bool,
    pub world_readable: bool,
    pub guest_can_join: bool,
    pub join_rule: RoomDirectoryJoinRule,
    pub membership: RoomMembershipState,
    /// Servers to try when joining. Empty for directory results (join by alias or by ID against our
    /// own homeserver, which already knows the room).
    pub via: Vec<String>,
}

/// A page of directory or space-hierarchy results.
#[derive(Debug, Clone)]
pub struct RoomDirectoryPage {
    pub request_id: u64,
    pub entries: Vec<RoomDirectoryEntry>,
    /// -1 when the server gave no estimate.
    pub total_approx: i32,
    pub next_token: Option<String>,
    pub done: bool,
}

/// What we can learn about a room before joining it. Matrix serves no message history to a
/// non-member, so this is the entire content of the preview screen.
#[derive(Debug, Clone)]
pub struct RoomPreviewInfo {
    pub room_id: String,
    pub name: String,
    pub topic: String,
    pub canonical_alias: String,
    pub avatar_url: String,
    pub member_count: u32,
    pub is_space: bool,
    pub join_rule: RoomDirectoryJoinRule,
    pub membership: RoomMembershipState,
    /// History is visible to non-members — the only case where a preview can show messages.
    pub world_readable: bool,
}

impl MessageContent {
    pub fn content_type(&self) -> ContentType {
        match self {
            Self::Text { .. } => ContentType::Text,
            Self::Image { .. } => ContentType::Image,
            Self::File { .. } => ContentType::File,
            Self::Video { .. } => ContentType::Video,
            Self::Audio { .. } => ContentType::Audio,
            Self::Service { .. } => ContentType::Service,
            Self::Poll { .. } => ContentType::Poll,
            Self::UnableToDecrypt { .. } => ContentType::UnableToDecrypt,
        }
    }

    /// The media body's location: an `mxc://` URI on a received event, a local
    /// filesystem path on one being sent.
    pub fn media_url(&self) -> Option<&str> {
        match self {
            Self::Image { url, .. } | Self::File { url, .. } | Self::Video { url, .. } => Some(url),
            Self::Audio { info } => Some(&info.url),
            _ => None,
        }
    }
}

// --- Registration types ---

/// A registration request from the UI layer.
#[derive(Debug, Clone)]
pub struct RegistrationRequest {
    pub homeserver: String,
    pub username: String,
    pub password: String,
    /// UIA session ID from a previous challenge (empty for initial attempt).
    pub session: Option<String>,
    /// Auth data for a UIA stage submission (JSON string).
    pub auth_json: Option<String>,
}

/// Result of a registration attempt: either success or a UIA challenge.
#[derive(Debug, Clone)]
pub enum RegistrationResult {
    /// Registration succeeded. Profile and session are now active.
    Success(UserProfile),
    /// Server requires additional authentication stages.
    Challenge(RegistrationChallenge),
}

/// UIA challenge returned by the server when registration requires more stages.
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct RegistrationChallenge {
    /// UIA session identifier.
    pub session: String,
    /// Available authentication flows. Each flow is a list of stage type strings.
    pub flows: Vec<Vec<String>>,
    /// Stages already completed in this session.
    pub completed: Vec<String>,
    /// Server-provided parameters per stage (JSON object).
    /// Key = stage type, value = JSON string of params.
    pub params: std::collections::HashMap<String, String>,
    /// Optional error code from the server (e.g. "M_FORBIDDEN").
    pub errcode: Option<String>,
    /// Optional human-readable error message.
    pub error: Option<String>,
}

/// Result of a username availability check.
#[derive(Debug, Clone)]
pub enum UsernameAvailability {
    Available,
    Unavailable,
    Invalid,
    Error(String),
}

// --- Account settings types ---

/// Server-reported capabilities for account management.
#[derive(Debug, Clone)]
pub struct AccountCapabilities {
    pub can_change_password: bool,
    pub can_change_3pid: bool,
    pub can_set_display_name: bool,
    pub can_set_avatar_url: bool,
}

impl Default for AccountCapabilities {
    fn default() -> Self {
        // Missing capability => enabled.
        Self {
            can_change_password: true,
            can_change_3pid: true,
            can_set_display_name: true,
            can_set_avatar_url: true,
        }
    }
}

/// Combined account summary for the settings page.
#[derive(Debug, Clone)]
pub struct AccountSummary {
    pub user_id: String,
    pub display_name: String,
    pub avatar_url: Option<String>,
    pub capabilities: AccountCapabilities,
}

/// Medium type for third-party identifiers.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum ThreePidMedium {
    Email = 0,
    Msisdn = 1,
}

/// A third-party identifier bound to the account.
#[derive(Debug, Clone)]
pub struct ThreePid {
    pub medium: ThreePidMedium,
    pub address: String,
    pub validated_at: Option<u64>,
    pub added_at: Option<u64>,
}

/// Response from requesting a 3PID verification token.
#[derive(Debug, Clone)]
pub struct ThreePidTokenResponse {
    pub sid: String,
    pub submit_url: Option<String>,
}

/// Result of a UIA-capable account action (change password, deactivate, add 3PID).
#[derive(Debug, Clone)]
pub struct AccountActionResult {
    pub completed: bool,
    pub error_message: Option<String>,
    /// If not completed and no error, a UIA challenge is in progress.
    /// `uia_session` holds the session ID, `uia_flows_json` holds the flows.
    pub uia_session: Option<String>,
    pub uia_flows_json: Option<String>,
}

// --- Sessions + Encryption types ---

/// Verification state of a device/session.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum DeviceVerificationState {
    Verified = 0,
    Unverified = 1,
    Unverifiable = 2,
}

/// A single device/session owned by the current user.
#[derive(Debug, Clone)]
pub struct DeviceSession {
    pub device_id: String,
    pub display_name: Option<String>,
    pub is_current: bool,
    pub is_dehydrated: bool,
    pub last_seen_ts: Option<u64>,
    pub last_seen_ip: Option<String>,
    pub last_seen_user_agent: Option<String>,
    pub app_name: Option<String>,
    pub app_version: Option<String>,
    pub device_model: Option<String>,
    pub os: Option<String>,
    pub browser: Option<String>,
    pub verification_state: DeviceVerificationState,
}

/// List of own devices with the current device ID highlighted.
#[derive(Debug, Clone)]
pub struct DeviceSessionList {
    pub current_device_id: String,
    pub sessions: Vec<DeviceSession>,
}

/// Result of attempting to delete device(s), possibly requiring UIA.
#[derive(Debug, Clone)]
pub struct DeleteDevicesResult {
    pub completed: bool,
    pub challenge_json: Option<String>,
    /// Web account-management URL to sign out the session, set when the server
    /// (MAS/OAuth) rejects the legacy device-delete endpoint as unrecognized.
    pub account_management_url: Option<String>,
}

/// Overall encryption health state: Ok, VerifyThisSession, SetUpRecovery,
/// KeyStorageOutOfSync, TurnOnKeyStorage, IdentityNeedsReset.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum EncryptionHealthState {
    Ok = 0,
    VerifyThisSession = 1,
    SetUpRecovery = 2,
    KeyStorageOutOfSync = 3,
    TurnOnKeyStorage = 4,
    IdentityNeedsReset = 5,
}

/// Snapshot of the current encryption/crypto state for the settings UI.
#[derive(Debug, Clone)]
pub struct EncryptionOverview {
    pub device_id: String,
    pub device_ed25519: Option<String>,
    pub is_current_device_verified: bool,
    pub cross_signing_ready: bool,
    pub cross_signing_keys_cached_locally: bool,
    pub cross_signing_keys_in_secret_storage: bool,
    pub secret_storage_ready: bool,
    pub secret_storage_default_key_id: Option<String>,
    pub key_backup_upload_active: bool,
    pub backup_key_cached: bool,
    pub backup_key_stored_in_4s: bool,
    pub backup_disabled_account_flag: bool,
    pub recovery_disabled_account_flag: bool,
    // Historical UTDs are recoverable: device verified && key backup usable.
    pub history_decryptable: bool,
    pub health_state: EncryptionHealthState,
}

/// Result of resetting cryptographic identity, possibly requiring UIA.
#[derive(Debug, Clone)]
pub struct ResetIdentityResult {
    pub completed: bool,
    pub challenge_json: Option<String>,
}

/// Result of importing E2E encryption keys from a file.
#[derive(Debug, Clone)]
pub struct ImportKeysResult {
    pub imported_count: u32,
    pub total_count: u32,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn preview_type_from_u32_out_of_range_falls_back_to_article() {
        // The only branch worth pinning: an unknown FFI discriminant must map to
        // `Article` (the safe "generic link card"), never `None`. The in-range
        // values are a trivial 1:1 table and are intentionally not enumerated.
        assert_eq!(PreviewType::from_u32(8), PreviewType::Article);
        assert_eq!(PreviewType::from_u32(u32::MAX), PreviewType::Article);
        // Sanity that 0 is still the genuine `None`, not the fallback.
        assert_eq!(PreviewType::from_u32(0), PreviewType::None);
    }

    #[test]
    fn user_trust_state_prioritizes_violation_over_verified() {
        assert_eq!(
            UserTrustState::from_flags(false, false),
            UserTrustState::Unverified
        );
        assert_eq!(
            UserTrustState::from_flags(true, false),
            UserTrustState::Verified
        );
        assert_eq!(
            UserTrustState::from_flags(false, true),
            UserTrustState::Violation
        );
        // A violation must win even if is_verified() also reports true, so a
        // changed-key peer never renders as a plain green "verified" shield.
        assert_eq!(
            UserTrustState::from_flags(true, true),
            UserTrustState::Violation
        );
    }
}

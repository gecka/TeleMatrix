// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <QVector>

#include "timeline_item_content.h"

namespace TeleMatrix {

inline constexpr int kDefaultSearchLimit = 50;
inline constexpr quint16 kDefaultTimelinePageLimit = 50;

/// Notification mode for a room.
enum class RoomNotificationMode : int {
    AllMessages = 0,
    MentionsOnly = 1,
    Mute = 2,
};

/// Whether a room counts as "muted" for the unread-count badge and folder counters. Any
/// non-default notification level qualifies — Mentions-only as well as fully Muted (or an
/// explicit isMuted flag) — so a chat the user has turned down at all can be excluded from
/// the counters.
[[nodiscard]] inline bool roomCountsAsMuted(RoomNotificationMode mode, bool isMuted) {
    return isMuted || mode != RoomNotificationMode::AllMessages;
}

/// Account-global default categories for the "Notifications for chats" settings.
enum class NotificationCategory : int {
    PrivateChats = 0,
    Rooms = 1,
};

/// "Mentions & keywords" master toggles (each backed by a default push rule).
enum class NotificationToggle : int {
    DisplayName = 0,
    Username = 1,
    Room = 2,
    Keywords = 3,
};

enum class InitialDialogsLoadState : int {
    NotStarted = 0,
    Loading = 1,
    Ready = 2,
};

struct ReactionInfo {
    QString key;
    int count = 0;
    bool isSelf = false;

    bool operator==(const ReactionInfo &other) const {
        return key == other.key
            && count == other.count
            && isSelf == other.isSelf;
    }
};

struct FolderInfo {
    int filterId = 0;
    QString displayName;
    int unreadCount = 0;
    bool unreadMuted = false;
    /// The durable identity of a custom folder: its `u.*` Matrix tag key. Empty
    /// for the built-in All/Personal/Unread filters and for spaces.
    QString sectionKey;
    /// A joined space rendered as a folder-like tab. When true, filtering is by
    /// `spaceId` (not `filterId`), and the tab shows the space avatar/glyph.
    bool isSpace = false;
    QString spaceId;
    QString avatarUrl; // space logo mxc, empty if none
};

/// A joined Matrix space surfaced in the left rail.
struct SpaceInfo {
    QString roomId;
    QString displayName;
    QString avatarUrl;
    int unreadCount = 0;
    bool unreadMuted = false;
    QString topic;
    quint64 memberCount = 0;
    QString canonicalAlias;
};

/// One entry in the unified sidebar order (folders + spaces interleaved).
struct SidebarEntry {
    bool isSpace = false;
    /// Folder ⇒ the `u.*` tag key; space ⇒ the space room id.
    QString key;

    bool operator==(const SidebarEntry &other) const {
        return isSpace == other.isSpace && key == other.key;
    }
    bool operator!=(const SidebarEntry &other) const { return !(*this == other); }
};

/// A user's profile information.
struct UserProfile {
    QString userId;
    QString displayName;
    QString avatarUrl; // empty if no avatar
};

/// Presence state for a user.
enum class PresenceState : int {
    Offline = 0,
    Online = 1,
    Unavailable = 2,
};

/// Membership state within a room.
enum class MembershipState : int {
    Join = 0,
    Invite = 1,
    Leave = 2,
    Ban = 3,
    Knock = 4,
};

/// Role derived from power level.
enum class MemberRole : int {
    Administrator = 0,
    Moderator = 1,
    User = 2,
};

/// Cross-signing trust state of another user (mirrors Rust `UserTrustState`).
enum class UserTrustState : int {
    Unverified = 0,
    Verified = 1,
    Violation = 2,
    // Identity verified, but the user has an unverified session — trust is
    // downgraded to a caution rather than a clean verified check.
    VerifiedWithWarning = 3,
};

/// Detailed user profile for the profile popup.
struct UserProfileDetails {
    QString roomId;
    QString userId;
    QString displayName;
    QString avatarUrl;
    PresenceState presence = PresenceState::Offline;
    qint64 lastActiveTs = 0;
    MembershipState membership = MembershipState::Join;
    qint64 powerLevel = 0;
    MemberRole role = MemberRole::User;
    bool isIgnored = false;
    QString dmRoomId;
    bool canInvite = false;
    bool canKick = false;
    bool canBan = false;
    bool canMute = false;
    bool canChangePowerLevel = false;
    qint64 maxAssignablePowerLevel = -1;
    /// Cross-signing trust of this user, for the trust shield.
    UserTrustState trustState = UserTrustState::Unverified;
};

/// Detailed info about a room member, including permission flags.
struct RoomMemberInfo {
    QString userId;
    QString displayName;
    QString avatarUrl;
    MembershipState membership = MembershipState::Join;
    qint64 powerLevel = 0;
    MemberRole role = MemberRole::User;
    bool isSelf = false;
    bool canBeRemovedByMe = false;
    bool canBeBannedByMe = false;
    bool canBeUnbannedByMe = false;
    /// Cross-signing trust of this member, for the trust shield.
    UserTrustState trustState = UserTrustState::Unverified;
};

/// Snapshot of a room's members with actor permission flags.
struct RoomMembersSnapshot {
    QString roomId;
    QString myUserId;
    bool canInvite = false;
    bool canRemoveAny = false;
    QVector<RoomMemberInfo> members;
};

/// Summary of a room for display in the chat list.
struct RoomSummary {
    QString roomId;
    QString displayName;
    QString canonicalAlias;
    QString avatarUrl; // empty if no avatar
    QString avatarEntityId; // user ID for DM fallback, else room ID
    QString lastMessage;
    QString lastSender;
    qint64 timestamp = 0; // seconds since UNIX epoch
    int unreadCount = 0;
    bool isMarkedUnread = false;
    int highlightCount = 0;
    RoomNotificationMode notificationMode = RoomNotificationMode::AllMessages;
    bool isMuted = false;
    bool isPinned = false;
    /// `order` from the room's `m.favourite` tag: where the user put this room among
    /// their pinned ones. Negative when the server holds no order for it (a room
    /// pinned before the app started recording one). Lower sorts higher.
    double pinnedOrder = -1.0;
    bool isDirect = false;
    /// The room's join rule is public — known at room-open time (no async settings fetch needed),
    /// used to decide whether to hide system messages without a flicker.
    bool isPublic = false;
    QVector<int> filterIds;
    /// Room ids of the joined spaces this room belongs to (recursively).
    QStringList spaceIds;
    bool isLastMessageOutgoing = false;
    bool isLastMessageService = false;
    SendState lastMessageSendState = SendState::Sent;
    quint64 memberCount = 0;
    bool canPinMessages = false;
    int peerPresence = 0; // 0=offline, 1=online, 2=unavailable (DM rooms only)
    MembershipState membership = MembershipState::Join;
    QString inviterUserId;
    QString inviterDisplayName;
    QString inviterAvatarUrl;
    QString roomTopic;
};

struct RoomUnreadSnapshot {
    int unreadCount = 0;
    int highlightCount = 0;
    RoomNotificationMode notificationMode = RoomNotificationMode::AllMessages;
    bool isMuted = false;
    bool isMarkedUnread = false;
};

/// Lightweight per-room notification fields, indexed by room id for O(1) lookup
/// (avoids scanning + copying the whole RoomSummary vector per unread snapshot).
struct RoomNotifInfo {
    RoomNotificationMode notificationMode = RoomNotificationMode::AllMessages;
    bool isMuted = false;
    bool isMarkedUnread = false;
};

/// A single message in a room timeline.
struct TimelineItem {
    QString eventId;
    QString transactionId;
    TimelineSenderInfo sender;
    qint64 timestamp = 0;     // seconds since UNIX epoch
    TimelineContent content;
    std::optional<TimelineReplyInfo> reply;
    std::optional<TimelineForwardInfo> forwardedFrom;
    std::optional<TimelineUrlPreviewInfo> urlPreview;
    TimelineEncryptionInfo encryption;
    TimelineDeliveryInfo delivery;
    QVector<ReactionInfo> reactions;
    bool isEdited = false;
    bool isPinned = false;
};

// --- Timeline Navigation Types ---

enum class TimelineUpdateKind {
    Full = 0,
    Append = 1,
    Prepend = 2,
    Replace = 3,
    MetadataOnly = 4,
};

/// A slice of the timeline returned by getTimelineSlice, including pagination metadata.
struct TimelineSlice {
    QVector<TimelineItem> items;
    TimelineUpdateKind updateKind = TimelineUpdateKind::Full;
    int updateIndex = 0;
    bool canPaginateBack = false;
    bool canPaginateForward = false;
    bool hitTimelineStart = false;
    bool isLive = true;
    QString focusEventId;  // empty when live
    QStringList pinnedEventIds;  // event IDs of pinned messages from room state
    QString firstUnreadEventId;  // event ID of the first unread message (empty if no unreads)
    // Whether the read marker is present in this loaded window. When true,
    // firstUnreadEventId is the confirmed first-unread and the delimiter may be
    // placed at once; when false it is a best-effort guess (marker older than the
    // window) and placement should page back to the true boundary first.
    bool readMarkerLoaded = false;
    int unreadCount = 0;         // server-reported unread count for this room
    bool unreadStateKnown = false; // false for fallback/transport-empty slices
};

/// Persisted scroll position for a room, used to restore position on re-entry.
struct RoomScrollState {
    QString anchorEventId;
    int anchorPixelOffset = 0;
    bool wasLive = true;
    QString focusEventId;
};

// --- Room Settings Snapshot ---

/// History visibility state for a room.
enum class HistoryVisibility : int {
    Joined = 0,
    Invited = 1,
    Shared = 2,
    WorldReadable = 3,
    Unknown = 4,
};

enum class RoomAccess : int {
    InviteOnly = 0,
    Public = 1,
    Knock = 2,
    Restricted = 3,
    KnockRestricted = 4,
    Private = 5,
    Unknown = 6,
};

/// A snapshot of a room's settings and security state.
struct RoomSettingsSnapshot {
    QString roomId;
    QString displayName;
    QString canonicalAlias;
    RoomNotificationMode notificationMode = RoomNotificationMode::AllMessages;
    bool isMuted = false;
    quint64 memberCount = 0;
    bool isEncrypted = false;
    QString encryptionAlgorithm;
    RoomAccess access = RoomAccess::Unknown;
    HistoryVisibility historyVisibility = HistoryVisibility::Unknown;
    bool newMembersCanSeeHistory = false;
    bool canInvite = false;
    bool canKick = false;
    bool canBan = false;
    bool canChangeAvatar = false;
    bool canChangeName = false;
    bool canChangeTopic = false;
    bool canChangeEncryption = false;
    bool canChangeAccess = false;
    bool canChangeHistoryVisibility = false;
};

// --- Search domain types (Phase 1) ---

/// Search scope: single room or all rooms.
enum class SearchScope : int {
    Room = 0,
    AllRooms = 1,
};

/// A search request sent to the backend.
struct SearchRequest {
    quint64 requestId = 0;
    SearchScope scope = SearchScope::Room;
    QString roomId;        // required for Room scope
    QString query;
    int limit = kDefaultSearchLimit;
    QString nextToken;     // empty for first page
    QString senderFilter;  // empty for no sender filtering
};

/// A single search result hit.
struct SearchHit {
    QString roomId;
    QString eventId;
    QString senderId;
    QString senderName;
    qint64 timestamp = 0;
    QString snippet;       // contextual text around the match
    int rank = 0;
    bool localOnly = false;
};

/// A page of search results.
struct SearchPage {
    quint64 requestId = 0;
    QVector<SearchHit> hits;
    int totalApprox = 0;
    QString nextToken;     // empty when no more pages
    bool done = false;
    bool e2eeDisabled = false; // empty because E2EE-room search is disabled
    bool indexing = false;     // E2EE room still building its local search index
};

/// How a room may be entered. Discriminants mirror Rust's `RoomDirectoryJoinRule`.
enum class RoomDirectoryJoinRule : int {
    Public = 0,
    Knock = 1,
    Invite = 2,
    Restricted = 3,
    KnockRestricted = 4,
    Private = 5,
    Unknown = 6,
};

/// Our membership in a discovered room. Discriminants mirror Rust's `RoomMembershipState`.
enum class RoomMembership : int {
    None = 0,
    Invited = 1,
    Joined = 2,
    Left = 3,
    Knocked = 4,
    Banned = 5,
};

/// One room or space from a directory search or a space's child list. Both sources render as the
/// same row, so they share a type.
struct RoomDirectoryEntry {
    QString roomId;
    QString name;          // falls back to the alias, then the room ID
    QString topic;
    QString canonicalAlias;
    QString avatarUrl;
    int memberCount = 0;
    int childrenCount = 0; // only meaningful for a space
    bool isSpace = false;
    bool worldReadable = false;
    bool guestCanJoin = false;
    RoomDirectoryJoinRule joinRule = RoomDirectoryJoinRule::Unknown;
    RoomMembership membership = RoomMembership::None;
    QStringList via;       // server hints for a federated join
};

/// A page of directory or space-hierarchy results.
struct RoomDirectoryPage {
    quint64 requestId = 0;
    QVector<RoomDirectoryEntry> entries;
    int totalApprox = -1;  // -1 when the server gave no estimate
    QString nextToken;     // empty when there are no more pages
    bool done = false;
    /// Set by the bridge, not the backend: which request this page answers, so the dialog can route
    /// it to the right view without a second signal.
    bool isSpaceChildren = false;
    QString spaceId;
};

/// What can be shown about a room before joining it. Matrix serves no history to a non-member, so
/// this is the whole content of the preview screen.
struct RoomPreviewInfo {
    QString roomId;
    QString name;
    QString topic;
    QString canonicalAlias;
    QString avatarUrl;
    int memberCount = 0;
    bool isSpace = false;
    RoomDirectoryJoinRule joinRule = RoomDirectoryJoinRule::Unknown;
    RoomMembership membership = RoomMembership::None;
    /// History is readable without joining — the only case where a preview shows messages.
    bool worldReadable = false;
};

/// Guest access policy for room creation.
enum class CreateRoomGuestAccess : int {
    Forbidden = 0,
    CanJoin = 1,
};

/// History visibility for room creation.
enum class CreateRoomHistoryVisibility : int {
    Joined = 0,    // Only from the point they joined
    Invited = 1,   // Only from the point they were invited
    Shared = 2,    // All members (including future)
    WorldReadable = 3, // Anyone
};

/// Request to create a new room.
struct CreateRoomRequest {
    QString name;
    QString topic;       // empty = no topic
    bool isPublic = false;
    bool encrypted = true;
    QString alias;       // room alias local part (empty = no alias)
    QString avatarPath;  // local file path for avatar (empty = none)
    CreateRoomGuestAccess guestAccess = CreateRoomGuestAccess::Forbidden;
    CreateRoomHistoryVisibility historyVisibility = CreateRoomHistoryVisibility::Shared;
    bool federate = true; // false sets m.federate=false in m.room.create
};

// --- Account settings types ---

/// Server-reported capabilities for account management.
struct AccountCapabilities {
	bool canChangePassword = true;
	bool canChange3pid = true;
	bool canSetDisplayName = true;
	bool canSetAvatarUrl = true;
};

/// Combined account summary for the settings page.
struct AccountSummary {
	QString userId;
	QString displayName;
	QString avatarUrl;
	AccountCapabilities capabilities;
};

/// Medium type for third-party identifiers.
enum class ThreePidMedium : int {
	Email = 0,
	Msisdn = 1,
};

/// A third-party identifier bound to the account.
struct ThreePid {
	ThreePidMedium medium = ThreePidMedium::Email;
	QString address;
	quint64 validatedAt = 0;
	quint64 addedAt = 0;
};

/// Response from requesting a 3PID verification token.
struct ThreePidTokenResponse {
	QString sid;
	QString submitUrl;
};

/// Result of a UIA-capable account action.
struct AccountActionResult {
	bool completed = false;
	QString errorMessage;
	QString uiaSession;
	QString uiaFlowsJson;
};

// --- Sessions + Encryption types ---

/// Verification state of a device/session.
enum class DeviceVerificationState : int {
    Verified = 0,
    Unverified = 1,
    Unverifiable = 2,
};

/// A single device/session owned by the current user.
struct DeviceSession {
    QString deviceId;
    QString displayName;
    bool isCurrent = false;
    bool isDehydrated = false;
    qint64 lastSeenTs = 0;       // UNIX epoch seconds, 0 if unknown
    bool hasLastSeenTs = false;
    QString lastSeenIp;
    QString lastSeenUserAgent;
    QString appName;
    QString appVersion;
    QString deviceModel;
    QString os;
    QString browser;
    DeviceVerificationState verificationState = DeviceVerificationState::Unverified;
};

/// List of own devices with the current device ID highlighted.
struct DeviceSessionList {
    QString currentDeviceId;
    QVector<DeviceSession> sessions;
};

/// Result of attempting to delete device(s), possibly requiring UIA.
struct DeleteDevicesResult {
    bool completed = false;
    QString challengeJson;         // non-empty when server requires UIA
    QString accountManagementUrl;  // non-empty when MAS/OAuth needs the web portal
};

/// Encryption health state — device key-storage and verification status.
enum class EncryptionHealthState : int {
    Ok = 0,
    VerifyThisSession = 1,
    SetUpRecovery = 2,
    KeyStorageOutOfSync = 3,
    TurnOnKeyStorage = 4,
    IdentityNeedsReset = 5,
};

/// Snapshot of the current encryption/crypto state for settings UI.
struct EncryptionOverview {
    QString deviceId;
    QString deviceEd25519;
    bool isCurrentDeviceVerified = false;
    bool crossSigningReady = false;
    bool crossSigningKeysCachedLocally = false;
    bool crossSigningKeysInSecretStorage = false;
    bool secretStorageReady = false;
    QString secretStorageDefaultKeyId;
    bool keyBackupUploadActive = false;
    bool backupKeyCached = false;
    bool backupKeyStoredIn4s = false;
    bool backupDisabledAccountFlag = false;
    bool recoveryDisabledAccountFlag = false;
    bool historyDecryptable = false; // verified && key backup usable
    EncryptionHealthState healthState = EncryptionHealthState::Ok;
};

/// Result of resetting cryptographic identity, possibly requiring UIA.
struct ResetIdentityResult {
    bool completed = false;
    QString challengeJson;
};

/// Result of importing E2E encryption keys from a file.
struct ImportKeysResult {
    int importedCount = 0;
    int totalCount = 0;
};

} // namespace TeleMatrix

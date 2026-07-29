// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "media/video_container.h"

#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QPair>
#include <QVector>

#include <map>
#include <memory>
#include <atomic>
#include <thread>
#include <QMutex>
#include <QSet>

#include "protocol_types.h"

// Forward-declare the Rust FFI handle type.
struct Handle;
struct TimelineCallbackData;
struct BridgeCallbackGuard;

namespace TeleMatrix {

/// Cache storage statistics returned by getCacheStats().
struct CacheStats {
    quint64 mediaFilesBytes = 0;
    quint64 previewCacheBytes = 0;
    quint64 appCacheBytes = 0;
    quint64 searchIndexBytes = 0;
    quint64 totalBytes = 0;
    quint64 mediaFileCount = 0;
};

/// C++ bridge wrapping the Rust protocol library via FFI.
///
/// Thread safety: all public methods must be called from the Qt main thread.
/// Rust async callbacks are marshaled to the main thread via QueuedConnection.
class ProtocolBridge : public QObject {
    Q_OBJECT

public:
    /// Create the bridge with the Matrix backend.
    /// dataDir is the path for persistent storage.
    explicit ProtocolBridge(const QString &dataDir, QObject *parent = nullptr);
    ~ProtocolBridge() override;

    // Non-copyable, non-movable (QObject)
    ProtocolBridge(const ProtocolBridge &) = delete;
    ProtocolBridge &operator=(const ProtocolBridge &) = delete;

    /// Asynchronously log in. Result arrives via loginResult signal.
    void login(const QString &homeserver, const QString &user, const QString &pass);

    /// Generate a bridge-wide request token for async snapshot calls.
    quint64 nextRequestId();

    /// Blocking startup-only room list read. Interactive UI must use
    /// cachedRooms() plus getRoomsAsync().
    QVector<RoomSummary> getRoomsBlockingForStartupOnly();

    // Recent emojis (server-synced via io.element.recent_emoji account data).
    // setRecentEmoji persists the full ordered (emoji, count) list; the startup
    // getter blocks on the local app_cache.db mirror for instant display.
    void setRecentEmoji(const QVector<QPair<QString, int>> &pairs);
    [[nodiscard]] QVector<QPair<QString, int>> recentEmojiForStartup();
    /// Asynchronously get the current room list. Result arrives via roomsReady.
    void getRoomsAsync(quint64 requestId = 0);
    /// Asynchronously get unread counters for a room without consuming timeline updates.
    void getRoomUnreadSnapshotAsync(const QString &roomId, quint64 requestId = 0);
    /// Return the latest room snapshot already copied into the Qt process.
    QVector<RoomSummary> cachedRooms() const;
    /// O(1) per-room notification fields from the cached snapshot (no vector copy).
    RoomNotifInfo roomNotifInfo(const QString &roomId) const;

    /// Asynchronously get the current timeline slice with pagination metadata.
    /// Result arrives via timelineSliceReady.
    void getTimelineSliceAsync(const QString &roomId, quint64 requestId = 0);
    /// Asynchronously get the latest incremental timeline update.
    /// Result arrives via timelineSliceReady.
    void getTimelineUpdateAsync(const QString &roomId, quint64 requestId = 0);
    /// Request backward pagination (fire-and-forget; timelineChanged fires when done).
    void paginateBack(const QString &roomId, quint16 count = kDefaultTimelinePageLimit);
    /// Request forward pagination (fire-and-forget; timelineChanged fires when done).
    void paginateForward(const QString &roomId, quint16 count = kDefaultTimelinePageLimit);
    /// Jump the timeline to a specific event. Result arrives via focusOnEventResult signal.
    void focusOnEvent(
        const QString &roomId,
        const QString &eventId,
        quint64 requestId = 0);
    /// Return the timeline to the live end (fire-and-forget; timelineChanged fires when done).
    void returnToLive(const QString &roomId);
    // Drop a no-longer-viewed room's resident Rust timeline state (window +
    // per-room caches) to bound memory across a long session. Best effort.
    void releaseRoomTimeline(const QString &roomId);
    /// Cancel an in-progress media upload by transaction ID.
    void cancelUpload(const QString &roomId, const QString &transactionId);
    /// Fetch pinned messages asynchronously. Result arrives via pinnedMessagesFetched signal.
    void getPinnedMessagesAsync(const QString &roomId);
    /// Unpin all messages in a room (single state event, async).
    void unpinAllMessages(const QString &roomId);

    /// Asynchronously get the member list for a room.
    /// Result arrives via roomMembersReady signal.
    void getRoomMembersAsync(const QString &roomId);
    /// Asynchronously get a detailed member snapshot with permission flags.
    /// Result arrives via roomMembersSnapshotReady signal.
    void getRoomMembersSnapshotAsync(const QString &roomId, bool forceRefresh = false);
    /// Asynchronously search the homeserver user directory.
    void searchUserDirectory(const QString &query, int limit = 50);

    /// Asynchronously get detailed user profile for the profile popup.
    /// Result arrives via userProfileDetailsReady signal.
    void getUserProfileDetailsAsync(const QString &roomId, const QString &userId);
    /// Asynchronously set a member's room power level. Result arrives via userPowerLevelSet.
    void setUserPowerLevel(const QString &roomId, const QString &userId, qint64 powerLevel);
    /// Asynchronously open or create a direct room. Result arrives via directRoomCreated.
    void createDirectRoom(const QString &userId);

    /// Cached Saved Messages room id; empty when no saved room exists (never
    /// created, or permanently deleted).
    [[nodiscard]] QString savedMessagesRoomId() const { return _savedMessagesRoomId; }

    /// Async. `create` (an explicit open / forward) creates + mutes the room on
    /// first use; without it (a passive session start) this only adopts an
    /// existing room and never creates. Result via savedMessagesRoomReady (an
    /// empty roomId means there is no saved room).
    void ensureSavedMessagesRoom(bool create = false);

    /// Permanently delete Saved Messages: leaves + forgets the room and clears
    /// the marker, so the id goes empty and the room drops from the list.
    void deleteSavedMessages();

    /// Asynchronously kick a user from a room. Result arrives via userKicked.
    void kickUser(const QString &roomId, const QString &userId, const QString &reason = QString());
    /// Asynchronously ban a user from a room. Result arrives via userBanned.
    void banUser(const QString &roomId, const QString &userId, const QString &reason = QString());
    /// Asynchronously unban a user from a room. Result arrives via userUnbanned.
    void unbanUser(const QString &roomId, const QString &userId);
    /// Asynchronously invite a user to a room. Result arrives via userInvited.
    void inviteUser(const QString &roomId, const QString &userId);
    /// Asynchronously set or unset a user as ignored. Result arrives via userIgnoredSet.
    void setUserIgnored(const QString &userId, bool ignored);

    /// Asynchronously search messages. Result arrives via searchPageReady signal.
    void searchMessagesAsync(const SearchRequest &request);

    /// Cancel an active async search request by ID.
    void cancelSearch(quint64 requestId);

    /// Search the homeserver's public room directory by name/topic/alias. An empty query browses
    /// the whole directory. Spaces are included. Result arrives via roomDirectoryPageReady.
    void searchPublicRoomsAsync(
        quint64 requestId,
        const QString &query,
        int limit,
        const QString &nextToken = QString());

    /// One page of a space's immediate children. There is no server-side search inside a space, so
    /// callers filter these locally. Result arrives via roomDirectoryPageReady.
    void getSpaceChildrenAsync(
        quint64 requestId,
        const QString &spaceId,
        int limit,
        const QString &nextToken = QString());

    /// Cancel a pending directory/hierarchy request by ID.
    void cancelRoomDirectoryRequest(quint64 requestId);

    /// What can be shown about a room before joining it. Result arrives via roomPreviewReady.
    void getRoomPreview(const QString &roomIdOrAlias, const QStringList &via = QStringList());

    /// Read a page of history for an unjoined room, paginating backward. `from` is empty for the
    /// newest page, else the previous result's next-token. Only world-readable rooms return
    /// messages; others fail (the caller falls back to the name+topic placeholder). Result arrives
    /// via roomPreviewMessagesReady.
    void previewMessages(const QString &roomId, const QString &from, int limit);

    /// Join a room by ID or alias. Result arrives via roomJoined.
    void joinRoom(const QString &roomIdOrAlias, const QStringList &via = QStringList());

    /// Knock on a room (request to join) by ID or alias. Result arrives via roomKnocked.
    void knockRoom(const QString &roomIdOrAlias, const QStringList &via = QStringList());

    /// Asynchronously send a message. Result arrives via messageSent signal.
    void sendMessage(
        const QString &roomId,
        const QString &body,
        const QString &formattedBody = QString(),
        const QString &replyToEventId = QString(),
        quint64 requestId = 0);

    /// Asynchronously edit a message. Result arrives via messageEdited. When `asMediaCaption` is
    /// true the target is a media message and only its caption changes (an empty body clears the
    /// caption) — the file is kept instead of being replaced by a text message.
    void editMessage(
        const QString &roomId,
        const QString &eventId,
        const QString &body,
        const QString &formattedBody = QString(),
        bool asMediaCaption = false);

    /// Asynchronously delete a message. Result arrives via messageDeleted.
    void deleteMessage(const QString &roomId, const QString &eventId);

    /// Asynchronously pin or unpin a message. Result arrives via messagePinned.
    void pinMessage(const QString &roomId, const QString &eventId, bool pinned);

    /// Persist a locally-learned audio duration (ms) for a media mxc URL, so the
    /// audio bubble can show the length on later loads without re-probing.
    void setAudioDuration(const QString &mxcUrl, quint64 durationMs);

    /// Homeserver maximum upload size in bytes, or 0 if not yet known. Used to
    /// reject oversized files before starting an upload.
    [[nodiscard]] quint64 maxUploadSize() const;
    /// Asynchronously pin or unpin a room in dialogs list.
    /// Pin/unpin a room. `order` is its place among the pinned rooms, stored in the
    /// `m.favourite` tag so it survives a re-login; negative records no place.
    void pinRoom(const QString &roomId, bool pinned, double order = -1.0);

    /// Store the pinned rooms' arrangement (top first) on the server.
    void setPinnedOrder(const QVector<QString> &roomIds);
    /// Asynchronously set the notification mode for a room.
    void setRoomNotificationMode(const QString &roomId, RoomNotificationMode mode);
    /// Read the account-global "Notifications for chats" settings → notificationSettingsReady.
    void getNotificationSettings();
    /// Set a chat category's default notification level (AllMessages / MentionsOnly).
    void setCategoryNotificationLevel(NotificationCategory category, RoomNotificationMode level);
    /// Reconcile the user's keyword rules to the comma-separated list.
    void setKeywords(const QString &keywordsCsv);
    /// Toggle a "Mentions & keywords" master switch → notificationSettingsSaved.
    void setNotificationToggle(NotificationToggle toggle, bool enabled);
    /// Asynchronously mark a room read/unread. Returns a local request token.
    quint64 markRoomRead(const QString &roomId, bool read);
    /// Asynchronously send a read receipt for a specific event. Returns a local request token.
    quint64 sendReadReceipt(const QString &roomId, const QString &eventId);
    /// Send a typing notice for the current user in a room.
    void sendTypingNotice(const QString &roomId, bool typing);
    /// Asynchronously leave a room.
    void leaveRoom(const QString &roomId);
    /// Asynchronously accept a room invite. Result arrives via inviteAccepted signal.
    void acceptInvite(const QString &roomId);
    /// Asynchronously create a new room. Result arrives via roomCreated signal.
    void createRoom(const CreateRoomRequest &request);
    /// Asynchronously toggle a room's membership in a folder section (`u.*` tag).
    void addRoomToFolder(const QString &roomId, const QString &sectionKey);

    /// Asynchronously create a new empty folder. folderCreated carries its key.
    void createFolder(const QString &name);
    /// Asynchronously rename a folder (re-tags member rooms).
    void editFolder(const QString &sectionKey, const QString &name);
    /// Asynchronously delete a folder (strips its tag and drops it from the order).
    void deleteFolder(const QString &sectionKey);
    /// Asynchronously persist the unified sidebar order (folders + spaces).
    void setSidebarOrder(const QVector<SidebarEntry> &order);
    /// Asynchronously fetch the unified sidebar order. Result via sidebarOrderReady.
    void getSidebarOrderAsync();
    /// Blocking startup-only custom folder read. Interactive UI must use
    /// cachedCustomFolders() plus getCustomFoldersAsync().
    QVector<FolderInfo> getCustomFoldersBlockingForStartupOnly();
    /// Asynchronously get custom folder metadata. Result arrives via customFoldersReady.
    void getCustomFoldersAsync();
    /// Return the latest custom folder snapshot already copied into the Qt process.
    QVector<FolderInfo> cachedCustomFolders() const;

    /// Asynchronously get the joined spaces. Result arrives via joinedSpacesReady.
    void getJoinedSpacesAsync();
    /// Return the latest joined-spaces snapshot copied into the Qt process.
    QVector<SpaceInfo> cachedJoinedSpaces() const;

    /// Asynchronously forward a message. Result arrives via messageForwarded.
    void forwardMessage(const QString &srcRoomId, const QString &eventId, const QString &dstRoomId);

    /// Asynchronously send media. Result arrives via mediaSent.
    void sendMedia(
        const QString &roomId,
        ContentType type,
        const QString &url,
        const QString &mime,
        const QString &filename,
        const QString &caption = QString(),
        const QString &thumbUrl = QString(),
        quint64 size = 0,
        int width = 0,
        int height = 0,
        quint64 durationMs = 0,
        const QString &transactionId = QString(),
        bool isVoice = false,
        const QByteArray &waveform = QByteArray());

    /// Asynchronously set/unset a reaction. Result arrives via reactionSet.
    void setReaction(const QString &roomId, const QString &eventId, const QString &key, bool active);
    /// Asynchronously send a vote for a poll. Result arrives via pollVoteSent.
    void sendPollVote(
        const QString &roomId,
        const QString &pollEventId,
        const QStringList &optionIds);

    // --- Registration ---

    /// Asynchronously register a new account.
    /// Result arrives via registrationSuccess, registrationChallenge, or registrationFailed.
    void registerAccount(
        const QString &homeserver,
        const QString &username,
        const QString &password,
        const QString &session = QString(),
        const QString &authJson = QString());

    /// Check if a username is available on the homeserver.
    /// Result arrives via usernameAvailabilityChecked.
    void checkUsernameAvailable(const QString &homeserver, const QString &username);

    // --- Password reset ---

    /// Request a password reset email. Generates a client_secret internally.
    /// Result arrives via passwordResetTokenSent signal.
    void requestPasswordReset(const QString &homeserver, const QString &email);

    /// Reset password after email verification.
    /// Result arrives via passwordResetComplete signal.
    void resetPassword(const QString &homeserver, const QString &newPassword,
                       const QString &sid, const QString &clientSecret);

    // --- Session management ---

    /// Asynchronously restore a session from saved tokens (skip login).
    /// Result arrives via sessionRestored signal.
    void restoreSession(
        const QString &homeserver,
        const QString &userId,
        const QString &deviceId,
        const QString &accessToken);

    struct SessionInfo {
        QString homeserver;
        QString userId;
        QString deviceId;
        QString accessToken;
    };
    /// Blocking persistence-only session read. Interactive UI must use cachedSessionInfo().
    SessionInfo getSessionInfoBlockingForPersistence();
    /// Return the latest session snapshot already copied into the Qt process.
    SessionInfo cachedSessionInfo() const;

    /// Asynchronously logout. Result arrives via loggedOut signal.
    void logout();

    /// Force an immediate sliding-sync reconnect, short-circuiting backoff. Called
    /// by the network monitor when the OS reports the interface returned.
    void reconnect();

    /// Asynchronously discover homeserver URL via .well-known.
    /// Result arrives via homeserverDiscovered signal.
    void discoverHomeserver(const QString &domain, quint64 requestId = 0);

    /// Asynchronously classify a homeserver's registration capability (resolve +
    /// validate Matrix server + detect OIDC/MAS delegation) for the two-step
    /// register flow. Result arrives via registrationClassified.
    void classifyRegistration(const QString &input, quint64 requestId = 0);

    /// Asynchronously probe whether a homeserver delegates auth to OIDC/MAS
    /// (accounts created on its website; legacy registration always 403s).
    /// Result arrives via authDelegationProbed signal.
    void probeAuthDelegation(const QString &baseUrl, quint64 requestId = 0);

    /// Asynchronously fetch the website where a delegated-auth homeserver manages
    /// account details (email/phone). Result arrives via accountManagementProbed.
    void probeAccountManagement(const QString &baseUrl, quint64 requestId = 0);

    /// Ask where a delegated-auth (OIDC/MAS) homeserver resets a FORGOTTEN
    /// password. Takes the raw homeserver the user typed and resolves it itself.
    /// Result arrives via passwordResetPageProbed; `available` is false on
    /// servers that reset passwords in-app, where there is nothing to link to.
    void probePasswordResetPage(const QString &homeserver, quint64 requestId = 0);

    /// Asynchronously ask whether the homeserver can verify email addresses at
    /// all, without sending one. Result arrives via emailThreepidSupportProbed.
    void probeEmailThreepidSupport(const QString &baseUrl, quint64 requestId = 0);

    /// Return a loopback HTTP URL for progressive streaming of an mxc:// video.
    /// Returns an empty string when no session is active or streaming is
    /// unavailable (caller falls back to the local-file download path).
    QString videoStreamUrl(const QString &mxcUrl);
    /// Fraction (0.0–1.0) of a streaming video already downloaded by the loopback
    /// proxy; 1.0 when it isn't proxy-streamed (so seeking is unrestricted).
    float videoStreamProgress(const QString &mxcUrl);
    /// Raw downloaded/total bytes for a proxy-streamed video; true if available.
    bool videoStreamProgressBytes(
        const QString &mxcUrl, quint64 &downloaded, quint64 &total);
    /// Whether the proxy's current download for `mxcUrl` has failed outright; false
    /// when no session / not streamed, so the retry loop only fails fast on a real
    /// error.
    bool videoStreamErrored(const QString &mxcUrl);
    /// What the video's container header says about progressive playback. Unknown
    /// until something has read its first bytes (this session's proxy download, or
    /// any earlier session's — the verdict is persisted per mxc).
    VideoContainer videoStreamContainer(const QString &mxcUrl);

    /// Asynchronously resolve an mxc:// URL to a local file path.
    /// Result arrives via mediaResolved signal.
    void resolveMedia(const QString &mxcUrl);

    /// Resolve an avatar via a server thumbnail (small download), delivered on the
    /// same mediaResolved signal and under the same key as resolveMedia — so the
    /// avatar paint path is unchanged. Falls back to the full image server-side if
    /// the server can't thumbnail. Use for any avatar; never needs the full original.
    void resolveAvatar(const QString &mxcUrl);

    /// Asynchronously resolve an mxc:// URL to decrypted in-memory bytes.
    /// Result arrives via mediaBytesResolved signal.
    void resolveMediaBytes(const QString &mxcUrl);

    /// Cancel an in-flight mxc:// media download if the backend still owns it.
    void cancelMediaDownload(const QString &mxcUrl);

    /// Resolve a server-generated thumbnail for a media URL.
    /// Uses the Matrix thumbnail API — fast even for large videos.
    /// Result arrives via mediaResolved signal (same as resolveMedia).
    void resolveMediaThumbnail(const QString &mxcUrl, int width = 640, int height = 480);
    /// Resolve a server-generated thumbnail into decrypted in-memory bytes.
    /// Result arrives via mediaBytesResolved signal under "srvthumb:<mxc>".
    void resolveMediaThumbnailBytes(const QString &mxcUrl, int width = 640, int height = 480);
    /// Extract (and persistently cache) a single frame from a video as a JPEG.
    /// Fast FFmpeg path with an encrypted cache keyed by event id; on a cache
    /// hit no network/decode happens. Result arrives via mediaBytesResolved
    /// under "vidthumb:<eventId>" as an "image/jpeg" payload.
    void getVideoThumbnail(const QString &eventId, const QString &mxcUrl, int width = 640, int height = 480);
    /// Resolve a small OG-card preview image via the thumbnail API while
    /// keeping the original mxc key in MediaCache.
    void resolveMediaPreviewImage(const QString &mxcUrl, int width = 640, int height = 480);
    /// Resolve a small OG-card preview image into memory.
    void resolveMediaPreviewImageBytes(const QString &mxcUrl, int width = 640, int height = 480);

    /// Export remote Matrix media directly to a user-selected path.
    /// Result arrives via mediaExported signal.
    void exportMediaToPath(const QString &mxcUrl, const QString &targetPath);

    // --- Cache management ---

    /// Asynchronously fetch cache size statistics.
    /// Result arrives via cacheStatsReady signal.
    void getCacheStats();
    /// Asynchronously clear media cache with age + size limits.
    /// Result arrives via cacheClearResult signal.
    void clearMediaCache(quint32 maxAgeDays, quint64 sizeLimitBytes);
    /// Asynchronously clear all caches (user-initiated full clear).
    /// Result arrives via cacheClearResult signal.
    void clearAllCaches();
    /// Trigger auto-cleanup if media cache exceeds size limit (fire-and-forget).
    void autoCleanupCache(quint64 sizeLimitBytes);
    /// Push the media-cache size budget (bytes) to the backend: bounds the SDK
    /// thumbnail store and enforces the media + stream cache budgets immediately.
    void setMediaCacheLimit(quint64 limitBytes);
    /// Enable/disable local search indexing of E2EE rooms. Off stops the indexing
    /// workers and wipes the local search DB; on reopens + re-backfills it.
    void setE2eeSearchEnabled(bool enabled);

    /// Current sync state. 0=not started, 1=syncing, 2=synced.
    int syncState() const { return _syncState; }
    void setSyncState(int state);

    /// One-shot dialogs bootstrap state for startup loading UX.
    InitialDialogsLoadState initialDialogsLoadState() const { return _initialDialogsLoadState; }
    void setInitialDialogsLoadState(InitialDialogsLoadState state);

    // --- Account settings ---

    /// Asynchronously fetch account summary. Result arrives via accountSummaryReady.
    void fetchAccountSummary();
    /// Asynchronously set display name. Result arrives via displayNameSet.
    void setDisplayName(const QString &name);
    /// Asynchronously set avatar URL. Result arrives via avatarSet.
    void setAvatarUrl(const QString &mxcUrl);
    /// Asynchronously upload avatar and set it. Result arrives via avatarUploaded.
    void uploadAvatarAndSet(const QByteArray &data, const QString &contentType);
    /// Asynchronously fetch 3PIDs. Result arrives via threepidsReady.
    void fetchThreepids();
    /// Asynchronously request 3PID verification token. Result arrives via threepidTokenReady.
    void requestThreepidToken(ThreePidMedium medium, const QString &address,
                              const QString &clientSecret, quint32 sendAttempt,
                              const QString &country = QString());
    /// Asynchronously add a 3PID (after token verification). Result via threepidAdded.
    void addThreepid(const QString &clientSecret, const QString &sid,
                     const QString &authJson = QString());
    /// Asynchronously delete a 3PID. Result arrives via threepidDeleted.
    void deleteThreepid(ThreePidMedium medium, const QString &address);
    /// Asynchronously change password. Result arrives via changePasswordResult.
    void changePassword(const QString &newPassword, const QString &authJson = QString());
    /// Asynchronously deactivate account. Result arrives via deactivateAccountResult.
    void deactivateAccount(bool eraseData, const QString &authJson = QString());

    // --- Session verification ---

    /// Start SAS emoji verification. Result arrives via sasVerificationStarted.
    void startSasVerification(const QString &transactionId = QString());
    /// Confirm SAS emoji match. Result arrives via sasConfirmed.
    void confirmSasMatch();
    /// Verify session with recovery key. Result arrives via recoveryKeyVerified.
    void verifyWithRecoveryKey(const QString &key);
    /// Skip verification. Result arrives via verificationSkipped.
    void skipVerification();
    /// Cancel active verification flow. Result arrives via verificationCancelled.
    void cancelVerification(const QString &transactionId = QString());
    void mismatchSas();
    /// Get verification capabilities. Result arrives via verificationCapabilitiesReady.
    void getVerificationCapabilities();
    /// Show a QR code for verification. Result arrives via qrCodeReady.
    void startQrVerification(const QString &transactionId = QString());
    /// Confirm the other device scanned our QR. Result arrives via qrScanConfirmed.
    void confirmQrScanned();

    // --- Cross-user verification ---

    /// Start verifying ANOTHER user's identity. Emojis arrive via sasVerificationStarted.
    void startUserVerification(const QString &userId);
    /// Withdraw our verification of a user. Result via userVerificationWithdrawn.
    void withdrawUserVerification(const QString &userId);
    /// Query a user's trust state. Result via userTrustStateResult.
    void userTrustState(const QString &userId);

    /// Asynchronously get a room's settings snapshot.
    /// Result arrives via roomSettingsReady signal.
    void getRoomSettings(const QString &roomId);

    /// Asynchronously enable encryption for a room.
    /// This is a one-way operation — once enabled, encryption cannot be disabled.
    /// Result arrives via roomEncryptionEnabled signal.
    void enableRoomEncryption(const QString &roomId);
    /// Update room access / join rule. Result arrives via roomAccessSet.
    void setRoomAccess(const QString &roomId, RoomAccess access);
    /// Update the room name (m.room.name). Result arrives via roomNameSet.
    void setRoomName(const QString &roomId, const QString &name);
    /// Update the room topic (m.room.topic). Result arrives via roomTopicSet.
    void setRoomTopic(const QString &roomId, const QString &topic);
    /// Update room history visibility. Result arrives via roomHistoryVisibilitySet.
    void setRoomHistoryVisibility(const QString &roomId, HistoryVisibility visibility);
    /// Upload a room avatar and send m.room.avatar.
    /// Result arrives via roomAvatarUploaded signal.
    void uploadRoomAvatar(
        const QString &roomId,
        const QByteArray &data,
        const QString &contentType);
    /// Delete a room avatar by sending an empty m.room.avatar.
    /// Result arrives via roomAvatarDeleted signal.
    void deleteRoomAvatar(const QString &roomId);

    // --- Sessions + Encryption ---

    /// Get the list of own devices/sessions. Result arrives via ownDevicesReady.
    void getOwnDevices();
    /// Rename a device. Result arrives via deviceRenamed.
    void renameDevice(const QString &deviceId, const QString &displayName);
    /// Delete one or more devices. Result arrives via devicesDeleted.
    void deleteDevices(const QStringList &deviceIds, const QString &authJson = QString());
    /// Get encryption overview snapshot. Result arrives via encryptionOverviewReady.
    void getEncryptionOverview();
    /// Enable or disable key storage. Result arrives via keyStorageUpdated.
    void setKeyStorageEnabled(bool enabled);
    /// Provision recovery on an account that has none: creates the key backup and secret storage
    /// and hands back the new recovery key. Result arrives via recoverySetupResult.
    void setupRecovery();
    /// Replace an unusable server-side key backup with a fresh one. Destructive — only call after
    /// the user confirms. Result arrives via recoverySetupResult.
    void resetRecovery();
    /// Submit a recovery key. Result arrives via recoveryKeyAccepted.
    void enterRecoveryKey(const QString &recoveryKey);
    /// Create a new recovery key. Result arrives via recoveryKeyCreated.
    void createRecoveryKey();
    /// Commit a new recovery key. Result arrives via recoveryKeyCommitted.
    void commitRecoveryKey(const QString &recoveryKey);
    /// Reset cryptographic identity. Result arrives via identityResetResult.
    void resetIdentity(const QString &authJson = QString());
    /// Export E2E keys to file. Result arrives via e2eKeysExported.
    void exportE2EKeys(const QString &path, const QString &passphrase);
    /// Import E2E keys from file. Result arrives via e2eKeysImported.
    void importE2EKeys(const QString &path, const QString &passphrase);

signals:
    /// Emitted when login completes. profile fields valid only if success is true.
    void loginResult(bool success, const QString &userId, const QString &displayName, const QString &avatarUrl);

    /// Emitted when an asynchronous room snapshot is ready.
    void roomsReady(quint64 requestId, bool success, const QVector<RoomSummary> &rooms);
    /// Emitted when a single-room unread snapshot is ready.
    void roomUnreadSnapshotReady(
        const QString &roomId,
        quint64 requestId,
        bool success,
        const RoomUnreadSnapshot &snapshot);

    /// Emitted when the room list changes (e.g. new message in a room).
    void roomListChanged();

    /// Emitted when sync state changes. state: 0=not started, 1=syncing, 2=synced.
    void syncStateChanged(int state);
    /// Emitted when the dialogs startup bootstrap state changes.
    void initialDialogsLoadStateChanged(TeleMatrix::InitialDialogsLoadState state);

    /// Emitted when a user's presence changes.
    /// state: 0=offline, 1=online, 2=unavailable.
    /// lastActiveTs: UNIX epoch seconds of last activity, or 0 if unknown.
    void presenceChanged(const QString &userId, int state, qint64 lastActiveTs);

    /// Emitted when the typing user list changes for a room.
    void typingChanged(const QString &roomId, const QStringList &userIds);

    /// Emitted when a room's timeline changes.
    void timelineChanged(const QString &roomId);

    /// Emitted when an asynchronous timeline slice is ready.
    void timelineSliceReady(
        const QString &roomId,
        quint64 requestId,
        bool success,
        const TimelineSlice &slice);

    /// Emitted when a focusOnEvent call completes.
    void focusOnEventResult(const QString &roomId, quint64 requestId, bool success);
    /// Emitted when async pinned messages fetch completes.
    void pinnedMessagesFetched(const QVector<TimelineItem> &messages);

    /// Emitted when a sent message enqueue attempt completes.
    /// eventId valid only if success is true; requestId is the caller-supplied local request token.
    void messageSent(quint64 requestId, bool success, const QString &eventId);

    /// Emitted when an edited message is confirmed.
    void messageEdited(bool success, const QString &eventId);

    /// Emitted when message deletion completes.
    void messageDeleted(bool success);

    /// Emitted when a message's URL link-preview fetch starts/stops, so the UI
    /// can glow the URL only while the fetch is active.
    void urlPreviewFetchingChanged(
        const QString &roomId,
        const QString &eventId,
        bool fetching);

    /// Emitted when pin/unpin completes.
    void messagePinned(bool success);
    /// Emitted when room pin/unpin completes.
    void roomPinned(bool success);
    void pinnedOrderStored(bool success);
    /// Emitted when a room notification mode change is requested.
    void roomNotificationModeChangeRequested(
        const QString &roomId,
        RoomNotificationMode mode);
    /// Emitted when room notification mode change completes.
    void roomNotificationModeSet(bool success);
    /// Emitted with the account-global notification settings (reply to getNotificationSettings).
    void notificationSettingsReady(
        bool success,
        RoomNotificationMode dmLevel,
        RoomNotificationMode roomLevel,
        bool mentionDisplayName,
        bool mentionUsername,
        bool mentionRoom,
        bool keywordsEnabled,
        const QString &keywordsCsv);
    /// Emitted when a category-level or keyword change completes.
    void notificationSettingsSaved(bool success);
    /// Emitted when room notification mode change completes with request context.
    void roomNotificationModeSetForRoom(
        const QString &roomId,
        RoomNotificationMode mode,
        bool success);
    /// Emitted when room read state toggle completes.
    void roomMarkedRead(quint64 requestId, const QString &roomId, bool read, bool success);
    /// Emitted when a read receipt send completes for a specific room/event.
    void readReceiptSent(quint64 requestId, const QString &roomId, const QString &eventId, bool success);
    /// Emitted when leave-room completes.
    void roomLeft(bool success);
    /// Emitted when accept-invite completes.
    void inviteAccepted(bool success, const QString &roomId);
    /// Emitted when room creation completes. roomId valid only if success.
    void roomCreated(bool success, const QString &roomId);

    /// Emitted when kick-user completes.
    void userKicked(bool success);
    /// Emitted when ban-user completes.
    void userBanned(bool success);
    /// Emitted when unban-user completes.
    void userUnbanned(bool success);
    /// Emitted when invite-user completes.
    void userInvited(bool success);
    /// Emitted when ignore-user state change completes.
    void userIgnoredSet(bool success, bool ignored);
    /// Emitted when power-level change completes.
    void userPowerLevelSet(
        const QString &roomId,
        const QString &userId,
        bool success,
        qint64 powerLevel);
    /// Emitted when direct-room create/open completes.
    void directRoomCreated(const QString &userId, bool success, const QString &roomId);
    /// ensure result: empty roomId means there is no saved room.
    void savedMessagesRoomReady(bool success, const QString &roomId);
    /// The adopted saved-room id changed (empty when it was deleted / none).
    void savedMessagesRoomChanged(const QString &roomId);

    /// Emitted when forward completes. eventId is the new event in destination room.
    void messageForwarded(bool success, const QString &eventId);

    /// Emitted when media send completes.
    void mediaSent(bool success, const QString &eventId);

    /// Byte progress for an in-flight direct upload, keyed by transaction id.
    /// (Direct uploads bypass the send queue, so progress doesn't arrive on a
    /// timeline item — the history widget applies it to the optimistic echo.)
    void uploadProgress(const QString &transactionId, quint64 current, quint64 total);

    /// Emitted when the server's recent-emoji account data changes (startup
    /// hydrate or cross-device update). Pairs are ordered `(emoji, count)`.
    void recentEmojiChanged(const QVector<QPair<QString, int>> &pairs);

    /// Emitted when a reaction set/unset call completes.
    void reactionSet(bool success);
    /// Emitted when a poll vote send completes.
    void pollVoteSent(bool success);

    /// Emitted when a folder is created. filterId (runtime handle) and sectionKey
    /// (durable tag key) are valid only if success; error carries the reason if not.
    void folderCreated(bool success, int filterId, const QString &sectionKey, const QString &error);
    /// Emitted when a folder rename completes (error carries the reason on failure).
    void folderEdited(bool success, const QString &error);
    /// Emitted when a folder deletion completes (error carries the reason on failure).
    void folderDeleted(bool success, const QString &error);
    /// Emitted when a room's folder membership toggle completes. roomId/sectionKey
    /// identify which change; error carries the reason on failure.
    void roomFolderChanged(bool success, const QString &roomId, const QString &sectionKey, const QString &error);
    /// Emitted when a sidebar-order save completes.
    void sidebarOrderSaved(bool success);
    /// Emitted when custom folder metadata is ready.
    void customFoldersReady(bool success, const QVector<FolderInfo> &folders);
    /// Emitted when the unified sidebar order is ready.
    void sidebarOrderReady(bool success, const QVector<SidebarEntry> &order);
    /// Emitted when the joined-spaces snapshot is ready.
    void joinedSpacesReady(bool success, const QVector<SpaceInfo> &spaces);

    /// Emitted when session restore completes. profile fields valid only if success.
    /// On failure `error` says why. A restore fails for plenty of reasons that have
    /// nothing to do with the token being dead — a timeout, an unreachable server —
    /// so never treat !success on its own as "signed out" and wipe the session.
    void sessionRestored(bool success, const QString &userId, const QString &displayName, const QString &avatarUrl, const QString &error);
    /// Emitted when logout completes.
    void loggedOut(bool success);
    /// Emitted once shutdownAsync() has fully torn down the Rust runtime
    /// (tm_destroy returned). After this it is safe to open a new bridge on the
    /// same data directory. The bridge deleteLater()s itself right after.
    void shutdownComplete();
    /// Emitted when homeserver discovery completes. url valid only if success.
    void homeserverDiscovered(quint64 requestId, bool success, const QString &url);
    /// Emitted when registration classification completes. status: 0 = not a
    /// Matrix server, 1 = password registration (url = base URL), 2 = OIDC/MAS
    /// delegated (url = registration website).
    void registrationClassified(quint64 requestId, int status, const QString &url);
    /// Emitted when the auth-delegation probe completes. accountUrl is the
    /// server's account website, non-empty only when delegated.
    void authDelegationProbed(quint64 requestId, bool delegated, const QString &accountUrl);
    /// Emitted when the account-management probe completes. url is the server's
    /// account website (email/phone live there), non-empty only when available.
    void accountManagementProbed(quint64 requestId, bool available, const QString &url);

    /// Where a delegated-auth homeserver resets a forgotten password. `url` is
    /// only valid when `available` is true.
    void passwordResetPageProbed(quint64 requestId, bool available, const QString &url);

    /// `known` is false when the homeserver's answer settles nothing — the caller
    /// must then assume email verification works.
    void emailThreepidSupportProbed(quint64 requestId, bool known, bool supported);
    /// Emitted while mxc:// media is downloading locally.
    void mediaDownloadProgress(
        const QString &mxcUrl,
        quint64 receivedBytes,
        quint64 totalBytes,
        uint phase);
    /// Emitted when mxc:// media resolution completes. localPath valid only if
    /// success. Trailing `terminal` marks a permanent failure (HTTP 4xx except 429)
    /// that must not be retried; it is false on success and on transient failures.
    /// Kept last so existing 3-arg slots stay connected (Qt drops trailing args).
    void mediaResolved(
        bool success,
        const QString &mxcUrl,
        const QString &localPath,
        bool terminal = false);
    /// Emitted when mxc:// media byte resolution completes. bytes valid only if
    /// success. Trailing `terminal` as in mediaResolved (kept last for the same
    /// reason).
    void mediaBytesResolved(
        bool success,
        const QString &mxcUrl,
        const QByteArray &bytes,
        const QString &mime,
        const QString &filename,
        bool terminal = false);
    /// Emitted when exportMediaToPath completes.
    void mediaExported(bool success, const QString &mxcUrl, const QString &targetPath);
    /// Emitted when cache stats are ready.
    void cacheStatsReady(const CacheStats &stats);
    /// Emitted when a cache clear operation completes.
    void cacheClearResult(bool success, quint64 freedBytes);

    /// Emitted when an async search page is ready.
    void searchPageReady(const SearchPage &page);
    /// Emitted when async room member loading finishes.
    void roomMembersReady(const QString &roomId, const QVector<UserProfile> &members);
    /// Emitted when an async room member snapshot finishes loading.
    void roomMembersSnapshotReady(const QString &roomId, bool success, const RoomMembersSnapshot &snapshot);
    /// Emitted when an async user profile details request finishes.
    void userProfileDetailsReady(
        const QString &roomId,
        const QString &userId,
        bool success,
        const UserProfileDetails &details);
    /// Emitted when a user-directory search finishes.
    void userDirectorySearchReady(const QString &query, bool success, const QVector<UserProfile> &results, bool limited);
    /// Emitted when an async search request fails.
    void searchFailed(quint64 requestId, const QString &error);

    // --- Room discovery signals ---

    /// A page of directory results, or of a space's children (`page.isSpaceChildren`).
    void roomDirectoryPageReady(const RoomDirectoryPage &page);
    /// A directory/hierarchy request failed. The message is the server's, shown verbatim.
    void roomDirectoryFailed(quint64 requestId, const QString &error);
    /// Result of getRoomPreview().
    void roomPreviewReady(
        const QString &roomIdOrAlias,
        bool success,
        const RoomPreviewInfo &preview,
        const QString &error);
    /// Result of previewMessages(). `items` is the read-only history for a world-readable room;
    /// on failure it is empty and the caller keeps the placeholder. `nextToken` is the pagination
    /// token for the next-older page, or empty at the start of history.
    void roomPreviewMessagesReady(
        const QString &roomId,
        bool success,
        const QVector<TimelineItem> &items,
        const QString &nextToken,
        const QString &error);
    /// Result of joinRoom(). `roomId` is the resolved ID, which differs from the request when
    /// joining by alias.
    void roomJoined(
        const QString &roomIdOrAlias,
        bool success,
        const QString &roomId,
        const QString &error);
    /// Result of knockRoom(). Success means the knock was sent; membership stays pending until
    /// someone accepts it.
    void roomKnocked(
        const QString &roomIdOrAlias,
        bool success,
        const QString &roomId,
        const QString &error);

    // --- Registration signals ---
    void registrationSuccess(const QString &userId, const QString &displayName, const QString &avatarUrl);
    void registrationChallenge(const QString &challengeJson);
    void registrationFailed(const QString &error);
    void usernameAvailabilityChecked(int status, const QString &message);

    // --- Password reset signals ---
    void passwordResetTokenSent(bool success, const QString &sid,
                                const QString &clientSecret, const QString &error);
    void passwordResetComplete(bool success, const QString &error);

    // --- Account settings signals ---
    void accountSummaryReady(bool success, const AccountSummary &summary, const QString &error);
    void displayNameSet(bool success, const QString &error);
    void avatarSet(bool success, const QString &error);
    void avatarUploaded(bool success, const QString &newAvatarUrl, const QString &error);
    void threepidsReady(bool success, const QVector<ThreePid> &items, const QString &error);
    void threepidTokenReady(bool success, const ThreePidTokenResponse &token, const QString &error);
    void threepidAdded(const AccountActionResult &result);
    void threepidDeleted(bool success);
    void changePasswordResult(const AccountActionResult &result);
    void deactivateAccountResult(const AccountActionResult &result);

    // --- Session verification signals ---
    void sasVerificationStarted(bool success, const QStringList &emojis, const QStringList &labels);
    void sasConfirmed(bool success);
    void recoveryKeyVerified(bool success);
    void verificationSkipped(bool success);
    void verificationCancelled(bool success);
    void verificationStateChanged(int state, const QString &flowId);
    void incomingVerificationRequestReceived(
        const QString &transactionId,
        const QString &deviceId,
        const QString &deviceName);
    /// An incoming request can no longer be answered: another of our sessions
    /// took it, the requester withdrew it, or it expired.
    void verificationRequestClosed(const QString &flowId);
    void verificationCapabilitiesReady(bool success, bool canDevice, bool canRecovery, bool sasOk, bool qrSupported);
    void qrCodeReady(bool success, const QByteArray &modules, int size);
    void qrScanConfirmed(bool success);
    void deviceVerifiedChanged(bool verified);
    /// Result of a userTrustState() query (state = UserTrustState discriminant).
    void userTrustStateResult(const QString &userId, int state);
    /// A user's cross-signing trust changed live (state = UserTrustState discriminant).
    void userTrustChanged(const QString &userId, int state);
    /// Result of withdrawUserVerification().
    void userVerificationWithdrawn(const QString &userId, bool success);
    /// Another user requested to verify with us (in-room cross-user verification).
    void incomingUserVerificationRequestReceived(
        const QString &flowId,
        const QString &userId,
        const QString &displayName);
    /// Emitted once per genuinely-new unread incoming message that should notify.
    void incomingNotification(
        const QString &roomId,
        const QString &eventId,
        const QString &senderDisplayName,
        const QString &senderAvatarUrl,
        const QString &roomDisplayName,
        const QString &body,
        bool isDirect,
        bool isMention,
        qint64 timestamp);
    /// Emitted with `inProgress` true/false around a room's one-shot member
    /// fetch, so the timeline top bar can show a "syncing members" indicator.
    void memberSyncStateChanged(const QString &roomId, bool inProgress);
    /// Emitted once per genuinely-new room invitation that should notify.
    void incomingInvite(
        const QString &roomId,
        const QString &inviterDisplayName,
        const QString &inviterAvatarUrl,
        const QString &roomDisplayName,
        bool isDirect);
    /// Emitted once when a new, unverified session appears on the account
    /// ("New login. Was this you?"). `lastSeenTs` is UNIX secs (0 if unknown).
    void newLoginReceived(
        const QString &deviceId,
        const QString &displayName,
        const QString &lastSeenIp,
        qint64 lastSeenTs);

    /// Emitted when room settings snapshot is ready.
    void roomSettingsReady(bool success, const RoomSettingsSnapshot &snapshot);

    /// Emitted when room encryption enable completes.
    void roomEncryptionEnabled(bool success);
    /// Emitted when room access update completes.
    void roomAccessSet(const QString &roomId, bool success);
    void roomNameSet(const QString &roomId, bool success);
    /// Emitted when room topic update completes.
    void roomTopicSet(const QString &roomId, bool success);
    /// Emitted when room history visibility update completes.
    void roomHistoryVisibilitySet(const QString &roomId, bool success);
    /// Emitted when room avatar upload/set completes.
    void roomAvatarUploaded(
        const QString &roomId,
        bool success,
        const QString &newAvatarUrl);
    /// Emitted when room avatar deletion completes.
    void roomAvatarDeleted(const QString &roomId, bool success);

    // --- Sessions + Encryption signals ---
    void ownDevicesReady(bool success, const DeviceSessionList &list);
    void deviceRenamed(bool success);
    void devicesDeleted(bool success, const DeleteDevicesResult &result);
    void encryptionOverviewReady(bool success, const EncryptionOverview &overview);
    void keyStorageUpdated(bool success);
    void recoveryKeyAccepted(bool success);
    void recoveryKeyCreated(bool success, const QString &recoveryKey);
    void recoveryKeyCommitted(bool success);
    /// Result of setupRecovery()/resetRecovery(). On failure errorCode is 1 when an unusable key
    /// backup already exists on the homeserver (the user can confirm a reset), 2 otherwise.
    void recoverySetupResult(bool success, const QString &recoveryKey, int errorCode,
                             const QString &error);
    void identityResetResult(bool success, const ResetIdentityResult &result);
    void e2eKeysExported(bool success);
    void e2eKeysImported(bool success, int importedCount, int totalCount);

public:
    /// Register a per-room timeline change callback.
    /// Call when switching to a new room.
    void watchTimeline(const QString &roomId);

    /// Called from FFI callback trampolines. These coalesce bursty Rust callbacks
    /// before emitting Qt signals on the main thread.
    void enqueueRoomListChangedFromCallback();
    void enqueueTimelineChangedFromCallback(const QString &roomId);
    void handleRoomsReady(
        quint64 requestId,
        bool success,
        const QVector<RoomSummary> &rooms);
    void handleRoomUnreadSnapshotReady(
        const QString &roomId,
        quint64 requestId,
        bool success,
        const RoomUnreadSnapshot &snapshot);
    void handleTimelineSliceReady(
        const QString &roomId,
        quint64 requestId,
        bool success,
        const TimelineSlice &slice);

    /// Called from FFI trampoline when a search page arrives. Handles stale-response protection.
    void handleSearchPageReady(quint64 requestId, const SearchPage &page);
    /// Called from FFI trampoline when a search request fails.
    void handleSearchFailed(quint64 requestId, const QString &error);

    /// Called from FFI trampoline when a directory/hierarchy page arrives. Drops stale responses —
    /// debounced typing and space drill-down together produce plenty of them.
    void handleRoomDirectoryPageReady(quint64 requestId, const RoomDirectoryPage &page);
    /// Called from FFI trampoline when a directory/hierarchy request fails.
    void handleRoomDirectoryFailed(quint64 requestId, const QString &error);

    /// Caches the latest device verification status and emits deviceVerifiedChanged.
    void handleDeviceVerifiedChanged(bool verified);

    /// Shut down the Rust runtime on a background thread, then delete this
    /// bridge. Use instead of deleteLater() to dispose of a live bridge: FFI
    /// callbacks keep touching it until the runtime stops. Idempotent.
    void shutdownAsync();
    /// Move the Rust handle out and drain it on the returned (unstarted-if-null)
    /// thread, so many bridges can drain CONCURRENTLY on app quit instead of ~1.5s
    /// each in sequence. The caller joins the threads. Idempotent: a later call or
    /// the destructor sees a null handle and no-ops. See code-review-2026-07-19 PERF-2.
    [[nodiscard]] std::thread drainForQuit();
    /// Latest device verification status pushed by Rust. Main-thread only.
    [[nodiscard]] bool isDeviceVerified() const { return _deviceVerified; }
    /// Cached cross-signing trust of a user (UserTrustState discriminant; 0 if
    /// unknown). O(1) — safe to read during paint. Kept live by userTrustChanged.
    [[nodiscard]] int cachedUserTrust(const QString &userId) const {
        return _userTrustCache.value(userId, 0);
    }
    /// Query a user's trust if not already cached (the result lands in the cache
    /// and fires userTrustChanged). Call when data loads, never during paint.
    void ensureUserTrust(const QString &userId);

    // --- System keychain access (static, no Handle required) ---

    /// Store a secret in the system keychain.
    static bool keychainStore(const QString &key, const QString &value);
    /// Load a secret from the system keychain. Returns an empty string when the
    /// secret is absent — and also when the keychain refused the read, which is a
    /// very different thing: pass `readFailed` to tell them apart. Anything that
    /// would discard a session or wipe secrets on an empty result MUST check it,
    /// or a momentarily unreadable keychain destroys a perfectly good session.
    static QString keychainLoad(const QString &key, bool *readFailed = nullptr);
    /// Drop the cached secret bundle so the next keychainLoad re-reads the keychain.
    /// Required before retrying a read that came back empty.
    static void keychainForgetCache();
    /// Delete a secret from the system keychain.
    static bool keychainDelete(const QString &key);
    /// Clear all TeleMatrix secrets from the keychain (for logout).
    static void keychainClearAll();

    // --- Secret backend selection / master-password vault (all platforms) ---

    /// Configure the secret backend at startup, before any secret access.
    /// backend: 0 = OS keychain, 1 = master-password file vault.
    static void secretStoreInit(const QString &dataDir, int backend);
    /// 0 KeychainReady, 1 KeychainUnavailable, 2 VaultLocked, 3 VaultUnlocked, 4 VaultAbsent.
    static int secretStoreState();
    /// Whether the OS Secret Service is reachable (choose keychain-vs-vault at login).
    static bool secretServiceAvailable();
    /// Granular reachability for diagnostics: 0 available, 1 no D-Bus, 2 no provider.
    static int secretServiceStatus();
    /// Unlock the master-password file vault. 0 = unlocked; 1 = wrong password;
    /// 2 = invalid vault file; 3 = unreadable/absent; 4 = contents corrupt.
    static int secretStoreUnlock(const QString &passphrase);
    /// Set/replace the vault master password and switch to the vault backend.
    static bool secretStoreSetPassphrase(const QString &passphrase);
    /// Migrate secrets to another backend (0 = keychain, 1 = vault). `passphrase` is
    /// the new master password for the vault target; empty for the keychain target.
    static bool secretStoreSwitchBackend(int backend, const QString &passphrase);

private:
    void registerCallbacks();

    Handle *_handle = nullptr;
    // Shared guard that lets persistent worker-thread callback trampolines
    // safely detect when this bridge has been torn down. Defined in the .cpp;
    // see BridgeCallbackGuard there for the synchronisation contract.
    std::shared_ptr<BridgeCallbackGuard> _callbackGuard;
    int _syncState = 0;
    bool _deviceVerified = false;
    QHash<QString, int> _userTrustCache; // userId -> UserTrustState discriminant
    bool _shutdownStarted = false;
    // Set once logout begins: the UI keeps painting (and requesting media/OG
    // images) while the sign-out overlay is up, but the client is already gone,
    // so suppress those fetches instead of firing doomed "Not logged in" round
    // trips at the Rust layer.
    bool _loggingOut = false;
    InitialDialogsLoadState _initialDialogsLoadState = InitialDialogsLoadState::NotStarted;
    quint64 _nextUnreadRequestId = 1;
    std::atomic_bool _roomListChangeQueued = false;
    std::atomic_bool _timelineChangeFlushQueued = false;
    QMutex _pendingTimelineRoomsMutex;
    QSet<QString> _pendingTimelineRooms;
    QVector<RoomSummary> _cachedRooms;
    QString _savedMessagesRoomId;
    void adoptSavedMessagesRoomId(const QString &roomId);
    void applySavedMessagesIdentity(QVector<RoomSummary> &rooms) const;
    // O(1) index into _cachedRooms' notification fields, kept in sync via
    // setCachedRooms() so unread-snapshot conversion needs no scan or vector copy.
    QHash<QString, RoomNotifInfo> _roomNotifById;
    void setCachedRooms(const QVector<RoomSummary> &rooms);
    QVector<FolderInfo> _cachedCustomFolders;
    QVector<SpaceInfo> _cachedJoinedSpaces;
    SessionInfo _cachedSessionInfo;
    std::map<std::string, std::unique_ptr<TimelineCallbackData>> _timelineCallbackDatas;
    QSet<quint64> _activeSearchRequests;
    QSet<quint64> _activeRoomDirectoryRequests;
};

} // namespace TeleMatrix

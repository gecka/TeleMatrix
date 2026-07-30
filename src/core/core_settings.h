// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QJsonObject>
#include <QMap>
#include <QRect>
#include <QString>
#include <QVector>

#include <cstdint>

namespace TeleMatrix::Core {

constexpr int kRecentEmojiLimit = 54;
constexpr int kRecentReactionLimit = 24;
constexpr int kCacheSizeLimitMinMB = 50;
constexpr int kCacheSizeLimitDefaultMB = 4000; // 4 GB (matches the "4 GB" slider preset)
constexpr int kCacheSizeLimitMaxMB = 10000;

struct RecentEmoji {
    QString emoji;
    uint16_t rating = 0;
};

struct RecentReaction {
    QString key;
    uint16_t rating = 0;
};

// Custom chat folder (id > 2, since 0 = All Chats, 1 = Personal, 2 = Unread).
struct CustomFolder {
    int id = 0;
    QString name;
};

// Persisted window position (monitor CRC + geometry + maximized/scale).
struct WindowPosition {
    int32_t moncrc = 0;     // Monitor CRC for multi-monitor detection.
    int32_t maximized = 0;
    int32_t scale = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t w = 0;
    int32_t h = 0;

    [[nodiscard]] QRect rect() const {
        return QRect(x, y, w, h);
    }
};

// One account's settings: everything that belongs to a single logged-in Matrix
// account rather than to the device. Each account owns an instance; the
// device-level `Settings` below is shared by all of them.
//
// The split is exactly what the old Settings::clearSession() wiped on logout —
// signing one account out must leave the others (and the device preferences)
// untouched.
class AccountSettings {
public:
    AccountSettings();

    // Matrix session identity.
    [[nodiscard]] const QString &sessionHomeserver() const { return _sessionHomeserver; }
    void setSessionHomeserver(const QString &v) { _sessionHomeserver = v; }

    [[nodiscard]] const QString &sessionUserId() const { return _sessionUserId; }
    void setSessionUserId(const QString &v) { _sessionUserId = v; }

    [[nodiscard]] const QString &sessionDeviceId() const { return _sessionDeviceId; }
    void setSessionDeviceId(const QString &v) { _sessionDeviceId = v; }

    // "vault" = this session's secrets live in the master-password file vault;
    // empty/"keychain" = the OS keychain. Chosen once at login and honored at
    // startup so live detection can't flip backends and trigger a wipe.
    [[nodiscard]] const QString &sessionSecretBackend() const { return _sessionSecretBackend; }
    void setSessionSecretBackend(const QString &v) { _sessionSecretBackend = v; }

    [[nodiscard]] bool hasSession() const {
        return !_sessionHomeserver.isEmpty()
            && !_sessionUserId.isEmpty()
            && !_sessionDeviceId.isEmpty();
    }

    [[nodiscard]] bool hadLegacySessionAccessToken() const {
        return _legacySessionAccessTokenPresent;
    }
    void clearLegacySessionAccessTokenMarker() {
        _legacySessionAccessTokenPresent = false;
    }

    // Pinned room IDs in display order.
    [[nodiscard]] const QVector<QString> &pinnedRoomIds() const { return _pinnedRoomIds; }
    void setPinnedRoomIds(const QVector<QString> &ids) { _pinnedRoomIds = ids; }

    // Last known Saved Messages room. Cached only so the very first paint can
    // already show the localized name and bookmark userpic — the account-data
    // marker remains the source of truth and corrects this on every session.
    [[nodiscard]] const QString &savedMessagesRoomId() const { return _savedMessagesRoomId; }
    void setSavedMessagesRoomId(const QString &id) { _savedMessagesRoomId = id; }

    // Custom chat folders.
    [[nodiscard]] const QVector<CustomFolder> &customFolders() const { return _customFolders; }
    void setCustomFolders(const QVector<CustomFolder> &folders) { _customFolders = folders; }

    // Room-to-folder assignments: roomId → list of custom folder IDs (id > 2).
    [[nodiscard]] const QMap<QString, QVector<int>> &roomFolderAssignments() const {
        return _roomFolderAssignments;
    }
    void setRoomFolderAssignments(const QMap<QString, QVector<int>> &assignments) {
        _roomFolderAssignments = assignments;
    }

    // Ordered list of folder tab IDs (excludes "All chats" / 0).
    [[nodiscard]] const QVector<int> &folderOrder() const { return _folderOrder; }
    void setFolderOrder(const QVector<int> &order) { _folderOrder = order; }

    // Whether folders have been migrated from local settings to server.
    [[nodiscard]] bool foldersServerMigrated() const { return _foldersServerMigrated; }
    void setFoldersServerMigrated(bool v) { _foldersServerMigrated = v; }

    [[nodiscard]] const QVector<RecentEmoji> &recentEmoji() const { return _recentEmoji; }
    void incrementRecentEmoji(const QString &emoji);
    // Replace the in-memory recent list (hydrated from account data by Rust).
    void setRecentEmoji(QVector<RecentEmoji> items);

    // Recent reactions (separate from compose emoji).
    [[nodiscard]] const QVector<RecentReaction> &recentReactions() const {
        return _recentReactions;
    }
    void incrementRecentReaction(const QString &key);

    // Wipe this account's data on sign-out. Device-level preferences live in
    // `Settings` and are untouched, as are every other account's settings.
    void clear();

    [[nodiscard]] QJsonObject toJson() const;
    bool addFromJson(const QJsonObject &object);

private:
    QString _sessionHomeserver;
    QString _sessionUserId;
    QString _sessionDeviceId;
    QString _sessionSecretBackend;
    bool _legacySessionAccessTokenPresent = false;
    QVector<QString> _pinnedRoomIds;
    QString _savedMessagesRoomId;
    QVector<CustomFolder> _customFolders;
    QMap<QString, QVector<int>> _roomFolderAssignments;
    QVector<int> _folderOrder;
    bool _foldersServerMigrated = false;
    QVector<RecentEmoji> _recentEmoji;
    QVector<RecentReaction> _recentReactions;
};

// Device-level application settings, shared by every account. Stored as
// versioned JSON alongside the account list.
class Settings {
public:
    Settings();

    // Window position.
    [[nodiscard]] const WindowPosition &windowPosition() const {
        return _windowPosition;
    }
    void setWindowPosition(const WindowPosition &position) {
        _windowPosition = position;
    }

    // Dialogs column width in pixels (0 = never set, use the default). Stored
    // absolutely, not as a fraction of the window: the column has stretch
    // factor 0 and keeps its pixel width across window resizes, so a fraction
    // would only reproduce the user's width at the window width it was saved at.
    [[nodiscard]] int dialogsWidth() const {
        return _dialogsWidth;
    }
    void setDialogsWidth(int width) {
        _dialogsWidth = width;
    }

    // Notification settings.
    [[nodiscard]] bool desktopNotify() const { return _desktopNotify; }
    void setDesktopNotify(bool v) { _desktopNotify = v; }

    [[nodiscard]] bool soundNotify() const { return _soundNotify; }
    void setSoundNotify(bool v) { _soundNotify = v; }

    [[nodiscard]] bool showSenderName() const { return _showSenderName; }
    void setShowSenderName(bool v) { _showSenderName = v; }

    [[nodiscard]] bool showMessagePreview() const { return _showMessagePreview; }
    void setShowMessagePreview(bool v) { _showMessagePreview = v; }

    // "Include muted chats in unread count" — the dock/tray/taskbar badge total.
    [[nodiscard]] bool includeMutedInBadge() const { return _includeMutedInBadge; }
    void setIncludeMutedInBadge(bool v) { _includeMutedInBadge = v; }

    // "Include muted chats in folders counters" — the per-folder unread badges.
    [[nodiscard]] bool includeMutedInFolders() const { return _includeMutedInFolders; }
    void setIncludeMutedInFolders(bool v) { _includeMutedInFolders = v; }

    [[nodiscard]] bool bounceDockIcon() const { return _bounceDockIcon; }
    void setBounceDockIcon(bool v) { _bounceDockIcon = v; }

    // Device-level preferred backend for NEW sessions ("vault"/"keychain"; empty =
    // keychain). Unlike AccountSettings::sessionSecretBackend this is NOT cleared on
    // logout, so the first-run choice sticks for the next sign-in on this device.
    [[nodiscard]] const QString &preferredSecretBackend() const { return _preferredSecretBackend; }
    void setPreferredSecretBackend(const QString &v) { _preferredSecretBackend = v; }

    // Named theme family ("dubai", "madrid", ...). Stored raw: an id this
    // build doesn't know falls back to the default at load time.
    [[nodiscard]] QString themeId() const { return _themeId; }
    void setThemeId(const QString &id) { _themeId = id; }

    // Theme mode (0 = Day, 1 = Night, 2 = System).
    [[nodiscard]] int themeMode() const { return _themeMode; }
    void setThemeMode(int mode) { _themeMode = mode; }

    // Auto-update policy (0 = Off, 1 = Check & notify, 2 = Check & auto-download).
    // Applying an update is user-initiated in every mode — never a silent restart.
    [[nodiscard]] int updatePolicy() const { return _updatePolicy; }
    void setUpdatePolicy(int policy) { _updatePolicy = policy; }

    /// Version the user dismissed from the rooms-list prompt. Per-version, so a
    /// later release notifies again. Empty = nothing skipped.
    [[nodiscard]] QString skippedUpdateVersion() const { return _skippedUpdateVersion; }
    void setSkippedUpdateVersion(const QString &v) { _skippedUpdateVersion = v; }

    // Opt in to pre-release builds. Off follows the stable channel only; on also
    // offers betas. Turning it back off never downgrades — a beta simply stops
    // being superseded until the matching final ships.
    [[nodiscard]] bool installBetaVersions() const { return _installBetaVersions; }
    void setInstallBetaVersions(bool v) { _installBetaVersions = v; }

    // Whether to follow OS dark mode when themeMode == System.
    [[nodiscard]] bool systemDarkModeEnabled() const { return _systemDarkModeEnabled; }
    void setSystemDarkModeEnabled(bool v) { _systemDarkModeEnabled = v; }

    // Appearance: UI scale (0 = auto, else percent 50-300).
    [[nodiscard]] int configScale() const { return _configScale; }
    void setConfigScale(int scale) { _configScale = scale; }

    // Appearance: custom font family (empty = default, "system" = system font).
    [[nodiscard]] const QString &customFontFamily() const { return _customFontFamily; }
    void setCustomFontFamily(const QString &family) { _customFontFamily = family; }

    // Appearance: the doodle pattern soft-lit over the chat wallpaper.
    [[nodiscard]] bool backgroundDoodles() const { return _backgroundDoodles; }
    void setBackgroundDoodles(bool v) { _backgroundDoodles = v; }

    // Appearance: large emoji in messages.
    [[nodiscard]] bool largeEmoji() const { return _largeEmoji; }
    void setLargeEmoji(bool v) { _largeEmoji = v; }

    // Appearance: hover reply / reaction buttons on message bubbles.
    [[nodiscard]] bool replyButtonOnMessages() const { return _replyButtonOnMessages; }
    void setReplyButtonOnMessages(bool v) { _replyButtonOnMessages = v; }
    [[nodiscard]] bool reactionButtonOnMessages() const { return _reactionButtonOnMessages; }
    void setReactionButtonOnMessages(bool v) { _reactionButtonOnMessages = v; }

    // Appearance: hide system/service messages (joins, leaves, name/topic changes) in public
    // rooms, where they are mostly noise. Private chats and DMs always show them.
    [[nodiscard]] bool hideSystemMessagesInPublicRooms() const {
        return _hideSystemMessagesInPublicRooms;
    }
    void setHideSystemMessagesInPublicRooms(bool v) { _hideSystemMessagesInPublicRooms = v; }

    // Search: index & search inside E2EE (encrypted) rooms. Off stops the local
    // indexing workers and wipes the search DB.
    [[nodiscard]] bool searchEncryptedRooms() const { return _searchEncryptedRooms; }
    void setSearchEncryptedRooms(bool v) { _searchEncryptedRooms = v; }

    // Preferences: language ID (empty = default/system language).
    [[nodiscard]] const QString &languageId() const { return _languageId; }
    void setLanguageId(const QString &id) { _languageId = id; }

    // Preferences: send submit way (0 = Enter sends, 1 = Ctrl/Cmd+Enter sends).
    [[nodiscard]] int sendSubmitWay() const { return _sendSubmitWay; }
    void setSendSubmitWay(int way) { _sendSubmitWay = way; }

    // Preferences: macOS warn before quit.
    [[nodiscard]] bool macWarnBeforeQuit() const { return _macWarnBeforeQuit; }
    void setMacWarnBeforeQuit(bool v) { _macWarnBeforeQuit = v; }

    // Preferences: downscale + re-encode images on send (opt-in; off by default).
    [[nodiscard]] bool compressImages() const { return _compressImages; }
    void setCompressImages(bool v) { _compressImages = v; }

    // Preferences: last directory used to save media from the viewer.
    // Empty = use the platform Downloads folder. Device-level (kept on logout).
    [[nodiscard]] const QString &mediaSaveDir() const { return _mediaSaveDir; }
    void setMediaSaveDir(const QString &dir) { _mediaSaveDir = dir; }

    // Serialize all settings to JSON.
    [[nodiscard]] QJsonObject toJson() const;

    // Deserialize settings from JSON. Returns false on error.
    bool addFromJson(const QJsonObject &object);

private:
    WindowPosition _windowPosition;
    int _dialogsWidth = 0; // 0 = use default
    bool _desktopNotify = true;
    bool _soundNotify = true;
    bool _showSenderName = true;
    bool _showMessagePreview = true;
    bool _includeMutedInBadge = true;
    bool _includeMutedInFolders = true;
    bool _bounceDockIcon = true;
    QString _preferredSecretBackend; // device-level; deliberately kept across logout
    QString _themeId = QStringLiteral("dubai");
    int _themeMode = 0;               // 0=Day, 1=Night, 2=System
    bool _systemDarkModeEnabled = true;
    int _updatePolicy = 2;            // 0=Off, 1=Check & notify, 2=Auto-download
    QString _skippedUpdateVersion;    // dismissed from the rooms-list prompt
    bool _installBetaVersions = false; // opt-in: also offer pre-release builds
    int _configScale = 0;              // 0=auto, else percent (50-300)
    QString _customFontFamily;         // empty=default, "system"=system font
    bool _backgroundDoodles = true;
    bool _largeEmoji = true;
    bool _replyButtonOnMessages = true;
    bool _reactionButtonOnMessages = true;
    bool _hideSystemMessagesInPublicRooms = true;
    bool _searchEncryptedRooms = true; // index+search E2EE rooms (local FTS)
    QString _languageId;               // empty=default/system language
    int _sendSubmitWay = 0;            // 0=Enter, 1=Ctrl/Cmd+Enter
    bool _macWarnBeforeQuit = true;
    bool _compressImages = false;      // downscale+recompress images on send (opt-in)
    QString _mediaSaveDir;             // last media-save dir (empty=Downloads)
    int _cacheSizeLimitMB = kCacheSizeLimitDefaultMB;
    bool _cacheAutoCleanup = false;    // Auto-cleanup on app start

public:
    [[nodiscard]] int cacheSizeLimitMB() const { return _cacheSizeLimitMB; }
    void setCacheSizeLimitMB(int mb) {
        _cacheSizeLimitMB = qBound(kCacheSizeLimitMinMB, mb, kCacheSizeLimitMaxMB);
    }
    [[nodiscard]] bool cacheAutoCleanup() const { return _cacheAutoCleanup; }
    void setCacheAutoCleanup(bool v) { _cacheAutoCleanup = v; }
};

} // namespace TeleMatrix::Core

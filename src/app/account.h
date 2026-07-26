// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "core/core_settings.h"
#include "protocol/protocol_types.h"

#include <QObject>
#include <QString>

#include <memory>
#include <optional>

namespace TeleMatrix {

class ProtocolBridge;
class UnreadStateStore;

/// One logged-in Matrix account: its protocol bridge, its own storage, its own
/// settings, and its own unread state.
///
/// Several accounts are alive at once — each keeps syncing in the background so
/// its notifications and unread counts stay live — but only one is *active*
/// (showing its UI) at a time. Nothing here is shared between accounts; what the
/// device owns rather than the account (theme, window, notification toggles, the
/// secret vault) lives in AppController and Core::Settings.
class Account {
public:
    /// `dirName` is the account's stable data-directory name, persisted in the
    /// account index so allocation order never matters.
    explicit Account(QString dirName);
    ~Account();

    Account(const Account &) = delete;
    Account &operator=(const Account &) = delete;

    /// How far this account has got in coming up. Restoring accounts have a
    /// bridge but no usable session yet; a failed restore is reported when the
    /// user switches to it, not while it is in the background.
    enum class State {
        Restoring,
        Ready,
        RestoreFailed,
    };

    [[nodiscard]] const QString &dirName() const { return _dirName; }
    /// Absolute path of this account's own store, media cache and search index.
    [[nodiscard]] QString dataDir() const;

    [[nodiscard]] Core::AccountSettings &settings() { return _settings; }
    [[nodiscard]] const Core::AccountSettings &settings() const { return _settings; }

    [[nodiscard]] ProtocolBridge *bridge() const { return _bridge.get(); }
    void setBridge(std::unique_ptr<ProtocolBridge> bridge);
    /// Hand the bridge to a caller that will tear it down asynchronously.
    [[nodiscard]] std::unique_ptr<ProtocolBridge> takeBridge();

    [[nodiscard]] UnreadStateStore *unreadStateStore() const { return _unreadStateStore.get(); }
    void setUnreadStateStore(std::unique_ptr<UnreadStateStore> store);

    [[nodiscard]] State state() const { return _state; }
    void setState(State state) { _state = state; }

    // Profile of the logged-in user, filled once the session is ready.
    [[nodiscard]] const QString &userId() const { return _userId; }
    [[nodiscard]] const QString &displayName() const { return _displayName; }
    [[nodiscard]] const QString &avatarUrl() const { return _avatarUrl; }
    void setProfile(
        const QString &userId,
        const QString &displayName,
        const QString &avatarUrl);
    void setDisplayName(const QString &displayName) { _displayName = displayName; }
    void setAvatarUrl(const QString &avatarUrl) { _avatarUrl = avatarUrl; }

    [[nodiscard]] const AccountSummary &cachedAccountSummary() const {
        return _cachedAccountSummary;
    }
    [[nodiscard]] bool cachedAccountSummaryLoaded() const { return _cachedAccountSummaryLoaded; }
    void setCachedAccountSummary(const AccountSummary &summary) {
        _cachedAccountSummary = summary;
        _cachedAccountSummaryLoaded = true;
    }

    [[nodiscard]] std::optional<bool> emailVerificationSupported() const {
        return _emailVerificationSupported;
    }
    void setEmailVerificationSupported(std::optional<bool> value) {
        _emailVerificationSupported = value;
    }

    // Once the user changes recents this session, the local list is authoritative:
    // ignore server account-data echoes (our own write, or a lagging value) so they
    // can't revert a just-picked ordering until the next launch.
    [[nodiscard]] bool recentEmojiTouchedLocally() const { return _recentEmojiTouchedLocally; }
    void markRecentEmojiTouchedLocally() { _recentEmojiTouchedLocally = true; }

    /// Record a signed-in session and its profile.
    void persistSession(
        const QString &homeserver,
        const QString &userId,
        const QString &deviceId,
        const QString &secretBackend,
        const QString &displayName,
        const QString &avatarUrl);

    /// Every keychain key this account owns: the three session secrets plus the
    /// three data-dir-namespaced local-cache passphrases. Signing one account out
    /// must delete exactly these and leave every sibling's secrets readable.
    [[nodiscard]] QStringList secretKeys() const;

    /// Delete this account's secrets from the keychain. Siblings are untouched —
    /// unlike a wholesale clear, which is only correct for the last account.
    void deleteSecrets() const;

    /// Forget this account's session, profile and user data (its dir is trashed
    /// separately by the bridge's logout).
    void clear();

private:
    QString _dirName;
    Core::AccountSettings _settings;
    std::unique_ptr<ProtocolBridge> _bridge;
    std::unique_ptr<UnreadStateStore> _unreadStateStore;
    State _state = State::Restoring;

    QString _userId;
    QString _displayName;
    QString _avatarUrl;
    AccountSummary _cachedAccountSummary;
    bool _cachedAccountSummaryLoaded = false;
    std::optional<bool> _emailVerificationSupported;
    bool _recentEmojiTouchedLocally = false;
};

} // namespace TeleMatrix

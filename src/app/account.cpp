// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "account.h"

#include "account_index.h"
#include "app_controller.h"
#include "unread_state_store.h"

#include "../protocol/protocol_bridge.h"

namespace TeleMatrix {

Account::Account(QString dirName)
: _dirName(std::move(dirName)) {
}

Account::~Account() = default;

QString Account::dataDir() const {
    return AppController::accountDataDir(_dirName);
}

void Account::setBridge(std::unique_ptr<ProtocolBridge> bridge) {
    _bridge = std::move(bridge);
}

std::unique_ptr<ProtocolBridge> Account::takeBridge() {
    return std::move(_bridge);
}

void Account::setUnreadStateStore(std::unique_ptr<UnreadStateStore> store) {
    _unreadStateStore = std::move(store);
}

void Account::setProfile(
        const QString &userId,
        const QString &displayName,
        const QString &avatarUrl) {
    _userId = userId;
    _displayName = displayName;
    _avatarUrl = avatarUrl;
}

void Account::persistSession(
        const QString &homeserver,
        const QString &userId,
        const QString &deviceId,
        const QString &secretBackend,
        const QString &displayName,
        const QString &avatarUrl) {
    _settings.setSessionHomeserver(homeserver);
    _settings.setSessionUserId(userId);
    _settings.setSessionDeviceId(deviceId);
    _settings.setSessionSecretBackend(secretBackend);
    setProfile(userId, displayName, avatarUrl);
}

QStringList Account::secretKeys() const {
    return AccountSecretKeys(
        _dirName,
        _settings.sessionHomeserver(),
        _settings.sessionUserId(),
        _settings.sessionDeviceId());
}

void Account::deleteSecrets() const {
    for (const auto &key : secretKeys()) {
        ProtocolBridge::keychainDelete(key);
    }
}

void Account::clear() {
    _settings.clear();
    _userId.clear();
    _displayName.clear();
    _avatarUrl.clear();
    _cachedAccountSummary = AccountSummary();
    _cachedAccountSummaryLoaded = false;
    _emailVerificationSupported.reset();
    _recentEmojiTouchedLocally = false;
}

} // namespace TeleMatrix

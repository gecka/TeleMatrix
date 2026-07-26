// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "account_domain.h"

#include "account.h"
#include "account_index.h"
#include "unread_state_store.h"

#include <QJsonArray>

namespace TeleMatrix {

AccountDomain::AccountDomain(QObject *parent)
: QObject(parent) {
}

AccountDomain::~AccountDomain() = default;

Account *AccountDomain::account(int index) const {
    if (index < 0 || index >= count()) {
        return nullptr;
    }
    return _accounts[size_t(index)].get();
}

Account *AccountDomain::active() const {
    return account(_active);
}

int AccountDomain::indexOfDirName(const QString &dirName) const {
    for (int i = 0; i < count(); ++i) {
        if (_accounts[size_t(i)]->dirName() == dirName) {
            return i;
        }
    }
    return -1;
}

int AccountDomain::indexOfUserId(const QString &userId) const {
    if (userId.isEmpty()) {
        return -1;
    }
    for (int i = 0; i < count(); ++i) {
        if (_accounts[size_t(i)]->settings().sessionUserId() == userId) {
            return i;
        }
    }
    return -1;
}

int AccountDomain::add(std::unique_ptr<Account> account) {
    if (!account || !canAddAccount()) {
        return -1;
    }
    _accounts.push_back(std::move(account));
    const auto index = count() - 1;
    if (_active < 0) {
        _active = index;
        Q_EMIT activeChanged(_active);
    }
    Q_EMIT accountsChanged();
    Q_EMIT unreadBadgeChanged();
    return index;
}

void AccountDomain::remove(int index) {
    if (index < 0 || index >= count()) {
        return;
    }
    const auto previousActive = _active;
    _accounts.erase(_accounts.begin() + index);
    _active = ActiveIndexAfterRemoval(previousActive, index, count());

    Q_EMIT accountsChanged();
    Q_EMIT unreadBadgeChanged();
    if (_active != previousActive || index == previousActive) {
        Q_EMIT activeChanged(_active);
    }
}

bool AccountDomain::activate(int index) {
    if (index < 0 || index >= count() || index == _active) {
        return false;
    }
    _active = index;
    Q_EMIT activeChanged(_active);
    return true;
}

QString AccountDomain::allocateDirName() const {
    QStringList taken;
    taken.reserve(count());
    for (const auto &account : _accounts) {
        taken.append(account->dirName());
    }
    return FirstFreeAccountDirName(taken);
}

int AccountDomain::totalUnreadBadge(bool includeMuted) const {
    int total = 0;
    for (const auto &account : _accounts) {
        if (const auto store = account->unreadStateStore()) {
            total += store->totalUnreadCount(includeMuted);
        }
    }
    return total;
}

AccountIndex AccountDomain::toIndex() const {
    AccountIndex index;
    index.entries.reserve(count());
    for (const auto &account : _accounts) {
        index.entries.push_back({ account->dirName(), account->settings() });
    }
    index.activeIndex = _active;
    return index;
}

void AccountDomain::restore(const AccountIndex &index) {
    _accounts.clear();
    for (const auto &entry : index.entries) {
        auto account = std::make_unique<Account>(entry.dirName);
        account->settings() = entry.settings;
        _accounts.push_back(std::move(account));
    }
    // Parsed indices arrive clamped, but restore must be safe on its own: an
    // out-of-range active here means active() returns nullptr and startup
    // dereferences it.
    _active = ClampActiveIndex(index.activeIndex, count());

    Q_EMIT accountsChanged();
    Q_EMIT activeChanged(_active);
    Q_EMIT unreadBadgeChanged();
}

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "account_index.h"

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <memory>
#include <vector>

namespace TeleMatrix {

class Account;

/// Every logged-in account on this device, in display order, plus which one is
/// active.
///
/// All of them stay connected: an account that isn't showing its UI keeps syncing
/// so its notifications arrive and its unread count stays live, which is why the
/// badge is a sum across the whole list rather than the active account's own.
class AccountDomain : public QObject {
    Q_OBJECT

public:
    explicit AccountDomain(QObject *parent = nullptr);
    ~AccountDomain() override;

    [[nodiscard]] int count() const { return int(_accounts.size()); }
    [[nodiscard]] bool isEmpty() const { return _accounts.empty(); }
    [[nodiscard]] bool canAddAccount() const { return count() < kMaxAccounts; }

    /// The account at `index`, or nullptr when out of range.
    [[nodiscard]] Account *account(int index) const;
    /// The account whose UI is showing, or nullptr when none is signed in.
    [[nodiscard]] Account *active() const;
    [[nodiscard]] int activeIndex() const { return _active; }
    /// Index of the account with this data-directory name, or -1.
    [[nodiscard]] int indexOfDirName(const QString &dirName) const;
    /// Index of the account signed in as `userId`, or -1. Used to keep a second
    /// sign-in from duplicating an account that is already here.
    [[nodiscard]] int indexOfUserId(const QString &userId) const;

    /// Append an account and return its index, or -1 when full.
    int add(std::unique_ptr<Account> account);
    /// Remove the account at `index`. When it was the active one, the next
    /// account in order takes over (tdesktop's behaviour), so the app keeps
    /// running as long as any account remains.
    void remove(int index);
    /// Make `index` the active account. No-op when already active or invalid.
    bool activate(int index);

    /// The first unused data-directory name. Names are persisted, so a removed
    /// account's name can be reused without disturbing anyone else's.
    [[nodiscard]] QString allocateDirName() const;

    /// Unread total across every account (the dock/tray badge). Background
    /// accounts count too — that is the point of keeping them synced.
    [[nodiscard]] int totalUnreadBadge(bool includeMuted) const;

    /// Snapshot the list for persistence.
    [[nodiscard]] AccountIndex toIndex() const;
    /// Replace the account list from a persisted index. Accounts come back with
    /// their settings but no bridge; the caller brings them up.
    void restore(const AccountIndex &index);

Q_SIGNALS:
    /// The active account changed (argument is the new index, -1 when none).
    void activeChanged(int index);
    /// An account was added or removed.
    void accountsChanged();
    /// Any account's unread total changed, so the aggregate badge is stale.
    void unreadBadgeChanged();

private:
    std::vector<std::unique_ptr<Account>> _accounts;
    int _active = -1;
};

} // namespace TeleMatrix

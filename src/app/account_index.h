// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "core/core_settings.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace TeleMatrix {

/// How many accounts may be signed in at once.
inline constexpr int kMaxAccounts = 6;

/// One entry of the persisted account list: which directory holds the account's
/// data, and that account's own settings.
struct AccountIndexEntry {
    QString dirName;
    Core::AccountSettings settings;
};

/// The account list plus which entry was active, as stored in settings.json.
struct AccountIndex {
    QVector<AccountIndexEntry> entries;
    int activeIndex = -1;
};

/// Serialize the account list for settings.json.
[[nodiscard]] QJsonObject SerializeAccountIndex(const AccountIndex &index);

/// Parse the account list. Entries without a directory name, duplicates of a name
/// already seen, and anything past `kMaxAccounts` are dropped: a nameless entry has
/// no storage to point at, and two accounts sharing a directory would open the same
/// SQLite files twice. The active index is clamped into range.
[[nodiscard]] AccountIndex ParseAccountIndex(const QJsonObject &object);

/// The first directory name not in `taken`. Names are persisted, so a removed
/// account's name is free to reuse without disturbing anyone else's storage.
[[nodiscard]] QString FirstFreeAccountDirName(const QStringList &taken);

/// Which entry is active after the one at `removed` goes away, given the list is
/// now `newCount` long. Removing the active account promotes the next one in order
/// (tdesktop's behaviour); removing one before it shifts the active index down.
/// Returns -1 when nothing is left.
[[nodiscard]] int ActiveIndexAfterRemoval(int active, int removed, int newCount);

/// A stored active index made safe for a list of `count` entries (-1 when empty).
[[nodiscard]] int ClampActiveIndex(int stored, int count);

/// Every keychain key one account owns: the three session secrets (keyed by the
/// full session identity) plus the three local-cache passphrases (keyed by data
/// directory, because those stores are opened before any session exists to name
/// them). Signing one account out must delete exactly these — no more, or a
/// sibling loses its secrets; no fewer, or secrets outlive the account.
///
/// Session keys are omitted when the session identity is incomplete: there is
/// nothing to delete for an account that never finished signing in.
[[nodiscard]] QStringList AccountSecretKeys(
    const QString &dirName,
    const QString &homeserver,
    const QString &userId,
    const QString &deviceId);

} // namespace TeleMatrix

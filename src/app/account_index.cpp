// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "account_index.h"

#include <QJsonArray>

#include <algorithm>

namespace TeleMatrix {

QJsonObject SerializeAccountIndex(const AccountIndex &index) {
    QJsonArray accounts;
    for (const auto &entry : index.entries) {
        QJsonObject object = entry.settings.toJson();
        object[QStringLiteral("dirName")] = entry.dirName;
        accounts.append(object);
    }
    return QJsonObject{
        { QStringLiteral("accounts"), accounts },
        { QStringLiteral("activeAccount"), index.activeIndex },
    };
}

AccountIndex ParseAccountIndex(const QJsonObject &object) {
    AccountIndex index;
    QStringList seen;
    for (const auto &value : object.value(QStringLiteral("accounts")).toArray()) {
        if (index.entries.size() >= kMaxAccounts) {
            break;
        }
        const auto entryObject = value.toObject();
        const auto dirName = entryObject.value(QStringLiteral("dirName")).toString();
        if (dirName.isEmpty() || seen.contains(dirName)) {
            continue;
        }
        seen.append(dirName);

        AccountIndexEntry entry;
        entry.dirName = dirName;
        entry.settings.addFromJson(entryObject);
        index.entries.push_back(std::move(entry));
    }

    index.activeIndex = ClampActiveIndex(
        object.value(QStringLiteral("activeAccount")).toInt(0),
        int(index.entries.size()));
    return index;
}

QString FirstFreeAccountDirName(const QStringList &taken) {
    for (int candidate = 0;; ++candidate) {
        const auto name = QString::number(candidate);
        if (!taken.contains(name)) {
            return name;
        }
    }
}

int ActiveIndexAfterRemoval(int active, int removed, int newCount) {
    if (newCount <= 0) {
        return -1;
    }
    if (removed < active) {
        // Everything after the removed entry shifted down by one.
        return std::clamp(active - 1, 0, newCount - 1);
    }
    if (removed > active) {
        return std::clamp(active, 0, newCount - 1);
    }
    // The active account itself went away: the entry that slid into its place is
    // the next one in order, or the new last when it was at the end.
    return std::min(removed, newCount - 1);
}

int ClampActiveIndex(int stored, int count) {
    if (count <= 0) {
        return -1;
    }
    return std::clamp(stored, 0, count - 1);
}

QStringList AccountSecretKeys(
        const QString &dirName,
        const QString &homeserver,
        const QString &userId,
        const QString &deviceId) {
    QStringList keys;
    if (!homeserver.isEmpty() && !userId.isEmpty() && !deviceId.isEmpty()) {
        for (const auto &kind : {
            QStringLiteral("session_access_token"),
            QStringLiteral("sdk_store_passphrase"),
            QStringLiteral("search_passphrase"),
        }) {
            keys.append(QStringLiteral("v1|%1|%2|%3|%4")
                .arg(kind, homeserver, userId, deviceId));
        }
    }
    for (const auto &kind : {
        QStringLiteral("app_cache_passphrase"),
        QStringLiteral("preview_cache_passphrase"),
        QStringLiteral("media_cache_passphrase"),
    }) {
        keys.append(QStringLiteral("v1|local_cache|%1|%2").arg(dirName, kind));
    }
    return keys;
}

} // namespace TeleMatrix

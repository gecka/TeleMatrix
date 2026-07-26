// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "room_directory_filter.h"

namespace TeleMatrix {

QVector<RoomDirectoryEntry> filterRoomEntries(
    const QVector<RoomDirectoryEntry> &entries,
    const QString &needle) {
    const auto trimmed = needle.trimmed();
    if (trimmed.isEmpty()) {
        return entries;
    }

    QVector<RoomDirectoryEntry> filtered;
    filtered.reserve(entries.size());
    for (const auto &entry : entries) {
        if (entry.name.contains(trimmed, Qt::CaseInsensitive)
            || entry.topic.contains(trimmed, Qt::CaseInsensitive)) {
            filtered.push_back(entry);
        }
    }
    return filtered;
}

} // namespace TeleMatrix

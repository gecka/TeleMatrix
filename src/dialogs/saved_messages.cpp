// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "saved_messages.h"

#include <QCoreApplication>

namespace TeleMatrix::SavedMessages {

QString displayName() {
    return QCoreApplication::translate("SavedMessages", "Saved Messages");
}

QVector<RoomSummary> arrangeForwardTargets(
        const QVector<RoomSummary> &rooms,
        const QString &savedRoomId,
        const QString &currentRoomId) {
    QVector<RoomSummary> out;
    out.reserve(rooms.size() + 1);

    // The cached summary (if any) is deliberately replaced by a synthesized
    // row — fixed name, drawn avatar — so the list never shows a stale server
    // name for the saved room.
    RoomSummary saved;
    saved.roomId = savedRoomId.isEmpty() ? QString(kPendingRoomId) : savedRoomId;
    saved.displayName = displayName();
    const bool savedIsCurrent =
        !savedRoomId.isEmpty() && savedRoomId == currentRoomId;

    if (!savedIsCurrent) {
        out.push_back(saved);
    }
    for (const auto &room : rooms) {
        if (room.roomId == currentRoomId || room.roomId == savedRoomId) {
            continue;
        }
        out.push_back(room);
    }
    return out;
}

} // namespace TeleMatrix::SavedMessages

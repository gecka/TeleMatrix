// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "window/notifications_linux_registry.h"

namespace TeleMatrix::Notifications {

void LinuxNotificationRegistry::add(const QString &roomId, uint notificationId) {
    if (roomId.isEmpty() || notificationId == 0) {
        return;
    }
    // Re-key if this id was tracked elsewhere (the daemon reused it).
    forget(notificationId);
    _byRoom[roomId].append(notificationId);
    _roomById.insert(notificationId, roomId);
}

QString LinuxNotificationRegistry::roomForId(uint notificationId) const {
    return _roomById.value(notificationId);
}

void LinuxNotificationRegistry::forget(uint notificationId) {
    const auto it = _roomById.constFind(notificationId);
    if (it == _roomById.cend()) {
        return;
    }
    const QString room = it.value();
    _roomById.erase(it);

    const auto roomIt = _byRoom.find(room);
    if (roomIt != _byRoom.end()) {
        roomIt->removeAll(notificationId);
        if (roomIt->isEmpty()) {
            _byRoom.erase(roomIt);
        }
    }
}

QList<uint> LinuxNotificationRegistry::takeRoom(const QString &roomId) {
    const QList<uint> ids = _byRoom.take(roomId);
    for (const uint id : ids) {
        _roomById.remove(id);
    }
    return ids;
}

QList<uint> LinuxNotificationRegistry::takeAll() {
    QList<uint> ids;
    ids.reserve(int(_roomById.size()));
    for (auto it = _roomById.cbegin(); it != _roomById.cend(); ++it) {
        ids.append(it.key());
    }
    _roomById.clear();
    _byRoom.clear();
    return ids;
}

} // namespace TeleMatrix::Notifications

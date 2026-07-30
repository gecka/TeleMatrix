// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QHash>
#include <QList>
#include <QString>

namespace TeleMatrix::Notifications {

// The freedesktop notification spec has no "group" concept (unlike the WinRT
// toast Group key), so to clear a room's toasts on read we must remember every
// live notification id the daemon handed back and `CloseNotification` each one.
// This is that bookkeeping: a two-way roomId <-> notification-id map. Pure (no
// D-Bus), so it is unit-testable on any platform.
class LinuxNotificationRegistry {
public:
    /// Remember that `notificationId` (from the Notify reply) belongs to `roomId`.
    /// No-op for an empty room or a 0 id. If the id was already tracked under a
    /// different room, it is moved. `eventId` may be empty (a grouped or
    /// event-less toast); clicking such a toast just opens the room.
    void add(const QString &roomId, const QString &eventId, uint notificationId);

    /// Room a notification belongs to, or empty if untracked.
    [[nodiscard]] QString roomForId(uint notificationId) const;

    /// Message the notification was raised for, so a click can jump to it rather
    /// than opening the room at its default position. Empty when unknown.
    [[nodiscard]] QString eventForId(uint notificationId) const;

    /// Drop a single id (e.g. on a NotificationClosed signal).
    void forget(uint notificationId);

    /// Remove and return all ids for a room (to CloseNotification each).
    [[nodiscard]] QList<uint> takeRoom(const QString &roomId);

    /// Remove and return every tracked id.
    [[nodiscard]] QList<uint> takeAll();

private:
    QHash<QString, QList<uint>> _byRoom;
    QHash<uint, QString> _roomById;
    QHash<uint, QString> _eventById;
};

} // namespace TeleMatrix::Notifications

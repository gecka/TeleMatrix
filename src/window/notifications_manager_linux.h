// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "window/notifications_manager.h"
#include "window/notifications_linux_registry.h"

#include <QHash>
#include <QStringList>
#include <QVariantList>

class QDBusInterface;

namespace TeleMatrix {

class AppMainWindow;

namespace Notifications {

/// Linux native notification manager using the freedesktop D-Bus service
/// `org.freedesktop.Notifications` (via Qt DBus). Needs the main window for
/// the X11 taskbar-urgency flash (`bounceDockIcon`); the unread badge goes
/// through Qt's cross-platform `setBadgeNumber` (Unity LauncherEntry on Linux).
class LinuxManager : public Manager {
    Q_OBJECT

public:
    explicit LinuxManager(AppMainWindow *window);
    ~LinuxManager() override;

    void showNotification(
        const QString &roomId,
        const QString &eventId,
        const QString &senderName,
        const QString &chatName,
        const QString &messageText,
        bool isDirect,
        bool isMention,
        const QString &avatarPath,
        bool isInvite) override;

    void clearFromRoom(const QString &roomId) override;
    void clearAll() override;
    void updateDockBadge(int totalUnread) override;
    void bounceDockIcon() override;

private Q_SLOTS:
    // Direct org.freedesktop.Notifications signals.
    void handleActionInvoked(uint id, const QString &actionKey);
    void handleNotificationClosed(uint id, uint reason);
    void handleNotificationReplied(uint id, const QString &text);
    // XDG portal (Flatpak/Snap) signal: (id, action, parameters).
    void handlePortalActionInvoked(
        const QString &id,
        const QString &action,
        const QVariantList &parameters);

private:
    void closeNotification(uint id);

    AppMainWindow *_window = nullptr;

    // Sandboxed installs (Flatpak/Snap) can't reach the direct service, so route
    // through org.freedesktop.portal.Notification instead.
    bool _usePortal = false;
    QDBusInterface *_iface = nullptr;  // direct service (non-sandboxed)
    QDBusInterface *_portal = nullptr; // portal (sandboxed)

    LinuxNotificationRegistry _registry; // direct: uint ids
    QHash<QString, QStringList> _portalRoomIds; // portal: room -> string ids
    QHash<QString, QString> _portalIdRoom;       // portal: string id -> room

    // Server capabilities from GetCapabilities (e.g. "actions", "inline-reply",
    // "body-markup"); empty until the async query returns.
    QStringList _capabilities;
};

} // namespace Notifications
} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.
//
// Linux-only: compiled solely on UNIX-and-not-Apple (see the if(UNIX AND NOT
// APPLE) block in CMakeLists.txt). Uses Qt DBus. Authored on macOS — not built
// or verified there; needs a Linux build.

#include "window/notifications_manager_linux.h"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QFileInfo>
#include <QGuiApplication>
#include <QStringList>
#include <QVariantMap>

#include "app/app_main_window.h"

namespace TeleMatrix::Notifications {

namespace {
// The freedesktop notifications service. Interface name == service name.
QString service() { return QStringLiteral("org.freedesktop.Notifications"); }
QString objectPath() { return QStringLiteral("/org/freedesktop/Notifications"); }
// Must match the installed .desktop file (resources/linux/...) and the name set
// via QGuiApplication::setDesktopFileName() in main.cpp — the Linux analogue of
// the Windows AUMID, used for the icon and Wayland app-id.
QString desktopEntry() { return QStringLiteral("dev.telematrix.TeleMatrix"); }

// XDG Desktop Portal notification interface (used under Flatpak/Snap).
QString portalService() { return QStringLiteral("org.freedesktop.portal.Desktop"); }
QString portalPath() { return QStringLiteral("/org/freedesktop/portal/desktop"); }
QString portalIface() { return QStringLiteral("org.freedesktop.portal.Notification"); }

// In a Flatpak (/.flatpak-info) or Snap ($SNAP) sandbox the direct
// org.freedesktop.Notifications service is unreachable, so route via the portal.
bool isSandboxed() {
    return QFileInfo::exists(QStringLiteral("/.flatpak-info"))
        || qEnvironmentVariableIsSet("SNAP");
}

// The portal keys notifications on an app-chosen string id (same id replaces).
// Matrix event ids are globally unique, so use that (room id as a fallback).
QString portalId(const QString &roomId, const QString &eventId) {
    return eventId.isEmpty() ? roomId : eventId;
}

// Escape the fdo body-markup subset so a plain message containing <, >, & isn't
// mis-rendered (or used for markup injection) on servers that parse markup.
QString escapeBodyMarkup(const QString &text) {
    QString out;
    out.reserve(text.size());
    for (const QChar ch : text) {
        switch (ch.unicode()) {
        case u'&': out += QStringLiteral("&amp;"); break;
        case u'<': out += QStringLiteral("&lt;"); break;
        case u'>': out += QStringLiteral("&gt;"); break;
        default: out += ch; break;
        }
    }
    return out;
}
} // namespace

LinuxManager::LinuxManager(AppMainWindow *window)
    : _window(window)
    , _usePortal(isSandboxed()) {
    if (_usePortal) {
        _portal = new QDBusInterface(
            portalService(), portalPath(), portalIface(),
            QDBusConnection::sessionBus(), this);
        QDBusConnection::sessionBus().connect(
            portalService(), portalPath(), portalIface(),
            QStringLiteral("ActionInvoked"), this,
            SLOT(handlePortalActionInvoked(QString, QString, QVariantList)));
        return;
    }

    _iface = new QDBusInterface(
        service(), objectPath(), service(), QDBusConnection::sessionBus(), this);

    auto bus = QDBusConnection::sessionBus();
    bus.connect(
        service(), objectPath(), service(), QStringLiteral("ActionInvoked"),
        this, SLOT(handleActionInvoked(uint, QString)));
    bus.connect(
        service(), objectPath(), service(), QStringLiteral("NotificationClosed"),
        this, SLOT(handleNotificationClosed(uint, uint)));
    // KDE inline-reply extension; harmless on servers that never emit it.
    bus.connect(
        service(), objectPath(), service(), QStringLiteral("NotificationReplied"),
        this, SLOT(handleNotificationReplied(uint, QString)));

    // Capability negotiation: learn what the server (GNOME / KDE / ...) supports
    // so we adapt the payload — markup escaping now, action buttons later.
    if (_iface) {
        auto *caps = new QDBusPendingCallWatcher(
            _iface->asyncCall(QStringLiteral("GetCapabilities")), this);
        connect(
            caps, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *call) {
                const QDBusPendingReply<QStringList> reply = *call;
                if (!reply.isError()) {
                    _capabilities = reply.value();
                }
                call->deleteLater();
            });
    }
}

LinuxManager::~LinuxManager() {
    clearAll();
}

void LinuxManager::showNotification(
    const QString &roomId,
    const QString &eventId,
    const QString &senderName,
    const QString &chatName,
    const QString &messageText,
    bool isDirect,
    bool isMention,
    const QString &avatarPath,
    bool isInvite) {
    // fdo/portal have one title (summary) + body, no subtitle. Map: DM ->
    // summary=sender, body=text; group -> summary=room, body="sender: text".
    const bool group = !isDirect && !chatName.isEmpty();
    QString summary = group ? chatName : senderName;
    if (summary.isEmpty()) {
        summary = QStringLiteral("TeleMatrix");
    }
    QString body = (group && !senderName.isEmpty())
        ? (senderName + QStringLiteral(": ") + messageText)
        : messageText;

    if (_usePortal) {
        if (!_portal) {
            return;
        }
        const QString id = portalId(roomId, eventId);
        QVariantMap notification;
        notification.insert(QStringLiteral("title"), summary);
        notification.insert(QStringLiteral("body"), body);
        notification.insert(
            QStringLiteral("priority"),
            isMention ? QStringLiteral("urgent") : QStringLiteral("normal"));
        notification.insert(
            QStringLiteral("default-action"), QStringLiteral("default"));
        _portal->asyncCall(QStringLiteral("AddNotification"), id, notification);
        _portalRoomIds[roomId].append(id);
        _portalIdRoom.insert(id, roomId);
        if (!eventId.isEmpty()) {
            _portalIdEvent.insert(id, eventId);
        }
        return;
    }

    if (!_iface) {
        return;
    }
    if (_capabilities.contains(QStringLiteral("body-markup"))) {
        body = escapeBodyMarkup(body);
    }

    // "default" is the action invoked when the toast body is clicked. Reply /
    // mark-read buttons are added only if the server advertises support.
    QStringList actions{ QStringLiteral("default"), QStringLiteral("Open") };

    QVariantMap hints;
    hints.insert(QStringLiteral("desktop-entry"), desktopEntry());
    hints.insert(QStringLiteral("category"), QStringLiteral("im.received"));
    // urgency byte: low/normal/critical = 0/1/2. Mentions are critical so they
    // surface under DND (parallels macOS time-sensitive / Windows urgent).
    hints.insert(
        QStringLiteral("urgency"),
        QVariant::fromValue<uchar>(isMention ? 2 : 1));
    if (!avatarPath.isEmpty()) {
        // fdo "image-path": absolute path / file URI to the sender avatar; the
        // server crops and scales it.
        hints.insert(QStringLiteral("image-path"), avatarPath);
    }
    // Invites carry no Reply / Mark-as-read actions (they don't apply); the body
    // click ("default") still opens the room.
    if (!isInvite && _capabilities.contains(QStringLiteral("actions"))) {
        actions << QStringLiteral("mark-read") << QStringLiteral("Mark as read");
    }
    if (!isInvite && _capabilities.contains(QStringLiteral("inline-reply"))) {
        actions << QStringLiteral("inline-reply") << QStringLiteral("Reply");
        hints.insert(
            QStringLiteral("x-kde-reply-placeholder-text"),
            QStringLiteral("Reply"));
    }

    const QDBusPendingCall pending = _iface->asyncCall(
        QStringLiteral("Notify"),
        QStringLiteral("TeleMatrix"), // app_name
        uint(0),                      // replaces_id (none; System de-dups upstream)
        desktopEntry(),               // app_icon (themed icon name)
        summary,
        body,
        actions,
        hints,
        int(-1)); // expire_timeout: server default

    auto *watcher = new QDBusPendingCallWatcher(pending, this);
    const QString room = roomId;
    const QString event = eventId;
    QObject::connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, room, event](QDBusPendingCallWatcher *call) {
            const QDBusPendingReply<uint> reply = *call;
            if (!reply.isError()) {
                _registry.add(room, event, reply.value());
            }
            call->deleteLater();
        });
}

void LinuxManager::clearFromRoom(const QString &roomId) {
    if (_usePortal) {
        const QStringList ids = _portalRoomIds.take(roomId);
        for (const QString &id : ids) {
            _portalIdRoom.remove(id);
            _portalIdEvent.remove(id);
            if (_portal) {
                _portal->asyncCall(QStringLiteral("RemoveNotification"), id);
            }
        }
        return;
    }
    const QList<uint> ids = _registry.takeRoom(roomId);
    for (const uint id : ids) {
        closeNotification(id);
    }
}

void LinuxManager::clearAll() {
    if (_usePortal) {
        const QList<QString> ids = _portalIdRoom.keys();
        _portalIdRoom.clear();
        _portalIdEvent.clear();
        _portalRoomIds.clear();
        for (const QString &id : ids) {
            if (_portal) {
                _portal->asyncCall(QStringLiteral("RemoveNotification"), id);
            }
        }
        return;
    }
    const QList<uint> ids = _registry.takeAll();
    for (const uint id : ids) {
        closeNotification(id);
    }
}

void LinuxManager::closeNotification(uint id) {
    if (_iface && id != 0) {
        _iface->asyncCall(QStringLiteral("CloseNotification"), id);
    }
}

void LinuxManager::updateDockBadge(int totalUnread) {
    // Qt routes this to the Unity LauncherEntry on Linux (needs the desktop file
    // name set in main.cpp). Qt >= 6.6. Called on the instance — it's a non-static
    // member on Linux/gcc Qt (a static-style call fails to compile there).
    qApp->setBadgeNumber(qMax(0, totalUnread));
}

void LinuxManager::bounceDockIcon() {
    // X11: taskbar urgency via Qt's alert(); no-op on Wayland.
    if (_window) {
        QApplication::alert(_window);
    }
}

void LinuxManager::handleActionInvoked(uint id, const QString &actionKey) {
    const QString room = _registry.roomForId(id);
    if (room.isEmpty()) {
        return;
    }
    if (actionKey == QStringLiteral("default")) {
        emit notificationActivated(room, _registry.eventForId(id));
    } else if (actionKey == QStringLiteral("mark-read")) {
        emit notificationMarkRead(room);
    }
    // "inline-reply" arrives via the NotificationReplied signal, not here.
}

void LinuxManager::handleNotificationClosed(uint id, uint reason) {
    Q_UNUSED(reason);
    _registry.forget(id);
}

void LinuxManager::handleNotificationReplied(uint id, const QString &text) {
    const QString room = _registry.roomForId(id);
    if (!room.isEmpty()) {
        emit notificationReplied(room, text);
    }
}

void LinuxManager::handlePortalActionInvoked(
    const QString &id,
    const QString &action,
    const QVariantList &parameters) {
    Q_UNUSED(parameters);
    if (action != QStringLiteral("default")) {
        return;
    }
    const QString room = _portalIdRoom.value(id);
    if (!room.isEmpty()) {
        emit notificationActivated(room, _portalIdEvent.value(id));
    }
}

} // namespace TeleMatrix::Notifications

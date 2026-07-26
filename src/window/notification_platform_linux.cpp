// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.
//
// Linux/BSD only (see if(UNIX AND NOT APPLE) in CMakeLists.txt). Authored on
// macOS; needs a Linux build to verify.

#include "window/notification_platform.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>

namespace TeleMatrix::Notifications::Platform {

bool shouldSuppressAlerts() {
    // Screen locked / screensaver active — the common cross-DE D-Bus interface
    // (GNOME, KDE, and others implement org.freedesktop.ScreenSaver). DND/
    // fullscreen suppression beyond this is left to the notification daemon.
    QDBusInterface screensaver(
        QStringLiteral("org.freedesktop.ScreenSaver"),
        QStringLiteral("/org/freedesktop/ScreenSaver"),
        QStringLiteral("org.freedesktop.ScreenSaver"),
        QDBusConnection::sessionBus());
    if (screensaver.isValid()) {
        const QDBusReply<bool> active =
            screensaver.call(QStringLiteral("GetActive"));
        if (active.isValid()) {
            return active.value();
        }
    }
    return false;
}

} // namespace TeleMatrix::Notifications::Platform

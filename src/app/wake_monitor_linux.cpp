// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.
//
// Linux/BSD-only: compiled solely in the `UNIX AND NOT APPLE` block of
// CMakeLists.txt. Not built or verified on the macOS dev host.

#include "app/wake_monitor.h"

#include <QtDBus/QDBusConnection>

namespace TeleMatrix {

WakeMonitor::WakeMonitor(QObject *parent) : QObject(parent) {
    // logind emits PrepareForSleep(true) before suspending and (false) after
    // resuming; only the resume edge interests us. Absent logind (or a system
    // bus) the connect simply fails and the signal never fires.
    QDBusConnection::systemBus().connect(
        QStringLiteral("org.freedesktop.login1"),
        QStringLiteral("/org/freedesktop/login1"),
        QStringLiteral("org.freedesktop.login1.Manager"),
        QStringLiteral("PrepareForSleep"),
        this,
        SLOT(handleSleepStateChanged(bool)));
}

WakeMonitor::~WakeMonitor() = default;

void WakeMonitor::handleSleepStateChanged(bool sleeping) {
    if (!sleeping) {
        Q_EMIT woke();
    }
}

} // namespace TeleMatrix

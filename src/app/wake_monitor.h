// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QObject>

namespace TeleMatrix {

/// Reports that the machine resumed from sleep.
///
/// NetworkMonitor cannot stand in for this: it only fires on a reachability
/// EDGE, and waking usually leaves Wi-Fi reporting "reachable" the whole time,
/// so nothing tells the sync loops their sockets died with the suspend. Without
/// this signal a wake is only noticed when the SDK's own offline probe happens
/// to succeed, which can take tens of seconds.
///
/// Backed by NSWorkspaceDidWakeNotification on macOS and logind's
/// PrepareForSleep on Linux; elsewhere it simply never fires.
class WakeMonitor : public QObject {
    Q_OBJECT

public:
    explicit WakeMonitor(QObject *parent = nullptr);
    ~WakeMonitor() override;

signals:
    /// Emitted on the main thread, once per resume.
    void woke();

private slots:
    /// logind's PrepareForSleep(bool), which needs a real slot for the D-Bus
    /// connection. Declared for every platform so the metaobject matches; only
    /// the Linux build does anything with it.
    void handleSleepStateChanged(bool sleeping);

private:
    // Platform observer token (an NSObject on macOS); unused elsewhere.
    [[maybe_unused]] void *_impl = nullptr;
};

} // namespace TeleMatrix

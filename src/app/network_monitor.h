// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QObject>

namespace TeleMatrix {

/// Thin wrapper over QNetworkInformation reporting OS-level reachability
/// (interface up/down) as a simple online/offline signal. Push-based: the OS
/// notifies instantly, unlike sync-request failures that only surface on a
/// long-poll timeout. Drives the connection indicator and a fast reconnect.
class NetworkMonitor : public QObject {
    Q_OBJECT

public:
    explicit NetworkMonitor(QObject *parent = nullptr);

    /// True if the OS reports a usable network (anything but Disconnected).
    /// False only when the OS reports no network at all.
    [[nodiscard]] bool online() const { return _online; }

    /// True if a platform backend loaded; otherwise reachability is unavailable
    /// and online() is optimistically true (never block the app on a missing
    /// backend).
    [[nodiscard]] bool available() const { return _available; }

signals:
    void onlineChanged(bool online);

private:
    bool _online = true;
    bool _available = false;
};

} // namespace TeleMatrix

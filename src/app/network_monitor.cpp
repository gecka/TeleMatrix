// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "app/network_monitor.h"

#include <QNetworkInformation>
#include <QtGlobal>

namespace TeleMatrix {

namespace {

bool reachabilityIsOnline(QNetworkInformation::Reachability r) {
    // Treat anything but an explicit Disconnected as online: Local/Site/Unknown
    // still mean some connectivity, and Qt warns its "Online" probe can
    // false-negative (blocked probe server, captive portal). We only want a hard
    // "no network at all" to drive "Waiting for network…".
    return r != QNetworkInformation::Reachability::Disconnected;
}

} // namespace

NetworkMonitor::NetworkMonitor(QObject *parent) : QObject(parent) {
    _available = QNetworkInformation::loadDefaultBackend();
    if (!_available) {
        qWarning("[net] QNetworkInformation backend unavailable; staying online");
        return; // no backend → stay optimistically online
    }
    auto *info = QNetworkInformation::instance();
    _online = reachabilityIsOnline(info->reachability());
    qInfo("[net] monitor active (online=%d)", _online);
    connect(info, &QNetworkInformation::reachabilityChanged, this,
        [this](QNetworkInformation::Reachability r) {
            const auto online = reachabilityIsOnline(r);
            if (online != _online) {
                _online = online;
                qInfo("[net] reachability changed (online=%d)", _online);
                emit onlineChanged(_online);
            }
        });
}

} // namespace TeleMatrix

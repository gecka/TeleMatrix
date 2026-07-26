// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "app/single_instance.h"

#include <QDir>
#include <QElapsedTimer>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QThread>

namespace TeleMatrix {
namespace {

// QLocalServer names have platform length limits (macOS sun_path ~104 chars,
// Windows pipe names), so derive a short, stable, per-profile name from the
// data dir. Distinct profiles get distinct names and can each run an instance.
QString instanceName(const QString &dataDir) {
    return QStringLiteral("telematrix-")
        + QString::number(qHash(dataDir), 16);
}

} // namespace

SingleInstance::SingleInstance(QObject *parent) : QObject(parent) {}

SingleInstance::~SingleInstance() = default;

bool SingleInstance::acquire(bool waitForPrimaryExit) {
    const auto dataDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir); // QLockFile needs the parent dir to exist.
    _serverName = instanceName(dataDir);

    // QLockFile gives atomic primary/secondary arbitration and recovers a lock
    // left by a crashed primary automatically (it stores PID + host and treats
    // the lock as stale once that process is gone).
    _lock = std::make_unique<QLockFile>(
        dataDir + QStringLiteral("/single_instance.lock"));

    auto locked = _lock->tryLock();
    if (!locked && waitForPrimaryExit) {
        // Relaunch we spawned during our own restart: the departing instance still
        // holds the lock (released only once it fully exits, which also frees the
        // shared SQLite stores and the loopback proxy port). Poll for it to go, up
        // to a generous cap, then take over — instead of treating it as a live
        // primary and exiting, which is what makes "restart" behave like "quit".
        QElapsedTimer timer;
        timer.start();
        while (!locked && timer.elapsed() < 10000) {
            QThread::msleep(50);
            locked = _lock->tryLock();
        }
    }

    if (!locked) {
        // A primary instance is running — ask it to come to the front, then tell
        // the caller to exit. (If the connect fails the primary may be mid-
        // shutdown; either way we are not the primary and must not start.)
        QLocalSocket socket;
        socket.connectToServer(_serverName);
        if (socket.waitForConnected(1000)) {
            socket.write("activate");
            socket.waitForBytesWritten(1000);
            socket.disconnectFromServer();
            if (socket.state() != QLocalSocket::UnconnectedState) {
                socket.waitForDisconnected(1000);
            }
        }
        _lock.reset();
        return false;
    }

    // Primary: clear any socket left by a previous crashed primary, then listen
    // for activation pings from future launches.
    QLocalServer::removeServer(_serverName);
    connect(&_server, &QLocalServer::newConnection, this, [this] {
        while (auto *client = _server.nextPendingConnection()) {
            emit activateRequested();
            connect(client, &QLocalSocket::disconnected,
                    client, &QLocalSocket::deleteLater);
            client->disconnectFromServer();
        }
    });
    if (!_server.listen(_serverName)) {
        // A stale socket can block listen(); clear and retry once. If it still
        // fails we keep running (single-instance IPC is best-effort).
        QLocalServer::removeServer(_serverName);
        _server.listen(_serverName);
    }
    return true;
}

} // namespace TeleMatrix

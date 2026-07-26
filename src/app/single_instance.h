// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QObject>
#include <QLocalServer>

#include <memory>

class QLockFile;

namespace TeleMatrix {

/// Enforces a single running instance per user profile (data dir).
///
/// `acquire()` returns true for the first (primary) instance, which keeps the
/// lock + a QLocalServer alive for the process lifetime. For a second launch it
/// pings the running primary to come to the front and returns false, so the
/// caller exits before any heavy init (e.g. opening the shared SQLite stores).
/// The primary emits activateRequested() whenever another launch is attempted.
///
/// `waitForPrimaryExit` is set when this process is a relaunch we spawned during
/// our own restart (see AppController::restartApplication): the departing instance
/// still holds the lock, so we briefly wait for it to fully exit — which also frees
/// its exclusive resources (the SQLite stores and the loopback proxy port) — and
/// then take over, rather than deferring to a corpse and exiting (that would turn
/// a "restart" into a plain "quit").
class SingleInstance : public QObject {
    Q_OBJECT

public:
    explicit SingleInstance(QObject *parent = nullptr);
    ~SingleInstance() override;

    [[nodiscard]] bool acquire(bool waitForPrimaryExit = false);

signals:
    void activateRequested();

private:
    std::unique_ptr<QLockFile> _lock;
    QLocalServer _server;
    QString _serverName;
};

} // namespace TeleMatrix

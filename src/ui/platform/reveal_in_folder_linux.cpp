// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.
//
// Linux/BSD-only: compiled solely in the `UNIX AND NOT APPLE` block of
// CMakeLists.txt. Not built or verified on the macOS dev host.

#include "ui/platform/reveal_in_folder.h"

#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusPendingCall>
#include <QtDBus/QDBusPendingCallWatcher>
#include <QtDBus/QDBusUnixFileDescriptor>

#include <fcntl.h>
#include <unistd.h>

#include <functional>
#include <utility>

namespace TeleMatrix::Platform {
namespace {

// A file manager that isn't running yet has to be D-Bus activated first, which
// can take seconds, so every call here is asynchronous — a blocking call would
// freeze the UI for the whole launch. 10s is well past any real activation and
// only bounds the wait before we fall through to the next strategy.
constexpr auto kCallTimeoutMs = 10 * 1000;

void CallAsync(const QDBusMessage &message, std::function<void()> fail) {
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        fail();
        return;
    }
    const auto watcher = new QDBusPendingCallWatcher(
        bus.asyncCall(message, kCallTimeoutMs));
    QObject::connect(
        watcher,
        &QDBusPendingCallWatcher::finished,
        watcher,
        [fail = std::move(fail)](QDBusPendingCallWatcher *watcher) {
            const auto failed = watcher->isError();
            watcher->deleteLater();
            if (failed) {
                fail();
            }
        });
}

// org.freedesktop.FileManager1 is the freedesktop.org interface implemented by
// Nautilus, Dolphin, Nemo, Thunar & co, and the only one of the three that
// actually selects the file rather than just opening its folder.
void FileManagerShowItems(const QString &filepath, std::function<void()> fail) {
    auto message = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.FileManager1"),
        QStringLiteral("/org/freedesktop/FileManager1"),
        QStringLiteral("org.freedesktop.FileManager1"),
        QStringLiteral("ShowItems"));
    message.setArguments({
        QStringList{ QUrl::fromLocalFile(filepath).toString() },
        QString(), // startup id — we have no activation token to hand over
    });
    CallAsync(message, std::move(fail));
}

// The XDG desktop portal can only open the containing directory, but it is what
// a sandboxed (Flatpak/Snap) build can reach when the host file manager's own
// name is not on its bus.
void PortalOpenDirectory(const QString &filepath, std::function<void()> fail) {
    const auto fd = ::open(
        QFile::encodeName(filepath).constData(),
        O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
        fail();
        return;
    }
    // QDBusUnixFileDescriptor duplicates the descriptor it is given.
    const auto descriptor = QDBusUnixFileDescriptor(fd);
    ::close(fd);

    auto message = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.portal.Desktop"),
        QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.OpenURI"),
        QStringLiteral("OpenDirectory"));
    message.setArguments({
        QString(), // parent window handle
        QVariant::fromValue(descriptor),
        QVariantMap(),
    });
    CallAsync(message, std::move(fail));
}

} // namespace

void RevealInFolder(const QString &filepath) {
    FileManagerShowItems(filepath, [filepath] {
        PortalOpenDirectory(filepath, [filepath] {
            QDesktopServices::openUrl(
                QUrl::fromLocalFile(QFileInfo(filepath).absolutePath()));
        });
    });
}

} // namespace TeleMatrix::Platform

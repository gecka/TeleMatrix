// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>
#include <QtWidgets/QFocusFrame>
#include <QtGui/QIcon>
#include <QtCore/QEvent>

#include "app/app_controller.h"
#include "app/single_instance.h"
#include "styles/style_constants.h"

#ifdef Q_OS_WIN
#include "app/platform/win/app_user_model_id.h"
#endif

#include <csignal>

using namespace Qt::Literals::StringLiterals;

namespace {
// Handle SIGTERM/SIGINT for clean shutdown (triggers aboutToQuit).
void signalHandler(int) {
    if (qApp) {
        qApp->quit();
    }
}

#ifdef Q_OS_MACOS
// The UI draws its own focus visuals (e.g. the blue border
// on input fields); it never uses the native macOS focus ring. Custom-painted
// widgets re-expose that ring, so a focused field/button shows a stray gray halo
// on top of its own focus border. Suppress the native ring on every widget via
// WA_MacShowFocusRect (no-op off macOS) instead of patching each widget class.
//
// On macOS 26 WA_MacShowFocusRect is no longer honored for line edits: the style
// still spawns a QFocusFrame companion that paints the ring around the field. So
// also hide any QFocusFrame the moment it appears — the UI never uses one.
class FocusRingDisabler final : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (event->type() == QEvent::Show) {
            if (auto *frame = qobject_cast<QFocusFrame *>(object)) {
                frame->hide();
                return true;
            }
            if (auto *widget = qobject_cast<QWidget *>(object)) {
                widget->setAttribute(Qt::WA_MacShowFocusRect, false);
            }
        }
        return QObject::eventFilter(object, event);
    }
};
#endif
} // namespace

int main(int argc, char *argv[]) {
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGINT, signalHandler);

    QApplication app(argc, argv);
    app.setFont(st::baseFont(st::fsize));

#ifdef Q_OS_MACOS
    // Disable the native macOS focus ring app-wide (see FocusRingDisabler).
    app.installEventFilter(new FocusRingDisabler(&app));
#endif

    // macOS-appropriate application settings.
    app.setApplicationName(u"TeleMatrix"_s);
    app.setApplicationVersion(QStringLiteral(TELEMATRIX_VERSION_STR));
    app.setOrganizationName(u"TeleMatrix"_s);
    app.setOrganizationDomain(u"telematrix.app"_s);
    app.setWindowIcon(QIcon(u":/telematrix/app/icon.png"_s));

#ifdef Q_OS_LINUX
    // Matches resources/linux/dev.telematrix.TeleMatrix.desktop — drives the
    // notification icon (desktop-entry hint), the Wayland app-id, and the Unity
    // launcher badge (setBadgeNumber).
    QGuiApplication::setDesktopFileName(u"dev.telematrix.TeleMatrix"_s);
#endif

#ifdef Q_OS_WIN
    // WinRT toasts only appear if the process has an explicit AUMID *and* a
    // matching Start-Menu shortcut. Do this before any window is shown.
    TeleMatrix::Platform::Win::setAppUserModelId();
    TeleMatrix::Platform::Win::ensureStartMenuShortcut();
#endif

    // Enforce a single running instance per profile. A second launch pings the
    // running one to come to the front and exits here, before any heavy init
    // (so two processes never open the same data-dir SQLite stores).
    TeleMatrix::SingleInstance singleInstance;
    // A relaunch we spawned during our own restart carries --relaunched: wait for
    // the departing instance to release the lock instead of deferring to it and
    // exiting (see AppController::restartApplication / SingleInstance::acquire).
    const auto relaunched =
        app.arguments().contains(QStringLiteral("--relaunched"));
    if (!singleInstance.acquire(relaunched)) {
        return 0;
    }

    TeleMatrix::AppController controller;
    QObject::connect(
        &singleInstance, &TeleMatrix::SingleInstance::activateRequested,
        &controller, &TeleMatrix::AppController::bringToFront);
    controller.start();
    // start() runs a blocking loop (e.g. the vault-unlock screen) BEFORE app.exec().
    // If the user quit there, entering app.exec() would start a fresh main loop and
    // leave the app running (the quit is lost when the nested loop unwinds), so skip
    // it and let main() return -> teardown.
    if (controller.startupQuitRequested()) {
        return 0;
    }

    return app.exec();
}

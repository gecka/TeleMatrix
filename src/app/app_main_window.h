// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QMainWindow>

namespace TeleMatrix {

class AppController;

/// Reason for application quit — determines whether confirm-quit dialog shows.
enum class QuitReason {
    KeyboardShortcut,  // Cmd+Q / Ctrl+Q
    MenuAction,        // File → Quit (mouse click)
    WindowClose,       // Close button
    Programmatic,      // API call
    Signal,            // SIGTERM / SIGINT
    Restart,           // Internal restart (font/scale change)
};

/// Main application window.
/// Sets up the macOS-appropriate window frame, menu bar, and size persistence.
class AppMainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit AppMainWindow(AppController *controller, QWidget *parent = nullptr);
    ~AppMainWindow() override = default;

    /// Save the current window position into Core::Settings.
    void savePositionToSettings();

    /// Restore saved window geometry from settings.
    void restoreWindowState();

    /// Un-minimize, show, raise and activate the window (also force-activates the
    /// app on macOS to defeat focus-stealing prevention).
    void bringToFront();

    /// Reset to default size and center on the primary screen. Supersedes any
    /// pending geometry restore, and stops the centred size from being persisted
    /// over the user's own — restoreWindowState() owns the geometry again.
    void useDefaultCentered();

    /// Whether the window is currently the active (focused) window.
    [[nodiscard]] bool isWindowActive() const { return _windowActive; }

    /// Request application quit with a specific reason.
    /// Only KeyboardShortcut on macOS (with setting enabled) shows confirm dialog.
    void requestQuit(QuitReason reason = QuitReason::MenuAction);

Q_SIGNALS:
    void windowActiveChanged(bool active);
    void settingsRequested();
    void exploreRoomsRequested();
    /// Cmd/Ctrl+F: search in the open room, or the chat list when none is open.
    void searchRequested();
    /// The user asked to quit (Cmd+Q or window close). Lets a nested startup loop
    /// (e.g. the vault unlock screen) break out so the process can actually exit.
    void quitRequested();

protected:
    void closeEvent(QCloseEvent *e) override;
    void moveEvent(QMoveEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void changeEvent(QEvent *e) override;
    void showEvent(QShowEvent *e) override;

private:
    void setupMenuBar();
    void showAboutBox();

    /// Register the Ctrl+Shift+1..N account-switch shortcuts.
    void setupAccountShortcuts();

    /// Register menu-less shortcuts: Ctrl/Cmd+K (explore rooms), Ctrl/Cmd+F (search).
    void setupUtilityShortcuts();

    AppController *_controller = nullptr;
    bool _windowActive = false;
    bool _positionPersistenceEnabled = false;
    // Geometry restoreWindowState() asked for, re-applied once the native window
    // exists — Windows can ignore a pre-show setGeometry(). Empty when the window
    // was centred at its default size instead.
    QRect _restoreGeometry;
    bool _restoreMaximized = false;
    bool _restorePendingShow = false;
    // Bumped every time the intended geometry changes. A queued restore carries
    // the epoch it was scheduled under and gives up when it no longer matches, so
    // it can never undo a centring that happened after it was queued.
    quint64 _geometryEpoch = 0;
    // True while the window shows a transient full-window screen (welcome, vault
    // unlock) at the default centred size.
    bool _defaultCentered = false;
    void settleRestoredGeometry();
    void applyDefaultCenteredGeometry();
};

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QObject>

class QSystemTrayIcon;

namespace TeleMatrix {

class AppMainWindow;

/// System-tray icon with the unread count rendered onto the app icon, a context
/// menu (Open / Quit), and click-to-raise. On Linux this is a StatusNotifierItem
/// (via Qt). The class is cross-platform (compiles everywhere) but is only
/// instantiated where wanted (Linux).
class TrayIcon : public QObject {
    Q_OBJECT

public:
    explicit TrayIcon(AppMainWindow *window, QObject *parent = nullptr);
    ~TrayIcon() override;

    /// Update the badge drawn on the tray icon (0 hides it).
    void setUnreadCount(int count);

private:
    void render();

    AppMainWindow *_window = nullptr;
    QSystemTrayIcon *_tray = nullptr;
    int _count = 0;
};

} // namespace TeleMatrix

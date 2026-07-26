// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "window/notifications_manager.h"

#include <memory>

namespace TeleMatrix {

class AppMainWindow;

namespace Notifications {

/// Windows native notification manager using WinRT toasts
/// (`Windows.UI.Notifications`). Needs the main window to resolve the HWND for
/// taskbar flash (`bounceDockIcon`) and the unread overlay badge
/// (`updateDockBadge`).
class WinManager : public Manager {
    Q_OBJECT

public:
    explicit WinManager(AppMainWindow *window);
    ~WinManager() override;

    void showNotification(
        const QString &roomId,
        const QString &eventId,
        const QString &senderName,
        const QString &chatName,
        const QString &messageText,
        bool isDirect,
        bool isMention,
        const QString &avatarPath,
        bool isInvite) override;

    void clearFromRoom(const QString &roomId) override;
    void clearAll() override;
    void updateDockBadge(int totalUnread) override;
    void bounceDockIcon() override;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
    AppMainWindow *_window = nullptr;
};

} // namespace Notifications
} // namespace TeleMatrix

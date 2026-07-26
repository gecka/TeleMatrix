// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QObject>
#include <QHash>
#include <QSet>
#include <QImage>
#include <QSoundEffect>
#include <QTimer>
#include <QVector>

#include "protocol/protocol_types.h"

namespace TeleMatrix {

class ProtocolBridge;
class AppMainWindow;
class AppMainWidget;
class UnreadStateStore;
class AccountDomain;
namespace Core { class Settings; }

namespace Notifications {

/// Abstract base for platform notification managers.
class Manager : public QObject {
    Q_OBJECT

public:
    Manager() = default;
    ~Manager() override = default;

    /// Show a native notification for a new message (or a room invite when
    /// `isInvite` is set). `isMention` marks a highlight (mention/keyword) so
    /// platforms can raise its priority (e.g. macOS time-sensitive delivery).
    /// `avatarPath` is a local image file for the sender/inviter avatar, or empty
    /// (not yet cached / hidden). When `isInvite` is set, platforms omit the
    /// Reply / Mark-as-read actions — it is really a "no message actions apply"
    /// flag, so roomless security alerts (a new login) set it too, and pass an
    /// empty `roomId` (clicking one only focuses the app).
    virtual void showNotification(
        const QString &roomId,
        const QString &eventId,
        const QString &senderName,
        const QString &chatName,
        const QString &messageText,
        bool isDirect,
        bool isMention,
        const QString &avatarPath,
        bool isInvite) = 0;

    /// Remove all delivered notifications for the given room.
    virtual void clearFromRoom(const QString &roomId) = 0;

    /// Remove all delivered notifications.
    virtual void clearAll() = 0;

    /// Update the dock/taskbar badge with total unread count.
    virtual void updateDockBadge([[maybe_unused]] int totalUnread) {}

    /// Bounce the dock icon (macOS) to draw attention.
    virtual void bounceDockIcon() {}

Q_SIGNALS:
    /// Emitted when the user clicks a notification.
    void notificationActivated(const QString &roomId);
    /// Emitted when the user submits an inline reply from the toast.
    void notificationReplied(const QString &roomId, const QString &text);
    /// Emitted when the user triggers the "Mark as read" toast action.
    void notificationMarkRead(const QString &roomId);
};

/// Central notification scheduler and orchestrator.
class System : public QObject {
    Q_OBJECT

public:
    /// `domain` supplies the badge total across every signed-in account (they all
    /// keep syncing, and the OS gives us exactly one badge). It may be null in
    /// tests, in which case the badge falls back to `unreadStateStore` alone.
    explicit System(
        ProtocolBridge *bridge,
        AppMainWindow *window,
        AppMainWidget *mainWidget,
        Core::Settings *settings,
        UnreadStateStore *unreadStateStore,
        AccountDomain *domain = nullptr,
        QObject *parent = nullptr);
    ~System() override = default;

    /// Set the platform-specific manager. System takes ownership.
    void setManager(std::unique_ptr<Manager> manager);

    /// Start delivering one account's notifications.
    ///
    /// Called for every signed-in account, not just the visible one: they all
    /// keep syncing, so a message arriving on a background account must notify
    /// just as it would if that account were on screen. `dirName` identifies the
    /// account so a click can be routed back to it.
    void attachAccount(const QString &dirName, ProtocolBridge *bridge);
    /// Stop delivering an account's notifications (it signed out).
    void detachAccount(const QString &dirName);

    /// Clear notifications for a specific room (e.g., when user opens it).
    void clearFromRoom(const QString &roomId);

    /// Recalculate and update the dock badge from current room state.
    void refreshBadge();

Q_SIGNALS:
    /// Forwarded from Manager: user clicked a notification. `accountDirName` is
    /// the account the notification came from — it may not be the one on screen,
    /// so the handler has to switch to it before opening the room.
    void activateRoom(const QString &accountDirName, const QString &roomId);
    /// Forwarded from Manager: user submitted an inline reply. `accountDirName` is
    /// the account the toast came from — the reply must act on THAT account, which
    /// may not be the one on screen.
    void replyToRoom(const QString &accountDirName, const QString &roomId, const QString &text);
    /// Forwarded from Manager: user triggered "Mark as read" on a (possibly
    /// background) account's toast.
    void markReadRoom(const QString &accountDirName, const QString &roomId);

private:
    void onIncomingNotification(
        const QString &accountDirName,
        const QString &roomId,
        const QString &eventId,
        const QString &sender,
        const QString &senderAvatarUrl,
        const QString &roomDisplayName,
        const QString &body,
        bool isDirect,
        bool isMention,
        qint64 timestamp);
    void onIncomingInvite(
        const QString &accountDirName,
        const QString &roomId,
        const QString &inviterName,
        const QString &inviterAvatarUrl,
        const QString &roomDisplayName,
        bool isDirect);
    /// A new, unverified session appeared on the account. (The signal also carries
    /// a last-seen timestamp; the toast doesn't need it, the banner shows it.)
    void onNewLogin(
        const QString &deviceId,
        const QString &displayName,
        const QString &lastSeenIp);
    void updateDockBadge(int totalUnread);
    [[nodiscard]] int currentBadgeTotalUnread() const;
    void maybePlaySound();

    ProtocolBridge *_bridge = nullptr;
    AppMainWindow *_window = nullptr;
    AppMainWidget *_mainWidget = nullptr;
    Core::Settings *_settings = nullptr;
    UnreadStateStore *_unreadStateStore = nullptr;
    AccountDomain *_domain = nullptr;
    std::unique_ptr<Manager> _manager;

    /// Bridges we are listening to, by account directory name, so an account can
    /// be detached on sign-out without disturbing the others.
    QHash<QString, ProtocolBridge*> _accountBridges;

    /// Which account each room's notifications came from, so a click on a
    /// background account's toast can switch to that account first. Room ids are
    /// unique to an account in practice, so last-writer-wins is right here.
    QHash<QString, QString> _notificationAccount;

    /// Already-shown event ids for de-dup, keyed by account AND room: two
    /// accounts in the same room would otherwise suppress each other's copies.
    QHash<QString, QSet<QString>> _shownEventIds;

    /// Sound effect for notification chime.
    QSoundEffect _sound;
    qint64 _lastSoundTime = 0;

    /// Badge refresh coalescing: one deferred recompute per burst, and skip the OS
    /// write when the total is unchanged. See PERF-8. (-1 = never written yet.)
    bool _badgeRefreshQueued = false;
    int _lastBadgeTotal = -1;
};

} // namespace Notifications
} // namespace TeleMatrix

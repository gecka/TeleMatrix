// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "window/notifications_manager.h"

#include "app/account.h"
#include "app/account_domain.h"
#include "app/unread_state_store.h"
#include "app/app_main_window.h"
#include "app/app_main_widget.h"
#include "core/core_settings.h"
#include "protocol/media_cache.h"
#include "protocol/protocol_bridge.h"
#include "window/notification_platform.h"

#include <QDateTime>

namespace TeleMatrix::Notifications {

namespace {
/// Minimum ms between notification sounds.
constexpr qint64 kMinimalAlertDelay = 50;
/// Cap the per-room shown-event set so a long-lived chat can't grow it without
/// bound; clearFromRoom (on read) is the normal eviction.
constexpr int kMaxShownEventIdsPerRoom = 256;
} // namespace

System::System(
    ProtocolBridge *bridge,
    AppMainWindow *window,
    AppMainWidget *mainWidget,
    Core::Settings *settings,
    UnreadStateStore *unreadStateStore,
    AccountDomain *domain,
    QObject *parent)
    : QObject(parent)
    , _bridge(bridge)
    , _window(window)
    , _mainWidget(mainWidget)
    , _settings(settings)
    , _unreadStateStore(unreadStateStore)
    , _domain(domain)
{
    // Load notification sound from bundled resource.
    _sound.setSource(QUrl(QStringLiteral("qrc:/sounds/notification.wav")));
    _sound.setVolume(1.0);

    // Accounts are attached by the caller (one per signed-in account, including
    // the ones whose UI isn't showing), so nothing is wired to a bridge here.

    // Dock/taskbar unread badge is driven independently of notifications.
    if (_unreadStateStore) {
        QObject::connect(
            _unreadStateStore,
            &UnreadStateStore::totalUnreadChanged,
            this,
            [this](int /*totalUnreadIncludingMuted*/) {
                refreshBadge();
            });
    }
    QTimer::singleShot(0, this, &System::refreshBadge);
}

void System::setManager(std::unique_ptr<Manager> manager) {
    _manager = std::move(manager);
    if (_manager) {
        QObject::connect(_manager.get(), &Manager::notificationActivated,
                         this, [this](const QString &roomId) {
            // The toast may belong to an account that isn't on screen; say which,
            // so the click can switch to it instead of opening a room the visible
            // account doesn't have.
            emit activateRoom(_notificationAccount.value(roomId), roomId);
        });
        QObject::connect(_manager.get(), &Manager::notificationReplied,
                         this, [this](const QString &roomId, const QString &text) {
            emit replyToRoom(_notificationAccount.value(roomId), roomId, text);
        });
        QObject::connect(_manager.get(), &Manager::notificationMarkRead,
                         this, [this](const QString &roomId) {
            emit markReadRoom(_notificationAccount.value(roomId), roomId);
        });
        refreshBadge();
    }
}

void System::clearFromRoom(const QString &roomId) {
    // _shownEventIds is keyed "dirName|roomId", so a bare-roomId remove never matched
    // and the intended on-read dedup eviction was dead. Clear every account's entry
    // for this room. See code-review-2026-07-19 MA-7.
    const auto suffix = QLatin1Char('|') + roomId;
    for (auto it = _shownEventIds.begin(); it != _shownEventIds.end();) {
        if (it.key().endsWith(suffix)) {
            it = _shownEventIds.erase(it);
        } else {
            ++it;
        }
    }
    if (_manager) {
        _manager->clearFromRoom(roomId);
    }
}

void System::refreshBadge() {
    // Coalesce: a burst of per-account unread deltas would otherwise each recompute the
    // all-account total and write the OS badge. Fold them into one deferred refresh, and
    // skip the OS write when the total hasn't changed. See code-review-2026-07-19 PERF-8.
    if (_badgeRefreshQueued) {
        return;
    }
    _badgeRefreshQueued = true;
    QTimer::singleShot(0, this, [this] {
        _badgeRefreshQueued = false;
        const int total = currentBadgeTotalUnread();
        if (total == _lastBadgeTotal) {
            return;
        }
        _lastBadgeTotal = total;
        updateDockBadge(total);
    });
}

void System::attachAccount(const QString &dirName, ProtocolBridge *bridge) {
    if (dirName.isEmpty() || !bridge || _accountBridges.contains(dirName)) {
        return;
    }
    _accountBridges.insert(dirName, bridge);

    // The backend already decided "genuinely-new unread incoming message that
    // should notify"; the handlers only add the UI-only gates (focused room,
    // desktop-notify setting). The account is bound here so those gates — and
    // the click routing — know which account the event belongs to.
    QObject::connect(bridge, &ProtocolBridge::incomingNotification, this,
        [this, dirName](
                const QString &roomId,
                const QString &eventId,
                const QString &sender,
                const QString &senderAvatarUrl,
                const QString &roomDisplayName,
                const QString &body,
                bool isDirect,
                bool isMention,
                qint64 timestamp) {
            onIncomingNotification(
                dirName, roomId, eventId, sender, senderAvatarUrl, roomDisplayName,
                body, isDirect, isMention, timestamp);
        });
    QObject::connect(bridge, &ProtocolBridge::incomingInvite, this,
        [this, dirName](
                const QString &roomId,
                const QString &inviterName,
                const QString &inviterAvatarUrl,
                const QString &roomDisplayName,
                bool isDirect) {
            onIncomingInvite(
                dirName, roomId, inviterName, inviterAvatarUrl, roomDisplayName,
                isDirect);
        });
    QObject::connect(bridge, &ProtocolBridge::newLoginReceived,
                     this, &System::onNewLogin);
}

void System::detachAccount(const QString &dirName) {
    const auto bridge = _accountBridges.take(dirName);
    if (bridge) {
        bridge->disconnect(this);
    }
    // Drop what only made sense while that account existed, so a directory name
    // reused by a later account starts clean.
    for (auto it = _notificationAccount.begin(); it != _notificationAccount.end();) {
        if (it.value() == dirName) {
            it = _notificationAccount.erase(it);
        } else {
            ++it;
        }
    }
    const auto prefix = dirName + QLatin1Char('|');
    for (auto it = _shownEventIds.begin(); it != _shownEventIds.end();) {
        if (it.key().startsWith(prefix)) {
            it = _shownEventIds.erase(it);
        } else {
            ++it;
        }
    }
}

void System::onIncomingNotification(
    const QString &accountDirName,
    const QString &roomId,
    const QString &eventId,
    const QString &sender,
    const QString &senderAvatarUrl,
    const QString &roomDisplayName,
    const QString &body,
    bool isDirect,
    bool isMention,
    qint64 timestamp)
{
    Q_UNUSED(timestamp);

    // De-dup: ignore an event we've already shown for this room (e.g. a retry,
    // or the same event re-delivered across a resubscribe). Keyed by account too,
    // so two accounts sharing a room don't swallow each other's copies.
    if (!eventId.isEmpty()) {
        auto &shown = _shownEventIds[accountDirName + QLatin1Char('|') + roomId];
        if (shown.contains(eventId)) {
            return;
        }
        if (shown.size() >= kMaxShownEventIdsPerRoom) {
            shown.clear();
        }
        shown.insert(eventId);
    }

    // UI-only suppression: app focused on this exact room (you're already reading
    // it). Notifications for other rooms still fire while the app is focused —
    // and a background account's room is never the one being read, so this only
    // applies to the account currently on screen.
    const auto activeDirName = (_domain && _domain->active())
        ? _domain->active()->dirName() : QString();
    if (accountDirName == activeDirName
        && _window && _window->isWindowActive()
        && _mainWidget && _mainWidget->activeRoomId() == roomId) {
        return;
    }
    _notificationAccount.insert(roomId, accountDirName);

    if (_manager && (!_settings || _settings->desktopNotify())) {
        const bool showSender = (!_settings || _settings->showSenderName());
        const QString senderName = showSender ? sender : QString();
        const QString chatName = isDirect ? QString() : roomDisplayName;
        const QString messageText = (!_settings || _settings->showMessagePreview())
            ? body : QString(u"You have a new message");

        // Sender avatar (same privacy class as the name, so gated on showSender):
        // resolve the mxc to a local image file via MediaCache. If it isn't cached
        // yet, warm it for next time and show no avatar this time.
        QString avatarPath;
        if (showSender && !senderAvatarUrl.isEmpty()) {
            avatarPath = MediaCache::localPath(senderAvatarUrl);
            // Resolved through the account that saw the event: media is fetched
            // with that account's credentials (and its keys, when encrypted).
            const auto owner = _accountBridges.value(accountDirName, _bridge);
            if (avatarPath.isEmpty() && owner
                && MediaCache::needsResolution(senderAvatarUrl)) {
                MediaCache::markRequested(senderAvatarUrl);
                owner->resolveAvatar(senderAvatarUrl);
            }
        }

        _manager->showNotification(
            roomId, eventId, senderName, chatName, messageText, isDirect,
            isMention, avatarPath, /*isInvite*/ false);
    }

    // Our sound/flash bypass the OS notification system, so (unlike the toast)
    // they aren't auto-suppressed under DND / screen-lock / fullscreen — gate
    // them on an explicit platform check.
    if (!Platform::shouldSuppressAlerts()) {
        maybePlaySound();
        if (_manager && _settings && _settings->bounceDockIcon()) {
            _manager->bounceDockIcon();
        }
    }
}

void System::onNewLogin(
    const QString &deviceId,
    const QString &displayName,
    const QString &lastSeenIp)
{
    // Rust alerts once per newly-appeared session, so no dedup here. Unlike a
    // message there is no room to be focused on, so nothing suppresses it.
    if (_manager && (!_settings || _settings->desktopNotify())) {
        auto detail = displayName.trimmed().isEmpty()
            ? deviceId
            : displayName.trimmed();
        if (!lastSeenIp.trimmed().isEmpty()) {
            detail += QStringLiteral(" · ") + lastSeenIp.trimmed();
        }
        // isInvite=true == "no message actions": Reply / Mark-as-read make no
        // sense for a security alert. Empty roomId -> clicking only focuses the
        // app, where the banner carries the actions.
        _manager->showNotification(
            /*roomId*/ QString(), /*eventId*/ QString(),
            tr("New login. Was this you?"), /*chatName*/ QString(), detail,
            /*isDirect*/ false, /*isMention*/ false,
            /*avatarPath*/ QString(), /*isInvite*/ true);
    }

    if (!Platform::shouldSuppressAlerts()) {
        maybePlaySound();
        if (_manager && _settings && _settings->bounceDockIcon()) {
            _manager->bounceDockIcon();
        }
    }
}

void System::onIncomingInvite(
    const QString &accountDirName,
    const QString &roomId,
    const QString &inviterName,
    const QString &inviterAvatarUrl,
    const QString &roomDisplayName,
    bool isDirect)
{
    // Rust already dedups invites per room (and re-arms after decline), so no
    // event-id dedup here. UI-only suppression: app focused on this exact room
    // means the invite prompt is already on screen — which can only be true for
    // the account currently showing.
    const auto activeDirName = (_domain && _domain->active())
        ? _domain->active()->dirName() : QString();
    if (accountDirName == activeDirName
        && _window && _window->isWindowActive()
        && _mainWidget && _mainWidget->activeRoomId() == roomId) {
        return;
    }
    _notificationAccount.insert(roomId, accountDirName);

    if (_manager && (!_settings || _settings->desktopNotify())) {
        // Respect "show sender name", but not "show message preview": an invite
        // isn't message content, so it always reads clearly.
        const bool showSender = (!_settings || _settings->showSenderName());
        const QString senderName = showSender ? inviterName : QString();
        const QString chatName = isDirect ? QString() : roomDisplayName;
        const QString messageText = isDirect
            ? tr("invited you to chat")
            : tr("invited you to join");

        QString avatarPath;
        if (showSender && !inviterAvatarUrl.isEmpty()) {
            avatarPath = MediaCache::localPath(inviterAvatarUrl);
            // Resolve via the invited account's own bridge, not the primary one
            // (they can differ, or the primary can be signed out). See R2-6.
            auto *owner = _accountBridges.value(accountDirName, _bridge);
            if (avatarPath.isEmpty() && owner
                && MediaCache::needsResolution(inviterAvatarUrl)) {
                MediaCache::markRequested(inviterAvatarUrl);
                owner->resolveAvatar(inviterAvatarUrl);
            }
        }

        _manager->showNotification(
            roomId, /*eventId*/ QString(), senderName, chatName, messageText,
            isDirect, /*isMention*/ false, avatarPath, /*isInvite*/ true);
    }

    if (!Platform::shouldSuppressAlerts()) {
        maybePlaySound();
        if (_manager && _settings && _settings->bounceDockIcon()) {
            _manager->bounceDockIcon();
        }
    }
}

void System::updateDockBadge(int totalUnread) {
    if (_manager) {
        _manager->updateDockBadge(totalUnread);
    }
}

int System::currentBadgeTotalUnread() const {
    const auto includeMuted = !_settings || _settings->includeMutedInBadge();
    // Every signed-in account, not just the one on screen: background accounts
    // keep syncing, so their unread has to show on the one badge the OS gives us.
    if (_domain && _domain->count() > 0) {
        return _domain->totalUnreadBadge(includeMuted);
    }
    if (_unreadStateStore) {
        return _unreadStateStore->totalUnreadCount(includeMuted);
    }

    const auto rooms = _bridge ? _bridge->cachedRooms() : QVector<RoomSummary>();
    int totalUnread = 0;
    for (const auto &room : rooms) {
        const bool muted = roomCountsAsMuted(room.notificationMode, room.isMuted);
        if (!muted || !_settings || _settings->includeMutedInBadge()) {
            totalUnread += qMax(0, room.unreadCount);
        }
    }
    return totalUnread;
}

void System::maybePlaySound() {
    if (_settings && !_settings->soundNotify()) {
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - _lastSoundTime < kMinimalAlertDelay) {
        return;
    }
    _lastSoundTime = now;
    if (_sound.status() == QSoundEffect::Ready) {
        _sound.play();
    }
}

} // namespace TeleMatrix::Notifications

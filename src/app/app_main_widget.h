// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QHash>
#include <QVector>
#include <QWidget>

#include "../protocol/protocol_types.h"

class QSplitter;
class QTimer;

namespace Ui {
class ConnectingWidget;
} // namespace Ui

namespace TeleMatrix {
class NetworkMonitor;
} // namespace TeleMatrix

namespace TeleMatrix {

class AppController;
class ProtocolBridge;
class DialogsWidget;
class DialogsMainMenuOverlay;
class HistoryWidget;
class MediaViewOverlay;
class RoomSettingsWidget;
class SettingsWidget;
class ThemeSelectorPanel;
class UnreadStateStore;
class UserProfilePopup;
class LayerStackWidget;

/// Two-panel layout: chat list sidebar (left) + message timeline (right).
class AppMainWidget : public QWidget {
    Q_OBJECT

public:
    explicit AppMainWidget(
        AppController *controller,
        ProtocolBridge *bridge,
        QWidget *parent = nullptr);

    /// Load and display a room in the history panel.
    void showRoom(const QString &roomId);
    /// Open `roomId` on its live timeline and highlight `eventId` there — the
    /// notification-click path. Falls back to showRoom() when the id is empty.
    void showRoomAtEvent(const QString &roomId, const QString &eventId);

    /// Show a room we are not a member of: its name and description over an empty timeline, with a
    /// Join bar where the composer would be. Matrix serves no history to a non-member, so there is
    /// nothing else to show. `via` carries server hints needed to reach a room from another server.
    void showRoomPreview(const QString &roomId, const QStringList &via = QStringList());

    /// Apply cached rooms from previous session for instant display.
    void applyCachedRooms(const QVector<RoomSummary> &rooms);

    /// The room currently displayed in the history panel.
    [[nodiscard]] QString activeRoomId() const { return _activeRoomId; }

    /// Show the settings panel as a layer overlay.
    void showSettings();

    /// Open Settings → Active sessions. When `signOutDeviceId` is set, also start
    /// that session's sign-out flow (from the "New login" banner).
    void showSessions(const QString &signOutDeviceId = QString());

    /// Open the theme picker side panel, closing the settings layer behind it.
    void showThemeSelector();

    /// Show the room settings panel as a layer overlay.
    void showRoomSettings(const QString &roomId);

    /// Open the "Explore rooms" box (main menu entry / Ctrl+K).
    // Opens the room-directory box. When spaceId is non-empty, the box opens
    // pre-entered into that joined space's children.
    void openExploreRooms(const QString &spaceId = {}, const QString &name = {});

    /// Open Saved Messages, creating the room on first use.
    void openSavedMessages();

    /// Ctrl+F: search in the open room, or the chat list when none is open.
    void focusSearch();

    /// Lift the floating connection pill above whatever the rooms list has
    /// pinned to its bottom edge (currently only the update bar).
    void setConnectingBottomSkip(int skip);

Q_SIGNALS:
    void activeRoomChanged(const QString &roomId);
    void logoutRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *e) override;

    /// Show a user profile popup via the layer stack.
    void showUserProfilePopup(
        const QString &roomId, const QString &userId, const QString &displayName);

private:
    /// Point the active-room bookkeeping (unread store, activeRoomChanged, chat
    /// list highlight) at `roomId`. No-op when it is already active, so it is
    /// safe to call after a path that has already switched rooms itself.
    void setActiveRoom(const QString &roomId);
    void setupLayout();
    void applySyncState(int state);
    void setNetworkOnline(bool online);
    void applyConnectionState();
    void restoreDialogsWidth();
    void saveDialogsWidth();
    void openVerifySessionDialog();
    void openIncomingVerifySessionDialog(const QString &transactionId);
    void openIncomingUserVerifyDialog(const QString &flowId, const QString &displayName);
    void scheduleUnreadRoomListRefresh();
    void scheduleUnreadTimelineRefresh(const QString &roomId);
    void refreshUnreadRoomListSnapshot();
    void refreshUnreadRoomSnapshot(const QString &roomId);
    void refreshUnreadTimelineSnapshot();
    void clearActiveRoomIfMissing(const QVector<RoomSummary> &rooms);
    void handleApplicationStateChanged(Qt::ApplicationState state);
    void showRoomSettingsForMembers(const QString &roomId);
    void showRoomSettingsInternal(const QString &roomId, bool showMembersSection);
    void ensureSettingsWidget();
    void showUserProfilePopupOverLayer(const QString &roomId, const QString &userId);
    void closeUserProfileOverlay();
    void positionUserProfileOverlay();

    AppController *_controller = nullptr;
    ProtocolBridge *_bridge = nullptr;
    UnreadStateStore *_unreadStateStore = nullptr;
    QTimer *_unreadRoomListRefreshTimer = nullptr;
    QTimer *_unreadTimelineRefreshTimer = nullptr;
    QSplitter *_splitter = nullptr;
    ::Ui::ConnectingWidget *_connecting = nullptr;
    ::TeleMatrix::NetworkMonitor *_networkMonitor = nullptr;
    bool _networkOnline = true;
    int _syncState = 0;
    DialogsWidget *_dialogs = nullptr;
    HistoryWidget *_history = nullptr;
    MediaViewOverlay *_mediaView = nullptr;
    QString _activeRoomId;
    QString _pendingUnreadTimelineRoomId;
    DialogsMainMenuOverlay *_mainMenuOverlay = nullptr;
    LayerStackWidget *_layerStack = nullptr;
    SettingsWidget *_settingsWidget = nullptr;
    ThemeSelectorPanel *_themePanel = nullptr;
    bool _reopenSettingsAfterTheme = false;
    RoomSettingsWidget *_roomSettingsWidget = nullptr;
    QWidget *_userProfileOverlay = nullptr;
    UserProfilePopup *_userProfilePopup = nullptr;
    bool _dialogsWidthRestored = false;
    bool _appActive = true;
    // True once sync state has reached 2 (synced) at least once. Gates the
    // "waiting for network" banner so it never shows during the first sync.
    bool _everSynced = false;
    bool _unreadRoomListRefreshPending = false;
    bool _unreadTimelineRefreshPending = false;
    quint64 _latestUnreadRoomListRequestId = 0;
    QHash<QString, quint64> _latestUnreadSnapshotRequestIds;
    quint64 _latestUnreadTimelineRequestId = 0;
    quint64 _latestRoomLeftCheckRequestId = 0;
};

} // namespace TeleMatrix

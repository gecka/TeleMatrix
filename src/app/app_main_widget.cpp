// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "app_main_widget.h"
#include "app_main_window.h"
#include "app_controller.h"
#include "dialogs_width.h"
#include "unread_state_store.h"
#include "theme/theme_manager.h"

#include <QApplication>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QSplitter>
#include <QSplitterHandle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "../dialogs/dialogs_main_menu_overlay.h"
#include "../dialogs/dialogs_invite_users_box.h"
#include "../dialogs/dialogs_explore_rooms_box.h"
#include "../dialogs/dialogs_new_chat_box.h"
#include "../dialogs/dialogs_room_create_dialog.h"
#include "../dialogs/dialogs_widget.h"
#include "../history/history_widget.h"
#include "../media/media_view_overlay.h"
#include "../protocol/media_cache.h"
#include "../protocol/protocol_bridge.h"
#include "../history/room_settings_widget.h"
#include "../history/user_profile_popup.h"
#include "../settings/appearance/theme_selector_panel.h"
#include "../settings/dialogs/verify_session_popup.h"
#include "../settings/dialogs/verify_user_dialog.h"
#include "../settings/settings_widget.h"
#include "../styles/style_constants.h"
#include "../ui/layers/layer_stack_widget.h"
#include "../ui/widgets/input_fields.h"
#include "../ui/widgets/connecting_widget.h"
#include "network_monitor.h"

#include <algorithm>

namespace TeleMatrix {

namespace {
// Pane width, so it carries the folders rail too: 300 of room list + 72 of rail.
// Left at 300 it would fall below the pane minimum and get clamped, making the
// default and the minimum the same width.
#define kSidebarDefaultWidth (300 + st::sideBarWidth)
constexpr int kContentMinWidth = 300;
constexpr int kUnreadRefreshDelay = 150;
constexpr int kUnreadActivationRefreshDelay = 750;
// Runtime-scaled via st::initPxValues().
//
// The folders rail lives INSIDE DialogsWidget, so its 72px comes out of whatever
// width the pane gets. columnMinimalWidthLeft is tdesktop's minimum for the room
// list itself, which there is a column of its own — so the pane minimum has to be
// the sum, or the list is squeezed to 188 while the pane still measures 260.
#define kSidebarMinWidth (st::columnMinimalWidthLeft + st::sideBarWidth)
#define kSidebarMaxWidth (st::columnMaximalWidthLeft + st::sideBarWidth)

QString serverNameFromSession(const ProtocolBridge::SessionInfo &session) {
    const auto userId = session.userId.trimmed();
    const auto colon = userId.lastIndexOf(QChar(':'));
    if (colon >= 0 && colon + 1 < userId.size()) {
        return userId.mid(colon + 1);
    }

    const auto homeserver = session.homeserver.trimmed();
    const auto url = QUrl(homeserver);
    if (!url.host().isEmpty()) {
        return url.host();
    }
    auto fallback = homeserver;
    if (fallback.startsWith(QStringLiteral("https://"))) {
        fallback = fallback.mid(8);
    } else if (fallback.startsWith(QStringLiteral("http://"))) {
        fallback = fallback.mid(7);
    }
    const auto slash = fallback.indexOf(QChar('/'));
    if (slash >= 0) {
        fallback = fallback.left(slash);
    }
    return fallback;
}

void paintLayerBoxShadow(QPainter &p, const QRect &boxRect) {
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    const auto extend = qMax(1, st::layerShadowExtend);
    for (int i = extend; i >= 1; --i) {
        const auto progress = qreal(extend - i) / extend;
        const auto alpha = int(18.0 * progress * progress);
        p.setBrush(st::withAlpha(st::windowShadowFg, alpha));
        const auto radius = st::boxRadius + i;
        p.drawRoundedRect(boxRect.adjusted(-i, -i, i, i), radius, radius);
    }
}

class UserProfileLayerOverlay : public QWidget {
public:
    explicit UserProfileLayerOverlay(QWidget *parent = nullptr)
        : QWidget(parent) {
        setAttribute(Qt::WA_TranslucentBackground);
        setFocusPolicy(Qt::StrongFocus);
    }

    void setPopup(QWidget *popup) {
        _popup = popup;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        if (!_popup) {
            return;
        }
        QPainter p(this);
        paintLayerBoxShadow(p, _popup->geometry());
    }

private:
    QPointer<QWidget> _popup;
};

// QSplitter whose handle is painted with the live st:: handle color instead
// of a frozen "QSplitter::handle { background: ... }" stylesheet. Because the
// handle reads st::splitterHandleBg in paintEvent, it tracks theme changes
// automatically (a plain child->update() repaints it correctly) -- which only
// holds because splitterHandleBg is a real palette token; it was a hardcoded
// 9%-black constant, invisible against every night theme.
class ThemedSplitterHandle : public QSplitterHandle {
public:
    ThemedSplitterHandle(Qt::Orientation orientation, QSplitter *parent)
        : QSplitterHandle(orientation, parent) {
        // Erased explicitly in paintEvent, so Qt need not clear first.
        setAttribute(Qt::WA_OpaquePaintEvent, true);
    }

    /// Paint `color` instead of the seam for the bottom `height` pixels.
    ///
    /// The rooms list's bottom bar (the update button) is a child of the dialogs
    /// widget, so it is clipped to it and cannot cover this handle — a sibling
    /// one pixel to its right. The seam therefore drew a light line down the edge
    /// of an otherwise full-bleed bar. Continuing the bar's own colour across the
    /// handle closes it. The handle keeps its full height, so dragging still
    /// works over the bar.
    ///
    /// A colour rather than a skip because WA_OpaquePaintEvent means every pixel
    /// must be painted every frame; leaving a gap would show stale content.
    void setBottomCover(int height, QColor color) {
        if (_coverHeight == height && _coverColor == color) {
            return;
        }
        _coverHeight = height;
        _coverColor = color;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        // Erase before tinting. splitterHandleBg is semi-transparent, and Qt
        // only auto-clears a child widget when autoFillBackground is set — so
        // without this the fill composites onto the PREVIOUS frame and the seam
        // ratchets darker with every repaint (a theme switch made it obvious;
        // a restart "fixed" it by resetting the backing store). Compositing it
        // over windowBg exactly once also matches what the contrast floors in
        // tools/theme/colorize.py and tst_theme_registry measure.
        const auto seam = (_coverHeight > 0)
            ? rect().adjusted(0, 0, 0, -_coverHeight)
            : rect();
        if (!seam.isEmpty()) {
            p.fillRect(seam, st::windowBg);
            p.fillRect(seam, st::splitterHandleBg);
        }
        if (_coverHeight > 0) {
            p.fillRect(
                QRect(0, height() - _coverHeight, width(), _coverHeight),
                _coverColor);
        }
    }

private:
    int _coverHeight = 0;
    QColor _coverColor;
};

class ThemedSplitter : public QSplitter {
public:
    ThemedSplitter(Qt::Orientation orientation, QWidget *parent)
        : QSplitter(orientation, parent) {}

    /// Forwarded to every handle. handle(0) is the hidden zero-width one Qt keeps
    /// before the first pane; covering it too is harmless. Uses the protected
    /// handle() rather than findChildren, which would need a Q_OBJECT this
    /// anonymous-namespace class cannot carry, and always returns live pointers.
    void setHandleBottomCover(int height, QColor color) {
        for (auto i = 0; i != count(); ++i) {
            if (auto *h = static_cast<ThemedSplitterHandle *>(handle(i))) {
                h->setBottomCover(height, color);
            }
        }
    }

protected:
    QSplitterHandle *createHandle() override {
        return new ThemedSplitterHandle(orientation(), this);
    }
};
} // namespace

AppMainWidget::AppMainWidget(
    AppController *controller,
    ProtocolBridge *bridge,
    QWidget *parent)
    : QWidget(parent)
    , _controller(controller)
    , _bridge(bridge)
    , _unreadStateStore(controller ? controller->unreadStateStore() : nullptr)
{
    setupLayout();
    qApp->installEventFilter(this);
}

void AppMainWidget::setupLayout() {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    _splitter = new ThemedSplitter(Qt::Horizontal, this);
    _splitter->setHandleWidth(1);
    // Without this a drag past the minimum collapses the pane to zero width,
    // which bypasses setMinimumWidth() and hides the rooms list outright. Off, the
    // drag stops at kSidebarMinWidth instead.
    _splitter->setChildrenCollapsible(false);

    // Left panel: chat list.
    _dialogs = new DialogsWidget(_splitter, _controller, _bridge);
    _dialogs->setMinimumWidth(kSidebarMinWidth);
    _dialogs->setMaximumWidth(kSidebarMaxWidth);
    connect(_dialogs, &DialogsWidget::verificationRequestAccepted,
        this, &AppMainWidget::openIncomingVerifySessionDialog);
    connect(_dialogs, &DialogsWidget::userVerificationRequestAccepted,
        this, &AppMainWidget::openIncomingUserVerifyDialog);
    connect(_dialogs, &DialogsWidget::signOutDeviceRequested,
        this, [this](const QString &deviceId) { showSessions(deviceId); });
    connect(_dialogs, &DialogsWidget::exploreSpaceRequested,
        this, [this](const QString &spaceId, const QString &name) {
            openExploreRooms(spaceId, name);
        });
    connect(_dialogs, &DialogsWidget::openRoomSettingsRequested,
        this, &AppMainWidget::showRoomSettings);

    // Right panel: message timeline.
    _history = new HistoryWidget(_controller, _splitter, _bridge);
    _history->setMinimumWidth(kContentMinWidth);

    _splitter->addWidget(_dialogs);
    _splitter->addWidget(_history);
    // Use fixed positive values — at construction time width() is 0,
    // so we can't compute from it. Qt uses these as proportional hints.
    _splitter->setSizes({ kSidebarDefaultWidth, 500 });
    _splitter->setStretchFactor(0, 0); // Sidebar doesn't stretch.
    _splitter->setStretchFactor(1, 1); // Content fills remaining space.

    // Save the dialogs width when the splitter handle is released.
    connect(_splitter, &QSplitter::splitterMoved,
            this, &AppMainWidget::saveDialogsWidth);

    layout->addWidget(_splitter);

    // Repaint all surfaces when theme changes.
    if (auto *tm = _controller->themeManager()) {
        connect(tm, &Theme::ThemeManager::themeChanged,
                this, [this](bool /*isNight*/, Theme::ThemeMode /*mode*/) {
            // Build the fresh application palette from current st:: tokens.
            QPalette appPal;
            appPal.setColor(QPalette::Window, st::windowBg);
            appPal.setColor(QPalette::WindowText, st::windowFg);
            appPal.setColor(QPalette::Base, st::windowBg);
            appPal.setColor(QPalette::AlternateBase, st::windowBgOver);
            appPal.setColor(QPalette::Text, st::windowFg);
            appPal.setColor(QPalette::Button, st::windowBgOver);
            appPal.setColor(QPalette::ButtonText, st::windowFg);
            appPal.setColor(QPalette::Highlight, st::windowBgActive);
            appPal.setColor(QPalette::HighlightedText, st::windowFgActive);
            appPal.setColor(QPalette::PlaceholderText, st::placeholderFg);

            // Re-apply to self and splitter.
            setPalette(appPal);
            _splitter->setPalette(appPal);

            // The splitter handle paints itself from live st:: colors
            // (ThemedSplitterHandle), so the child->update() loop below
            // repaints it for the current theme — no stylesheet to re-apply.

            // Recurse through all child widgets: re-apply palette and repaint.
            // Widgets with autoFillBackground get their Window color refreshed
            // so container backgrounds match the current theme.
            const auto allChildren = findChildren<QWidget *>();
            for (auto *child : allChildren) {
                if (child->autoFillBackground()) {
                    QPalette pal = appPal;
                    pal.setColor(QPalette::Window, st::windowBg);
                    child->setPalette(pal);
                }
                child->update();
            }

            // Re-read cached InputFieldStyle colors for all input fields.
            // Use borderRadius to distinguish round filter fields (dialogsFilter)
            // from flat fields (defaultInputField) — cancelVisible() is unreliable
            // because the search field cancel button only shows when text is present.
            const auto inputFields = findChildren<::Ui::InputField *>();
            for (auto *field : inputFields) {
                field->refreshStyle(field->currentStyle().borderRadius > 0
                    ? st::dialogsFilter
                    : st::defaultInputField);
            }
        });
    }

    // Wire up room selection: clicking a room loads its timeline.
    _dialogs->setRoomSelectedCallback([this](const QString &roomId) {
        showRoom(roomId);
    });

    QObject::connect(
        _history,
        &HistoryWidget::draftChanged,
        _dialogs,
        [this](const QString &roomId, const QString &text) {
            _dialogs->setRoomDraft(roomId, text);
        });

    QObject::connect(
        _history,
        &HistoryWidget::searchInChatRequested,
        _dialogs,
        [this](const QString &roomId, const QString &roomName, bool isDirect) {
            _dialogs->focusSearchInChat(roomId, roomName, isDirect);
        });

    QObject::connect(
        _dialogs,
        &DialogsWidget::searchInChatClosed,
        _history,
        &HistoryWidget::clearSearchActive);

    QObject::connect(
        _history,
        &HistoryWidget::openMediaViewRequested,
        this,
        [this](const QVector<TimelineItem> &items, int index) {
            if (items.isEmpty() || index < 0 || index >= items.size()) {
                return;
            }
            auto *host = window();
            if (!_mediaView) {
                _mediaView = new MediaViewOverlay(host ? host : this);
                _mediaView->setResolveMediaCallback([this](const QString &mxcUrl, bool preferBytes) {
                    if (!_bridge || !mxcUrl.startsWith(QStringLiteral("mxc://"))) {
                        return;
                    }
                    if (!MediaCache::needsResolution(mxcUrl)) {
                        return;
                    }
                    MediaCache::markRequested(mxcUrl);
                    if (preferBytes) {
                        _bridge->resolveMediaBytes(mxcUrl);
                    } else {
                        _bridge->resolveMedia(mxcUrl);
                    }
                });
                _mediaView->setExportMediaCallback([this](const QString &mxcUrl, const QString &targetPath) {
                    if (_bridge && !mxcUrl.isEmpty() && !targetPath.isEmpty()) {
                        _bridge->exportMediaToPath(mxcUrl, targetPath);
                    }
                });
                _mediaView->setVideoStreamUrlCallback([this](const QString &mxcUrl) -> QString {
                    return _bridge ? _bridge->videoStreamUrl(mxcUrl) : QString();
                });
                _mediaView->setVideoStreamProgressCallback([this](const QString &mxcUrl) -> float {
                    return _bridge ? _bridge->videoStreamProgress(mxcUrl) : 1.0f;
                });
                _mediaView->setVideoStreamProgressBytesCallback(
                    [this](const QString &mxcUrl, quint64 &d, quint64 &t) {
                        return _bridge && _bridge->videoStreamProgressBytes(mxcUrl, d, t);
                    });
                _mediaView->setVideoStreamErroredCallback([this](const QString &mxcUrl) {
                    return _bridge && _bridge->videoStreamErrored(mxcUrl);
                });
                _mediaView->setVideoStreamContainerCallback(
                    [this](const QString &mxcUrl) {
                        return _bridge ? _bridge->videoStreamContainer(mxcUrl)
                                       : VideoContainer::Unknown;
                    });
                _mediaView->setSavedMediaDirProvider([this]() -> QString {
                    return _controller ? _controller->settings().mediaSaveDir() : QString();
                });
                _mediaView->setRememberSaveDir([this](const QString &dir) {
                    if (!_controller || dir.isEmpty()
                        || _controller->settings().mediaSaveDir() == dir) {
                        return;
                    }
                    _controller->settings().setMediaSaveDir(dir);
                    _controller->saveSettingsDelayed();
                });
                // Returning from fullscreen: re-sync an inline player that handed
                // off (kept paused) to the final position so it shows a paused frame
                // with controls instead of reverting to the poster.
                QObject::connect(_mediaView, &MediaViewOverlay::videoClosed, this,
                    [this](const QString &mxcUrl, qint64 positionMs) {
                        if (_history) {
                            _history->onFullscreenVideoClosed(mxcUrl, positionMs);
                        }
                    });
            }
            _mediaView->showMediaWithContext(items, index);
        });

    QObject::connect(
        _bridge,
        &ProtocolBridge::mediaResolved,
        this,
        [this](bool success, const QString &mxcUrl, const QString & /*localPath*/) {
            if (!success) {
                // mxcUrl is the actual cache key, which may be a derived key
                // ("previewthumb:"/"srvthumb:"/"vidthumb:") that does not start
                // with "mxc://". Release it on failure so it can be re-requested
                // instead of sticking in the requested set (loading) until restart.
                MediaCache::clearRequested(mxcUrl);
            }
            if (!success || !_mediaView) {
                return;
            }
            _mediaView->mediaResolved(mxcUrl);
        });

    QObject::connect(
        _bridge,
        &ProtocolBridge::mediaBytesResolved,
        this,
        [this](bool success,
               const QString &mxcUrl,
               const QByteArray &bytes,
               const QString &mime,
               const QString & /*filename*/) {
            if (!success) {
                // mxcUrl is the actual cache key, which may be a derived key
                // ("previewthumb:"/"srvthumb:"/"vidthumb:") that does not start
                // with "mxc://". Release it on failure so it can be re-requested
                // instead of sticking in the requested set (loading) until restart.
                MediaCache::clearRequested(mxcUrl);
            }
            if (!success || !_mediaView) {
                return;
            }
            if (!MediaCache::insertImageBytes(mxcUrl, bytes, mime)) {
                MediaCache::clearRequested(mxcUrl);
                if (MediaCache::needsResolution(mxcUrl)) {
                    MediaCache::markRequested(mxcUrl);
                    _bridge->resolveMedia(mxcUrl);
                }
                return;
            }
            _mediaView->mediaResolved(mxcUrl);
        });

    // Escape/back closes the open room and clears the section stack,
    // returning focus to the chat list with no active room.
    QObject::connect(
        _history,
        &HistoryWidget::cancelRequests,
        this,
        [this] {
            _history->closeRoom();
            _dialogs->clearSelection();
            _dialogs->setFocus();
            if (!_activeRoomId.isEmpty()) {
                _activeRoomId.clear();
                if (_unreadStateStore) {
                    _unreadStateStore->setActiveRoomId(QString());
                }
                emit activeRoomChanged(QString());
            }
        });

    // Defer window-active connection until the widget is parented
    // to AppMainWindow (window() may be nullptr at construction).
    QTimer::singleShot(0, this, [this] {
        auto *mainWindow = qobject_cast<AppMainWindow *>(window());
        if (mainWindow) {
            QObject::connect(mainWindow, &AppMainWindow::windowActiveChanged,
                _history, &HistoryWidget::setWindowActive);
            // Sync initial state (window may already be active at startup).
            _history->setWindowActive(mainWindow->isWindowActive());
        }
    });

    // Navigate to a specific message when a search result is clicked.
    QObject::connect(
        _dialogs,
        &DialogsWidget::searchResultClicked,
        this,
        [this](const QString &roomId, const QString &eventId) {
            _history->showMessage(roomId, eventId);
            // Also update active room if switching.
            if (roomId != _activeRoomId) {
                _activeRoomId = roomId;
                if (_unreadStateStore) {
                    _unreadStateStore->setActiveRoomId(roomId);
                }
                emit activeRoomChanged(roomId);
            }
        });

    // Fallback path only when the shared unread store is unavailable.
    if (!_unreadStateStore) {
        QObject::connect(_history, &HistoryWidget::unreadCountChanged,
            _dialogs, &DialogsWidget::setRoomUnreadCount);
    }

    if (_unreadStateStore && _bridge) {
        _appActive = (QGuiApplication::applicationState() == Qt::ApplicationActive);
        _unreadRoomListRefreshTimer = new QTimer(this);
        _unreadRoomListRefreshTimer->setSingleShot(true);
        _unreadRoomListRefreshTimer->setInterval(kUnreadRefreshDelay);
        QObject::connect(_unreadRoomListRefreshTimer, &QTimer::timeout,
            this, &AppMainWidget::refreshUnreadRoomListSnapshot);

        _unreadTimelineRefreshTimer = new QTimer(this);
        _unreadTimelineRefreshTimer->setSingleShot(true);
        _unreadTimelineRefreshTimer->setInterval(kUnreadRefreshDelay);
        QObject::connect(_unreadTimelineRefreshTimer, &QTimer::timeout,
            this, &AppMainWidget::refreshUnreadTimelineSnapshot);

        QObject::connect(
            qApp,
            &QGuiApplication::applicationStateChanged,
            this,
            &AppMainWidget::handleApplicationStateChanged);
        QObject::connect(_bridge, &ProtocolBridge::roomListChanged, this, [this] {
            scheduleUnreadRoomListRefresh();
        });
        QObject::connect(_bridge, &ProtocolBridge::timelineChanged, this,
            [this](const QString &roomId) {
                if (roomId.isEmpty()) {
                    return;
                }
                refreshUnreadRoomSnapshot(roomId);
                if (roomId == _activeRoomId) {
                    scheduleUnreadTimelineRefresh(roomId);
                }
            });
        QObject::connect(_bridge, &ProtocolBridge::roomsReady, this,
            [this](
                quint64 requestId,
                bool success,
                const QVector<RoomSummary> &rooms) {
                if (requestId != _latestUnreadRoomListRequestId
                    || !success
                    || rooms.isEmpty()) {
                    return;
                }
                _unreadStateStore->applyRoomListSnapshot(rooms);
            });
        QObject::connect(_bridge, &ProtocolBridge::roomUnreadSnapshotReady, this,
            [this](
                const QString &roomId,
                quint64 requestId,
                bool success,
                const RoomUnreadSnapshot &snapshot) {
                if (roomId.isEmpty()) {
                    return;
                }
                const auto it = _latestUnreadSnapshotRequestIds.constFind(roomId);
                if (it == _latestUnreadSnapshotRequestIds.constEnd()
                    || it.value() != requestId) {
                    return;
                }
                _latestUnreadSnapshotRequestIds.remove(roomId);
                if (!success) {
                    return;
                }
                _unreadStateStore->applyRoomUnreadSnapshot(roomId, snapshot);
            });
        QObject::connect(_bridge, &ProtocolBridge::timelineSliceReady, this,
            [this](
                const QString &roomId,
                quint64 requestId,
                bool success,
                const TimelineSlice &slice) {
                if (requestId != _latestUnreadTimelineRequestId
                    || !success
                    || roomId != _activeRoomId) {
                    return;
                }
                _unreadStateStore->applyTimelineSnapshot(roomId, slice);
            });
        QObject::connect(
            _bridge,
            &ProtocolBridge::roomNotificationModeSetForRoom,
            this,
            [this](const QString &roomId, RoomNotificationMode mode, bool success) {
                if (!success || roomId.isEmpty()) {
                    return;
                }
                _unreadStateStore->setRoomNotificationMode(
                    roomId,
                    mode,
                    mode == RoomNotificationMode::Mute);
            });
        QTimer::singleShot(0, this, [this] {
            refreshUnreadRoomListSnapshot();
            if (!_activeRoomId.isEmpty()) {
                _pendingUnreadTimelineRoomId = _activeRoomId;
                refreshUnreadTimelineSnapshot();
            }
        });
    }

    if (_bridge) {
        QObject::connect(_bridge, &ProtocolBridge::roomsReady, this,
            [this](
                quint64 requestId,
                bool success,
                const QVector<RoomSummary> &rooms) {
                if (_latestRoomLeftCheckRequestId == 0
                    || requestId != _latestRoomLeftCheckRequestId) {
                    return;
                }
                _latestRoomLeftCheckRequestId = 0;
                if (success) {
                    clearActiveRoomIfMissing(rooms);
                }
            });
    }

    // Room settings requested from history top bar.
    QObject::connect(_history, &HistoryWidget::openRoomSettingsRequested,
        this, &AppMainWidget::showRoomSettings);
    QObject::connect(_history, &HistoryWidget::openRoomMembersSettingsRequested,
        this, &AppMainWidget::showRoomSettingsForMembers);

    // User profile popup requested from avatar click.
    QObject::connect(_history, &HistoryWidget::openUserProfileRequested,
        this, &AppMainWidget::showUserProfilePopup);

    // When HistoryWidget navigates to a different room (e.g. matrix.to link),
    // sync the chat list selection and active room tracking. A room we are
    // not a member of has no timeline to switch to — open it as a preview
    // (with the Join bar) instead, like the Explore flow does.
    QObject::connect(_history, &HistoryWidget::roomSwitchRequested,
        this, [this](const QString &roomId, const QStringList &via) {
        bool joined = false;
        if (_bridge) {
            for (const auto &room : _bridge->cachedRooms()) {
                if (room.roomId == roomId
                    && room.membership == MembershipState::Join) {
                    joined = true;
                    break;
                }
            }
        }
        if (!joined) {
            showRoomPreview(roomId, via);
            return;
        }
        if (roomId != _activeRoomId) {
            _activeRoomId = roomId;
            if (_unreadStateStore) {
                _unreadStateStore->setActiveRoomId(roomId);
            }
            emit activeRoomChanged(roomId);
            _dialogs->selectRoomById(roomId);
        }
    });

    // Main menu overlay (Telegram-style left drawer).
    _mainMenuOverlay = new DialogsMainMenuOverlay(_controller, this);
    QObject::connect(_dialogs, &DialogsWidget::mainMenuRequested,
        _mainMenuOverlay, &DialogsMainMenuOverlay::toggle);

    // Wire "New Chat" from main menu to user search and direct-room open.
    QObject::connect(_mainMenuOverlay, &DialogsMainMenuOverlay::newChatRequested,
        this, [this] {
            if (!_bridge) {
                return;
            }
            auto *dlg = new DialogsNewChatBox(_bridge, this);
            if (dlg->exec() == DialogsNewChatBox::Accepted) {
                const auto userId = dlg->selectedUserId();
                if (!userId.isEmpty()) {
                    auto *waiter = new QObject(this);
                    QObject::connect(_bridge, &ProtocolBridge::directRoomCreated,
                        waiter, [this, waiter, userId](
                            const QString &createdFor,
                            bool success,
                            const QString &roomId) {
                        if (createdFor != userId) {
                            return;
                        }
                        waiter->deleteLater();
                        if (!success || roomId.isEmpty()) {
                            return;
                        }
                        if (_dialogs) {
                            _dialogs->refreshRooms();
                        }
                        showRoom(roomId);
                    });
                    _bridge->createDirectRoom(userId);
                }
            }
            dlg->deleteLater();
        });

    QObject::connect(_mainMenuOverlay, &DialogsMainMenuOverlay::exploreRoomsRequested,
        this, [this] { openExploreRooms(); });

    QObject::connect(_mainMenuOverlay, &DialogsMainMenuOverlay::savedMessagesRequested,
        this, &AppMainWidget::openSavedMessages);

    // Wire "New Room" from main menu to create room dialog.
    QObject::connect(_mainMenuOverlay, &DialogsMainMenuOverlay::newRoomRequested,
        this, [this] {
            if (!_bridge) {
                return;
            }
            auto *dlg = new DialogsRoomCreateDialog(
                serverNameFromSession(_bridge->cachedSessionInfo()),
                this);
            QPointer<DialogsRoomCreateDialog> guard(dlg);
            QString createdRoomId;

            QObject::connect(dlg, &DialogsRoomCreateDialog::createRequested,
                dlg, [this, guard] {
                    if (!guard || !_bridge) {
                        return;
                    }
                    const auto name = guard->roomName();
                    if (name.isEmpty()) {
                        return;
                    }
                    CreateRoomRequest req;
                    req.name = name;
                    req.topic = guard->roomTopic();
                    req.isPublic = guard->isPublic();
                    req.encrypted = guard->isEncrypted();
                    req.alias = guard->roomAlias();
                    req.avatarPath = guard->avatarPath();
                    req.guestAccess = static_cast<CreateRoomGuestAccess>(
                        guard->guestAccess());
                    req.historyVisibility = static_cast<CreateRoomHistoryVisibility>(
                        guard->historyVisibility());
                    req.federate = !guard->blockFederated();
                    guard->showError(QString());
                    guard->setControlsEnabled(false);
                    _bridge->createRoom(req);
                });

            QObject::connect(_bridge, &ProtocolBridge::roomCreated,
                dlg, [guard, &createdRoomId](bool success, const QString &roomId) {
                    if (!guard) {
                        return;
                    }
                    if (!success || roomId.isEmpty()) {
                        guard->setControlsEnabled(true);
                        guard->showError(tr(
                            "Failed to create room. Check the room settings and try again."));
                        return;
                    }
                    createdRoomId = roomId;
                    guard->accept();
                });

            if (dlg->exec() == DialogsRoomCreateDialog::Accepted
                && !createdRoomId.isEmpty()) {
                auto *inviteBox = new InviteUsersBox(createdRoomId, _bridge, this);
                inviteBox->exec();
                inviteBox->deleteLater();

                // Give sync a moment to include the new room in the list.
                QTimer::singleShot(500, this, [this, createdRoomId] {
                    showRoom(createdRoomId);
                });
            }
            dlg->deleteLater();
        });

    // Wire "Settings" from main menu.
    QObject::connect(_mainMenuOverlay, &DialogsMainMenuOverlay::settingsRequested,
        this, &AppMainWidget::showSettings);

    // Wire "Verify session" from main menu.
    QObject::connect(_mainMenuOverlay, &DialogsMainMenuOverlay::verifySessionRequested,
        this, [this] {
            QTimer::singleShot(st::mainMenuAnimationDuration, this, [this] {
                openVerifySessionDialog();
            });
        });

    // Wire "Sign Out" from main menu.
    QObject::connect(_mainMenuOverlay, &DialogsMainMenuOverlay::signOutRequested,
        this, &AppMainWidget::logoutRequested);

    // Leave room from top-bar 3-dots menu.
    QObject::connect(_history, &HistoryWidget::leaveRoomRequested,
        this, [this](const QString &roomId) {
            if (_bridge && !roomId.isEmpty()) {
                _bridge->leaveRoom(roomId);
            }
        });

    // Room left: if the active room was left, clear the history view.
    QObject::connect(_bridge, &ProtocolBridge::roomLeft,
        this, [this](bool success) {
            if (!success) {
                return;
            }
            if (_activeRoomId.isEmpty()) {
                return;
            }
            _latestRoomLeftCheckRequestId = _bridge->nextRequestId();
            _bridge->getRoomsAsync(_latestRoomLeftCheckRequestId);
        });

    // Saved Messages permanently deleted while it is the open room: the cached
    // id goes empty as the room leaves. Re-check the list and clear the view if
    // the active room is gone — the same treatment as leaving any room.
    QObject::connect(_bridge, &ProtocolBridge::savedMessagesRoomChanged,
        this, [this](const QString &roomId) {
            if (!roomId.isEmpty() || _activeRoomId.isEmpty()) {
                return;
            }
            _latestRoomLeftCheckRequestId = _bridge->nextRequestId();
            _bridge->getRoomsAsync(_latestRoomLeftCheckRequestId);
        });

    // Floating connection indicator at the bottom-left of the chat
    // list. Driven by BOTH the OS network monitor (instant up/down) and the sync
    // state; not in the layout so it floats over the dialogs column.
    _connecting = new ::Ui::ConnectingWidget(this);
    connect(_connecting, &::Ui::ConnectingWidget::retryRequested, this, [this] {
        if (_bridge) {
            _bridge->reconnect();
        }
    });
    _networkMonitor = new ::TeleMatrix::NetworkMonitor(this);
    _networkOnline = _networkMonitor->online();
    connect(_networkMonitor, &::TeleMatrix::NetworkMonitor::onlineChanged, this,
        [this](bool online) {
            setNetworkOnline(online);
            // Interface returned → reconnect now instead of waiting out the
            // long-poll/backoff.
            if (online && _bridge) {
                _bridge->reconnect();
            }
            // Update the upload "Waiting for network..." label and auto-resend
            // failed direct uploads on reconnect.
            if (_history) {
                _history->onNetworkOnlineChanged(online);
            }
        });
    if (_history) {
        _history->onNetworkOnlineChanged(_networkOnline); // seed initial state
    }

    // Show on a sync regression (state 1) once we've already synced (state 2) at
    // least once. The bridge is recreated on logout, but so is this widget, so
    // the connection is re-armed naturally.
    if (_bridge) {
        QObject::connect(_bridge, &ProtocolBridge::syncStateChanged,
            this, &AppMainWidget::applySyncState);
        // Apply the current state in case sync changed before we connected.
        applySyncState(_bridge->syncState());
    }
    applyConnectionState();

    // Layer stack sits on top of everything, hidden by default.
    _layerStack = new LayerStackWidget(this);
}

void AppMainWidget::applySyncState(int state) {
    // States: 0=not-started, 1=syncing, 2=synced, 3=store-error.
    // Store-error (3) is handled by AppController's forced-logout dialog.
    if (state == 2) {
        _everSynced = true;
    }
    _syncState = state;
    applyConnectionState();
}

void AppMainWidget::setNetworkOnline(bool online) {
    if (_networkOnline == online) {
        return;
    }
    _networkOnline = online;
    applyConnectionState();
}

// Show the indicator whenever a working connection is lost — the OS reports no
// network (instant, via the monitor) or the sync regressed to SYNCING after the
// first successful sync. The widget itself owns the "Connecting…" → "Reconnect in
// N s…" progression; the first sync (everSynced == false) shows nothing.
void AppMainWidget::applyConnectionState() {
    const auto disconnected = _everSynced
        && (_syncState == 1 || !_networkOnline);
    if (_connecting) {
        _connecting->setLeftOffset(_dialogs ? _dialogs->chatListLeft() : 0);
        _connecting->setConnected(!disconnected);
    }
    if (_history) {
        _history->setConnecting(disconnected);
    }
}

void AppMainWidget::applyCachedRooms(const QVector<RoomSummary> &rooms) {
    if (_dialogs && !rooms.isEmpty()) {
        _dialogs->applyCachedRooms(rooms);
    }
    if (_unreadStateStore && !rooms.isEmpty()) {
        _unreadStateStore->applyRoomListSnapshot(rooms);
    }
}

void AppMainWidget::scheduleUnreadRoomListRefresh() {
    if (!_unreadStateStore || !_bridge || !_unreadRoomListRefreshTimer) {
        return;
    }
    _unreadRoomListRefreshPending = true;
    if (!_appActive) {
        if (_unreadRoomListRefreshTimer->isActive()) {
            _unreadRoomListRefreshTimer->stop();
        }
        return;
    }
    _unreadRoomListRefreshTimer->start(kUnreadRefreshDelay);
}

void AppMainWidget::scheduleUnreadTimelineRefresh(const QString &roomId) {
    if (!_unreadStateStore || !_bridge || !_unreadTimelineRefreshTimer
        || roomId.isEmpty()) {
        return;
    }
    _pendingUnreadTimelineRoomId = roomId;
    _unreadTimelineRefreshPending = true;
    if (!_appActive) {
        if (_unreadTimelineRefreshTimer->isActive()) {
            _unreadTimelineRefreshTimer->stop();
        }
        return;
    }
    _unreadTimelineRefreshTimer->start(kUnreadRefreshDelay);
}

void AppMainWidget::refreshUnreadRoomListSnapshot() {
    if (!_unreadStateStore || !_bridge) {
        return;
    }
    _unreadRoomListRefreshPending = false;
    const auto cached = _bridge->cachedRooms();
    if (!cached.isEmpty()) {
        _unreadStateStore->applyRoomListSnapshot(cached);
    }
    _latestUnreadRoomListRequestId = _bridge->nextRequestId();
    _bridge->getRoomsAsync(_latestUnreadRoomListRequestId);
}

void AppMainWidget::refreshUnreadRoomSnapshot(const QString &roomId) {
    if (!_unreadStateStore || !_bridge || roomId.isEmpty()) {
        return;
    }
    const auto requestId = _bridge->nextRequestId();
    _latestUnreadSnapshotRequestIds.insert(roomId, requestId);
    _bridge->getRoomUnreadSnapshotAsync(roomId, requestId);
}

void AppMainWidget::refreshUnreadTimelineSnapshot() {
    if (!_unreadStateStore || !_bridge) {
        return;
    }
    const auto roomId = _pendingUnreadTimelineRoomId.isEmpty()
        ? _activeRoomId
        : _pendingUnreadTimelineRoomId;
    _pendingUnreadTimelineRoomId.clear();
    _unreadTimelineRefreshPending = false;
    if (roomId.isEmpty() || roomId != _activeRoomId) {
        return;
    }
    _latestUnreadTimelineRequestId = _bridge->nextRequestId();
    _bridge->getTimelineSliceAsync(roomId, _latestUnreadTimelineRequestId);
}

void AppMainWidget::clearActiveRoomIfMissing(const QVector<RoomSummary> &rooms) {
    if (_activeRoomId.isEmpty()) {
        return;
    }
    // A previewed room is deliberately not in the rooms list — we haven't joined it. Without this
    // it would be force-closed the moment the user leaves any other room.
    if (_history && _history->isPreviewingRoom()) {
        return;
    }
    const auto found = std::any_of(
        rooms.cbegin(),
        rooms.cend(),
        [this](const RoomSummary &room) {
            return room.roomId == _activeRoomId;
        });
    if (found) {
        return;
    }
    _history->closeRoom();
    _dialogs->clearSelection();
    _dialogs->setFocus();
    _activeRoomId.clear();
    if (_unreadStateStore) {
        _unreadStateStore->setActiveRoomId(QString());
    }
    emit activeRoomChanged(QString());
}

void AppMainWidget::handleApplicationStateChanged(Qt::ApplicationState state) {
    const auto active = (state == Qt::ApplicationActive);
    if (_appActive == active) {
        return;
    }
    _appActive = active;
    if (!_appActive) {
        if (_unreadRoomListRefreshTimer) {
            _unreadRoomListRefreshTimer->stop();
        }
        if (_unreadTimelineRefreshTimer) {
            _unreadTimelineRefreshTimer->stop();
        }
        return;
    }

    if (_unreadRoomListRefreshPending && _unreadRoomListRefreshTimer) {
        _unreadRoomListRefreshTimer->start(kUnreadActivationRefreshDelay);
    }
    if (_unreadTimelineRefreshPending
        && !_pendingUnreadTimelineRoomId.isEmpty()
        && _pendingUnreadTimelineRoomId == _activeRoomId
        && _unreadTimelineRefreshTimer) {
        _unreadTimelineRefreshTimer->start(kUnreadActivationRefreshDelay);
    }
}

void AppMainWidget::showRoomAtEvent(const QString &roomId, const QString &eventId) {
    if (eventId.isEmpty() || !_history) {
        showRoom(roomId);
        return;
    }
    // showMessage is the same jump funnel a search result hit uses: it opens the
    // room positioned on the event and highlights it, instead of landing on the
    // unread delimiter the way a plain open does.
    _history->showMessage(roomId, eventId);
    if (_activeRoomId != roomId) {
        _activeRoomId = roomId;
        if (_unreadStateStore) {
            _unreadStateStore->setActiveRoomId(roomId);
        }
        emit activeRoomChanged(roomId);
    }
}

void AppMainWidget::showRoom(const QString &roomId) {
    if (_history) {
        if (_activeRoomId == roomId) {
            _history->scrollToUnreadOrBottom();
        } else {
            _history->loadRoom(roomId);
        }
    }
    if (_activeRoomId != roomId) {
        _activeRoomId = roomId;
        if (_unreadStateStore) {
            _unreadStateStore->setActiveRoomId(roomId);
        }
        emit activeRoomChanged(roomId);
        // Sync the chat list highlight to match the displayed room.
        // Without this, external triggers (push notifications, deep links)
        // leave the list highlighting the previous room.
        if (_dialogs) {
            _dialogs->selectRoomById(roomId);
        }
    }
}

bool AppMainWidget::eventFilter(QObject *watched, QEvent *event) {
    if (watched == _userProfileOverlay) {
        if (event->type() == QEvent::KeyPress) {
            const auto key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Escape && key->modifiers() == Qt::NoModifier) {
                closeUserProfileOverlay();
                key->accept();
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonPress) {
            const auto mouse = static_cast<QMouseEvent*>(event);
            if (!_userProfilePopup || !_userProfilePopup->geometry().contains(mouse->pos())) {
                closeUserProfileOverlay();
                mouse->accept();
                return true;
            }
        }
    }

    if (event->type() != QEvent::KeyPress
        || !_dialogs
        || !_history) {
        return QWidget::eventFilter(watched, event);
    }

    const auto key = static_cast<QKeyEvent*>(event);
    if (key->key() != Qt::Key_Escape || key->modifiers() != Qt::NoModifier) {
        return QWidget::eventFilter(watched, event);
    }

    const auto belongsToMainWindow = [this](QWidget *widget) {
        return widget
            && window()
            && (widget == window() || widget->window() == window());
    };
    const auto target = qobject_cast<QWidget*>(watched);
    if (target && !belongsToMainWindow(target)) {
        return QWidget::eventFilter(watched, event);
    }

    if (_layerStack && _layerStack->hasLayer()) {
        return QWidget::eventFilter(watched, event);
    }
    if (_mainMenuOverlay && _mainMenuOverlay->isShown()) {
        return QWidget::eventFilter(watched, event);
    }
    if (_mediaView && _mediaView->isVisible()) {
        return QWidget::eventFilter(watched, event);
    }

    if (_dialogs->hasActiveSearch() && !_history->inputHasFocus()
        && _dialogs->handleSearchEscape()) {
        key->accept();
        return true;
    }

    const auto targetInHistory = target
        && (target == _history || _history->isAncestorOf(target));
    const auto targetIsStaleHidden = target && !target->isVisible();
    if (!_activeRoomId.isEmpty()
        && !_history->inputHasFocus()
        && (targetInHistory || targetIsStaleHidden)) {
        _history->escape();
        key->accept();
        return true;
    }

    return QWidget::eventFilter(watched, event);
}

// The box only finds a room; opening it in preview mode (with the Join bar) is
// what actually joins.
void AppMainWidget::openExploreRooms(const QString &spaceId, const QString &name) {
    if (!_bridge) {
        return;
    }
    auto *dlg = new DialogsExploreRoomsBox(_bridge, this);
    if (!spaceId.isEmpty()) {
        dlg->openInSpace(spaceId, name);
    }
    if (dlg->exec() == DialogsExploreRoomsBox::Accepted) {
        const auto roomId = dlg->chosenRoomId();
        if (!roomId.isEmpty()) {
            // If we are already a member, open the real room, not a read-only preview with a
            // pointless JOIN button. cachedRooms is authoritative for joined rooms.
            bool alreadyJoined = false;
            for (const auto &room : _bridge->cachedRooms()) {
                if (room.roomId == roomId
                    && room.membership == MembershipState::Join) {
                    alreadyJoined = true;
                    break;
                }
            }
            if (alreadyJoined) {
                showRoom(roomId);
            } else {
                showRoomPreview(roomId, dlg->chosenVia());
            }
        }
    }
    dlg->deleteLater();
}

void AppMainWidget::openSavedMessages() {
    if (!_bridge) {
        return;
    }
    const auto existing = _bridge->savedMessagesRoomId();
    if (!existing.isEmpty()) {
        showRoom(existing);
        return;
    }
    // No saved room yet: opening it from the menu is one of the two moments it
    // is allowed to be created (the other is forwarding to it).
    auto *waiter = new QObject(this);
    QObject::connect(_bridge, &ProtocolBridge::savedMessagesRoomReady, waiter,
        [this, waiter](bool success, const QString &roomId) {
            waiter->deleteLater();
            if (success && !roomId.isEmpty()) {
                if (_dialogs) {
                    _dialogs->refreshRooms();
                }
                showRoom(roomId);
            }
        });
    _bridge->ensureSavedMessagesRoom(/*create=*/true);
}

void AppMainWidget::focusSearch() {
    // Don't focus a widget that is covered by a layer or the media viewer.
    if (_layerStack && _layerStack->hasLayer()) {
        return;
    }
    if (_mediaView && _mediaView->isVisible()) {
        return;
    }
    if (_mainMenuOverlay && _mainMenuOverlay->isShown()) {
        _mainMenuOverlay->hideAnimated();
    }
    if (!_activeRoomId.isEmpty()
        && _history
        && _history->requestSearchInCurrentRoom()) {
        return;
    }
    if (_dialogs) {
        _dialogs->focusSearch();
    }
}

void AppMainWidget::ensureSettingsWidget() {
    if (_settingsWidget) {
        return;
    }
    _settingsWidget = new SettingsWidget(_controller, _layerStack);
    connect(_settingsWidget, &SettingsWidget::closeRequested,
            _layerStack, &LayerStackWidget::hideLayer);
    connect(_settingsWidget, &SettingsWidget::logoutRequested,
            this, &AppMainWidget::logoutRequested);
}

void AppMainWidget::showSettings() {
    // Close main menu drawer before opening settings.
    if (_mainMenuOverlay && _mainMenuOverlay->isShown()) {
        _mainMenuOverlay->hideAnimated();
    }
    if (_layerStack->hasLayer()) {
        _layerStack->hideLayer();
        return;
    }
    ensureSettingsWidget();
    _settingsWidget->prepareForShow();
    _layerStack->showLayer(_settingsWidget);
}

void AppMainWidget::showSessions(const QString &signOutDeviceId) {
    if (_mainMenuOverlay && _mainMenuOverlay->isShown()) {
        _mainMenuOverlay->hideAnimated();
    }
    ensureSettingsWidget();
    // Unlike showSettings() this never toggles: the banner asked for the sessions
    // page specifically, so always land there. prepareForShow() restores the last
    // section, so select Sessions after it.
    _settingsWidget->prepareForShow();
    _layerStack->showLayer(_settingsWidget);
    _settingsWidget->openSessions(signOutDeviceId);
}

void AppMainWidget::showThemeSelector() {
    // The picker is a full-height side panel, so the settings layer (and the
    // dimming it brings) has to go: the point is to see the app re-skin. It
    // comes back once the panel is done, since that is where we came from.
    _reopenSettingsAfterTheme = _layerStack->hasLayer();
    if (_layerStack->hasLayer()) {
        _layerStack->hideLayer();
    }
    if (_mainMenuOverlay && _mainMenuOverlay->isShown()) {
        _mainMenuOverlay->hideAnimated();
    }
    if (!_themePanel) {
        _themePanel = new ThemeSelectorPanel(_controller, this);
        connect(_themePanel, &ThemeSelectorPanel::closed, this, [this] {
            if (_reopenSettingsAfterTheme) {
                _reopenSettingsAfterTheme = false;
                showSettings();
            }
        });
    }
    _themePanel->showAnimated();
}

void AppMainWidget::showRoomPreview(const QString &roomId, const QStringList &via) {
    if (!_history) {
        return;
    }
    _history->loadRoomPreview(roomId, via);

    // Deliberately not registered with the unread store: we are not a member, so it has no state
    // for this room and never will until we join.
    _activeRoomId = roomId;
    emit activeRoomChanged(roomId);
    if (_dialogs) {
        // There is no row to select — the room is not in the list.
        _dialogs->clearSelection();
    }
}

void AppMainWidget::showRoomSettings(const QString &roomId) {
    showRoomSettingsInternal(roomId, false);
}

void AppMainWidget::showRoomSettingsForMembers(const QString &roomId) {
    showRoomSettingsInternal(roomId, true);
}

void AppMainWidget::showRoomSettingsInternal(const QString &roomId, bool showMembersSection) {
    if (roomId.isEmpty()) return;

    if (_layerStack->hasLayer()) {
        _layerStack->hideLayer();
        return;
    }

    _roomSettingsWidget = new RoomSettingsWidget(roomId, _controller, _bridge, _layerStack);
    if (showMembersSection) {
        _roomSettingsWidget->showMembersSection();
    }
    connect(_roomSettingsWidget, &RoomSettingsWidget::closeRequested,
            _layerStack, &LayerStackWidget::hideLayer);
    connect(_roomSettingsWidget, &RoomSettingsWidget::exportHistoryRequested,
            this, [this](const QString &roomId) {
        _layerStack->hideLayer();
        QTimer::singleShot(0, this, [this, roomId] {
            if (_history) {
                _history->exportRoomHistory(roomId);
            }
        });
    });
    connect(_roomSettingsWidget, &RoomSettingsWidget::leaveRoomRequested,
            this, [this](const QString &roomId) {
        _layerStack->hideLayer();
        _bridge->leaveRoom(roomId);
    });
    connect(_roomSettingsWidget, &RoomSettingsWidget::openUserProfileRequested,
            this, &AppMainWidget::showUserProfilePopupOverLayer);
    connect(_layerStack, &LayerStackWidget::layerHidden,
            this, [this] {
        if (_roomSettingsWidget) {
            _roomSettingsWidget->deleteLater();
            _roomSettingsWidget = nullptr;
        }
    }, Qt::SingleShotConnection);
    _layerStack->showLayer(_roomSettingsWidget);
}

void AppMainWidget::showUserProfilePopup(
        const QString &roomId, const QString &userId, const QString &displayName) {
    if (userId.isEmpty()) return;

    if (_layerStack->hasLayer()) {
        _layerStack->hideLayer();
        return;
    }

    _userProfilePopup = new UserProfilePopup(roomId, userId, displayName, _bridge, _layerStack);
    connect(_userProfilePopup, &UserProfilePopup::mentionRequested,
            this, [this](const QString &roomId, const QString &userId, const QString &displayName) {
        QTimer::singleShot(0, this, [this, roomId, userId, displayName] {
            if (!_history || userId.isEmpty()) {
                return;
            }
            if (!roomId.isEmpty() && roomId != _activeRoomId) {
                showRoom(roomId);
            }
            _history->mentionUser(userId, displayName);
        });
    });
    connect(_userProfilePopup, &UserProfilePopup::openRoomRequested,
            this, [this](const QString &roomId) {
        QTimer::singleShot(0, this, [this, roomId] {
            if (roomId.isEmpty()) {
                return;
            }
            showRoom(roomId);
            if (_dialogs) {
                _dialogs->selectRoomById(roomId);
            }
        });
    });
    connect(_userProfilePopup, &UserProfilePopup::closeRequested,
            _layerStack, &LayerStackWidget::hideLayer);
    connect(_layerStack, &LayerStackWidget::layerHidden,
            this, [this] {
        if (_userProfilePopup) {
            _userProfilePopup->deleteLater();
            _userProfilePopup = nullptr;
        }
    }, Qt::SingleShotConnection);
    _layerStack->showLayer(_userProfilePopup);
}

void AppMainWidget::showUserProfilePopupOverLayer(
    const QString &roomId,
    const QString &userId) {
    if (userId.isEmpty()) {
        return;
    }
    if (!_layerStack || !_layerStack->hasLayer()) {
        showUserProfilePopup(roomId, userId, QString());
        return;
    }

    closeUserProfileOverlay();

    auto *overlay = new UserProfileLayerOverlay(_layerStack);
    _userProfileOverlay = overlay;
    _userProfileOverlay->setFocusPolicy(Qt::StrongFocus);
    _userProfileOverlay->setGeometry(_layerStack->rect());
    _userProfileOverlay->installEventFilter(this);

    _userProfilePopup = new UserProfilePopup(roomId, userId, QString(), _bridge, _userProfileOverlay);
    overlay->setPopup(_userProfilePopup);
    connect(_userProfilePopup, &UserProfilePopup::sizeHintChanged,
            this, &AppMainWidget::positionUserProfileOverlay);
    connect(_userProfilePopup, &UserProfilePopup::closeRequested,
            this, &AppMainWidget::closeUserProfileOverlay);
    connect(_userProfilePopup, &UserProfilePopup::mentionRequested,
            this, [this](const QString &roomId, const QString &userId, const QString &displayName) {
        closeUserProfileOverlay();
        if (_layerStack && _layerStack->hasLayer()) {
            _layerStack->hideLayer();
        }
        QTimer::singleShot(0, this, [this, roomId, userId, displayName] {
            if (!_history || userId.isEmpty()) {
                return;
            }
            if (!roomId.isEmpty() && roomId != _activeRoomId) {
                showRoom(roomId);
            }
            _history->mentionUser(userId, displayName);
        });
    });
    connect(_userProfilePopup, &UserProfilePopup::openRoomRequested,
            this, [this](const QString &roomId) {
        closeUserProfileOverlay();
        if (_layerStack && _layerStack->hasLayer()) {
            _layerStack->hideLayer();
        }
        QTimer::singleShot(0, this, [this, roomId] {
            if (roomId.isEmpty()) {
                return;
            }
            showRoom(roomId);
            if (_dialogs) {
                _dialogs->selectRoomById(roomId);
            }
        });
    });
    connect(_layerStack, &LayerStackWidget::layerHidden,
            _userProfileOverlay, [this] {
        closeUserProfileOverlay();
    });

    positionUserProfileOverlay();
    _userProfileOverlay->show();
    _userProfileOverlay->raise();
    _userProfilePopup->show();
    _userProfilePopup->setFocus();
}

void AppMainWidget::closeUserProfileOverlay() {
    if (!_userProfileOverlay) {
        return;
    }
    auto *overlay = _userProfileOverlay;
    _userProfileOverlay = nullptr;
    _userProfilePopup = nullptr;
    overlay->removeEventFilter(this);
    overlay->hide();
    overlay->deleteLater();
    if (_roomSettingsWidget) {
        _roomSettingsWidget->setFocus();
    }
}

void AppMainWidget::positionUserProfileOverlay() {
    if (!_userProfileOverlay || !_userProfilePopup || !_layerStack) {
        return;
    }
    _userProfileOverlay->setGeometry(_layerStack->rect());
    const auto hint = _userProfilePopup->sizeHint();
    const int margin = st::layerVerticalMargin;
    const int availableWidth = qMax(0, _userProfileOverlay->width() - 2 * margin);
    const int availableHeight = qMax(0, _userProfileOverlay->height() - 2 * margin);
    const int popupWidth = qMin(hint.width(), availableWidth);
    const int popupHeight = qMin(hint.height(), availableHeight);
    const int popupX = (_userProfileOverlay->width() - popupWidth) / 2;
    const int popupY = margin + (availableHeight - popupHeight) / 2;
    _userProfilePopup->setGeometry(popupX, popupY, popupWidth, popupHeight);
    _userProfileOverlay->update();
}

void AppMainWidget::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    if (!_dialogsWidthRestored) {
        restoreDialogsWidth();
    }
    if (_mainMenuOverlay) {
        _mainMenuOverlay->setGeometry(rect());
    }
    if (_layerStack) {
        _layerStack->setGeometry(rect());
    }
    if (_connecting) {
        _connecting->setLeftOffset(_dialogs ? _dialogs->chatListLeft() : 0);
        _connecting->reposition();
    }
    positionUserProfileOverlay();
}

void AppMainWidget::setConnectingBottomSkip(int skip) {
    if (_connecting) {
        _connecting->setBottomSkip(skip);
    }
    // The bar is full-bleed, so the pane seam must not cross it. groupCallLive2
    // is the gradient's right-hand stop, i.e. exactly the colour the bar ends on
    // one pixel to the handle's left — so the two read as one surface.
    if (_splitter) {
        static_cast<ThemedSplitter *>(_splitter)->setHandleBottomCover(
            skip, skip > 0 ? st::groupCallLive2 : QColor());
    }
}

void AppMainWidget::restoreDialogsWidth() {
    if (_dialogsWidthRestored || width() <= 0) return;

    const int sidebarWidth = Dialogs::RestoredWidth(
        _controller->settings().dialogsWidth(),
        kSidebarDefaultWidth,
        kSidebarMinWidth,
        kSidebarMaxWidth);
    _splitter->setSizes({ sidebarWidth, qMax(0, width() - sidebarWidth) });
    _dialogsWidthRestored = true;
}

void AppMainWidget::saveDialogsWidth() {
    const auto sizes = _splitter->sizes();
    if (sizes.size() < 2 || sizes[0] <= 0) return;

    _controller->settings().setDialogsWidth(sizes[0]);
    _controller->saveSettingsDelayed();
}

void AppMainWidget::openVerifySessionDialog() {
    if (!_bridge) {
        return;
    }

    if (ShowVerifySessionPopup(_bridge, this)) {
        _bridge->getEncryptionOverview();
    }
}

void AppMainWidget::openIncomingVerifySessionDialog(const QString &transactionId) {
    if (!_bridge) {
        return;
    }

    if (ShowVerifySessionPopup(_bridge, this, transactionId)) {
        _bridge->getEncryptionOverview();
    }
}

void AppMainWidget::openIncomingUserVerifyDialog(
        const QString &flowId,
        const QString &displayName) {
    if (!_bridge) {
        return;
    }

    // Incoming cross-user request: the request already exists in the active
    // context, so start SAS on it by flow id (empty targetUserId), while the
    // display name titles the dialog "Verify <name>".
    auto *dialog = new VerifyUserDialog(
        _bridge,
        this,
        QString(),
        displayName,
        flowId);
    dialog->exec();
    dialog->deleteLater();
}

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>
#include <QVector>

#include "../protocol/protocol_types.h"

class QAbstractButton;
class QEventLoop;
class QLabel;
class QLineEdit;
class QKeyEvent;
class QMouseEvent;
class QPainter;
class QPaintEvent;
class QScrollArea;
class QTimer;
class QVariantAnimation;

namespace Ui {
class TextButton;
} // namespace Ui

namespace TeleMatrix {

class ProtocolBridge;

class ExploreRoomListInner final : public QWidget {
    Q_OBJECT

public:
    explicit ExploreRoomListInner(QWidget *parent = nullptr);

    void setEntries(const QVector<RoomDirectoryEntry> &entries);
    void setStatusText(const QString &text);
    [[nodiscard]] QVector<RoomDirectoryEntry> entries() const;

    /// Keeps the list at least as tall as the viewport, so a short result set still fills the box
    /// and the empty-state text centres over the whole of it.
    void setViewportHeight(int height);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    /// A space row — drill into its children.
    void spaceClicked(const QString &spaceId, const QString &name);
    /// An ordinary room row — open it (in preview mode if we are not a member).
    void roomClicked(const QString &roomId, const QStringList &via);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    [[nodiscard]] int indexAt(const QPoint &pos) const;
    [[nodiscard]] int contentHeight() const;
    void paintRow(QPainter &p, int index, bool hovered);

    QVector<RoomDirectoryEntry> _entries;
    QString _statusText;
    int _hovered = -1;
    int _viewportHeight = 0;
};

/// Search the homeserver's public room directory, and drill into any space it turns up.
///
/// The directory search is server-side (the homeserver matches the query against a room's name,
/// topic and canonical alias). A space cannot be searched at all — its children are paged in and
/// filtered locally, because the protocol offers nothing else.
///
/// This box only *finds* rooms. Choosing one closes it; joining happens in the history view, which
/// opens the room in preview mode with a Join bar.
class DialogsExploreRoomsBox final : public QWidget {
    Q_OBJECT

public:
    enum DialogCode {
        Rejected = 0,
        Accepted = 1,
    };

    explicit DialogsExploreRoomsBox(
        ProtocolBridge *bridge,
        QWidget *parent = nullptr);

    int exec();

    /// Open the box pre-entered into a joined space's children. Call before exec().
    void openInSpace(const QString &spaceId, const QString &name) {
        // Opened straight into the space (not navigated from the directory), so
        // the space is the root — there is nowhere to go "back" to from it.
        _rootIsSpace = true;
        enterSpace(spaceId, name);
    }

    /// The room the user picked, and the server hints needed to open/join it.
    [[nodiscard]] QString chosenRoomId() const;
    [[nodiscard]] QStringList chosenVia() const;

private:
    enum class View {
        Directory,
        Space,
    };

    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

    void accept();
    void reject();

    void onSearchTextChanged(const QString &text);
    void triggerDirectorySearch();
    void enterSpace(const QString &spaceId, const QString &name);
    // Load a space's children into the Space view (no navigation-stack bookkeeping — enterSpace
    // and leaveSpace manage that around it).
    void loadSpace(const QString &spaceId, const QString &name);
    void leaveSpace();
    void chooseRoom(const QString &roomId, const QStringList &via);

    void onPageReady(const RoomDirectoryPage &page);
    void onRequestFailed(quint64 requestId, const QString &error);
    void maybeLoadMore();
    // Grow the scrollable list (and the box with it) to fill the window height.
    void updateListHeight();
    void applySpaceFilter();
    void showEntries(const QVector<RoomDirectoryEntry> &entries);
    void updateHeader();
    void resolveAvatars(const QVector<RoomDirectoryEntry> &entries);

    ProtocolBridge *_bridge = nullptr;
    QWidget *_panel = nullptr;
    ExploreRoomListInner *_inner = nullptr;
    QScrollArea *_scroll = nullptr;
    QLineEdit *_searchField = nullptr;
    QLabel *_titleText = nullptr;
    QAbstractButton *_back = nullptr; // "<" chevron to the left of the title, shown inside a space
    ::Ui::TextButton *_cancel = nullptr;
    QTimer *_searchTimer = nullptr;

    View _view = View::Directory;

    // Directory view.
    QVector<RoomDirectoryEntry> _directoryEntries;
    QString _directoryNextToken;
    QString _directoryQuery;
    bool _directoryDone = false;
    int _directoryScrollPos = 0;

    // Space view. Filtering is local, so every page fetched stays here and the filter re-applies as
    // more arrive.
    QVector<RoomDirectoryEntry> _spaceEntries;
    QString _spaceNextToken;
    QString _spaceId;
    QString _spaceName;
    bool _spaceDone = false;
    // Ancestor spaces (id, name) from the directory down to — but excluding — the current one, so
    // "back" pops one level to the parent space instead of jumping all the way home.
    QVector<QPair<QString, QString>> _spaceStack;
    // The box was opened straight into a space (openInSpace), so the directory
    // is not its home: the "back" chevron is hidden at the root space and only
    // appears once the user drills into a sub-space (_spaceStack non-empty).
    bool _rootIsSpace = false;

    quint64 _requestCounter = 0;
    quint64 _activeRequestId = 0;
    // A scrollbar fires valueChanged many times per gesture; without this, one flick becomes a dozen
    // /hierarchy calls and an immediate 429.
    bool _loadingMore = false;

    QString _chosenRoomId;
    QStringList _chosenVia;

    qreal _bgOpacity = 0.0;
    qreal _layerOpacity = 0.0;
    QVariantAnimation *_a_shown = nullptr;
    QVariantAnimation *_a_layerShown = nullptr;

    QEventLoop *_loop = nullptr;
    int _result = Rejected;
};

} // namespace TeleMatrix

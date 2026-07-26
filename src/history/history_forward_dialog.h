// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>
#include <QVector>

#include "../protocol/protocol_types.h"

class QEventLoop;
class QLineEdit;
class QScrollArea;
class QVariantAnimation;
class QPaintEvent;
class QMouseEvent;
class QKeyEvent;

namespace Ui {
class TextButton;
} // namespace Ui

namespace TeleMatrix {

class ProtocolBridge;

/// Inner list widget for the forward dialog — custom-painted vertical
/// row list (peerListBoxItem style).
class PeerListInner final : public QWidget {
    Q_OBJECT

public:
    explicit PeerListInner(QWidget *parent = nullptr);

    void setRooms(const QVector<RoomSummary> &rooms);
    void setFilter(const QString &query);
    /// Rows with this id (or the pending sentinel) get the bookmark userpic.
    void setSavedMessagesRoomId(const QString &roomId);
    [[nodiscard]] QString selectedRoomId() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void roomClicked(const QString &roomId);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    int indexAt(const QPoint &pos) const;
    void paintRow(QPainter &p, int filteredIndex, bool selected, bool hovered);
    int contentHeight() const;

    QVector<RoomSummary> _allRooms;
    QVector<int> _filtered; // indices into _allRooms
    QString _savedRoomId;
    int _selected = -1;     // index in _filtered (-1 = none)
    int _hovered = -1;      // index in _filtered (-1 = none)
};

/// Modal overlay dialog for choosing a forward target.
/// A peer-list box in forward mode.
class HistoryForwardDialog final : public QWidget {
    Q_OBJECT

public:
    enum DialogCode { Rejected = 0, Accepted = 1 };

    explicit HistoryForwardDialog(
        const QVector<RoomSummary> &rooms,
        ProtocolBridge *bridge,
        QWidget *parent = nullptr);

    int exec();

    [[nodiscard]] QString selectedRoomId() const;

private:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

    void accept();
    void reject();

    QVector<RoomSummary> _rooms;
    QWidget *_panel = nullptr;
    PeerListInner *_inner = nullptr;
    QScrollArea *_scroll = nullptr;
    QLineEdit *_searchField = nullptr;
    ::Ui::TextButton *_cancel = nullptr;

    // _a_shown (background, easeOutCirc) + _a_layerShown (box shadow, linear)
    qreal _bgOpacity = 0.0;
    qreal _layerOpacity = 0.0;
    QVariantAnimation *_a_shown = nullptr;
    QVariantAnimation *_a_layerShown = nullptr;

    QEventLoop *_loop = nullptr;
    int _result = Rejected;
};

} // namespace TeleMatrix

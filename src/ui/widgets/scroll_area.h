// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

// Custom ScrollArea with an overlay scrollbar.
//
// Hides Qt's native scrollbar and paints a custom rounded-rect bar
// with auto-hide, hover states, and drag-to-scroll, styled to match
// the chat history scrollbar.
#pragma once

#include <QScrollArea>
#include <QScrollBar>
#include <QWidget>
#include <QTimer>
#include <QElapsedTimer>

#include "ui/widgets/input_fields.h" // for object_ptr

namespace Ui {

class ScrollArea;

/// Custom overlay scrollbar painted as rounded rects.
/// Child of ScrollArea, sits on top of content.
class ScrollBar : public QWidget {
    Q_OBJECT

public:
    ScrollBar(ScrollArea *parent, bool vertical);

    void recountSize();
    void updateBar(bool force = false);
    void hideTimeout(int ms);

protected:
    void paintEvent(QPaintEvent *e) override;
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;

public:
    // History-scroll style values (public for ScrollArea access).
    static constexpr int kWidth = 12;
    static constexpr int kDeltaX = 3;
    static constexpr int kDeltaT = 3;
    static constexpr int kDeltaB = 3;
    static constexpr int kMinHeight = 20;
    static constexpr int kRound = 3;
    static constexpr int kDuration = 150;   // animation ms
    static constexpr int kHiding = 1000;    // auto-hide timeout ms

private:
    ScrollArea *area();
    void setOver(bool over);
    void setOverBar(bool overbar);
    void setMoving(bool moving);
    void startOpacityAnimation(qreal from, qreal to);
    void startOverAnimation(qreal from, qreal to);
    void startBarOverAnimation(qreal from, qreal to);
    void hideTimerTick();

    bool _vertical = true;
    bool _hiding = true;
    bool _over = false;
    bool _overbar = false;
    bool _moving = false;

    QPoint _dragStart;
    QScrollBar *_connected = nullptr;
    int _startFrom = 0;
    int _scrollMax = 0;

    int _hideIn = kHiding;
    QTimer _hideTimer;

    // Animation state (0..1 values, updated by QTimer-driven ticks).
    qreal _opacityValue = 0.0;
    qreal _opacityTarget = 0.0;
    qreal _overValue = 0.0;
    qreal _overTarget = 0.0;
    qreal _barOverValue = 0.0;
    qreal _barOverTarget = 0.0;

    QElapsedTimer _opacityAnimTime;
    QElapsedTimer _overAnimTime;
    QElapsedTimer _barOverAnimTime;
    QTimer _animTimer;

    QRect _bar;
};

/// Thin capsule scrollbar for QAbstractScrollArea subclasses that cannot be
/// swapped for ScrollArea (text edits and the like), so they don't fall back to
/// the platform's native bar. Painted with live st:: colors, never a stylesheet,
/// so it follows theme changes.
///
/// Install with `edit->setVerticalScrollBar(new ThinScrollBar(edit))`; the
/// widget takes ownership. Width is set by the caller (kDefaultWidth suits the
/// flat input fields).
class ThinScrollBar : public QScrollBar {
    Q_OBJECT

public:
    explicit ThinScrollBar(QWidget *parent = nullptr);

    static constexpr int kDefaultWidth = 7;

protected:
    void paintEvent(QPaintEvent *e) override;
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    bool _hovered = false;
};

/// ScrollArea with a custom overlay scrollbar.
/// Hides Qt's native scrollbar. Provides lib_ui-compatible API.
class ScrollArea : public QScrollArea {
    Q_OBJECT

public:
    explicit ScrollArea(QWidget *parent = nullptr);
    ~ScrollArea() override;

    // lib_ui API: set the owned inner widget.
    template <typename T>
    void setOwnedWidget(object_ptr<T> widget) {
        auto *w = widget.data();
        QScrollArea::setWidget(w);
        if (w) {
            w->setAutoFillBackground(false);
        }
    }

    // lib_ui API: scroll to a specific vertical position.
    void scrollToY(int y) {
        verticalScrollBar()->setValue(y);
    }

    // lib_ui API: get the current scroll position.
    [[nodiscard]] int scrollTop() const {
        return verticalScrollBar()->value();
    }

    // lib_ui API: get the maximum scroll position.
    [[nodiscard]] int scrollTopMax() const {
        return verticalScrollBar()->maximum();
    }

    [[nodiscard]] int scrollHeight() const;

    void updateBars();

protected:
    void resizeEvent(QResizeEvent *e) override;
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    ScrollBar *_verticalBar = nullptr;
};

} // namespace Ui

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "scroll_area.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QApplication>

#include "styles/style_constants.h"
#include "ui/painter.h"

namespace Ui {

namespace {

/// Linearly interpolate between two colors by t (0..1).
QColor lerpColor(const QColor &a, const QColor &b, qreal t) {
    return QColor(
        int(a.red()   + t * (b.red()   - a.red())),
        int(a.green() + t * (b.green() - a.green())),
        int(a.blue()  + t * (b.blue()  - a.blue())),
        int(a.alpha() + t * (b.alpha() - a.alpha())));
}

/// Advance an animation value toward target, return true if still animating.
bool advanceAnimation(
    qreal &value,
    qreal target,
    QElapsedTimer &timer,
    int durationMs)
{
    if (!timer.isValid()) {
        value = target;
        return false;
    }
    const auto elapsed = timer.elapsed();
    if (elapsed >= durationMs) {
        value = target;
        timer.invalidate();
        return false;
    }
    const auto progress = qreal(elapsed) / qreal(durationMs);
    // Linear interpolation from start toward target.
    // start = (target == 1) ? 0 : 1, since we animate 0→1 or 1→0.
    const auto start = (target >= 0.5) ? 0.0 : 1.0;
    value = start + progress * (target - start);
    return true;
}

} // namespace

// ─── ScrollBar ───────────────────────────────────────────────

ScrollBar::ScrollBar(ScrollArea *parent, bool vertical)
    : QWidget(parent)
    , _vertical(vertical)
    , _hiding(true)
    , _connected(vertical
        ? parent->verticalScrollBar()
        : parent->horizontalScrollBar())
    , _scrollMax(_connected->maximum())
{
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setMouseTracking(true);
    recountSize();

    connect(_connected, &QAbstractSlider::valueChanged, this, [this] {
        updateBar();
    });
    connect(_connected, &QAbstractSlider::rangeChanged, this, [this] {
        updateBar();
    });

    // Single animation timer drives all three animations.
    _animTimer.setInterval(16); // ~60fps
    connect(&_animTimer, &QTimer::timeout, this, [this] {
        bool any = false;
        any |= advanceAnimation(_opacityValue, _opacityTarget, _opacityAnimTime, kDuration);
        any |= advanceAnimation(_overValue, _overTarget, _overAnimTime, kDuration);
        any |= advanceAnimation(_barOverValue, _barOverTarget, _barOverAnimTime, kDuration);
        update();
        if (!any) {
            _animTimer.stop();
            if (_opacityValue == 0.0) {
                // Fully faded out — don't hide, keep widget visible
                // for mouse enter detection when scrollbar region is hovered.
            }
        }
    });

    connect(&_hideTimer, &QTimer::timeout, this, &ScrollBar::hideTimerTick);
    _hideTimer.setSingleShot(true);

    updateBar();
}

void ScrollBar::recountSize() {
    setGeometry(
        area()->width() - kWidth,
        kDeltaT,
        kWidth,
        area()->height() - kDeltaT - kDeltaB);
}

void ScrollBar::updateBar(bool force) {
    if (_connected->maximum() != _scrollMax) {
        _scrollMax = _connected->maximum();
    }

    const auto sh = area()->scrollHeight();
    const auto rh = height();
    auto h = sh ? int((rh * qint64(area()->height())) / sh) : 0;

    if (h >= rh || !area()->scrollTopMax() || rh < kMinHeight) {
        if (!isHidden()) hide();
        return;
    }

    if (h < kMinHeight) h = kMinHeight;

    const auto stm = area()->scrollTopMax();
    const auto y = stm
        ? qMin(int(((rh - h) * qint64(area()->scrollTop())) / stm), rh - h)
        : 0;

    const auto newBar = QRect(kDeltaX, y, width() - 2 * kDeltaX, h);
    if (newBar != _bar || force) {
        _bar = newBar;
        update();
    }
    if (isHidden()) show();
}

void ScrollBar::paintEvent(QPaintEvent *) {
    if (!_bar.width() && !_bar.height()) {
        return;
    }
    auto opacity = _opacityValue;
    if (_hiding && !_opacityAnimTime.isValid()) {
        opacity = 0.0;
    }
    if (opacity <= 0.001) return;

    QPainter p(this);
    p.setPen(Qt::NoPen);

    // Blend colors based on hover state.
    auto bg = lerpColor(
        st::historyScrollBg,
        st::historyScrollBgOver,
        _overValue);
    bg.setAlpha(int(bg.alpha() * opacity));

    auto bar = lerpColor(
        st::historyScrollBarBg,
        st::historyScrollBarBgOver,
        _barOverValue);
    bar.setAlpha(int(bar.alpha() * opacity));

    // Draw with rounded rects.
    const auto outer = QRect(
        kDeltaX, 0,
        width() - 2 * kDeltaX,
        height());

    {
        PainterHighQualityEnabler hq(p);
        p.setBrush(bg);
        p.drawRoundedRect(outer, kRound, kRound);
        p.setBrush(bar);
        p.drawRoundedRect(_bar, kRound, kRound);
    }
}

void ScrollBar::hideTimeout(int ms) {
    if (_hiding && ms > 0) {
        _hiding = false;
        startOpacityAnimation(0.0, 1.0);
    }
    _hideIn = ms;
    if (!_moving && ms > 0) {
        _hideTimer.start(ms);
    }
}

void ScrollBar::hideTimerTick() {
    if (!_hiding) {
        _hiding = true;
        startOpacityAnimation(1.0, 0.0);
    }
}

ScrollArea *ScrollBar::area() {
    return static_cast<ScrollArea *>(parentWidget());
}

void ScrollBar::setOver(bool over) {
    if (_over != over) {
        auto wasOver = (_over || _moving);
        _over = over;
        auto nowOver = (_over || _moving);
        if (wasOver != nowOver) {
            startOverAnimation(nowOver ? 0.0 : 1.0, nowOver ? 1.0 : 0.0);
        }
        if (nowOver && _hiding) {
            _hiding = false;
            startOpacityAnimation(_opacityValue, 1.0);
        }
    }
}

void ScrollBar::setOverBar(bool overbar) {
    if (_overbar != overbar) {
        auto wasBarOver = (_overbar || _moving);
        _overbar = overbar;
        auto nowBarOver = (_overbar || _moving);
        if (wasBarOver != nowBarOver) {
            startBarOverAnimation(
                nowBarOver ? 0.0 : 1.0,
                nowBarOver ? 1.0 : 0.0);
        }
    }
}

void ScrollBar::setMoving(bool moving) {
    if (_moving != moving) {
        auto wasOver = (_over || _moving);
        auto wasBarOver = (_overbar || _moving);
        _moving = moving;
        auto nowBarOver = (_overbar || _moving);
        if (wasBarOver != nowBarOver) {
            startBarOverAnimation(
                nowBarOver ? 0.0 : 1.0,
                nowBarOver ? 1.0 : 0.0);
        }
        auto nowOver = (_over || _moving);
        if (wasOver != nowOver) {
            startOverAnimation(nowOver ? 0.0 : 1.0, nowOver ? 1.0 : 0.0);
        }
        if (!nowOver && _hideIn > 0 && !_hiding) {
            _hideTimer.start(_hideIn);
        }
    }
}

void ScrollBar::startOpacityAnimation(qreal from, qreal to) {
    _opacityValue = from;
    _opacityTarget = to;
    _opacityAnimTime.restart();
    if (!_animTimer.isActive()) _animTimer.start();
}

void ScrollBar::startOverAnimation(qreal from, qreal to) {
    _overValue = from;
    _overTarget = to;
    _overAnimTime.restart();
    if (!_animTimer.isActive()) _animTimer.start();
}

void ScrollBar::startBarOverAnimation(qreal from, qreal to) {
    _barOverValue = from;
    _barOverTarget = to;
    _barOverAnimTime.restart();
    if (!_animTimer.isActive()) _animTimer.start();
}

void ScrollBar::enterEvent(QEnterEvent *) {
    _hideTimer.stop();
    setOver(true);
}

void ScrollBar::leaveEvent(QEvent *) {
    if (!_moving) {
        setMouseTracking(true); // keep tracking for bar detection
    }
    setOver(false);
    setOverBar(false);
    if (_hideIn > 0 && !_hiding) {
        _hideTimer.start(_hideIn);
    }
}

void ScrollBar::mouseMoveEvent(QMouseEvent *e) {
    setOverBar(_bar.contains(e->pos()));
    if (_moving) {
        int delta = 0;
        const auto barDelta = _vertical
            ? (height() - _bar.height())
            : (width() - _bar.width());
        if (barDelta > 0) {
            const auto d = e->globalPosition().toPoint() - _dragStart;
            delta = _vertical
                ? int((d.y() * qint64(area()->scrollTopMax())) / barDelta)
                : int((d.x() * qint64(area()->scrollTopMax())) / barDelta);
        }
        _connected->setValue(_startFrom + delta);
    }
}

void ScrollBar::mousePressEvent(QMouseEvent *e) {
    if (!width() || !height()) return;
    _dragStart = e->globalPosition().toPoint();
    setMoving(true);
    if (_overbar) {
        _startFrom = _connected->value();
    } else {
        // Click on track: jump to position.
        const auto val = _vertical ? e->pos().y() : e->pos().x();
        const auto div = _vertical ? height() : width();
        const auto adjusted = qMax(0, val - kDeltaT);
        const auto range = qMax(1, div - kDeltaT - kDeltaB);
        _startFrom = int((adjusted * qint64(area()->scrollTopMax())) / range);
        _connected->setValue(_startFrom);
        setOverBar(true);
    }
}

void ScrollBar::mouseReleaseEvent(QMouseEvent *) {
    if (_moving) {
        setMoving(false);
    }
}

void ScrollBar::resizeEvent(QResizeEvent *) {
    updateBar();
}

void ScrollBar::wheelEvent(QWheelEvent *e) {
    // Forward wheel events to the scroll area's scrollbar.
    QApplication::sendEvent(_connected, e);
}

// ─── ScrollArea ──────────────────────────────────────────────

ScrollArea::ScrollArea(QWidget *parent)
    : QScrollArea(parent)
{
    // Deliberately NOT widgetResizable. The inner widget manages its own
    // size via resize(), so scrollbar range is immediately valid after
    // content changes.
    setWidgetResizable(false);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::NoFrame);
    viewport()->setAutoFillBackground(false);

    _verticalBar = new ScrollBar(this, true);
    _verticalBar->updateBar(true);
}

ScrollArea::~ScrollArea() = default;

int ScrollArea::scrollHeight() const {
    auto *w = widget();
    return w ? qMax(w->height(), height()) : height();
}

void ScrollArea::updateBars() {
    _verticalBar->updateBar(true);
}

void ScrollArea::resizeEvent(QResizeEvent *e) {
    QScrollArea::resizeEvent(e);
    // With widgetResizable(false), manually keep widget width
    // in sync with viewport.
    if (auto *w = widget()) {
        if (w->width() != viewport()->width()) {
            w->resize(viewport()->width(), w->height());
        }
    }
    _verticalBar->recountSize();
}

void ScrollArea::enterEvent(QEnterEvent *e) {
    _verticalBar->hideTimeout(ScrollBar::kHiding);
    QScrollArea::enterEvent(e);
}

void ScrollArea::leaveEvent(QEvent *e) {
    _verticalBar->hideTimeout(0);
    QScrollArea::leaveEvent(e);
}

} // namespace Ui

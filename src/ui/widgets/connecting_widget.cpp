// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "connecting_widget.h"

#include "connecting_radial.h"
#include "../../styles/style_constants.h"
#include "../style/runtime_scale.h"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>

namespace Ui {

namespace {

// Pill geometry (design px; scaled at use). The reference design's pill is
// 40px, but this app renders ~1.2x larger, so the pill is scaled to ~0.8x here
// (32px) to match it visually; cap == pillH/2.
constexpr int kMargin = 2;        // connectingMargin — shadow room
constexpr int kPillH = 32;        // pill height (40 * ~0.8)
constexpr int kCap = 16;          // cap width == pill radius (semicircular caps)
constexpr int kTextPadL = 14;     // connectingTextPadding.left
constexpr int kTextPadR = 14;     // connectingTextPadding.right
constexpr int kRetryGap = 6;      // connectingRetryLink.padding before the link
constexpr int kSpinnerBox = 16;   // connectingRadial.size
constexpr int kSpinnerThickness = 2; // connectingRadial.thickness
constexpr int kDuration = 150;    // connectingDuration
constexpr int kBottomSkip = 10;   // raise the pill off the window's bottom edge

// Timing: wait a beat before showing, then "Connecting…" for a spell before
// falling back to the retry countdown.
constexpr int kSuppressMs = 1000;   // kConnectingStateDelay
constexpr int kConnectingMs = 4000; // kMinimalWaitingStateDuration
constexpr int kInitialBackoff = 5;  // seconds
constexpr int kMaxBackoff = 30;

int scale(int v) {
    return TeleMatrix::Style::ConvertScale(v);
}

} // namespace

ConnectingWidget::ConnectingWidget(QWidget *parent) : QWidget(parent) {
    setVisible(false);
    setMouseTracking(true); // hover-track the "Try now" link

    _phaseTimer = new QTimer(this);
    _phaseTimer->setSingleShot(true);
    connect(_phaseTimer, &QTimer::timeout, this, [this] {
        if (_phase == Phase::Suppressing) {
            enterConnecting();
        } else if (_phase == Phase::Connecting) {
            enterReconnecting();
        }
    });

    _countdownTimer = new QTimer(this);
    _countdownTimer->setInterval(1000);
    connect(_countdownTimer, &QTimer::timeout, this, [this] { onCountdownTick(); });

    _spinnerTimer = new QTimer(this);
    _spinnerTimer->setInterval(16); // ~60fps while visible
    connect(_spinnerTimer, &QTimer::timeout, this, [this] { update(); });

    _visibilityAnim = new QVariantAnimation(this);
    _visibilityAnim->setDuration(kDuration);
    _visibilityAnim->setEasingCurve(QEasingCurve::OutCirc);
    connect(_visibilityAnim, &QVariantAnimation::valueChanged, this,
        [this](const QVariant &v) {
            _visibility = v.toReal();
            applyGeometry();
        });
    connect(_visibilityAnim, &QVariantAnimation::finished, this, [this] {
        if (_visibility <= 0.0) {
            setVisible(false);
            setSpinnerRunning(false);
        }
    });

    _widthAnim = new QVariantAnimation(this);
    _widthAnim->setDuration(kDuration);
    _widthAnim->setEasingCurve(QEasingCurve::OutCirc);
    connect(_widthAnim, &QVariantAnimation::valueChanged, this,
        [this](const QVariant &v) {
            _contentWidth = v.toReal();
            applyGeometry();
        });
}

void ConnectingWidget::setConnected(bool connected) {
    if (_connected == connected) {
        return;
    }
    _connected = connected;
    if (connected) {
        _phaseTimer->stop();
        _countdownTimer->stop();
        _backoffSeconds = kInitialBackoff;
        _phase = Phase::Hidden;
        slideTo(false);
    } else {
        // Wait a beat before showing, so a brief reconnect doesn't flash.
        _phase = Phase::Suppressing;
        _phaseTimer->start(kSuppressMs);
    }
}

void ConnectingWidget::enterConnecting() {
    _phase = Phase::Connecting;
    _countdownTimer->stop();
    setSpinnerRunning(true);
    if (!isVisible()) {
        _contentWidth = contentWidthFor(); // appear at full width
        setVisible(true);
        applyGeometry();
        slideTo(true);
    } else {
        retargetWidth();
    }
    _phaseTimer->start(kConnectingMs);
}

void ConnectingWidget::enterReconnecting() {
    _phase = Phase::Reconnecting;
    _retrySeconds = _backoffSeconds;
    retargetWidth();
    update();
    _countdownTimer->start();
}

void ConnectingWidget::onCountdownTick() {
    if (--_retrySeconds > 0) {
        retargetWidth(); // width only changes when the digit count does
        update();
        return;
    }
    // Wait elapsed — retry now and grow the backoff for the next cycle.
    _countdownTimer->stop();
    _backoffSeconds = std::min(_backoffSeconds * 2, kMaxBackoff);
    emit retryRequested();
    enterConnecting();
}

QString ConnectingWidget::labelText() const {
    return (_phase == Phase::Reconnecting)
        ? tr("Reconnect in %1 s…").arg(_retrySeconds)
        : tr("Connecting…");
}

int ConnectingWidget::contentWidthFor() const {
    const QFontMetrics fm(static_cast<const QFont &>(st::normalFont));
    auto w = scale(kTextPadL) + fm.horizontalAdvance(labelText());
    if (showLink()) {
        w += scale(kRetryGap) + fm.horizontalAdvance(tr("Try now"));
    }
    return w + scale(kTextPadR);
}

void ConnectingWidget::retargetWidth() {
    const auto target = qreal(contentWidthFor());
    if (qFuzzyCompare(_contentWidth + 1.0, target + 1.0)) {
        return;
    }
    _widthAnim->stop();
    _widthAnim->setStartValue(_contentWidth);
    _widthAnim->setEndValue(target);
    _widthAnim->start();
}

void ConnectingWidget::slideTo(bool visible) {
    _visibilityAnim->stop();
    _visibilityAnim->setStartValue(_visibility);
    _visibilityAnim->setEndValue(visible ? 1.0 : 0.0);
    _visibilityAnim->start();
}

void ConnectingWidget::reposition() {
    applyGeometry();
}

void ConnectingWidget::applyGeometry() {
    if (!parentWidget()) {
        return;
    }
    const auto m = scale(kMargin);
    const auto cap = scale(kCap);
    const auto totalH = scale(kPillH) + 2 * m;
    const auto totalW = 2 * m + 2 * cap + int(std::lround(_contentWidth));
    const auto ph = parentWidget()->height();
    const auto yShown = ph - totalH - scale(kBottomSkip);
    const auto yHidden = ph - m; // slid down: only the top margin peeks
    const auto y = int(std::lround(yHidden + (yShown - yHidden) * _visibility));
    setGeometry(_leftOffset, y, totalW, totalH);
    update();
}

void ConnectingWidget::setSpinnerRunning(bool running) {
    if (running) {
        if (!_spinnerClock.isValid()) {
            _spinnerClock.start();
        }
        if (!_spinnerTimer->isActive()) {
            _spinnerTimer->start();
        }
    } else {
        _spinnerTimer->stop();
    }
}

void ConnectingWidget::mouseMoveEvent(QMouseEvent *e) {
    const auto over = _linkRect.contains(e->pos());
    if (over != _linkHovered) {
        _linkHovered = over;
        setCursor(over ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void ConnectingWidget::leaveEvent(QEvent *) {
    if (_linkHovered) {
        _linkHovered = false;
        setCursor(Qt::ArrowCursor);
        update();
    }
}

void ConnectingWidget::mousePressEvent(QMouseEvent *e) {
    if (showLink() && _linkRect.contains(e->pos())) {
        emit retryRequested();
        enterConnecting(); // "Try now" — skip the wait
    }
}

void ConnectingWidget::paintEvent(QPaintEvent *) {
    if (_phase == Phase::Hidden && _visibility <= 0.0) {
        return;
    }
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const auto m = scale(kMargin);
    const auto cap = scale(kCap);
    const auto pillH = scale(kPillH);
    const QRectF pill(m, m, width() - 2 * m, pillH);
    const auto radius = pill.height() / 2.0;

    // Soft drop shadow (windowShadowFg) inside the 2px margin: layered translucent
    // capsules fake the pre-rendered shadow PNGs' gaussian falloff.
    p.setPen(Qt::NoPen);
    for (int i = 0; i < 4; ++i) {
        auto shadow = st::windowShadowFg;
        shadow.setAlphaF(0.05);
        p.setBrush(shadow);
        const auto grow = i * (m / 3.0);
        const QRectF sr = pill.adjusted(-grow, -grow + 0.7, grow, grow + 0.7);
        p.drawRoundedRect(sr, sr.height() / 2.0, sr.height() / 2.0);
    }

    // Capsule.
    p.setBrush(st::windowBg);
    p.drawRoundedRect(pill, radius, radius);

    // Radial spinner, in the left cap.
    const auto spinnerBox = scale(kSpinnerBox);
    const auto spinnerX = m + cap - spinnerBox / 2;
    const auto spinnerY = m + (pillH - spinnerBox) / 2;
    const auto shift = scale(1); // thickness - thickness/2
    const QRect arcRect(spinnerX + shift, spinnerY + shift,
        spinnerBox - 2 * shift, spinnerBox - 2 * shift);
    DrawConnectingRadial(p, arcRect, scale(kSpinnerThickness), st::menuIconFg,
        _spinnerClock.isValid() ? _spinnerClock.elapsed() : 0);

    // Status text + (in the Reconnecting phase) the "Try now" link, clipped to the
    // pill while the body width tweens.
    p.save();
    p.setClipRect(pill.toRect());
    const auto font = static_cast<const QFont &>(st::normalFont);
    const QFontMetrics fm(font);
    const auto textX = m + cap + scale(kTextPadL);
    const auto label = labelText();
    const auto textW = fm.horizontalAdvance(label);
    p.setFont(font);
    p.setPen(st::windowSubTextFg);
    p.drawText(QRect(textX, m, textW, pillH), Qt::AlignVCenter | Qt::AlignLeft, label);

    if (showLink()) {
        const auto link = tr("Try now");
        const auto linkX = textX + textW + scale(kRetryGap);
        const auto linkW = fm.horizontalAdvance(link);
        auto linkFont = font;
        linkFont.setUnderline(_linkHovered);
        p.setFont(linkFont);
        p.setPen(st::windowActiveTextFg);
        p.drawText(QRect(linkX, m, linkW, pillH), Qt::AlignVCenter | Qt::AlignLeft, link);
        _linkRect = QRect(linkX, m, linkW, pillH);
    } else {
        _linkRect = QRect();
    }
    p.restore();
}

} // namespace Ui

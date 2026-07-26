// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "ui/toast_widget.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>

#include "styles/style_constants.h"

namespace Ui {

namespace {
// Content padding: 19px left, 13px top, 19px right, 12px bottom
constexpr int kPadLeft = 19;
constexpr int kPadTop = 13;
constexpr int kPadRight = 19;
constexpr int kPadBottom = 12;

// Corner radius: 6px
constexpr int kRadius = 6;

// Fade-in 200ms, visible 1500ms, fade-out 1000ms
constexpr int kFadeInMs = 200;
constexpr int kVisibleMs = 1500;
constexpr int kFadeOutMs = 1000;

// Bottom margin from parent edge (13px).
constexpr int kBottomMargin = 13;
} // namespace

ToastWidget::ToastWidget(QWidget *parent)
    : QWidget(parent)
    , _fadeIn(this, "shownLevel")
    , _fadeOut(this, "shownLevel")
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_TranslucentBackground);

    _fadeIn.setDuration(kFadeInMs);
    _fadeIn.setStartValue(0.0);
    _fadeIn.setEndValue(1.0);

    _fadeOut.setDuration(kFadeOutMs);
    _fadeOut.setStartValue(1.0);
    _fadeOut.setEndValue(0.0);

    _hideTimer.setSingleShot(true);
    connect(&_hideTimer, &QTimer::timeout, this, &ToastWidget::startFadeOut);
    connect(&_fadeOut, &QPropertyAnimation::finished, this, &QWidget::hide);
}

void ToastWidget::showToast(const QString &text) {
    _text = text;

    // Stop any running animations.
    _fadeIn.stop();
    _fadeOut.stop();
    _hideTimer.stop();

    // Size to fit text.
    QFontMetrics fm(static_cast<const QFont &>(st::normalFont));
    const int textW = fm.horizontalAdvance(_text);
    const int w = kPadLeft + textW + kPadRight;
    const int h = kPadTop + fm.height() + kPadBottom;
    resize(w, h);

    positionInParent();
    show();
    raise();

    _shownLevel = 0.0;
    _fadeIn.start();
    _hideTimer.start(kFadeInMs + kVisibleMs);
}

void ToastWidget::startFadeOut() {
    _fadeOut.start();
}

void ToastWidget::setShownLevel(qreal level) {
    _shownLevel = level;
    update();
}

void ToastWidget::positionInParent() {
    // Center in the top-level window.
    auto *win = window();
    if (!win) return;
    const auto globalCenter = win->geometry().center();
    const auto local = parentWidget()->mapFromGlobal(globalCenter);
    move(local.x() - width() / 2, local.y() - height() / 2);
}

void ToastWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setOpacity(_shownLevel);
    p.setRenderHint(QPainter::Antialiasing);

    // Background.
    p.setPen(Qt::NoPen);
    p.setBrush(st::toastBg);
    p.drawRoundedRect(rect(), kRadius, kRadius);

    // Text.
    p.setFont(st::normalFont);
    p.setPen(st::toastFg);
    p.drawText(
        kPadLeft,
        kPadTop + QFontMetrics(static_cast<const QFont &>(st::normalFont)).ascent(),
        _text);
}

void ToastWidget::showTransient(const QString &text) {
    // One-shot: remove ourselves once the fade-out finishes.
    connect(&_fadeOut, &QPropertyAnimation::finished, this, &QObject::deleteLater);
    showToast(text);
}

void ShowToast(const QString &text) {
    auto *win = QApplication::activeWindow();
    if (!win) {
        const auto tops = QApplication::topLevelWidgets();
        for (auto *w : tops) {
            if (w->isWindow() && w->isVisible()) {
                win = w;
                break;
            }
        }
    }
    if (!win) {
        return;
    }
    // Parented to the window; deletes itself after fading out (showTransient).
    auto *toast = new ToastWidget(win);
    toast->showTransient(text);
}

} // namespace Ui

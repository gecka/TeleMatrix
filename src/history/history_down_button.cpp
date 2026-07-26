// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_down_button.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>

#include "ui/painter.h"
#include "styles/style_constants.h"

namespace TeleMatrix {

namespace {

// Button geometry: 52x62. The circle + shadow are 44px (rippleAreaSize),
// with 10px extra top space for badge clearance.
constexpr int kCircleSize = 44;
constexpr int kCircleLeft = 4;  // (52 - 44) / 2
constexpr int kCircleTop = 14;  // 4 + historyToDownPaddingTop(10)

// Unread badge.
constexpr int kBadgeHeight = 22;
constexpr int kBadgePadding = 5;

} // namespace

HistoryDownButton::HistoryDownButton(QWidget *parent)
    : QWidget(parent)
{
    resize(kWidth, kHeight);
    setCursor(Qt::PointingHandCursor);
    // No opaque background — button area outside circle must be transparent.
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    hide();
}

void HistoryDownButton::setUnreadCount(int count) {
    if (_unreadCount != count) {
        _unreadCount = count;
        update();
    }
}

void HistoryDownButton::setJumpToLatestMode(bool jumpToLatest) {
    if (_jumpToLatest != jumpToLatest) {
        _jumpToLatest = jumpToLatest;
        update();
    }
}

void HistoryDownButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const auto cx = kCircleLeft;
    const auto cy = kCircleTop;

    // Layer 1: Shadow (offset 1px down).
    {
        p.setPen(Qt::NoPen);
        p.setBrush(st::historyToDownShadow);
        p.drawEllipse(cx, cy + 1, kCircleSize, kCircleSize);
    }

    // Layer 2: Circle background.
    {
        const auto &bg = _over
            ? st::historyToDownBgOver
            : st::historyToDownBg;
        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        p.drawEllipse(cx, cy, kCircleSize, kCircleSize);
    }

    // Layer 3: Chevron arrow.
    // When in jump-to-latest mode the arrow points UP (↑) so the user
    // understands tapping will return to the live end of the timeline.
    {
        const auto &fg = _over
            ? st::historyToDownFgOver
            : st::historyToDownFg;
        p.setPen(QPen(fg, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);

        const auto centerX = cx + kCircleSize / 2.0;
        const auto centerY = cy + kCircleSize / 2.0;

        // Arrow always points DOWN.
        // The button means "go to latest" in both live and focused mode.
        QPainterPath path;
        path.moveTo(centerX - 8, centerY - 3);
        path.lineTo(centerX, centerY + 5);
        path.lineTo(centerX + 8, centerY - 3);
        p.drawPath(path);
    }

    // Layer 4: Unread badge (painted at top).
    if (_unreadCount > 0) {
        const auto text = QString::number(_unreadCount);
        const auto font = static_cast<const QFont &>(st::dialogsUnreadFont);
        const QFontMetrics fm(font);
        const auto textWidth = fm.horizontalAdvance(text);
        const auto badgeWidth = qMax(kBadgeHeight, textWidth + 2 * kBadgePadding);
        const auto badgeLeft = (width() - badgeWidth) / 2;
        const auto badgeTop = 0;
        const auto radius = kBadgeHeight / 2;

        p.setPen(Qt::NoPen);
        p.setBrush(st::dialogsUnreadBg);
        p.drawRoundedRect(
            badgeLeft, badgeTop, badgeWidth, kBadgeHeight,
            radius, radius);

        p.setFont(font);
        p.setPen(st::dialogsUnreadFg);
        p.drawText(
            badgeLeft + (badgeWidth - textWidth) / 2,
            badgeTop + (kBadgeHeight - fm.height()) / 2 + fm.ascent(),
            text);
    }
}

void HistoryDownButton::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) {
        _pressed = true;
    }
}

void HistoryDownButton::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && _pressed) {
        _pressed = false;
        if (rect().contains(e->pos())) {
            emit clicked();
        }
    }
}

void HistoryDownButton::enterEvent(QEnterEvent *) {
    _over = true;
    update();
}

void HistoryDownButton::leaveEvent(QEvent *) {
    _over = false;
    _pressed = false;
    update();
}

} // namespace TeleMatrix

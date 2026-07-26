// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "ui/widgets/close_button.h"

#include "styles/style_constants.h"
#include "ui/style/icon_provider.h"

#include <QMouseEvent>
#include <QPainter>

namespace Ui {

namespace {

QImage closeIcon(const QColor &color) {
    return TeleMatrix::Style::IconProvider::tintedIcon(
        QStringLiteral(":/settings_icons/info_close"), QString(), color);
}

QSize iconLogicalSize(const QImage &icon, int fallback) {
    if (icon.isNull()) {
        return QSize(fallback, fallback);
    }
    const auto dpr = icon.devicePixelRatio();
    return QSize(qRound(icon.width() / dpr), qRound(icon.height() / dpr));
}

} // namespace

CloseButton::CloseButton(QWidget *parent)
    : QWidget(parent)
    , _icon(closeIcon(st::settingsCloseIconFg))
    , _iconOver(closeIcon(st::settingsCloseIconFgOver)) {
    setFixedSize(st::settingsCloseButtonSize, st::boxTitleHeight);
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
}

void CloseButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const auto &icon = _hovered ? _iconOver : _icon;
    const auto iconSize = iconLogicalSize(icon, st::userProfileCloseButtonSize);
    const auto topLeft = QPoint(
        st::userProfileCloseIconLeft,
        (height() - iconSize.height()) / 2);
    if (!icon.isNull()) {
        p.drawImage(topLeft, icon);
        return;
    }
    // Fallback vector × if the asset is missing.
    p.setPen(QPen(_hovered ? st::boxTitleCloseFgOver : st::boxTitleCloseFg, 2));
    const auto s = st::userProfileCloseButtonSize;
    p.drawLine(topLeft, topLeft + QPoint(s, s));
    p.drawLine(topLeft + QPoint(s, 0), topLeft + QPoint(0, s));
}

void CloseButton::enterEvent(QEnterEvent *) {
    _hovered = true;
    update();
}

void CloseButton::leaveEvent(QEvent *) {
    _hovered = false;
    update();
}

void CloseButton::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && isEnabled()) {
        Q_EMIT clicked();
    }
}

} // namespace TeleMatrix::Ui

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "settings/encryption/encryption_action_row.h"

#include "styles/style_constants.h"

#include <QMouseEvent>
#include <QPainter>

namespace TeleMatrix {

EncryptionActionRow::EncryptionActionRow(
        const QString &text,
        const QColor &color,
        QWidget *parent)
    : QWidget(parent)
    , _text(text)
    , _color(color) {
    setFixedHeight(st::settingsButtonHeight);
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
}

void EncryptionActionRow::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), _hovered ? st::windowBgOver : st::windowBg);
    p.setFont(st::baseFont(14));
    p.setPen(_color);
    const auto metrics = QFontMetrics(st::baseFont(14));
    p.drawText(
        st::settingsButtonPaddingLeft,
        (height() - metrics.height()) / 2 + metrics.ascent(),
        _text);
}

void EncryptionActionRow::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        Q_EMIT clicked();
        return;
    }
    QWidget::mousePressEvent(event);
}

void EncryptionActionRow::enterEvent(QEnterEvent *) {
    _hovered = true;
    update();
}

void EncryptionActionRow::leaveEvent(QEvent *) {
    _hovered = false;
    update();
}

} // namespace TeleMatrix

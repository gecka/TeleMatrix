// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "buttons.h"

#include "ui/painter.h"

#include <QFontMetrics>
#include <QPainter>

namespace Ui {

IconButton::~IconButton() = default;

TextButton::TextButton(const QString &text, const Style &style, QWidget *parent)
    : QAbstractButton(parent)
    , _style(style)
{
    setText(text);
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
    if (_style.height > 0) {
        setFixedHeight(_style.height);
    }
}

TextButton::~TextButton() = default;

void TextButton::setButtonStyle(const Style &style) {
    _style = style;
    if (_style.height > 0) {
        setFixedHeight(_style.height);
    }
    updateGeometry();
    update();
}

QSize TextButton::sizeHint() const {
    const QFontMetrics fm(font());
    const int w = fm.horizontalAdvance(text()) + 2 * _style.paddingH;
    const int h = (_style.height > 0) ? _style.height : (fm.height() + 8);
    return QSize(w, h);
}

void TextButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    PainterHighQualityEnabler hq(p);

    // Dim the whole button when disabled (e.g. while a confirm action runs).
    if (!isEnabled()) {
        p.setOpacity(0.5);
    }

    const QColor *fill = (_hovered && _style.bgOver) ? _style.bgOver : _style.bg;
    if (fill && fill->alpha() > 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(*fill);
        if (_style.radius > 0) {
            p.drawRoundedRect(rect(), _style.radius, _style.radius);
        } else {
            p.fillRect(rect(), *fill);
        }
    }
    if (_style.fg) {
        p.setPen(*_style.fg);
        p.drawText(rect(), Qt::AlignCenter, text());
    }
}

void TextButton::enterEvent(QEnterEvent *) {
    _hovered = true;
    update();
}

void TextButton::leaveEvent(QEvent *) {
    _hovered = false;
    update();
}

} // namespace Ui

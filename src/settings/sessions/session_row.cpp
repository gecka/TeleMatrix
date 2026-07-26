// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "settings/sessions/session_row.h"

#include "styles/style_constants.h"
#include "ui/widgets/buttons.h"

#include <QFont>
#include <QPainter>
#include <QResizeEvent>

namespace TeleMatrix {

SessionRow::SessionRow(
        const DeviceSession &session,
        const QString &subtitle,
        QWidget *parent)
    : QWidget(parent)
    , _deviceName(session.displayName.isEmpty()
        ? session.deviceId
        : session.displayName)
    , _deviceId(session.deviceId)
    , _subtitle(subtitle)
    , _verified(session.verificationState == DeviceVerificationState::Verified)
    , _unverifiable(session.verificationState == DeviceVerificationState::Unverifiable) {
    setFixedHeight(st::settingsSessionRowHeight);

    auto buttonFont = st::baseFont(12);

    ::Ui::TextButton::Style renameStyle;
    renameStyle.bgOver = &st::settingsButtonBgOver;  // transparent until hovered
    renameStyle.fg = &st::windowActiveTextFg;
    renameStyle.radius = 4;
    _renameButton = new ::Ui::TextButton(tr("Rename"), renameStyle, this);
    _renameButton->setFont(buttonFont);
    _renameButton->setFixedSize(60, 26);
    connect(_renameButton, &QAbstractButton::clicked, this, [this] {
        Q_EMIT renameRequested(_deviceId, _deviceName);
    });

    ::Ui::TextButton::Style signOutStyle;
    signOutStyle.bgOver = &st::attentionButtonBgOver;  // transparent until hovered
    signOutStyle.fg = &st::attentionButtonFg;
    signOutStyle.radius = 4;
    _signOutButton = new ::Ui::TextButton(tr("Sign out"), signOutStyle, this);
    _signOutButton->setFont(buttonFont);
    _signOutButton->setFixedSize(72, 26);
    connect(_signOutButton, &QAbstractButton::clicked, this, [this] {
        Q_EMIT signOutRequested(_deviceId);
    });
}

void SessionRow::positionButtons() {
    if (_signOutButton) {
        _signOutButton->move(
            width() - st::settingsButtonPaddingRight - _signOutButton->width(),
            (height() - _signOutButton->height()) / 2);
    }
    if (_renameButton && _signOutButton) {
        _renameButton->move(
            _signOutButton->x() - _renameButton->width() - 2,
            (height() - _renameButton->height()) / 2);
    }
}

void SessionRow::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), st::windowBg);

    const int left = st::settingsButtonPaddingLeft;
    const int right = st::settingsButtonPaddingRight;

    const int dotSize = st::settingsVerifiedDotSize;
    const int dotX = left;
    const int dotY = (height() - dotSize) / 2;
    p.setPen(Qt::NoPen);
    if (_unverifiable) {
        p.setBrush(st::windowSubTextFg);
    } else {
        p.setBrush(_verified ? st::settingsVerifiedDotColor : st::attentionButtonFg);
    }
    p.drawEllipse(dotX, dotY, dotSize, dotSize);

    const int textLeft = dotX + dotSize + 10;

    p.setFont(st::baseFont(14, true));
    p.setPen(st::windowBoldFg);
    const int nameY = 12 + QFontMetrics(st::baseFont(14, true)).ascent();
    p.drawText(textLeft, nameY, _deviceName);

    const auto metaFont = st::baseFont(13);
    p.setFont(metaFont);
    p.setPen(st::windowSubTextFg);
    const int metaY = nameY + 5 + QFontMetrics(metaFont).ascent();
    const int availW = width() - textLeft - right - 148;
    const QString elidedSubtitle = QFontMetrics(metaFont).elidedText(
        _subtitle,
        Qt::ElideRight,
        availW);
    p.drawText(textLeft, metaY, elidedSubtitle);

    p.setPen(Qt::NoPen);
    p.setBrush(st::shadowFg);
    p.drawRect(left, height() - 1, width() - left - right, 1);

    positionButtons();
}

void SessionRow::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    positionButtons();
}

} // namespace TeleMatrix

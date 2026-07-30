// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "settings/sessions/current_session_card.h"

#include "styles/style_constants.h"
#include "ui/widgets/buttons.h"

#include <QFont>
#include <QPainter>
#include <QResizeEvent>

namespace TeleMatrix {

CurrentSessionCard::CurrentSessionCard(
        const DeviceSession &session,
        QWidget *parent)
    : QWidget(parent)
    , _deviceName(session.displayName.isEmpty()
        ? session.deviceId
        : session.displayName)
    , _deviceId(session.deviceId)
    , _verified(session.verificationState == DeviceVerificationState::Verified) {
    setFixedHeight(80);

    auto buttonFont = st::baseFont(13);

    ::Ui::TextButton::Style renameStyle;
    renameStyle.bgOver = &st::settingsButtonBgOver;  // transparent until hovered
    renameStyle.fg = &st::windowActiveTextFg;
    renameStyle.radius = 4;
    _renameButton = new ::Ui::TextButton(tr("Rename"), renameStyle, this);
    _renameButton->setFont(buttonFont);
    // Width from the text, not a literal: the 72/80 these used to be were sized
    // for "Rename"/"Sign out" and clipped every longer translation ("Renombrar",
    // "Cerrar sesión"). sizeHint() is textWidth + 2 * paddingH.
    _renameButton->setFixedSize(_renameButton->sizeHint().width(), 28);
    connect(_renameButton, &QAbstractButton::clicked, this, [this] {
        Q_EMIT renameRequested(_deviceId, _deviceName);
    });

    ::Ui::TextButton::Style signOutStyle;
    signOutStyle.bgOver = &st::attentionButtonBgOver;  // transparent until hovered
    signOutStyle.fg = &st::attentionButtonFg;
    signOutStyle.radius = 4;
    _signOutButton = new ::Ui::TextButton(tr("Sign out"), signOutStyle, this);
    _signOutButton->setFont(buttonFont);
    _signOutButton->setFixedSize(_signOutButton->sizeHint().width(), 28);
    connect(_signOutButton, &QAbstractButton::clicked, this, [this] {
        Q_EMIT signOutRequested(_deviceId);
    });
}

void CurrentSessionCard::positionButtons() {
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

void CurrentSessionCard::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), st::windowBg);

    const int left = st::settingsButtonPaddingLeft;

    p.setFont(st::baseFont(14, true));
    p.setPen(st::windowBoldFg);
    const int nameY = 14 + QFontMetrics(st::baseFont(14, true)).ascent();
    p.drawText(left, nameY, _deviceName);

    const auto statusFont = st::baseFont(13);
    p.setFont(statusFont);
    const int statusY = nameY + 6 + QFontMetrics(statusFont).ascent();

    const int dotSize = st::settingsVerifiedDotSize;
    // Centre the dot on the text's cap-height midline (not the ascent midline,
    // which sits noticeably too high next to "Verified").
    const int dotY = statusY - (QFontMetrics(statusFont).capHeight() + dotSize) / 2;
    p.setPen(Qt::NoPen);
    p.setBrush(_verified ? st::settingsVerifiedDotColor : st::attentionButtonFg);
    p.drawEllipse(left, dotY, dotSize, dotSize);

    const int textAfterDot = left + dotSize + 6;
    p.setFont(statusFont);
    p.setPen(_verified ? st::settingsSessionActiveFg : st::attentionButtonFg);
    p.drawText(textAfterDot, statusY, _verified ? tr("Verified") : tr("Unverified"));

    const auto idFont = st::baseFont(12);
    p.setFont(idFont);
    p.setPen(st::windowSubTextFg);
    const int idY = statusY + 5 + QFontMetrics(idFont).ascent();
    p.drawText(left, idY, _deviceId);

    positionButtons();
}

void CurrentSessionCard::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    positionButtons();
}

} // namespace TeleMatrix

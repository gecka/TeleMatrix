// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "intro_verify_success.h"
#include "intro_widget.h"

#include "../styles/style_constants.h"
#include "intro_colors.h"

#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>
#include <QResizeEvent>

namespace TeleMatrix {

IntroVerifySuccess::IntroVerifySuccess(QWidget *parent)
    : IntroStep(parent, false /* hasCover */)
{
    setTitleText(tr("Session Verified"));
    setDescriptionText(tr("This session is now verified. Your encrypted messages are secure."));

    // Center-align title and description (override the default left-align).
    titleLabel()->setAlignment(Qt::AlignHCenter);
    descriptionLabel()->setAlignment(Qt::AlignHCenter);
}

void IntroVerifySuccess::activate() {
    IntroStep::activate();
    nextButton()->setFocus();
}

void IntroVerifySuccess::submit() {
    Q_EMIT goNext();
}

QString IntroVerifySuccess::nextButtonText() const {
    return tr("Continue");
}

void IntroVerifySuccess::paintEvent(QPaintEvent *e) {
    IntroStep::paintEvent(e);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const auto top = contentTop();
    const auto checkSize = st::introVerifyCheckSize;

    // Green circle centered horizontally.
    const auto circleX = (width() - checkSize) / 2;
    const auto circleY = top + 40;

    p.setPen(Qt::NoPen);
    p.setBrush(intro::verifySuccessBg);
    p.drawEllipse(circleX, circleY, checkSize, checkSize);

    // White checkmark inside the circle.
    const auto cx = circleX + checkSize / 2;
    const auto cy = circleY + checkSize / 2;

    QPainterPath checkPath;
    // Checkmark: from bottom-left leg to bottom center, then up to top-right.
    // Coordinates relative to circle center, scaled to circle size.
    const auto scale = checkSize / 64.0;
    checkPath.moveTo(cx - 14 * scale, cy - 1 * scale);
    checkPath.lineTo(cx - 4 * scale, cy + 10 * scale);
    checkPath.lineTo(cx + 14 * scale, cy - 10 * scale);

    p.setPen(QPen(intro::bg, 2.0 * scale, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(checkPath);
}

void IntroVerifySuccess::resizeEvent(QResizeEvent *e) {
    IntroStep::resizeEvent(e);
    updateSuccessLayout();
}

void IntroVerifySuccess::updateSuccessLayout() {
    const auto left = contentLeft();
    const auto top = contentTop();
    const auto checkSize = st::introVerifyCheckSize;

    // Circle is at top + 40, with height checkSize.
    // Title goes below circle with a 20px gap.
    const auto titleY = top + 40 + checkSize + 20;
    titleLabel()->move(left, titleY);
    titleLabel()->setFixedWidth(st::introStepWidth);

    // Description below title with a small gap.
    const auto descY = titleY + titleLabel()->sizeHint().height() + 8;
    descriptionLabel()->move(left, descY);
    descriptionLabel()->setFixedWidth(st::introStepWidth);

    // Center the next button below description.
    const auto nextLeft = (width() - st::introNextButtonWidth) / 2;
    const auto nextY = descY + descriptionLabel()->sizeHint().height() + 32;
    nextButton()->move(nextLeft, nextY);
}

} // namespace TeleMatrix

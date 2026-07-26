// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "intro_start.h"
#include "intro_widget.h"

#include "intro_colors.h"
#include "intro_widgets.h"
#include "styles/style_constants.h"
#include "ui/painter.h"

#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>

namespace TeleMatrix {
namespace {

constexpr int kIconSize = 88;
constexpr int kIconRadius = 22;
constexpr int kColumnWidth = 320;
constexpr int kButtonWidth = 300;
constexpr int kTitleTop = 22;      // below the icon
constexpr int kButtonsTop = 36;    // below the title
constexpr int kButtonGap = 10;
constexpr int kTitleSize = 29;

} // namespace

IntroStart::IntroStart(IntroWidget *parent)
    : IntroStep(parent, false /* hasCover */)
{
    // The wordmark and the buttons are the whole screen; the base class's
    // title/description labels would duplicate them, so they stay empty and the
    // heading is painted with the icon instead.
    _icon = QPixmap(QStringLiteral(":/telematrix/app/icon.png"));

    _signIn = new intro::FilledButton(tr("Sign in"), this);
    _signIn->setFixedWidth(kButtonWidth);
    connect(_signIn, &QPushButton::clicked, this, [this] { Q_EMIT goNext(); });

    _createAccount = new intro::GhostButton(tr("Create account"), this);
    _createAccount->setFixedWidth(kButtonWidth);
    connect(_createAccount, &QPushButton::clicked,
            this, &IntroStart::createAccountRequested);

    // The shared Next button is unused here — this screen has its own pair.
    nextButton()->hide();
}

void IntroStart::activate() {
    IntroStep::activate();
    nextButton()->hide();
    _signIn->setFocus();
}

void IntroStart::submit() {
    Q_EMIT goNext();
}

QString IntroStart::nextButtonText() const {
    return tr("Sign in");
}

int IntroStart::contentHeight() const {
    const QFontMetrics titleMetrics(titleFont());
    return kIconSize
        + kTitleTop + titleMetrics.height()
        + kButtonsTop + intro::metrics::buttonHeight
        + kButtonGap + intro::metrics::buttonHeight;
}

QFont IntroStart::titleFont() const {
    auto font = st::baseFont(kTitleSize);
    font.setWeight(QFont::Bold);
    font.setLetterSpacing(QFont::PercentageSpacing, 97.5); // -.025em
    return font;
}

void IntroStart::resizeEvent(QResizeEvent *e) {
    IntroStep::resizeEvent(e);

    const QFontMetrics titleMetrics(titleFont());
    const auto left = (width() - kColumnWidth) / 2;
    auto y = (height() - contentHeight()) / 2;

    _iconRect = QRect(
        left + (kColumnWidth - kIconSize) / 2, y, kIconSize, kIconSize);
    y += kIconSize + kTitleTop;

    _titleRect = QRect(left, y, kColumnWidth, titleMetrics.height());
    y += titleMetrics.height() + kButtonsTop;

    const auto buttonLeft = left + (kColumnWidth - kButtonWidth) / 2;
    _signIn->move(buttonLeft, y);
    y += intro::metrics::buttonHeight + kButtonGap;
    _createAccount->move(buttonLeft, y);
}

void IntroStart::paintEvent(QPaintEvent *e) {
    IntroStep::paintEvent(e);

    QPainter p(this);
    PainterHighQualityEnabler hq(p);

    if (!_icon.isNull()) {
        // Rounded-square mask, matching the platform app-icon shape rather than
        // the circular avatar treatment used elsewhere in the app.
        QPainterPath clip;
        clip.addRoundedRect(QRectF(_iconRect), kIconRadius, kIconRadius);
        p.save();
        p.setClipPath(clip);
        p.drawPixmap(_iconRect, _icon);
        p.restore();
    }

    p.setFont(titleFont());
    p.setPen(intro::inkHeading);
    p.drawText(_titleRect, Qt::AlignHCenter | Qt::AlignVCenter,
        QStringLiteral("TeleMatrix"));
}

} // namespace TeleMatrix

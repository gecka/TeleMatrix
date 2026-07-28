// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "intro_verify_emoji.h"
#include "intro_widget.h"

#include "../protocol/protocol_bridge.h"
#include "../styles/style_constants.h"
#include "intro_colors.h"
#include "intro_widgets.h"

#include "ui/painter.h"
#include "ui/emoji_sprites.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>

namespace TeleMatrix {

namespace {

// Flat text button with a transparent normal background that highlights its
// background on hover / press (the "They Don't Match" action). Painted with
// live intro:: colors so it tracks theme changes; the text color is fixed in
// the normal/hover/pressed states and dims when disabled.
class IntroOutlineButton : public QPushButton {
public:
    IntroOutlineButton(
        const QColor *fg,
        const QColor *bgOver,
        const QColor *bgPressed,
        const QColor *disabledFg,
        int radius,
        QWidget *parent)
        : QPushButton(parent)
        , _fg(fg)
        , _bgOver(bgOver)
        , _bgPressed(bgPressed)
        , _disabledFg(disabledFg)
        , _radius(radius) {
        setMouseTracking(true);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);

        const QColor *fill = nullptr;
        if (isEnabled()) {
            if (isDown() && _bgPressed) {
                fill = _bgPressed;
            } else if (_hovered && _bgOver) {
                fill = _bgOver;
            }
        }
        if (fill) {
            p.setPen(Qt::NoPen);
            p.setBrush(*fill);
            if (_radius > 0) {
                p.drawRoundedRect(rect(), _radius, _radius);
            } else {
                p.fillRect(rect(), *fill);
            }
        }

        const QColor *text =
            (!isEnabled() && _disabledFg) ? _disabledFg : _fg;
        if (text) {
            p.setPen(*text);
            p.setFont(font());
            p.drawText(rect(), Qt::AlignCenter, this->text());
        }
    }
    void enterEvent(QEnterEvent *) override { _hovered = true; update(); }
    void leaveEvent(QEvent *) override { _hovered = false; update(); }

private:
    const QColor *_fg = nullptr;
    const QColor *_bgOver = nullptr;
    const QColor *_bgPressed = nullptr;
    const QColor *_disabledFg = nullptr;
    int _radius = 0;
    bool _hovered = false;
};


// Resize a wrapping label down to the width its current text actually needs
// (clamped to the window, minus a margin) so it renders on one line instead of
// wherever IntroStep's fixed introStepWidth happens to wrap it, then center it
// at left/top. Returns the label's single-line height.
int sizeLabelToOneLine(QLabel *label, int windowWidth, int top) {
    const QFontMetrics fm(label->font());
    const auto maxWidth = windowWidth - 2 * st::introStepFieldTop;
    const auto contentWidth = fm.horizontalAdvance(label->text()) + 4;
    const auto labelWidth = qMin(contentWidth, maxWidth);
    label->setFixedWidth(labelWidth);
    label->move((windowWidth - labelWidth) / 2, top);
    const auto heightForWidth = label->heightForWidth(labelWidth);
    return heightForWidth > 0 ? heightForWidth : label->sizeHint().height();
}

} // namespace


IntroVerifyEmoji::IntroVerifyEmoji(IntroWidget *parent, ProtocolBridge *bridge)
    : IntroStep(parent, false /* hasCover */)
    , _bridge(bridge)
{
    setTitleText(tr("Compare emoji"));
    setDescriptionText(tr("They should appear in the same order on both sessions."));

    const auto actionButtonWidth = (st::introNextButtonWidth - st::introFieldSpacing) / 2;
    nextButton()->setFixedSize(actionButtonWidth, st::introNextButtonHeight);

    // "They Don't Match" button in the same action row as "They Match".
    _mismatchLink = new IntroOutlineButton(
        &intro::attentionFg,   // text
        &intro::bgOver,        // hover background
        &intro::inputBorder,   // pressed background
        &intro::subtextFg,     // disabled text
        st::introNextButtonRadius,
        this);
    _mismatchLink->setCursor(Qt::PointingHandCursor);
    {
        auto linkFont = st::baseFont(13);
        _mismatchLink->setFont(linkFont);
    }
    _mismatchLink->setText(tr("They don\u2019t match"));
    _mismatchLink->setFixedSize(actionButtonWidth, st::introNextButtonHeight);
    connect(_mismatchLink, &QPushButton::clicked,
            this, &IntroVerifyEmoji::mismatch);

    _retryLink = new intro::LinkButton(tr("Retry"), this);
    _retryLink->setCursor(Qt::PointingHandCursor);
    _retryLink->setFont(st::baseFont(13));
    _retryLink->adjustSize();
    _retryLink->hide();
    connect(_retryLink, &QPushButton::clicked,
            this, &IntroVerifyEmoji::startVerification);

    _recoveryKeyLink = new intro::LinkButton(tr("Use recovery key instead"), this);
    _recoveryKeyLink->setCursor(Qt::PointingHandCursor);
    _recoveryKeyLink->setFont(st::baseFont(13));
    _recoveryKeyLink->adjustSize();
    connect(_recoveryKeyLink, &QPushButton::clicked,
            this, &IntroVerifyEmoji::useRecoveryKeyVerification);
    setTabOrder(nextButton(), _recoveryKeyLink);

    _qrLink = new intro::LinkButton(tr("Scan QR code instead"), this);
    _qrLink->setCursor(Qt::PointingHandCursor);
    _qrLink->setFont(st::baseFont(13));
    _qrLink->adjustSize();
    connect(_qrLink, &QPushButton::clicked,
            this, &IntroVerifyEmoji::useQrVerification);
    setTabOrder(_recoveryKeyLink, _qrLink);

    _skipLink = new intro::LinkButton(tr("Skip for now"), this, true);
    _skipLink->setCursor(Qt::PointingHandCursor);
    _skipLink->setFont(st::baseFont(13));
    _skipLink->adjustSize();
    connect(_skipLink, &QPushButton::clicked,
            this, &IntroVerifyEmoji::skipVerification);
    setTabOrder(_qrLink, _skipLink);

    // Listen for verification events from the protocol bridge.
    connect(_bridge, &ProtocolBridge::sasVerificationStarted,
            this, &IntroVerifyEmoji::onSasStarted);
    connect(_bridge, &ProtocolBridge::sasConfirmed,
            this, &IntroVerifyEmoji::onSasConfirmed);
    connect(_bridge, &ProtocolBridge::verificationStateChanged,
            this, [this](int state, const QString &flowId) {
        constexpr int kCancelled = 9;
        if (state != kCancelled || !isVisible()) {
            return;
        }
        // Ignore a Cancelled emitted while tearing down a flow we deliberately
        // left (e.g. the QR flow when the user chose "compare emoji instead").
        if (!flowId.isEmpty() && flowId == _ignoredFlowId) {
            return;
        }
        setWaitingState(false);
        showFailure(tr(
            "The request was denied or timed out, "
            "or there was a verification mismatch"));
    });
}

void IntroVerifyEmoji::activate() {
    IntroStep::activate();
    startVerification();
}

void IntroVerifyEmoji::startVerification() {
    // Reset state for a fresh SAS attempt.
    _emojis.clear();
    _labels.clear();
    _waiting = false;
    hideError();
    _retryLink->hide();
    nextButton()->setEnabled(false);
    nextButton()->setText(nextButtonText());
    _mismatchLink->show();
    _mismatchLink->setEnabled(false);
    _recoveryKeyLink->setEnabled(true);
    _qrLink->setEnabled(true);
    _skipLink->setEnabled(true);

    // Show waiting state until emojis arrive.
    setDescriptionText(tr("Waiting for the other device\xE2\x80\xA6"));

    _bridge->startSasVerification();
    updateEmojiLayout();
    update();
}

void IntroVerifyEmoji::submit() {
    if (_waiting) {
        return;
    }
    setWaitingState(true);
    _bridge->confirmSasMatch();
}

QString IntroVerifyEmoji::nextButtonText() const {
    return tr("They match");
}

void IntroVerifyEmoji::onSasStarted(bool success, const QStringList &emojis, const QStringList &labels) {
    if (success) {
        _emojis = emojis;
        _labels = labels;
        hideError();
        setDescriptionText(tr("They should appear in the same order on both sessions."));
        nextButton()->setEnabled(true);
        _mismatchLink->setEnabled(true);
        // Active comparison started — lock the alternative-method links so the
        // user can't switch verification method mid-flow.
        _recoveryKeyLink->setEnabled(false);
        _qrLink->setEnabled(false);
    } else {
        _emojis.clear();
        _labels.clear();
        showFailure(tr("Failed to start emoji verification"));
    }
    update();
}

void IntroVerifyEmoji::onSasConfirmed(bool success) {
    if (success) {
        Q_EMIT verified();
    } else {
        setWaitingState(false);
        showFailure(tr(
            "The request was denied or timed out, "
            "or there was a verification mismatch"));
    }
}

void IntroVerifyEmoji::setWaitingState(bool waiting) {
    _waiting = waiting;

    if (_waiting) {
        setDescriptionText(tr("Waiting for your other device to confirm..."));
        nextButton()->setEnabled(false);
        _mismatchLink->show();
        _mismatchLink->setEnabled(false);
        _retryLink->hide();
        _recoveryKeyLink->setEnabled(false);
        _qrLink->setEnabled(false);
        _skipLink->setEnabled(false);
    } else {
        setDescriptionText(tr("They should appear in the same order on both sessions."));
        const auto hasEmojis = !_emojis.isEmpty();
        nextButton()->setEnabled(hasEmojis);
        _mismatchLink->show();
        _mismatchLink->setEnabled(hasEmojis);
        _recoveryKeyLink->setEnabled(true);
        _qrLink->setEnabled(true);
        _skipLink->setEnabled(true);
    }
}

void IntroVerifyEmoji::showFailure(const QString &message) {
    _emojis.clear();
    _labels.clear();
    _waiting = false;
    setDescriptionText(tr("They should appear in the same order on both sessions."));
    nextButton()->setEnabled(false);
    nextButton()->setText(nextButtonText());
    _mismatchLink->show();
    _mismatchLink->setEnabled(false);
    _recoveryKeyLink->setEnabled(true);
    _qrLink->setEnabled(true);
    _skipLink->setEnabled(true);
    showError(message);
    _retryLink->show();
    _retryLink->raise();
    updateEmojiLayout();
    update();
}

void IntroVerifyEmoji::paintEvent(QPaintEvent *e) {
    IntroStep::paintEvent(e);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    paintEmojiContainer(p);
}

void IntroVerifyEmoji::paintEmojiContainer(QPainter &p) {
    const auto containerX = (width() - st::introVerifyEmojiContainerW) / 2;
    const auto containerY = contentTop() + st::introStepFieldTop;

    // Draw rounded container background.
    QPainterPath containerPath;
    containerPath.addRoundedRect(
        QRectF(containerX, containerY, st::introVerifyEmojiContainerW, st::introVerifyEmojiContainerH),
        st::introVerifyEmojiContainerR, st::introVerifyEmojiContainerR);
    p.fillPath(containerPath, intro::bgOver);

    if (_emojis.isEmpty()) {
        return;
    }

    // Compute emoji cell dimensions.
    const int emojiCount = _emojis.size();
    const int firstRowCount = qMin(emojiCount, 4);
    const int secondRowCount = qMax(0, emojiCount - 4);

    const auto emojiFont = st::baseFont(st::introVerifyEmojiFontSize);
    const auto labelFont = st::baseFont(st::introVerifyEmojiLabelSize);

    const int rowsCount = secondRowCount > 0 ? 2 : 1;
    const int innerX = containerX + st::introVerifyEmojiPadding;
    const int innerY = containerY + st::introVerifyEmojiPadding;
    const int innerW = st::introVerifyEmojiContainerW - 2 * st::introVerifyEmojiPadding;
    const int innerH = st::introVerifyEmojiContainerH - 2 * st::introVerifyEmojiPadding;

    const auto paintRow = [&](int start, int rowCount, int rowIndex) {
        if (rowCount <= 0) {
            return;
        }
        const int rowY = innerY + (innerH * rowIndex) / rowsCount;
        const int nextRowY = innerY + (innerH * (rowIndex + 1)) / rowsCount;
        const int rowH = nextRowY - rowY;

        for (int cellIndex = 0; cellIndex < rowCount; ++cellIndex) {
            const int idx = start + cellIndex;
            const int cellX = innerX + (innerW * cellIndex) / rowCount;
            const int nextCellX = innerX + (innerW * (cellIndex + 1)) / rowCount;
            const int cellW = nextCellX - cellX;
            const int contentH = rowH - st::introVerifyEmojiCellGap;
            const int emojiH = (contentH * 3) / 4;
            const int labelH = contentH - emojiH;

            // Font/pen are for the text fallback; a sprite fills its own box, so it is
            // drawn at the font size rather than the ~1.5x a glyph would occupy.
            p.setFont(emojiFont);
            p.setPen(intro::titleFg);
            TeleMatrix::Emoji::DrawCentered(
                p,
                _emojis.value(idx),
                st::introVerifyEmojiFontSize,
                QRect(cellX, rowY, cellW, emojiH));

            p.setFont(labelFont);
            p.setPen(intro::subtextFg);
            p.drawText(QRect(cellX, rowY + emojiH + st::introVerifyEmojiCellGap, cellW, labelH),
                       Qt::AlignHCenter | Qt::AlignVCenter,
                       _labels.value(idx));
        }
    };

    paintRow(0, firstRowCount, 0);
    paintRow(4, secondRowCount, 1);
}

void IntroVerifyEmoji::resizeEvent(QResizeEvent *e) {
    IntroStep::resizeEvent(e);
    updateEmojiLayout();
}

void IntroVerifyEmoji::updateEmojiLayout() {
    sizeLabelToOneLine(
        descriptionLabel(), width(), contentTop() + st::introDescriptionTop);

    const auto containerX = (width() - st::introVerifyEmojiContainerW) / 2;
    const auto containerY = contentTop() + st::introStepFieldTop;
    _retryLink->adjustSize();
    _retryLink->move(
        containerX + (st::introVerifyEmojiContainerW - _retryLink->width()) / 2,
        containerY + (st::introVerifyEmojiContainerH - _retryLink->height()) / 2);

    const auto actionButtonWidth = (st::introNextButtonWidth - st::introFieldSpacing) / 2;
    const auto rowWidth = actionButtonWidth * 2 + st::introFieldSpacing;
    const auto rowLeft = (width() - rowWidth) / 2;
    const auto rowTop = contentTop() + st::introNextTop;

    _mismatchLink->setFixedSize(actionButtonWidth, st::introNextButtonHeight);
    nextButton()->setFixedSize(actionButtonWidth, st::introNextButtonHeight);
    nextButton()->move(rowLeft, rowTop);
    _mismatchLink->move(rowLeft + actionButtonWidth + st::introFieldSpacing, rowTop);

    // Error goes below the match/don't-match row, not IntroStep's default spot
    // (between description and card) — that gap is too tight for a two-line
    // message here and it overlapped the buttons. One line only, same as above.
    auto *error = errorLabel();
    const auto errorTop = rowTop + st::introNextButtonHeight + st::introLinkTop;
    const auto errorHeight = sizeLabelToOneLine(error, width(), errorTop);

    _recoveryKeyLink->adjustSize();
    _qrLink->adjustSize();
    _skipLink->adjustSize();
    const auto linksTop = errorTop
        + (error->isVisible() ? errorHeight + st::introVerifyCardTextGap : 0);
    // Order per the redesign: the two other methods (QR, then recovery key),
    // then "Skip for now" last.
    _qrLink->move(
        (width() - _qrLink->width()) / 2,
        linksTop);
    const auto recoveryY =
        linksTop + _qrLink->height() + st::introVerifyCardTextGap;
    _recoveryKeyLink->move(
        (width() - _recoveryKeyLink->width()) / 2,
        recoveryY);
    _skipLink->move(
        (width() - _skipLink->width()) / 2,
        recoveryY + _recoveryKeyLink->height() + st::introVerifyCardTextGap);
}

} // namespace TeleMatrix

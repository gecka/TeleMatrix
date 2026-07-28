// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "intro_verify_recovery_key.h"
#include "intro_widget.h"

#include "../protocol/protocol_bridge.h"
#include "../styles/style_constants.h"
#include "intro_colors.h"
#include "intro_widgets.h"

#include "ui/painter.h"
#include "ui/recovery_key_format.h"

#include <QFontMetrics>
#include <QLineEdit>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QResizeEvent>

namespace TeleMatrix {

namespace {

using IntroKeyLineEdit = intro::Field;


} // namespace

IntroVerifyRecoveryKey::IntroVerifyRecoveryKey(
    QWidget *parent,
    ProtocolBridge *bridge)
    : IntroStep(parent, false /* hasCover */)
    , _bridge(bridge)
{
    setTitleText(tr("Enter recovery key"));
    setDescriptionText(tr(
        "Enter your recovery key to verify this session. "
        "You can find it in your security settings"));

    // Recovery key input: keep the same dimensions and font as the
    // settings verification popup. IntroKeyLineEdit paints its border/focus
    // border with live intro:: colors; text/background/selection/disabled
    // colours come from its palette.
    _keyInput = new IntroKeyLineEdit(this);
    _keyInput->setFixedSize(st::introRecoveryKeyWidth, st::introRecoveryKeyHeight);
    _keyInput->setPlaceholderText(tr("Recovery key or phrase"));
    _keyInput->setMaxLength(59);

    // Equal inner padding, and the key is centred: a complete key is narrower than
    // the field, so left-aligning it left all the slack on the right and read as
    // lopsided padding.
    _keyInput->setTextMargins(10, 0, 10, 0);
    _keyInput->setAlignment(Qt::AlignCenter);

    // Type the key in groups of four, as it was shown when it was created.
    connect(_keyInput, &QLineEdit::textEdited, this, [this] {
        const auto formatted = FormatRecoveryKey(
            _keyInput->text(), _keyInput->cursorPosition());
        if (formatted.text == _keyInput->text()) {
            return;
        }
        // setText() emits textChanged (validation still runs) but not textEdited,
        // so this cannot re-enter.
        _keyInput->setText(formatted.text);
        _keyInput->setCursorPosition(formatted.cursor);
    });

    // Font via QFont — NOT stylesheet font-size (macOS pitfall).
    auto keyFont = st::monospaceFont(st::introRecoveryKeyFontSize);
    _keyInput->setFont(keyFont);

    // Tab order.
    setTabOrder(_keyInput, nextButton());

    // Enter key submits from the single-line field.
    connect(_keyInput, &QLineEdit::returnPressed,
            this, &IntroVerifyRecoveryKey::submit);
    connect(_keyInput, &QLineEdit::textChanged,
            this, &IntroVerifyRecoveryKey::updateSubmitState);

    // Listen for verification result from the protocol bridge.
    connect(_bridge, &ProtocolBridge::recoveryKeyVerified,
            this, &IntroVerifyRecoveryKey::onVerifyResult);

    _emojiLink = new intro::LinkButton(tr("Compare emoji instead"), this);
    _emojiLink->setCursor(Qt::PointingHandCursor);
    _emojiLink->setFont(st::baseFont(13));
    _emojiLink->adjustSize();
    connect(_emojiLink, &QPushButton::clicked,
            this, &IntroVerifyRecoveryKey::useEmojiVerification);
    setTabOrder(nextButton(), _emojiLink);

    _qrLink = new intro::LinkButton(tr("Scan QR code instead"), this);
    _qrLink->setCursor(Qt::PointingHandCursor);
    _qrLink->setFont(st::baseFont(13));
    _qrLink->adjustSize();
    connect(_qrLink, &QPushButton::clicked,
            this, &IntroVerifyRecoveryKey::useQrVerification);
    setTabOrder(_emojiLink, _qrLink);

    _skipLink = new intro::LinkButton(tr("Skip for now"), this, true);
    _skipLink->setCursor(Qt::PointingHandCursor);
    _skipLink->setFont(st::baseFont(13));
    _skipLink->adjustSize();
    connect(_skipLink, &QPushButton::clicked,
            this, &IntroVerifyRecoveryKey::skipVerification);
    setTabOrder(_qrLink, _skipLink);

    nextButton()->setEnabled(false);
}

void IntroVerifyRecoveryKey::submit() {
    if (_submitting) {
        return;
    }

    const auto text = _keyInput->text().trimmed();

    if (text.isEmpty()) {
        showError(tr("Please enter your recovery key"));
        return;
    }

    hideError();
    _submitting = true;

    _keyInput->setEnabled(false);
    nextButton()->setEnabled(false);
    nextButton()->setText(tr("Verifying..."));
    _emojiLink->setEnabled(false);
    _qrLink->setEnabled(false);
    _skipLink->setEnabled(false);

    _bridge->verifyWithRecoveryKey(text);
}

void IntroVerifyRecoveryKey::onVerifyResult(bool success) {
    _submitting = false;

    _keyInput->setEnabled(true);
    nextButton()->setText(nextButtonText());
    _emojiLink->setEnabled(true);
    _qrLink->setEnabled(true);
    _skipLink->setEnabled(true);
    updateSubmitState();

    // Fix #15: ignore completions that arrive after the user has backed out.
    // _active is cleared by IntroWidget before navigating away from this step.
    if (!_active) {
        return;
    }

    if (success) {
        hideError();
        Q_EMIT verified();
    } else {
        showError(tr("Invalid recovery key. Please check and try again"));
    }
}

QString IntroVerifyRecoveryKey::nextButtonText() const {
    return tr("Verify");
}

void IntroVerifyRecoveryKey::deactivate() {
    _active = false; // Fix #15: prevent stale completions from forcing navigation.
}

void IntroVerifyRecoveryKey::activate() {
    IntroStep::activate();
    _active = true; // Fix #15: mark step as visible.
    hideError();

    // Reset submission state in case user navigated away mid-submit.
    if (_submitting) {
        _submitting = false;
        _keyInput->setEnabled(true);
    }
    _emojiLink->setEnabled(!_submitting);
    _qrLink->setEnabled(!_submitting);
    _skipLink->setEnabled(!_submitting);
    updateSubmitState();

    _keyInput->setFocus();
}

void IntroVerifyRecoveryKey::showError(const QString &text) {
    errorLabel()->setText(text);
    errorLabel()->show();
    updateKeyLayout();
}

void IntroVerifyRecoveryKey::hideError() {
    errorLabel()->clear();
    errorLabel()->hide();
    updateKeyLayout();
}

void IntroVerifyRecoveryKey::updateSubmitState() {
    const auto hasText = !_keyInput->text().trimmed().isEmpty();
    nextButton()->setEnabled(hasText && !_submitting);
    _emojiLink->setEnabled(!_submitting);
    _qrLink->setEnabled(!_submitting);
    _skipLink->setEnabled(!_submitting);
    if (hasText && errorLabel()->isVisible()) {
        hideError();
    }
}

void IntroVerifyRecoveryKey::resizeEvent(QResizeEvent *e) {
    IntroStep::resizeEvent(e);
    updateKeyLayout();
}

void IntroVerifyRecoveryKey::updateSkipVisibility() {
    _skipLink->setVisible(allowsSkip());
    // Unlike the other verify screens this one centres its block as a whole, so
    // dropping a link has to re-centre what is left.
    updateKeyLayout();
}

void IntroVerifyRecoveryKey::updateKeyLayout() {
    _emojiLink->adjustSize();
    _qrLink->adjustSize();
    _skipLink->adjustSize();

    const auto groupHeight = st::introStepFieldTop
        + st::introRecoveryKeyHeight
        + st::introRecoveryKeyErrorTop
        + st::introRecoveryKeyErrorHeight
        + st::introRecoveryKeyButtonGap
        + st::introNextButtonHeight
        + st::introLinkTop
        + _emojiLink->height()
        + st::introVerifyCardTextGap
        + _qrLink->height()
        + st::introVerifyCardTextGap
        + _skipLink->height();
    const auto top = qMax(st::introStepTopMin, (height() - groupHeight) / 2);
    const auto keyLeft = (width() - st::introRecoveryKeyWidth) / 2;
    const auto contentLeft = this->contentLeft();

    // contentStartTop() reserves the error slot, so the message has somewhere
    // to go without displacing the field.
    const int fieldY = contentStartTop();

    titleLabel()->move(contentLeft, top + st::introTitleTop);
    titleLabel()->setFixedWidth(st::introStepWidth);

    descriptionLabel()->setFixedWidth(st::introStepWidth);
    const auto descriptionHeight = qMax(
        descriptionLabel()->heightForWidth(st::introStepWidth),
        descriptionLabel()->sizeHint().height());
    descriptionLabel()->setFixedHeight(descriptionHeight);
    descriptionLabel()->move(contentLeft, top + st::introDescriptionTop);

    // Recovery key input. The error goes ABOVE it, like every other form —
    // it used to sit between the field and the button, which both broke the
    // convention and pushed the button down when a message appeared.
    _keyInput->move(keyLeft, fieldY);
    placeErrorAbove(fieldY);

    const auto buttonLeft = (width() - st::introNextButtonWidth) / 2;
    const auto buttonY = fieldY
        + st::introRecoveryKeyHeight
        + st::introFieldsToButton;
    nextButton()->move(buttonLeft, buttonY);

    const auto linkX = (width() - _emojiLink->width()) / 2;
    const auto linkY = buttonY + st::introNextButtonHeight + st::introLinkTop;
    // Order per the redesign: the two other methods (QR, then emoji), then
    // "Skip for now" last.
    _qrLink->move(
        (width() - _qrLink->width()) / 2,
        linkY);

    const auto emojiY = linkY + _qrLink->height() + st::introVerifyCardTextGap;
    _emojiLink->move(
        (width() - _emojiLink->width()) / 2,
        emojiY);

    _skipLink->move(
        (width() - _skipLink->width()) / 2,
        emojiY + _emojiLink->height() + st::introVerifyCardTextGap);

    centerContentVertically();
}

} // namespace TeleMatrix

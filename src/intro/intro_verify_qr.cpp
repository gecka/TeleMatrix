// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "intro_verify_qr.h"
#include "intro_widget.h"

#include "../protocol/protocol_bridge.h"
#include "../protocol/verification_page_rules.h"
#include "../styles/style_constants.h"
#include "intro_colors.h"
#include "intro_widgets.h"

#include "ui/painter.h"
#include "ui/qr_code_image.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>

namespace TeleMatrix {

namespace {


} // namespace

IntroVerifyQr::IntroVerifyQr(QWidget *parent, ProtocolBridge *bridge)
    : IntroStep(parent, false /* hasCover */)
    , _bridge(bridge)
{
    setTitleText(tr("Scan QR code"));
    setDescriptionText(tr("Scan this code with another session to verify."));

    nextButton()->setFixedSize(st::introNextButtonWidth, st::introNextButtonHeight);

    _retryLink = new intro::LinkButton(tr("Retry"), this);
    _retryLink->setCursor(Qt::PointingHandCursor);
    _retryLink->setFont(st::baseFont(13));
    _retryLink->adjustSize();
    _retryLink->hide();
    connect(_retryLink, &QPushButton::clicked,
            this, &IntroVerifyQr::startVerification);

    _emojiLink = new intro::LinkButton(tr("Compare emoji instead"), this);
    _emojiLink->setCursor(Qt::PointingHandCursor);
    _emojiLink->setFont(st::baseFont(13));
    _emojiLink->adjustSize();
    connect(_emojiLink, &QPushButton::clicked,
            this, &IntroVerifyQr::useEmojiVerification);
    setTabOrder(nextButton(), _emojiLink);

    _recoveryLink = new intro::LinkButton(tr("Use recovery key instead"), this);
    _recoveryLink->setCursor(Qt::PointingHandCursor);
    _recoveryLink->setFont(st::baseFont(13));
    _recoveryLink->adjustSize();
    connect(_recoveryLink, &QPushButton::clicked,
            this, &IntroVerifyQr::useRecoveryKeyVerification);
    setTabOrder(_emojiLink, _recoveryLink);

    _skipLink = new intro::LinkButton(tr("Skip for now"), this, true);
    _skipLink->setCursor(Qt::PointingHandCursor);
    _skipLink->setFont(st::baseFont(13));
    _skipLink->adjustSize();
    connect(_skipLink, &QPushButton::clicked,
            this, &IntroVerifyQr::skipVerification);
    setTabOrder(_recoveryLink, _skipLink);

    connect(_bridge, &ProtocolBridge::qrCodeReady,
            this, &IntroVerifyQr::onQrCodeReady);
    connect(_bridge, &ProtocolBridge::qrScanConfirmed,
            this, &IntroVerifyQr::onQrScanConfirmed);
    connect(_bridge, &ProtocolBridge::qrCodeDataReady,
            this, [this](const QString &flowId, const QByteArray &modules, int size) {
        if (!isVisible() || modules.isEmpty() || size <= 0) {
            return;
        }
        // Don't render another flow's code. This page has no adoption path, so
        // the pre-latch wildcard is enough here — unlike the emoji page, which
        // needs the stricter emojisBelongToPage.
        if (!VerificationPageRules::flowMatches(flowId, _flowId)) {
            return;
        }
        if (!flowId.isEmpty()) {
            _flowId = flowId;
        }
        _modules = modules;
        _qrSize = size;
        hideError();
        setDescriptionText(tr("Scan this code with another session to verify."));
        update();
    });
    connect(_bridge, &ProtocolBridge::verificationCancelInfo,
            this, [this](const QString &flowId, const QString &code, bool) {
        if (!isVisible()) {
            return;
        }
        // Require a real match on both sides. Adoption only selects the
        // wording of a failure this page shows anyway, so a code we decline to
        // adopt costs at most the routine wording — while a foreign flow's
        // code adopted here would mislabel this page's own later Cancelled.
        if (VerificationPageRules::adoptCancelCodeStrict(flowId, _flowId)) {
            _lastCancelCode = code;
        }
    });
    connect(_bridge, &ProtocolBridge::verificationStateChanged,
            this, [this](int state, const QString &flowId) {
        if (!isVisible()) {
            return;
        }
        // Fallback to the start reply's latch, for a flow this page adopted
        // without starting it. Only fills an empty latch, and skips the two
        // states that can carry a foreign or stale id (see the rule).
        if (VerificationPageRules::qrLatchesFromState(state, flowId, _flowId)) {
            _flowId = flowId;
        }
        if (state == VerificationPageRules::kDone) {
            // Mirror the Cancelled guard below: don't fire verified() before
            // this page has latched a flow, or for a different flow's Done.
            if (!VerificationPageRules::acceptDone(flowId, _flowId)) {
                return;
            }
            Q_EMIT verified();
            return;
        }
        if (state == VerificationPageRules::kQrCodeScanned) {
            setScannedState();
        } else if (state == VerificationPageRules::kCancelled) {
            // Ignore a Cancelled when this page does not yet own a flow (the QR
            // code isn't shown / latched): it belongs to a previously-cancelled
            // flow, e.g. when arriving here from the emoji page which tore its SAS
            // down. Also ignore one that belongs to a different flow.
            if (!VerificationPageRules::showCancelledOnQrPage(flowId, _flowId)) {
                return;
            }
            setWaitingState(false);
            const bool securityFailure =
                VerificationPageRules::isSecurityCancelCode(_lastCancelCode);
            showFailure(securityFailure
                ? tr("Verification failed: the keys did not match. "
                     "Your messages may not be secure.")
                : tr("The request was denied or timed out, "
                     "or it was cancelled on the other device"));
        }
    });
}

void IntroVerifyQr::activate() {
    IntroStep::activate();
    startVerification();
}

void IntroVerifyQr::startVerification() {
    _modules.clear();
    _qrSize = 0;
    _scanned = false;
    _waiting = false;
    _flowId.clear();
    _lastCancelCode.clear();
    hideError();
    _retryLink->hide();
    nextButton()->setEnabled(false);
    nextButton()->setText(nextButtonText());
    _emojiLink->setEnabled(true);
    _recoveryLink->setEnabled(true);
    _skipLink->setEnabled(true);

    setDescriptionText(tr("Waiting for your other session to accept\xE2\x80\xA6"));

    _startRequestId = _bridge->startQrVerification();
    updateLayout();
    update();
}

void IntroVerifyQr::submit() {
    if (_waiting || !_scanned) {
        return;
    }
    setWaitingState(true);
    _bridge->confirmQrScanned();
}

QString IntroVerifyQr::nextButtonText() const {
    return tr("Continue");
}

void IntroVerifyQr::onQrCodeReady(
        quint64 requestId, bool success, const QString &flowId) {
    if (requestId != _startRequestId || _startRequestId == 0) {
        return; // another surface's start; not this page's reply
    }
    _startRequestId = 0;
    if (success) {
        // Early latch: our own flow, from our own call's acknowledgement. The
        // modules themselves arrive later on qrCodeDataReady.
        _flowId = flowId;
        return;
    }
    if (!isVisible()) {
        return;
    }
    _modules.clear();
    _qrSize = 0;
    showFailure(tr("Couldn’t start QR verification"));
    update();
}

void IntroVerifyQr::onQrScanConfirmed(bool success) {
    // success only means the confirmation was sent; completion arrives as Done.
    if (success) {
        return;
    }
    setWaitingState(false);
    showFailure(tr(
        "The request was denied or timed out, "
        "or there was a verification mismatch"));
}

void IntroVerifyQr::setScannedState() {
    _scanned = true;
    _waiting = false;
    setDescriptionText(tr(
        "Your other session scanned the code. If it shows a checkmark, "
        "click Continue."));
    nextButton()->setEnabled(true);
    // Scanned = actively verifying — lock the method-switch links.
    _emojiLink->setEnabled(false);
    _recoveryLink->setEnabled(false);
    _skipLink->setEnabled(true);
    update();
}

void IntroVerifyQr::setWaitingState(bool waiting) {
    _waiting = waiting;
    if (_waiting) {
        setDescriptionText(tr("Waiting for your other session to confirm\xE2\x80\xA6"));
        nextButton()->setEnabled(false);
        _emojiLink->setEnabled(false);
        _recoveryLink->setEnabled(false);
        _skipLink->setEnabled(false);
    } else {
        nextButton()->setEnabled(_scanned);
        _emojiLink->setEnabled(true);
        _recoveryLink->setEnabled(true);
        _skipLink->setEnabled(true);
    }
}

void IntroVerifyQr::showFailure(const QString &message) {
    _modules.clear();
    _qrSize = 0;
    _scanned = false;
    _waiting = false;
    setDescriptionText(tr("Scan this code with another session to verify."));
    nextButton()->setEnabled(false);
    nextButton()->setText(nextButtonText());
    _emojiLink->setEnabled(true);
    _recoveryLink->setEnabled(true);
    _skipLink->setEnabled(true);
    showError(message);
    _retryLink->show();
    _retryLink->raise();
    updateLayout();
    update();
}

void IntroVerifyQr::paintEvent(QPaintEvent *e) {
    IntroStep::paintEvent(e);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    paintQrContainer(p);
}

void IntroVerifyQr::paintQrContainer(QPainter &p) {
    // Square container sized for a QR code (not the wide emoji box).
    const int side = st::introVerifyEmojiContainerH;
    const int containerX = (width() - side) / 2;
    const int containerY = contentTop() + st::introStepFieldTop;

    QPainterPath containerPath;
    containerPath.addRoundedRect(
        QRectF(containerX, containerY, side, side),
        st::introVerifyEmojiContainerR, st::introVerifyEmojiContainerR);

    const bool hasQr = !_modules.isEmpty() && _qrSize > 0;
    // White card behind a ready code (QR needs a light quiet zone); a gray
    // placeholder block while waiting for the code.
    p.fillPath(containerPath,
               hasQr ? QColor(Qt::white) : st::withAlpha(st::windowSubTextFg, 64));

    if (!hasQr) {
        return;
    }

    const int pad = st::introVerifyEmojiPadding;
    const QRect inner(containerX + pad, containerY + pad, side - 2 * pad, side - 2 * pad);
    // Fixed black-on-white for reliable scanning, independent of theme.
    paintQrModules(p, inner, _modules, _qrSize, QColor(Qt::black), QColor(Qt::white));
}

void IntroVerifyQr::resizeEvent(QResizeEvent *e) {
    IntroStep::resizeEvent(e);
    updateLayout();
}

void IntroVerifyQr::updateSkipVisibility() {
    _skipLink->setVisible(allowsSkip());
}

void IntroVerifyQr::updateLayout() {
    const int side = st::introVerifyEmojiContainerH;
    const auto containerX = (width() - side) / 2;
    const auto containerY = contentTop() + st::introStepFieldTop;
    _retryLink->adjustSize();
    _retryLink->move(
        containerX + (side - _retryLink->width()) / 2,
        containerY + (side - _retryLink->height()) / 2);

    // The default error slot sits where the QR block is drawn, so a failure
    // message lands on top of the container it is about. Put it above, matching
    // the login/register steps — but wider than the form column, because these
    // messages are full sentences and the slot's top line overlaps the subtitle.
    placeErrorAbove(containerY, st::introVerifyErrorWidth);

    const auto rowTop = contentTop() + st::introNextTop;
    nextButton()->setFixedSize(st::introNextButtonWidth, st::introNextButtonHeight);
    nextButton()->move((width() - st::introNextButtonWidth) / 2, rowTop);

    _emojiLink->adjustSize();
    _recoveryLink->adjustSize();
    _skipLink->adjustSize();
    const auto linkY = rowTop + st::introNextButtonHeight + st::introLinkTop;
    _emojiLink->move((width() - _emojiLink->width()) / 2, linkY);
    const auto recoveryY = linkY + _emojiLink->height() + st::introVerifyCardTextGap;
    _recoveryLink->move((width() - _recoveryLink->width()) / 2, recoveryY);
    _skipLink->move(
        (width() - _skipLink->width()) / 2,
        recoveryY + _recoveryLink->height() + st::introVerifyCardTextGap);
}

} // namespace TeleMatrix

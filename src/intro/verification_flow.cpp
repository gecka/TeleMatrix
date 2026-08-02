// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "verification_flow.h"

#include "intro_colors.h"
#include "intro_step.h"
#include "intro_setup_encryption.h"
#include "intro_verify_choice.h"
#include "intro_verify_emoji.h"
#include "intro_verify_qr.h"
#include "intro_verify_recovery_key.h"
#include "intro_verify_success.h"

#include "../protocol/protocol_bridge.h"

#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace TeleMatrix {

namespace {

constexpr int kStepChoice = 0;
constexpr int kStepEmoji = 1;
constexpr int kStepQr = 2;
constexpr int kStepRecoveryKey = 3;
constexpr int kStepSuccess = 4;
constexpr int kStepSetupEncryption = 5;

} // namespace

VerificationFlow::VerificationFlow(ProtocolBridge *bridge, QWidget *parent)
    : QWidget(parent)
    , _bridge(bridge)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    _stack = new QStackedWidget(this);
    layout->addWidget(_stack);

    _choiceStep = new IntroVerifyChoice(this, _bridge);
    _emojiStep = new IntroVerifyEmoji(this, _bridge);
    _qrStep = new IntroVerifyQr(this, _bridge);
    _recoveryKeyStep = new IntroVerifyRecoveryKey(this, _bridge);
    _successStep = new IntroVerifySuccess(this);
    _setupEncryptionStep = new IntroSetupEncryption(this, _bridge);

    _stack->addWidget(_choiceStep);           // 0
    _stack->addWidget(_emojiStep);            // 1
    _stack->addWidget(_qrStep);               // 2
    _stack->addWidget(_recoveryKeyStep);      // 3
    _stack->addWidget(_successStep);          // 4
    _stack->addWidget(_setupEncryptionStep);  // 5

    // A popup over the running app, not the first-run stage: the version line
    // belongs to the stage. (The keys line is off unless a step asks for it.)
    //
    // "Skip for now" goes too, and only here. It exists so that signing in or
    // creating an account can't strand someone outside the app — but this popup
    // is opened from inside an app that is already running, where closing the
    // card is the way out. Those flows are IntroWidget's, not ours, so they keep
    // their skip.
    for (int i = 0; i != _stack->count(); ++i) {
        if (auto *step = qobject_cast<IntroStep *>(_stack->widget(i))) {
            step->setShowsVersion(false);
            step->setAllowsSkip(false);
        }
    }

    connect(_choiceStep, &IntroVerifyChoice::qrChosen,
            this, [this] { showStep(kStepQr); });
    connect(_choiceStep, &IntroVerifyChoice::emojiChosen,
            this, [this] {
        // A stale incoming request from an earlier entry must not be re-answered
        // when the user explicitly picked emoji from the choice screen.
        _emojiStep->setRequestFlowId(QString());
        showStep(kStepEmoji);
    });
    connect(_choiceStep, &IntroVerifyChoice::recoveryKeyChosen,
            this, [this] { showStep(kStepRecoveryKey); });
    connect(_choiceStep, &IntroVerifyChoice::skipVerification,
            this, [this] { onSkipped(); });
    connect(_choiceStep, &IntroVerifyChoice::setupEncryptionNeeded,
            this, [this] { showStep(kStepSetupEncryption); });

    connect(_emojiStep, &IntroVerifyEmoji::verified,
            this, &VerificationFlow::onVerified);
    connect(_emojiStep, &IntroVerifyEmoji::mismatch, this, [this] {
        _bridge->mismatchSas();
        showStep(kStepChoice);
    });
    connect(_emojiStep, &IntroVerifyEmoji::useRecoveryKeyVerification,
            this, [this] {
        // Cancel BEFORE switching: activeFlowId() reads the current page, and
        // must still see the emoji page here to name the flow being left.
        _bridge->cancelVerification(activeFlowId());
        showStep(kStepRecoveryKey);
    });
    connect(_emojiStep, &IntroVerifyEmoji::useQrVerification, this, [this] {
        // Cancel BEFORE switching: showStep activates the QR page, which starts
        // a fresh flow — cancelling after would tear down the new QR instead of
        // the SAS being left. activeFlowId() still reads the emoji page here.
        _bridge->cancelVerification(activeFlowId());
        showStep(kStepQr);
    });
    connect(_emojiStep, &IntroVerifyEmoji::skipVerification, this, [this] {
        _bridge->cancelVerification(activeFlowId());
        onSkipped();
    });

    connect(_qrStep, &IntroVerifyQr::verified,
            this, &VerificationFlow::onVerified);
    connect(_qrStep, &IntroVerifyQr::useEmojiVerification, this, [this] {
        // Tell the emoji page to ignore the QR flow's Cancelled (emitted by the
        // cancel below) so it doesn't surface as a failure there.
        _emojiStep->ignoreFlow(_qrStep->currentFlowId());
        _bridge->cancelVerification(activeFlowId());
        showStep(kStepEmoji);
    });
    connect(_qrStep, &IntroVerifyQr::useRecoveryKeyVerification, this, [this] {
        // Cancel BEFORE switching: activeFlowId() reads the current page, and
        // must still see the QR page here to name the flow being left.
        _bridge->cancelVerification(activeFlowId());
        showStep(kStepRecoveryKey);
    });
    connect(_qrStep, &IntroVerifyQr::skipVerification, this, [this] {
        _bridge->cancelVerification(activeFlowId());
        onSkipped();
    });

    connect(_bridge, &ProtocolBridge::sasEmojisAvailable, this,
            [this](const QString &flowId,
                   const QStringList &emojis,
                   const QStringList &labels) {
        if (_stack->currentIndex() != kStepQr) {
            return;
        }
        // Peer picked emoji while our QR was up — follow them (Element's
        // panel switches to the SAS phase the same way).
        _emojiStep->presentAdoptedSas(flowId, emojis, labels);
        showStep(kStepEmoji);
    });

    connect(_recoveryKeyStep, &IntroVerifyRecoveryKey::verified,
            this, &VerificationFlow::onVerified);
    connect(_recoveryKeyStep, &IntroVerifyRecoveryKey::useEmojiVerification,
            this, [this] {
        _recoveryKeyStep->deactivate();
        showStep(kStepEmoji);
    });
    connect(_recoveryKeyStep, &IntroVerifyRecoveryKey::useQrVerification,
            this, [this] {
        _recoveryKeyStep->deactivate();
        showStep(kStepQr);
    });
    connect(_recoveryKeyStep, &IntroVerifyRecoveryKey::skipVerification,
            this, [this] {
        _recoveryKeyStep->deactivate();
        onSkipped();
    });

    connect(_successStep, &IntroStep::goNext, this, &VerificationFlow::done);

    // Provisioning signs the device's own keys, so it ends genuinely verified —
    // unlike skipping, there is nothing to tell the backend afterwards.
    connect(_setupEncryptionStep, &IntroSetupEncryption::done, this, [this] {
        _setupEncryptionStep->deactivate();
        _verified = true;
        _finished = true;
        Q_EMIT verified();
        Q_EMIT done();
    });
    connect(_setupEncryptionStep, &IntroSetupEncryption::skipped, this, [this] {
        _setupEncryptionStep->deactivate();
        onSkipped();
    });
}

void VerificationFlow::start(Entry entry, const QString &flowId) {
    if (entry == Entry::Emoji) {
        // Every incoming request lands here: a request carries the methods its
        // sender SUPPORTS, never the one its user picked, so there is nothing in
        // it to route on.
        _emojiStep->setRequestFlowId(flowId);
        // An incoming request is one specific flow the other session is waiting
        // on; the remaining links start a different one, so drop them.
        _emojiStep->setShowsAlternativeMethods(false);
        showStep(kStepEmoji);
        return;
    }
    showStep(kStepChoice);
}

void VerificationFlow::cancel() {
    // Whatever page is up may have a flow in flight; leaving it running would
    // strand a request the user has walked away from. Harmless when there is
    // none; skipped and verified have already settled the backend.
    if (_finished) {
        return;
    }
    _finished = true;
    _recoveryKeyStep->deactivate();
    _setupEncryptionStep->deactivate();
    _bridge->cancelVerification(activeFlowId());
}

QString VerificationFlow::activeFlowId() const {
    switch (_stack->currentIndex()) {
    case kStepEmoji: return _emojiStep->currentFlowId();
    case kStepQr: return _qrStep->currentFlowId();
    default: return QString();
    }
}

void VerificationFlow::showStep(int index) {
    _stack->setCurrentIndex(index);
    if (auto *step = qobject_cast<IntroStep *>(_stack->currentWidget())) {
        step->activate();
    }
}

void VerificationFlow::onVerified() {
    _verified = true;
    _finished = true;
    Q_EMIT verified();
    showStep(kStepSuccess);
}

void VerificationFlow::onSkipped() {
    // Same as the intro's "Skip for now": tell the backend it was skipped rather
    // than merely cancelled, so permanently-undecryptable messages show the
    // "Unable to decrypt" card instead of waiting on keys that won't arrive.
    _finished = true;
    _bridge->skipVerification();
    Q_EMIT skipped();
}

void VerificationFlow::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), intro::bg);
}

void VerificationFlow::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    _stack->setGeometry(rect());
}

} // namespace TeleMatrix

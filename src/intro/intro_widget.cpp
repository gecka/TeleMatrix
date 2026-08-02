// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "intro_widget.h"
#include "intro_start.h"
#include "intro_secret_backend.h"
#include "intro_create_password.h"
#include "intro_login.h"
#include "intro_register.h"
#include "intro_forgot_password.h"
#include "intro_verify_choice.h"
#include "intro_verify_emoji.h"
#include "intro_verify_qr.h"
#include "intro_verify_recovery_key.h"
#include "intro_verify_success.h"
#include "intro_setup_encryption.h"

#include "../app/app_controller.h"
#include "../protocol/protocol_bridge.h"
#include "../styles/style_constants.h"
#include "intro_colors.h"

#include <QPainter>
#include <QPaintEvent>
#include <QVBoxLayout>
#include <QResizeEvent>

namespace TeleMatrix {

IntroWidget::IntroWidget(
    ProtocolBridge *bridge,
    InitialStep initialStep,
    QWidget *parent)
    : QWidget(parent)
    , _bridge(bridge)
{
    // No stylesheet — white background painted in paintEvent to avoid
    // Qt stylesheet engine cascading to child QLineEdits on macOS
    // (causes white-on-white text).

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    _stack = new QStackedWidget(this);
    layout->addWidget(_stack);

    // Create all steps.
    _startStep = new IntroStart(this);
    _secretBackendStep = new IntroSecretBackend(this);
    _createPasswordStep = new IntroCreatePassword(this);
    _loginStep = new IntroLogin(this, _bridge);
    _registerStep = new IntroRegister(this, _bridge);
    _verifyChoiceStep = new IntroVerifyChoice(this, _bridge);
    _verifyEmojiStep = new IntroVerifyEmoji(this, _bridge);
    _verifyRecoveryKeyStep = new IntroVerifyRecoveryKey(this, _bridge);
    _verifySuccessStep = new IntroVerifySuccess(this);
    _forgotPasswordStep = new IntroForgotPassword(this, _bridge);
    _verifyQrStep = new IntroVerifyQr(this, _bridge);
    _setupEncryptionStep = new IntroSetupEncryption(this, _bridge);

    // Add to stack (order determines indices).
    _stack->addWidget(_startStep);              // 0
    _stack->addWidget(_loginStep);              // 1
    _stack->addWidget(_registerStep);           // 2
    _stack->addWidget(_verifyChoiceStep);       // 3
    _stack->addWidget(_verifyEmojiStep);        // 4
    _stack->addWidget(_verifyRecoveryKeyStep);  // 5
    _stack->addWidget(_verifySuccessStep);      // 6
    _stack->addWidget(_forgotPasswordStep);     // 7
    _stack->addWidget(_verifyQrStep);           // 8
    _stack->addWidget(_secretBackendStep);      // 9
    _stack->addWidget(_createPasswordStep);     // 10
    _stack->addWidget(_setupEncryptionStep);    // 11

    // Wire step navigation — IntroStart and IntroLogin.
    connect(_startStep, &IntroStep::goNext, this, &IntroWidget::onStartNext);
    connect(_startStep, &IntroStart::createAccountRequested,
            this, [this] { showAccountStep(2); });
    connect(_secretBackendStep, &IntroStep::goBack, this, &IntroWidget::onSecretBackendBack);
    connect(_secretBackendStep, &IntroStep::goNext, this, &IntroWidget::onSecretBackendNext);
    connect(_secretBackendStep, &IntroSecretBackend::secretBackendChosen,
            this, &IntroWidget::secretBackendChosen);
    // Key storage is a screen opened from the stage's "Change" link, not a step
    // in the forward flow, so all three entry screens can reach it and it
    // returns to whichever one did.
    for (auto *step : { static_cast<IntroStep *>(_startStep),
                        static_cast<IntroStep *>(_loginStep),
                        static_cast<IntroStep *>(_registerStep) }) {
        connect(step, &IntroStep::changeKeyStorage,
                this, &IntroWidget::openKeyStorage);
    }
    // Keep the line in step with the choice.
    connect(_secretBackendStep, &IntroSecretBackend::secretBackendChosen,
            this, [this] { refreshKeysLine(); });
    refreshKeysLine();
    // Vault chosen -> in-window create-password form (index 10). Setting the
    // master password is part of the key-storage visit, so it returns wherever
    // that visit started — NOT to Login. Hardcoding 1 here would drop someone
    // who pressed "Change" on the first-run screen onto the sign-in form.
    connect(_secretBackendStep, &IntroSecretBackend::createMasterPassword,
            this, [this] {
        _createPasswordReturnStep = _keyStorageReturnStep;
        showStep(10);
    });
    connect(_createPasswordStep, &IntroCreatePassword::created, this, [this] {
        Q_EMIT secretBackendChosen(true); // persist the vault choice
        // Back to whichever form sent us here (Login, unless Register did).
        showStep(_createPasswordReturnStep);
    });
    connect(_createPasswordStep, &IntroCreatePassword::skipToKeychain, this, [this] {
        // Reverse the vault choice: use the system keychain instead. Pre-login the
        // secret cache is empty, so this is a pure backend flip + vault cleanup.
        const int state = ProtocolBridge::secretStoreState();
        if (state == 2 || state == 3 || state == 4) {
            ProtocolBridge::secretStoreSwitchBackend(0, QString());
        }
        Q_EMIT secretBackendChosen(false);
        showStep(_createPasswordReturnStep);
    });
    connect(_loginStep, &IntroStep::goNext, this, &IntroWidget::onLoginNext);
    connect(_loginStep, &IntroStep::goBack, this, &IntroWidget::onLoginBack);
    connect(_loginStep, &IntroLogin::goRegister, this, &IntroWidget::onLoginGoRegister);
    connect(_loginStep, &IntroLogin::goForgotPassword, this, &IntroWidget::onLoginGoForgotPassword);
    // Backstop for a store that became unusable while the form was open (the
    // ordinary case is handled before the form, in showAccountStep): set the
    // vault up, then come back.
    connect(_loginStep, &IntroLogin::needMasterPassword, this, [this] {
        _createPasswordReturnStep = 1;
        showStep(10);
    });

    // Wire forgot password step.
    connect(_forgotPasswordStep, &IntroStep::goBack, this, &IntroWidget::onForgotPasswordBack);
    connect(_forgotPasswordStep, &IntroForgotPassword::passwordResetSuccess, this, &IntroWidget::onPasswordResetSuccess);
    connect(_forgotPasswordStep, &IntroForgotPassword::goSignIn, this, &IntroWidget::onForgotPasswordBack);

    // Wire register step.
    connect(_registerStep, &IntroStep::goBack, this, &IntroWidget::onRegisterBack);
    connect(_registerStep, &IntroRegister::registerSuccess, this, &IntroWidget::onRegisterSuccess);
    connect(_registerStep, &IntroRegister::goSignIn, this, &IntroWidget::onRegisterBack);
    connect(_registerStep, &IntroRegister::needMasterPassword, this, [this] {
        _createPasswordReturnStep = 2;
        showStep(10);
    });

    // Wire verification choice.
    connect(_verifyChoiceStep, &IntroStep::goBack, this, &IntroWidget::onVerifyChoiceBack);
    connect(_verifyChoiceStep, &IntroVerifyChoice::emojiChosen, this, &IntroWidget::onEmojiChosen);
    connect(_verifyChoiceStep, &IntroVerifyChoice::recoveryKeyChosen, this, &IntroWidget::onRecoveryKeyChosen);
    connect(_verifyChoiceStep, &IntroVerifyChoice::skipVerification, this, &IntroWidget::onSkipVerification);
    connect(_verifyChoiceStep, &IntroVerifyChoice::qrChosen, this, &IntroWidget::onQrChosen);
    connect(_verifyChoiceStep, &IntroVerifyChoice::setupEncryptionNeeded,
            this, &IntroWidget::onSetupEncryptionNeeded);

    // Wire encryption setup.
    connect(_setupEncryptionStep, &IntroSetupEncryption::done,
            this, &IntroWidget::onSetupEncryptionDone);
    connect(_setupEncryptionStep, &IntroSetupEncryption::skipped,
            this, &IntroWidget::onSetupEncryptionSkipped);

    // Wire emoji verification.
    connect(_verifyEmojiStep, &IntroStep::goBack, this, &IntroWidget::onEmojiBack);
    connect(_verifyEmojiStep, &IntroVerifyEmoji::mismatch, this, &IntroWidget::onEmojiMismatch);
    connect(_verifyEmojiStep, &IntroVerifyEmoji::useRecoveryKeyVerification,
            this, &IntroWidget::onEmojiUseRecoveryKey);
    connect(_verifyEmojiStep, &IntroVerifyEmoji::useQrVerification,
            this, &IntroWidget::onEmojiUseQr);
    connect(_verifyEmojiStep, &IntroVerifyEmoji::skipVerification,
            this, &IntroWidget::onEmojiSkipVerification);
    connect(_verifyEmojiStep, &IntroVerifyEmoji::verified, this, &IntroWidget::onEmojiVerified);

    // Wire QR verification.
    connect(_verifyQrStep, &IntroStep::goBack, this, &IntroWidget::onQrBack);
    connect(_verifyQrStep, &IntroVerifyQr::useEmojiVerification,
            this, &IntroWidget::onQrUseEmoji);
    connect(_verifyQrStep, &IntroVerifyQr::useRecoveryKeyVerification,
            this, &IntroWidget::onQrUseRecoveryKey);
    connect(_verifyQrStep, &IntroVerifyQr::skipVerification,
            this, &IntroWidget::onQrSkipVerification);
    connect(_verifyQrStep, &IntroVerifyQr::verified, this, &IntroWidget::onQrVerified);
    connect(_bridge, &ProtocolBridge::sasEmojisAvailable, this,
            [this](const QString &flowId,
                   const QStringList &emojis,
                   const QStringList &labels) {
        if (_stack->currentWidget() != _verifyQrStep) {
            return;
        }
        // Peer picked emoji while our QR was up — follow them rather than
        // leaving a QR code nobody will scan.
        _verifyEmojiStep->presentAdoptedSas(flowId, emojis, labels);
        showStep(4); // QR -> Emoji.
    });

    // Wire recovery key verification.
    connect(_verifyRecoveryKeyStep, &IntroStep::goBack, this, &IntroWidget::onRecoveryKeyBack);
    connect(_verifyRecoveryKeyStep, &IntroVerifyRecoveryKey::useEmojiVerification,
            this, &IntroWidget::onRecoveryKeyUseEmoji);
    connect(_verifyRecoveryKeyStep, &IntroVerifyRecoveryKey::useQrVerification,
            this, &IntroWidget::onRecoveryKeyUseQr);
    connect(_verifyRecoveryKeyStep, &IntroVerifyRecoveryKey::skipVerification,
            this, &IntroWidget::onRecoveryKeySkipVerification);
    connect(_verifyRecoveryKeyStep, &IntroVerifyRecoveryKey::verified, this, &IntroWidget::onRecoveryKeyVerified);

    // Wire success screen.
    connect(_verifySuccessStep, &IntroStep::goNext, this, &IntroWidget::onVerifySuccessNext);

    // Listen for login result — intercept to navigate to verification.
    connect(_bridge, &ProtocolBridge::loginResult,
            this, [this](bool success, const QString &userId, const QString & /*displayName*/, const QString & /*avatarUrl*/) {
        if (success && _stack->currentWidget() == _loginStep) {
            _pendingUserId = userId;
            showStep(3); // Navigate to IntroVerifyChoice.
        }
    });

    if (initialStep == InitialStep::Login) {
        showAccountStep(1);
    } else {
        showStep(0);
    }
}

void IntroWidget::setInnerFocus() {
    if (auto *step = qobject_cast<IntroStep *>(_stack->currentWidget())) {
        step->activate();
    }
}

void IntroWidget::showStep(int index) {
    // Secret-service availability and the current choice are both live state —
    // a keyring can start after launch, and the choice changes mid-flow — so the
    // keys line is re-evaluated on every step change rather than only at build.
    refreshKeysLine();
    _stack->setCurrentIndex(index);
    if (auto *step = qobject_cast<IntroStep *>(_stack->currentWidget())) {
        step->activate();
    }
}

// --- Navigation handlers ---

void IntroWidget::showAccountStep(int index) {
    // Only a missing master password reorders the flow. A keychain that merely
    // refused a read (macOS/Windows) is retryable, so those still go to the form
    // and report inline on submit — bouncing them to a vault setup would move the
    // device off a working keychain over a hiccup.
    if (AppController::checkSecretBackendForNewSession()
            != AppController::SecretSetup::NeedsMasterPassword) {
        showStep(index);
        return;
    }
    _createPasswordReturnStep = index;
    showStep(10);
}

void IntroWidget::onStartNext() {
    // Straight to sign-in — unless the vault still needs its master password.
    // Which backend holds the secrets is NOT a step in the forward flow (it has
    // a sensible default and lives behind the "Keys: … Change" line), but a
    // vault with no password can't take the secrets sign-in is about to write,
    // so that one screen comes first.
    showAccountStep(1);
}

void IntroWidget::onSecretBackendBack() {
    showStep(_keyStorageReturnStep);
}

void IntroWidget::onSecretBackendNext() {
    showStep(_keyStorageReturnStep);
}

void IntroWidget::openKeyStorage() {
    // Remember where we came from: this screen is reachable from first run,
    // sign in and create, and must return to whichever asked.
    _keyStorageReturnStep = _stack->currentIndex();
    showStep(9);
}

void IntroWidget::setEmbedded(bool embedded) {
    _embedded = embedded;
    for (int i = 0; i != _stack->count(); ++i) {
        if (auto *step = qobject_cast<IntroStep *>(_stack->widget(i))) {
            step->setShowsVersion(!embedded);
        }
    }
    refreshKeysLine();
}

void IntroWidget::refreshKeysLine() {
    const auto label = _secretBackendStep
        ? _secretBackendStep->choiceLabel()
        : QString();
    // With no Secret Service there is no choice to offer: the private vault is
    // the only backend, so a "Change" link would open a screen with one
    // selectable option. Hide the whole line rather than advertise a decision
    // that cannot be made. (The master password is still asked for before
    // sign-in or sign-up — see showAccountStep.)
    const auto hasChoice = !_embedded
        && ProtocolBridge::secretServiceAvailable();
    // Per the design the line shows on first run, sign in and create only —
    // not on reset, and not once the flow has moved past account entry.
    for (auto *step : { static_cast<IntroStep *>(_startStep),
                        static_cast<IntroStep *>(_loginStep),
                        static_cast<IntroStep *>(_registerStep) }) {
        if (step) {
            step->setKeysLabel(label);
            step->setShowsKeysLine(hasChoice);
        }
    }
}

void IntroWidget::onLoginBack() {
    showStep(0); // Login -> Welcome.
}

void IntroWidget::onLoginNext() {
    // Login succeeded — the loginResult handler above navigates to verification.
}

void IntroWidget::onLoginGoRegister() {
    showAccountStep(2); // Login -> Register.
}

void IntroWidget::onLoginGoForgotPassword() {
    showStep(7); // Login -> Forgot Password.
}

void IntroWidget::onForgotPasswordBack() {
    showStep(1); // Forgot Password -> Login.
}

void IntroWidget::onPasswordResetSuccess() {
    showStep(1); // Forgot Password (success) -> Login.
}

void IntroWidget::onRegisterBack() {
    // Fix #17: deactivate so late in-flight results are ignored.
    _registerStep->deactivate();
    showStep(1); // Register -> Login.
}

void IntroWidget::onRegisterSuccess(
        const QString &userId,
        const QString &displayName,
        const QString &avatarUrl) {
    _pendingUserId = userId;
    Q_EMIT registrationAccepted(userId, displayName, avatarUrl);
    // Straight to encryption setup, not verification: a brand-new account has no other session to
    // verify against and no recovery key to enter, so the verify step could only be skipped.
    showStep(11); // Register -> Set up encryption.
}

void IntroWidget::onVerifyChoiceBack() {
    showStep(1); // Verify choice -> Login.
}

void IntroWidget::onEmojiChosen() {
    // A stale incoming request from an earlier entry must not be re-answered
    // when the user explicitly picked emoji from the choice screen.
    _verifyEmojiStep->setRequestFlowId(QString());
    showStep(4); // Verify choice -> Emoji.
}

void IntroWidget::onRecoveryKeyChosen() {
    showStep(5); // Verify choice -> Recovery key.
}

void IntroWidget::onRecoveryKeyUseEmoji() {
    _verifyRecoveryKeyStep->deactivate();
    showStep(4); // Recovery key -> Emoji.
}

void IntroWidget::onRecoveryKeyUseQr() {
    _verifyRecoveryKeyStep->deactivate();
    showStep(8); // Recovery key -> QR.
}

void IntroWidget::onRecoveryKeySkipVerification() {
    _verifyRecoveryKeyStep->deactivate();
    onSkipVerification();
}

void IntroWidget::onSkipVerification() {
    // Skip verification — proceed to main app.
    _bridge->skipVerification();
    Q_EMIT loginSuccess(_pendingUserId);
}

void IntroWidget::onEmojiBack() {
    _bridge->cancelVerification();
    showStep(3); // Emoji → Verify choice.
}

void IntroWidget::onEmojiMismatch() {
    _bridge->mismatchSas();
    showStep(3); // Emoji mismatch → Verify choice.
}

void IntroWidget::onEmojiUseRecoveryKey() {
    showStep(5); // Emoji -> Recovery key.
    _bridge->cancelVerification();
}

void IntroWidget::onEmojiUseQr() {
    // Cancel the emoji SAS BEFORE switching: showStep(8) activates the QR step,
    // which starts a fresh QR flow — cancelling after would tear down the new QR
    // instead of the old SAS. The QR page ignores the (stale) SAS Cancelled until
    // it owns its own flow.
    _bridge->cancelVerification();
    showStep(8); // Emoji -> QR.
}

void IntroWidget::onEmojiSkipVerification() {
    _bridge->cancelVerification();
    onSkipVerification();
}

void IntroWidget::onEmojiVerified() {
    showStep(6); // Emoji verified -> Success.
}

void IntroWidget::onQrChosen() {
    showStep(8); // Verify choice -> QR.
}

void IntroWidget::onQrBack() {
    _bridge->cancelVerification();
    showStep(3); // QR -> Verify choice.
}

void IntroWidget::onQrUseEmoji() {
    // Tell the emoji step to ignore the QR flow's Cancelled (emitted by the
    // cancel below) so it doesn't surface as a failure on the freshly-shown
    // emoji page.
    _verifyEmojiStep->ignoreFlow(_verifyQrStep->currentFlowId());
    _bridge->cancelVerification();
    showStep(4); // QR -> Emoji.
}

void IntroWidget::onQrUseRecoveryKey() {
    showStep(5); // QR -> Recovery key.
    _bridge->cancelVerification();
}

void IntroWidget::onQrSkipVerification() {
    _bridge->cancelVerification();
    onSkipVerification();
}

void IntroWidget::onQrVerified() {
    showStep(6); // QR verified -> Success.
}

void IntroWidget::onRecoveryKeyBack() {
    // Fix #15: deactivate before navigating so a late backend result is ignored.
    _verifyRecoveryKeyStep->deactivate();
    showStep(3); // Recovery key -> Verify choice.
}

void IntroWidget::onRecoveryKeyVerified() {
    showStep(6); // Recovery key verified -> Success.
}

void IntroWidget::onVerifySuccessNext() {
    // Verification complete — proceed to main app.
    Q_EMIT loginSuccess(_pendingUserId);
}

void IntroWidget::onSetupEncryptionNeeded() {
    // The account has nothing to verify against and nothing to recover from — offer to set
    // encryption up rather than three verification methods that cannot work.
    showStep(11); // Verify choice -> Set up encryption.
}

void IntroWidget::onSetupEncryptionDone() {
    // The device signs its own keys as it provisions them, so it is genuinely verified — unlike
    // the skip path below, there is no need to tell Rust otherwise.
    _setupEncryptionStep->deactivate();
    Q_EMIT loginSuccess(_pendingUserId);
}

void IntroWidget::onSetupEncryptionSkipped() {
    _setupEncryptionStep->deactivate();
    onSkipVerification();
}

void IntroWidget::paintEvent(QPaintEvent *e) {
    QPainter p(this);
    p.fillRect(rect(), intro::bg);
}

void IntroWidget::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    updateControlsGeometry();
}

void IntroWidget::updateControlsGeometry() {
    _stack->setGeometry(rect());
}

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>
#include <QStackedWidget>

#include <functional>

namespace TeleMatrix {

class ProtocolBridge;
class IntroStep;
class IntroStart;
class IntroSecretBackend;
class IntroCreatePassword;
class IntroLogin;
class IntroRegister;
class IntroVerifyChoice;
class IntroVerifyEmoji;
class IntroVerifyQr;
class IntroVerifyRecoveryKey;
class IntroForgotPassword;
class IntroVerifySuccess;
class IntroSetupEncryption;

/// Container widget that manages the intro flow step navigation.
/// Shows IntroStart first, then IntroLogin on "Start Messaging" click.
/// After login success, navigates to verification flow.
/// Emits loginSuccess when verification completes (or is skipped).
///
/// Provides simplified step navigation.
class IntroWidget : public QWidget {
    Q_OBJECT

public:
    enum class InitialStep {
        Welcome,
        Login,
    };

    explicit IntroWidget(
        ProtocolBridge *bridge,
        InitialStep initialStep = InitialStep::Welcome,
        QWidget *parent = nullptr);

    /// Set focus to the active step.
    void setInnerFocus();

    /// Render as an embedded dialog rather than the full-window first-run
    /// stage: no version line, no key-storage line. The popup is a dialog over
    /// the running app, where both read as stray chrome.
    void setEmbedded(bool embedded);

signals:
    /// Emitted when login + verification succeeds. AppController listens for this.
    void loginSuccess(const QString &userId);
    /// Emitted when registration succeeded and the session should be persisted
    /// before the verification flow continues.
    void registrationAccepted(
        const QString &userId,
        const QString &displayName,
        const QString &avatarUrl);
    /// Emitted from the first-run backend choice step; AppController persists it.
    void secretBackendChosen(bool vault);

protected:
    void resizeEvent(QResizeEvent *e) override;
    void paintEvent(QPaintEvent *e) override;

private:
    void showStep(int index);
    /// Show an account-entry form (Login or Register) — but on a device whose
    /// vault has no master password yet, show the create-password step first and
    /// come back to `index` once it is set. Sign-in and sign-up write secrets the
    /// moment they succeed, so the store has to be ready before the form, not
    /// after it.
    void showAccountStep(int index);
    /// Show the emoji-compare step — the same index onEmojiChosen uses.
    /// Centralizes what every entry into the step must do: deactivate the
    /// recovery-key step (harmless no-op unless it's the one being left; see
    /// Fix #15) so a late result there cannot force navigation back out, and
    /// set whether the alternative-method links show, since that flag sticks
    /// for the step's whole life and must not leak between entries.
    void showVerifyEmojiStep(bool showsAlternatives);
    /// Whether a verification step (choice, emoji, QR, recovery key) is the
    /// one currently showing. Used to decide whether an incoming request
    /// arriving mid-intro should be auto-routed here; sign-in steps
    /// (login/register/password) are not ours to hijack.
    bool onVerificationStep() const;

    // Navigation handlers.
    void onStartNext();
    void onSecretBackendBack();
    void onSecretBackendNext();
    /// Show the key-storage screen, remembering where to return to.
    void openKeyStorage();
    /// Push the current choice into the stage's "Keys:" line on the screens
    /// that show it.
    void refreshKeysLine();
    void onLoginBack();
    void onLoginNext();
    void onLoginGoRegister();
    void onLoginGoForgotPassword();
    void onForgotPasswordBack();
    void onPasswordResetSuccess();
    void onRegisterBack();
    void onRegisterSuccess(
        const QString &userId,
        const QString &displayName,
        const QString &avatarUrl);
    void onVerifyChoiceBack();
    void onEmojiChosen();
    void onRecoveryKeyChosen();
    void onRecoveryKeyUseEmoji();
    void onRecoveryKeyUseQr();
    void onRecoveryKeySkipVerification();
    void onSkipVerification();
    void onEmojiBack();
    void onEmojiMismatch();
    void onEmojiUseRecoveryKey();
    void onEmojiUseQr();
    void onEmojiSkipVerification();
    void onEmojiVerified();
    void onQrChosen();
    void onQrBack();
    void onQrUseEmoji();
    void onQrUseRecoveryKey();
    void onQrSkipVerification();
    void onQrVerified();
    void onRecoveryKeyBack();
    void onRecoveryKeyVerified();
    void onVerifySuccessNext();
    void onSetupEncryptionNeeded();
    void onSetupEncryptionDone();
    void onSetupEncryptionSkipped();

    void updateControlsGeometry();

    ProtocolBridge *_bridge = nullptr;
    QString _pendingUserId;
    // Step to return to once the master password is set (or skipped): the form
    // the user was heading for — Login (1), or Register (2) — or, when the visit
    // came from the key-storage screen, wherever that visit started.
    int _createPasswordReturnStep = 1;
    // Where the key-storage screen returns to. It is opened from the "Change"
    // link rather than reached in sequence, so it has no fixed predecessor.
    int _keyStorageReturnStep = 0;
    bool _embedded = false;

    QStackedWidget *_stack = nullptr;
    IntroStart *_startStep = nullptr;                     // index 0
    IntroLogin *_loginStep = nullptr;                     // index 1
    IntroSecretBackend *_secretBackendStep = nullptr;     // index 9
    IntroCreatePassword *_createPasswordStep = nullptr;   // index 10
    IntroRegister *_registerStep = nullptr;               // index 2
    IntroVerifyChoice *_verifyChoiceStep = nullptr;       // index 3
    IntroVerifyEmoji *_verifyEmojiStep = nullptr;         // index 4
    IntroVerifyRecoveryKey *_verifyRecoveryKeyStep = nullptr; // index 5
    IntroVerifySuccess *_verifySuccessStep = nullptr;     // index 6
    IntroForgotPassword *_forgotPasswordStep = nullptr;  // index 7
    IntroVerifyQr *_verifyQrStep = nullptr;              // index 8
    IntroSetupEncryption *_setupEncryptionStep = nullptr; // index 11
};

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "intro_step.h"

class QLineEdit;
class QPushButton;
class QLabel;

namespace TeleMatrix {

class ProtocolBridge;

/// Forgot-password flow step -- 3 states:
/// 1. Enter email address and homeserver, send reset email
/// 2. Wait for user to verify email, click "I've verified my email"
/// 3. Enter new password + confirm, submit reset
class IntroForgotPassword : public IntroStep {
    Q_OBJECT

public:
    explicit IntroForgotPassword(IntroWidget *parent, ProtocolBridge *bridge);

    void activate() override;
    void submit() override;
    QString nextButtonText() const override;

signals:
    /// Emitted when password reset succeeds (navigate back to login).
    void passwordResetSuccess();
    /// Emitted when user clicks "Sign in" link.
    void goSignIn();

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    enum class State { EnterEmail, WaitingForEmail, EnterNewPassword };
    void setState(State state);
    void updateFieldLayout();
    void relayout() override { updateFieldLayout(); }
    void onPasswordResetTokenSent(bool success, const QString &sid,
                                   const QString &clientSecret, const QString &error);
    void onPasswordResetComplete(bool success, const QString &error);
    void setFieldsEnabled(bool enabled);
    void updateSubmitEnabled();
    /// Show "reset it on the server's website", and go find the actual page so
    /// the message can become a link. Safe to call before the URL is known.
    void showDelegatedResetNotice();
    void onPasswordResetPageProbed(quint64 requestId, bool available, const QString &url);
    void togglePasswordVisibility();
    void toggleConfirmPasswordVisibility();
    QLineEdit *createField(const QString &placeholder);

    ProtocolBridge *_bridge = nullptr;
    State _state = State::EnterEmail;
    QString _homeserver;
    /// Correlates the in-flight reset-page probe; a late answer for a homeserver
    /// the user has since changed must not rewrite the current message.
    quint64 _resetPageRequestId = 0;
    QString _sid;
    QString _clientSecret;

    QLineEdit *_homeserverField = nullptr;
    QLineEdit *_emailField = nullptr;
    QLineEdit *_newPasswordField = nullptr;
    QLineEdit *_confirmPasswordField = nullptr;
    QPushButton *_passwordToggle = nullptr;
    QPushButton *_confirmPasswordToggle = nullptr;
    QLabel *_instructionLabel = nullptr;
    QPushButton *_emailConfirmedButton = nullptr;
    QWidget *_sentStrip = nullptr;   // "Sent to <address> · Resend"
    QPushButton *_signInLink = nullptr;

    bool _submitting = false;
};

} // namespace TeleMatrix

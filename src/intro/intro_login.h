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

/// Login form step -- homeserver URL, username, and password fields.
/// Calls ProtocolBridge::login() on submit and handles the result.
/// Features homeserver auto-discovery via .well-known and password visibility toggle.
class IntroLogin : public IntroStep {
    Q_OBJECT

public:
    explicit IntroLogin(IntroWidget *parent, ProtocolBridge *bridge);

    void activate() override;
    void submit() override;
    QString nextButtonText() const override;

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
signals:
    /// Emitted when user clicks "Create account" link.
    void goRegister();
    /// Emitted when user clicks "Forgot password?" link.
    void goForgotPassword();
    /// No usable secret store: the submit was held so the user can set up the
    /// master-password vault on the in-window create-password step.
    void needMasterPassword();

private:
    void onLoginResult(
        bool success,
        const QString &userId,
        const QString &displayName,
        const QString &avatarUrl);
    void onHomeserverDiscovered(quint64 requestId, bool success, const QString &url);
    void setFieldsEnabled(bool enabled);
    [[nodiscard]] bool allFieldsFilled() const;
    void updateSubmitEnabled();
    void updateFieldLayout();
    void relayout() override { updateFieldLayout(); }
    void startDiscovery();
    void togglePasswordVisibility();
    QLineEdit *createField(const QString &placeholder);

    ProtocolBridge *_bridge = nullptr;

    QLineEdit *_homeserver = nullptr;
    QLineEdit *_username = nullptr;
    QLineEdit *_password = nullptr;
    QPushButton *_passwordToggle = nullptr;
    QPushButton *_createAccountLink = nullptr;
    QPushButton *_forgotPasswordLink = nullptr;

    bool _submitting = false;
    bool _discovering = false;
    QString _discoveredUrl;
    quint64 _pendingDiscoveryRequestId = 0;
    quint64 _nextDiscoveryRequestId = 1;
};

} // namespace TeleMatrix

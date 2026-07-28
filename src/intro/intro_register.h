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
class QTimer;

namespace TeleMatrix {

class ProtocolBridge;

/// Registration form step. Two-phase: first only the homeserver + "Continue";
/// on submit the server is classified, then either the username/password fields
/// (password registration) or a "register on the website" notice (OIDC/MAS) is
/// shown, or an error if the URL is not a Matrix server.
/// Handles UIA challenges (terms, registration token, dummy) inline.
/// Reuses login step visual style for consistency.
class IntroRegister : public IntroStep {
    Q_OBJECT

public:
    explicit IntroRegister(QWidget *parent, ProtocolBridge *bridge);

    void activate() override;
    /// Called when navigating away from this step. Prevents stale backend
    /// callbacks from forcing navigation after the user backed out (#17).
    void deactivate();
    void submit() override;
    QString nextButtonText() const override;

signals:
    /// Emitted when registration succeeds (navigate to verification).
    void registerSuccess(
        const QString &userId,
        const QString &displayName,
        const QString &avatarUrl);
    /// Emitted when user clicks "Sign in" link.
    void goSignIn();
    /// No usable secret store: the submit was held so the user can set up the
    /// master-password vault on the in-window create-password step.
    void needMasterPassword();

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    // Two-phase registration: enter a homeserver, then (once classified) either
    // fill credentials or follow a link to the server's own signup page.
    enum class Phase { Homeserver, Credentials, Delegated };

    void onRegistrationSuccess(
        const QString &userId,
        const QString &displayName,
        const QString &avatarUrl);
    void onRegistrationChallenge(const QString &challengeJson);
    void onRegistrationFailed(const QString &error);
    void onUsernameAvailability(int status, const QString &message);
    void onRegistrationClassified(quint64 requestId, int status, const QString &url);

    void setFieldsEnabled(bool enabled);
    void updateFieldLayout();
    void relayout() override { updateFieldLayout(); }
    /// Kick off the server classification (from "Continue" on the homeserver phase).
    void classifyServer();
    /// Switch phase: (un)hide the relevant controls and re-lay-out.
    void setPhase(Phase phase);
    void updateSubmitEnabled();
    void startUsernameCheck();
    void submitUiaStage(const QString &stageType, const QString &authJson);
    void showUiaStage(const QString &stageType, const QJsonObject &params);
    void setUiaControlsEnabled(bool enabled);
    void hideUiaWidgets();
    void togglePasswordVisibility();
    void toggleConfirmPasswordVisibility();
    QLineEdit *createField(const QString &placeholder);

    ProtocolBridge *_bridge = nullptr;

    // Form fields.
    QLineEdit *_homeserver = nullptr;
    QLineEdit *_username = nullptr;
    QLineEdit *_password = nullptr;
    QLineEdit *_confirmPassword = nullptr;
    QPushButton *_passwordToggle = nullptr;
    QPushButton *_confirmPasswordToggle = nullptr;
    QPushButton *_signInLink = nullptr;
    // "Change" action inside the homeserver field (shown past the first phase):
    // returns to the homeserver step so the locked homeserver can be edited.
    QPushButton *_changeHomeserver = nullptr;

    // Username availability indicator.
    QLabel *_usernameStatus = nullptr;

    // Notice shown in the Delegated phase: text with a link to the server's
    // own registration website.
    QLabel *_delegatedInfo = nullptr;

    // UIA state.
    QString _uiaSession;
    QString _currentHomeserver;
    QWidget *_uiaContainer = nullptr;

    // UIA stage widgets (created on demand).
    QLabel *_uiaLabel = nullptr;
    QLineEdit *_tokenField = nullptr;
    QPushButton *_uiaSubmitButton = nullptr;

    bool _submitting = false;

    // Current phase + in-flight server classification (the "Continue" check).
    Phase _phase = Phase::Homeserver;
    bool _checking = false;
    quint64 _pendingClassifyRequestId = 0;
    quint64 _nextClassifyRequestId = 1;

    // Resolved homeserver base URL (classify status 1); the account is created
    // against this.
    QString _discoveredUrl;
    // MSC3861/MAS: the server's own signup website (classify status 2 =
    // Delegated). Accounts on such servers are created there, not here.
    QString _delegatedAuthUrl;
    // Fix #17: true while this step is the active visible page.
    // registerSuccess / registrationChallenge are ignored when false so a
    // late in-flight response cannot force navigation after the user left.
    bool _active = false;

    // Cached credentials from initial submit — used in UIA continuation
    // to prevent race if user modifies fields between submit and UIA stage.
    QString _cachedUsername;
    QString _cachedPassword;

    // Fix #19: last username for which a check was dispatched.
    // onUsernameAvailability() compares against the current field value and
    // discards stale results where the user has already typed something else.
    QString _lastCheckedUsername;
    bool _usernameAvailabilityErrorVisible = false;
};

} // namespace TeleMatrix

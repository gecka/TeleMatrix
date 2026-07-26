// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "intro_step.h"

class QLineEdit;
class QPushButton;

namespace TeleMatrix {

class ProtocolBridge;

/// Recovery key verification step.
/// Allows the user to verify their session by entering a recovery key.
class IntroVerifyRecoveryKey : public IntroStep {
    Q_OBJECT

public:
    explicit IntroVerifyRecoveryKey(IntroWidget *parent, ProtocolBridge *bridge);

    void activate() override;
    void showError(const QString &text);
    void hideError();
    /// Called when navigating away from this step. Prevents stale backend
    /// results from forcing navigation after the user has backed out (#15).
    void deactivate();
    void submit() override;
    QString nextButtonText() const override;

signals:
    void verified();
    void useEmojiVerification();
    void useQrVerification();
    void skipVerification();

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    void onVerifyResult(bool success);
    void updateSubmitState();
    void updateKeyLayout();

    ProtocolBridge *_bridge = nullptr;

    QLineEdit *_keyInput = nullptr;
    QPushButton *_emojiLink = nullptr;
    QPushButton *_qrLink = nullptr;
    QPushButton *_skipLink = nullptr;

    bool _submitting = false;
    // Fix #15: set true while this step is the visible stack page.
    // onVerifyResult() is ignored when false, preventing a stale backend
    // completion from forcing navigation after the user already backed out.
    bool _active = false;
};

} // namespace TeleMatrix

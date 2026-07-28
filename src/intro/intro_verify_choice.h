// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "intro_step.h"

class QLabel;
class QPushButton;
class QTimer;

namespace TeleMatrix {

class ProtocolBridge;

class IntroVerifyChoice : public IntroStep {
    Q_OBJECT

public:
    explicit IntroVerifyChoice(QWidget *parent, ProtocolBridge *bridge);

    void activate() override;
    void submit() override;
    QString nextButtonText() const override;

signals:
    void qrChosen();
    void emojiChosen();
    void recoveryKeyChosen();
    void skipVerification();
    /// The account has no other session and no recovery key, so none of the three methods here
    /// can work. Set encryption up instead.
    void setupEncryptionNeeded();

protected:
    void resizeEvent(QResizeEvent *e) override;
    void updateSkipVisibility() override;

private:
    void updateChoiceLayout();
    void onCapabilitiesReady(
        bool success, bool canDevice, bool canRecovery, bool sasOk, bool qrSupported);
    void setCardEnabled(QPushButton *card, bool enabled);
    void revealChoices();

    ProtocolBridge *_bridge = nullptr;

    QPushButton *_qrCard = nullptr;
    QPushButton *_emojiCard = nullptr;
    QPushButton *_recoveryCard = nullptr;
    QPushButton *_skipButton = nullptr;
    QLabel *_checkingLabel = nullptr;

    bool _qrEnabled = true;
    bool _emojiEnabled = true;
    bool _recoveryEnabled = true;

    // The cards stay hidden until the capability probe answers, because the answer can send the
    // user to a different step entirely and a card flash on the way there looks like a glitch.
    bool _checking = false;
    QTimer *_checkingLabelTimer = nullptr;
    QTimer *_checkingTimeout = nullptr;
};

} // namespace TeleMatrix

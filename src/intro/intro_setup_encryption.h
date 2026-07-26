// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "intro_step.h"

class QCheckBox;
class QLabel;

namespace TeleMatrix {

class ProtocolBridge;

/// Provisions encryption for an account that has none — key backup plus secret storage — and
/// shows the user the recovery key it produced, which is the only moment that key exists in
/// plaintext. Replaces the verification step for a brand-new account, which has no other session
/// to verify against and nothing to recover from.
class IntroSetupEncryption : public IntroStep {
    Q_OBJECT

public:
    explicit IntroSetupEncryption(IntroWidget *parent, ProtocolBridge *bridge);

    void activate() override;
    void deactivate();
    void submit() override;
    QString nextButtonText() const override;

signals:
    /// The key is set up (or the user declined to save it) — carry on into the app.
    void done();
    /// The user chose not to set encryption up at all.
    void skipped();

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    enum class State {
        Working,      // Waiting for the homeserver.
        Ready,        // Key in hand, shown to the user.
        BackupExists, // An unusable key backup is in the way; needs the user's consent to clear.
        Failed,
    };

    void onSetupResult(bool success, const QString &recoveryKey, int errorCode,
                       const QString &error);
    void setState(State state);
    void startSetup();
    void copyKey();
    void updateContinueState();
    void updateSetupLayout();

    ProtocolBridge *_bridge = nullptr;
    State _state = State::Working;
    QString _key;
    // Cleared while the step is not on screen, so a late reply from a flow the user has already
    // left cannot drive navigation.
    bool _active = false;
    // activate() can be called again on a step that is already showing (a focus restore, say).
    // Provisioning twice would mint a second recovery key and silently invalidate the one on
    // screen, so the request happens once per visit.
    bool _setupRequested = false;

    QLabel *_statusLabel = nullptr;
    QLabel *_keyLabel = nullptr;
    QPushButton *_copyButton = nullptr;
    QCheckBox *_savedCheckbox = nullptr;
    QPushButton *_skipLink = nullptr;
};

} // namespace TeleMatrix

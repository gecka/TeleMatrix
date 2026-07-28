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

// In-window "Create a master password" form, shown after the user picks the
// private vault on IntroSecretBackend (in place of a modal popup). Two fields
// with a confirm; the Continue button is disabled while the password is empty
// and shows a busy state while the key is derived (Argon2id, ~1s, off-thread).
class IntroCreatePassword final : public IntroStep {
    Q_OBJECT

public:
    explicit IntroCreatePassword(QWidget *parent);

    void activate() override;
    void submit() override;
    [[nodiscard]] QString nextButtonText() const override;

signals:
    // The master password was set; the vault is now unlocked.
    void created();
    // The user chose to skip the vault and use the system keychain instead.
    void skipToKeychain();

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    void updateFieldLayout();
    void updateButtonEnabled();
    void togglePasswordVisibility();

    QLineEdit *_password = nullptr;
    QLineEdit *_confirm = nullptr;
    QPushButton *_passwordToggle = nullptr;
    QPushButton *_skipLink = nullptr; // null when no system keychain is available
};

} // namespace TeleMatrix

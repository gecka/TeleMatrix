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

// Welcome-screen-style master-password unlock, shown full-window at startup when
// a saved session's vault is locked (in place of a modal box, so it reads as part
// of the intro flow). Verifies the password against the vault itself, so a wrong
// one shows an inline error rather than silently re-prompting; the Unlock button
// is disabled while the field is empty. Hosted by AppController via a nested loop,
// not the IntroWidget stack, so its IntroWidget parent is null.
class IntroVaultUnlock final : public IntroStep {
    Q_OBJECT

public:
    explicit IntroVaultUnlock(IntroWidget *parent = nullptr);

    void activate() override;
    void submit() override;
    [[nodiscard]] QString nextButtonText() const override;

signals:
    // The password matched and the vault is unlocked.
    void unlocked();
    // The user asked to reset local data (host confirms + performs it).
    void resetRequested();

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    void updateFieldLayout();
    void updateButtonEnabled();
    void togglePasswordVisibility();

    QLineEdit *_password = nullptr;
    QPushButton *_passwordToggle = nullptr;
    QPushButton *_resetLink = nullptr;
};

} // namespace TeleMatrix

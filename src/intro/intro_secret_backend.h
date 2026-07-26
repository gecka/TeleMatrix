// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "intro_step.h"

class QPushButton;

namespace TeleMatrix {

// First-run step (after the welcome screen) letting the user choose how local
// secrets are protected: the OS keychain or a master-password vault. Choosing the
// vault sets the master password before continuing to login; choosing the
// keychain migrates back if a vault was set up. On a system
// with no keychain the vault is the only option.
class IntroSecretBackend : public IntroStep {
    Q_OBJECT

public:
    explicit IntroSecretBackend(IntroWidget *parent);

    void activate() override;
    void submit() override;
    QString nextButtonText() const override;

    /// Label for the stage's "Keys: <choice>" line — the current selection, in
    /// the same words the cards use.
    [[nodiscard]] QString choiceLabel() const;
    [[nodiscard]] bool vaultSelected() const { return _vaultSelected; }

signals:
    // Forwarded up so the choice is persisted at device level (survives logout).
    void secretBackendChosen(bool vault);
    // Vault chosen but not yet unlocked: show the in-window create-password step.
    void createMasterPassword();

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    void updateChoiceLayout();
    void select(bool vault);

    QPushButton *_keychainCard = nullptr;
    QPushButton *_vaultCard = nullptr;
    bool _vaultSelected = false;
    bool _keychainAvailable = true;
};

} // namespace TeleMatrix

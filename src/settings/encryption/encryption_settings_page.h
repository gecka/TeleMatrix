// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "protocol/protocol_types.h"
#include "settings/settings_page.h"

namespace TeleMatrix {

class AppController;

class EncryptionSettingsPage final : public SettingsScrollPage {
    Q_OBJECT

public:
    explicit EncryptionSettingsPage(
        AppController *controller,
        QWidget *parent = nullptr);

    void refreshOverview();

Q_SIGNALS:
    void sessionsRefreshRequested();

private:
    void rebuildUi(const EncryptionOverview &overview);
    void openVerifySessionDialog();
    void enterRecoveryKey();
    void resetIdentity();
    void showActionPreloader(const QString &title, const QString &text);
    void hideActionPreloader();

    AppController *_controller = nullptr;
    EncryptionOverview _lastOverview;
    QWidget *_actionPreloader = nullptr;
    bool _recoveryKeyChangeInProgress = false;
};

} // namespace TeleMatrix

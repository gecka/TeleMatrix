// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "settings/settings_page.h"

#include <QVector>

class QVBoxLayout;

namespace TeleMatrix {

class AppController;
class SettingsValueButton;
class SettingsChoiceRow;
class SettingsToggleButton;

namespace Core {
class Settings;
} // namespace Core

class HelpAboutSettingsPage final : public SettingsScrollPage {
    Q_OBJECT

public:
    explicit HelpAboutSettingsPage(
        AppController *controller,
        Core::Settings *settings,
        QWidget *parent = nullptr);

private:
    void setupUpdateSection(QWidget *content, QVBoxLayout *layout);
    /// Re-render the status row from the update service's current state.
    void refreshUpdateRow();
    void setPolicy(int policy);
    void setInstallBetaVersions(bool beta);
    /// Click handler for the status row — what it does depends on that state.
    void onUpdateRowClicked();

    AppController *_controller = nullptr;
    Core::Settings *_settings = nullptr;

    SettingsValueButton *_updateRow = nullptr;
    QVector<SettingsChoiceRow *> _policyRows;
    SettingsToggleButton *_betaToggle = nullptr;

    // Transient state the service doesn't keep: the last error, and whether the
    // last check came back clean (so "Up to date" can be shown once).
    QString _lastError;
    bool _checkedClean = false;
    quint64 _received = 0;
    quint64 _total = 0;
};

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "settings/settings_page.h"

class QLabel;
class QSlider;

namespace TeleMatrix {

class AppController;
namespace Core { class Settings; }

class PreferencesSettingsPage final : public SettingsScrollPage {
    Q_OBJECT

public:
    PreferencesSettingsPage(
        AppController *controller,
        Core::Settings *settings,
        QWidget *parent = nullptr);

    void refreshCacheStats();

private:
    AppController *_controller = nullptr;
    QLabel *_cacheSizeLabel = nullptr;
    QLabel *_cacheDetailLabel = nullptr;
    QSlider *_cacheLimitSlider = nullptr;
    QLabel *_cacheLimitValueLabel = nullptr;
};

} // namespace TeleMatrix

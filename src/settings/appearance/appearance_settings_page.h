// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "settings/settings_page.h"

namespace TeleMatrix {

class AppController;
namespace Core { class Settings; }

class AppearanceSettingsPage final : public SettingsScrollPage {
    Q_OBJECT

public:
    AppearanceSettingsPage(
        AppController *controller,
        Core::Settings *settings,
        QWidget *parent = nullptr);
};

} // namespace TeleMatrix

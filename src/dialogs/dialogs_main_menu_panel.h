// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>

class QVBoxLayout;
class QVariantAnimation;

namespace TeleMatrix {

class AppController;

class DialogsMainMenuPanel final : public QWidget {
    Q_OBJECT

public:
    explicit DialogsMainMenuPanel(
        AppController *controller,
        QWidget *parent = nullptr);

    [[nodiscard]] QSize sizeHint() const override;

    // Set the night mode toggle state without emitting nightModeToggled.
    void setNightModeChecked(bool checked);

    void applyTheme();

Q_SIGNALS:
    void newRoomClicked();
    void newChatClicked();
    void exploreRoomsClicked();
    void savedMessagesClicked();
    void verifySessionClicked();
    void settingsClicked();
    /// Open the theme picker directly, without going through Settings.
    void colorThemeClicked();
    void nightModeToggled(bool enabled);
    void signOutClicked();
    /// The user picked another account from the switcher.
    void accountSwitchRequested(int accountIndex);
    /// The user asked to sign in with an additional account.
    void addAccountClicked();

private:
    /// Rebuild the switcher's rows from the current account list, returning the
    /// height they need (summed as they are built — the layout's own hint is not
    /// up to date until it has been activated).
    [[nodiscard]] int rebuildAccountsList();

    AppController *_controller = nullptr;
    QWidget *_nightModeRow = nullptr;
    QWidget *_header = nullptr;
    QWidget *_accountsWrap = nullptr;
    QVBoxLayout *_accountsLayout = nullptr;
    QVariantAnimation *_accountsAnimation = nullptr;
    bool _accountsExpanded = false;
};

} // namespace TeleMatrix

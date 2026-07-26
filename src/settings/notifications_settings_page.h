// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "protocol/protocol_types.h"
#include "settings/settings_page.h"

class QPlainTextEdit;

namespace TeleMatrix {

class AppController;
class ProtocolBridge;
class SettingsToggleButton;
namespace Core { class Settings; }

class NotificationsSettingsPage final : public SettingsScrollPage {
    Q_OBJECT

public:
    NotificationsSettingsPage(
        AppController *controller,
        Core::Settings *settings,
        QWidget *parent = nullptr);

    // Re-fetch the account-global chat settings from the server (called each time
    // the page is shown, so keywords/levels can't go stale).
    void refresh();

Q_SIGNALS:
    void settingsChanged();

private:
    // Apply the account-global settings snapshot from the backend to the controls.
    void applyNotificationSettings(
        RoomNotificationMode dmLevel,
        RoomNotificationMode roomLevel,
        bool mentionDisplayName,
        bool mentionUsername,
        bool mentionRoom,
        bool keywordsEnabled,
        const QString &keywordsCsv);

    AppController *_controller = nullptr;
    ProtocolBridge *_bridge = nullptr;
    // On = AllMessages, Off = MentionsOnly (mentions/keywords still notify).
    SettingsToggleButton *_dmToggle = nullptr;
    SettingsToggleButton *_roomToggle = nullptr;
    // "Mentions & keywords" master toggles (each = a default push rule's enabled flag).
    SettingsToggleButton *_displayNameToggle = nullptr;
    SettingsToggleButton *_usernameToggle = nullptr;
    SettingsToggleButton *_roomMentionToggle = nullptr;
    SettingsToggleButton *_keywordsToggle = nullptr;
    QPlainTextEdit *_keywordEdit = nullptr;
};

} // namespace TeleMatrix

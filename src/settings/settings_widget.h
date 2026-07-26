// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "settings/settings_common_widgets.h"

#include <QImage>
#include <QWidget>

#include <map>

class QStackedWidget;

namespace TeleMatrix {

class AppController;
namespace Core { class Settings; }

/// Settings panel with two-column layout (sidebar + content).
class SettingsWidget : public QWidget {
    Q_OBJECT

public:
    explicit SettingsWidget(AppController *controller, QWidget *parent = nullptr);
    void prepareForShow();
    /// Open the Active-sessions section. When `signOutDeviceId` is set, also start
    /// that session's confirm+password sign-out (from the "New login" banner).
    void openSessions(const QString &signOutDeviceId = QString());

    [[nodiscard]] QSize sizeHint() const override;

Q_SIGNALS:
    void closeRequested();
    void logoutRequested();

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;

private:
    enum class Section {
        Account,
        Notifications,
        Encryption,
        Sessions,
        Appearance,
        Preferences,
        HelpAbout,
    };

    void setupSidebar();
    QWidget *createPage(Section section);
    QWidget *pageForSection(Section section) const;
    void ensureSectionPageBuilt(Section section);
    void refreshCacheStats();
    void showSection(Section section);
    void paintTopBar(QPainter &p);

    AppController *_controller = nullptr;
    Core::Settings *_settings = nullptr;

    Section _currentSection = Section::Account;
    QStackedWidget *_stack = nullptr;
    std::map<Section, QWidget *> _pages;

    QWidget *_sidebar = nullptr;
    SettingsMenuButton *_accountButton = nullptr;
    SettingsMenuButton *_notificationsButton = nullptr;
    SettingsMenuButton *_encryptionButton = nullptr;
    SettingsMenuButton *_sessionsButton = nullptr;
    SettingsMenuButton *_appearanceButton = nullptr;
    SettingsMenuButton *_preferencesButton = nullptr;
    SettingsMenuButton *_helpAboutButton = nullptr;

    QRect _closeButtonRect;
    bool _closeHovered = false;
    QImage _closeIcon;
    QImage _closeIconOver;
};

} // namespace TeleMatrix

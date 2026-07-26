// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "settings/settings_widget.h"

#include "app/app_controller.h"
#include "core/core_settings.h"
#include "settings/account/account_settings_page.h"
#include "settings/appearance/appearance_settings_page.h"
#include "settings/encryption/encryption_settings_page.h"
#include "settings/help/help_about_settings_page.h"
#include "settings/notifications_settings_page.h"
#include "settings/preferences/preferences_settings_page.h"
#include "settings/sessions/sessions_settings_page.h"
#include "styles/style_constants.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace TeleMatrix {

SettingsWidget::SettingsWidget(AppController *controller, QWidget *parent)
    : QWidget(parent)
    , _controller(controller)
    , _settings(&controller->settings()) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    _closeIcon = loadColorizedSettingsIcon(
        QStringLiteral("info_close"),
        st::settingsCloseIconFg);
    _closeIconOver = loadColorizedSettingsIcon(
        QStringLiteral("info_close"),
        st::settingsCloseIconFgOver);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    auto *topBarSpacer = new QWidget(this);
    topBarSpacer->setFixedHeight(st::settingsTopBarHeight);
    topBarSpacer->setAttribute(Qt::WA_TransparentForMouseEvents);
    mainLayout->addWidget(topBarSpacer);

    auto *bodyLayout = new QHBoxLayout;
    bodyLayout->setContentsMargins(0, 0, 0, st::boxRadius);
    bodyLayout->setSpacing(0);

    setupSidebar();
    bodyLayout->addWidget(_sidebar);

    _stack = new QStackedWidget(this);
    bodyLayout->addWidget(_stack);

    mainLayout->addLayout(bodyLayout);
    showSection(Section::Account);
}

void SettingsWidget::prepareForShow() {
    // Reopen on the last-viewed section (remembered in memory only — the widget is
    // reused across opens, so _currentSection persists; nothing is written to disk).
    showSection(_currentSection);
    if (auto *page = qobject_cast<AccountSettingsPage *>(pageForSection(Section::Account))) {
        page->prepareForShow();
    }
}

QSize SettingsWidget::sizeHint() const {
    return QSize(st::settingsMaxWidth, 0);
}

void SettingsWidget::setupSidebar() {
    _sidebar = new QWidget(this);
    _sidebar->setFixedWidth(st::settingsSidebarWidth);

    auto *layout = new QVBoxLayout(_sidebar);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->setSpacing(0);

    _accountButton = new SettingsMenuButton(
        SettingsMenuButton::IconType::MyAccount,
        tr("Account"),
        _sidebar);
    layout->addWidget(_accountButton);

    _notificationsButton = new SettingsMenuButton(
        SettingsMenuButton::IconType::Notifications,
        tr("Notifications"),
        _sidebar);
    layout->addWidget(_notificationsButton);

    _encryptionButton = new SettingsMenuButton(
        SettingsMenuButton::IconType::Encryption,
        tr("Encryption"),
        _sidebar);
    layout->addWidget(_encryptionButton);

    _sessionsButton = new SettingsMenuButton(
        SettingsMenuButton::IconType::Sessions,
        tr("Sessions"),
        _sidebar);
    layout->addWidget(_sessionsButton);

    _appearanceButton = new SettingsMenuButton(
        SettingsMenuButton::IconType::Appearance,
        tr("Appearance"),
        _sidebar);
    layout->addWidget(_appearanceButton);

    _preferencesButton = new SettingsMenuButton(
        SettingsMenuButton::IconType::Advanced,
        tr("Preferences"),
        _sidebar);
    layout->addWidget(_preferencesButton);

    _helpAboutButton = new SettingsMenuButton(
        SettingsMenuButton::IconType::HelpAbout,
        tr("About"),
        _sidebar);
    layout->addWidget(_helpAboutButton);

    layout->addStretch(1);

    connect(_accountButton, &SettingsMenuButton::clicked,
        this, [this] { showSection(Section::Account); });
    connect(_notificationsButton, &SettingsMenuButton::clicked,
        this, [this] { showSection(Section::Notifications); });
    connect(_encryptionButton, &SettingsMenuButton::clicked,
        this, [this] { showSection(Section::Encryption); });
    connect(_sessionsButton, &SettingsMenuButton::clicked,
        this, [this] { showSection(Section::Sessions); });
    connect(_appearanceButton, &SettingsMenuButton::clicked,
        this, [this] { showSection(Section::Appearance); });
    connect(_preferencesButton, &SettingsMenuButton::clicked,
        this, [this] { showSection(Section::Preferences); });
    connect(_helpAboutButton, &SettingsMenuButton::clicked,
        this, [this] { showSection(Section::HelpAbout); });
}

QWidget *SettingsWidget::createPage(Section section) {
    switch (section) {
    case Section::Account:
        {
            auto *page = new AccountSettingsPage(_controller, &_controller->accountSettings(), _stack);
            connect(page, &AccountSettingsPage::logoutRequested,
                this, &SettingsWidget::logoutRequested);
            return page;
        }
    case Section::Notifications:
        return new NotificationsSettingsPage(_controller, _settings, _stack);
    case Section::Encryption:
        {
            auto *page = new EncryptionSettingsPage(_controller, _stack);
            connect(page, &EncryptionSettingsPage::sessionsRefreshRequested,
                this, [this] {
                    if (auto *sessions = qobject_cast<SessionsSettingsPage *>(
                            pageForSection(Section::Sessions))) {
                        sessions->refreshList();
                    }
                });
            return page;
        }
    case Section::Sessions:
        return new SessionsSettingsPage(_controller, _stack);
    case Section::Appearance:
        return new AppearanceSettingsPage(_controller, _settings, _stack);
    case Section::Preferences:
        return new PreferencesSettingsPage(_controller, _settings, _stack);
    case Section::HelpAbout:
        return new HelpAboutSettingsPage(_controller, _settings, _stack);
    }
    return nullptr;
}

QWidget *SettingsWidget::pageForSection(Section section) const {
    const auto page = _pages.find(section);
    return (page != _pages.end()) ? page->second : nullptr;
}

void SettingsWidget::ensureSectionPageBuilt(Section section) {
    if (pageForSection(section)) {
        return;
    }
    if (auto *page = createPage(section)) {
        _stack->addWidget(page);
        _pages.emplace(section, page);
    }
}

void SettingsWidget::refreshCacheStats() {
    if (auto *page = qobject_cast<PreferencesSettingsPage *>(
            pageForSection(Section::Preferences))) {
        page->refreshCacheStats();
    }
}

void SettingsWidget::openSessions(const QString &signOutDeviceId) {
    showSection(Section::Sessions);
    if (signOutDeviceId.isEmpty()) {
        return;
    }
    if (auto *sessions = qobject_cast<SessionsSettingsPage *>(
            pageForSection(Section::Sessions))) {
        sessions->requestSignOut(signOutDeviceId);
    }
}

void SettingsWidget::showSection(Section section) {
    _currentSection = section;
    ensureSectionPageBuilt(section);

    _accountButton->setSelected(section == Section::Account);
    _notificationsButton->setSelected(section == Section::Notifications);
    _encryptionButton->setSelected(section == Section::Encryption);
    _sessionsButton->setSelected(section == Section::Sessions);
    _appearanceButton->setSelected(section == Section::Appearance);
    _preferencesButton->setSelected(section == Section::Preferences);
    _helpAboutButton->setSelected(section == Section::HelpAbout);

    auto *currentPage = pageForSection(section);
    if (currentPage) {
        _stack->setCurrentWidget(currentPage);
    }

    switch (section) {
    case Section::Account:
        if (auto *page = qobject_cast<AccountSettingsPage *>(currentPage)) {
            page->refreshData();
        }
        break;
    case Section::Notifications:
        if (auto *page = qobject_cast<NotificationsSettingsPage *>(currentPage)) {
            page->refresh();
        }
        break;
    case Section::Encryption:
        if (auto *page = qobject_cast<EncryptionSettingsPage *>(currentPage)) {
            page->refreshOverview();
        }
        break;
    case Section::Sessions:
        if (auto *page = qobject_cast<SessionsSettingsPage *>(currentPage)) {
            page->refreshList();
        }
        break;
    case Section::Appearance:
        break;
    case Section::Preferences:
        refreshCacheStats();
        break;
    case Section::HelpAbout:
        break;
    }
    update();
}

void SettingsWidget::paintTopBar(QPainter &p) {
    const int dpr = qMax(1, qRound(devicePixelRatioF()));

    p.setFont(st::settingsTitleFont());
    p.setPen(st::settingsTitleFg);
    p.drawText(
        st::settingsTitleLeft,
        st::settingsTitleTop + QFontMetrics(st::settingsTitleFont()).ascent(),
        tr("Settings"));

    const auto &closeImg = _closeHovered ? _closeIconOver : _closeIcon;
    if (!closeImg.isNull()) {
        const int iconH = closeImg.height() / dpr;
        const int cx = _closeButtonRect.x() + 4;
        const int cy = (st::settingsTopBarHeight - iconH) / 2;
        p.drawImage(cx, cy, closeImg);
    }

    p.setPen(Qt::NoPen);
    p.setBrush(st::shadowFg);
    p.drawRect(0, st::settingsTopBarHeight - 1, width(), 1);
}

void SettingsWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    QPainterPath clip;
    clip.addRoundedRect(QRectF(rect()), st::boxRadius, st::boxRadius);
    p.setClipPath(clip);

    p.fillRect(rect(), st::windowBg);
    paintTopBar(p);

    const int divX = st::settingsSidebarWidth;
    const int divY = st::settingsTopBarHeight;
    p.setPen(Qt::NoPen);
    p.setBrush(st::shadowFg);
    p.drawRect(divX, divY, 1, height() - divY);
}

void SettingsWidget::mousePressEvent(QMouseEvent *e) {
    if (_closeButtonRect.contains(e->pos())) {
        Q_EMIT closeRequested();
        return;
    }
    QWidget::mousePressEvent(e);
}

void SettingsWidget::mouseMoveEvent(QMouseEvent *e) {
    const bool closeHovered = _closeButtonRect.contains(e->pos());
    if (closeHovered != _closeHovered) {
        _closeHovered = closeHovered;
        update(_closeButtonRect);
    }
    setCursor(closeHovered ? Qt::PointingHandCursor : Qt::ArrowCursor);
    QWidget::mouseMoveEvent(e);
}

void SettingsWidget::leaveEvent(QEvent *e) {
    if (_closeHovered) {
        _closeHovered = false;
        update(_closeButtonRect);
    }
    QWidget::leaveEvent(e);
}

void SettingsWidget::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    _closeButtonRect = QRect(
        width() - st::settingsCloseButtonSize,
        0,
        st::settingsCloseButtonSize,
        st::settingsTopBarHeight);
}

void SettingsWidget::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape) {
        Q_EMIT closeRequested();
        e->accept();
        return;
    }
    QWidget::keyPressEvent(e);
}

} // namespace TeleMatrix

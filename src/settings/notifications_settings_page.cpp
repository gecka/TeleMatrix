// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "settings/notifications_settings_page.h"

#include "app/app_controller.h"
#include "core/core_settings.h"
#include "protocol/protocol_bridge.h"
#include "settings/settings_common_widgets.h"
#include "styles/style_constants.h"
#include "ui/painter.h"

#include <QFontMetrics>
#include <QLabel>
#include <QPainter>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <functional>

namespace TeleMatrix {
namespace {

// On = notify all messages; Off = mentions & keywords only (those still notify —
// they're global rules that outrank the per-category default).
RoomNotificationMode levelForToggle(bool on) {
    return on ? RoomNotificationMode::AllMessages : RoomNotificationMode::MentionsOnly;
}

// Rounded themed panel (bg + border from live st:: tokens) — wraps the keyword
// textarea so it matches the settings input look without a stylesheet.
class ThemedFrame final : public QWidget {
public:
    explicit ThemedFrame(QWidget *parent) : QWidget(parent) {}

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        const auto r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(_focused ? st::activeLineFg : st::inputBorderFg);
        p.setBrush(st::boxBg);
        p.drawRoundedRect(r, 4, 4);
    }

public:
    void setFocused(bool focused) {
        if (_focused != focused) {
            _focused = focused;
            update();
        }
    }

private:
    bool _focused = false;
};

// Two-row keyword editor: a frameless, transparent QPlainTextEdit (the ThemedFrame
// behind it draws the box) that commits on focus-out.
class KeywordEdit final : public QPlainTextEdit {
public:
    explicit KeywordEdit(QWidget *parent) : QPlainTextEdit(parent) {
        setFrameShape(QFrame::NoFrame);
        auto f = font();
        f.setPixelSize(14);
        setFont(f);
        setLineWrapMode(QPlainTextEdit::WidgetWidth);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setTabChangesFocus(true);
        const QFontMetrics fm(font());
        setFixedHeight(fm.lineSpacing() * 2 + 12);
        QPalette pal = palette();
        pal.setColor(QPalette::Base, Qt::transparent);
        pal.setColor(QPalette::Text, st::windowFg);
        pal.setColor(QPalette::Highlight, st::windowBgActive);
        setPalette(pal);
        viewport()->setAutoFillBackground(false);
    }

    std::function<void()> onCommit;
    std::function<void(bool)> onFocusChanged;

protected:
    void focusInEvent(QFocusEvent *e) override {
        QPlainTextEdit::focusInEvent(e);
        if (onFocusChanged) {
            onFocusChanged(true);
        }
    }
    void focusOutEvent(QFocusEvent *e) override {
        QPlainTextEdit::focusOutEvent(e);
        if (onFocusChanged) {
            onFocusChanged(false);
        }
        if (onCommit) {
            onCommit();
        }
    }
};

} // namespace

NotificationsSettingsPage::NotificationsSettingsPage(
        AppController *controller,
        Core::Settings *settings,
        QWidget *parent)
    : SettingsScrollPage(parent)
    , _controller(controller)
    , _bridge(controller ? controller->bridge() : nullptr) {
    auto *content = contentWidget();
    auto *layout = contentLayout();

    layout->addSpacing(14);

    addSettingsSectionTitle(content, layout, tr("Notifications"));
    auto *desktopNotify = addSettingsToggle(
        content, layout, tr("Desktop notifications"), settings->desktopNotify());
    auto *soundNotify = addSettingsToggle(
        content, layout, tr("Sound"), settings->soundNotify());
    auto *bounceDock = addSettingsToggle(
        content, layout, tr("Bounce the Dock icon"), settings->bounceDockIcon());

    layout->addSpacing(st::settingsCheckboxesSkip);
    addSettingsSectionTitle(content, layout, tr("Show in notifications"));
    auto *showSender = addSettingsToggle(
        content, layout, tr("Sender name"), settings->showSenderName());
    auto *showPreview = addSettingsToggle(
        content, layout, tr("Message preview"), settings->showMessagePreview());

    layout->addSpacing(st::settingsCheckboxesSkip);
    addSettingsSectionTitle(content, layout, tr("Badge counter"));
    auto *includeMutedBadge = addSettingsToggle(
        content, layout, tr("Include muted chats in unread count"),
        settings->includeMutedInBadge());
    auto *includeMutedFolders = addSettingsToggle(
        content, layout, tr("Include muted chats in folders counters"),
        settings->includeMutedInFolders());

    // Account-global "Notifications for chats" — server push rules, loaded async
    // via the bridge and applied in applyNotificationSettings(). Placed last.
    // On/off toggles: on = all messages, off = mentions & keywords only.
    layout->addSpacing(st::settingsCheckboxesSkip);
    addSettingsSectionTitle(content, layout, tr("Notifications for chats"));
    _dmToggle = addSettingsToggle(content, layout, tr("Private chats"), true);
    _roomToggle = addSettingsToggle(content, layout, tr("Rooms"), true);
    connect(_dmToggle, &SettingsToggleButton::toggled, this, [this](bool on) {
        if (_bridge) {
            _bridge->setCategoryNotificationLevel(
                NotificationCategory::PrivateChats, levelForToggle(on));
        }
    });
    connect(_roomToggle, &SettingsToggleButton::toggled, this, [this](bool on) {
        if (_bridge) {
            _bridge->setCategoryNotificationLevel(
                NotificationCategory::Rooms, levelForToggle(on));
        }
    });

    // "Mentions & keywords" — four master toggles, each a server default push rule.
    // The keyword text field sits under the "Keywords" toggle.
    layout->addSpacing(st::settingsCheckboxesSkip);
    addSettingsSectionTitle(content, layout, tr("Notifications for mentions & keywords"));
    _displayNameToggle = addSettingsToggle(content, layout, tr("Display name"), true);
    _usernameToggle = addSettingsToggle(content, layout, tr("Username"), true);
    _roomMentionToggle = addSettingsToggle(content, layout, tr("@room"), true);
    _keywordsToggle = addSettingsToggle(content, layout, tr("Keywords"), true);
    connect(_displayNameToggle, &SettingsToggleButton::toggled, this, [this](bool on) {
        if (_bridge) {
            _bridge->setNotificationToggle(NotificationToggle::DisplayName, on);
        }
    });
    connect(_usernameToggle, &SettingsToggleButton::toggled, this, [this](bool on) {
        if (_bridge) {
            _bridge->setNotificationToggle(NotificationToggle::Username, on);
        }
    });
    connect(_roomMentionToggle, &SettingsToggleButton::toggled, this, [this](bool on) {
        if (_bridge) {
            _bridge->setNotificationToggle(NotificationToggle::Room, on);
        }
    });
    connect(_keywordsToggle, &SettingsToggleButton::toggled, this, [this](bool on) {
        if (_bridge) {
            _bridge->setNotificationToggle(NotificationToggle::Keywords, on);
        }
    });

    // Keyword editor: a 2-row textarea with the description as a label below it.
    auto *keywordBlock = new QWidget(content);
    auto *keywordLayout = new QVBoxLayout(keywordBlock);
    keywordLayout->setContentsMargins(
        st::settingsButtonPaddingLeft, 6, st::settingsButtonPaddingRight, 4);
    keywordLayout->setSpacing(6);

    auto *frame = new ThemedFrame(keywordBlock);
    auto *frameLayout = new QVBoxLayout(frame);
    frameLayout->setContentsMargins(4, 2, 4, 2);
    auto *keywordEdit = new KeywordEdit(frame);
    _keywordEdit = keywordEdit;
    frameLayout->addWidget(keywordEdit);
    keywordLayout->addWidget(frame);

    auto *keywordDesc = new QLabel(
        tr("Keywords that trigger notifications, comma-separated."), keywordBlock);
    keywordDesc->setFont(st::baseFont(12));
    keywordDesc->setWordWrap(true);
    {
        QPalette pal = keywordDesc->palette();
        pal.setColor(QPalette::WindowText, st::windowSubTextFg);
        keywordDesc->setPalette(pal);
    }
    keywordLayout->addWidget(keywordDesc);
    layout->addWidget(keywordBlock);

    keywordEdit->onFocusChanged = [frame](bool focused) {
        frame->setFocused(focused);
    };
    keywordEdit->onCommit = [this] {
        if (_bridge) {
            // Send even when empty (clears all keywords). Newlines act as commas.
            auto text = _keywordEdit->toPlainText();
            text.replace(QChar('\n'), QChar(','));
            _bridge->setKeywords(text);
        }
    };

    layout->addStretch(1);

    auto save = [this, controller] {
        if (controller) {
            controller->saveSettingsDelayed();
        }
        emit settingsChanged();
    };

    connect(desktopNotify, &SettingsToggleButton::toggled, this, [settings, save](bool v) {
        settings->setDesktopNotify(v);
        save();
    });
    connect(soundNotify, &SettingsToggleButton::toggled, this, [settings, save](bool v) {
        settings->setSoundNotify(v);
        save();
    });
    connect(bounceDock, &SettingsToggleButton::toggled, this, [settings, save](bool v) {
        settings->setBounceDockIcon(v);
        save();
    });
    connect(showSender, &SettingsToggleButton::toggled, this, [settings, save](bool v) {
        settings->setShowSenderName(v);
        save();
    });
    connect(showPreview, &SettingsToggleButton::toggled, this, [settings, save](bool v) {
        settings->setShowMessagePreview(v);
        save();
    });
    connect(includeMutedBadge, &SettingsToggleButton::toggled, this, [controller, settings, save](bool v) {
        settings->setIncludeMutedInBadge(v);
        if (controller) {
            controller->refreshNotificationsBadge();
        }
        save();
    });
    connect(includeMutedFolders, &SettingsToggleButton::toggled, this, [controller, settings, save](bool v) {
        settings->setIncludeMutedInFolders(v);
        if (controller) {
            controller->notifyIncludeMutedInFoldersChanged();
        }
        save();
    });

    // Apply the account-global chat settings whenever the server answers. The
    // fetch itself is triggered by refresh() (on every show), so values can't go
    // stale between visits.
    if (_bridge) {
        connect(_bridge, &ProtocolBridge::notificationSettingsReady, this,
                [this](bool success, RoomNotificationMode dmLevel,
                       RoomNotificationMode roomLevel, bool mentionDisplayName,
                       bool mentionUsername, bool mentionRoom, bool keywordsEnabled,
                       const QString &keywordsCsv) {
            if (success) {
                applyNotificationSettings(dmLevel, roomLevel, mentionDisplayName,
                    mentionUsername, mentionRoom, keywordsEnabled, keywordsCsv);
            }
        });
    }
}

void NotificationsSettingsPage::refresh() {
    if (_bridge) {
        _bridge->getNotificationSettings();
    }
}

void NotificationsSettingsPage::applyNotificationSettings(
        RoomNotificationMode dmLevel,
        RoomNotificationMode roomLevel,
        bool mentionDisplayName,
        bool mentionUsername,
        bool mentionRoom,
        bool keywordsEnabled,
        const QString &keywordsCsv) {
    // Reflect server state without re-triggering a write (setChecked emits toggled).
    const auto applyToggle = [](SettingsToggleButton *btn, bool on) {
        if (btn) {
            const QSignalBlocker blocker(btn);
            btn->setChecked(on);
        }
    };
    applyToggle(_dmToggle, dmLevel == RoomNotificationMode::AllMessages);
    applyToggle(_roomToggle, roomLevel == RoomNotificationMode::AllMessages);
    applyToggle(_displayNameToggle, mentionDisplayName);
    applyToggle(_usernameToggle, mentionUsername);
    applyToggle(_roomMentionToggle, mentionRoom);
    applyToggle(_keywordsToggle, keywordsEnabled);
    // Don't clobber an edit in progress.
    if (_keywordEdit && !_keywordEdit->hasFocus()) {
        _keywordEdit->setPlainText(keywordsCsv);
    }
}

} // namespace TeleMatrix

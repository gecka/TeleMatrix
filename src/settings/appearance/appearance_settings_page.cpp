// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "settings/appearance/appearance_settings_page.h"

#include "app/app_controller.h"
#include "core/core_settings.h"
#include "core/localization.h"
#include "history/history_confirm_dialog.h"
#include "settings/appearance/interface_scale_control.h"
#include "settings/settings_common_widgets.h"
#include "styles/style_constants.h"
#include "theme/theme_manager.h"
#include "theme/theme_registry.h"
#include "ui/input_submit_settings.h"
#include "ui/internal_choice_dialog.h"
#include "ui/style/runtime_font.h"
#include "ui/style/runtime_scale.h"

#include <QCoreApplication>
#include <QFontDatabase>
#include <QLabel>
#include <QLocale>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>

namespace TeleMatrix {
namespace {

using SettingsChoiceEntry = Ui::InternalChoiceEntry;

QString fontFamilyLabel(const QString &family) {
    if (family.isEmpty()) {
        return QCoreApplication::translate("SettingsWidget", "Default");
    }
    if (family == QStringLiteral("system")) {
        return QCoreApplication::translate("SettingsWidget", "System");
    }
    return family;
}

QVector<SettingsChoiceEntry> fontFamilyChoices(const QString &current) {
    QVector<SettingsChoiceEntry> result;
    result.push_back({
        QString(),
        QCoreApplication::translate("SettingsWidget", "Default"),
        QCoreApplication::translate("SettingsWidget", "Telegram Desktop default font"),
        st::baseFont(14),
    });
    result.push_back({
        QStringLiteral("system"),
        QCoreApplication::translate("SettingsWidget", "System"),
        QCoreApplication::translate("SettingsWidget", "macOS system font"),
        QFontDatabase::systemFont(QFontDatabase::GeneralFont),
    });

    auto families = QFontDatabase::families();
    families.sort(Qt::CaseInsensitive);
    for (const auto &family : families) {
        if (!QFontDatabase::isScalable(family)) {
            continue;
        }
        QFont preview(family);
        preview.setPixelSize(14);
        result.push_back({ family, family, QString(), preview });
    }

    if (!current.isEmpty() && current != QStringLiteral("system")) {
        const auto exists = std::any_of(result.cbegin(), result.cend(),
            [&current](const SettingsChoiceEntry &entry) {
                return entry.id == current;
            });
        if (!exists) {
            QFont preview(current);
            preview.setPixelSize(14);
            result.insert(2, { current, current, QString(), preview });
        }
    }
    return result;
}

QString languageLabel(const QString &id) {
    // No "System Default" entry: an unset/unknown id resolves to the system
    // language (or the English fallback), so always show a concrete language name.
    return Core::languageNativeName(Core::resolveLanguageId(id));
}

QVector<SettingsChoiceEntry> languageChoices(const QString &current) {
    QVector<SettingsChoiceEntry> result;
    // No "System Default" row — the system language is only used to pick the
    // default selection (via resolveLanguageId); the list shows real languages.
    for (const auto &language : Core::supportedLanguages()) {
        result.push_back({
            language.id,
            language.nativeName,
            language.name,
            st::baseFont(14),
        });
    }
    if (!current.isEmpty()) {
        const auto normalized = Core::normalizeLanguageId(current);
        const auto exists = std::any_of(result.cbegin(), result.cend(),
            [&current, &normalized](const SettingsChoiceEntry &entry) {
                return entry.id == current
                    || (!normalized.isEmpty() && entry.id == normalized);
            });
        if (!exists) {
            result.push_back({
                current,
                languageLabel(current),
                current,
                st::baseFont(14),
            });
        }
    }
    return result;
}

QString autoNightModeLabel(bool enabled) {
    return enabled
        ? QCoreApplication::translate("SettingsWidget", "System")
        : QCoreApplication::translate("SettingsWidget", "Off");
}

QString themeLabel(const Theme::ThemeManager *themeManager) {
    if (!themeManager) {
        return {};
    }
    const auto variant = themeManager->isNight()
        ? QCoreApplication::translate("SettingsWidget", "Night")
        : QCoreApplication::translate("SettingsWidget", "Day");
    return QStringLiteral("%1 · %2")
        .arg(Theme::ThemeById(themeManager->themeId()).displayName(), variant);
}

bool confirmRestart(QWidget *parent, const QString &text, const QString &confirmText) {
    HistoryConfirmDialog confirm(
        parent,
        QCoreApplication::translate("SettingsWidget", "Restart Required"),
        text,
        confirmText,
        QCoreApplication::translate("SettingsWidget", "Cancel"));
    return confirm.exec() == HistoryConfirmDialog::Accepted;
}

} // namespace

AppearanceSettingsPage::AppearanceSettingsPage(
        AppController *controller,
        Core::Settings *settings,
        QWidget *parent)
    : SettingsScrollPage(parent) {
    auto *content = contentWidget();
    auto *layout = contentLayout();
    layout->setContentsMargins(0, 16, 0, 20);

    auto *themeManager = controller ? controller->themeManager() : nullptr;

    auto *themeButton = new SettingsValueButton(
        tr("Color theme"),
        themeLabel(themeManager),
        content);
    layout->addWidget(themeButton);
    // The picker is a side panel over the whole window, so it has to replace the
    // settings layer rather than sit on top of it.
    themeButton->setClickedCallback([controller] {
        if (controller) {
            controller->requestThemeSelector();
        }
    });

    // "Display background doodles" toggle is hidden — no theme ships a doodle
    // any more. The backgroundDoodles() setting + its render path are kept, so
    // the control can return if doodles ever do.

    auto *autoNightButton = new SettingsValueButton(
        tr("Auto-Night Mode"),
        autoNightModeLabel(settings->systemDarkModeEnabled()),
        content);
    layout->addWidget(autoNightButton);
    autoNightButton->setClickedCallback([controller, settings, autoNightButton] {
        const auto enable = !settings->systemDarkModeEnabled();
        settings->setSystemDarkModeEnabled(enable);
        if (auto *themeManager = controller ? controller->themeManager() : nullptr) {
            themeManager->setSystemDarkModeEnabled(enable);
        }
        autoNightButton->setValue(autoNightModeLabel(enable));
        if (controller) {
            controller->saveSettingsDelayed();
        }
    });

    // Picking a theme card pins Day or Night, which turns Auto-Night off inside
    // the ThemeManager. Read both labels back from it (not from settings, whose
    // update rides a different themeChanged handler) so the rows stay honest
    // without reopening the page.
    if (themeManager) {
        connect(themeManager, &Theme::ThemeManager::themeChanged, this,
                [themeManager, themeButton, autoNightButton](bool, Theme::ThemeMode) {
            themeButton->setValue(themeLabel(themeManager));
            autoNightButton->setValue(
                autoNightModeLabel(themeManager->systemDarkModeEnabled()));
        });
    }

    const auto ratio = Style::DevicePixelRatio();
    const auto scaleMin = Style::kScaleMin;
    const auto scaleMax = Style::MaxScaleForRatio(ratio);
    const auto step = 5;
    const auto currentConfig = settings->configScale();
    const auto defaultScale = (ratio >= 2) ? 110 : 100;
    const auto currentEffective = (currentConfig == Style::kScaleAuto)
        ? defaultScale
        : currentConfig;
    QVector<int> values;
    for (int scale = scaleMin; scale <= scaleMax; scale += step) {
        values.append(scale);
    }
    if (values.isEmpty() || values.last() != scaleMax) {
        values.append(scaleMax);
    }
    const auto lastManualScale = (currentConfig != Style::kScaleAuto)
        ? currentConfig
        : currentEffective;

    auto *autoToggle = addSettingsToggle(
        content,
        layout,
        tr("Default interface scale"),
        currentConfig == Style::kScaleAuto);
    auto *scaleControl = new InterfaceScaleControl(values, currentEffective, content);
    layout->addWidget(scaleControl);
    // The scale value label colors itself from st:: at build time; refresh it on
    // theme change so it doesn't keep the previous theme's accent color.
    if (auto *tm = controller ? controller->themeManager() : nullptr) {
        connect(tm, &Theme::ThemeManager::themeChanged, scaleControl,
                [scaleControl](bool, Theme::ThemeMode) { scaleControl->refreshTheme(); });
    }

    connect(scaleControl, &InterfaceScaleControl::scaleReleased, this,
        [this, controller, settings, autoToggle, scaleControl, defaultScale](int newScale) {
        const auto oldEffective = (settings->configScale() == Style::kScaleAuto)
            ? defaultScale
            : settings->configScale();
        if (newScale == oldEffective) {
            return;
        }
        if (autoToggle->isChecked()) {
            QSignalBlocker blocker(autoToggle);
            autoToggle->setChecked(false);
        }
        const auto confirmed = confirmRestart(
            window(),
            tr("TeleMatrix needs to restart to apply this scale change."),
            tr("OK"));
        if (confirmed) {
            settings->setConfigScale(newScale);
            controller->saveSettings();
            controller->restartApplication();
        } else {
            scaleControl->setScale(oldEffective);
            if (settings->configScale() == Style::kScaleAuto) {
                QSignalBlocker blocker(autoToggle);
                autoToggle->setChecked(true);
            }
        }
    });

    connect(autoToggle, &SettingsToggleButton::toggled, this,
        [this, controller, settings, scaleControl, autoToggle, lastManualScale, defaultScale](bool checked) {
        if (checked) {
            const auto oldConfig = settings->configScale();
            const auto oldEffective = (oldConfig == Style::kScaleAuto)
                ? defaultScale
                : oldConfig;
            if (oldEffective != defaultScale) {
                const auto confirmed = confirmRestart(
                    window(),
                    tr("TeleMatrix needs to restart to apply this scale change."),
                    tr("OK"));
                if (confirmed) {
                    settings->setConfigScale(Style::kScaleAuto);
                    controller->saveSettings();
                    controller->restartApplication();
                } else {
                    QSignalBlocker blocker(autoToggle);
                    autoToggle->setChecked(false);
                }
            } else {
                settings->setConfigScale(Style::kScaleAuto);
                controller->saveSettings();
            }
            scaleControl->setScale(defaultScale);
        } else {
            settings->setConfigScale(lastManualScale);
            controller->saveSettings();
            scaleControl->setScale(lastManualScale);
        }
    });

    auto *languageButton = new SettingsValueButton(
        tr("Language"),
        languageLabel(settings->languageId()),
        content);
    layout->addWidget(languageButton);
    languageButton->setClickedCallback([this, controller, settings, languageButton] {
        // Resolve empty/unset to the concrete system-default language so the
        // dialog preselects a real entry (there is no "System Default" row).
        const auto current = Core::resolveLanguageId(settings->languageId());
        Ui::InternalChoiceDialog dialog(
            this,
            tr("Language"),
            languageChoices(current),
            current);
        if (dialog.exec() != Ui::InternalChoiceDialog::Accepted) {
            return;
        }
        const auto lang = dialog.chosenId();
        if (lang == current) {
            return;
        }
        if (confirmRestart(
                this,
                tr("TeleMatrix needs to restart to apply this language change."),
                tr("Restart"))) {
            languageButton->setValue(languageLabel(lang));
            controller->setLanguage(lang);
        }
    });

    auto *fontButton = new SettingsValueButton(
        tr("Font Family"),
        fontFamilyLabel(settings->customFontFamily()),
        content);
    layout->addWidget(fontButton);
    fontButton->setClickedCallback([this, controller, settings, fontButton] {
        const auto current = settings->customFontFamily();
        Ui::InternalChoiceDialog dialog(
            this,
            tr("Font Family"),
            fontFamilyChoices(current),
            current);
        if (dialog.exec() != Ui::InternalChoiceDialog::Accepted) {
            return;
        }
        const auto chosen = dialog.chosenId();
        if (chosen == current) {
            return;
        }
        if (confirmRestart(
                this,
                tr("TeleMatrix needs to restart to apply this font change."),
                tr("Restart"))) {
            settings->setCustomFontFamily(chosen);
            fontButton->setValue(fontFamilyLabel(chosen));
            controller->saveSettings();
            controller->restartApplication();
        }
    });

    auto *largeEmojiToggle = addSettingsToggle(
        content,
        layout,
        tr("Large emoji"),
        settings->largeEmoji());
    connect(largeEmojiToggle, &SettingsToggleButton::toggled, this, [controller, settings](bool checked) {
        settings->setLargeEmoji(checked);
        controller->saveSettingsDelayed();
        controller->notifyLargeEmojiChanged(checked);
    });

    auto *replyButtonToggle = addSettingsToggle(
        content,
        layout,
        tr("Reply button on messages"),
        settings->replyButtonOnMessages());
    connect(replyButtonToggle, &SettingsToggleButton::toggled, this, [controller, settings](bool checked) {
        settings->setReplyButtonOnMessages(checked);
        controller->saveSettingsDelayed();
        controller->notifyReplyButtonOnMessagesChanged(checked);
    });

    auto *reactionButtonToggle = addSettingsToggle(
        content,
        layout,
        tr("Reaction button on messages"),
        settings->reactionButtonOnMessages());
    connect(reactionButtonToggle, &SettingsToggleButton::toggled, this, [controller, settings](bool checked) {
        settings->setReactionButtonOnMessages(checked);
        controller->saveSettingsDelayed();
        controller->notifyReactionButtonOnMessagesChanged(checked);
    });

    auto *hideSystemToggle = addSettingsToggle(
        content,
        layout,
        tr("Hide system messages in public rooms"),
        settings->hideSystemMessagesInPublicRooms());
    connect(hideSystemToggle, &SettingsToggleButton::toggled, this, [controller, settings](bool checked) {
        settings->setHideSystemMessagesInPublicRooms(checked);
        controller->saveSettingsDelayed();
        controller->notifyHideSystemMessagesInPublicRoomsChanged(checked);
    });

    layout->addStretch(1);
}

} // namespace TeleMatrix

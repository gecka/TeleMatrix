// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "settings/help/help_about_settings_page.h"

#include "app/app_controller.h"
#include "core/core_settings.h"
#include "core/update_service.h"
#include "history/history_confirm_dialog.h"
#include "settings/settings_common_widgets.h"
#include "styles/style_constants.h"
#include "ui/safe_url.h"

#include <QLabel>
#include <QVBoxLayout>

#include <algorithm>

namespace TeleMatrix {
namespace {

using Policy = Core::UpdateService::Policy;
using ApplyMode = Core::UpdateService::ApplyMode;

[[nodiscard]] QString PolicyId(Policy policy) {
    switch (policy) {
    case Policy::Off: return QStringLiteral("off");
    case Policy::CheckAndNotify: return QStringLiteral("notify");
    case Policy::AutoDownload: return QStringLiteral("auto");
    }
    return QStringLiteral("notify");
}

} // namespace

HelpAboutSettingsPage::HelpAboutSettingsPage(
        AppController *controller,
        Core::Settings *settings,
        QWidget *parent)
    : SettingsScrollPage(parent)
    , _controller(controller)
    , _settings(settings) {
    auto *content = contentWidget();
    auto *layout = contentLayout();

    layout->addSpacing(14);

    auto *description = new QLabel(
        tr("A desktop Matrix client with the look and feel of "
           "Telegram Desktop"),
        content);
    description->setFont(st::baseFont(14));
    description->setWordWrap(true);
    description->setContentsMargins(
        st::settingsButtonPaddingLeft,
        0,
        st::settingsButtonPaddingRight,
        0);
    {
        QPalette pal = description->palette();
        pal.setColor(QPalette::WindowText, st::settingsCheckboxTextFg);
        description->setPalette(pal);
    }
    layout->addWidget(description);

    layout->addSpacing(14);
    addSettingsDivider(content, layout);
    layout->addSpacing(14);

    auto addLink = [content, layout](const QString &label, const QString &url) {
        auto *button = new SettingsValueButton(label, url, content);
        button->setClickedCallback([url] {
            OpenSafeExternalUrl(url);
        });
        layout->addWidget(button);
    };
    addLink(tr("Source code"), QStringLiteral("https://github.com/gecka/telematrix"));

    layout->addWidget(new SettingsValueButton(
        tr("Version"),
        QStringLiteral(TELEMATRIX_VERSION_STR),
        content));

    setupUpdateSection(content, layout);

    layout->addStretch(1);
}

void HelpAboutSettingsPage::setupUpdateSection(QWidget *content, QVBoxLayout *layout) {
    auto *service = _controller ? _controller->updateService() : nullptr;
    if (!service) {
        return;
    }

    // Status row sits right under Version — that is where someone looking at
    // "which version am I on" will next ask "is there a newer one".
    _updateRow = new SettingsValueButton(QString(), QString(), content);
    _updateRow->setClickedCallback([this] { onUpdateRowClicked(); });
    layout->addWidget(_updateRow);

    layout->addSpacing(14);
    addSettingsDivider(content, layout);
    layout->addSpacing(14);
    addSettingsSectionTitle(content, layout, tr("Updates"));

    const auto notifyOnly = (service->applyMode() != ApplyMode::OneClick);
    // Fallback matches Settings::_updatePolicy's default (auto-download).
    const auto current = std::clamp(
        _settings ? _settings->updatePolicy() : static_cast<int>(Policy::AutoDownload),
        0,
        2);

    struct Entry {
        Policy policy;
        QString title;
        QString subtitle;
        bool enabled;
    };
    const QVector<Entry> entries = {
        { Policy::Off,
          tr("Never check for updates"),
          QString(),
          true },
        { Policy::CheckAndNotify,
          tr("Check for updates"),
          tr("Tell me when a new version is available"),
          true },
        { Policy::AutoDownload,
          tr("Download updates automatically"),
          notifyOnly
              // Auto-download is pointless when nothing can be applied here, so
              // it is greyed out with the reason rather than silently no-oping.
              ? tr("Not available for this installation")
              : tr("Install them whenever you're ready"),
          !notifyOnly },
    };

    // A persisted AutoDownload meeting a notify-only install would otherwise
    // paint a checked *disabled* row — and lie, since download() is gated on
    // one-click so the effective behaviour is Check & notify. Show what actually
    // happens; the stored value is left alone so moving the app somewhere
    // writable makes it take effect again.
    const auto shown = (notifyOnly && current == static_cast<int>(Policy::AutoDownload))
        ? static_cast<int>(Policy::CheckAndNotify)
        : current;

    for (const auto &entry : entries) {
        auto *row = new SettingsChoiceRow(
            SettingsChoiceEntry{
                PolicyId(entry.policy),
                entry.title,
                entry.subtitle,
                st::baseFont(14),
                entry.enabled,
            },
            shown == static_cast<int>(entry.policy),
            content);
        const auto value = static_cast<int>(entry.policy);
        row->setClickedCallback([this, value](const QString &) { setPolicy(value); });
        layout->addWidget(row);
        _policyRows.push_back(row);
    }

    // Orthogonal to the policy above: that picks *whether* we check, this picks
    // *which channel*. Pointless with checking off, so it follows the same
    // greying-out convention the disabled auto-download row uses.
    _betaToggle = addSettingsToggle(
        content,
        layout,
        tr("Install beta versions"),
        _settings && _settings->installBetaVersions());
    _betaToggle->setEnabled(current != static_cast<int>(Policy::Off));
    connect(_betaToggle, &SettingsToggleButton::toggled, this,
            [this](bool checked) { setInstallBetaVersions(checked); });

    if (notifyOnly) {
        const auto reason = service->notifyOnlyReason();
        if (!reason.isEmpty()) {
            layout->addSpacing(6);
            auto *note = new QLabel(reason, content);
            note->setFont(st::baseFont(13));
            note->setWordWrap(true);
            note->setContentsMargins(
                st::settingsButtonPaddingLeft, 0, st::settingsButtonPaddingRight, 0);
            QPalette pal = note->palette();
            pal.setColor(QPalette::WindowText, st::windowSubTextFg);
            note->setPalette(pal);
            layout->addWidget(note);
        }
    }

    connect(service, &Core::UpdateService::checkStarted, this, [this] {
        _lastError.clear();
        _checkedClean = false;
        refreshUpdateRow();
    });
    connect(service, &Core::UpdateService::updateUpToDate, this, [this] {
        _checkedClean = true;
        refreshUpdateRow();
    });
    connect(service, &Core::UpdateService::checkFinished, this, [this] {
        // A silent failure: say nothing about why, but stop showing "Checking…".
        refreshUpdateRow();
    });
    connect(service, &Core::UpdateService::updateAvailable, this, [this](const QString &) {
        _checkedClean = false;
        refreshUpdateRow();
    });
    connect(service, &Core::UpdateService::downloadStarted, this, [this] {
        _lastError.clear();
        _received = 0;
        _total = 0;
        refreshUpdateRow();
    });
    connect(service, &Core::UpdateService::downloadCancelled, this, [this] {
        // Back to "available / Download", not an error state.
        _lastError.clear();
        _received = 0;
        _total = 0;
        refreshUpdateRow();
    });
    connect(service, &Core::UpdateService::updateProgress, this,
            [this](quint64 received, quint64 total) {
        _received = received;
        _total = total;
        refreshUpdateRow();
    });
    connect(service, &Core::UpdateService::updateReady, this, [this](const QString &) {
        refreshUpdateRow();
    });
    connect(service, &Core::UpdateService::applyStarted, this, [this] {
        _lastError.clear();
        refreshUpdateRow();
    });
    connect(service, &Core::UpdateService::updateError, this, [this](const QString &message) {
        _lastError = message;
        refreshUpdateRow();
    });

    refreshUpdateRow();
}

void HelpAboutSettingsPage::refreshUpdateRow() {
    auto *service = _controller ? _controller->updateService() : nullptr;
    if (!_updateRow || !service) {
        return;
    }

    if (service->applying()) {
        // First, and with no action: the verify + swap runs on a worker for a
        // few seconds, and a second click must not start another.
        _updateRow->setText(tr("Updating…"));
        _updateRow->setValue(QString());
        return;
    }
    if (service->checking()) {
        _updateRow->setText(tr("Checking for updates…"));
        _updateRow->setValue(QString());
        return;
    }
    if (service->downloading()) {
        const auto percent = (_total > 0) ? int((_received * 100) / _total) : 0;
        _updateRow->setText(tr("Downloading… %1%").arg(percent));
        _updateRow->setValue(tr("Cancel"));
        return;
    }
    if (!service->readyPath().isEmpty()) {
        _updateRow->setText(
            tr("Version %1 is ready to install").arg(service->availableVersion()));
        _updateRow->setValue(tr("Update & Restart"));
        return;
    }
    if (!_lastError.isEmpty()) {
        _updateRow->setText(_lastError);
        _updateRow->setValue(tr("Try again"));
        return;
    }
    if (!service->availableVersion().isEmpty()) {
        const auto oneClick = (service->applyMode() == ApplyMode::OneClick);
        _updateRow->setText(
            tr("Version %1 is available").arg(service->availableVersion()));
        _updateRow->setValue(oneClick ? tr("Download") : tr("Open release page"));
        return;
    }
    _updateRow->setText(_checkedClean
        ? tr("TeleMatrix is up to date")
        : tr("Check for updates"));
    _updateRow->setValue(_checkedClean ? tr("Check again") : tr("Check"));
}

void HelpAboutSettingsPage::setPolicy(int policy) {
    if (!_settings || !_controller) {
        return;
    }
    for (auto i = 0; i != _policyRows.size(); ++i) {
        _policyRows[i]->setChecked(i == policy);
    }
    if (_settings->updatePolicy() == policy) {
        return;
    }
    _settings->setUpdatePolicy(policy);
    _controller->saveSettingsDelayed();
    _controller->notifyUpdatePolicyChanged();
    if (_betaToggle) {
        _betaToggle->setEnabled(policy != static_cast<int>(Policy::Off));
    }
}

void HelpAboutSettingsPage::setInstallBetaVersions(bool beta) {
    if (!_settings || !_controller) {
        return;
    }
    if (_settings->installBetaVersions() == beta) {
        return;
    }
    _settings->setInstallBetaVersions(beta);
    _controller->saveSettingsDelayed();
    // The service reads the channel at the start of each check, so this only
    // needs to reach it before the next one — no re-check is forced here.
    if (auto *service = _controller->updateService()) {
        service->setBetaChannel(beta);
    }
}

void HelpAboutSettingsPage::onUpdateRowClicked() {
    auto *service = _controller ? _controller->updateService() : nullptr;
    if (!service || service->checking() || service->applying()) {
        return;
    }
    if (service->downloading()) {
        service->cancelDownload();
        return;
    }
    if (!service->readyPath().isEmpty()) {
        HistoryConfirmDialog dialog(
            this,
            tr("Update TeleMatrix"),
            tr("TeleMatrix will restart to finish updating."),
            tr("Update & Restart"),
            tr("Later"));
        if (dialog.exec() != HistoryConfirmDialog::Accepted) {
            return;
        }
        // Kicks off staging on a worker and returns; AppController quits on
        // applyReady, since the helper it leaves behind is waiting on this PID.
        service->applyAndRestart();
        return;
    }
    if (!service->availableVersion().isEmpty()) {
        if (service->applyMode() == ApplyMode::OneClick) {
            service->download();
        } else if (!service->releasePage().isEmpty()) {
            OpenSafeExternalUrl(service->releasePage());
        }
        return;
    }
    // Manual checks work under every policy, including Off.
    _lastError.clear();
    service->check(true);
}

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "settings/preferences/preferences_settings_page.h"

#include "app/app_controller.h"
#include "core/core_settings.h"
#include "protocol/protocol_bridge.h"
#include "settings/settings_common_widgets.h"
#include "styles/style_constants.h"
#include "ui/input_submit_settings.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QSlider>
#include <QStyle>
#include <QVBoxLayout>

#include "ui/painter.h"

namespace TeleMatrix {
namespace {

QString formatCacheBytes(quint64 bytes) {
    if (bytes >= 1024ULL * 1024 * 1024) {
        return QString::number(bytes / (1024.0 * 1024 * 1024), 'f', 2)
            + QStringLiteral(" GB");
    }
    return QString::number(bytes / (1024.0 * 1024), 'f', 1)
        + QStringLiteral(" MB");
}

constexpr int kCachePresetCount = 18;
const int kCachePresets[kCachePresetCount] = {
    50, 100, 200, 300, 400, 500, 750, 1000,
    1500, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000, 10000
};

// Horizontal slider painted with live st:: colors (replaces the shared
// settingsSliderStyleSheet() inline stylesheet): 4px groove (windowBgOver),
// filled sub-page + 16px round handle (windowActiveTextFg).
class CacheSlider final : public QSlider {
public:
    explicit CacheSlider(QWidget *parent)
        : QSlider(Qt::Horizontal, parent) {}

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);

        constexpr int kGroove = 4;
        constexpr int kHandle = 16;

        const int cy = height() / 2;
        const int trackLeft = kHandle / 2;
        const int trackRight = width() - kHandle / 2;
        const int trackWidth = qMax(0, trackRight - trackLeft);

        // Groove.
        const QRect groove(
            trackLeft, cy - kGroove / 2, trackWidth, kGroove);
        p.setBrush(st::windowBgOver);
        p.drawRoundedRect(groove, kGroove / 2, kGroove / 2);

        // Handle centre position.
        const int span = (maximum() > minimum())
            ? QStyle::sliderPositionFromValue(
                minimum(), maximum(), value(), trackWidth)
            : 0;
        const int handleCx = trackLeft + span;

        // Sub-page (filled portion up to the handle).
        if (handleCx > trackLeft) {
            const QRect sub(
                trackLeft, cy - kGroove / 2, handleCx - trackLeft, kGroove);
            p.setBrush(st::windowActiveTextFg);
            p.drawRoundedRect(sub, kGroove / 2, kGroove / 2);
        }

        // Handle.
        p.setBrush(st::windowActiveTextFg);
        p.drawEllipse(
            QPoint(handleCx, cy), kHandle / 2, kHandle / 2);
    }
};

} // namespace

PreferencesSettingsPage::PreferencesSettingsPage(
        AppController *controller,
        Core::Settings *settings,
        QWidget *parent)
    : SettingsScrollPage(parent)
    , _controller(controller) {
    auto *content = contentWidget();
    auto *layout = contentLayout();
    layout->setContentsMargins(0, 16, 0, 20);

    const auto savedWay = settings->sendSubmitWay();
    const auto currentWay = (savedWay == static_cast<int>(InputSubmitSettings::CtrlEnter))
        ? InputSubmitSettings::CtrlEnter
        : InputSubmitSettings::Enter;

    auto *enterRow = new SettingsChoiceRow(
        {
            QStringLiteral("enter"),
            LabelForSubmitSetting(InputSubmitSettings::Enter),
            QString(),
            st::baseFont(14),
        },
        currentWay == InputSubmitSettings::Enter,
        content);
    auto *ctrlEnterRow = new SettingsChoiceRow(
        {
            QStringLiteral("ctrl_enter"),
            LabelForSubmitSetting(InputSubmitSettings::CtrlEnter),
            QString(),
            st::baseFont(14),
        },
        currentWay == InputSubmitSettings::CtrlEnter,
        content);
    layout->addWidget(enterRow);
    layout->addWidget(ctrlEnterRow);

    const auto setSendWay = [controller, settings, enterRow, ctrlEnterRow](InputSubmitSettings value) {
        enterRow->setChecked(value == InputSubmitSettings::Enter);
        ctrlEnterRow->setChecked(value == InputSubmitSettings::CtrlEnter);
        if (settings->sendSubmitWay() == static_cast<int>(value)) {
            return;
        }
        settings->setSendSubmitWay(static_cast<int>(value));
        controller->saveSettingsDelayed();
        emit controller->sendSubmitWayChanged(value);
    };
    enterRow->setClickedCallback([setSendWay](const QString &) {
        setSendWay(InputSubmitSettings::Enter);
    });
    ctrlEnterRow->setClickedCallback([setSendWay](const QString &) {
        setSendWay(InputSubmitSettings::CtrlEnter);
    });

#ifdef Q_OS_MAC
    auto *quitWarnToggle = addSettingsToggle(
        content,
        layout,
        tr("Warn before quitting with %1").arg(QStringLiteral("\u2318Q")),
        settings->macWarnBeforeQuit());
    connect(quitWarnToggle, &SettingsToggleButton::toggled, this, [controller, settings](bool checked) {
        settings->setMacWarnBeforeQuit(checked);
        controller->saveSettingsDelayed();
    });
#endif

    layout->addSpacing(st::settingsCheckboxesSkip);
    addSettingsSectionTitle(content, layout, tr("Search"));

    auto *e2eeSearchToggle = addSettingsToggle(
        content,
        layout,
        tr("Search in encrypted rooms"),
        settings->searchEncryptedRooms());
    connect(e2eeSearchToggle, &SettingsToggleButton::toggled, this,
        [controller, settings](bool checked) {
            settings->setSearchEncryptedRooms(checked);
            controller->saveSettingsDelayed();
            // Apply to EVERY account, not just the visible one: the setting is
            // app-global, and background accounts must run the same index
            // wipe/backfill side-effects. See MA-5.
            controller->setE2eeSearchEnabledAllAccounts(checked);
        });
    {
        auto *hint = new QLabel(
            tr("Builds a local, encrypted search index for encrypted rooms. "
               "Turning this off deletes the index; turning it back on re-scans "
               "history."),
            content);
        hint->setFont(st::msgFont);
        QPalette pal = hint->palette();
        pal.setColor(QPalette::WindowText, st::windowSubTextFg);
        hint->setPalette(pal);
        hint->setWordWrap(true);
        hint->setContentsMargins(st::settingsButtonPaddingLeft, 0,
                                 st::settingsButtonPaddingRight, 8);
        layout->addWidget(hint);
    }

    layout->addSpacing(st::settingsCheckboxesSkip);
    addSettingsSectionTitle(content, layout, tr("Cache"));

    auto *sizeRow = new QWidget(content);
    auto *sizeLayout = new QHBoxLayout(sizeRow);
    sizeLayout->setContentsMargins(st::settingsButtonPaddingLeft, 8,
                                   st::settingsButtonPaddingRight, 4);
    _cacheSizeLabel = new QLabel(tr("Calculating..."), sizeRow);
    _cacheSizeLabel->setFont(st::semiboldFont);
    sizeLayout->addWidget(_cacheSizeLabel);
    sizeLayout->addStretch();
    layout->addWidget(sizeRow);

    auto *detailRow = new QWidget(content);
    auto *detailLayout = new QHBoxLayout(detailRow);
    detailLayout->setContentsMargins(st::settingsButtonPaddingLeft, 0,
                                     st::settingsButtonPaddingRight, 12);
    _cacheDetailLabel = new QLabel(content);
    _cacheDetailLabel->setFont(st::msgFont);
    {
        QPalette pal = _cacheDetailLabel->palette();
        pal.setColor(QPalette::WindowText, st::windowSubTextFg);
        _cacheDetailLabel->setPalette(pal);
    }
    _cacheDetailLabel->setWordWrap(true);
    detailLayout->addWidget(_cacheDetailLabel);
    layout->addWidget(detailRow);

    auto *limitRow = new QWidget(content);
    auto *limitLayout = new QHBoxLayout(limitRow);
    limitLayout->setContentsMargins(st::settingsButtonPaddingLeft, 4,
                                    st::settingsButtonPaddingRight, 8);
    limitLayout->setSpacing(12);

    _cacheLimitSlider = new CacheSlider(limitRow);
    _cacheLimitSlider->setRange(0, kCachePresetCount - 1);
    _cacheLimitSlider->setPageStep(1);
    _cacheLimitSlider->setSingleStep(1);
    _cacheLimitSlider->setFixedHeight(28);
    _cacheLimitSlider->setCursor(Qt::PointingHandCursor);

    const auto currentMB = settings->cacheSizeLimitMB();
    auto index = 7;
    for (auto i = 0; i < kCachePresetCount; ++i) {
        if (kCachePresets[i] >= currentMB) {
            index = i;
            break;
        }
    }
    _cacheLimitSlider->setValue(index);

    _cacheLimitValueLabel = new QLabel(limitRow);
    _cacheLimitValueLabel->setFont(st::baseFont(14));
    _cacheLimitValueLabel->setMinimumWidth(60);
    _cacheLimitValueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    {
        QPalette pal = _cacheLimitValueLabel->palette();
        pal.setColor(QPalette::WindowText, st::windowActiveTextFg);
        _cacheLimitValueLabel->setPalette(pal);
    }

    auto updateLimitLabel = [this, controller, settings](int index) {
        const auto mb = kCachePresets[qBound(0, index, kCachePresetCount - 1)];
        _cacheLimitValueLabel->setText(mb >= 1000
            ? QString::number(mb / 1000.0, 'g', 3) + QStringLiteral(" GB")
            : QString::number(mb) + QStringLiteral(" MB"));
        settings->setCacheSizeLimitMB(mb);
        controller->saveSettingsDelayed();
        if (auto *bridge = controller->bridge()) {
            bridge->setMediaCacheLimit(quint64(mb) * 1024 * 1024);
        }
    };
    connect(_cacheLimitSlider, &QSlider::valueChanged, this, updateLimitLabel);
    updateLimitLabel(index);

    auto *cacheLimitTitle = new QLabel(tr("Cache limit:"), limitRow);
    cacheLimitTitle->setFont(st::baseFont(14));
    {
        QPalette pal = cacheLimitTitle->palette();
        pal.setColor(QPalette::WindowText, st::settingsCheckboxTextFg);
        cacheLimitTitle->setPalette(pal);
    }
    limitLayout->addWidget(cacheLimitTitle);
    limitLayout->addWidget(_cacheLimitSlider, 1);
    limitLayout->addWidget(_cacheLimitValueLabel);
    layout->addWidget(limitRow);

    auto *clearRow = new QWidget(content);
    auto *clearLayout = new QHBoxLayout(clearRow);
    clearLayout->setContentsMargins(0, 0, st::settingsButtonPaddingRight, 0);

    auto *clearCacheLink = new SettingsLinkButton(
        tr("Clear local cache"),
        st::attentionButtonFg,
        clearRow);
    auto *clearedLabel = new QLabel(clearRow);
    clearedLabel->setFont(st::msgFont);
    {
        QPalette pal = clearedLabel->palette();
        pal.setColor(QPalette::WindowText, st::windowSubTextFg);
        clearedLabel->setPalette(pal);
    }

    clearCacheLink->setClickedCallback([controller, clearCacheLink, clearedLabel] {
        clearCacheLink->setText(QObject::tr("Clearing..."));
        clearCacheLink->setEnabled(false);
        clearedLabel->clear();
        controller->bridge()->clearAllCaches();
    });
    connect(controller->bridge(), &ProtocolBridge::cacheClearResult,
        this, [this, clearCacheLink, clearedLabel](bool success, quint64 freedBytes) {
        clearCacheLink->setText(tr("Clear local cache"));
        clearCacheLink->setEnabled(true);
        if (success && freedBytes > 0) {
            const auto mb = freedBytes / (1024.0 * 1024.0);
            clearedLabel->setText(
                tr("Cleared %1 MB").arg(QString::number(mb, 'f', 1)));
        }
        refreshCacheStats();
    });

    clearLayout->addWidget(clearCacheLink, 1);
    clearLayout->addWidget(clearedLabel);
    layout->addWidget(clearRow);

    connect(controller->bridge(), &ProtocolBridge::cacheStatsReady,
        this, [this](const CacheStats &stats) {
        if (!_cacheSizeLabel) return;
        // "Clear local cache" frees media + preview only. App cache (live
        // rooms/folders/emoji) is repopulated immediately by the running session,
        // and the encrypted search index is intentionally kept (see
        // clear_cache_data) -- neither is shown, so the total equals what clearing
        // actually frees.
        const auto cleanableBytes = stats.mediaFilesBytes
            + stats.previewCacheBytes;
        _cacheSizeLabel->setText(tr("Total cache: %1").arg(formatCacheBytes(cleanableBytes)));
        _cacheDetailLabel->setText(
            tr("Media files: %1\nPreview cache: %2")
                .arg(formatCacheBytes(stats.mediaFilesBytes))
                .arg(formatCacheBytes(stats.previewCacheBytes)));
    });

    refreshCacheStats();

    layout->addStretch(1);
}

void PreferencesSettingsPage::refreshCacheStats() {
    if (_controller && _controller->bridge()) {
        _controller->bridge()->getCacheStats();
    }
}

} // namespace TeleMatrix

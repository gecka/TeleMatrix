// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "settings/encryption/encryption_settings_page.h"

#include "app/app_controller.h"
#include "history/history_confirm_dialog.h"
#include "protocol/protocol_bridge.h"
#include "settings/dialogs/interactive_auth_dialog.h"
#include "settings/dialogs/recovery_key_dialog.h"
#include "settings/dialogs/reset_identity_dialog.h"
#include "settings/dialogs/settings_passphrase_dialog.h"
#include "settings/dialogs/verify_session_dialog.h"
#include "settings/encryption/encryption_action_row.h"
#include "settings/settings_common_widgets.h"
#include "styles/style_constants.h"
#include "theme/theme_manager.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QDebug>
#include <QFileDialog>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>

namespace TeleMatrix {
namespace {

void showSettingsInformBox(QWidget *parent, const QString &title, const QString &text) {
    HistoryConfirmDialog dialog(
        parent,
        title,
        text,
        QCoreApplication::translate("SettingsWidget", "OK"),
        QString(),
        HistoryConfirmDialog::Normal,
        0,
        -1,
        false);
    dialog.exec();
}

bool confirmSettingsAction(
        QWidget *parent,
        const QString &title,
        const QString &text,
        const QString &confirmText,
        const QString &cancelText,
        HistoryConfirmDialog::ConfirmStyle style = HistoryConfirmDialog::Normal) {
    HistoryConfirmDialog dialog(
        parent,
        title,
        text,
        confirmText,
        cancelText,
        style);
    return dialog.exec() == HistoryConfirmDialog::Accepted;
}

void paintSettingsBoxShadow(QPainter &p, const QRect &boxRect) {
    PainterHighQualityEnabler hq(p);
    p.setPen(Qt::NoPen);
    constexpr auto kShadowExtend = 10;
    for (int i = kShadowExtend; i >= 1; --i) {
        const auto progress = qreal(kShadowExtend - i) / kShadowExtend;
        const auto alpha = int(18.0 * progress * progress);
        p.setBrush(st::withAlpha(st::windowShadowFg, alpha));
        const auto r = st::boxRadius + i;
        p.drawRoundedRect(boxRect.adjusted(-i, -i, i, i), r, r);
    }
}

// Panel surface painted with live st:: colors (tracks theme changes) instead
// of a frozen stylesheet background.
class RoundedPanel final : public QWidget {
public:
    explicit RoundedPanel(QWidget *parent) : QWidget(parent) {}

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(st::boxBg);
        p.drawRoundedRect(rect(), st::boxRadius, st::boxRadius);
    }
};

// Outline (bordered, transparent-fill) text button painted with live st::
// colors. ::Ui::TextButton has no border support, so this small subclass keeps
// the 1px outline the original stylesheet drew.
class OutlineButton final : public QAbstractButton {
public:
    OutlineButton(const QString &text, QWidget *parent)
        : QAbstractButton(parent) {
        setText(text);
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);
        setFixedHeight(36);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        if (_hovered) {
            p.setPen(Qt::NoPen);
            p.setBrush(st::settingsButtonBgOver);
            p.drawRoundedRect(rect(), 4, 4);
        }
        const auto inset = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(QPen(st::windowActiveTextFg, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(inset, 4, 4);
        p.setPen(st::windowActiveTextFg);
        p.drawText(rect(), Qt::AlignCenter, text());
    }
    void enterEvent(QEnterEvent *) override { _hovered = true; update(); }
    void leaveEvent(QEvent *) override { _hovered = false; update(); }

    QSize sizeHint() const override {
        const QFontMetrics fm(font());
        return QSize(fm.horizontalAdvance(text()) + 48, 36);
    }

private:
    bool _hovered = false;
};

class ActionSpinner final : public QWidget {
public:
    explicit ActionSpinner(QWidget *parent = nullptr)
        : QWidget(parent) {
        setFixedSize(20, 20);
        _timer.setInterval(33);
        QObject::connect(&_timer, &QTimer::timeout, this, [this] {
            _angle = (_angle + 30) % 360;
            update();
        });
        _timer.start();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        auto pen = QPen(st::windowActiveTextFg, 2);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawArc(rect().adjusted(3, 3, -3, -3), (_angle + 35) * 16, 285 * 16);
    }

private:
    QTimer _timer;
    int _angle = 0;
};

class ActionLoadingOverlay final : public QWidget {
public:
    ActionLoadingOverlay(const QString &title, const QString &text, QWidget *parent)
        : QWidget(parent ? parent->window() : nullptr) {
        if (parentWidget()) {
            setGeometry(parentWidget()->rect());
            parentWidget()->installEventFilter(this);
        }
        setAttribute(Qt::WA_DeleteOnClose, false);

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->addStretch(1);

        _panel = new RoundedPanel(this);
        _panel->setFixedWidth(st::boxWideWidth);
        root->addWidget(_panel, 0, Qt::AlignHCenter);
        root->addStretch(1);

        auto *panelLayout = new QVBoxLayout(_panel);
        panelLayout->setContentsMargins(0, 0, 0, 0);
        panelLayout->setSpacing(0);

        auto *titleLabel = new QLabel(title, _panel);
        titleLabel->setFixedHeight(st::boxTitleHeight);
        titleLabel->setContentsMargins(st::boxTitlePosition.x(), 0, 0, 0);
        titleLabel->setFont(st::boxTitleFont);
        titleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        {
            QPalette pal = titleLabel->palette();
            pal.setColor(QPalette::WindowText, st::boxTitleFg);
            titleLabel->setPalette(pal);
        }
        panelLayout->addWidget(titleLabel);

        auto *separator = new QWidget(_panel);
        separator->setFixedHeight(1);
        separator->setAutoFillBackground(true);
        {
            QPalette pal = separator->palette();
            pal.setColor(QPalette::Window, st::shadowFg);
            separator->setPalette(pal);
        }
        panelLayout->addWidget(separator);

        auto *body = new QWidget(_panel);
        auto *bodyLayout = new QHBoxLayout(body);
        bodyLayout->setContentsMargins(
            st::boxPadding.left(),
            st::boxPadding.top(),
            st::boxPadding.right(),
            st::boxPadding.bottom());
        bodyLayout->setSpacing(12);
        bodyLayout->addWidget(new ActionSpinner(body), 0, Qt::AlignTop);

        auto *label = new QLabel(text, body);
        label->setWordWrap(true);
        label->setFont(st::baseFont(14));
        {
            QPalette pal = label->palette();
            pal.setColor(QPalette::WindowText, st::windowFg);
            label->setPalette(pal);
        }
        bodyLayout->addWidget(label, 1);
        panelLayout->addWidget(body);

        _panel->adjustSize();
        _panel->setFixedHeight(_panel->sizeHint().height());
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), st::layerBg);
        if (_panel) {
            paintSettingsBoxShadow(p, _panel->geometry());
        }
    }

    bool eventFilter(QObject *obj, QEvent *event) override {
        if (obj == parentWidget() && event->type() == QEvent::Resize) {
            setGeometry(parentWidget()->rect());
        }
        return QWidget::eventFilter(obj, event);
    }

private:
    QWidget *_panel = nullptr;
};

} // namespace

EncryptionSettingsPage::EncryptionSettingsPage(
        AppController *controller,
        QWidget *parent)
    : SettingsScrollPage(parent)
    , _controller(controller) {
    contentLayout()->addStretch(1);

    if (auto *themeManager = _controller ? _controller->themeManager() : nullptr) {
        connect(themeManager, &Theme::ThemeManager::themeChanged,
                this, [this](bool, Theme::ThemeMode) {
            QTimer::singleShot(0, this, [this] {
                rebuildUi(_lastOverview);
            });
        });
    }

    if (auto *bridge = _controller ? _controller->bridge() : nullptr) {
        connect(bridge, &ProtocolBridge::encryptionOverviewReady,
                this, [this](bool success, const EncryptionOverview &overview) {
            if (success) {
                rebuildUi(overview);
            } else {
                qWarning() << "Failed to fetch encryption overview";
            }
        });
        connect(bridge, &ProtocolBridge::keyStorageUpdated,
                this, [this](bool success) {
            if (success) {
                refreshOverview();
            } else {
                qWarning() << "Failed to update key storage setting";
            }
        });
        connect(bridge, &ProtocolBridge::recoveryKeyAccepted,
                this, [this](bool success) {
            if (success) {
                refreshOverview();
            } else {
                showSettingsInformBox(this, tr("Recovery Key"),
                    tr("The recovery key was not accepted. Please check and try again."));
            }
        });
        connect(bridge, &ProtocolBridge::recoveryKeyCreated,
                this, [this](bool success, const QString &recoveryKey) {
            hideActionPreloader();
            if (success) {
                auto *dialog = new RecoveryKeyDialog(
                    RecoveryKeyDialog::Display, recoveryKey, this);
                if (dialog->exec() == RecoveryKeyDialog::Accepted) {
                    if (auto *b = _controller ? _controller->bridge() : nullptr) {
                        showActionPreloader(
                            tr("Updating recovery key"),
                            tr("Please wait while your new recovery key is saved."));
                        b->commitRecoveryKey(recoveryKey);
                    } else {
                        _recoveryKeyChangeInProgress = false;
                    }
                } else {
                    _recoveryKeyChangeInProgress = false;
                }
                dialog->deleteLater();
            } else {
                _recoveryKeyChangeInProgress = false;
                qWarning() << "Failed to create recovery key";
            }
        });
        connect(bridge, &ProtocolBridge::recoveryKeyCommitted,
                this, [this](bool success) {
            hideActionPreloader();
            _recoveryKeyChangeInProgress = false;
            if (success) {
                refreshOverview();
            } else {
                qWarning() << "Failed to commit recovery key";
            }
        });
        connect(bridge, &ProtocolBridge::e2eKeysExported,
                this, [this](bool success) {
            showSettingsInformBox(this, tr("Export Keys"),
                success
                    ? tr("E2E room keys exported successfully.")
                    : tr("Failed to export E2E room keys."));
        });
        connect(bridge, &ProtocolBridge::e2eKeysImported,
                this, [this](bool success, int importedCount, int totalCount) {
            if (success) {
                showSettingsInformBox(this, tr("Import Keys"),
                    tr("Imported %1 of %2 keys.").arg(importedCount).arg(totalCount));
                refreshOverview();
            } else {
                showSettingsInformBox(this, tr("Import Keys"),
                    tr("Failed to import E2E room keys."));
            }
        });
        connect(bridge, &ProtocolBridge::identityResetResult,
                this, [this](bool success, const ResetIdentityResult &result) {
            if (success && result.completed) {
                showSettingsInformBox(this, tr("Identity Reset"),
                    tr("Cryptographic identity has been reset."));
                refreshOverview();
            } else if (!result.challengeJson.isEmpty()) {
                auto *dialog = new InteractiveAuthDialog(
                    _controller ? _controller->userId() : QString(),
                    result.challengeJson,
                    this);
                if (dialog->exec() == InteractiveAuthDialog::Accepted) {
                    if (auto *b = _controller ? _controller->bridge() : nullptr) {
                        b->resetIdentity(dialog->authJson());
                    }
                }
                dialog->deleteLater();
            } else {
                showSettingsInformBox(this, tr("Identity Reset"),
                    tr("Failed to reset cryptographic identity."));
            }
        });
    }
}

void EncryptionSettingsPage::refreshOverview() {
    if (auto *bridge = _controller ? _controller->bridge() : nullptr) {
        bridge->getEncryptionOverview();
    }
}

void EncryptionSettingsPage::openVerifySessionDialog() {
    auto *bridge = _controller ? _controller->bridge() : nullptr;
    if (!bridge) {
        return;
    }

    auto *dialog = new VerifySessionDialog(bridge, this);
    if (dialog->exec() == VerifySessionDialog::Accepted) {
        refreshOverview();
        Q_EMIT sessionsRefreshRequested();
    }
    dialog->deleteLater();
}

void EncryptionSettingsPage::enterRecoveryKey() {
    auto *dialog = new RecoveryKeyDialog(
        RecoveryKeyDialog::Entry,
        QString(),
        this);
    if (dialog->exec() == RecoveryKeyDialog::Accepted) {
        if (auto *bridge = _controller ? _controller->bridge() : nullptr) {
            bridge->enterRecoveryKey(dialog->recoveryKey());
        }
    }
    dialog->deleteLater();
}

void EncryptionSettingsPage::resetIdentity() {
    auto *dialog = new ResetIdentityDialog(this);
    if (dialog->exec() == ResetIdentityDialog::Accepted) {
        if (auto *bridge = _controller ? _controller->bridge() : nullptr) {
            bridge->resetIdentity();
        }
    }
    dialog->deleteLater();
}

void EncryptionSettingsPage::showActionPreloader(
        const QString &title,
        const QString &text) {
    hideActionPreloader();
    _actionPreloader = new ActionLoadingOverlay(title, text, this);
    _actionPreloader->show();
    _actionPreloader->raise();
}

void EncryptionSettingsPage::hideActionPreloader() {
    if (!_actionPreloader) {
        return;
    }
    _actionPreloader->hide();
    _actionPreloader->deleteLater();
    _actionPreloader = nullptr;
}

void EncryptionSettingsPage::rebuildUi(const EncryptionOverview &overview) {
    _lastOverview = overview;
    clearContent();

    auto *layout = contentLayout();
    auto *content = contentWidget();
    layout->addSpacing(14);

    const auto state = overview.healthState;

    // Filled primary button painted with live st:: colors (replaces an inline
    // stylesheet). `bg`/`bgOver` are pointers to st:: globals so paint tracks
    // theme changes.
    auto addFilledButton = [this, layout, content](
            const QString &text,
            const QColor *bg,
            const QColor *bgOver,
            std::function<void()> action,
            int top = 4) {
        auto *container = new QWidget(content);
        auto *buttonLayout = new QHBoxLayout(container);
        buttonLayout->setContentsMargins(
            st::settingsButtonPaddingLeft,
            top,
            st::settingsButtonPaddingRight,
            4);
        ::Ui::TextButton::Style style;
        style.bg = bg;
        style.bgOver = bgOver;
        style.fg = &st::activeButtonFg;  // white
        style.radius = 4;
        style.height = 36;
        style.paddingH = 24;
        auto *button = new ::Ui::TextButton(text, style, container);
        button->setFont(st::baseFont(14));
        connect(button, &QAbstractButton::clicked, this, [action] {
            action();
        });
        buttonLayout->addWidget(button);
        buttonLayout->addStretch(1);
        layout->addWidget(container);
    };

    // Outline (bordered, transparent) primary button.
    auto addOutlineButton = [this, layout, content](
            const QString &text,
            std::function<void()> action,
            int top = 4) {
        auto *container = new QWidget(content);
        auto *buttonLayout = new QHBoxLayout(container);
        buttonLayout->setContentsMargins(
            st::settingsButtonPaddingLeft,
            top,
            st::settingsButtonPaddingRight,
            4);
        auto *button = new OutlineButton(text, container);
        button->setFont(st::baseFont(14));
        connect(button, &QAbstractButton::clicked, this, [action] {
            action();
        });
        buttonLayout->addWidget(button);
        buttonLayout->addStretch(1);
        layout->addWidget(container);
    };

    if (state == EncryptionHealthState::VerifyThisSession) {
        auto *descLabel = new QLabel(
            tr("Verify this session to access your encrypted messages and prove your identity to others."),
            content);
        descLabel->setFont(st::baseFont(13));
        descLabel->setWordWrap(true);
        {
            QPalette pal = descLabel->palette();
            pal.setColor(QPalette::WindowText, st::windowSubTextFg);
            descLabel->setPalette(pal);
        }
        descLabel->setContentsMargins(
            st::settingsButtonPaddingLeft,
            0,
            st::settingsButtonPaddingRight,
            12);
        layout->addWidget(descLabel);

        addFilledButton(
            tr("Verify with another device"),
            &st::windowActiveTextFg,
            &st::windowActiveTextFg,
            [this] { openVerifySessionDialog(); });
        addOutlineButton(
            tr("Enter recovery key"),
            [this] { enterRecoveryKey(); },
            8);
    } else if (state == EncryptionHealthState::KeyStorageOutOfSync) {
        auto *descLabel = new QLabel(
            tr("Enter your recovery key to sync encryption keys across your devices."),
            content);
        descLabel->setFont(st::baseFont(13));
        descLabel->setWordWrap(true);
        {
            QPalette pal = descLabel->palette();
            pal.setColor(QPalette::WindowText, st::windowSubTextFg);
            descLabel->setPalette(pal);
        }
        descLabel->setContentsMargins(
            st::settingsButtonPaddingLeft,
            0,
            st::settingsButtonPaddingRight,
            12);
        layout->addWidget(descLabel);

        addFilledButton(
            tr("Enter recovery key"),
            &st::windowActiveTextFg,
            &st::windowActiveTextFg,
            [this] { enterRecoveryKey(); });

        auto *forgotContainer = new QWidget(content);
        auto *forgotLayout = new QHBoxLayout(forgotContainer);
        forgotLayout->setContentsMargins(
            st::settingsButtonPaddingLeft,
            4,
            st::settingsButtonPaddingRight,
            4);

        // Flat underlined "link" button painted with live st:: colors.
        auto forgotFont = st::baseFont(13);
        forgotFont.setUnderline(true);
        ::Ui::TextButton::Style forgotStyle;
        forgotStyle.fg = &st::windowActiveTextFg;  // transparent bg (flat)
        forgotStyle.paddingH = 0;
        auto *forgotBtn = new ::Ui::TextButton(
            tr("Forgot recovery key?"), forgotStyle, forgotContainer);
        forgotBtn->setFont(forgotFont);
        connect(forgotBtn, &QAbstractButton::clicked, this, [this] {
            resetIdentity();
        });
        forgotLayout->addWidget(forgotBtn);
        forgotLayout->addStretch(1);
        layout->addWidget(forgotContainer);
    } else if (state == EncryptionHealthState::IdentityNeedsReset) {
        auto *descLabel = new QLabel(
            tr("Your encryption keys are no longer available. Reset to create new ones. "
               "You will lose access to previously encrypted messages."),
            content);
        descLabel->setFont(st::baseFont(13));
        descLabel->setWordWrap(true);
        {
            QPalette pal = descLabel->palette();
            pal.setColor(QPalette::WindowText, st::windowSubTextFg);
            descLabel->setPalette(pal);
        }
        descLabel->setContentsMargins(
            st::settingsButtonPaddingLeft,
            0,
            st::settingsButtonPaddingRight,
            12);
        layout->addWidget(descLabel);

        addFilledButton(
            tr("Reset identity"),
            &st::attentionButtonFg,
            &st::attentionButtonBgOver,
            [this] { resetIdentity(); });
    }

    addSettingsInfoRow(content, layout,
        tr("Device ID"), overview.deviceId, true, true);
    addSettingsInfoRow(content, layout,
        tr("Session Key"), overview.deviceEd25519, true, true);

    auto *keyStorageToggle = addSettingsToggle(
        content,
        layout,
        tr("Allow key storage"),
        overview.keyBackupUploadActive);
    connect(keyStorageToggle, &SettingsToggleButton::toggled, this, [this](bool checked) {
        if (!checked) {
            const auto confirmed = confirmSettingsAction(this,
                tr("Disable Key Storage"),
                tr("Disabling key storage means new devices won't be able to access "
                   "your message history. Continue?"),
                tr("Disable"),
                tr("Cancel"),
                HistoryConfirmDialog::Attention);
            if (!confirmed) {
                auto *toggle = qobject_cast<SettingsToggleButton*>(sender());
                if (toggle) {
                    toggle->blockSignals(true);
                    toggle->setChecked(true);
                    toggle->blockSignals(false);
                }
                return;
            }
        }
        if (auto *bridge = _controller ? _controller->bridge() : nullptr) {
            bridge->setKeyStorageEnabled(checked);
        }
    });

    auto addActionRow = [this, layout, content](
            const QString &text,
            const QColor &color,
            std::function<void()> action) {
        auto *row = new EncryptionActionRow(text, color, content);
        connect(row, &EncryptionActionRow::clicked, this, [action] {
            action();
        });
        layout->addWidget(row);
    };

    addActionRow(tr("Change recovery key"), st::windowActiveTextFg, [this] {
        if (_recoveryKeyChangeInProgress) {
            return;
        }
        if (auto *bridge = _controller ? _controller->bridge() : nullptr) {
            _recoveryKeyChangeInProgress = true;
            bridge->createRecoveryKey();
        }
    });

    addActionRow(tr("Import E2EE room keys"), st::windowActiveTextFg, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this,
            tr("Import E2EE room keys"),
            QString(),
            tr("Key files (*.txt *.bin);;All files (*)"));
        if (path.isEmpty()) {
            return;
        }
        SettingsPassphraseDialog dialog(
            tr("Passphrase"),
            tr("Enter the passphrase for the key file:"),
            this);
        if (dialog.exec() == SettingsPassphraseDialog::Accepted) {
            const auto passphrase = dialog.passphrase();
            if (passphrase.isEmpty()) {
                return;
            }
            if (auto *bridge = _controller ? _controller->bridge() : nullptr) {
                bridge->importE2EKeys(path, passphrase);
            }
        }
    });

    addActionRow(tr("Export E2EE room keys"), st::windowActiveTextFg, [this] {
        const QString path = QFileDialog::getSaveFileName(
            this,
            tr("Export E2EE room keys"),
            QString(),
            tr("Key files (*.txt *.bin);;All files (*)"));
        if (path.isEmpty()) {
            return;
        }
        SettingsPassphraseDialog dialog(
            tr("Passphrase"),
            tr("Enter a passphrase to protect the exported keys:"),
            this);
        if (dialog.exec() == SettingsPassphraseDialog::Accepted) {
            const auto passphrase = dialog.passphrase();
            if (passphrase.isEmpty()) {
                return;
            }
            if (auto *bridge = _controller ? _controller->bridge() : nullptr) {
                bridge->exportE2EKeys(path, passphrase);
            }
        }
    });

    addActionRow(tr("Reset cryptographic identity"), st::attentionButtonFg, [this] {
        resetIdentity();
    });

    layout->addStretch(1);
}

} // namespace TeleMatrix

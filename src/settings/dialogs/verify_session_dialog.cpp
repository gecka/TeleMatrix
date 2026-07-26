// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "verify_session_dialog.h"
#include "ui/style/runtime_scale.h"

#include <QAbstractButton>
#include <QByteArray>
#include <QEventLoop>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QStackedWidget>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include "protocol/protocol_bridge.h"
#include "settings/settings_common_widgets.h"
#include "styles/style_constants.h"
#include "ui/focus_restore.h"
#include "ui/painter.h"
#include "ui/recovery_key_format.h"
#include "ui/qr_code_image.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/close_button.h"
#include "ui/widgets/input_fields.h"

namespace TeleMatrix {

namespace {

constexpr int kShadowExtend = 10;
constexpr int kPageChoice   = 0;
constexpr int kPageEmoji    = 1;
constexpr int kPageRecovery = 2;
constexpr int kPageSuccess  = 3;
constexpr int kPageQr       = 4;

void paintBoxShadow(QPainter &p, const QRect &boxRect) {
    PainterHighQualityEnabler hq(p);
    p.setPen(Qt::NoPen);
    for (int i = kShadowExtend; i >= 1; --i) {
        const auto progress = qreal(kShadowExtend - i) / kShadowExtend;
        const auto alpha = int(18.0 * progress * progress);
        p.setBrush(st::withAlpha(st::windowShadowFg, alpha));
        const auto r = st::boxRadius + i;
        p.drawRoundedRect(boxRect.adjusted(-i, -i, i, i), r, r);
    }
}

// Panel surface painted with live st:: colors (so it tracks theme changes)
// instead of a frozen stylesheet background.
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

// Container for the emoji grid: paints a rounded windowBgOver background once
// the emojis arrive (toggled via showBackground()), reading st:: live.
class EmojiContainer final : public QWidget {
public:
    explicit EmojiContainer(QWidget *parent) : QWidget(parent) {}

    void showBackground() {
        _showBackground = true;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        if (!_showBackground) {
            return;
        }
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(st::windowBgOver);
        p.drawRoundedRect(
            rect(),
            st::introVerifyEmojiContainerR,
            st::introVerifyEmojiContainerR);
    }

private:
    bool _showBackground = false;
};

// Shows a QR module grid (fixed black-on-white for reliable scanning).
class QrDisplay final : public QWidget {
public:
    explicit QrDisplay(QWidget *parent) : QWidget(parent) {}

    void setQr(const QByteArray &modules, int size) {
        _modules = modules;
        _size = size;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        if (_modules.isEmpty() || _size <= 0) {
            return;
        }
        QPainter p(this);
        paintQrModules(
            p, rect(), _modules, _size, QColor(Qt::black), QColor(Qt::white));
    }

private:
    QByteArray _modules;
    int _size = 0;
};

// "Danger" text link button: transparent, no background hover; only the text
// color shifts on hover (attentionButtonFg -> attentionButtonFgOver). Painted
// from live st:: colors. (TextButton has no per-state fg, hence a dedicated
// widget.)
class DangerLinkButton final : public QAbstractButton {
public:
    DangerLinkButton(const QString &text, QWidget *parent)
        : QAbstractButton(parent) {
        setText(text);
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);
        setFont(st::baseFont(13));
    }

    [[nodiscard]] QSize sizeHint() const override {
        const QFontMetrics fm(font());
        return QSize(
            fm.horizontalAdvance(text()) + 16,  // padding 8px each side
            fm.height() + 8);                   // padding 4px each side
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setPen(_hovered ? st::attentionButtonFgOver : st::attentionButtonFg);
        p.setFont(font());
        p.drawText(rect(), Qt::AlignCenter, text());
    }
    void enterEvent(QEnterEvent *) override { _hovered = true; update(); }
    void leaveEvent(QEvent *) override { _hovered = false; update(); }

private:
    bool _hovered = false;
};

// Create an "active" (filled accent) TextButton with live st:: colors.
::Ui::TextButton *makeActiveButton(
        const QString &text, int height, QWidget *parent) {
    ::Ui::TextButton::Style style;
    style.bg = &st::activeButtonBg;
    style.bgOver = &st::activeButtonBgOver;
    style.fg = &st::activeButtonFg;
    style.radius = st::boxRadius;
    style.height = height;
    auto *button = new ::Ui::TextButton(text, style, parent);
    auto font = st::baseFont(14);
    font.setWeight(QFont::DemiBold);
    button->setFont(font);
    return button;
}

// Create a "light" (flat) TextButton with live st:: colors.
::Ui::TextButton *makeLightButton(
        const QString &text, int height, QWidget *parent) {
    ::Ui::TextButton::Style style;
    style.bgOver = &st::lightButtonBgOver;  // transparent until hovered
    style.fg = &st::lightButtonFg;
    style.radius = st::boxRadius;
    style.height = height;
    auto *button = new ::Ui::TextButton(text, style, parent);
    auto font = st::baseFont(14);
    font.setWeight(QFont::DemiBold);
    button->setFont(font);
    return button;
}

// Enabled (filled accent) style for an "active" button at the given height.
::Ui::TextButton::Style enabledActiveStyle(int height) {
    ::Ui::TextButton::Style s;
    s.bg = &st::activeButtonBg;
    s.bgOver = &st::activeButtonBgOver;
    s.fg = &st::activeButtonFg;
    s.radius = st::boxRadius;
    s.height = height;
    return s;
}

// Disabled (muted fill, no hover change) style for an "active" button.
::Ui::TextButton::Style disabledActiveStyle(int height) {
    ::Ui::TextButton::Style s;
    s.bg = &st::windowBgOver;
    s.bgOver = &st::windowBgOver;  // no hover change while disabled
    s.fg = &st::windowSubTextFg;
    s.radius = st::boxRadius;
    s.height = height;
    return s;
}

// Set an "active" button's enabled state and swap its style so the disabled
// state is visually muted (the old QSS :disabled rule, lost in the H4 pass).
void setActiveButtonEnabled(
        ::Ui::TextButton *button, bool enabled, int height) {
    button->setEnabled(enabled);
    button->setButtonStyle(
        enabled ? enabledActiveStyle(height) : disabledActiveStyle(height));
}

// Apply a label's window-text color in place of the inline QLabel stylesheet.
void applyLabelColor(QLabel *label, const QColor &color) {
    QPalette pal = label->palette();
    pal.setColor(QPalette::WindowText, color);
    label->setPalette(pal);
}

// Process-wide handle to the currently-open verification dialog. The backend
// keeps a single active verification flow, so a second dialog would strand the
// first; opening a new one supersedes any existing one.
VerifySessionDialog *g_activeVerifyDialog = nullptr;

} // namespace

VerifySessionDialog::VerifySessionDialog(
    ProtocolBridge *bridge,
    QWidget *parent,
    StartMode startMode,
    const QString &transactionId,
    const QString &targetUserId,
    const QString &targetDisplayName)
    : QWidget(parent ? parent->window() : nullptr)
    , _bridge(bridge)
    , _startMode(startMode)
    , _transactionId(transactionId)
    , _targetUserId(targetUserId)
    , _targetDisplayName(targetDisplayName)
{
    // Supersede any dialog already open — reject() cancels its backend flow and
    // quits its nested loop, so only one verification is ever live at a time.
    if (g_activeVerifyDialog && g_activeVerifyDialog != this) {
        g_activeVerifyDialog->reject();
    }
    g_activeVerifyDialog = this;

    if (parentWidget()) {
        setGeometry(parentWidget()->rect());
        parentWidget()->installEventFilter(this);
    }

    setFocusPolicy(Qt::StrongFocus);

    // Background fade-in animation.
    _a_shown = new QVariantAnimation(this);
    _a_shown->setDuration(200);
    _a_shown->setEasingCurve(QEasingCurve::OutCirc);
    _a_shown->setStartValue(0.0);
    _a_shown->setEndValue(1.0);
    connect(_a_shown, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) {
        _bgOpacity = value.toReal();
        update();
    });

    // Layer scale-in animation.
    _a_layerShown = new QVariantAnimation(this);
    _a_layerShown->setDuration(200);
    _a_layerShown->setEasingCurve(QEasingCurve::Linear);
    _a_layerShown->setStartValue(0.0);
    _a_layerShown->setEndValue(1.0);
    connect(_a_layerShown, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) {
        _layerOpacity = value.toReal();
        if (_panel && !_panel->isVisible() && _layerOpacity > 0) {
            _panel->setVisible(true);
        }
        update();
    });

    // Root layout — centers the panel vertically.
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addStretch(1);

    // Panel container.
    _panel = new RoundedPanel(this);
    _panel->setVisible(false);
    _panel->setFixedWidth(TeleMatrix::Style::ConvertScale(500));
    root->addWidget(_panel, 0, Qt::AlignHCenter);
    root->addStretch(1);

    auto *panelLayout = new QVBoxLayout(_panel);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(0);

    // Title bar.
    auto *titleBar = new QWidget(_panel);
    titleBar->setFixedHeight(st::boxTitleHeight);
    panelLayout->addWidget(titleBar);

    _titleLabel = new QLabel(tr("Verify this session"), titleBar);
    _titleLabel->setFont(st::boxTitleFont);
    applyLabelColor(_titleLabel, st::boxTitleFg);
    _titleLabel->setFixedWidth(
        TeleMatrix::Style::ConvertScale(500) - st::boxTitlePosition.x()
            - st::settingsCloseButtonSize - 8);
    _titleLabel->move(st::boxTitlePosition.x(), st::boxTitlePosition.y());

    // Close button.
    auto *close = new ::Ui::CloseButton(titleBar);
    close->move(
        TeleMatrix::Style::ConvertScale(500) - st::settingsCloseButtonSize, 0);
    connect(close, &::Ui::CloseButton::clicked, this,
            [this] { reject(); });
    _closeButton = close;

    // Stacked widget for the 4 pages.
    _stack = new QStackedWidget(_panel);
    panelLayout->addWidget(_stack);

    // Latch our flow id from the first state that carries one. The outgoing
    // cross-user path (and a freshly-started self verify) has no flow id up
    // front — startUserVerification returns only emojis — so without this the
    // per-page guards below (which ignore Cancelled from a *different* flow)
    // stay disabled and any unrelated flow's Cancelled would fail this dialog.
    // Connected before the page builders so it runs first for each state.
    if (_bridge) {
        connect(_bridge, &ProtocolBridge::verificationStateChanged, this,
                [this](int, const QString &flowId) {
            if (_transactionId.isEmpty() && !flowId.isEmpty()) {
                _transactionId = flowId;
            }
        });
    }

    buildChoicePage();
    buildEmojiPage();
    buildRecoveryPage();
    buildSuccessPage();
    buildQrPage();

    if (!_targetUserId.isEmpty()) {
        // Verifying ANOTHER user: straight to emoji via the cross-user start
        // (the Choice/Recovery pages are session-only and never shown here).
        showPage(kPageEmoji);
        QTimer::singleShot(0, this, [this] {
            if (_bridge) {
                _bridge->startUserVerification(_targetUserId);
            }
        });
    } else if (_startMode == StartMode::Emoji) {
        showPage(kPageEmoji);
        QTimer::singleShot(0, this, [this] {
            if (_bridge) {
                _bridge->startSasVerification(_transactionId);
            }
        });
    } else {
        showPage(kPageChoice);
    }
}

VerifySessionDialog::~VerifySessionDialog() {
    if (g_activeVerifyDialog == this) {
        g_activeVerifyDialog = nullptr;
    }
    // Disconnect all bridge signals to avoid callbacks after destruction.
    if (_bridge) {
        disconnect(_bridge, nullptr, this, nullptr);
    }
}

// --- Page 0: Choice ---

void VerifySessionDialog::buildChoicePage() {
    auto *page = new QWidget(_stack);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(
        st::boxPadding.left(),
        st::boxPadding.top(),
        st::boxPadding.right(),
        4);
    layout->setSpacing(10);

    auto *descLabel = new QLabel(
        tr("Choose how to verify this session to access "
            "your encrypted messages."),
        page);
    descLabel->setFont(st::baseFont(14));
    descLabel->setWordWrap(true);
    applyLabelColor(descLabel, st::windowFg);
    layout->addWidget(descLabel);

    layout->addSpacing(4);

    // All three options share the light style on purpose: the user is picking a
    // method, not confirming a recommendation, so none of them is the default.
    auto *emojiBtn = makeLightButton(
        tr("Verify with emoji"),
        TeleMatrix::Style::ConvertScale(36),
        page);
    connect(emojiBtn, &QAbstractButton::clicked, this, [this] {
        showPage(kPageEmoji);
        // Start the SAS flow.
        if (_bridge) {
            _bridge->startSasVerification(_transactionId);
        }
    });
    layout->addWidget(emojiBtn);

    // "Verify with QR code" button.
    auto *qrBtn = makeLightButton(
        tr("Verify with QR code"),
        TeleMatrix::Style::ConvertScale(36),
        page);
    connect(qrBtn, &QAbstractButton::clicked, this, [this] {
        showPage(kPageQr);
        if (_bridge) {
            _bridge->startQrVerification(_transactionId);
        }
    });
    layout->addWidget(qrBtn);

    // "Enter recovery key" button.
    auto *recoveryBtn = makeLightButton(
        tr("Enter recovery key"),
        TeleMatrix::Style::ConvertScale(36),
        page);
    connect(recoveryBtn, &QAbstractButton::clicked, this, [this] {
        showPage(kPageRecovery);
    });
    layout->addWidget(recoveryBtn);

    layout->addSpacing(4);

    // Cancel button row.
    auto *buttonsContainer = new QWidget(page);
    buttonsContainer->setFixedHeight(
        st::boxButtonPadding.top()
        + st::boxButtonHeight
        + st::boxButtonPadding.bottom());

    auto *buttonsLayout = new QHBoxLayout(buttonsContainer);
    buttonsLayout->setContentsMargins(0, st::boxButtonPadding.top(), 0, st::boxButtonPadding.bottom());
    buttonsLayout->setSpacing(8);
    buttonsLayout->addStretch(1);

    auto *cancelBtn = makeLightButton(
        tr("Cancel"), st::boxButtonHeight, buttonsContainer);
    connect(cancelBtn, &QAbstractButton::clicked, this, [this] { reject(); });
    buttonsLayout->addWidget(cancelBtn);

    layout->addWidget(buttonsContainer);

    _stack->addWidget(page); // index 0
}

// --- Page 1: Emoji SAS ---

void VerifySessionDialog::buildEmojiPage() {
    _emojiPage = new QWidget(_stack);
    auto *layout = new QVBoxLayout(_emojiPage);
    layout->setContentsMargins(
        st::boxPadding.left(),
        st::boxPadding.top(),
        st::boxPadding.right(),
        4);
    layout->setSpacing(10);

    // Waiting label (shown until emojis arrive).
    _emojiWaitLabel = new QLabel(
        tr("Waiting for the other device..."),
        _emojiPage);
    _emojiWaitLabel->setFont(st::baseFont(14));
    _emojiWaitLabel->setAlignment(Qt::AlignCenter);
    applyLabelColor(_emojiWaitLabel, st::windowSubTextFg);
    _emojiWaitLabel->setFixedHeight(st::introVerifyEmojiContainerH);
    layout->addWidget(_emojiWaitLabel);

    // Emoji container — hidden until emojis arrive.
    // We use a custom-painted widget for the emoji grid, following
    // the same approach as IntroVerifyEmoji.
    auto *emojiContainer = new EmojiContainer(_emojiPage);
    _emojiContainer = emojiContainer;
    _emojiContainer->setFixedSize(
        st::introVerifyEmojiContainerW,
        st::introVerifyEmojiContainerH);
    _emojiContainer->hide();
    layout->addWidget(_emojiContainer, 0, Qt::AlignHCenter);

    // Error label.
    _emojiErrorLabel = new QLabel(_emojiPage);
    _emojiErrorLabel->setFont(st::baseFont(12));
    applyLabelColor(_emojiErrorLabel, st::attentionButtonFg);
    _emojiErrorLabel->setWordWrap(true);
    _emojiErrorLabel->hide();
    layout->addWidget(_emojiErrorLabel);

    // Buttons.
    auto *buttonsContainer = new QWidget(_emojiPage);
    buttonsContainer->setFixedHeight(
        st::boxButtonPadding.top()
        + st::boxButtonHeight
        + st::boxButtonPadding.bottom());

    auto *buttonsLayout = new QHBoxLayout(buttonsContainer);
    buttonsLayout->setContentsMargins(0, st::boxButtonPadding.top(), 0, st::boxButtonPadding.bottom());
    buttonsLayout->setSpacing(8);
    buttonsLayout->addStretch(1);

    _emojiNoMatchButton = new DangerLinkButton(
        tr("They Don\u2019t Match"), buttonsContainer);
    _emojiNoMatchButton->setFixedHeight(st::boxButtonHeight);
    connect(_emojiNoMatchButton, &QAbstractButton::clicked, this, [this] {
        if (_bridge) {
            _bridge->mismatchSas();
        }
        reject();
    });
    buttonsLayout->addWidget(_emojiNoMatchButton);

    _emojiMatchButton = makeActiveButton(
        tr("They Match"), st::boxButtonHeight, buttonsContainer);
    setActiveButtonEnabled(_emojiMatchButton, false, st::boxButtonHeight);
    connect(_emojiMatchButton, &QAbstractButton::clicked, this, [this] {
        setActiveButtonEnabled(_emojiMatchButton, false, st::boxButtonHeight);
        _emojiMatchButton->setText(tr("Confirming..."));
        _emojiNoMatchButton->hide();
        if (_bridge) {
            _bridge->confirmSasMatch();
        }
    });
    buttonsLayout->addWidget(_emojiMatchButton);

    layout->addWidget(buttonsContainer);

    const auto showEmojiFailure = [this] {
        // Replace emoji grid content with failure message inside
        // the highlighted container area.
        _emojiWaitLabel->hide();
        _emojiNoMatchButton->hide();

        // Clear emoji grid and show error text inside the container.
        if (_emojiContainer->layout()) {
            clearSettingsLayout(_emojiContainer->layout());
            delete _emojiContainer->layout();
        }
        auto *errorLayout = new QVBoxLayout(_emojiContainer);
        errorLayout->setContentsMargins(16, 16, 16, 16);
        auto *errorText = new QLabel(tr(
            "The request was denied or timed out, "
            "or there was a verification mismatch"), _emojiContainer);
        errorText->setFont(st::baseFont(15));
        errorText->setWordWrap(true);
        errorText->setAlignment(Qt::AlignCenter);
        applyLabelColor(errorText, st::windowFg);
        errorLayout->addWidget(errorText);
        _emojiContainer->show();

        _emojiErrorLabel->hide();
        _titleLabel->setText(tr("Verification failed"));

        // Repurpose match button as "Close".
        _emojiMatchButton->setText(tr("Close"));
        setActiveButtonEnabled(_emojiMatchButton, true, st::boxButtonHeight);
        _emojiMatchButton->show();
        _emojiMatchButton->disconnect();
        connect(_emojiMatchButton, &QAbstractButton::clicked, this, [this] {
            reject();
        });
    };

    // Connect bridge signals.
    connect(_bridge, &ProtocolBridge::sasVerificationStarted,
            this, [this, emojiContainer, showEmojiFailure](bool success, const QStringList &emojis, const QStringList &labels) {
        if (!success) {
            showEmojiFailure();
            return;
        }

        _emojiWaitLabel->hide();
        _emojiErrorLabel->hide();

        // Build the emoji grid inside _emojiContainer.
        // Remove old content if any.
        if (_emojiContainer->layout()) {
            clearSettingsLayout(_emojiContainer->layout());
            delete _emojiContainer->layout();
        }

        auto *grid = new QVBoxLayout(_emojiContainer);
        grid->setContentsMargins(
            st::introVerifyEmojiPadding,
            st::introVerifyEmojiPadding,
            st::introVerifyEmojiPadding,
            st::introVerifyEmojiPadding);
        grid->setSpacing(0);

        // Compute rows: 4 in first row, rest in second.
        const int count = emojis.size();
        const int firstRowCount = qMin(count, 4);
        const int secondRowCount = qMax(0, count - 4);

        auto makeRow = [&](int start, int rowCount) {
            auto *rowWidget = new QWidget(_emojiContainer);
            auto *rowLayout = new QHBoxLayout(rowWidget);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(0);
            for (int i = start; i < start + rowCount; ++i) {
                auto *cell = new QWidget(rowWidget);
                auto *cellLayout = new QVBoxLayout(cell);
                cellLayout->setContentsMargins(0, 0, 0, 0);
                cellLayout->setSpacing(st::introVerifyEmojiCellGap);

                auto *emojiLabel = new QLabel(emojis.value(i), cell);
                emojiLabel->setFont(st::baseFont(st::introVerifyEmojiFontSize));
                emojiLabel->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
                applyLabelColor(emojiLabel, st::windowBoldFg);
                cellLayout->addWidget(emojiLabel, 3);

                auto *labelText = new QLabel(labels.value(i), cell);
                labelText->setFont(st::baseFont(st::introVerifyEmojiLabelSize));
                labelText->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
                applyLabelColor(labelText, st::windowSubTextFg);
                cellLayout->addWidget(labelText, 1);

                rowLayout->addWidget(cell);
            }
            return rowWidget;
        };

        if (firstRowCount > 0) {
            grid->addWidget(makeRow(0, firstRowCount), 1);
        }
        if (secondRowCount > 0) {
            grid->addWidget(makeRow(4, secondRowCount), 1);
        }

        emojiContainer->showBackground();
        _emojiContainer->show();
        setActiveButtonEnabled(_emojiMatchButton, true, st::boxButtonHeight);
    });

    connect(_bridge, &ProtocolBridge::sasConfirmed,
            this, [this, showEmojiFailure](bool success) {
        if (success) {
            showPage(kPageSuccess);
        } else {
            showEmojiFailure();
        }
    });

    // Handle Cancelled state from the Rust verification state machine.
    connect(_bridge, &ProtocolBridge::verificationStateChanged,
            this, [this, showEmojiFailure](int state, const QString &flowId) {
        constexpr int kCancelled = 9;
        if (state != kCancelled || _stack->currentIndex() != kPageEmoji) {
            return;
        }
        // Ignore a Cancelled belonging to a different flow (e.g. a QR flow torn
        // down when switching to emoji).
        if (!flowId.isEmpty() && !_transactionId.isEmpty() && flowId != _transactionId) {
            return;
        }
        showEmojiFailure();
    });

    _stack->addWidget(_emojiPage); // index 1
}

// --- Page 4: QR code ---

void VerifySessionDialog::buildQrPage() {
    _qrPage = new QWidget(_stack);
    auto *layout = new QVBoxLayout(_qrPage);
    layout->setContentsMargins(
        st::boxPadding.left(),
        st::boxPadding.top(),
        st::boxPadding.right(),
        4);
    layout->setSpacing(10);

    _qrWaitLabel = new QLabel(tr("Waiting for your other session..."), _qrPage);
    _qrWaitLabel->setFont(st::baseFont(14));
    _qrWaitLabel->setAlignment(Qt::AlignCenter);
    applyLabelColor(_qrWaitLabel, st::windowSubTextFg);
    _qrWaitLabel->setFixedHeight(st::introVerifyEmojiContainerH);
    layout->addWidget(_qrWaitLabel);

    auto *qrDisplay = new QrDisplay(_qrPage);
    _qrDisplay = qrDisplay;
    qrDisplay->setFixedSize(
        st::introVerifyEmojiContainerH, st::introVerifyEmojiContainerH);
    qrDisplay->hide();
    layout->addWidget(qrDisplay, 0, Qt::AlignHCenter);

    _qrErrorLabel = new QLabel(_qrPage);
    _qrErrorLabel->setFont(st::baseFont(12));
    applyLabelColor(_qrErrorLabel, st::attentionButtonFg);
    _qrErrorLabel->setWordWrap(true);
    _qrErrorLabel->hide();
    layout->addWidget(_qrErrorLabel);

    auto *buttonsContainer = new QWidget(_qrPage);
    buttonsContainer->setFixedHeight(
        st::boxButtonPadding.top()
        + st::boxButtonHeight
        + st::boxButtonPadding.bottom());

    auto *buttonsLayout = new QHBoxLayout(buttonsContainer);
    buttonsLayout->setContentsMargins(0, st::boxButtonPadding.top(), 0, st::boxButtonPadding.bottom());
    buttonsLayout->setSpacing(8);
    buttonsLayout->addStretch(1);

    auto *cancelBtn = new DangerLinkButton(tr("Cancel"), buttonsContainer);
    cancelBtn->setFixedHeight(st::boxButtonHeight);
    connect(cancelBtn, &QAbstractButton::clicked, this, [this] {
        if (_bridge) {
            _bridge->cancelVerification(_transactionId);
        }
        reject();
    });
    buttonsLayout->addWidget(cancelBtn);

    _qrConfirmButton = makeActiveButton(
        tr("Continue"), st::boxButtonHeight, buttonsContainer);
    setActiveButtonEnabled(_qrConfirmButton, false, st::boxButtonHeight);
    connect(_qrConfirmButton, &QAbstractButton::clicked, this, [this] {
        setActiveButtonEnabled(_qrConfirmButton, false, st::boxButtonHeight);
        _qrConfirmButton->setText(tr("Confirming..."));
        if (_bridge) {
            _bridge->confirmQrScanned();
        }
    });
    buttonsLayout->addWidget(_qrConfirmButton);

    layout->addWidget(buttonsContainer);

    const auto showQrFailure = [this] {
        _qrWaitLabel->hide();
        if (_qrDisplay) {
            _qrDisplay->hide();
        }
        _titleLabel->setText(tr("Verification failed"));
        _qrErrorLabel->setText(tr(
            "The request was denied or timed out, "
            "or there was a verification mismatch"));
        _qrErrorLabel->show();
        _qrConfirmButton->setText(tr("Close"));
        setActiveButtonEnabled(_qrConfirmButton, true, st::boxButtonHeight);
        _qrConfirmButton->disconnect();
        connect(_qrConfirmButton, &QAbstractButton::clicked, this, [this] {
            reject();
        });
    };

    connect(_bridge, &ProtocolBridge::qrCodeReady,
            this, [this, qrDisplay, showQrFailure](
                bool success, const QByteArray &modules, int size) {
        if (_stack->currentIndex() != kPageQr) {
            return;
        }
        if (!success || modules.isEmpty() || size <= 0) {
            showQrFailure();
            return;
        }
        _qrWaitLabel->hide();
        _qrErrorLabel->hide();
        qrDisplay->setQr(modules, size);
        qrDisplay->show();
    });

    connect(_bridge, &ProtocolBridge::qrScanConfirmed,
            this, [this, showQrFailure](bool success) {
        if (_stack->currentIndex() != kPageQr) {
            return;
        }
        if (success) {
            showPage(kPageSuccess);
        } else {
            showQrFailure();
        }
    });

    connect(_bridge, &ProtocolBridge::verificationStateChanged,
            this, [this, showQrFailure](int state, const QString &flowId) {
        if (_stack->currentIndex() != kPageQr) {
            return;
        }
        // Ignore states belonging to a different flow than the one this dialog runs.
        if (!flowId.isEmpty() && !_transactionId.isEmpty() && flowId != _transactionId) {
            return;
        }
        constexpr int kQrCodeScanned = 7;
        constexpr int kCancelled = 9;
        if (state == kQrCodeScanned) {
            _titleLabel->setText(tr("Confirm on your other session"));
            setActiveButtonEnabled(_qrConfirmButton, true, st::boxButtonHeight);
        } else if (state == kCancelled) {
            showQrFailure();
        }
    });

    _stack->addWidget(_qrPage); // index 4
}

// --- Page 2: Recovery Key Entry ---

void VerifySessionDialog::buildRecoveryPage() {
    _recoveryPage = new QWidget(_stack);
    auto *layout = new QVBoxLayout(_recoveryPage);
    layout->setContentsMargins(
        st::boxPadding.left(),
        st::boxPadding.top(),
        st::boxPadding.right(),
        4);
    layout->setSpacing(10);

    // The stack sizes every page to its tallest sibling; without stretches the
    // spare height spreads between the rows. Bracketing stretches centre the
    // description+input block between the title and the bottom buttons.
    layout->addStretch(1);

    auto *descLabel = new QLabel(
        tr("Enter your recovery key to verify this session "
            "and restore access to encrypted messages."),
        _recoveryPage);
    descLabel->setFont(st::baseFont(14));
    descLabel->setWordWrap(true);
    applyLabelColor(descLabel, st::windowFg);
    layout->addWidget(descLabel);

    _recoveryInput = new ::Ui::BorderedLineEdit(_recoveryPage);
    _recoveryInput->setPlaceholderText(tr("Recovery key"));
    _recoveryInput->setFixedHeight(TeleMatrix::Style::ConvertScale(36));
    _recoveryInput->setMaxLength(59);
    _recoveryInput->setFont(st::monospaceFont(12));
    // Type the key in groups of four, as it was shown when it was created.
    connect(_recoveryInput, &QLineEdit::textEdited, this, [this] {
        const auto formatted = TeleMatrix::FormatRecoveryKey(
            _recoveryInput->text(), _recoveryInput->cursorPosition());
        if (formatted.text == _recoveryInput->text()) {
            return;
        }
        _recoveryInput->setText(formatted.text);
        _recoveryInput->setCursorPosition(formatted.cursor);
    });
    layout->addWidget(_recoveryInput);

    // Error label.
    _recoveryErrorLabel = new QLabel(_recoveryPage);
    _recoveryErrorLabel->setFont(st::baseFont(12));
    applyLabelColor(_recoveryErrorLabel, st::attentionButtonFg);
    _recoveryErrorLabel->setWordWrap(true);
    _recoveryErrorLabel->hide();
    layout->addWidget(_recoveryErrorLabel);

    layout->addStretch(1);

    // Buttons.
    auto *buttonsContainer = new QWidget(_recoveryPage);
    buttonsContainer->setFixedHeight(
        st::boxButtonPadding.top()
        + st::boxButtonHeight
        + st::boxButtonPadding.bottom());

    auto *buttonsLayout = new QHBoxLayout(buttonsContainer);
    buttonsLayout->setContentsMargins(0, st::boxButtonPadding.top(), 0, st::boxButtonPadding.bottom());
    buttonsLayout->setSpacing(8);
    buttonsLayout->addStretch(1);

    auto *backBtn = makeLightButton(
        tr("Back"), st::boxButtonHeight, buttonsContainer);
    connect(backBtn, &QAbstractButton::clicked, this, [this] {
        _recoveryErrorLabel->hide();
        _recoveryInput->clear();
        showPage(kPageChoice);
    });
    buttonsLayout->addWidget(backBtn);

    _recoverySubmitButton = makeActiveButton(
        tr("Verify"), st::boxButtonHeight, buttonsContainer);
    setActiveButtonEnabled(_recoverySubmitButton, false, st::boxButtonHeight);
    connect(_recoverySubmitButton, &QAbstractButton::clicked, this, [this] {
        const auto key = _recoveryInput->text().trimmed();
        if (key.isEmpty()) return;

        setActiveButtonEnabled(_recoverySubmitButton, false, st::boxButtonHeight);
        _recoverySubmitButton->setText(tr("Verifying..."));
        _recoveryInput->setEnabled(false);
        _recoveryErrorLabel->hide();

        if (_bridge) {
            _bridge->verifyWithRecoveryKey(key);
        }
    });
    buttonsLayout->addWidget(_recoverySubmitButton);

    layout->addWidget(buttonsContainer);

    // Enable submit button when input has text.
    connect(_recoveryInput, &QLineEdit::textChanged, this, [this] {
        const bool hasText = !_recoveryInput->text().trimmed().isEmpty();
        setActiveButtonEnabled(_recoverySubmitButton, hasText, st::boxButtonHeight);
        if (_recoveryErrorLabel->isVisible()) {
            _recoveryErrorLabel->hide();
        }
    });

    // Enter key submits.
    connect(_recoveryInput, &QLineEdit::returnPressed, this, [this] {
        if (_recoverySubmitButton->isEnabled()) {
            _recoverySubmitButton->click();
        }
    });

    // Connect bridge signal for recovery key result.
    connect(_bridge, &ProtocolBridge::recoveryKeyVerified,
            this, [this](bool success) {
        if (success) {
            showPage(kPageSuccess);
        } else {
            setActiveButtonEnabled(_recoverySubmitButton, true, st::boxButtonHeight);
            _recoverySubmitButton->setText(tr("Verify"));
            _recoveryInput->setEnabled(true);
            _recoveryErrorLabel->setText(
                tr("Invalid recovery key. Please check and try again."));
            _recoveryErrorLabel->show();
            _recoveryInput->setFocus();
        }
    });

    _stack->addWidget(_recoveryPage); // index 2
}

// --- Page 3: Success ---

void VerifySessionDialog::buildSuccessPage() {
    auto *page = new QWidget(_stack);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(
        st::boxPadding.left(),
        st::boxPadding.top() + 8,
        st::boxPadding.right(),
        4);
    layout->setSpacing(10);

    // Green checkmark circle — paint via a QWidget subclass.
    class CheckmarkWidget : public QWidget {
    public:
        explicit CheckmarkWidget(QWidget *parent) : QWidget(parent) {
            setFixedSize(st::introVerifyCheckSize, st::introVerifyCheckSize);
        }
    protected:
        void paintEvent(QPaintEvent *) override {
            QPainter p(this);
            PainterHighQualityEnabler hq(p);

            const auto s = st::introVerifyCheckSize;
            p.setPen(Qt::NoPen);
            p.setBrush(st::introVerifySuccessBg);
            p.drawEllipse(0, 0, s, s);

            // White checkmark.
            const auto cx = s / 2;
            const auto cy = s / 2;
            const auto scale = s / 64.0;

            QPainterPath checkPath;
            checkPath.moveTo(cx - 14 * scale, cy - 1 * scale);
            checkPath.lineTo(cx - 4 * scale, cy + 10 * scale);
            checkPath.lineTo(cx + 14 * scale, cy - 10 * scale);

            p.setPen(QPen(st::windowBg, 2.0 * scale, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            p.drawPath(checkPath);
        }
    };

    auto *checkmark = new CheckmarkWidget(page);
    layout->addWidget(checkmark, 0, Qt::AlignHCenter);

    layout->addSpacing(8);

    auto *successTitle = new QLabel(
        tr("Session Verified"), page);
    successTitle->setFont(st::boxTitleFont);
    successTitle->setAlignment(Qt::AlignCenter);
    applyLabelColor(successTitle, st::windowFg);
    layout->addWidget(successTitle);

    auto *successDesc = new QLabel(
        tr("This session is now verified. "
            "Your encrypted messages are secure."),
        page);
    successDesc->setFont(st::baseFont(14));
    successDesc->setWordWrap(true);
    successDesc->setAlignment(Qt::AlignCenter);
    applyLabelColor(successDesc, st::windowSubTextFg);
    layout->addWidget(successDesc);

    layout->addSpacing(8);

    // "Done" button.
    auto *buttonsContainer = new QWidget(page);
    buttonsContainer->setFixedHeight(
        st::boxButtonPadding.top()
        + st::boxButtonHeight
        + st::boxButtonPadding.bottom());

    auto *buttonsLayout = new QHBoxLayout(buttonsContainer);
    buttonsLayout->setContentsMargins(0, st::boxButtonPadding.top(), 0, st::boxButtonPadding.bottom());
    buttonsLayout->setSpacing(8);
    buttonsLayout->addStretch(1);

    auto *doneBtn = makeActiveButton(
        tr("Done"), st::boxButtonHeight, buttonsContainer);
    connect(doneBtn, &QAbstractButton::clicked, this, [this] { accept(); });
    buttonsLayout->addWidget(doneBtn);

    layout->addWidget(buttonsContainer);

    _stack->addWidget(page); // index 3
}

void VerifySessionDialog::showPage(int index) {
    _stack->setCurrentIndex(index);

    switch (index) {
    case kPageChoice:
        _titleLabel->setText(tr("Verify this session"));
        break;
    case kPageEmoji:
        _titleLabel->setText(_targetDisplayName.isEmpty()
            ? tr("Compare emojis")
            : tr("Verify %1").arg(_targetDisplayName));
        break;
    case kPageRecovery:
        _titleLabel->setText(tr("Enter recovery key"));
        QTimer::singleShot(0, _recoveryInput, [this] {
            _recoveryInput->setFocus();
        });
        break;
    case kPageSuccess:
        _titleLabel->setText(_targetDisplayName.isEmpty()
            ? tr("Session verified")
            : tr("%1 verified").arg(_targetDisplayName));
        break;
    case kPageQr:
        _titleLabel->setText(tr("Scan QR code"));
        break;
    }
}

int VerifySessionDialog::exec() {
    const auto restoreFocus = ::TeleMatrix::Focus::saveFocusForPopup();

    raise();
    show();
    setFocus();

    if (_a_shown) _a_shown->start();
    if (_a_layerShown) _a_layerShown->start();

    QEventLoop loop;
    _loop = &loop;
    loop.exec();
    _loop = nullptr;

    hide();
    ::TeleMatrix::Focus::restoreFocusAfterPopup(restoreFocus);
    return _result;
}

void VerifySessionDialog::accept() {
    _result = Accepted;
    if (_loop) _loop->quit();
}

void VerifySessionDialog::reject() {
    // Cancel any in-flight verification to avoid orphaned backend operations.
    if (_bridge) {
        _bridge->cancelVerification(_transactionId);
    }
    _result = Rejected;
    if (_loop) _loop->quit();
}

void VerifySessionDialog::paintEvent(QPaintEvent *) {
    QPainter p(this);

    p.setOpacity(_bgOpacity);
    p.fillRect(rect(), st::layerBg);

    if (_panel && _layerOpacity > 0) {
        p.setOpacity(_layerOpacity);
        paintBoxShadow(p, _panel->geometry());
    }
}

void VerifySessionDialog::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool VerifySessionDialog::eventFilter(QObject *obj, QEvent *event) {
    if (obj == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace TeleMatrix

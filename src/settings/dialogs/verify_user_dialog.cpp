// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "verify_user_dialog.h"
#include "ui/style/runtime_scale.h"

#include <QAbstractButton>
#include <QEventLoop>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
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
#include "ui/emoji_sprites.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/close_button.h"

namespace TeleMatrix {

namespace {

constexpr int kShadowExtend = 10;
constexpr int kPageEmoji   = 0;
constexpr int kPageSuccess = 1;

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

// Process-wide handle to the currently-open dialog. The backend keeps a single
// active verification flow, so a second dialog would strand the first; opening a
// new one supersedes any existing one.
VerifyUserDialog *g_activeVerifyDialog = nullptr;

} // namespace

VerifyUserDialog::VerifyUserDialog(
    ProtocolBridge *bridge,
    QWidget *parent,
    const QString &targetUserId,
    const QString &targetDisplayName,
    const QString &flowId)
    : QWidget(parent ? parent->window() : nullptr)
    , _bridge(bridge)
    , _transactionId(flowId)
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

    _titleLabel = new QLabel(titleBar);
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

    _stack = new QStackedWidget(_panel);
    panelLayout->addWidget(_stack);

    // Latch our flow id from the first state that carries a trustworthy one.
    // The outgoing path has no flow id up front — startUserVerification returns
    // only emojis — so without this the page guard below (which ignores
    // Cancelled from a *different* flow) stays disabled and any unrelated flow's
    // Cancelled would fail this dialog. Connected before the page builders so it
    // runs first for each state.
    if (_bridge) {
        connect(_bridge, &ProtocolBridge::verificationStateChanged, this,
                [this](int state, const QString &flowId) {
            // NOT RequestingVerification: Rust emits it before tagging the new
            // flow's id, so from the second flow in a session onward it still
            // carries the previous (already-ended) flow's id. Latching that
            // strands the dialog for good — every later guard, including the one
            // gating the emojis, then rejects its own flow.
            constexpr int kRequestingVerification = 0;
            if (state == kRequestingVerification) {
                return;
            }
            if (_transactionId.isEmpty() && !flowId.isEmpty()) {
                _transactionId = flowId;
            }
        });
    }

    buildEmojiPage();
    buildSuccessPage();

    showPage(kPageEmoji);
    QTimer::singleShot(0, this, [this] {
        if (!_bridge) {
            return;
        }
        if (!_targetUserId.isEmpty()) {
            _bridge->startUserVerification(_targetUserId);
        } else {
            // Incoming request: attach to the flow it arrived on.
            _bridge->startSasVerification(_transactionId);
        }
    });
}

VerifyUserDialog::~VerifyUserDialog() {
    if (g_activeVerifyDialog == this) {
        g_activeVerifyDialog = nullptr;
    }
    // Disconnect all bridge signals to avoid callbacks after destruction.
    if (_bridge) {
        disconnect(_bridge, nullptr, this, nullptr);
    }
}

// --- Page 0: Emoji SAS ---

void VerifyUserDialog::buildEmojiPage() {
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
        tr("They Don’t Match"), buttonsContainer);
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

    const auto showEmojis = [this, emojiContainer](
            const QStringList &emojis,
            const QStringList &labels) {
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

                // The only SAS emoji surface that is a QLabel rather than a painter, so
                // it takes a pixmap. Falls back to the text label when the emoji has no
                // sprite, which is why the font and colour are still set either way.
                auto *emojiLabel = new QLabel(emojis.value(i), cell);
                emojiLabel->setFont(st::baseFont(st::introVerifyEmojiFontSize));
                emojiLabel->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
                applyLabelColor(emojiLabel, st::windowBoldFg);
                const auto sprite = TeleMatrix::Emoji::Pixmap(
                    emojis.value(i),
                    st::introVerifyEmojiFontSize);
                if (!sprite.isNull()) {
                    emojiLabel->setPixmap(sprite);
                }
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
    };

    // Connect bridge signals.
    connect(_bridge, &ProtocolBridge::sasVerificationStarted,
            this, [showEmojiFailure](bool success, const QStringList &, const QStringList &) {
        if (success) {
            return; // emojis arrive via sasEmojisAvailable; only failures matter
        }
        showEmojiFailure();
    });

    connect(_bridge, &ProtocolBridge::sasEmojisAvailable,
            this, [this, showEmojis](
                const QString &flowId,
                const QStringList &emojis,
                const QStringList &labels) {
        if (!flowId.isEmpty() && !_transactionId.isEmpty()
            && flowId != _transactionId) {
            return;
        }
        if (_transactionId.isEmpty()) {
            _transactionId = flowId;
        }
        _emojisShown = true;
        showEmojis(emojis, labels);
    });

    connect(_bridge, &ProtocolBridge::sasConfirmed,
            this, [showEmojiFailure](bool success) {
        // success only means the confirmation was sent; completion arrives as
        // the Done state below.
        if (success) {
            return;
        }
        showEmojiFailure();
    });

    // Terminal states come from the Rust verification state machine.
    connect(_bridge, &ProtocolBridge::verificationStateChanged,
            this, [this, showEmojiFailure](int state, const QString &flowId) {
        constexpr int kDone = 8;
        constexpr int kCancelled = 9;
        if (_stack->currentIndex() != kPageEmoji) {
            return;
        }
        // Ignore a terminal state belonging to a different flow.
        if (!flowId.isEmpty() && !_transactionId.isEmpty() && flowId != _transactionId) {
            return;
        }
        if (state == kDone) {
            // Only a flow whose emojis we actually showed can have been
            // confirmed here; a Done for anything else (a foreign id latched
            // above) must not claim this user is verified. A cross-user SAS
            // cannot complete without its emojis, so this cannot strand us.
            if (!_emojisShown) {
                return;
            }
            showPage(kPageSuccess);
        } else if (state == kCancelled) {
            showEmojiFailure();
        }
    });

    _stack->addWidget(_emojiPage); // index 0
}

// --- Page 1: Success ---

void VerifyUserDialog::buildSuccessPage() {
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
        _targetDisplayName.isEmpty()
            ? tr("Verified")
            : tr("%1 verified").arg(_targetDisplayName),
        page);
    successTitle->setFont(st::boxTitleFont);
    successTitle->setAlignment(Qt::AlignCenter);
    applyLabelColor(successTitle, st::windowFg);
    layout->addWidget(successTitle);

    auto *successDesc = new QLabel(
        tr("Their identity is confirmed. "
            "Your messages with them are secure."),
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

    _stack->addWidget(page); // index 1
}

void VerifyUserDialog::showPage(int index) {
    _stack->setCurrentIndex(index);

    switch (index) {
    case kPageEmoji:
        _titleLabel->setText(_targetDisplayName.isEmpty()
            ? tr("Compare emojis")
            : tr("Verify %1").arg(_targetDisplayName));
        break;
    case kPageSuccess:
        _titleLabel->setText(_targetDisplayName.isEmpty()
            ? tr("Verified")
            : tr("%1 verified").arg(_targetDisplayName));
        break;
    }
}

int VerifyUserDialog::exec() {
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

void VerifyUserDialog::accept() {
    _result = Accepted;
    if (_loop) _loop->quit();
}

void VerifyUserDialog::reject() {
    // Cancel any in-flight verification to avoid orphaned backend operations.
    if (_bridge) {
        _bridge->cancelVerification(_transactionId);
    }
    _result = Rejected;
    if (_loop) _loop->quit();
}

void VerifyUserDialog::paintEvent(QPaintEvent *) {
    QPainter p(this);

    p.setOpacity(_bgOpacity);
    p.fillRect(rect(), st::layerBg);

    if (_panel && _layerOpacity > 0) {
        p.setOpacity(_layerOpacity);
        paintBoxShadow(p, _panel->geometry());
    }
}

void VerifyUserDialog::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool VerifyUserDialog::eventFilter(QObject *obj, QEvent *event) {
    if (obj == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace TeleMatrix

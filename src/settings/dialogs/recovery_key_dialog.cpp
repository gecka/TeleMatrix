// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "recovery_key_dialog.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFont>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QRegularExpression>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include "styles/style_constants.h"
#include "ui/focus_restore.h"
#include "ui/painter.h"
#include "ui/style/icon_provider.h"
#include "ui/toast_widget.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/close_button.h"
#include "ui/widgets/input_fields.h"

#include <algorithm>

namespace TeleMatrix {

namespace {

constexpr int kShadowExtend = 10;

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

QImage loadTemplateIcon(const QString &path, const QColor &color) {
    return Style::IconProvider::tintedIcon(path, QString(), color);
}

// Copy-to-clipboard icon button painted with live st:: colors instead of an
// inline stylesheet (so hover background and icon tint track theme changes).
class CopyIconButton final : public QAbstractButton {
public:
    explicit CopyIconButton(QWidget *parent) : QAbstractButton(parent) {
        setCursor(Qt::PointingHandCursor);
        setFixedSize(34, 34);
        setMouseTracking(true);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        if (_hovered) {
            p.setPen(Qt::NoPen);
            p.setBrush(st::lightButtonBgOver);
            p.drawRoundedRect(rect(), st::boxRadius, st::boxRadius);
        }
        const auto icon = loadTemplateIcon(
            QStringLiteral(":/telematrix/icons/menu/copy"),
            st::lightButtonFg);
        if (!icon.isNull()) {
            const auto dpr = icon.devicePixelRatio();
            const auto w = int(icon.width() / dpr);
            const auto h = int(icon.height() / dpr);
            p.drawImage(
                QPoint((width() - w) / 2, (height() - h) / 2),
                icon);
        }
    }
    void enterEvent(QEnterEvent *) override { _hovered = true; update(); }
    void leaveEvent(QEvent *) override { _hovered = false; update(); }

private:
    bool _hovered = false;
};

CopyIconButton *createCopyIconButton(QWidget *parent) {
    auto *button = new CopyIconButton(parent);
    button->setToolTip(QCoreApplication::translate(
        "RecoveryKeyDialog",
        "Copy to clipboard"));
    return button;
}

// Rounded panel with a 1px border painted from live st:: colors (used for the
// recovery-key display box that previously used a #RecoveryKeyBox stylesheet).
class BorderedBox final : public QWidget {
public:
    explicit BorderedBox(QWidget *parent) : QWidget(parent) {}

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        const auto r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setBrush(st::windowBgOver);
        p.setPen(QPen(st::inputBorderFg, 1));
        p.drawRoundedRect(r, 4, 4);
    }
};

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

class RecoveryCheckBox final : public QCheckBox {
public:
    RecoveryCheckBox(const QString &text, QWidget *parent)
        : QCheckBox(text, parent) {
        setCursor(Qt::PointingHandCursor);
        setFont(st::baseFont(14));
        setFixedHeight(sizeHint().height());
    }

    QSize sizeHint() const override {
        constexpr auto kIndicator = 18;
        constexpr auto kSkip = 8;
        const auto metrics = QFontMetrics(font());
        return QSize(
            kIndicator + kSkip + metrics.horizontalAdvance(text()),
            std::max(28, metrics.lineSpacing() + 8));
    }

protected:
    void paintEvent(QPaintEvent *) override {
        constexpr auto kIndicator = 18;
        constexpr auto kSkip = 8;

        QPainter p(this);
        PainterHighQualityEnabler hq(p);

        const auto top = (height() - kIndicator) / 2;
        const QRect boxRect(0, top, kIndicator, kIndicator);
        const auto checked = isChecked();

        p.setPen(QPen(checked ? st::activeButtonBg : st::inputBorderFg, 2));
        p.setBrush(checked ? st::activeButtonBg : st::boxBg);
        p.drawRoundedRect(boxRect.adjusted(1, 1, -1, -1), 3, 3);

        if (checked) {
            QPen checkPen(st::activeButtonFg, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            p.setPen(checkPen);
            const auto left = boxRect.left();
            const auto y = boxRect.top();
            p.drawLine(QPointF(left + 4, y + 9), QPointF(left + 8, y + 13));
            p.drawLine(QPointF(left + 8, y + 13), QPointF(left + 14, y + 5));
        }

        p.setPen(st::windowFg);
        p.setFont(font());
        p.drawText(
            QRect(kIndicator + kSkip, 0, width() - kIndicator - kSkip, height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            text());
    }
};

::Ui::TextButton::Style enabledConfirmStyle() {
    ::Ui::TextButton::Style s;
    s.bgOver = &st::lightButtonBgOver;  // transparent until hovered
    s.fg = &st::lightButtonFg;
    s.radius = st::boxRadius;
    s.height = st::boxButtonHeight;
    return s;
}

::Ui::TextButton::Style disabledConfirmStyle() {
    ::Ui::TextButton::Style s;
    s.fg = &st::windowSubTextFg;  // flat, muted, no hover
    s.radius = st::boxRadius;
    s.height = st::boxButtonHeight;
    return s;
}

} // namespace

RecoveryKeyDialog::RecoveryKeyDialog(
    Mode mode,
    const QString &key,
    QWidget *parent)
    : QWidget(parent ? parent->window() : nullptr)
    , _mode(mode)
    , _key(key)
{
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
    const auto panelWidth = (_mode == Display)
        ? st::boxWideWidth + st::settingsSidebarWidth
        : st::boxWideWidth;
    _panel = new RoundedPanel(this);
    _panel->setVisible(false);
    _panel->setFixedWidth(panelWidth);
    root->addWidget(_panel, 0, Qt::AlignHCenter);
    root->addStretch(1);

    auto *panelLayout = new QVBoxLayout(_panel);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(0);

    // Title bar.
    auto *titleBar = new QWidget(_panel);
    titleBar->setFixedHeight(st::boxTitleHeight);
    panelLayout->addWidget(titleBar);

    const QString titleText = (_mode == Display)
        ? tr("Save your recovery key")
        : tr("Enter recovery key");

    auto *titleLabel = new QLabel(titleText, titleBar);
    titleLabel->setFont(st::boxTitleFont);
    {
        QPalette pal = titleLabel->palette();
        pal.setColor(QPalette::WindowText, st::boxTitleFg);
        titleLabel->setPalette(pal);
    }
    titleLabel->move(st::boxTitlePosition.x(), st::boxTitlePosition.y());

    // Close button.
    auto *close = new ::Ui::CloseButton(titleBar);
    close->move(panelWidth - st::settingsCloseButtonSize, 0);
    connect(close, &::Ui::CloseButton::clicked, this,
            [this] { reject(); });
    _closeButton = close;

    // Separator.
    auto *titleSep = new QWidget(_panel);
    titleSep->setFixedHeight(1);
    titleSep->setAutoFillBackground(true);
    {
        QPalette pal = titleSep->palette();
        pal.setColor(QPalette::Window, st::shadowFg);
        titleSep->setPalette(pal);
    }
    panelLayout->addWidget(titleSep);

    // Content area.
    auto *contentContainer = new QWidget(_panel);
    auto *contentLayout = new QVBoxLayout(contentContainer);
    contentLayout->setContentsMargins(
        st::boxPadding.left(),
        st::boxPadding.top(),
        st::boxPadding.right(),
        4);
    contentLayout->setSpacing(10);

    if (_mode == Display) {
        // Description.
        auto *descLabel = new QLabel(
            tr("Store this recovery key somewhere safe. "
                "You can use it to access your encrypted messages "
                "if you lose access to all your sessions."),
            contentContainer);
        descLabel->setFont(st::baseFont(14));
        descLabel->setWordWrap(true);
        descLabel->setFixedHeight(QFontMetrics(descLabel->font()).lineSpacing() * 3);
        {
            QPalette pal = descLabel->palette();
            pal.setColor(QPalette::WindowText, st::windowFg);
            descLabel->setPalette(pal);
        }
        contentLayout->addWidget(descLabel);

        auto *keyRow = new QWidget(contentContainer);
        auto *keyRowLayout = new QHBoxLayout(keyRow);
        keyRowLayout->setContentsMargins(0, 0, 0, 0);
        keyRowLayout->setSpacing(8);

        auto *keyBox = new BorderedBox(keyRow);
        auto *keyLayout = new QHBoxLayout(keyBox);
        keyLayout->setContentsMargins(12, 8, 8, 8);
        keyLayout->setSpacing(0);

        // Recovery key display — monospace, formatted in 4-char groups.
        _keyDisplay = new QLabel(formatKey(_key), keyBox);
        _keyDisplay->setFont(st::monospaceFont(19));
        _keyDisplay->setWordWrap(true);
        const auto keyLineHeight = QFontMetrics(_keyDisplay->font()).lineSpacing();
        _keyDisplay->setFixedHeight(keyLineHeight * 2);
        keyBox->setFixedHeight(keyLineHeight * 2 + 16);
        _keyDisplay->setTextInteractionFlags(Qt::NoTextInteraction);
        _keyDisplay->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        {
            QPalette pal = _keyDisplay->palette();
            pal.setColor(QPalette::WindowText, st::windowFg);
            _keyDisplay->setPalette(pal);
        }
        keyLayout->addWidget(_keyDisplay, 1);
        keyRowLayout->addWidget(keyBox, 1);

        _copyButton = createCopyIconButton(keyRow);
        connect(_copyButton, &QAbstractButton::clicked, this, [this] {
            QApplication::clipboard()->setText(_key);
            ::Ui::ShowToast(tr("Recovery key copied to clipboard"));
            const auto copiedKey = _key;
            QTimer::singleShot(30000, qApp, [copiedKey] {
                auto *clipboard = QApplication::clipboard();
                if (clipboard && clipboard->text() == copiedKey) {
                    clipboard->clear();
                }
            });
            _copyButton->setToolTip(tr("Copied!"));
            QTimer::singleShot(2000, this, [this] {
                if (_copyButton) {
                    _copyButton->setToolTip(tr("Copy to clipboard"));
                }
            });
        });
        keyRowLayout->addWidget(_copyButton, 0, Qt::AlignTop);
        contentLayout->addWidget(keyRow);

        // "I've saved my recovery key" checkbox.
        _savedCheckbox = new RecoveryCheckBox(
            tr("I've saved my recovery key"), contentContainer);
        connect(_savedCheckbox, &QCheckBox::toggled, this,
                [this] { updateConfirmButton(); });
        contentLayout->addWidget(_savedCheckbox, 0, Qt::AlignLeft);

    } else {
        // Entry mode — description + line edit.
        auto *descLabel = new QLabel(
            tr("Enter your recovery key to restore access "
                "to your encrypted messages."),
            contentContainer);
        descLabel->setFont(st::baseFont(14));
        descLabel->setWordWrap(true);
        {
            QPalette pal = descLabel->palette();
            pal.setColor(QPalette::WindowText, st::windowFg);
            descLabel->setPalette(pal);
        }
        contentLayout->addWidget(descLabel);

        _keyInput = new ::Ui::InputField(
            contentContainer,
            st::defaultInputField,
            rpl::single<QString>(tr("Recovery key")));
        _keyInput->setFont(st::monospaceFont(17));
        connect(_keyInput, &QLineEdit::textChanged, this,
                [this] { updateConfirmButton(); });
        connect(_keyInput, &QLineEdit::returnPressed, this, [this] {
            if (_confirmButton && _confirmButton->isEnabled()) {
                accept();
            }
        });
        contentLayout->addWidget(_keyInput);
    }

    panelLayout->addWidget(contentContainer);
    contentContainer->adjustSize();
    contentContainer->setMinimumHeight(contentContainer->sizeHint().height());

    // Buttons.
    auto *buttonsContainer = new QWidget(_panel);
    buttonsContainer->setFixedHeight(
        st::boxButtonPadding.top()
        + st::boxButtonHeight
        + st::boxButtonPadding.bottom());
    panelLayout->addWidget(buttonsContainer);

    auto *buttonsLayout = new QHBoxLayout(buttonsContainer);
    buttonsLayout->setContentsMargins(
        st::boxButtonPadding.left(),
        st::boxButtonPadding.top(),
        st::boxButtonPadding.right(),
        st::boxButtonPadding.bottom());
    buttonsLayout->setSpacing(8);
    buttonsLayout->addStretch(1);

    auto buttonFont = st::baseFont(14);
    buttonFont.setWeight(QFont::DemiBold);

    ::Ui::TextButton::Style cancelStyle;
    cancelStyle.bgOver = &st::lightButtonBgOver;  // transparent until hovered
    cancelStyle.fg = &st::lightButtonFg;
    cancelStyle.radius = st::boxRadius;
    cancelStyle.height = st::boxButtonHeight;
    _cancel = new ::Ui::TextButton(tr("Cancel"), cancelStyle, buttonsContainer);
    _cancel->setFont(buttonFont);
    connect(_cancel, &QAbstractButton::clicked, this, [this] { reject(); });
    buttonsLayout->addWidget(_cancel);

    const QString confirmLabel = (_mode == Display)
        ? tr("Continue")
        : tr("Submit");

    _confirmButton = new ::Ui::TextButton(
        confirmLabel, enabledConfirmStyle(), buttonsContainer);
    _confirmButton->setFont(buttonFont);
    connect(_confirmButton, &QAbstractButton::clicked, this, [this] { accept(); });
    buttonsLayout->addWidget(_confirmButton);

    updateConfirmButton();

    _panel->adjustSize();
    _panel->setFixedHeight(_panel->sizeHint().height());

    // Focus the input field in Entry mode after event loop starts.
    if (_mode == Entry && _keyInput) {
        QTimer::singleShot(0, _keyInput, [this] {
            _keyInput->setFocus();
        });
    }
}

int RecoveryKeyDialog::exec() {
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

void RecoveryKeyDialog::accept() {
    if (_mode == Entry) {
        if (!_keyInput || _keyInput->text().trimmed().isEmpty()) {
            return;
        }
    }
    _result = Accepted;
    if (_loop) _loop->quit();
}

void RecoveryKeyDialog::reject() {
    _result = Rejected;
    if (_loop) _loop->quit();
}

QString RecoveryKeyDialog::recoveryKey() const {
    if (_mode == Entry && _keyInput) {
        return _keyInput->text().trimmed();
    }
    return _key;
}

void RecoveryKeyDialog::updateConfirmButton() {
    if (!_confirmButton) return;

    const bool enabled = (_mode == Display)
        // Enabled only when the "I've saved" checkbox is checked.
        ? (_savedCheckbox && _savedCheckbox->isChecked())
        // Enabled when the input has text.
        : (_keyInput && !_keyInput->text().trimmed().isEmpty());
    _confirmButton->setEnabled(enabled);
    _confirmButton->setButtonStyle(
        enabled ? enabledConfirmStyle() : disabledConfirmStyle());
}

QString RecoveryKeyDialog::formatKey(const QString &key) {
    const auto compact = QString(key).remove(QRegularExpression(QStringLiteral("\\s+")));
    QString result;
    for (int i = 0; i < compact.size(); i += 4) {
        if (!result.isEmpty()) result += QLatin1Char(' ');
        result += compact.mid(i, 4);
    }
    return result;
}

void RecoveryKeyDialog::paintEvent(QPaintEvent *) {
    QPainter p(this);

    p.setOpacity(_bgOpacity);
    p.fillRect(rect(), st::layerBg);

    if (_panel && _layerOpacity > 0) {
        p.setOpacity(_layerOpacity);
        paintBoxShadow(p, _panel->geometry());
    }
}

void RecoveryKeyDialog::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool RecoveryKeyDialog::eventFilter(QObject *obj, QEvent *event) {
    if (obj == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace TeleMatrix

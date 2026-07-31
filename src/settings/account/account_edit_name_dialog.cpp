// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "account_edit_name_dialog.h"
#include "ui/widgets/emoji_input_field.h"

#include <QEventLoop>
#include <QFont>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPalette>
#include <QPlainTextEdit>
#include <QTextCursor>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include "styles/style_constants.h"
#include "ui/focus_restore.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/close_button.h"
#include "ui/widgets/input_fields.h"

namespace TeleMatrix {

namespace {

constexpr int kShadowExtend = 10;
constexpr int kMaxDisplayNameLength = 64;

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
class RoundedPanel : public QWidget {
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

// Multiline underline field (topics/descriptions), matching the flat single-line
// Ui::InputField look: transparent background, a bottom underline that goes accent
// on focus, no frame. A few lines tall with scrolling for longer text.
class MultilineField : public QPlainTextEdit {
public:
    MultilineField(QWidget *parent, const QString &placeholder)
        : QPlainTextEdit(parent) {
        setFrameShape(QFrame::NoFrame);
        setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setLineWrapMode(QPlainTextEdit::WidgetWidth);
        setPlaceholderText(placeholder);
        setAttribute(Qt::WA_MacShowFocusRect, false);
        setFixedHeight(96);
        setViewportMargins(0, 4, 0, 6);
        auto f = font();
        f.setPixelSize(14);
        setFont(f);
        QPalette pal = palette();
        pal.setColor(QPalette::Base, Qt::transparent);
        pal.setColor(QPalette::Text, st::windowFg);
        pal.setColor(QPalette::PlaceholderText, st::windowSubTextFg);
        pal.setColor(QPalette::Highlight, st::msgInBgSelected);
        pal.setColor(QPalette::HighlightedText, st::windowFgActive);
        setPalette(pal);
        viewport()->setAutoFillBackground(false);
    }

protected:
    void paintEvent(QPaintEvent *e) override {
        QPlainTextEdit::paintEvent(e);
        // A QPlainTextEdit is a QAbstractScrollArea — paint on its viewport, NOT
        // on `this` (that has no paint engine and floods the log with
        // "QWidget::paintEngine: Should no longer be called").
        QPainter p(viewport());
        const auto focused = hasFocus();
        const auto w = focused ? 2 : 1;
        const auto *vp = viewport();
        p.fillRect(
            0, vp->height() - w, vp->width(), w,
            focused ? st::activeLineFg : st::inputBorderFg);
    }
};

::Ui::TextButton::Style enabledSaveStyle() {
    ::Ui::TextButton::Style s;
    s.bgOver = &st::lightButtonBgOver;  // transparent until hovered
    s.fg = &st::lightButtonFg;
    s.radius = st::boxRadius;
    s.height = st::boxButtonHeight;
    return s;
}

::Ui::TextButton::Style disabledSaveStyle() {
    ::Ui::TextButton::Style s;
    s.fg = &st::windowSubTextFg;  // flat, muted, no hover
    s.radius = st::boxRadius;
    s.height = st::boxButtonHeight;
    return s;
}

} // namespace

AccountEditNameDialog::AccountEditNameDialog(
    const QString &currentName,
    QWidget *parent,
    const QString &title,
    const QString &placeholder,
    bool multiline)
    : QWidget(parent ? parent->window() : nullptr)
{
    if (parentWidget()) {
        setGeometry(parentWidget()->rect());
        parentWidget()->installEventFilter(this);
    }

    setFocusPolicy(Qt::StrongFocus);

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

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addStretch(1);

    _panel = new RoundedPanel(this);
    _panel->setVisible(false);
    _panel->setFixedWidth(st::boxWideWidth);
    root->addWidget(_panel, 0, Qt::AlignHCenter);
    root->addStretch(1);

    auto *panelLayout = new QVBoxLayout(_panel);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(0);

    // Title bar.
    auto *titleBar = new QWidget(_panel);
    titleBar->setFixedHeight(st::boxTitleHeight);
    panelLayout->addWidget(titleBar);

    auto *titleText = new QLabel(
        title.isEmpty() ? tr("Edit Display Name") : title, titleBar);
    titleText->setFont(st::boxTitleFont);
    {
        QPalette pal = titleText->palette();
        pal.setColor(QPalette::WindowText, st::boxTitleFg);
        titleText->setPalette(pal);
    }
    titleText->move(st::boxTitlePosition.x(), st::boxTitlePosition.y());

    // Close button.
    auto *close = new ::Ui::CloseButton(titleBar);
    close->move(st::boxWideWidth - st::settingsCloseButtonSize, 0);
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

    // Input field.
    auto *inputContainer = new QWidget(_panel);
    auto *inputLayout = new QVBoxLayout(inputContainer);
    inputLayout->setContentsMargins(
        st::boxPadding.left(),
        st::boxPadding.top(),
        st::boxPadding.right(),
        st::boxPadding.bottom());
    inputLayout->setSpacing(0);

    // tdesktop-style flat underline field. Single field → no floating caption
    // (the placeholder just sits inside until you type). A multiline dialog
    // (e.g. a room topic) uses a taller wrapping field instead.
    const auto ph = placeholder.isEmpty() ? tr("Display name") : placeholder;
    if (multiline) {
        _multilineField = new MultilineField(inputContainer, ph);
        _multilineField->setPlainText(currentName);
        inputLayout->addWidget(_multilineField);
    } else {
        _nameField = new ::Ui::EmojiInputField(
            inputContainer, st::defaultInputField, ph);
        _nameField->setMaxLength(kMaxDisplayNameLength);
        _nameField->setText(currentName);
        inputLayout->addWidget(_nameField);
    }

    panelLayout->addWidget(inputContainer);

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

    _save = new ::Ui::TextButton(tr("Save"), enabledSaveStyle(), buttonsContainer);
    _save->setFont(buttonFont);
    connect(_save, &QAbstractButton::clicked, this, [this] { accept(); });
    buttonsLayout->addWidget(_save);

    if (_multilineField) {
        // Enter inserts a newline in a multiline topic; Save submits.
        connect(_multilineField, &QPlainTextEdit::textChanged, this,
                [this] { updateSaveButton(); });
    } else {
        connect(_nameField, &QTextEdit::textChanged, this,
                [this] { updateSaveButton(); });
        connect(_nameField, &::Ui::EmojiInputField::submitted, this, [this] {
            if (_save->isEnabled()) {
                accept();
            }
        });
    }

    updateSaveButton();

    if (_multilineField) {
        QTimer::singleShot(0, _multilineField, [this] {
            _multilineField->setFocus();
            _multilineField->moveCursor(QTextCursor::End);
        });
    } else {
        QTimer::singleShot(0, _nameField, [this] {
            _nameField->setFocus();
            _nameField->selectAll();
        });
    }
}

int AccountEditNameDialog::exec() {
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

void AccountEditNameDialog::accept() {
    _result = Accepted;
    if (_loop) _loop->quit();
}

void AccountEditNameDialog::reject() {
    _result = Rejected;
    if (_loop) _loop->quit();
}

QString AccountEditNameDialog::displayName() const {
    if (_multilineField) {
        return _multilineField->toPlainText().trimmed();
    }
    return _nameField ? _nameField->text().trimmed() : QString();
}

void AccountEditNameDialog::updateSaveButton() {
    const bool hasText = !displayName().isEmpty();
    if (_save) {
        _save->setEnabled(hasText);
        _save->setButtonStyle(hasText ? enabledSaveStyle() : disabledSaveStyle());
    }
}

void AccountEditNameDialog::paintEvent(QPaintEvent *) {
    QPainter p(this);

    p.setOpacity(_bgOpacity);
    p.fillRect(rect(), st::layerBg);

    if (_panel && _layerOpacity > 0) {
        p.setOpacity(_layerOpacity);
        paintBoxShadow(p, _panel->geometry());
    }
}

void AccountEditNameDialog::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool AccountEditNameDialog::eventFilter(QObject *obj, QEvent *event) {
    if (obj == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace TeleMatrix

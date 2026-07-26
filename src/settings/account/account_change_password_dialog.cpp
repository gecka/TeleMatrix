// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "account_change_password_dialog.h"

#include <QEventLoop>
#include <QFont>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPalette>
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

AccountChangePasswordDialog::AccountChangePasswordDialog(QWidget *parent)
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

    auto *titleText = new QLabel(tr("Change Password"), titleBar);
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

    // Input fields.
    auto *inputContainer = new QWidget(_panel);
    auto *inputLayout = new QVBoxLayout(inputContainer);
    inputLayout->setContentsMargins(
        st::boxPadding.left(),
        0,
        st::boxPadding.right(),
        4);
    inputLayout->setSpacing(10);

    _errorLabel = new QLabel(inputContainer);
    _errorLabel->setFont(st::baseFont(12));
    {
        QPalette pal = _errorLabel->palette();
        pal.setColor(QPalette::WindowText, st::attentionButtonFg);
        _errorLabel->setPalette(pal);
    }
    _errorLabel->setFixedHeight(st::boxLabelLineHeight);
    _errorLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    _errorLabel->clear();
    inputLayout->addWidget(_errorLabel);

    auto *currentPasswordField = new ::Ui::InputField(
        inputContainer,
        st::defaultInputField,
        rpl::single<QString>(tr("Current password")));
    currentPasswordField->setFloatingPlaceholder(true);
    currentPasswordField->setEchoMode(QLineEdit::Password);
    _currentPasswordField = currentPasswordField;
    inputLayout->addWidget(_currentPasswordField);

    auto *newPasswordField = new ::Ui::InputField(
        inputContainer,
        st::defaultInputField,
        rpl::single<QString>(tr("New password")));
    newPasswordField->setFloatingPlaceholder(true);
    newPasswordField->setEchoMode(QLineEdit::Password);
    _newPasswordField = newPasswordField;
    inputLayout->addWidget(_newPasswordField);

    auto *confirmField = new ::Ui::InputField(
        inputContainer,
        st::defaultInputField,
        rpl::single<QString>(tr("Confirm new password")));
    confirmField->setFloatingPlaceholder(true);
    confirmField->setEchoMode(QLineEdit::Password);
    _confirmField = confirmField;
    inputLayout->addWidget(_confirmField);

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

    _save = new ::Ui::TextButton(
        tr("Change Password"), enabledSaveStyle(), buttonsContainer);
    _save->setFont(buttonFont);
    connect(_save, &QAbstractButton::clicked, this, [this] { accept(); });
    buttonsLayout->addWidget(_save);

    connect(_currentPasswordField, &QLineEdit::textChanged, this,
            [this] { updateSaveButton(); });
    connect(_newPasswordField, &QLineEdit::textChanged, this,
            [this] { updateSaveButton(); });
    connect(_confirmField, &QLineEdit::textChanged, this,
            [this] { updateSaveButton(); });
    connect(_confirmField, &QLineEdit::returnPressed, this, [this] {
        if (_save->isEnabled()) {
            accept();
        }
    });

    updateSaveButton();

    QTimer::singleShot(0, _currentPasswordField, [this] {
        _currentPasswordField->setFocus();
    });
}

int AccountChangePasswordDialog::exec() {
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

void AccountChangePasswordDialog::accept() {
    if (_currentPasswordField->text().isEmpty()) {
        _errorLabel->setText(tr("Please enter your current password."));
        _errorLabel->show();
        return;
    }
    if (_newPasswordField->text().isEmpty()) {
        _errorLabel->setText(tr("New password cannot be empty."));
        _errorLabel->show();
        return;
    }
    if (_newPasswordField->text() != _confirmField->text()) {
        _errorLabel->setText(tr("Passwords do not match."));
        _errorLabel->show();
        return;
    }
    _result = Accepted;
    if (_loop) _loop->quit();
}

void AccountChangePasswordDialog::reject() {
    _result = Rejected;
    if (_loop) _loop->quit();
}

QString AccountChangePasswordDialog::currentPassword() const {
    return _currentPasswordField ? _currentPasswordField->text() : QString();
}

QString AccountChangePasswordDialog::newPassword() const {
    return _newPasswordField ? _newPasswordField->text() : QString();
}

void AccountChangePasswordDialog::updateSaveButton() {
    const bool valid = _currentPasswordField
        && !_currentPasswordField->text().isEmpty()
        && _newPasswordField
        && !_newPasswordField->text().isEmpty()
        && _confirmField
        && !_confirmField->text().isEmpty();
    if (_save) {
        _save->setEnabled(valid);
        _save->setButtonStyle(valid ? enabledSaveStyle() : disabledSaveStyle());
    }
    // Keep the reserved error row in place while clearing stale text.
    if (_errorLabel && !_errorLabel->text().isEmpty()) {
        _errorLabel->clear();
    }
}

void AccountChangePasswordDialog::paintEvent(QPaintEvent *) {
    QPainter p(this);

    p.setOpacity(_bgOpacity);
    p.fillRect(rect(), st::layerBg);

    if (_panel && _layerOpacity > 0) {
        p.setOpacity(_layerOpacity);
        paintBoxShadow(p, _panel->geometry());
    }
}

void AccountChangePasswordDialog::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool AccountChangePasswordDialog::eventFilter(QObject *obj, QEvent *event) {
    if (obj == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace TeleMatrix

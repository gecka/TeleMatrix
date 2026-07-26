// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "interactive_auth_dialog.h"

#include <QEventLoop>
#include <QFont>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
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

InteractiveAuthDialog::InteractiveAuthDialog(
    const QString &userId,
    const QString &challengeJson,
    QWidget *parent,
    const QString &title,
    const QString &description,
    const QString &confirmText)
    : QWidget(parent ? parent->window() : nullptr)
    , _userId(userId)
{
    // Parse the UIA session from the challenge JSON.
    const auto doc = QJsonDocument::fromJson(challengeJson.toUtf8());
    if (doc.isObject()) {
        _session = doc.object().value(QStringLiteral("session")).toString();
    }

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
        title.isEmpty() ? tr("Confirm your identity") : title,
        titleBar);
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

    // Input area.
    auto *inputContainer = new QWidget(_panel);
    auto *inputLayout = new QVBoxLayout(inputContainer);
    inputLayout->setContentsMargins(
        st::boxPadding.left(),
        st::boxPadding.top(),
        st::boxPadding.right(),
        4);
    inputLayout->setSpacing(10);

    // Description label.
    auto *descLabel = new QLabel(
        description.isEmpty() ? tr("Enter your password to continue") : description,
        inputContainer);
    descLabel->setFont(st::baseFont(14));
    descLabel->setWordWrap(true);
    {
        QPalette pal = descLabel->palette();
        pal.setColor(QPalette::WindowText, st::windowFg);
        descLabel->setPalette(pal);
    }
    inputLayout->addWidget(descLabel);

    _passwordField = new ::Ui::InputField(
        inputContainer,
        st::defaultInputField,
        rpl::single<QString>(tr("Password")));
    _passwordField->setEchoMode(QLineEdit::Password);
    inputLayout->addWidget(_passwordField);

    _errorLabel = new QLabel(inputContainer);
    _errorLabel->setFont(st::baseFont(12));
    {
        QPalette pal = _errorLabel->palette();
        pal.setColor(QPalette::WindowText, st::attentionButtonFg);
        _errorLabel->setPalette(pal);
    }
    _errorLabel->setWordWrap(true);
    _errorLabel->hide();
    inputLayout->addWidget(_errorLabel);

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

    _confirm = new ::Ui::TextButton(
        confirmText.isEmpty() ? tr("Confirm") : confirmText,
        enabledConfirmStyle(),
        buttonsContainer);
    _confirm->setFont(buttonFont);
    connect(_confirm, &QAbstractButton::clicked, this, [this] { accept(); });
    buttonsLayout->addWidget(_confirm);

    connect(_passwordField, &QLineEdit::textChanged, this,
            [this] { updateConfirmButton(); });
    connect(_passwordField, &QLineEdit::returnPressed, this, [this] {
        if (_confirm->isEnabled()) {
            accept();
        }
    });

    updateConfirmButton();

    _panel->adjustSize();
    _panel->setFixedHeight(_panel->sizeHint().height());

    QTimer::singleShot(0, _passwordField, [this] {
        _passwordField->setFocus();
    });
}

int InteractiveAuthDialog::exec() {
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

void InteractiveAuthDialog::accept() {
    if (!_passwordField || _passwordField->text().isEmpty()) {
        _errorLabel->setText(tr("Password cannot be empty."));
        _errorLabel->show();
        return;
    }
    _result = Accepted;
    if (_loop) _loop->quit();
}

void InteractiveAuthDialog::reject() {
    _result = Rejected;
    if (_loop) _loop->quit();
}

QString InteractiveAuthDialog::authJson() const {
    QJsonObject identifier;
    identifier[QStringLiteral("type")] = QStringLiteral("m.id.user");
    identifier[QStringLiteral("user")] = _userId;

    QJsonObject auth;
    auth[QStringLiteral("type")] = QStringLiteral("m.login.password");
    if (!_session.isEmpty()) {
        auth[QStringLiteral("session")] = _session;
    }
    auth[QStringLiteral("identifier")] = identifier;
    auth[QStringLiteral("password")] = _passwordField ? _passwordField->text() : QString();

    return QString::fromUtf8(QJsonDocument(auth).toJson(QJsonDocument::Compact));
}

void InteractiveAuthDialog::updateConfirmButton() {
    const bool valid = _passwordField && !_passwordField->text().isEmpty();
    if (_confirm) {
        _confirm->setEnabled(valid);
        _confirm->setButtonStyle(valid ? enabledConfirmStyle() : disabledConfirmStyle());
    }
    // Hide error when user modifies input.
    if (_errorLabel && _errorLabel->isVisible()) {
        _errorLabel->hide();
    }
}

void InteractiveAuthDialog::paintEvent(QPaintEvent *) {
    QPainter p(this);

    p.setOpacity(_bgOpacity);
    p.fillRect(rect(), st::layerBg);

    if (_panel && _layerOpacity > 0) {
        p.setOpacity(_layerOpacity);
        paintBoxShadow(p, _panel->geometry());
    }
}

void InteractiveAuthDialog::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool InteractiveAuthDialog::eventFilter(QObject *obj, QEvent *event) {
    if (obj == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace TeleMatrix

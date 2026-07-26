// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "settings_passphrase_dialog.h"

#include <QEventLoop>
#include <QFont>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
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

} // namespace

SettingsPassphraseDialog::SettingsPassphraseDialog(
    const QString &title,
    const QString &description,
    QWidget *parent,
    bool requireConfirm)
    : QWidget(parent ? parent->window() : nullptr)
    , _requireConfirm(requireConfirm) {
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

    auto *titleBar = new QWidget(_panel);
    titleBar->setFixedHeight(st::boxTitleHeight);
    panelLayout->addWidget(titleBar);

    auto *titleLabel = new QLabel(title, titleBar);
    titleLabel->setFont(st::boxTitleFont);
    {
        QPalette pal = titleLabel->palette();
        pal.setColor(QPalette::WindowText, st::boxTitleFg);
        titleLabel->setPalette(pal);
    }
    titleLabel->move(st::boxTitlePosition.x(), st::boxTitlePosition.y());

    // Close button.
    auto *close = new ::Ui::CloseButton(titleBar);
    close->move(st::boxWideWidth - st::settingsCloseButtonSize, 0);
    connect(close, &::Ui::CloseButton::clicked, this,
            [this] { reject(); });

    auto *titleSep = new QWidget(_panel);
    titleSep->setFixedHeight(1);
    titleSep->setAutoFillBackground(true);
    {
        QPalette pal = titleSep->palette();
        pal.setColor(QPalette::Window, st::shadowFg);
        titleSep->setPalette(pal);
    }
    panelLayout->addWidget(titleSep);

    auto *content = new QWidget(_panel);
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(
        st::boxPadding.left(),
        st::boxPadding.top(),
        st::boxPadding.right(),
        4);
    contentLayout->setSpacing(10);

    if (!description.isEmpty()) {
        auto *label = new QLabel(description, content);
        label->setFont(st::baseFont(14));
        label->setWordWrap(true);
        {
            QPalette pal = label->palette();
            pal.setColor(QPalette::WindowText, st::windowFg);
            label->setPalette(pal);
        }
        contentLayout->addWidget(label);
    }

    // Floating caption only when there are two fields to disambiguate; a lone
    // passphrase field keeps its placeholder inside (floating off).
    auto *passField = new ::Ui::InputField(
        content,
        st::defaultInputField,
        rpl::single<QString>(tr("Passphrase")));
    passField->setFloatingPlaceholder(_requireConfirm);
    passField->setEchoMode(QLineEdit::Password);
    _field = passField;
    contentLayout->addWidget(_field);

    if (_requireConfirm) {
        auto *confirmField = new ::Ui::InputField(
            content,
            st::defaultInputField,
            rpl::single<QString>(tr("Confirm passphrase")));
        confirmField->setFloatingPlaceholder(true);
        confirmField->setEchoMode(QLineEdit::Password);
        _confirm = confirmField;
        contentLayout->addWidget(_confirm);
    }

    _error = new QLabel(content);
    _error->setFont(st::baseFont(12));
    _error->setWordWrap(true);
    _error->setVisible(false);
    {
        QPalette pal = _error->palette();
        pal.setColor(QPalette::WindowText, st::boxTextFgError);
        _error->setPalette(pal);
    }
    contentLayout->addWidget(_error);

    panelLayout->addWidget(content);

    auto *buttonsContainer = new QWidget(_panel);
    buttonsContainer->setFixedHeight(
        st::boxButtonPadding.top()
        + st::boxButtonHeight
        + st::boxButtonPadding.bottom());
    panelLayout->addWidget(buttonsContainer);

    auto *buttons = new QHBoxLayout(buttonsContainer);
    buttons->setContentsMargins(
        st::boxButtonPadding.left(),
        st::boxButtonPadding.top(),
        st::boxButtonPadding.right(),
        st::boxButtonPadding.bottom());
    buttons->setSpacing(8);
    buttons->addStretch(1);

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
    buttons->addWidget(_cancel);

    ::Ui::TextButton::Style okStyle;
    okStyle.bgOver = &st::lightButtonBgOver;  // transparent until hovered
    okStyle.fg = &st::lightButtonFg;
    okStyle.radius = st::boxRadius;
    okStyle.height = st::boxButtonHeight;
    _ok = new ::Ui::TextButton(tr("OK"), okStyle, buttonsContainer);
    _ok->setFont(buttonFont);
    connect(_ok, &QAbstractButton::clicked, this, [this] { accept(); });
    buttons->addWidget(_ok);

    if (_confirm) {
        connect(_field, &QLineEdit::returnPressed, this,
                [this] { _confirm->setFocus(); });
        connect(_confirm, &QLineEdit::returnPressed, this, [this] { accept(); });
    } else {
        connect(_field, &QLineEdit::returnPressed, this, [this] { accept(); });
    }

    // OK stays disabled while the password field is empty (there's nothing to
    // submit); TextButton dims itself at 0.5 opacity when disabled.
    const auto updateOkEnabled = [this] {
        _ok->setEnabled(!_field->text().isEmpty());
    };
    connect(_field, &QLineEdit::textChanged, this, updateOkEnabled);
    updateOkEnabled();

    _panel->adjustSize();
    _panel->setFixedHeight(_panel->sizeHint().height());
}

int SettingsPassphraseDialog::exec() {
    const auto restoreFocus = ::TeleMatrix::Focus::saveFocusForPopup();

    raise();
    show();
    setFocus();

    if (_a_shown) _a_shown->start();
    if (_a_layerShown) _a_layerShown->start();

    QTimer::singleShot(0, _field, [this] { _field->setFocus(); });

    QEventLoop loop;
    _loop = &loop;
    loop.exec();
    _loop = nullptr;

    hide();
    ::TeleMatrix::Focus::restoreFocusAfterPopup(restoreFocus);
    return _result;
}

QString SettingsPassphraseDialog::passphrase() const {
    return _field ? _field->text() : QString();
}

void SettingsPassphraseDialog::accept() {
    if (_requireConfirm) {
        const auto pass = _field ? _field->text() : QString();
        const auto confirm = _confirm ? _confirm->text() : QString();
        if (pass.isEmpty()) {
            if (_error) {
                _error->setText(tr("Enter a master password."));
                _error->setVisible(true);
            }
            return;
        }
        if (pass != confirm) {
            if (_error) {
                _error->setText(tr("The passwords don't match."));
                _error->setVisible(true);
            }
            if (_confirm) {
                _confirm->clear();
                _confirm->setFocus();
            }
            return;
        }
    }
    _result = Accepted;
    if (_loop) _loop->quit();
}

void SettingsPassphraseDialog::reject() {
    _result = Rejected;
    if (_loop) _loop->quit();
}

void SettingsPassphraseDialog::paintEvent(QPaintEvent *) {
    QPainter p(this);

    p.setOpacity(_bgOpacity);
    p.fillRect(rect(), st::layerBg);

    if (_panel && _layerOpacity > 0) {
        p.setOpacity(_layerOpacity);
        paintBoxShadow(p, _panel->geometry());
    }
}

void SettingsPassphraseDialog::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool SettingsPassphraseDialog::eventFilter(QObject *obj, QEvent *event) {
    if (obj == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace TeleMatrix

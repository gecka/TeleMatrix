// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "settings/sessions/session_rename_dialog.h"

#include "styles/style_constants.h"
#include "ui/focus_restore.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/close_button.h"
#include "ui/widgets/input_fields.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFont>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPalette>
#include <QTimer>
#include <QVBoxLayout>

namespace TeleMatrix {
namespace {

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

SessionRenameDialog::SessionRenameDialog(
        const QString &currentName,
        QWidget *parent)
    : QWidget(parent ? parent->window() : nullptr) {
    if (parentWidget()) {
        setGeometry(parentWidget()->rect());
        parentWidget()->installEventFilter(this);
    }
    setFocusPolicy(Qt::StrongFocus);

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

    auto *titleBar = new QWidget(_panel);
    titleBar->setFixedHeight(st::boxTitleHeight);
    panelLayout->addWidget(titleBar);

    auto *titleLabel = new QLabel(
        QCoreApplication::translate("SettingsWidget", "Rename Session"),
        titleBar);
    titleLabel->setFont(st::boxTitleFont);
    {
        QPalette pal = titleLabel->palette();
        pal.setColor(QPalette::WindowText, st::boxTitleFg);
        titleLabel->setPalette(pal);
    }
    titleLabel->move(st::boxTitlePosition.x(), st::boxTitlePosition.y());

    auto *close = new ::Ui::CloseButton(titleBar);
    close->move(st::boxWideWidth - st::settingsCloseButtonSize, 0);
    connect(close, &::Ui::CloseButton::clicked, this, [this] {
        reject();
    });

    auto *separator = new QWidget(_panel);
    separator->setFixedHeight(1);
    separator->setAutoFillBackground(true);
    {
        QPalette pal = separator->palette();
        pal.setColor(QPalette::Window, st::shadowFg);
        separator->setPalette(pal);
    }
    panelLayout->addWidget(separator);

    auto *inputContainer = new QWidget(_panel);
    auto *inputLayout = new QVBoxLayout(inputContainer);
    inputLayout->setContentsMargins(
        st::boxPadding.left(),
        st::boxPadding.top(),
        st::boxPadding.right(),
        4);
    inputLayout->setSpacing(10);

    auto *description = new QLabel(
        QCoreApplication::translate("SettingsWidget", "Enter a new name for this session:"),
        inputContainer);
    description->setWordWrap(true);
    description->setFont(st::baseFont(14));
    {
        QPalette pal = description->palette();
        pal.setColor(QPalette::WindowText, st::windowFg);
        description->setPalette(pal);
    }
    inputLayout->addWidget(description);

    _field = new ::Ui::InputField(
        inputContainer,
        st::defaultInputField,
        rpl::single<QString>(QString()));
    _field->setText(currentName);
    _field->selectAll();
    inputLayout->addWidget(_field);

    _error = new QLabel(inputContainer);
    _error->setFont(st::baseFont(12));
    {
        QPalette pal = _error->palette();
        pal.setColor(QPalette::WindowText, st::attentionButtonFg);
        _error->setPalette(pal);
    }
    _error->hide();
    inputLayout->addWidget(_error);
    panelLayout->addWidget(inputContainer);

    auto *buttonsContainer = new QWidget(_panel);
    buttonsContainer->setFixedHeight(
        st::boxButtonPadding.top()
        + st::boxButtonHeight
        + st::boxButtonPadding.bottom());
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
    auto *cancel = new ::Ui::TextButton(
        QCoreApplication::translate("SettingsWidget", "Cancel"),
        cancelStyle,
        buttonsContainer);
    cancel->setFont(buttonFont);
    connect(cancel, &QAbstractButton::clicked, this, [this] { reject(); });
    buttons->addWidget(cancel);

    ::Ui::TextButton::Style saveStyle;
    saveStyle.bgOver = &st::lightButtonBgOver;  // transparent until hovered
    saveStyle.fg = &st::lightButtonFg;
    saveStyle.radius = st::boxRadius;
    saveStyle.height = st::boxButtonHeight;
    auto *save = new ::Ui::TextButton(
        QCoreApplication::translate("SettingsWidget", "Save"),
        saveStyle,
        buttonsContainer);
    save->setFont(buttonFont);
    connect(save, &QAbstractButton::clicked, this, [this] { accept(); });
    buttons->addWidget(save);
    panelLayout->addWidget(buttonsContainer);

    connect(_field, &QLineEdit::returnPressed, this, [this] { accept(); });

    _panel->adjustSize();
    _panel->setFixedHeight(_panel->sizeHint().height());
}

int SessionRenameDialog::exec() {
    const auto restoreFocus = ::TeleMatrix::Focus::saveFocusForPopup();

    raise();
    show();
    setFocus();
    QTimer::singleShot(0, _field, [this] { _field->setFocus(); });
    QEventLoop loop;
    _loop = &loop;
    loop.exec();
    _loop = nullptr;
    hide();
    ::TeleMatrix::Focus::restoreFocusAfterPopup(restoreFocus);
    return _result;
}

QString SessionRenameDialog::text() const {
    return _field ? _field->text().trimmed() : QString();
}

void SessionRenameDialog::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), st::layerBg);
    if (_panel) {
        paintSettingsBoxShadow(p, _panel->geometry());
    }
}

void SessionRenameDialog::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool SessionRenameDialog::eventFilter(QObject *obj, QEvent *event) {
    if (obj == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(obj, event);
}

void SessionRenameDialog::accept() {
    if (text().isEmpty()) {
        _error->setText(QCoreApplication::translate(
            "SettingsWidget",
            "Session name cannot be empty."));
        _error->show();
        return;
    }
    _result = Accepted;
    if (_loop) {
        _loop->quit();
    }
}

void SessionRenameDialog::reject() {
    _result = Rejected;
    if (_loop) {
        _loop->quit();
    }
}

} // namespace TeleMatrix

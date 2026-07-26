// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "reset_identity_dialog.h"

#include <QEventLoop>
#include <QFont>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include "styles/style_constants.h"
#include "ui/focus_restore.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/close_button.h"

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

ResetIdentityDialog::ResetIdentityDialog(QWidget *parent)
    : QWidget(parent ? parent->window() : nullptr)
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

    auto *titleLabel = new QLabel(
        tr("Reset cryptographic identity"), titleBar);
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

    // Warning content.
    auto *contentContainer = new QWidget(_panel);
    auto *contentLayout = new QVBoxLayout(contentContainer);
    contentLayout->setContentsMargins(
        st::boxPadding.left(),
        st::boxPadding.top(),
        st::boxPadding.right(),
        4);
    contentLayout->setSpacing(10);

    auto *warningLabel = new QLabel(
        tr("This will create a new encryption identity. "
            "You will lose access to old encrypted messages unless "
            "you have a backup. This action cannot be undone."),
        contentContainer);
    warningLabel->setFont(st::baseFont(14));
    warningLabel->setWordWrap(true);
    {
        QPalette pal = warningLabel->palette();
        pal.setColor(QPalette::WindowText, st::windowFg);
        warningLabel->setPalette(pal);
    }
    contentLayout->addWidget(warningLabel);

    panelLayout->addWidget(contentContainer);

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

    // Destructive "Reset" button with attention/red styling.
    ::Ui::TextButton::Style resetStyle;
    resetStyle.bg = &st::attentionButtonFg;
    resetStyle.bgOver = &st::attentionButtonBgOver;
    resetStyle.fg = &st::activeButtonFg;
    resetStyle.radius = st::boxRadius;
    resetStyle.height = st::boxButtonHeight;
    _resetButton = new ::Ui::TextButton(tr("Reset"), resetStyle, buttonsContainer);
    _resetButton->setFont(buttonFont);
    connect(_resetButton, &QAbstractButton::clicked, this, [this] { accept(); });
    buttonsLayout->addWidget(_resetButton);

    _panel->adjustSize();
    _panel->setFixedHeight(_panel->sizeHint().height());
}

int ResetIdentityDialog::exec() {
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

void ResetIdentityDialog::accept() {
    _result = Accepted;
    if (_loop) _loop->quit();
}

void ResetIdentityDialog::reject() {
    _result = Rejected;
    if (_loop) _loop->quit();
}

void ResetIdentityDialog::paintEvent(QPaintEvent *) {
    QPainter p(this);

    p.setOpacity(_bgOpacity);
    p.fillRect(rect(), st::layerBg);

    if (_panel && _layerOpacity > 0) {
        p.setOpacity(_layerOpacity);
        paintBoxShadow(p, _panel->geometry());
    }
}

void ResetIdentityDialog::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool ResetIdentityDialog::eventFilter(QObject *obj, QEvent *event) {
    if (obj == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace TeleMatrix

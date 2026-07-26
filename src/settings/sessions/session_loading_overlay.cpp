// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "settings/sessions/session_loading_overlay.h"

#include "styles/style_constants.h"
#include "ui/painter.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPalette>
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

SessionLoadingOverlay::SessionLoadingOverlay(
        const QString &title,
        const QString &text,
        QWidget *parent)
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

    auto *label = new QLabel(text, body);
    label->setWordWrap(true);
    label->setFont(st::baseFont(14));
    label->setAlignment(Qt::AlignCenter);
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

void SessionLoadingOverlay::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), st::layerBg);
    if (_panel) {
        paintSettingsBoxShadow(p, _panel->geometry());
    }
}

bool SessionLoadingOverlay::eventFilter(QObject *obj, QEvent *event) {
    if (obj == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace TeleMatrix

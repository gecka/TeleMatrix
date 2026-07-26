// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "dialogs_add_account_box.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include "intro/intro_colors.h"
#include "intro/intro_widget.h"
#include "styles/style_constants.h"
#include "ui/painter.h"
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

// The card body: an anti-aliased rounded rect, like the other box dialogs'
// RoundedPanel. Painted in intro::bg, NOT st::boxBg — the intro flow is always
// light regardless of the app theme (see intro_colors.h), so a theme-aware
// (dark) frame would vanish into the dark dimmed backdrop in night theme and the
// always-light intro would show square corners. The hosted IntroWidget is inset
// by boxRadius (see the layout below) so it never paints over the rounded
// corners — that inset, not a jagged 1-bit mask, is what keeps them smooth; the
// inset border is seamless because it is the same intro::bg the intro paints.
class RoundedHostPanel final : public QWidget {
public:
    explicit RoundedHostPanel(QWidget *parent) : QWidget(parent) {}

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(intro::bg);
        p.drawRoundedRect(rect(), st::boxRadius, st::boxRadius);
    }
};

} // namespace

DialogsAddAccountBox::DialogsAddAccountBox(IntroWidget *intro, QWidget *parent)
    : QWidget(parent ? parent->window() : nullptr)
    , _intro(intro) {
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

    _panel = new RoundedHostPanel(this);
    _panel->setVisible(false);
    root->addWidget(_panel, 0, Qt::AlignHCenter);
    root->addStretch(1);

    auto *panelLayout = new QVBoxLayout(_panel);
    // Inset by the corner radius so the intro's opaque full-bleed background
    // never covers the panel's rounded corners; the border is the same intro::bg
    // the panel paints, so it reads as one seamless rounded card.
    const auto r = st::boxRadius;
    panelLayout->setContentsMargins(r, r, r, r);
    panelLayout->setSpacing(0);
    _intro->setParent(_panel);
    panelLayout->addWidget(_intro);

    // Top-right × (cancel). A manual child, not a layout item, raised above the
    // intro; positioned in updatePanelSize with the panel.
    auto *close = new ::Ui::CloseButton(_panel);
    connect(close, &::Ui::CloseButton::clicked, this,
            [this] { reject(); });
    _close = close;
    _close->raise();

    updatePanelSize();
}

void DialogsAddAccountBox::updatePanelSize() {
    if (!_panel) {
        return;
    }
    // Clamp to the window so the card never exceeds it (the intro does not
    // scroll, so on a short window the tallest step can still clip at the bottom
    // — a documented limitation of the in-window card vs the old separate window).
    const auto margin = Style::ConvertScale(16);
    const auto avail = parentWidget() ? parentWidget()->size() : size();
    const int w = qMin(st::addAccountBoxWidth, avail.width() - 2 * margin);
    const int h = qMin(st::addAccountBoxHeight, avail.height() - 2 * margin);
    _panel->setFixedSize(qMax(w, 0), qMax(h, 0));

    if (_close) {
        // Flush to the top-right rounded corner; the × glyph is centered in the
        // hit area, well clear of the corner arc.
        _close->move(_panel->width() - _close->width(), 0);
        _close->raise();
    }
}

void DialogsAddAccountBox::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    if (_shown) {
        return;
    }
    _shown = true;
    raise();
    // Do NOT steal focus — the hosted intro's input field needs it (the caller
    // calls intro->setInnerFocus() right after show()).
    if (_a_shown) _a_shown->start();
    if (_a_layerShown) _a_layerShown->start();
}

void DialogsAddAccountBox::accept() {
    closeWith(Accepted);
}

void DialogsAddAccountBox::reject() {
    closeWith(Rejected);
}

void DialogsAddAccountBox::closeWith(int result) {
    if (_closing) {
        return;
    }
    _closing = true;
    _result = result;
    hide();
    Q_EMIT finished(result);
    // Deferred delete (mirrors QDialog's WA_DeleteOnClose): accept() is often
    // reached from inside the hosted intro's own signal emission, so the intro —
    // our child — must outlive the current call stack.
    deleteLater();
}

void DialogsAddAccountBox::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setOpacity(_bgOpacity);
    p.fillRect(rect(), st::layerBg);
    if (_panel && _layerOpacity > 0) {
        p.setOpacity(_layerOpacity);
        paintBoxShadow(p, _panel->geometry());
    }
}

void DialogsAddAccountBox::mousePressEvent(QMouseEvent *event) {
    // Clicks inside the card reach the intro (child widgets); only the dimmed
    // area outside the panel lands here — treat it as cancel.
    if (_panel && !_panel->geometry().contains(event->pos())) {
        reject();
        return;
    }
    QWidget::mousePressEvent(event);
}

void DialogsAddAccountBox::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool DialogsAddAccountBox::eventFilter(QObject *obj, QEvent *event) {
    if (obj == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
        updatePanelSize();
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace TeleMatrix

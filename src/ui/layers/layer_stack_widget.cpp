// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "ui/layers/layer_stack_widget.h"

#include "styles/style_constants.h"
#include "ui/focus_restore.h"
#include "ui/painter.h"

#include <QGraphicsEffect>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

namespace TeleMatrix {

namespace {

// Shadow uses an 8px round radius with a 10px extend on all four sides.
// Same approach as HistoryConfirmDialog / HistoryForwardDialog / DialogsEditFolderBox.
constexpr int kShadowExtend = 10;

/// QGraphicsEffect that clips the widget (including all children) to a
/// rounded rect. This is the only reliable way to clip child widget
/// painting to rounded corners in Qt — QPainter::setClipPath only clips
/// the parent's own paint, and QWidget::setMask produces jagged edges.
class RoundedClipEffect : public QGraphicsEffect {
public:
    RoundedClipEffect(int radius, QObject *parent = nullptr)
        : QGraphicsEffect(parent), _radius(radius) {}

protected:
    void draw(QPainter *painter) override {
        // Use the source bounding rect (in logical coordinates) for the
        // clip, not the pixmap dimensions which can have DPR rounding errors.
        const auto br = sourceBoundingRect(Qt::LogicalCoordinates);

        QPoint offset;
        const QPixmap src = sourcePixmap(Qt::LogicalCoordinates, &offset);
        if (src.isNull()) return;

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        QPainterPath clip;
        clip.addRoundedRect(
            QRectF(offset.x(), offset.y(), br.width(), br.height()),
            _radius, _radius);
        painter->setClipPath(clip);
        painter->drawPixmap(offset, src);
        painter->restore();
    }

private:
    int _radius;
};

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

} // namespace

LayerStackWidget::LayerStackWidget(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    hide();
}

void LayerStackWidget::showLayer(QWidget *layer) {
    if (_layer) {
        _layer->hide();
    } else if (layer) {
        _restoreFocus = Focus::saveFocusForPopup();
    }
    _layer = layer;
    if (_layer) {
        _layer->setParent(this);

        // Apply rounded clip effect to the layer widget so ALL child
        // widget painting is clipped to the rounded shape.
        _layer->setGraphicsEffect(
            new RoundedClipEffect(st::boxRadius, _layer));

        positionLayer();
        _layer->show();

        show();
        raise();
        setFocus();
    }
}

void LayerStackWidget::hideLayer() {
    const auto restoreFocus = _restoreFocus;
    _restoreFocus.clear();
    if (_layer) {
        _layer->hide();
        _layer = nullptr;
    }
    hide();
    Focus::restoreFocusAfterPopup(restoreFocus);
    emit layerHidden();
}

void LayerStackWidget::paintEvent(QPaintEvent *) {
    positionLayer();

    QPainter p(this);

    // Semi-transparent overlay background (layerBg = #0000007f).
    p.fillRect(rect(), st::layerBg);

    if (!_layer) return;

    // Paint shadow around the layer box (same as other modal dialogs).
    paintBoxShadow(p, _layer->geometry());
}

void LayerStackWidget::mousePressEvent(QMouseEvent *e) {
    // Click on background (outside layer) closes the layer.
    if (_layer && !_layer->geometry().contains(e->pos())) {
        hideLayer();
    }
}

void LayerStackWidget::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape) {
        hideLayer();
        e->accept();
        return;
    }
    QWidget::keyPressEvent(e);
}

void LayerStackWidget::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    positionLayer();
}

void LayerStackWidget::positionLayer() {
    if (!_layer) return;

    // Center the layer horizontally, add vertical margins for rounded corners.
    const auto hint = _layer->sizeHint();
    const int hintedWidth = hint.width();
    const int layerWidth = qMin(hintedWidth > 0 ? hintedWidth : width(), width());
    const int x = (width() - layerWidth) / 2;
    const int margin = st::layerVerticalMargin;
    const int availableHeight = qMax(0, height() - margin * 2);
    const int hintedHeight = hint.height();
    const int layerHeight = (hintedHeight > 0)
        ? qMin(hintedHeight, availableHeight)
        : availableHeight;
    const int y = (hintedHeight > 0)
        ? margin + (availableHeight - layerHeight) / 2
        : margin;
    const QRect geometry(x, y, layerWidth, layerHeight);
    if (_layer->geometry() != geometry) {
        _layer->setGeometry(geometry);
    }
}

} // namespace TeleMatrix

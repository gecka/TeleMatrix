// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_pinned_bar.h"

#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>

#include "protocol/media_cache.h"
#include "styles/style_constants.h"
#include "ui/painter.h"
#include "ui/style/icon_provider.h"

namespace TeleMatrix {

namespace {

} // namespace

HistoryPinnedBar::HistoryPinnedBar(QWidget *parent)
    : QWidget(parent) {
    setMouseTracking(true);
    setFixedHeight(st::historyReplyHeight);
}

void HistoryPinnedBar::setPinnedMessage(
    const QString &eventId,
    const QString &title,
    const QString &text,
    const QString &previewPath) {
    _eventId = eventId;
    _title = title.isEmpty() ? QStringLiteral("Pinned Message") : title;
    _text = text.simplified();
    _previewPath = previewPath;
    _previewImage = _previewPath.isEmpty() ? QImage() : MediaCache::loadImage(_previewPath);
    _barPressed = false;
    _closePressed = false;
    update();
}

void HistoryPinnedBar::clearPinnedMessage() {
    _eventId.clear();
    _title.clear();
    _text.clear();
    _previewPath.clear();
    _previewImage = QImage();
    _closeRect = QRect();
    _closeHover = false;
    _barPressed = false;
    _closePressed = false;
    setCursor(Qt::ArrowCursor);
    update();
}

void HistoryPinnedBar::setPinnedCount(int count) {
    if (_pinnedCount == count) {
        return;
    }
    _pinnedCount = count;
    update();
}

void HistoryPinnedBar::ensureShowAllIcon() {
    if (!_showAllIcon.isNull()) {
        return;
    }
    const auto mask = TeleMatrix::Style::IconProvider::loadScaledMask(
        QStringLiteral(":/telematrix/icons/chat/"),
        QStringLiteral("pinned_show_all"));
    if (mask.isNull()) {
        return;
    }
    _showAllIcon = TeleMatrix::Style::IconProvider::colorizeMask(mask, st::historyReplyCancelFg);
    _showAllIconOver = TeleMatrix::Style::IconProvider::colorizeMask(mask, st::historyReplyCancelFgOver);
}

void HistoryPinnedBar::paintEvent(QPaintEvent *) {
    QPainter p(this);

    p.fillRect(rect(), st::historyPinnedBg);

    const auto barX = st::msgReplyBarSkip + st::msgReplyBarPos.x();
    const auto barY = st::msgReplyPadding.top() + st::msgReplyBarPos.y();
    const auto barW = st::msgReplyBarSize.width();
    const auto barH = st::msgReplyBarSize.height();
    {
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(st::msgInReplyBarColor);
        p.drawRoundedRect(QRect(barX, barY, barW, barH), 1.0, 1.0);
    }

    // Show-all / cancel button is 49×49.
    _closeRect = QRect(width() - 49, 0, 49, height());

    auto bodyLeft = 2 * st::msgReplyBarSkip;
    if (_previewImage.isNull() && !_previewPath.isEmpty()) {
        _previewImage = MediaCache::loadImage(_previewPath);
    }
    if (!_previewImage.isNull()) {
        const auto dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
        const auto previewRect = QRect(
            bodyLeft,
            (height() - st::historyReplyPreview) / 2,
            st::historyReplyPreview,
            st::historyReplyPreview);
        {
            PainterHighQualityEnabler hq(p);
            p.setPen(Qt::NoPen);
            p.setBrush(st::windowBgOver);
            p.drawRoundedRect(previewRect, 3.0, 3.0);
        }
        p.save();
        QPainterPath clip;
        clip.addRoundedRect(previewRect, 3.0, 3.0);
        p.setClipPath(clip);
        const auto drawSize = QSize(
            qMax(1, qRound(previewRect.width() * dpr)),
            qMax(1, qRound(previewRect.height() * dpr)));
        const auto scaled = _previewImage.scaled(
            drawSize,
            Qt::KeepAspectRatioByExpanding,
            Qt::SmoothTransformation);
        const auto sx = qMax(0, (scaled.width() - drawSize.width()) / 2);
        const auto sy = qMax(0, (scaled.height() - drawSize.height()) / 2);
        p.drawImage(previewRect, scaled, QRect(sx, sy, drawSize.width(), drawSize.height()));
        p.restore();
        bodyLeft += st::historyReplyPreview + 6;
    }
    const auto bodyRight = _closeRect.left() - st::msgReplyPadding.right();
    const auto bodyWidth = qMax(0, bodyRight - bodyLeft);
    const auto title = _title.isEmpty() ? QStringLiteral("Pinned Message") : _title;

    p.setPen(st::windowActiveTextFg);
    p.setFont(st::msgServiceNameFont);
    const auto titleText = QFontMetrics(st::msgServiceNameFont).elidedText(
        title,
        Qt::ElideRight,
        bodyWidth);
    p.drawText(
        bodyLeft,
        st::msgReplyPadding.top() + st::msgServiceNameFont->ascent,
        titleText);

    p.setPen(st::historyTextInFg);
    p.setFont(st::msgFont);
    const auto previewText = QFontMetrics(st::msgFont).elidedText(
        _text,
        Qt::ElideRight,
        bodyWidth);
    p.drawText(
        bodyLeft,
        st::msgReplyPadding.top() + st::msgServiceNameFont->height + st::msgFont->ascent,
        previewText);

    // Always draw show-all icon.
    // Icon position offset is (-1, -1) within the button.
    ensureShowAllIcon();
    const auto &icon = _closeHover ? _showAllIconOver : _showAllIcon;
    if (!icon.isNull()) {
        const auto iconW = int(icon.width() / icon.devicePixelRatio());
        const auto iconH = int(icon.height() / icon.devicePixelRatio());
        p.drawImage(
            QPoint(
                _closeRect.center().x() - iconW / 2 - 1,
                _closeRect.center().y() - iconH / 2 - 1),
            icon);
    }

}

void HistoryPinnedBar::mouseMoveEvent(QMouseEvent *e) {
    updateHoverState(e->pos());
    QWidget::mouseMoveEvent(e);
}

void HistoryPinnedBar::leaveEvent(QEvent *e) {
    if (_closeHover) {
        _closeHover = false;
        update();
    }
    setCursor(Qt::ArrowCursor);
    QWidget::leaveEvent(e);
}

void HistoryPinnedBar::mousePressEvent(QMouseEvent *e) {
    if (!_eventId.isEmpty() && e->button() == Qt::LeftButton) {
        _closePressed = _closeRect.contains(e->pos());
        _barPressed = !_closePressed;
        e->accept();
        return;
    }
    QWidget::mousePressEvent(e);
}

void HistoryPinnedBar::mouseReleaseEvent(QMouseEvent *e) {
    if (!_eventId.isEmpty() && e->button() == Qt::LeftButton) {
        const auto wasClosePressed = _closePressed;
        const auto wasBarPressed = _barPressed;
        _closePressed = false;
        _barPressed = false;

        if (wasClosePressed && _closeRect.contains(e->pos())) {
            emit showAllClicked();
            e->accept();
            return;
        }
        if (wasBarPressed && rect().contains(e->pos()) && !_closeRect.contains(e->pos())) {
            emit barClicked(_eventId);
            e->accept();
            return;
        }
    }
    QWidget::mouseReleaseEvent(e);
}

void HistoryPinnedBar::updateHoverState(const QPoint &pos) {
    const auto hover = !_eventId.isEmpty() && _closeRect.contains(pos);
    if (hover != _closeHover) {
        _closeHover = hover;
        update();
    }
    if (_eventId.isEmpty()) {
        setCursor(Qt::ArrowCursor);
    } else {
        setCursor((hover || rect().contains(pos))
            ? Qt::PointingHandCursor
            : Qt::ArrowCursor);
    }
}

} // namespace TeleMatrix

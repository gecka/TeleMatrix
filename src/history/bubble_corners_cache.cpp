// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history/bubble_corners_cache.h"

#include <QHash>
#include <QImage>
#include <QList>
#include <QPainter>
#include <QPainterPath>
#include <QPaintDevice>

#include <algorithm>
#include <cmath>

namespace TeleMatrix::BubbleSprites {

using HistoryMessage::BubbleCorners;
using HistoryMessage::BubbleTailSide;

BubbleCorners clampCorners(BubbleCorners c, int width, int height) {
    const auto limit = std::max(0, std::min(width, height) / 2);
    c.topLeft = std::clamp(c.topLeft, 0, limit);
    c.topRight = std::clamp(c.topRight, 0, limit);
    c.bottomRight = std::clamp(c.bottomRight, 0, limit);
    c.bottomLeft = std::clamp(c.bottomLeft, 0, limit);
    return c;
}

std::size_t qHash(const Key &k, std::size_t seed) {
    return qHashMulti(
        seed,
        k.topLeft,
        k.topRight,
        k.bottomRight,
        k.bottomLeft,
        k.tail,
        k.color,
        k.dprTimes100);
}

namespace {

enum Corner { TL = 0, TR = 1, BR = 2, BL = 3 };

struct Sprites {
    QImage corners[4];  // indexed by Corner
    QImage tail;        // left tail (empty otherwise)
};

constexpr int kMaxKeys = 64;

QHash<Key, Sprites> &spriteCache() {
    static QHash<Key, Sprites> cache;
    return cache;
}

QList<Key> &lruOrder() {
    static QList<Key> order;
    return order;
}

// Render one filled rounded corner into an r x r sprite (transparent outside the
// arc). The quad control point matches roundedBubblePath exactly, so the blit is
// pixel-identical to that path's fill in the corner box.
QImage renderCorner(int radius, Corner which, const QColor &color, qreal dpr) {
    if (radius <= 0) {
        return QImage();
    }
    QImage img(QSize(radius, radius) * dpr, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::transparent);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(color);

    const auto r = qreal(radius);
    QPainterPath path;
    switch (which) {
    case TL:  // outer corner at local (0,0)
        path.moveTo(r, 0);
        path.lineTo(r, r);
        path.lineTo(0, r);
        path.quadTo(0, 0, r, 0);
        break;
    case TR:  // outer corner at local (r,0)
        path.moveTo(0, 0);
        path.lineTo(0, r);
        path.lineTo(r, r);
        path.quadTo(r, 0, 0, 0);
        break;
    case BR:  // outer corner at local (r,r)
        path.moveTo(0, 0);
        path.lineTo(r, 0);
        path.quadTo(r, r, 0, r);
        break;
    case BL:  // outer corner at local (0,r)
        path.moveTo(0, 0);
        path.lineTo(r, 0);
        path.lineTo(r, r);
        path.quadTo(0, r, 0, 0);
        break;
    }
    path.closeSubpath();
    p.drawPath(path);
    return img;
}

// Left tail: local x=w is the bubble's left edge, sweeping to x=0 at the bottom.
// Matches bubbleTailPath(Left) exactly.
QImage renderTailLeft(int w, int h, const QColor &color, qreal dpr) {
    if (w <= 0 || h <= 0) {
        return QImage();
    }
    QImage img(QSize(w, h) * dpr, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::transparent);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(color);

    QPainterPath path;
    path.moveTo(w, 0);
    path.quadTo(w - 1.0, h - 4.0, 0.0, h);
    path.lineTo(w, h);
    path.closeSubpath();
    p.drawPath(path);
    return img;
}

Sprites spritesFor(
        const Key &key,
        const QColor &color,
        qreal dpr,
        int tailWidth,
        int tailHeight) {
    auto &cache = spriteCache();
    auto &order = lruOrder();
    const auto it = cache.find(key);
    if (it != cache.end()) {
        order.removeOne(key);
        order.prepend(key);
        return it.value();
    }

    Sprites s;
    s.corners[TL] = renderCorner(key.topLeft, TL, color, dpr);
    s.corners[TR] = renderCorner(key.topRight, TR, color, dpr);
    s.corners[BR] = renderCorner(key.bottomRight, BR, color, dpr);
    s.corners[BL] = renderCorner(key.bottomLeft, BL, color, dpr);
    if (key.tail == int(BubbleTailSide::Left)) {
        s.tail = renderTailLeft(tailWidth, tailHeight, color, dpr);
    }

    cache.insert(key, s);
    order.prepend(key);
    while (order.size() > kMaxKeys) {
        cache.remove(order.takeLast());
    }
    return s;
}

} // namespace

void paintBubble(
        QPainter &p,
        const QRectF &rect,
        BubbleCorners corners,
        BubbleTailSide tail,
        const QColor &color,
        int tailWidth,
        int tailHeight) {
    if (rect.isEmpty()) {
        return;
    }
    const auto dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
    corners = clampCorners(corners, int(rect.width()), int(rect.height()));

    const Key key{
        corners.topLeft,
        corners.topRight,
        corners.bottomRight,
        corners.bottomLeft,
        int(tail),
        color.rgba(),
        int(std::lround(dpr * 100.0))};
    const auto sprites = spritesFor(key, color, dpr, tailWidth, tailHeight);

    const auto l = rect.x();
    const auto t = rect.y();
    const auto w = rect.width();
    const auto h = rect.height();
    const auto r = l + w;
    const auto b = t + h;
    const auto tl = corners.topLeft;
    const auto tr = corners.topRight;
    const auto bl = corners.bottomLeft;
    const auto br = corners.bottomRight;
    const auto maxTop = qMax(tl, tr);
    const auto maxBottom = qMax(bl, br);

    // Interior fills — non-overlapping tiles covering everything but the four
    // corner boxes (clampCorners guarantees maxTop+maxBottom <= h, tl+tr <= w).
    if (h > maxTop + maxBottom) {
        p.fillRect(QRectF(l, t + maxTop, w, h - maxTop - maxBottom), color);
    }
    if (w > tl + tr) {
        p.fillRect(QRectF(l + tl, t, w - tl - tr, maxTop), color);
    }
    if (maxTop > tl) {
        p.fillRect(QRectF(l, t + tl, tl, maxTop - tl), color);
    }
    if (maxTop > tr) {
        p.fillRect(QRectF(r - tr, t + tr, tr, maxTop - tr), color);
    }
    if (w > bl + br) {
        p.fillRect(QRectF(l + bl, b - maxBottom, w - bl - br, maxBottom), color);
    }
    if (maxBottom > bl) {
        p.fillRect(QRectF(l, b - maxBottom, bl, maxBottom - bl), color);
    }
    if (maxBottom > br) {
        p.fillRect(QRectF(r - br, b - maxBottom, br, maxBottom - br), color);
    }

    // Corner blits.
    if (!sprites.corners[TL].isNull()) {
        p.drawImage(QPointF(l, t), sprites.corners[TL]);
    }
    if (!sprites.corners[TR].isNull()) {
        p.drawImage(QPointF(r - tr, t), sprites.corners[TR]);
    }
    if (!sprites.corners[BR].isNull()) {
        p.drawImage(QPointF(r - br, b - br), sprites.corners[BR]);
    }
    if (!sprites.corners[BL].isNull()) {
        p.drawImage(QPointF(l, b - bl), sprites.corners[BL]);
    }

    // Left tail (its right edge abuts the opaque body fill at x=l — no seam).
    if (tail == BubbleTailSide::Left && !sprites.tail.isNull()) {
        p.drawImage(QPointF(l - tailWidth, b - tailHeight), sprites.tail);
    }
}

} // namespace TeleMatrix::BubbleSprites

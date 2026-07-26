// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QColor>
#include <QRectF>
#include <cstddef>

#include "history/history_message_layout.h"

class QPainter;

// Cached rounded-bubble corner sprites:
// pre-render the four rounded corners (and the left tail) once per style key,
// then paint a bubble as a few non-overlapping fillRects plus corner blits
// instead of an anti-aliased QPainterPath fill per bubble per frame. The cache
// key excludes the bubble width/height (only the clamped radii, tail, colour and
// dpr matter), so a handful of keys cover every bubble on screen.
namespace TeleMatrix::BubbleSprites {

// Clamp corner radii to half the smaller dimension. Pure int math, matching
// roundedBubblePath's clamp; extracted so it can be unit-tested without Qt GUI.
[[nodiscard]] HistoryMessage::BubbleCorners clampCorners(
    HistoryMessage::BubbleCorners corners,
    int width,
    int height);

struct Key {
    int topLeft = 0;
    int topRight = 0;
    int bottomRight = 0;
    int bottomLeft = 0;
    int tail = 0;          // BubbleTailSide as int
    QRgb color = 0;
    int dprTimes100 = 100; // devicePixelRatio * 100, so keys are dpr-exact
    bool operator==(const Key &) const = default;
};

[[nodiscard]] std::size_t qHash(const Key &key, std::size_t seed = 0);

// Paint a single-colour rounded bubble (optional left tail) filling `rect` using
// cached corner/tail sprites. Pixel-identical to filling bubblePath() with the
// same colour. Requires an opaque colour (the fillRects tile without overlap, so
// translucent layers must use the path fill instead — see paintBubbleLayer).
// tailWidth/tailHeight are the (dpr-scaled) tail sprite dimensions; passed in so
// this stays free of the Qt-Widgets-heavy history_message.h.
void paintBubble(
    QPainter &p,
    const QRectF &rect,
    HistoryMessage::BubbleCorners corners,
    HistoryMessage::BubbleTailSide tail,
    const QColor &color,
    int tailWidth,
    int tailHeight);

} // namespace TeleMatrix::BubbleSprites

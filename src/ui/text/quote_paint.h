// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

// Quote / pre block decoration paint cache.
// Renders rounded block background, outline bar and top-right icon.
#pragma once

#include <array>
#include <QColor>
#include <QImage>
#include <QRect>
#include <QString>

#include "styles/style_constants.h"

class QPainter;

namespace Ui::Text {

/// Cached images for quote/pre block decoration rendering.
/// Rebuilt only when colors change (checked via stored color values).
struct QuotePaintCache {
    // Active colors for the cache.
    QColor bg;
    std::array<QColor, 3> outlines;
    QColor header;
    QColor icon;

    // Cached images.
    QImage corners;
    QImage outline;
    mutable QImage bottomCorner;
    mutable QImage bottomRounding;

    // Stored values to detect when cache needs rebuild.
    std::array<QColor, 3> _cachedOutlines;
    QColor _cachedBg;
    QColor _cachedHeader;
    QColor _cachedIcon;
    QString _cachedIconName;
    int _cachedDpr = 0;
};

/// Set cache colors from a base accent color using standard opacity values.
/// bg = 12%, outline = 90%, header = 30%, icon = 60%.
void SetQuoteCacheColors(QuotePaintCache &cache, const QColor &base);

/// Rebuild the cached corner image if colors have changed.
/// Call after SetQuoteCacheColors() and before FillQuotePaint().
void ValidateQuotePaintCache(
    QuotePaintCache &cache,
    const st::QuoteStyle &style);

/// Paint a quote/pre block decoration into the given rect using cached images.
/// Splits the rect into corner regions + center fills for crisp rendering.
void FillQuotePaint(
    QPainter &p,
    const QRect &rect,
    const QuotePaintCache &cache,
    const st::QuoteStyle &style);

} // namespace Ui::Text

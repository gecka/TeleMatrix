// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

// Quote / pre block decoration paint cache: validates and fills the rounded
// block background, outline bar and corner icon.

#include "ui/text/quote_paint.h"

#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

#include "ui/painter.h"
#include "ui/style/icon_provider.h"

namespace Ui::Text {
namespace {

constexpr int kLineWidth = 1;

[[nodiscard]] int DevicePixelRatio() {
    return qCeil(qGuiApp->devicePixelRatio());
}

[[nodiscard]] QImage coloredStyleIcon(
    const st::QuoteStyle &style,
    const QColor &color) {
    if (style.iconName.isEmpty()) {
        return {};
    }
    const auto mask = TeleMatrix::Style::IconProvider::loadScaledMask(
        QStringLiteral(":/telematrix/icons/chat/"),
        style.iconName);
    if (mask.isNull()) {
        return {};
    }
    return TeleMatrix::Style::IconProvider::colorizeMask(mask, color);
}

} // namespace

void SetQuoteCacheColors(QuotePaintCache &cache, const QColor &base) {
    cache.bg = base;
    cache.bg.setAlpha(31); // 0.12 * 255

    cache.outlines[0] = base;
    cache.outlines[0].setAlpha(230); // 0.9 * 255
    cache.outlines[1] = Qt::transparent;
    cache.outlines[2] = Qt::transparent;

    cache.header = base;
    cache.header.setAlpha(77); // 0.3 * 255

    cache.icon = base;
    cache.icon.setAlpha(153); // 0.6 * 255
}

void ValidateQuotePaintCache(
    QuotePaintCache &cache,
    const st::QuoteStyle &style) {
    const auto ratio = DevicePixelRatio();
    if (!cache.corners.isNull()
        && cache._cachedBg == cache.bg
        && cache._cachedOutlines == cache.outlines
        && (!style.header || cache._cachedHeader == cache.header)
        && (style.iconName.isEmpty() || cache._cachedIcon == cache.icon)
        && cache._cachedIconName == style.iconName
        && cache._cachedDpr == ratio) {
        return;
    }

    // Recolor the icon only on a cache miss — it was previously computed on every
    // call (including hits), a needless recolor per paint.
    const auto icon = coloredStyleIcon(style, cache.icon);

    cache._cachedBg = cache.bg;
    cache._cachedOutlines = cache.outlines;
    cache._cachedHeader = style.header ? cache.header : QColor();
    cache._cachedIcon = style.iconName.isEmpty() ? QColor() : cache.icon;
    cache._cachedIconName = style.iconName;
    cache._cachedDpr = ratio;

    const auto radius = style.radius;
    const auto header = style.header;
    const auto outline = style.outline;

    const auto iconW = icon.isNull()
        ? 0
        : int(icon.width() / icon.devicePixelRatio());
    const auto iconH = icon.isNull()
        ? 0
        : int(icon.height() / icon.devicePixelRatio());
    const auto wiconsize = iconW ? (iconW + style.iconPosition.x()) : 0;
    const auto hiconsize = iconH ? (iconH + style.iconPosition.y()) : 0;
    const auto wcorner = std::max({ radius, outline, wiconsize });
    const auto hcorner = std::max({ header, radius, hiconsize });
    const auto wside = 2 * wcorner + kLineWidth;
    const auto hside = 2 * hcorner + kLineWidth;

    if (!cache.outlines[1].alpha()) {
        cache.outline = QImage();
    } else if (const auto outlineSize = style.outline) {
        const auto third = (cache.outlines[2].alpha() != 0);
        const auto size = QSize(outlineSize, outlineSize * (third ? 6 : 4));
        cache.outline = QImage(
            size * ratio,
            QImage::Format_ARGB32_Premultiplied);
        cache.outline.fill(cache.outlines[0]);
        cache.outline.setDevicePixelRatio(ratio);

        QPainter p(&cache.outline);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        PainterHighQualityEnabler hq(p);

        auto path = QPainterPath();
        path.moveTo(outlineSize, outlineSize);
        path.lineTo(outlineSize, outlineSize * (third ? 4 : 3));
        path.lineTo(0, outlineSize * (third ? 5 : 4));
        path.lineTo(0, outlineSize * 2);
        path.lineTo(outlineSize, outlineSize);
        p.fillPath(path, cache.outlines[third ? 2 : 1]);

        if (third) {
            auto second = QPainterPath();
            second.moveTo(outlineSize, outlineSize * 3);
            second.lineTo(outlineSize, outlineSize * 5);
            second.lineTo(0, outlineSize * 6);
            second.lineTo(0, outlineSize * 4);
            second.lineTo(outlineSize, outlineSize * 3);
            p.fillPath(second, cache.outlines[1]);
        }
    }

    auto image = QImage(
        QSize(wside, hside) * ratio,
        QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    image.setDevicePixelRatio(ratio);

    {
        QPainter p(&image);
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);

        if (header) {
            p.setBrush(cache.header);
            p.setClipRect(outline, 0, wside - outline, header);
            p.drawRoundedRect(0, 0, wside, hcorner + radius, radius, radius);
        }
        if (outline) {
            const auto rect = QRect(0, 0, outline + radius * 2, hside);
            if (!cache.outline.isNull()) {
                const auto shift = QPoint(0, style.outlineShift);
                p.translate(shift);
                p.setBrush(QBrush(cache.outline));
                p.setClipRect(QRect(-shift, QSize(outline, hside)));
                p.drawRoundedRect(rect.translated(-shift), radius, radius);
                p.translate(-shift);
            } else {
                p.setBrush(cache.outlines[0]);
                p.setClipRect(0, 0, outline, hside);
                p.drawRoundedRect(rect, radius, radius);
            }
        }

        p.setBrush(cache.bg);
        p.setClipRect(outline, header, wside - outline, hside - header);
        p.drawRoundedRect(0, 0, wside, hside, radius, radius);

        if (!icon.isNull()) {
            p.setClipping(false);
            const auto left = wside - iconW - style.iconPosition.x();
            const auto top = style.iconPosition.y();
            p.drawImage(QRect(left, top, iconW, iconH), icon);
        }
    }

    cache.corners = std::move(image);
    cache.bottomCorner = QImage();
    cache.bottomRounding = QImage();
}

void FillQuotePaint(
    QPainter &p,
    const QRect &rect,
    const QuotePaintCache &cache,
    const st::QuoteStyle &style) {
    const auto &image = cache.corners;
    if (image.isNull() || rect.isEmpty()) {
        return;
    }

    const auto ratio = int(image.devicePixelRatio());
    const auto iwidth = image.width() / ratio;
    const auto iheight = image.height() / ratio;
    const auto whalf = (iwidth - kLineWidth) / 2;
    const auto hhalf = (iheight - kLineWidth) / 2;
    const auto x = rect.left();
    const auto width = rect.width();
    auto y = rect.top();
    auto height = rect.height();

    const auto top = std::min(height, hhalf);
    p.drawImage(
        QRect(x, y, whalf, top),
        image,
        QRect(0, 0, whalf * ratio, top * ratio));
    p.drawImage(
        QRect(x + width - whalf, y, whalf, top),
        image,
        QRect((iwidth - whalf) * ratio, 0, whalf * ratio, top * ratio));
    if (const auto middle = width - 2 * whalf) {
        const auto header = style.header;
        const auto fillHeader = std::min(header, top);
        if (fillHeader) {
            p.fillRect(x + whalf, y, middle, fillHeader, cache.header);
        }
        if (const auto fillBody = top - fillHeader) {
            p.fillRect(
                QRect(x + whalf, y + fillHeader, middle, fillBody),
                cache.bg);
        }
    }
    height -= top;
    if (!height) {
        return;
    }
    y += top;

    const auto outline = style.outline;
    const auto bottom = std::min(height, hhalf);
    const auto skip = !cache.outline.isNull() ? outline : 0;
    p.drawImage(
        QRect(x + skip, y + height - bottom, whalf - skip, bottom),
        image,
        QRect(
            skip * ratio,
            (iheight - bottom) * ratio,
            (whalf - skip) * ratio,
            bottom * ratio));
    p.drawImage(
        QRect(
            x + width - whalf,
            y + height - bottom,
            whalf,
            bottom),
        image,
        QRect(
            (iwidth - whalf) * ratio,
            (iheight - bottom) * ratio,
            whalf * ratio,
            bottom * ratio));
    if (const auto middle = width - 2 * whalf) {
        p.fillRect(
            QRect(x + whalf, y + height - bottom, middle, bottom),
            cache.bg);
    }

    if (skip) {
        if (cache.bottomCorner.size() != QSize(skip, hhalf)) {
            cache.bottomCorner = QImage(
                QSize(skip, hhalf) * ratio,
                QImage::Format_ARGB32_Premultiplied);
            cache.bottomCorner.setDevicePixelRatio(ratio);
            cache.bottomCorner.fill(Qt::transparent);

            cache.bottomRounding = QImage(
                QSize(skip, hhalf) * ratio,
                QImage::Format_ARGB32_Premultiplied);
            cache.bottomRounding.setDevicePixelRatio(ratio);
            cache.bottomRounding.fill(Qt::transparent);

            QPainter q(&cache.bottomRounding);
            PainterHighQualityEnabler hq(q);
            q.setPen(Qt::NoPen);
            q.setBrush(Qt::white);
            q.drawRoundedRect(
                0,
                -2 * style.radius,
                skip + 2 * style.radius,
                hhalf + 2 * style.radius,
                style.radius,
                style.radius);
        }

        QPainter q(&cache.bottomCorner);
        const auto skipped = (height - bottom) + hhalf - style.outlineShift;
        q.translate(0, -skipped);
        q.fillRect(0, skipped, skip, bottom, QBrush(cache.outline));
        q.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        q.drawImage(0, skipped + bottom - hhalf, cache.bottomRounding);
        q.end();

        p.drawImage(
            QRect(x, y + height - bottom, skip, bottom),
            cache.bottomCorner,
            QRect(0, 0, skip * ratio, bottom * ratio));
    }

    height -= bottom;
    if (outline && height > 0) {
        if (!cache.outline.isNull()) {
            const auto skipped = style.outlineShift - hhalf;
            const auto topShifted = y + skipped;
            p.translate(x, topShifted);
            p.fillRect(0, -skipped, outline, height, QBrush(cache.outline));
            p.translate(-x, -topShifted);
        } else {
            p.fillRect(x, y, outline, height, cache.outlines[0]);
        }
    }
    p.fillRect(x + outline, y, width - outline, height, cache.bg);
}

} // namespace Ui::Text

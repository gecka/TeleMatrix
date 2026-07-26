// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

// src/ui/style/icon_provider.cpp
#include "ui/style/icon_provider.h"
#include "ui/style/runtime_scale.h"

#include <QGuiApplication>
#include <QPainter>

namespace TeleMatrix::Style {

QHash<QString, QImage> IconProvider::_cache;

QString IconProvider::resourcePath(
        const QString &basePath,
        const QString &name,
        int band) {
    switch (band) {
    case 3: return basePath + name + QStringLiteral("@3x.png");
    case 2: return basePath + name + QStringLiteral("@2x.png");
    default: return basePath + name + QStringLiteral(".png");
    }
}

QImage IconProvider::loadScaledMask(
        const QString &basePath,
        const QString &name) {
    const auto scale = Scale();
    // Fractional ratio, not the integer DevicePixelRatio(): rounding it blurs
    // icons at fractional (125%/150%) Windows scaling.
    const qreal dprF = qApp->devicePixelRatio();
    const auto realScale = scale * dprF;

    // Select the best available sprite band for the current scale.
    int band;
    if (realScale <= 100) {
        band = 1;
    } else if (realScale <= 200) {
        band = 2;
    } else {
        band = 3;
    }

    auto image = QImage(resourcePath(basePath, name, band));

    // Fallback: if the selected band is missing, try lower bands.
    if (image.isNull() && band == 3) {
        image = QImage(resourcePath(basePath, name, 2));
        if (!image.isNull()) band = 2;
    }
    if (image.isNull() && band >= 2) {
        image = QImage(resourcePath(basePath, name, 1));
        if (!image.isNull()) band = 1;
    }
    if (image.isNull()) {
        return {};
    }

    // Derive 1x dimensions from the loaded band.
    const auto w1x = image.width() / band;
    const auto h1x = image.height() / band;

    const int targetW = qRound(ConvertScale(w1x, scale) * dprF);
    const int targetH = qRound(ConvertScale(h1x, scale) * dprF);

    // Scale if the loaded band doesn't match exactly.
    if (image.width() != targetW || image.height() != targetH) {
        image = image.scaled(
            targetW,
            targetH,
            Qt::IgnoreAspectRatio,
            Qt::SmoothTransformation);
    }

    image.setDevicePixelRatio(dprF);
    return image;
}

QImage IconProvider::colorizeMask(
        const QImage &mask,
        const QColor &color) {
    const auto source = mask.convertToFormat(QImage::Format_ARGB32);
    if (source.isNull()) {
        return {};
    }

    auto result = QImage(source.size(), QImage::Format_ARGB32_Premultiplied);
    const auto *src = reinterpret_cast<const QRgb*>(source.constBits());
    auto *dst = reinterpret_cast<QRgb*>(result.bits());
    const auto pixelCount = source.width() * source.height();
    auto alphaMask = false;
    for (auto i = 0; i < pixelCount; ++i) {
        if (qAlpha(src[i]) < 255) {
            alphaMask = true;
            break;
        }
    }
    for (auto i = 0; i < pixelCount; ++i) {
        // Icon masks are white-on-black (alpha often 255).
        // Use true alpha for transparent PNG masks, otherwise use pixel
        // brightness (red channel) as effective alpha.
        const auto srcAlpha = qAlpha(src[i]);
        const auto brightness = qRed(src[i]);
        const auto effectiveAlpha = alphaMask ? srcAlpha : brightness;
        const auto alpha = (effectiveAlpha * color.alpha()) / 255;
        dst[i] = qPremultiply(qRgba(
            color.red(),
            color.green(),
            color.blue(),
            alpha));
    }
    result.setDevicePixelRatio(source.devicePixelRatio());
    return result;
}

QImage IconProvider::tintedIcon(
        const QString &basePath,
        const QString &name,
        const QColor &color) {
    const auto key = basePath
        + name
        + QLatin1Char('|')
        + QString::number(Scale())
        + QLatin1Char('|')
        + QString::number(qApp->devicePixelRatio())
        + QLatin1Char('|')
        + QString::number(color.rgba(), 16);

    if (const auto it = _cache.constFind(key); it != _cache.cend()) {
        return it.value();
    }

    auto mask = loadScaledMask(basePath, name);
    if (mask.isNull()) {
        return {};
    }

    auto icon = colorizeMask(mask, color);
    _cache.insert(key, icon);
    return icon;
}

void IconProvider::clearCache() {
    _cache.clear();
}

} // namespace TeleMatrix::Style

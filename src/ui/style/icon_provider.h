// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

// src/ui/style/icon_provider.h
#pragma once

#include <QColor>
#include <QHash>
#include <QImage>
#include <QString>

namespace TeleMatrix::Style {

/// Centralized icon loading, scaling, colorization, and caching.
///
/// Replaces the per-file colorizeMaskIcon / tintedXxxIcon / iconResourcePath
/// pattern with a single scale-aware implementation using sprite-band
/// selection.
///
/// Usage:
///   auto icon = IconProvider::tintedIcon(":/telematrix/icons/menu/", "reply", color);
///   p.drawImage(x, y, icon);
///
/// The returned QImage has devicePixelRatio set correctly so QPainter draws
/// it at the right logical size.
class IconProvider {
public:
    /// Load a PNG mask icon from `basePath + name`, select the best
    /// 1x/2x/3x variant for the current Scale() * DevicePixelRatio(),
    /// scale to exact target size if needed, and return the raw mask.
    /// The returned image has devicePixelRatio set.
    [[nodiscard]] static QImage loadScaledMask(
        const QString &basePath,
        const QString &name);

    /// Colorize a pre-loaded mask image with the given color.
    /// Does NOT cache — use when you already have the mask.
    [[nodiscard]] static QImage colorizeMask(
        const QImage &mask,
        const QColor &color);

    /// Load + colorize + cache.  This is the primary API replacing
    /// tintedMenuIcon / tintedChatIcon / tintedDialogIcon.
    /// Cache key includes name, scale, dpr, and color.
    [[nodiscard]] static QImage tintedIcon(
        const QString &basePath,
        const QString &name,
        const QColor &color);

    /// Clear all cached icons.  Call on scale change or theme change.
    static void clearCache();

private:
    /// Resolve the resource path for a given band (1, 2, or 3).
    [[nodiscard]] static QString resourcePath(
        const QString &basePath,
        const QString &name,
        int band);

    static QHash<QString, QImage> _cache;
};

} // namespace TeleMatrix::Style

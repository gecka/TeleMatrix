// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

// Draws emoji from the sprite atlases, keyed by the plain QString the rest of the app
// already passes around (Matrix reaction keys, the picker table, SAS emoji). Nothing
// upstream of this needs to learn about EmojiPtr.
//
// Every entry point falls back to QPainter::drawText() with the painter's current font
// when an emoji has no sprite — an emoji newer than the vendored atlases, a non-emoji
// reaction key, or a build whose Qt cannot decode WebP. That keeps the port strictly
// additive: worst case is exactly today's behaviour.
//
// Main thread only (it caches QPixmaps).

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtGui/QPixmap>

class QPainter;
class QRect;

namespace TeleMatrix::Emoji {

// True when sprite emoji are actually usable. Decodes one atlas page the first time
// it is called, so prefer calling it once at startup rather than per paint.
[[nodiscard]] bool Available();

// Top-left at (x, y), `sizePx` square, in logical pixels.
void Draw(QPainter &p, const QString &emoji, int sizePx, int x, int y);

// Centered in `cell`. The fallback is p.drawText(cell, Qt::AlignCenter, emoji), which
// is what the call sites did before the port.
void DrawCentered(QPainter &p, const QString &emoji, int sizePx, const QRect &cell);

// For widgets that need a pixmap rather than a painter (QLabel). Null when unavailable.
[[nodiscard]] QPixmap Pixmap(const QString &emoji, int sizePx);

// Populates the cache for a known set of emoji, visiting atlas pages in order so each
// is decoded at most once instead of thrashing the two-page residency cap. Worth doing
// before showing a grid of hundreds of emoji; pointless for a handful.
void Prewarm(const QStringList &emoji, int sizePx);

// Drops cached pixmaps and decoded atlas pages. Call when the interface scale or the
// device pixel ratio changes — cached pixmaps are sized in device pixels.
void ClearCache();

} // namespace TeleMatrix::Emoji

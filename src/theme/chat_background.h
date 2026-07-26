// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QColor>
#include <QImage>
#include <QPixmap>
#include <QSize>
#include <QString>

// Chat wallpaper compositing. The background is two layers:
// a 4-colour gradient, and a monochrome doodle tiled over it in soft-light at
// 50% opacity. Soft-light is what makes the doodle read as embossed texture --
// the same artwork darkens the light corners and lightens the dark ones.
//
// Everything here is pure (no st::, no QtWidgets) so it can be unit-tested
// headless. Not thread-safe: the caches below assume the GUI thread.
namespace TeleMatrix::Theme {

// The default wallpaper uses pattern intensity 50; opacity is intensity / 100.
inline constexpr auto kPatternOpacity = 0.5;

// Which theme's doodle the wallpaper is drawn from. Each theme ships its own
// pattern.svg; ThemeManager points this at the active one, and it starts on the
// default theme's. Setting a different path bumps PatternGeneration(), which is
// what invalidates the tile caches and any ChatBackgroundCache built from the
// previous doodle.
void SetPatternPath(const QString &path);
[[nodiscard]] const QString &PatternPath();
[[nodiscard]] quint64 PatternGeneration();

// The doodle, rasterised from PatternPath() at its native 1440x2960 and kept as
// Format_Alpha8 (~4.3 MB, against 17 MB for ARGB32). Only the coverage matters
// -- the tint is chosen per tile. Null if the SVG is missing or fails to parse.
// Two rasterisations are cached, so flipping between two themes in the settings
// preview doesn't re-render the SVG on every click.
//
// Coverage is alpha * (1 - luminance), so the artwork must be DARK on a light
// or transparent canvas. An opaque light background rect is fine and reads as
// empty; light-on-transparent artwork reads as empty everywhere.
[[nodiscard]] const QImage &PatternAlpha();

// Mean colour across all pixels of the image.
[[nodiscard]] QColor AverageColor(const QImage &image);

// A dark background needs a white doodle, since soft-lighting black onto
// near-black is a no-op.
[[nodiscard]] bool IsPatternInverted(QColor average);

struct CornerColors {
	QColor topLeft;
	QColor topRight;
	QColor bottomRight;
	QColor bottomLeft;
};

// Bilinear blend of four corner colours. The fallback wallpaper when the theme
// ships no background image.
[[nodiscard]] QImage GenerateCornerGradient(QSize size, CornerColors colors);

struct TileLayout {
	int columns = 0; // always odd
	int rows = 0;
	qreal xshift = 0.; // logical px; negative when the run overhangs
};

// Where the doodle tiles land. Columns are forced odd so one tile sits dead
// centre, and the run is centred horizontally -- so the overhang is split
// evenly and there is no seam bias. `deviceTile` is the tile in device pixels.
[[nodiscard]] TileLayout ComputeTileLayout(QSize area, qreal dpr, QSize deviceTile);

// Scale `gradient` to `area`, then tile the doodle over it. The tile is scaled
// to the area's height keeping aspect, so it comes out tall and narrow
// (w ~= 0.4865 * h) and exactly one row covers the viewport. Columns are forced
// odd and centred horizontally.
//
// `area` is in logical pixels; the returned image is `area * dpr` device pixels
// with devicePixelRatio() set, in Format_ARGB32_Premultiplied.
[[nodiscard]] QImage ComposeChatBackground(
	const QImage &gradient,
	QSize area,
	qreal dpr,
	bool inverted,
	qreal patternOpacity = kPatternOpacity);

// Per-widget cache of the composited viewport pixmap. Deliberately not a
// QObject and holds no timer: the owning widget decides when a stale cache is
// worth rebuilding.
class ChatBackgroundCache {
public:
	// Themes describe their wallpaper as four corner colours, so `fallback` is
	// the normal path; `themeBackground` covers a theme that ships an image
	// instead. Recomputes the inversion flag and drops any composited pixmap.
	void setSource(const QPixmap &themeBackground, CornerColors fallback);

	// Whether the doodle is soft-lit over the gradient ("Display background
	// doodles"). Off leaves the bare gradient. Drops the cache when it changes.
	void setDoodlesEnabled(bool enabled);

	[[nodiscard]] bool isNull() const;

	// False once the doodle changed under us, so a theme switch that only
	// repaints (without re-feeding the source) still rebuilds.
	[[nodiscard]] bool matches(QSize area, qreal dpr) const;

	// Valid once rebuild() has run at least once. May be sized for a stale
	// area -- check matches() and stretch it while a rebuild is pending.
	[[nodiscard]] const QPixmap &pixmap() const;

	// Synchronous. Cheap to call when it already matches().
	void rebuild(QSize area, qreal dpr);

private:
	QImage _source;
	QPixmap _pixmap;
	QSize _area;
	qreal _dpr = 0.;
	quint64 _patternGeneration = 0;
	bool _inverted = false;
	bool _doodles = true;
};

} // namespace TeleMatrix::Theme

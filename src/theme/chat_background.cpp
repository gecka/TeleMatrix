// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "theme/chat_background.h"

#include <QPainter>
#include <QSvgRenderer>

#include <array>
#include <cmath>

namespace TeleMatrix::Theme {
namespace {

QString &PatternPathRef() {
	// The default theme's doodle. A literal rather than a registry lookup, so
	// this file stays free of everything but Qt.
	static auto path = QStringLiteral(":/theme/dubai/pattern.svg");
	return path;
}

quint64 &PatternGenerationRef() {
	static quint64 generation = 0;
	return generation;
}

struct TileKey {
	int deviceHeight = 0;
	bool inverted = false;
	int dprTimes100 = 0;   // so the key is dpr-exact
	quint64 generation = 0; // so a doodle swap can't hit a stale tile
	bool operator==(const TileKey &) const = default;
};

// Smooth-downscaling the 1440x2960 doodle dominates the cost of a rebuild, and
// the history list, the pinned list and the no-chat placeholder all ask for the
// same height. Two slots cover a day/night pair, or a stale size alongside the
// live one during a resize.
[[nodiscard]] QImage PatternTile(int deviceHeight, bool inverted, qreal dpr) {
	struct Entry {
		TileKey key;
		QImage image;
	};
	static std::array<Entry, 2> cache;
	static auto next = 0;

	const auto key = TileKey{
		deviceHeight,
		inverted,
		qRound(dpr * 100),
		PatternGeneration()};
	for (const auto &entry : cache) {
		if (entry.key == key && !entry.image.isNull()) {
			return entry.image; // QImage is implicitly shared: no copy.
		}
	}

	const auto &alpha = PatternAlpha();
	if (alpha.isNull() || deviceHeight <= 0) {
		return QImage();
	}

	// KeepAspectRatio into a square box pins the tile to the box height, so a
	// 1440x2960 source comes out tall and narrow (w ~= 0.4865 * h).
	auto scaled = alpha
		.scaled(
			deviceHeight,
			deviceHeight,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation)
		.convertToFormat(QImage::Format_Alpha8);
	if (scaled.isNull() || scaled.width() <= 0) {
		return QImage();
	}

	// Tint the coverage mask. Premultiplied, so white is (a,a,a,a) and black is
	// (0,0,0,a).
	auto tile = QImage(scaled.size(), QImage::Format_ARGB32_Premultiplied);
	for (auto y = 0, h = scaled.height(); y != h; ++y) {
		const auto *src = scaled.constScanLine(y);
		auto *dst = reinterpret_cast<QRgb *>(tile.scanLine(y));
		for (auto x = 0, w = scaled.width(); x != w; ++x) {
			const auto a = uint(src[x]);
			dst[x] = inverted
				? ((a << 24) | (a << 16) | (a << 8) | a)
				: (a << 24);
		}
	}
	tile.setDevicePixelRatio(dpr);

	cache[next] = Entry{key, tile};
	next = (next + 1) % int(cache.size());
	return tile;
}

// Rasterise one doodle to its coverage mask.
//
// Coverage = alpha * (1 - luminance). Only the coverage survives; the tint is
// chosen per tile. Weighting by luminance rather than reading alpha alone
// accepts both conventions an exported doodle may use:
//   - dark art on a transparent canvas, and
//   - dark art on an opaque light background rect (a plain SVG export),
// which would otherwise rasterise to a fully-opaque mask and darken the whole
// wallpaper uniformly instead of drawing doodles. The artwork must therefore be
// DARK on light/transparent -- light-on-transparent art reads as empty.
[[nodiscard]] QImage RasterisePattern(const QString &path) {
	auto renderer = QSvgRenderer(path);
	if (!renderer.isValid()) {
		return QImage();
	}
	const auto size = renderer.defaultSize(); // 1440x2960 from the viewBox
	if (size.isEmpty()) {
		return QImage();
	}
	// Non-premultiplied, so qGray() below sees the true stroke colour.
	auto argb = QImage(size, QImage::Format_ARGB32);
	argb.fill(Qt::transparent);
	{
		auto p = QPainter(&argb);
		renderer.render(&p, QRect(QPoint(), size));
	}

	auto mask = QImage(size, QImage::Format_Alpha8);
	for (auto y = 0, h = size.height(); y != h; ++y) {
		const auto *src = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
		auto *dst = mask.scanLine(y);
		for (auto x = 0, w = size.width(); x != w; ++x) {
			const auto pixel = src[x];
			dst[x] = uchar(qAlpha(pixel) * (255 - qGray(pixel)) / 255);
		}
	}
	return mask; // the 17 MB ARGB buffer is freed here
}

} // namespace

void SetPatternPath(const QString &path) {
	if (PatternPathRef() == path) {
		return;
	}
	PatternPathRef() = path;
	++PatternGenerationRef();
}

const QString &PatternPath() {
	return PatternPathRef();
}

quint64 PatternGeneration() {
	return PatternGenerationRef();
}

const QImage &PatternAlpha() {
	// Two slots so the settings preview can flip between a pair of themes
	// without re-rendering the SVG each time.
	struct Entry {
		QString path;
		QImage image;
	};
	static std::array<Entry, 2> cache;
	static auto next = 0;

	const auto &path = PatternPathRef();
	for (auto &entry : cache) {
		if (entry.path == path) {
			return entry.image;
		}
	}

	cache[next] = Entry{path, RasterisePattern(path)};
	const auto &stored = cache[next].image;
	next = (next + 1) % int(cache.size());
	return stored;
}

QColor AverageColor(const QImage &image) {
	if (image.isNull()) {
		return QColor();
	}
	const auto rgb = image.convertToFormat(QImage::Format_RGB32);
	const auto width = rgb.width();
	const auto height = rgb.height();
	const auto size = quint64(width) * quint64(height);
	if (!size) {
		return QColor();
	}
	quint64 components[3] = {0, 0, 0};
	for (auto y = 0; y != height; ++y) {
		const auto *line = reinterpret_cast<const QRgb *>(rgb.constScanLine(y));
		for (auto x = 0; x != width; ++x) {
			components[0] += uint(qRed(line[x]));
			components[1] += uint(qGreen(line[x]));
			components[2] += uint(qBlue(line[x]));
		}
	}
	return QColor(
		int(components[0] / size),
		int(components[1] / size),
		int(components[2] / size));
}

bool IsPatternInverted(QColor average) {
	return average.isValid() && (average.toHsv().valueF() <= 0.3);
}

QImage GenerateCornerGradient(QSize size, CornerColors colors) {
	const auto width = size.width();
	const auto height = size.height();
	if (width <= 0 || height <= 0) {
		return QImage();
	}
	auto result = QImage(width, height, QImage::Format_RGB32);
	const auto &c0 = colors.topLeft;
	const auto &c1 = colors.topRight;
	const auto &c2 = colors.bottomRight;
	const auto &c3 = colors.bottomLeft;
	for (auto y = 0; y != height; ++y) {
		auto *line = reinterpret_cast<QRgb *>(result.scanLine(y));
		const auto fy = qreal(y) / qreal(qMax(height - 1, 1));
		for (auto x = 0; x != width; ++x) {
			const auto fx = qreal(x) / qreal(qMax(width - 1, 1));
			const auto tr = c0.red() + fx * (c1.red() - c0.red());
			const auto tg = c0.green() + fx * (c1.green() - c0.green());
			const auto tb = c0.blue() + fx * (c1.blue() - c0.blue());
			const auto br = c3.red() + fx * (c2.red() - c3.red());
			const auto bg = c3.green() + fx * (c2.green() - c3.green());
			const auto bb = c3.blue() + fx * (c2.blue() - c3.blue());
			const auto r = qBound(0, int(tr + fy * (br - tr)), 255);
			const auto g = qBound(0, int(tg + fy * (bg - tg)), 255);
			const auto b = qBound(0, int(tb + fy * (bb - tb)), 255);
			line[x] = qRgb(r, g, b);
		}
	}
	return result;
}

TileLayout ComputeTileLayout(QSize area, qreal dpr, QSize deviceTile) {
	if (area.isEmpty() || dpr <= 0. || deviceTile.isEmpty()) {
		return {};
	}
	// drawImage(QPointF, QImage) sizes the image by size() / devicePixelRatio(),
	// so the tile occupies w x h *logical* units. PatternTile stamps dpr onto
	// the tile to make that true.
	const auto w = deviceTile.width() / dpr;
	const auto h = deviceTile.height() / dpr;
	const auto cx = int(std::ceil(area.width() / w));
	const auto cy = int(std::ceil(area.height() / h));
	const auto columns = ((cx / 2) * 2) + 1; // odd, so one tile lands dead centre
	const auto deviceWidth = qRound(area.width() * dpr);
	return {
		.columns = columns,
		.rows = cy,
		.xshift = (deviceWidth - columns * deviceTile.width()) / (2. * dpr),
	};
}

QImage ComposeChatBackground(
		const QImage &gradient,
		QSize area,
		qreal dpr,
		bool inverted,
		qreal patternOpacity) {
	if (gradient.isNull() || area.isEmpty() || dpr <= 0.) {
		return QImage();
	}
	const auto device = QSize(
		qRound(area.width() * dpr),
		qRound(area.height() * dpr));

	auto result = QImage(device, QImage::Format_ARGB32_Premultiplied);
	if (gradient.size() == QSize(1, 1)) {
		// A 1x1 theme background is a solid colour (night ships one); skip the
		// smooth scaler entirely.
		result.fill(gradient.pixelColor(0, 0));
	} else {
		result = gradient
			.scaled(device, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
			.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	}
	result.setDevicePixelRatio(dpr);

	if (patternOpacity <= 0.) {
		// Doodles off: the gradient is the whole background. Return before
		// rasterising a tile that would be composited at zero opacity.
		return result;
	}

	const auto tile = PatternTile(device.height(), inverted, dpr);
	if (tile.isNull() || tile.width() <= 0) {
		return result;
	}

	auto p = QPainter(&result);
	p.setCompositionMode(QPainter::CompositionMode_SoftLight);
	p.setOpacity(patternOpacity);

	const auto layout = ComputeTileLayout(area, dpr, tile.size());
	const auto w = tile.width() / dpr;
	const auto h = tile.height() / dpr;
	for (auto y = 0; y != layout.rows; ++y) {
		for (auto x = 0; x != layout.columns; ++x) {
			p.drawImage(QPointF(layout.xshift + x * w, y * h), tile);
		}
	}
	return result;
}

void ChatBackgroundCache::setSource(
		const QPixmap &themeBackground,
		CornerColors fallback) {
	_source = themeBackground.isNull()
		? GenerateCornerGradient(QSize(512, 512), fallback)
		: themeBackground.toImage();
	_inverted = IsPatternInverted(AverageColor(_source));
	_pixmap = QPixmap();
	_area = QSize();
	_dpr = 0.;
	_patternGeneration = PatternGeneration();
}

bool ChatBackgroundCache::isNull() const {
	return _pixmap.isNull();
}

bool ChatBackgroundCache::matches(QSize area, qreal dpr) const {
	return !_pixmap.isNull()
		&& (_area == area)
		&& qFuzzyCompare(_dpr, dpr)
		&& (_patternGeneration == PatternGeneration());
}

const QPixmap &ChatBackgroundCache::pixmap() const {
	return _pixmap;
}

void ChatBackgroundCache::setDoodlesEnabled(bool enabled) {
	if (_doodles == enabled) {
		return;
	}
	_doodles = enabled;
	_pixmap = QPixmap();
	_area = QSize();
	_dpr = 0.;
}

void ChatBackgroundCache::rebuild(QSize area, qreal dpr) {
	if (matches(area, dpr)) {
		return;
	}
	auto image = ComposeChatBackground(
		_source,
		area,
		dpr,
		_inverted,
		_doodles ? kPatternOpacity : 0.);
	if (image.isNull()) {
		return;
	}
	_pixmap = QPixmap::fromImage(std::move(image));
	_area = area;
	_dpr = dpr;
	// Whatever doodle ComposeChatBackground just tiled is the one this pixmap
	// now holds; without this, matches() would stay false and every paint would
	// recomposite after a theme switch.
	_patternGeneration = PatternGeneration();
}

} // namespace TeleMatrix::Theme

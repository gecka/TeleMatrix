// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from desktop-app/lib_ui (ui/emoji_config.cpp), which is:
//   Copyright the Desktop App Toolkit authors.
//   https://github.com/desktop-app/legal/blob/master/LEGAL
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "ui/emoji_config.h"

#include <QtCore/QDebug>
#include <QtGui/QImageReader>

#include <algorithm>
#include <deque>
#include <vector>

namespace Ui {
namespace Emoji {
namespace {

// Must match codegen_emoji's geometry (lib/codegen/codegen/emoji/generator.cpp:177-190)
// and the index bit-packing in One::sprite()/row()/column(). Change one, change all three,
// and re-vendor the atlases.
constexpr auto kUniversalSize = 72;
constexpr auto kImagesPerRow = 32;
constexpr auto kImageRowsPerSprite = 16;

// Decoded atlas pages are 2304x1152 ARGB32 — 10.6 MB each, 85 MB for all eight. Upstream
// can hold them because it immediately pre-scales whole atlases and frees the masters;
// we scale per emoji instead, so we cap residency and evict least-recently-used. Two is
// enough in practice: ui/emoji_sprites.cpp caches the scaled result per emoji, so a page
// is only re-touched by an emoji nobody has drawn yet at that size.
constexpr auto kMaxResidentPages = 2;

auto SpritesCountValue = -1;
auto Pages = std::vector<QImage>();
auto ResidentOrder = std::deque<int>();   // least-recently-used first

enum class Probe {
	Unknown,
	Good,
	Failed,
};
auto AtlasProbe = Probe::Unknown;

[[nodiscard]] QString PagePath(int index) {
	return QStringLiteral(":/gui/emoji/emoji_%1.webp").arg(index + 1);
}

void EvictDownTo(int limit) {
	while (int(ResidentOrder.size()) > limit) {
		const auto victim = ResidentOrder.front();
		ResidentOrder.pop_front();
		Pages[victim] = QImage();
	}
}

void TouchPage(int index) {
	const auto i = std::find(begin(ResidentOrder), end(ResidentOrder), index);
	if (i != end(ResidentOrder)) {
		ResidentOrder.erase(i);
	}
	ResidentOrder.push_back(index);
}

// Loads page `index`, validating it against the geometry the generated tables imply.
// A size mismatch means the atlases and emoji.cpp came from different upstream
// revisions — that would silently draw every emoji as the wrong picture, so refuse.
[[nodiscard]] const QImage *EnsurePage(int index) {
	if (index < 0 || index >= SpritesCountValue) {
		return nullptr;
	}
	if (!Pages[index].isNull()) {
		TouchPage(index);
		return &Pages[index];
	}

	auto image = QImage(PagePath(index), "WEBP");
	if (image.isNull()) {
		if (AtlasProbe != Probe::Failed) {
			AtlasProbe = Probe::Failed;
			const auto formats = QImageReader::supportedImageFormats();
			qWarning().noquote()
				<< "Emoji: could not decode" << PagePath(index)
				<< "- WebP support is"
				<< (formats.contains("webp") ? "present" : "MISSING")
				<< "(needs the qtimageformats module). Falling back to text emoji.";
		}
		return nullptr;
	}

	const auto expected = QSize(
		kImagesPerRow * kUniversalSize,
		RowsCount(index) * kUniversalSize);
	if (image.size() != expected) {
		AtlasProbe = Probe::Failed;
		qWarning().noquote()
			<< "Emoji:" << PagePath(index) << "is" << image.size()
			<< "but the generated tables expect" << expected
			<< "- the atlases and emoji.txt are from different revisions."
			<< "See resources/emoji/README.md. Falling back to text emoji.";
		return nullptr;
	}

	EvictDownTo(kMaxResidentPages - 1);
	Pages[index] = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	TouchPage(index);
	AtlasProbe = Probe::Good;
	return &Pages[index];
}

} // namespace

void Init() {
	internal::Init();

	const auto count = internal::FullCount();
	const auto persprite = kImagesPerRow * kImageRowsPerSprite;
	SpritesCountValue = (count / persprite) + ((count % persprite) ? 1 : 0);
	Pages.assign(SpritesCountValue, QImage());
	ResidentOrder.clear();
	AtlasProbe = Probe::Unknown;
}

bool Available() {
	if (AtlasProbe == Probe::Unknown) {
		EnsurePage(0);
	}
	return (AtlasProbe == Probe::Good);
}

int SpritesCount() {
	return SpritesCountValue;
}

int RowsCount(int index) {
	if (index + 1 < SpritesCountValue) {
		return kImageRowsPerSprite;
	}
	const auto count = internal::FullCount()
		- (index * kImagesPerRow * kImageRowsPerSprite);
	return (count / kImagesPerRow)
		+ ((count % kImagesPerRow) ? 1 : 0);
}

QImage Cell(EmojiPtr emoji, int sizePx) {
	if (!emoji || sizePx <= 0) {
		return QImage();
	}
	const auto page = EnsurePage(emoji->sprite());
	if (!page) {
		return QImage();
	}

	// Upstream's UniversalImages::draw() cell extraction: wrap the 72x72 cell in a
	// non-owning QImage over the page's pixels, then scale. The row offset goes through
	// bytesPerLine rather than assuming a packed stride — same result for these atlases,
	// but it does not depend on Qt's alignment choice.
	const auto large = kUniversalSize;
	const auto data = page->constBits();
	const auto stride = page->bytesPerLine();
	const auto format = page->format();
	const auto row = emoji->row();
	const auto column = emoji->column();
	const auto cell = QImage(
		data + (row * large * stride) + (column * large * 4),
		large,
		large,
		stride,
		format);

	// `cell` does not own its pixels — it is a window onto the atlas page, which the
	// LRU above may evict at any later point. Scaling deep-copies, EXCEPT that
	// QImage::scaled() short-circuits to `return *this` when the requested size already
	// equals the source size. Hitting that path would hand out an alias of page memory,
	// QPixmap::fromImage would share the same buffer, and the cached pixmap would draw
	// freed memory as noise once the page was evicted. Jumbo emoji land on it exactly:
	// 36px at devicePixelRatio 2 is 72, the cell size. So copy explicitly.
	return (sizePx == large)
		? cell.copy()
		: cell.scaled(
			sizePx,
			sizePx,
			Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation);
}

void ReleasePages() {
	EvictDownTo(0);
}

One::One(
	const QString &id,
	EmojiPtr original,
	uint32 index,
	bool hasPostfix,
	bool colorizable,
	const CreationTag &)
: _id(id)
, _original(original)
, _index(index)
, _hasPostfix(hasPostfix)
, _colorizable(colorizable) {
	Expects(!_colorizable || !colored());
}

int One::variantsCount() const {
	return hasVariants() ? 5 : 0;
}

int One::variantIndex(EmojiPtr variant) const {
	return (variant - original());
}

EmojiPtr One::variant(int index) const {
	return (index >= 0 && index <= variantsCount()) ? (original() + index) : this;
}

} // namespace Emoji
} // namespace Ui

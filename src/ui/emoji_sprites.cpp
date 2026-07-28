// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "ui/emoji_sprites.h"

#include "ui/emoji_config.h"
#include "ui/style/runtime_scale.h"

#include <QtCore/QHash>
#include <QtCore/QRect>
#include <QtGui/QPainter>

#include <algorithm>
#include <vector>

namespace TeleMatrix::Emoji {
namespace {

// Emoji are small, so entries are cheap (a 40px cell at dpr 2 is ~25 KB). The cap is a
// runaway guard, not a working-set limit; the picker's whole table at one size is ~700.
constexpr auto kMaxCachedPixmaps = 4096;

auto Resolved = QHash<QString, EmojiPtr>();
auto Pixmaps = QHash<quint64, QPixmap>();
auto CachedRatio = 0;

[[nodiscard]] quint64 CacheKey(EmojiPtr emoji, int sizeDevicePx) {
	return (quint64(emoji->index()) << 20) | quint64(sizeDevicePx & 0xFFFFF);
}

// Cached QString -> EmojiPtr. Misses are cached as nullptr too: reaction keys are
// arbitrary strings from the server, and re-running the trie on every paint of a
// non-emoji key would be pure waste.
[[nodiscard]] EmojiPtr Resolve(const QString &emoji) {
	if (emoji.isEmpty()) {
		return nullptr;
	}
	const auto i = Resolved.constFind(emoji);
	if (i != Resolved.cend()) {
		return i.value();
	}
	// Find() matches a prefix; a trailing variation selector that the table does not
	// carry still resolves to the right picture, so a partial match is accepted.
	const auto found = Ui::Emoji::Find(QStringView(emoji));
	Resolved.insert(emoji, found);
	return found;
}

void EnsureRatioCurrent() {
	const auto ratio = Style::DevicePixelRatio();
	if (ratio != CachedRatio) {
		Pixmaps.clear();
		CachedRatio = ratio;
	}
}

[[nodiscard]] QPixmap Lookup(EmojiPtr emoji, int sizePx) {
	if (!emoji || sizePx <= 0) {
		return QPixmap();
	}
	EnsureRatioCurrent();

	const auto ratio = std::max(CachedRatio, 1);
	const auto sizeDevicePx = sizePx * ratio;
	const auto key = CacheKey(emoji, sizeDevicePx);
	const auto i = Pixmaps.constFind(key);
	if (i != Pixmaps.cend()) {
		return i.value();
	}

	auto image = Ui::Emoji::Cell(emoji, sizeDevicePx);
	if (image.isNull()) {
		return QPixmap();
	}
	image.setDevicePixelRatio(ratio);
	auto pixmap = QPixmap::fromImage(std::move(image));

	if (Pixmaps.size() >= kMaxCachedPixmaps) {
		Pixmaps.clear();
	}
	Pixmaps.insert(key, pixmap);
	return pixmap;
}

} // namespace

bool Available() {
	return Ui::Emoji::Available();
}

QPixmap Pixmap(const QString &emoji, int sizePx) {
	return Lookup(Resolve(emoji), sizePx);
}

void Draw(QPainter &p, const QString &emoji, int sizePx, int x, int y) {
	const auto pixmap = Lookup(Resolve(emoji), sizePx);
	if (pixmap.isNull()) {
		p.drawText(QRect(x, y, sizePx, sizePx), Qt::AlignCenter, emoji);
		return;
	}
	p.drawPixmap(QPoint(x, y), pixmap);
}

void DrawCentered(
		QPainter &p,
		const QString &emoji,
		int sizePx,
		const QRect &cell) {
	const auto pixmap = Lookup(Resolve(emoji), sizePx);
	if (pixmap.isNull()) {
		p.drawText(cell, Qt::AlignCenter, emoji);
		return;
	}
	p.drawPixmap(
		QPoint(
			cell.x() + (cell.width() - sizePx) / 2,
			cell.y() + (cell.height() - sizePx) / 2),
		pixmap);
}

void Prewarm(const QStringList &emoji, int sizePx) {
	if (sizePx <= 0 || !Available()) {
		return;
	}
	// Sort by atlas page so each page is decoded once, instead of evicting and
	// re-decoding as the caller's order hops between pages.
	auto pointers = std::vector<EmojiPtr>();
	pointers.reserve(emoji.size());
	for (const auto &one : emoji) {
		if (const auto found = Resolve(one)) {
			pointers.push_back(found);
		}
	}
	std::sort(
		begin(pointers),
		end(pointers),
		[](EmojiPtr a, EmojiPtr b) { return a->sprite() < b->sprite(); });
	for (const auto one : pointers) {
		Lookup(one, sizePx);
	}
	// The scaled pixmaps are cached now, so the decoded pages are just held memory.
	Ui::Emoji::ReleasePages();
}

void ClearCache() {
	Pixmaps.clear();
	Ui::Emoji::ReleasePages();
}

} // namespace TeleMatrix::Emoji

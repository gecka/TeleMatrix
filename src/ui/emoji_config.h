// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from desktop-app/lib_ui (ui/emoji_config.h), which is:
//   Copyright the Desktop App Toolkit authors.
//   https://github.com/desktop-app/legal/blob/master/LEGAL
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

// Sprite-sheet emoji, ported from Telegram Desktop.
//
// `class One` and the lookup helpers are upstream verbatim — the codegen output in
// build/generated/emoji/emoji.cpp constructs `One` directly and is a friend of it, so
// its shape is fixed. What is deliberately NOT ported is everything built to serve
// tdesktop's downloadable emoji sets and its inline-text rendering: the `Instance`
// per-size pre-scaled atlas cache, the on-disk cache with its SHA-256 trailer,
// set switching, and config.json validation. Dropping them takes OpenSSL, crl::async,
// base::parse and rpl out of the dependency list; we ship exactly one set (id 0).
//
// Upstream also exposes Draw() as a two-fixed-sizes dispatcher that calls
// Unexpected() — i.e. crashes — on any other size. Our call sites need about seven
// different sizes, so scaling happens per emoji instead; see ui/emoji_sprites.h.

#include "ui/emoji_compat.h"

#include "emoji.h"

#include <QtGui/QImage>

namespace Ui {
namespace Emoji {

// Builds the emoji tables and works out how many atlas pages there are. Must run once,
// on the main thread, before anything else here. Cheap: it touches no image data.
void Init();

// True once at least one atlas page has decoded successfully. False means the WebP
// image plugin is missing (see resources/emoji/README.md) or the resources are absent,
// and callers should fall back to drawing the emoji as text.
[[nodiscard]] bool Available();

[[nodiscard]] int SpritesCount();

// Rows actually present in atlas page `index` — the last page is short.
[[nodiscard]] int RowsCount(int index);

// One emoji, scaled from the 72px master to `sizePx` square. Null QImage if the emoji
// has no cell or the atlases are unavailable. Decodes the containing page on demand.
[[nodiscard]] QImage Cell(EmojiPtr emoji, int sizePx);

// Drop decoded atlas pages. The per-emoji pixmaps in ui/emoji_sprites.h are unaffected,
// so anything already drawn stays cached.
void ReleasePages();

class One {
	struct CreationTag {
	};

public:
	One(One &&other) = default;
	One(
		const QString &id,
		EmojiPtr original,
		uint32 index,
		bool hasPostfix,
		bool colorizable,
		const CreationTag &);

	[[nodiscard]] QString id() const {
		return _id;
	}
	[[nodiscard]] QString text() const {
		return hasPostfix() ? (_id + QChar(kPostfix)) : _id;
	}

	[[nodiscard]] bool colored() const {
		return (_original != nullptr);
	}
	[[nodiscard]] EmojiPtr original() const {
		return _original ? _original : this;
	}
	[[nodiscard]] QString nonColoredId() const {
		return original()->id();
	}

	[[nodiscard]] bool hasPostfix() const {
		return _hasPostfix;
	}

	[[nodiscard]] bool hasVariants() const {
		return _colorizable || colored();
	}
	[[nodiscard]] int variantsCount() const;
	[[nodiscard]] int variantIndex(EmojiPtr variant) const;
	[[nodiscard]] EmojiPtr variant(int index) const;

	[[nodiscard]] int index() const {
		return _index;
	}
	// The atlas coordinates are packed into the index: 512 emoji per page,
	// 32 per row, 16 rows. Must agree with the generator's geometry constants.
	[[nodiscard]] int sprite() const {
		return int(_index >> 9);
	}
	[[nodiscard]] int row() const {
		return int((_index >> 5) & 0x0FU);
	}
	[[nodiscard]] int column() const {
		return int(_index & 0x1FU);
	}

private:
	const QString _id;
	const EmojiPtr _original = nullptr;
	const uint32 _index = 0;
	const bool _hasPostfix = false;
	const bool _colorizable = false;

	friend void internal::Init();

};

[[nodiscard]] inline EmojiPtr Find(const QChar *start, const QChar *end, int *outLength = nullptr) {
	return internal::Find(start, end, outLength);
}

[[nodiscard]] inline EmojiPtr Find(QStringView text, int *outLength = nullptr) {
	return Find(text.begin(), text.end(), outLength);
}

[[nodiscard]] inline int ColorIndexFromCode(uint32 code) {
	switch (code) {
	case 0xD83CDFFBU: return 1;
	case 0xD83CDFFCU: return 2;
	case 0xD83CDFFDU: return 3;
	case 0xD83CDFFEU: return 4;
	case 0xD83CDFFFU: return 5;
	}
	return 0;
}

} // namespace Emoji
} // namespace Ui

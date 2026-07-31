// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

// Sprite emoji inside running text, without a text engine.
//
// Qt shapes emoji with whatever colour font the host happens to have — Segoe UI Emoji on
// Windows, nothing at all on a Linux box whose fontconfig has never heard of our bundled
// Open Sans. Neither matches the atlas artwork the picker and the reaction pills draw. The
// fix is to keep Qt doing the layout and take over only the emoji:
//
//   1. Prepare() returns a *display* string in which each emoji's UTF-16 code units are
//      replaced by the same number of U+00A0. Same length, so every index the caller
//      already holds — format ranges, link ranges, selection offsets, xToCursor results —
//      keeps pointing at the same character. Nothing needs remapping.
//   2. SpacingFormats() widens the first placeholder of each emoji to the sprite slot and
//      collapses the rest to zero, through QTextCharFormat letter spacing. Qt then wraps,
//      measures and hit-tests as if the emoji were one wide blank glyph.
//   3. DrawSprites() blits the sprites over those gaps after QTextLayout::draw().
//
// U+00A0 specifically: it is blank in every text font, and QTextLine exempts it from
// trailing-whitespace trimming (its decomposition is <noBreak> 0020), so an emoji at the
// end of a wrapped line keeps its reserved width. A plain space would be trimmed and the
// sprite would hang outside the measured text. It is also non-breaking, so layouts that
// hold long emoji runs need QTextOption::WrapAtWordBoundaryOrAnywhere to wrap them.
// All four behaviours are pinned by tests/tst_emoji_text.cpp.
//
// Everything degrades to today's rendering when the text has no atlas emoji or the
// atlases are unavailable: Scan() returns empty and every entry point falls through to
// the plain QPainter/QTextLayout path.
//
// Main thread only (it draws through the QPixmap cache in ui/emoji_sprites.h).

#include <QtCore/QList>
#include <QtCore/QString>
#include <QtGui/QTextLayout>

#include "ui/emoji_config.h"

class QFont;
class QPainter;
class QTextDocument;

namespace TeleMatrix::EmojiText {

// One emoji occurrence. `position` and `length` are UTF-16 indices valid in BOTH the
// source string and the display string — the substitution is one code unit for one.
struct Entry {
    int position = 0;
    int length = 0;
    EmojiPtr emoji = nullptr;
};

// `slot` is the advance reserved in the layout, `glyph` the sprite drawn inside it; they
// differ because atlas cells are edge-to-edge artwork, so the glyph needs its own padding
// (the same reason st::reactionInlineEmoji is smaller than st::reactionInlineSize).
// `baselineTop` is where the sprite's top goes relative to the text baseline.
struct Metrics {
    int slot = 0;
    int glyph = 0;
    int baselineTop = 0;
    qreal nbspAdvance = 0.;
};
[[nodiscard]] Metrics MetricsFor(const QFont &font, int slotPx, int glyphPx);

// The same, memoised by font. MetricsFor runs a probe layout to learn what the shaper
// does with a placeholder, which is too much to repeat per painted row.
[[nodiscard]] const Metrics &CachedMetricsFor(
    const QFont &font,
    int slotPx,
    int glyphPx);

[[nodiscard]] QList<Entry> Scan(QStringView text);
[[nodiscard]] bool HasEmoji(QStringView text);

// Fills `display` and `entries` and returns true when there is anything to draw. Returns
// false and touches neither output otherwise, so callers keep using the source string.
bool Prepare(const QString &source, QString *display, QList<Entry> *entries);

// Append these LAST to the layout's format list: QTextEngine merges overlapping ranges in
// order, so a spacing range has to come after the bold/mono/link ranges to pin the font
// and keep the reserved width the same inside <b> and <pre>.
[[nodiscard]] QList<QTextLayout::FormatRange> SpacingFormats(
    const QList<Entry> &entries,
    const QFont &baseFont,
    const Metrics &metrics);

// Call after QTextLayout::draw() with the same origin, so selection backgrounds and link
// underlines end up behind the sprites.
void DrawSprites(
    QPainter &p,
    const QTextLayout &layout,
    const QList<Entry> &entries,
    QPointF origin,
    const Metrics &metrics);

// Nearest position that is not inside an emoji. Every placeholder is a legal cursor stop,
// unlike the surrogate pairs it replaced, so hit-test results have to be snapped before
// they are used to slice the source string — otherwise a drag-select can copy half a
// surrogate pair. Negative sentinels pass through untouched.
[[nodiscard]] int SnapCursor(const QList<Entry> &entries, int position);

// Single-line helpers for the QPainter::drawText surfaces (rooms list, search results,
// pinned bar, and so on). The Draw* pair uses the painter's current font. Both return the
// horizontal advance of what was drawn.
[[nodiscard]] int Width(const QString &text, const QFont &font, const Metrics &metrics);
// `mode` is honoured only when the text has no emoji (ElideMiddle over sprite slots is
// not worth the complexity); emoji-bearing text always elides on the right.
[[nodiscard]] QString Elide(
    const QString &text,
    const QFont &font,
    const Metrics &metrics,
    int availableWidth,
    Qt::TextElideMode mode = Qt::ElideRight);
int DrawLine(
    QPainter &p,
    int x,
    int baseline,
    const QString &text,
    const Metrics &metrics);
int DrawElided(
    QPainter &p,
    int x,
    int baseline,
    int availableWidth,
    const QString &text,
    const Metrics &metrics);

// The word-wrapping rect draws — `p.drawText(rect, Qt::TextWordWrap | align, text)`.
// WrappedHeight is the QFontMetrics::boundingRect(...).height() those call sites pair
// with, measured with emoji slots instead of font glyphs.
[[nodiscard]] int WrappedHeight(
    const QString &text,
    const QFont &font,
    const Metrics &metrics,
    int availableWidth);
void DrawWrapped(
    QPainter &p,
    const QRect &rect,
    Qt::Alignment alignment,
    const QString &text,
    const Metrics &metrics);

// ── QTextEdit emoji objects ──
// A QTextEdit gets sprite emoji a different way: each one becomes an inline image whose
// name is an `emoji://<index>/<w>x<h>` URL (see ui/widgets/emoji_objects.h). The URL
// scheme and the read-back live here, in the core library, because the HTML serializer
// and its tests need them and cannot depend on app-side style constants.

[[nodiscard]] QString EmojiUrl(EmojiPtr emoji, int boxWidth, int boxHeight);
[[nodiscard]] bool IsEmojiUrl(const QString &name);
[[nodiscard]] EmojiPtr EmojiFromUrl(const QString &name);

// QTextDocument::toPlainText() with emoji objects expanded back to their characters.
// Fast-pathed: a document with no object-replacement character returns Qt's own result.
[[nodiscard]] QString DocumentText(const QTextDocument *doc);

} // namespace TeleMatrix::EmojiText

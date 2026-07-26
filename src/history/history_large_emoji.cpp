// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_large_emoji.h"

#include <QFontMetrics>

namespace TeleMatrix {

namespace {

/// Check if a codepoint is a keycap base (# * 0-9).
/// These are only emoji when followed by VS16 + U+20E3 (keycap sequence).
/// They must NOT be treated as standalone emoji.
bool isKeycapBase(char32_t cp) {
    return cp == 0x23 || cp == 0x2A || (cp >= 0x30 && cp <= 0x39);
}

/// Check if a codepoint is in an emoji range.
/// Covers the main Unicode emoji blocks.
/// NOTE: keycap bases (0-9, #, *) are excluded — handled separately.
bool isEmojiCodepoint(char32_t cp) {
    // Misc symbols and dingbats
    if (cp == 0x00A9 || cp == 0x00AE) return true; // (C) (R)
    if (cp == 0x203C || cp == 0x2049) return true;
    if (cp >= 0x2122 && cp <= 0x2199) return true;
    if (cp >= 0x2300 && cp <= 0x23FF) return true; // Misc Technical
    if (cp >= 0x2460 && cp <= 0x24FF) return true; // Enclosed Alphanumerics
    if (cp >= 0x25A0 && cp <= 0x27BF) return true; // Geometric, Misc Symbols, Dingbats
    if (cp >= 0x2934 && cp <= 0x2935) return true;
    if (cp >= 0x2B05 && cp <= 0x2B07) return true;
    if (cp >= 0x2B1B && cp <= 0x2B1C) return true;
    if (cp == 0x2B50 || cp == 0x2B55) return true;
    if (cp == 0x3030 || cp == 0x303D || cp == 0x3297 || cp == 0x3299) return true;
    // Emoticons
    if (cp >= 0x1F600 && cp <= 0x1F64F) return true;
    // Misc Symbols and Pictographs
    if (cp >= 0x1F300 && cp <= 0x1F5FF) return true;
    // Transport and Map Symbols
    if (cp >= 0x1F680 && cp <= 0x1F6FF) return true;
    // Supplemental Symbols and Pictographs
    if (cp >= 0x1F900 && cp <= 0x1F9FF) return true;
    // Symbols and Pictographs Extended-A
    if (cp >= 0x1FA00 && cp <= 0x1FA6F) return true;
    if (cp >= 0x1FA70 && cp <= 0x1FAFF) return true;
    // Regional indicator symbols (flags)
    if (cp >= 0x1F1E0 && cp <= 0x1F1FF) return true;
    // Tags block (used in flag sequences)
    if (cp >= 0xE0020 && cp <= 0xE007F) return true;
    // CJK/enclosed ideographic supplement (some emoji)
    if (cp >= 0x1F200 && cp <= 0x1F2FF) return true;
    // Playing cards
    if (cp == 0x1F0CF) return true;
    // Mahjong
    if (cp == 0x1F004) return true;
    // Skin tone modifiers
    if (cp >= 0x1F3FB && cp <= 0x1F3FF) return true;
    // Various isolated emoji codepoints
    if (cp == 0x200D) return false; // ZWJ itself — handled as joiner, not emoji
    if (cp == 0xFE0F) return false; // variation selector — handled as modifier
    if (cp == 0x20E3) return false; // combining enclosing keycap — handled as modifier
    // Additional ranges
    if (cp >= 0x2600 && cp <= 0x26FF) return true; // Misc symbols (sun, cloud, etc.)
    if (cp >= 0x2700 && cp <= 0x27BF) return true; // Dingbats
    if (cp >= 0xFE00 && cp <= 0xFE0F) return false; // variation selectors
    return false;
}

/// Check if a codepoint is a variation selector (VS15/VS16).
bool isVariationSelector(char32_t cp) {
    return cp == 0xFE0E || cp == 0xFE0F;
}

/// Check if a codepoint is a Zero Width Joiner.
bool isZWJ(char32_t cp) {
    return cp == 0x200D;
}

/// Check if a codepoint is a skin tone modifier (Fitzpatrick).
bool isSkinToneModifier(char32_t cp) {
    return cp >= 0x1F3FB && cp <= 0x1F3FF;
}

/// Check if a codepoint is a combining enclosing keycap.
bool isKeycapCombining(char32_t cp) {
    return cp == 0x20E3;
}

/// Check if codepoint is a regional indicator symbol (used in flag sequences).
bool isRegionalIndicator(char32_t cp) {
    return cp >= 0x1F1E0 && cp <= 0x1F1FF;
}

/// Check if codepoint is a tag character (E0020-E007F) used in flag sequences.
bool isTagCharacter(char32_t cp) {
    return cp >= 0xE0020 && cp <= 0xE007F;
}

/// Read one Unicode codepoint from a QString at position `i`, advancing `i`.
char32_t readCodepoint(const QString &s, int &i) {
    const auto ch = s.at(i);
    if (ch.isHighSurrogate() && (i + 1) < s.size()) {
        const auto lo = s.at(i + 1);
        if (lo.isLowSurrogate()) {
            i += 2;
            return QChar::surrogateToUcs4(ch, lo);
        }
    }
    ++i;
    return ch.unicode();
}

/// Try to parse one complete emoji sequence starting at position `i`.
/// Returns the emoji as a QString, or empty if not an emoji.
/// Advances `i` past the consumed characters.
QString parseOneEmoji(const QString &s, int &i) {
    const int start = i;

    // Read the base codepoint.
    const auto base = readCodepoint(s, i);

    // Keycap bases (0-9, #, *) are only emoji as part of a keycap sequence:
    // base + optional VS16 (U+FE0F) + combining enclosing keycap (U+20E3).
    if (isKeycapBase(base)) {
        // Must be followed by the keycap sequence to be emoji.
        bool foundKeycap = false;
        const int afterBase = i;
        if (i < s.size()) {
            const auto cp1 = readCodepoint(s, i);
            if (cp1 == 0xFE0F && i < s.size()) {
                // VS16 — now need U+20E3.
                const auto cp2 = readCodepoint(s, i);
                if (cp2 == 0x20E3) {
                    foundKeycap = true;
                }
            } else if (cp1 == 0x20E3) {
                // Direct keycap without VS16.
                foundKeycap = true;
            }
        }
        if (!foundKeycap) {
            i = start;
            return {};
        }
        // Keycap sequence consumed — return it.
        return s.mid(start, i - start);
    }

    if (!isEmojiCodepoint(base)) {
        i = start;
        return {};
    }

    // Consume variation selectors, skin tone modifiers, keycap combining,
    // ZWJ sequences, and tag sequences.
    while (i < s.size()) {
        const int before = i;
        const auto cp = readCodepoint(s, i);

        if (isVariationSelector(cp)) {
            // Consumed — continue to check for keycap or ZWJ.
            continue;
        }
        if (isSkinToneModifier(cp)) {
            // Consumed — continue.
            continue;
        }
        if (isKeycapCombining(cp)) {
            // Consumed — this completes a keycap sequence.
            continue;
        }
        if (isTagCharacter(cp)) {
            // Tag sequence (e.g., flag subdivisions). Consume all tags.
            while (i < s.size()) {
                const int tagBefore = i;
                const auto tagCp = readCodepoint(s, i);
                if (!isTagCharacter(tagCp) && tagCp != 0xE007F) {
                    // Cancel tag (0xE007F) is the terminator.
                    i = tagBefore;
                    break;
                }
                if (tagCp == 0xE007F) {
                    break; // End of tag sequence.
                }
            }
            continue;
        }
        if (isRegionalIndicator(cp) && isRegionalIndicator(base)) {
            // Second regional indicator completes a flag pair (e.g. 🇬🇷).
            continue;
        }
        if (isZWJ(cp)) {
            // ZWJ sequence: must be followed by another emoji codepoint.
            if (i < s.size()) {
                const int nextStart = i;
                const auto next = readCodepoint(s, i);
                if (isEmojiCodepoint(next)) {
                    // Consumed the ZWJ + next emoji base. Continue loop
                    // to consume its modifiers too.
                    continue;
                }
                // Not a valid ZWJ sequence — back up past the ZWJ.
                i = nextStart;
            }
            // ZWJ at end of string with nothing following — back up.
            i = before;
            break;
        }

        // Not part of this emoji sequence — back up.
        i = before;
        break;
    }

    return s.mid(start, i - start);
}

/// Large emoji font (cached).
const QFont &largeEmojiFont() {
    static QFont font;
    static bool initialized = false;
    if (!initialized) {
        font = QFont();
        font.setPixelSize(kLargeEmojiSize);
        initialized = true;
    }
    return font;
}

} // namespace

IsolatedEmoji detectIsolatedEmoji(const QString &body) {
    IsolatedEmoji result;
    if (body.isEmpty()) {
        return result;
    }

    int i = 0;
    const int len = body.size();

    while (i < len && result.items.size() < 3) {
        // Skip whitespace.
        while (i < len && body.at(i).isSpace()) {
            ++i;
        }
        if (i >= len) {
            break;
        }

        const auto emoji = parseOneEmoji(body, i);
        if (emoji.isEmpty()) {
            // Non-emoji character found — not isolated emoji.
            return IsolatedEmoji{};
        }
        result.items.append(emoji);
    }

    // Skip trailing whitespace.
    while (i < len && body.at(i).isSpace()) {
        ++i;
    }

    // Must have consumed the entire string and have 1-3 emoji.
    if (i == len && result.items.size() >= 1 && result.items.size() <= 3) {
        result.valid = true;
    }
    return result;
}

bool resolvesToEmojiSticker(const QString &/*emoji*/) {
    // Stub: always returns false. Seam for future emoji sticker support.
    return false;
}

bool shouldRenderLargeEmoji(const QString &body, bool largeEmojiEnabled) {
    if (!largeEmojiEnabled) {
        return false;
    }
    const auto isolated = detectIsolatedEmoji(body);
    if (!isolated.valid) {
        return false;
    }
    // For single emoji, check if it resolves to an emoji sticker.
    if (isolated.items.size() == 1 && resolvesToEmojiSticker(isolated.items.first())) {
        return false;
    }
    return true;
}

/// Each emoji occupies a cell of (largeEmojiSize + 2*largeEmojiOutline) = 38px.
/// The visual gap between cells is (largeEmojiSkip - 2*largeEmojiOutline) = 2px.
static constexpr int kCellSize = kLargeEmojiSize + 2 * kLargeEmojiOutline; // 38
static constexpr int kVisualSkip = kLargeEmojiSkip - 2 * kLargeEmojiOutline; // 2

int largeEmojiWidth(const IsolatedEmoji &emoji) {
    if (!emoji.valid || emoji.items.isEmpty()) {
        return 0;
    }
    const auto count = emoji.items.size();
    return count * kCellSize + (count - 1) * kVisualSkip;
}

int largeEmojiHeight() {
    return kCellSize;
}

void paintLargeEmoji(
    QPainter &p,
    const IsolatedEmoji &emoji,
    int x,
    int y,
    int availableWidth)
{
    if (!emoji.valid || emoji.items.isEmpty()) {
        return;
    }

    const auto &font = largeEmojiFont();
    static const QFontMetrics fm(font);

    const auto totalW = largeEmojiWidth(emoji);

    p.save();
    // Clip to the allocated area — Apple Color Emoji metrics can exceed
    // the cell size, so prevent overflow into adjacent messages.
    p.setClipRect(QRect(x, y, totalW, kCellSize));
    p.setFont(font);

    // Left-align within the content area.
    auto cellX = x;

    for (int i = 0; i < emoji.items.size(); ++i) {
        if (i > 0) {
            cellX += kVisualSkip;
        }

        // Center glyph within the 38px cell.
        const auto glyphW = fm.horizontalAdvance(emoji.items[i]);
        const auto offsetX = (kCellSize - glyphW) / 2;
        const auto offsetY = (kCellSize - fm.height()) / 2;
        const auto drawX = cellX + offsetX;
        const auto drawY = y + offsetY + fm.ascent();

        // Draw the emoji glyph. No white outline — Apple Color Emoji is a
        // bitmap font that ignores pen color, so outline would just produce
        // ghosted copies. A baked-outline approach would need pre-rendered sprites.
        p.drawText(drawX, drawY, emoji.items[i]);

        cellX += kCellSize;
    }

    p.restore();
}

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QString>
#include <QVector>
#include <QPainter>

namespace TeleMatrix {

/// Result of isolated emoji detection.
struct IsolatedEmoji {
    QVector<QString> items;  // 1-3 emoji strings
    bool valid = false;      // true if message is purely isolated emoji
};

/// Detect if a message body is purely isolated emoji (1-3 emoji).
/// Returns valid=true with the emoji list if so.
IsolatedEmoji detectIsolatedEmoji(const QString &body);

/// Returns true if this emoji resolves to an emoji sticker.
/// Stub: always returns false for now (seam for future emoji sticker support).
bool resolvesToEmojiSticker(const QString &emoji);

/// Check if a message should render as large emoji.
/// Requires: largeEmoji setting is true, body is isolated emoji,
/// and for single emoji: not an emoji sticker.
bool shouldRenderLargeEmoji(const QString &body, bool largeEmojiEnabled);

/// Metrics for large static emoji.
constexpr int kLargeEmojiSize = 36;     // px, glyph size for oversized single emoji
constexpr int kLargeEmojiOutline = 1;   // px, outline width around the glyph
constexpr int kLargeEmojiSkip = 4;      // px, spacing between adjacent emoji

/// Calculate the width needed for large emoji rendering.
int largeEmojiWidth(const IsolatedEmoji &emoji);

/// Calculate the height needed for large emoji rendering.
int largeEmojiHeight();

/// Paint large emoji at the given position.
/// The emoji are centered horizontally within availableWidth.
void paintLargeEmoji(
    QPainter &p,
    const IsolatedEmoji &emoji,
    int x,
    int y,
    int availableWidth);

} // namespace TeleMatrix

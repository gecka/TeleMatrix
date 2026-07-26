// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

// Circular avatar placeholder painter.
// Uses vertical gradient, 2-char initials, (id % 7) + remap color selection.
#pragma once

#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QString>
#include <QColor>
#include <QTextOption>

#include "styles/style_constants.h"
#include "ui/painter.h"

namespace Ui {

class EmptyUserpic {
public:
    struct BgColors {
        QColor color1; // top of gradient
        QColor color2; // bottom of gradient
    };

    EmptyUserpic() = default;

    /// Color index: id % 7, then remap through {0,7,4,1,6,3,5}.
    /// For Matrix IDs (strings), we use a CRC-like hash of the string.
    static uint8_t colorIndexForId(const QString &id) {
        // Color hashing sums char codes (for Matrix ID compatibility), then
        // applies mod-7 + remap to get the correct palette index.
        uint32_t hash = 0;
        for (const auto &ch : id) {
            hash += ch.unicode();
        }
        return hash % 7;
    }

    /// Map color index (0-6) to palette index (0-7) via the remap table.
    static uint8_t colorIndexToPaletteIndex(uint8_t colorIndex) {
        static constexpr int8_t map[] = { 0, 7, 4, 1, 6, 3, 5 };
        return map[colorIndex % 7];
    }

    /// Return the gradient color pair for a given color index.
    static BgColors userpicColor(uint8_t colorIndex) {
        static const BgColors colors[] = {
            { st::peerUserpicBg1, st::peerUserpicBg1_2 },
            { st::peerUserpicBg2, st::peerUserpicBg2_2 },
            { st::peerUserpicBg3, st::peerUserpicBg3_2 },
            { st::peerUserpicBg4, st::peerUserpicBg4_2 },
            { st::peerUserpicBg5, st::peerUserpicBg5_2 },
            { st::peerUserpicBg6, st::peerUserpicBg6_2 },
            { st::peerUserpicBg7, st::peerUserpicBg7_2 },
            { st::peerUserpicBg8, st::peerUserpicBg8_2 },
        };
        return colors[colorIndexToPaletteIndex(colorIndex)];
    }

    /// Get the gradient colors for a given ID string.
    static BgColors colorsForId(const QString &id) {
        return userpicColor(colorIndexForId(id));
    }

    /// Legacy single-color accessor (returns the top gradient color).
    /// Used by code that only needs a flat color (e.g. dialogs list).
    static QColor colorForName(const QString &id) {
        return colorsForId(id).color1;
    }

    /// Extract up to 2 initials from a display name.
    /// Takes first letter of first word + first letter of next word (after space).
    /// Hyphens are lower priority than spaces.
    static QString extractInitials(const QString &name) {
        if (name.isEmpty()) return {};

        QString result;
        bool afterSeparator = true;
        bool foundFirst = false;

        for (int i = 0; i < name.size(); ++i) {
            const auto ch = name.at(i);

            // Skip emoji (surrogate pairs).
            if (ch.isHighSurrogate()) {
                ++i; // skip low surrogate
                afterSeparator = false;
                continue;
            }

            if (ch == u' ' || ch == u'-') {
                afterSeparator = true;
                continue;
            }

            if (afterSeparator && (ch.isLetter() || ch.isDigit())) {
                if (!foundFirst) {
                    result += ch.toUpper();
                    foundFirst = true;
                } else if (result.size() < 2) {
                    result += ch.toUpper();
                    break; // got two initials
                }
                afterSeparator = false;
            } else if (foundFirst) {
                afterSeparator = false;
            }
            // Leading punctuation before the first initial (e.g. the "@" of a
            // raw MXID like "@irc_...:server") must NOT consume the word-start
            // state, or extractInitials returns empty and the avatar shows a
            // blank circle. Keeping afterSeparator=true until the first letter
            // yields "I" — matching the profile popup, which uses the localpart.
        }
        return result;
    }

    /// Paint a circular avatar placeholder at (x, y) with the given size.
    static void paint(
        QPainter &p,
        const QString &id,
        const QString &displayName,
        int x, int y, int size)
    {
        const auto colors = colorsForId(id);
        const auto initials = extractInitials(displayName);

        // Font size formula: (size * 13) / 33
        const int fontSize = (size * 13) / 33;

        PainterHighQualityEnabler hq(p);

        // Vertical gradient background.
        QLinearGradient gradient(x, y, x, y + size);
        gradient.setColorAt(0.0, colors.color1);
        gradient.setColorAt(1.0, colors.color2);
        p.setBrush(gradient);
        p.setPen(Qt::NoPen);
        p.drawEllipse(x, y, size, size);

        // Draw initials.
        if (!initials.isEmpty()) {
            p.setFont(st::baseFont(fontSize, true));
            p.setBrush(Qt::NoBrush);
            p.setPen(st::historyPeerUserpicFg);
            p.drawText(
                QRect(x, y, size, size),
                initials,
                QTextOption(Qt::AlignCenter));
        }
    }

    /// Convenience overload: uses same string for both ID hash and display name.
    static void paint(
        QPainter &p,
        const QString &name,
        int x, int y, int size)
    {
        paint(p, name, name, x, y, size);
    }

    /// tdesktop's Saved Messages userpic: accent disc + outlined bookmark.
    /// The glyph geometry is copied verbatim from tdesktop's
    /// EmptyUserpic::PaintSavedMessagesInner (proportions, parity increments,
    /// cap/join styles), so it renders pixel-identically.
    static void paintSavedMessages(QPainter &p, int x, int y, int size) {
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        // tdesktop's disc gradient: historyPeerSavedMessagesBg aliases the
        // blue userpic pair, so reuse those theme-mapped tokens verbatim.
        QLinearGradient bg(x, y, x, y + size);
        bg.setColorAt(0.0, st::peerUserpicBg4);
        bg.setColorAt(1.0, st::peerUserpicBg4_2);
        p.setBrush(bg);
        p.drawEllipse(x, y, size, size);

        const auto thickness = qRound(size * 0.055);
        const auto increment = thickness % 2 + (size % 2);
        const auto width = qRound(size * 0.15) * 2 + increment;
        const auto height = qRound(size * 0.19) * 2 + increment;
        const auto add = qRound(size * 0.064);

        const auto left = x + (size - width) / 2;
        const auto top = y + (size - height) / 2;
        const auto right = left + width;
        const auto bottom = top + height;
        const auto middle = (left + right) / 2;
        const auto half = (top + bottom) / 2;

        p.setBrush(Qt::NoBrush);
        auto pen = QPen(QColor(st::historyPeerUserpicFg));
        pen.setWidthF(thickness);
        pen.setCapStyle(Qt::FlatCap);

        {
            // Straight top part of the bookmark.
            pen.setJoinStyle(Qt::RoundJoin);
            p.setPen(pen);
            QPainterPath path;
            path.moveTo(left, half);
            path.lineTo(left, top);
            path.lineTo(right, top);
            path.lineTo(right, half);
            p.drawPath(path);
        }
        {
            // Bottom part with the inward notch.
            pen.setJoinStyle(Qt::MiterJoin);
            p.setPen(pen);
            QPainterPath path;
            path.moveTo(left, half);
            path.lineTo(left, bottom);
            path.lineTo(middle, bottom - add);
            path.lineTo(right, bottom);
            path.lineTo(right, half);
            p.drawPath(path);
        }
    }
};

} // namespace Ui

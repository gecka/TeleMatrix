// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

// Compatibility header: provides Ui::Text::String stub.
//
// In lib_ui, Ui::Text::String is a rich text layout engine that handles
// text shaping, entity rendering, emoji, RTL, etc.
// This stub provides a minimal version for plain text rendering.
#pragma once

#include <QFont>
#include <QPainter>
#include <QFontMetrics>
#include <QString>
#include <QPoint>

namespace Ui {
namespace Text {

// Draw request parameters (matching lib_ui's API).
struct DrawRequest {
    QPoint position;
    int availableWidth = 0;
    int elisionLines = 0;
};

// Simplified Text::String: wraps a QString with a QFont for painting.
// The real lib_ui version does full text shaping, entity highlighting,
// emoji substitution, and selection rendering.
class String {
public:
    String() = default;

    // Construct from a font and plain text.
    String(const QFont &font, const QString &text)
        : _font(font)
        , _text(text)
        , _metrics(font)
    {}

    // Draw the text. Supports single-line elision.
    void draw(QPainter &p, const DrawRequest &req) const {
        p.setFont(_font);
        const auto elidedText = _metrics.elidedText(
            _text,
            Qt::ElideRight,
            req.availableWidth);
        p.drawText(req.position.x(),
                   req.position.y() + _metrics.ascent(),
                   elidedText);
    }

    // Width of the full (non-elided) text.
    [[nodiscard]] int maxWidth() const {
        return _metrics.horizontalAdvance(_text);
    }

    // Height of one line.
    [[nodiscard]] int minHeight() const {
        return _metrics.height();
    }

    [[nodiscard]] bool isEmpty() const {
        return _text.isEmpty();
    }

private:
    QFont _font;
    QString _text;
    QFontMetrics _metrics = QFontMetrics(QFont());
};

} // namespace Text
} // namespace Ui

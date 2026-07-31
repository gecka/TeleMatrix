// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "ui/text/emoji_text.h"

#include "ui/emoji_sprites.h"

#include <QtGui/QFont>
#include <QtGui/QFontMetrics>
#include <QtGui/QFontMetricsF>
#include <QtGui/QPainter>
#include <QtGui/QTextBlock>
#include <QtGui/QTextDocument>
#include <QtGui/QTextFragment>
#include <QtGui/QTextOption>

namespace TeleMatrix::EmojiText {
namespace {

constexpr auto kNbsp = QChar(0xA0);

// NoFontMerging is load-bearing, not a micro-optimisation. U+00A0's script is Common, so
// Qt itemises it with whatever script surrounds it and shapes it with that script's
// fallback font — inside Arabic text the placeholder came out 2.64px wide instead of the
// primary font's 3.75px, and since the tail units cancel against a measured advance, the
// reserved slot silently shrank. Disabling merging pins every placeholder to one engine.
[[nodiscard]] QFont PlaceholderFont(const QFont &font) {
    auto result = font;
    result.setStyleStrategy(QFont::NoFontMerging);
    return result;
}

// The advance the shaper will actually use, which is not QFontMetricsF's: hinting rounds
// it, and the tail units of a long ZWJ sequence cancel their own width against this
// number ten times over. A 0.2px design/shaped disagreement there is a 2px slot error.
[[nodiscard]] qreal ShapedNbspAdvance(const QFont &font) {
    auto layout = QTextLayout(QString(2, kNbsp), PlaceholderFont(font));
    layout.beginLayout();
    auto line = layout.createLine();
    line.setLineWidth(100000);
    layout.endLayout();
    return line.cursorToX(1) - line.cursorToX(0);
}

[[nodiscard]] QList<Entry> ScanImpl(QStringView text, bool stopAtFirst) {
    auto result = QList<Entry>();
    if (text.isEmpty() || !Emoji::Available()) {
        return result;
    }
    const auto begin = text.begin();
    const auto end = text.end();
    for (auto ch = begin; ch != end;) {
        auto length = 0;
        const auto emoji = Ui::Emoji::Find(ch, end, &length);
        if (emoji && length > 0) {
            result.append({ int(ch - begin), length, emoji });
            if (stopAtFirst) {
                return result;
            }
            ch += length;
            continue;
        }
        // Step by whole code points, so a lone high surrogate can never become the start
        // of the next match attempt.
        ch += (ch->isHighSurrogate() && (ch + 1) != end && (ch + 1)->isLowSurrogate())
            ? 2
            : 1;
    }
    return result;
}

} // namespace

Metrics MetricsFor(const QFont &font, int slotPx, int glyphPx) {
    const auto metrics = QFontMetricsF(font);
    const auto ascent = metrics.ascent();
    const auto descent = metrics.descent();
    return {
        .slot = slotPx,
        .glyph = glyphPx,
        // Centred on the ascent/descent box: the sprite's bottom lands near the
        // descender and its top near the ascender, which is how emoji sit next to text.
        // The one number worth re-tuning by eye.
        .baselineTop = qRound(-ascent + (ascent + descent - glyphPx) / 2.),
        .nbspAdvance = ShapedNbspAdvance(font),
    };
}

const Metrics &CachedMetricsFor(const QFont &font, int slotPx, int glyphPx) {
    static auto cache = QHash<QString, Metrics>();
    const auto key = QStringLiteral("%1|%2|%3")
        .arg(font.key())
        .arg(slotPx)
        .arg(glyphPx);
    auto i = cache.find(key);
    if (i == cache.end()) {
        i = cache.insert(key, MetricsFor(font, slotPx, glyphPx));
    }
    return *i;
}

QList<Entry> Scan(QStringView text) {
    return ScanImpl(text, false);
}

bool HasEmoji(QStringView text) {
    return !ScanImpl(text, true).isEmpty();
}

bool Prepare(const QString &source, QString *display, QList<Entry> *entries) {
    auto found = Scan(source);
    if (found.isEmpty()) {
        return false;
    }
    auto substituted = source;
    for (const auto &entry : found) {
        for (auto i = 0; i != entry.length; ++i) {
            substituted[entry.position + i] = kNbsp;
        }
    }
    *display = std::move(substituted);
    *entries = std::move(found);
    return true;
}

QList<QTextLayout::FormatRange> SpacingFormats(
        const QList<Entry> &entries,
        const QFont &baseFont,
        const Metrics &metrics) {
    auto result = QList<QTextLayout::FormatRange>();
    if (entries.isEmpty()) {
        return result;
    }

    auto head = QTextCharFormat();
    // FontPropertiesAll, not the default: a property the base font never set explicitly
    // would otherwise stay inherited from the bold or monospace range underneath, and the
    // reserved width has to be identical everywhere.
    head.setFont(PlaceholderFont(baseFont), QTextCharFormat::FontPropertiesAll);
    head.setFontLetterSpacingType(QFont::AbsoluteSpacing);
    head.setFontLetterSpacing(metrics.slot - metrics.nbspAdvance);
    auto tail = head;
    tail.setFontLetterSpacing(-metrics.nbspAdvance);

    result.reserve(entries.size() * 2);
    for (const auto &entry : entries) {
        result.append({ entry.position, 1, head });
        if (entry.length > 1) {
            result.append({ entry.position + 1, entry.length - 1, tail });
        }
    }
    return result;
}

void DrawSprites(
        QPainter &p,
        const QTextLayout &layout,
        const QList<Entry> &entries,
        QPointF origin,
        const Metrics &metrics) {
    const auto lines = layout.lineCount();
    if (entries.isEmpty() || lines <= 0) {
        return;
    }
    const auto inset = (metrics.slot - metrics.glyph) / 2;

    // Entries and lines are both position-ordered, so one shared walk beats a
    // lineForTextPosition() scan per emoji.
    auto index = 0;
    for (const auto &entry : entries) {
        while (index + 1 < lines) {
            const auto line = layout.lineAt(index);
            if (entry.position < line.textStart() + line.textLength()) {
                break;
            }
            ++index;
        }
        const auto line = layout.lineAt(index);
        if (entry.position < line.textStart()) {
            continue;
        }
        // Break-anywhere wrapping can split the zero-width tail onto the next line; the
        // head is what carries the slot, so the leading edge is always the right anchor.
        const auto left = qMin(
            line.cursorToX(entry.position),
            line.cursorToX(entry.position + entry.length));
        Emoji::Draw(
            p,
            entry.emoji->text(),
            metrics.glyph,
            qRound(origin.x() + left) + inset,
            qRound(origin.y() + line.y() + line.ascent()) + metrics.baselineTop);
    }
}

int SnapCursor(const QList<Entry> &entries, int position) {
    if (position <= 0) {
        return position;
    }
    for (const auto &entry : entries) {
        if (position <= entry.position) {
            break;
        }
        const auto after = entry.position + entry.length;
        if (position < after) {
            return (position - entry.position < after - position)
                ? entry.position
                : after;
        }
    }
    return position;
}

int Width(const QString &text, const QFont &font, const Metrics &metrics) {
    const auto entries = Scan(text);
    const auto fm = QFontMetrics(font);
    if (entries.isEmpty()) {
        return fm.horizontalAdvance(text);
    }
    auto width = 0;
    auto at = 0;
    for (const auto &entry : entries) {
        if (entry.position > at) {
            width += fm.horizontalAdvance(text.mid(at, entry.position - at));
        }
        width += metrics.slot;
        at = entry.position + entry.length;
    }
    if (at < text.size()) {
        width += fm.horizontalAdvance(text.mid(at));
    }
    return width;
}

QString Elide(
        const QString &text,
        const QFont &font,
        const Metrics &metrics,
        int availableWidth,
        Qt::TextElideMode mode) {
    if (availableWidth <= 0) {
        return QString();
    }
    const auto entries = Scan(text);
    const auto fm = QFontMetrics(font);
    if (entries.isEmpty()) {
        return fm.elidedText(text, mode, availableWidth);
    }
    if (Width(text, font, metrics) <= availableWidth) {
        return text;
    }

    const auto ellipsis = QStringLiteral("…");
    const auto budget = availableWidth - fm.horizontalAdvance(ellipsis);
    auto result = QString();
    auto width = 0;
    auto at = 0;
    for (auto i = 0; i != entries.size() + 1; ++i) {
        const auto runEnd = (i != entries.size())
            ? entries[i].position
            : int(text.size());
        if (runEnd > at) {
            const auto run = text.mid(at, runEnd - at);
            const auto runWidth = fm.horizontalAdvance(run);
            if (width + runWidth > budget) {
                // elidedText appends its own ellipsis.
                return result
                    + fm.elidedText(run, Qt::ElideRight, availableWidth - width);
            }
            result += run;
            width += runWidth;
            at = runEnd;
        }
        if (i == entries.size()) {
            break;
        }
        if (width + metrics.slot > budget) {
            return result + ellipsis;
        }
        result += text.mid(entries[i].position, entries[i].length);
        width += metrics.slot;
        at = entries[i].position + entries[i].length;
    }
    return result;
}

int DrawLine(
        QPainter &p,
        int x,
        int baseline,
        const QString &text,
        const Metrics &metrics) {
    const auto entries = Scan(text);
    if (entries.isEmpty()) {
        p.drawText(x, baseline, text);
        return QFontMetrics(p.font()).horizontalAdvance(text);
    }
    const auto fm = QFontMetrics(p.font());
    const auto inset = (metrics.slot - metrics.glyph) / 2;
    auto left = x;
    auto at = 0;
    const auto drawRun = [&](int from, int till) {
        if (till <= from) {
            return;
        }
        const auto run = text.mid(from, till - from);
        p.drawText(left, baseline, run);
        left += fm.horizontalAdvance(run);
    };
    for (const auto &entry : entries) {
        drawRun(at, entry.position);
        Emoji::Draw(
            p,
            entry.emoji->text(),
            metrics.glyph,
            left + inset,
            baseline + metrics.baselineTop);
        left += metrics.slot;
        at = entry.position + entry.length;
    }
    drawRun(at, int(text.size()));
    return left - x;
}

namespace {

// Shared by WrappedHeight and DrawWrapped so a measured height and a drawn one can never
// disagree. `lines` collects (line, y) when the caller is painting.
[[nodiscard]] int LayoutWrapped(
        const QString &text,
        const QFont &font,
        const Metrics &metrics,
        int availableWidth,
        QTextLayout *layout,
        QList<Entry> *entries) {
    auto display = QString();
    const auto hasEmoji = Prepare(text, &display, entries);
    layout->setText(hasEmoji ? display : text);
    layout->setFont(font);
    auto option = QTextOption();
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout->setTextOption(option);
    if (hasEmoji) {
        layout->setFormats(SpacingFormats(*entries, font, metrics));
    }
    auto height = 0;
    layout->beginLayout();
    while (true) {
        auto line = layout->createLine();
        if (!line.isValid()) {
            break;
        }
        line.setLineWidth(availableWidth);
        line.setPosition(QPointF(0, height));
        height += qRound(line.height());
    }
    layout->endLayout();
    return height;
}

} // namespace

int WrappedHeight(
        const QString &text,
        const QFont &font,
        const Metrics &metrics,
        int availableWidth) {
    if (text.isEmpty() || availableWidth <= 0) {
        return 0;
    }
    auto layout = QTextLayout();
    auto entries = QList<Entry>();
    return LayoutWrapped(text, font, metrics, availableWidth, &layout, &entries);
}

void DrawWrapped(
        QPainter &p,
        const QRect &rect,
        Qt::Alignment alignment,
        const QString &text,
        const Metrics &metrics) {
    if (text.isEmpty() || rect.width() <= 0) {
        return;
    }
    auto layout = QTextLayout();
    auto entries = QList<Entry>();
    const auto height = LayoutWrapped(
        text, p.font(), metrics, rect.width(), &layout, &entries);

    auto origin = QPointF(rect.x(), rect.y());
    if (alignment & Qt::AlignVCenter) {
        origin.setY(rect.y() + (rect.height() - height) / 2.);
    } else if (alignment & Qt::AlignBottom) {
        origin.setY(rect.y() + rect.height() - height);
    }
    // Horizontal alignment is QTextOption's job, but only AlignHCenter is ever asked
    // for here and setting it on the option keeps per-line centring correct.
    if (alignment & Qt::AlignHCenter) {
        auto option = layout.textOption();
        option.setAlignment(Qt::AlignHCenter);
        layout.setTextOption(option);
    }
    layout.draw(&p, origin);
    DrawSprites(p, layout, entries, origin, metrics);
}

int DrawElided(
        QPainter &p,
        int x,
        int baseline,
        int availableWidth,
        const QString &text,
        const Metrics &metrics) {
    return DrawLine(
        p,
        x,
        baseline,
        Elide(text, p.font(), metrics, availableWidth),
        metrics);
}

namespace {

const auto kEmojiUrlPrefix = QStringLiteral("emoji://");

} // namespace

QString EmojiUrl(EmojiPtr emoji, int boxWidth, int boxHeight) {
    return kEmojiUrlPrefix
        + QString::number(emoji->index())
        + u'/' + QString::number(boxWidth)
        + u'x' + QString::number(boxHeight);
}

bool IsEmojiUrl(const QString &name) {
    return name.startsWith(kEmojiUrlPrefix);
}

EmojiPtr EmojiFromUrl(const QString &name) {
    if (!IsEmojiUrl(name)) {
        return nullptr;
    }
    const auto rest = QStringView(name).mid(kEmojiUrlPrefix.size());
    const auto slash = rest.indexOf(u'/');
    auto ok = false;
    const auto index = ((slash < 0) ? rest : rest.left(slash)).toInt(&ok);
    if (!ok || index < 0 || index >= Ui::Emoji::internal::FullCount()) {
        return nullptr;
    }
    return Ui::Emoji::internal::ByIndex(index);
}

QString DocumentText(const QTextDocument *doc) {
    if (!doc) {
        return QString();
    }
    auto result = doc->toPlainText();
    if (!result.contains(QChar::ObjectReplacementCharacter)) {
        return result;
    }
    // Only now walk fragments. Adjacent identical emoji merge into a single fragment
    // holding several object-replacement characters, so this expands per character
    // rather than per fragment.
    result.clear();
    for (auto block = doc->begin(); block.isValid(); block = block.next()) {
        if (block != doc->begin()) {
            result += u'\n';
        }
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid()) {
                continue;
            }
            const auto format = fragment.charFormat();
            const auto emoji = format.isImageFormat()
                ? EmojiFromUrl(format.toImageFormat().name())
                : nullptr;
            const auto text = fragment.text();
            if (!emoji) {
                result += text;
                continue;
            }
            for (const auto ch : text) {
                if (ch == QChar::ObjectReplacementCharacter) {
                    result += emoji->text();
                } else {
                    result += ch;
                }
            }
        }
    }
    // Qt's toPlainText() normalises these; match it so callers comparing against a
    // previous extraction do not see spurious edits.
    result.replace(QChar::LineSeparator, u'\n');
    result.replace(QChar::ParagraphSeparator, u'\n');
    result.replace(QChar::Nbsp, u' ');
    return result;
}

} // namespace TeleMatrix::EmojiText

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include <QtGui/QFont>
#include <QtGui/QFontMetricsF>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtCore/QUrl>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>
#include <QtGui/QTextLayout>

#include "ui/emoji_config.h"
#include "ui/text/emoji_text.h"

// Sprite emoji inside running text work by replacing each emoji's UTF-16 code units with
// the same number of U+00A0, reserving the sprite's width through a QTextCharFormat, and
// blitting the sprite over the resulting gap. That trades a whole text engine for four
// undocumented QTextLayout behaviours. Each one is pinned here: if Qt changes any of them
// the emoji drift out of their slots, and this is the file that says so.

namespace {

// Spelled out rather than embedded in the string literals: an invisible U+00A0 in source
// is indistinguishable from a space, and a well-meaning editor would silently break every
// assertion in this file.
constexpr auto kNbsp = QChar(0xA0);

[[nodiscard]] QString nbspSample() {
    return QStringLiteral("A") + kNbsp + QStringLiteral("B");
}

[[nodiscard]] QFont testFont(int pixelSize = 13) {
    auto font = QFont();
    font.setPixelSize(pixelSize);
    return font;
}

[[nodiscard]] QTextCharFormat spacingFormat(const QFont &font, qreal spacing) {
    auto format = QTextCharFormat();
    format.setFont(font);
    format.setFontLetterSpacingType(QFont::AbsoluteSpacing);
    format.setFontLetterSpacing(spacing);
    return format;
}

// Lays out one unbroken line and returns the advance of the character at `index`.
[[nodiscard]] qreal advanceAt(
        const QString &text,
        const QFont &font,
        const QList<QTextLayout::FormatRange> &formats,
        int index) {
    auto layout = QTextLayout(text, font);
    if (!formats.isEmpty()) {
        layout.setFormats(formats);
    }
    layout.beginLayout();
    auto line = layout.createLine();
    line.setLineWidth(100000);
    layout.endLayout();
    return line.cursorToX(index + 1) - line.cursorToX(index);
}

[[nodiscard]] qreal naturalWidth(const QString &text, const QFont &font) {
    auto layout = QTextLayout(text, font);
    layout.beginLayout();
    auto line = layout.createLine();
    line.setLineWidth(100000);
    layout.endLayout();
    return line.naturalTextWidth();
}

// Width of the first line when `text` is forced to wrap. Trailing-whitespace trimming only
// happens at a wrap boundary, so this is the only way to observe it.
[[nodiscard]] qreal firstWrappedLineWidth(
        const QString &text,
        const QFont &font,
        int wrapWidth) {
    auto layout = QTextLayout(text, font);
    layout.beginLayout();
    auto line = layout.createLine();
    line.setLineWidth(wrapWidth);
    const auto result = line.naturalTextWidth();
    while (layout.createLine().isValid()) {
    }
    layout.endLayout();
    return result;
}

} // namespace

class TestEmojiText : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        Ui::Emoji::Init();
    }

    // S1. The load-bearing one. QTextEngine::shapeText applies AbsoluteSpacing to the
    // trailing glyph unconditionally, which is the only reason a one-character run can be
    // widened to an exact pixel value. If this fails, the placeholder scheme is dead and
    // the fallbacks are percentage spacing across the run, then setFontStretch, then
    // rendering message bodies through a QTextDocument.
    void absoluteSpacingWidensASingleCharacterRun() {
        const auto font = testFont();
        const auto text = nbspSample();
        const auto base = advanceAt(text, font, {}, 1);
        QVERIFY2(base > 0, "no NBSP advance to widen");

        constexpr auto kExtra = 7.0;
        const auto widened = advanceAt(
            text,
            font,
            { { 1, 1, spacingFormat(font, kExtra) } },
            1);
        QVERIFY2(
            qAbs(widened - (base + kExtra)) < 1.0,
            qPrintable(QStringLiteral("expected %1, got %2")
                .arg(base + kExtra).arg(widened)));
    }

    // The tail units of a multi-unit emoji are collapsed the same way, with negative
    // spacing. Only the head carries the slot, so the sprite sits in one place.
    void negativeAbsoluteSpacingCollapsesACharacterToZeroWidth() {
        const auto font = testFont();
        const auto text = nbspSample();
        const auto base = advanceAt(text, font, {}, 1);

        const auto collapsed = advanceAt(
            text,
            font,
            { { 1, 1, spacingFormat(font, -base) } },
            1);
        QVERIFY2(
            qAbs(collapsed) < 1.0,
            qPrintable(QStringLiteral("expected 0, got %1").arg(collapsed)));
    }

    // S2. Why the placeholder is NBSP and not a space. QTextLine::layout_helper skips
    // trailing-whitespace trimming for characters whose decompositionTag() is NoBreak, and
    // U+00A0 decomposes as <noBreak> 0020. A plain space would be trimmed at end of line,
    // silently dropping the emoji's width from the bubble-width computation.
    void trailingNbspKeepsItsWidthWhereATrailingSpaceDoesNot() {
        const auto font = testFont();
        QCOMPARE(kNbsp.decompositionTag(), QChar::NoBreak);

        // A long second word forces the wrap; the first line ends with the placeholder.
        const auto tail = QStringLiteral(" ") + QString(40, u'b');
        const auto wrapWidth = int(naturalWidth(QStringLiteral("aa"), font)) + 8;
        const auto nbspAdvance = QFontMetricsF(font).horizontalAdvance(kNbsp);

        const auto bare = firstWrappedLineWidth(
            QStringLiteral("a") + tail, font, wrapWidth);
        const auto withNbsp = firstWrappedLineWidth(
            QStringLiteral("a") + kNbsp + tail, font, wrapWidth);
        const auto withSpace = firstWrappedLineWidth(
            QStringLiteral("a ") + tail, font, wrapWidth);

        QVERIFY2(
            qAbs(withNbsp - (bare + nbspAdvance)) < 1.0,
            qPrintable(QStringLiteral("NBSP lost its width at a wrap: %1 vs %2")
                .arg(withNbsp).arg(bare + nbspAdvance)));
        QVERIFY2(
            qAbs(withSpace - bare) < 1.0,
            qPrintable(QStringLiteral("a trailing space was not trimmed: %1 vs %2")
                .arg(withSpace).arg(bare)));
    }

    // S3. NBSP is UAX#14 class GL, so a run of emoji is one unbreakable word. Wrapping only
    // survives because of the break-anywhere fallback, which every message-body layout
    // enables. A layout that forgets it overflows instead of wrapping — the second half of
    // this test is what layoutCaptionLines used to do.
    void longPlaceholderRunsWrapOnlyWithBreakAnywhere() {
        const auto font = testFont();
        const auto text = QString(200, QChar(0xA0));
        constexpr auto kWidth = 100;

        auto wrapping = QTextOption();
        wrapping.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        auto layout = QTextLayout(text, font);
        layout.setTextOption(wrapping);
        layout.beginLayout();
        auto lines = 0;
        while (true) {
            auto line = layout.createLine();
            if (!line.isValid()) {
                break;
            }
            line.setLineWidth(kWidth);
            QVERIFY(line.naturalTextWidth() <= kWidth + 1);
            ++lines;
        }
        layout.endLayout();
        QVERIFY2(lines > 1, "break-anywhere did not wrap an unbreakable run");

        auto strict = QTextOption();
        strict.setWrapMode(QTextOption::WordWrap);
        auto overflowing = QTextLayout(text, font);
        overflowing.setTextOption(strict);
        overflowing.beginLayout();
        auto line = overflowing.createLine();
        line.setLineWidth(kWidth);
        overflowing.endLayout();
        QVERIFY2(
            line.naturalTextWidth() > kWidth,
            "word-boundary wrapping unexpectedly broke the run");
    }

    // S4. Spacing ranges are appended after the bold/mono/link ranges so that
    // QTextEngine's format merge lets them win. They pin the font outright, because the
    // reserved width has to be the same whether or not the emoji sits inside <b> or <pre>.
    void aLaterFormatRangeOverridesTheFontOfAnEarlierOne() {
        const auto font = testFont();
        const auto text = nbspSample();
        const auto base = advanceAt(text, font, {}, 1);

        auto large = QTextCharFormat();
        large.setFont(testFont(26));
        const auto enlarged = advanceAt(text, font, { { 0, 3, large } }, 1);
        QVERIFY2(enlarged > base + 1.0, "the test font ignores pixel size");

        constexpr auto kExtra = 7.0;
        const auto pinned = advanceAt(
            text,
            font,
            { { 0, 3, large }, { 1, 1, spacingFormat(font, kExtra) } },
            1);
        QVERIFY2(
            qAbs(pinned - (base + kExtra)) < 1.0,
            qPrintable(QStringLiteral("expected the pinned font to win: %1 vs %2")
                .arg(base + kExtra).arg(pinned)));
    }

    // S5. Spacing is computed from QFontMetricsF but consumed by the shaper. They agree
    // closely enough that any residual shows as a fractional gap after the emoji, never as
    // a misplaced sprite (sprites are positioned from the real cursorToX).
    void metricsAgreeWithTheShapedAdvance() {
        for (const auto pixelSize : { 11, 13, 20 }) {
            auto font = testFont(pixelSize);
            const auto shaped = advanceAt(nbspSample(), font, {}, 1);
            const auto measured = QFontMetricsF(font).horizontalAdvance(QChar(0xA0));
            QVERIFY2(
                qAbs(shaped - measured) < 1.0,
                qPrintable(QStringLiteral("%1px: shaped %2, measured %3")
                    .arg(pixelSize).arg(shaped).arg(measured)));

            font.setBold(true);
            const auto shapedBold = advanceAt(nbspSample(), font, {}, 1);
            const auto measuredBold = QFontMetricsF(font).horizontalAdvance(QChar(0xA0));
            QVERIFY(qAbs(shapedBold - measuredBold) < 1.0);
        }
    }

    // S6. Message bodies keep their QTextLayout alive across frames with setCacheEnabled,
    // so positions have to be stable once shaped — the sprite pass reads cursorToX on
    // every paint.
    void cachedLayoutsReportStablePositions() {
        const auto font = testFont();
        const auto text = QStringLiteral("hi ") + kNbsp + QStringLiteral(" there");
        auto layout = QTextLayout(text, font);
        layout.setCacheEnabled(true);
        layout.setFormats({ { 3, 1, spacingFormat(font, 9.0) } });
        layout.beginLayout();
        auto line = layout.createLine();
        line.setLineWidth(100000);
        layout.endLayout();

        const auto first = line.cursorToX(4);
        const auto width = line.naturalTextWidth();
        auto image = QImage(200, 40, QImage::Format_ARGB32_Premultiplied);
        for (auto i = 0; i != 2; ++i) {
            image.fill(Qt::transparent);
            auto p = QPainter(&image);
            layout.draw(&p, QPointF(0, 0));
        }
        QCOMPARE(line.cursorToX(4), first);
        QCOMPARE(line.naturalTextWidth(), width);
    }

    // ── the module itself ──

    void scanFindsEmojiRunsAndLeavesPlainTextAlone() {
        using namespace TeleMatrix;
        QVERIFY(EmojiText::Scan(QStringLiteral("no emoji here")).isEmpty());
        QVERIFY(!EmojiText::HasEmoji(QStringLiteral("no emoji here")));

        const auto entries = EmojiText::Scan(QStringLiteral("a👍b🎉"));
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries[0].position, 1);
        QCOMPARE(entries[0].length, 2);
        QCOMPARE(entries[1].position, 4);
        QVERIFY(entries[0].emoji != nullptr);
    }

    // The presentation policy, such as it is. The generated table refuses to match
    // U+2122/U+00A9/U+00AE without a variation selector (codegen's PostfixRequired set),
    // so prose keeps its typography. Everything else the table does match becomes a
    // sprite — going stricter would render bare ❤ ✌ ☀ as font glyphs, which is the
    // blank-on-Linux bug this whole change exists to remove.
    void bareTypographicSymbolsStayText() {
        using namespace TeleMatrix;
        QVERIFY(EmojiText::Scan(QStringLiteral("Acme™ (c)© (r)®")).isEmpty());
        QVERIFY(!EmojiText::Scan(QStringLiteral("™️")).isEmpty());
    }

    // S8. The whole design rests on this: same length, same characters outside the
    // emoji. Any caller-held index stays valid without remapping.
    void preparedTextPreservesEveryIndex() {
        using namespace TeleMatrix;
        for (const auto &source : corpus()) {
            auto display = QString();
            auto entries = QList<EmojiText::Entry>();
            if (!EmojiText::Prepare(source, &display, &entries)) {
                continue;
            }
            QCOMPARE(display.size(), source.size());
            auto covered = QList<bool>();
            covered.fill(false, source.size());
            for (const auto &entry : entries) {
                for (auto i = 0; i != entry.length; ++i) {
                    QCOMPARE(display.at(entry.position + i), kNbsp);
                    covered[entry.position + i] = true;
                }
            }
            for (auto i = 0; i != source.size(); ++i) {
                if (!covered[i]) {
                    QCOMPARE(display.at(i), source.at(i));
                }
            }
        }
    }

    // The integration proof: after substitution and spacing, Qt reserves exactly the slot
    // for an emoji — including underneath a bold range, which is what a <b> body does.
    void spacingFormatsReserveExactlyTheSlot() {
        using namespace TeleMatrix;
        const auto font = testFont();
        const auto metrics = EmojiText::MetricsFor(font, 20, 18);

        for (const auto &source : corpus()) {
            auto display = QString();
            auto entries = QList<EmojiText::Entry>();
            if (!EmojiText::Prepare(source, &display, &entries)) {
                continue;
            }
            auto bold = QTextCharFormat();
            bold.setFontWeight(QFont::Bold);
            auto formats = QList<QTextLayout::FormatRange>{
                { 0, int(display.size()), bold } };
            formats += EmojiText::SpacingFormats(entries, font, metrics);

            auto layout = QTextLayout(display, font);
            layout.setFormats(formats);
            layout.beginLayout();
            auto line = layout.createLine();
            line.setLineWidth(100000);
            layout.endLayout();

            for (const auto &entry : entries) {
                // The invariant DrawSprites relies on: the two edges bracket a
                // slot-wide box. In RTL they arrive in the opposite order, which is
                // why the sprite is anchored to the smaller of the two.
                const auto one = line.cursorToX(entry.position);
                const auto other = line.cursorToX(entry.position + entry.length);
                const auto reserved = qMax(one, other) - qMin(one, other);
                QVERIFY2(
                    qAbs(reserved - metrics.slot) < 1.0,
                    qPrintable(QStringLiteral("%1: reserved %2, wanted %3")
                        .arg(source).arg(reserved).arg(metrics.slot)));
            }
        }
    }

    // S7. Every placeholder is a legal cursor stop, unlike the surrogate pairs it
    // replaced. Unsnapped, a drag-select could slice a lone surrogate into the clipboard.
    void snapCursorNeverLandsInsideAnEmoji() {
        using namespace TeleMatrix;
        for (const auto &source : corpus()) {
            const auto entries = EmojiText::Scan(source);
            for (auto position = 0; position <= source.size(); ++position) {
                const auto snapped = EmojiText::SnapCursor(entries, position);
                QVERIFY(snapped >= 0 && snapped <= source.size());
                for (const auto &entry : entries) {
                    QVERIFY2(
                        snapped <= entry.position
                            || snapped >= entry.position + entry.length,
                        qPrintable(QStringLiteral("%1: %2 landed inside an emoji")
                            .arg(source).arg(position)));
                }
                if (snapped > 0) {
                    QVERIFY(!source.at(snapped - 1).isHighSurrogate());
                }
                if (snapped < source.size()) {
                    QVERIFY(!source.at(snapped).isLowSurrogate());
                }
            }
        }
    }

    void snapCursorPassesSentinelsThrough() {
        const auto entries = TeleMatrix::EmojiText::Scan(QStringLiteral("a👍b"));
        QCOMPARE(TeleMatrix::EmojiText::SnapCursor(entries, -1), -1);
        QCOMPARE(TeleMatrix::EmojiText::SnapCursor(entries, 0), 0);
    }

    void widthAndElideCountTheSlotNotTheFontGlyph() {
        using namespace TeleMatrix;
        const auto font = testFont();
        const auto metrics = EmojiText::MetricsFor(font, 20, 18);
        const auto fm = QFontMetrics(font);

        const auto plain = QStringLiteral("ab");
        QCOMPARE(
            EmojiText::Width(plain, font, metrics),
            fm.horizontalAdvance(plain));
        QCOMPARE(
            EmojiText::Width(QStringLiteral("ab👍"), font, metrics),
            fm.horizontalAdvance(plain) + metrics.slot);

        const auto text = QStringLiteral("hello 👍 world");
        const auto full = EmojiText::Width(text, font, metrics);
        QCOMPARE(EmojiText::Elide(text, font, metrics, full), text);
        const auto elided = EmojiText::Elide(text, font, metrics, full / 2);
        QVERIFY(elided.size() < text.size());
        QVERIFY(EmojiText::Width(elided, font, metrics) <= full / 2);
    }

    // ── composer emoji objects ──

    void emojiUrlsRoundTrip() {
        using namespace TeleMatrix;
        const auto emoji = Ui::Emoji::Find(QStringLiteral("👍"));
        QVERIFY(emoji);
        const auto url = EmojiText::EmojiUrl(emoji, 20, 18);
        QVERIFY(EmojiText::IsEmojiUrl(url));
        QCOMPARE(EmojiText::EmojiFromUrl(url), emoji);

        QVERIFY(!EmojiText::IsEmojiUrl(QStringLiteral("https://example.com/a.png")));
        QVERIFY(!EmojiText::EmojiFromUrl(QStringLiteral("emoji://not-a-number")));
        QVERIFY(!EmojiText::EmojiFromUrl(QStringLiteral("emoji://99999999/20x18")));
    }

    // What the composer sends. The document holds object-replacement characters; if this
    // ever regresses, messages go out with U+FFFC where the emoji were.
    void documentTextExpandsEmojiObjects() {
        using namespace TeleMatrix;
        const auto emoji = Ui::Emoji::Find(QStringLiteral("👍"));
        QVERIFY(emoji);
        auto format = QTextImageFormat();
        format.setName(EmojiText::EmojiUrl(emoji, 20, 18));

        auto doc = QTextDocument();
        auto cursor = QTextCursor(&doc);
        cursor.insertText(QStringLiteral("a"));
        cursor.insertText(QString(QChar::ObjectReplacementCharacter), format);
        cursor.insertText(QStringLiteral("b"));
        QCOMPARE(
            EmojiText::DocumentText(&doc),
            QStringLiteral("a") + emoji->text() + QStringLiteral("b"));
    }

    // Qt merges adjacent fragments that share a format, so two identical emoji arrive as
    // one fragment holding two object-replacement characters. Expanding per fragment
    // instead of per character would silently drop one of them.
    void adjacentIdenticalEmojiBothExpand() {
        using namespace TeleMatrix;
        const auto emoji = Ui::Emoji::Find(QStringLiteral("😀"));
        QVERIFY(emoji);
        auto format = QTextImageFormat();
        format.setName(EmojiText::EmojiUrl(emoji, 20, 18));

        auto doc = QTextDocument();
        auto cursor = QTextCursor(&doc);
        cursor.insertText(QString(QChar::ObjectReplacementCharacter), format);
        cursor.insertText(QString(QChar::ObjectReplacementCharacter), format);
        QCOMPARE(EmojiText::DocumentText(&doc), emoji->text() + emoji->text());
    }

    void documentTextLeavesPlainDocumentsAlone() {
        auto doc = QTextDocument();
        doc.setPlainText(QStringLiteral("no objects here"));
        QCOMPARE(
            TeleMatrix::EmojiText::DocumentText(&doc),
            QStringLiteral("no objects here"));
    }

    // Reproduces the composer's "broken image" box: a QTextImageFormat whose resource
    // the document cannot resolve renders as Qt's missing-image icon. Documents that
    // were handed the pixmap through addResource() must render the real thing.
    void imageObjectsNeedAnAddedResource() {
        const auto url = QStringLiteral("emoji://0/20x18");
        auto pixmap = QPixmap(20, 18);
        pixmap.fill(Qt::red);

        const auto render = [&](bool registerResource) {
            auto doc = QTextDocument();
            doc.setDefaultFont(testFont());
            if (registerResource) {
                doc.addResource(
                    QTextDocument::ImageResource,
                    QUrl(url),
                    QVariant(pixmap));
            }
            auto format = QTextImageFormat();
            format.setName(url);
            format.setWidth(20);
            format.setHeight(18);
            auto cursor = QTextCursor(&doc);
            cursor.insertText(QString(QChar::ObjectReplacementCharacter), format);

            auto image = QImage(60, 40, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::white);
            auto p = QPainter(&image);
            doc.drawContents(&p);
            p.end();

            auto red = 0;
            for (auto y = 0; y != image.height(); ++y) {
                for (auto x = 0; x != image.width(); ++x) {
                    if (image.pixelColor(x, y) == QColor(Qt::red)) {
                        ++red;
                    }
                }
            }
            return red;
        };

        QVERIFY2(
            render(true) > 100,
            "an added ImageResource did not reach the layout");
        QCOMPARE(render(false), 0);
    }

private:
    [[nodiscard]] static QStringList corpus() {
        return {
            QStringLiteral("a👍b"),               // surrogate pair between plain chars
            QStringLiteral("👨‍👩‍👧‍👦"),                 // ZWJ sequence
            QStringLiteral("🇪🇸"),                 // regional indicator pair
            QStringLiteral("1️⃣"),                 // keycap
            QStringLiteral("👍🏽"),                 // skin tone modifier
            QStringLiteral("❤️"),                 // variation selector
            QStringLiteral("😀😀😀"),             // adjacent emoji
            QStringLiteral("hello 😀 world 🎉"),  // mixed
            QStringLiteral("مرحبا 👋 بالعالم"),   // RTL
        };
    }
};

QTEST_MAIN(TestEmojiText)
#include "tst_emoji_text.moc"

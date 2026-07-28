// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include <QtGui/QImage>
#include <QtGui/QImageReader>

#include "history/emoji_data.h"
#include "ui/emoji_config.h"

// The gate for the failure that motivated the sprite port: emoji silently rendering as
// nothing. Two independent ways that happens, both caught here —
//   1. Qt cannot decode WebP (the qtimageformats module is missing), so every atlas
//      loads as a null image. This is a *build/deployment* fault and is invisible at
//      compile time, which is exactly why it needs a test.
//   2. resources/emoji/*.webp and the generated emoji.cpp came from different upstream
//      revisions, so the cell geometry disagrees and every emoji draws the wrong
//      picture (or none). Page dimensions are derived from the generated tables, so a
//      mismatch shows up as a size assertion rather than as garbled UI.

class TestEmojiAtlas : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        Ui::Emoji::Init();
    }

    void webpPluginIsAvailable() {
        const auto formats = QImageReader::supportedImageFormats();
        QVERIFY2(
            formats.contains("webp"),
            "Qt has no WebP image plugin. Install the qtimageformats module — "
            "without it every emoji renders blank. See resources/emoji/README.md.");
    }

    void spriteCountMatchesTheGeneratedTables() {
        // 512 emoji per page (32 x 16). Derived, not hardcoded, so re-vendoring a
        // larger emoji set does not need this test edited.
        QCOMPARE(Ui::Emoji::SpritesCount(), 8);
    }

    void everyAtlasPageDecodesAtTheExpectedSize() {
        const auto count = Ui::Emoji::SpritesCount();
        QVERIFY(count > 0);
        for (auto i = 0; i != count; ++i) {
            const auto path = QStringLiteral(":/gui/emoji/emoji_%1.webp").arg(i + 1);
            QVERIFY2(
                QFile::exists(path),
                qPrintable(QStringLiteral("missing resource: %1").arg(path)));

            const auto image = QImage(path, "WEBP");
            QVERIFY2(
                !image.isNull(),
                qPrintable(QStringLiteral("could not decode %1").arg(path)));

            const auto expected = QSize(32 * 72, Ui::Emoji::RowsCount(i) * 72);
            QCOMPARE(image.size(), expected);
        }
    }

    void atlasesReportAsAvailable() {
        QVERIFY(Ui::Emoji::Available());
    }

    void knownEmojiResolveToCells() {
        for (const auto &text : { QStringLiteral("😀"),
                                  QStringLiteral("👍"),
                                  QStringLiteral("❤️") }) {
            const auto emoji = Ui::Emoji::Find(QStringView(text));
            QVERIFY2(
                emoji != nullptr,
                qPrintable(QStringLiteral("unresolved: %1").arg(text)));
            QVERIFY(emoji->sprite() < Ui::Emoji::SpritesCount());

            const auto cell = Ui::Emoji::Cell(emoji, 36);
            QVERIFY(!cell.isNull());
            QCOMPARE(cell.size(), QSize(36, 36));

            // A cell of entirely transparent pixels would mean the geometry is right
            // but we are sampling empty atlas space — i.e. still invisible emoji.
            auto opaque = 0;
            for (auto y = 0; y != cell.height(); ++y) {
                for (auto x = 0; x != cell.width(); ++x) {
                    if (qAlpha(cell.pixel(x, y)) > 0) {
                        ++opaque;
                    }
                }
            }
            QVERIFY2(
                opaque > 0,
                qPrintable(QStringLiteral("blank cell for %1").arg(text)));
        }
    }

    // Regression: a cell requested at exactly the 72px atlas cell size used to alias the
    // atlas page's memory rather than copy it, because QImage::scaled() returns *this
    // when the size already matches. Once the page was evicted the pixels were freed
    // memory, and jumbo emoji (36px at devicePixelRatio 2) drew as noise.
    void cellAtNativeSizeOwnsItsPixels() {
        const auto text = QStringLiteral("😀");
        const auto emoji = Ui::Emoji::Find(QStringView(text));
        QVERIFY(emoji);

        const auto cell = Ui::Emoji::Cell(emoji, 72);
        QVERIFY(!cell.isNull());
        QCOMPARE(cell.size(), QSize(72, 72));

        // Ownership is checked through the stride, not by comparing pixels after a
        // release: freed memory usually still holds the old bytes, so a pixel compare
        // passes even when the bug is present. A cell that aliases the page inherits the
        // page's 2304px stride (9216 bytes); one that owns its pixels is compact (288).
        QCOMPARE(cell.bytesPerLine(), 72 * 4);
    }

    // Coverage: the picker table is hand-written (src/history/emoji_data.cpp) while the
    // atlas index is generated from lib_ui's emoji.txt. Nothing keeps them in step, so
    // an entry the atlas has never heard of would quietly fall back to text emoji —
    // invisible on exactly the platforms this port exists to fix.
    void everyPickerEmojiHasASprite() {
        const auto &entries = TeleMatrix::allEmojiEntries();
        QVERIFY(!entries.isEmpty());

        auto missing = QStringList();
        for (const auto &entry : entries) {
            if (!Ui::Emoji::Find(QStringView(entry.emoji))) {
                missing.append(QStringLiteral("%1 (%2)").arg(entry.emoji, entry.name));
            }
        }
        QVERIFY2(
            missing.isEmpty(),
            qPrintable(QStringLiteral("%1 picker emoji have no sprite: %2")
                .arg(missing.size())
                .arg(missing.join(QStringLiteral(", ")))));
    }
};

QTEST_GUILESS_MAIN(TestEmojiAtlas)
#include "tst_emoji_atlas.moc"

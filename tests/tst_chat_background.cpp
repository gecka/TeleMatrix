// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>
#include <QDir>
#include <QFile>

#include "theme/chat_background.h"

using namespace TeleMatrix;
using Theme::CornerColors;

namespace {

// Default wallpaper corner colours, mirrored by st::historyBg{TopLeft,...}.
constexpr auto kTopLeft = 0xDBDDBB;
constexpr auto kTopRight = 0x6BA587;
constexpr auto kBottomRight = 0xD5D88D;
constexpr auto kBottomLeft = 0x88B884;

// Doodles were removed from the shipped themes (each pattern.svg is now an
// empty, zero-size SVG). The render-path tests below therefore supply their own
// real doodle so the retained soft-light / coverage / inversion support stays
// covered; two distinct fixtures stand in for "each theme's own doodle".
[[nodiscard]] QString WriteDoodle(const QString &name, const QString &shapes) {
    const auto path = QDir::temp().filePath(name);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QStringLiteral(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1440\" "
            "height=\"2960\" viewBox=\"0 0 1440 2960\">%1</svg>")
            .arg(shapes).toUtf8());
    }
    return path;
}

const auto kDoodleAShapes = QStringLiteral(
    "<rect x='100' y='120' width='240' height='240' fill='black'/>"
    "<circle cx='760' cy='1500' r='170' fill='#0a0a0a'/>"
    "<rect x='1000' y='2300' width='300' height='180' fill='#050505'/>");
const auto kDoodleBShapes = QStringLiteral(
    "<circle cx='320' cy='420' r='210' fill='black'/>"
    "<rect x='820' y='1000' width='260' height='520' fill='#080808'/>");

[[nodiscard]] CornerColors DefaultCorners() {
    return {
        QColor(kTopLeft),
        QColor(kTopRight),
        QColor(kBottomRight),
        QColor(kBottomLeft),
    };
}

[[nodiscard]] QImage SolidGradient(QColor color) {
    auto image = QImage(QSize(8, 8), QImage::Format_RGB32);
    image.fill(color);
    return image;
}

} // namespace

class TestChatBackground : public QObject {
    Q_OBJECT

    QString _doodleA;
    QString _doodleB;

private slots:
    void initTestCase() {
        _doodleA = WriteDoodle(QStringLiteral("tm_test_doodle_a.svg"), kDoodleAShapes);
        _doodleB = WriteDoodle(QStringLiteral("tm_test_doodle_b.svg"), kDoodleBShapes);
    }

    // A real doodle fixture is active before every test; shipped themes carry an
    // empty doodle, so the render-path tests below rely on the fixture. A test
    // that swaps the path need not put it back.
    void init() {
        Theme::SetPatternPath(_doodleA);
    }

    // --- the doodle asset ------------------------------------------------

    // Rasterised from PatternPath() at the viewBox size. If the SVG pipeline is
    // broken this is the test that says so.
    void doodleRasterisesAtItsNativeSize() {
        const auto &alpha = Theme::PatternAlpha();
        QVERIFY(!alpha.isNull());
        QCOMPARE(alpha.size(), QSize(1440, 2960));
        QCOMPARE(alpha.format(), QImage::Format_Alpha8);
    }

    // The shipped resources are wired in, and every shipped theme now carries an
    // EMPTY doodle (the gradient is the whole background) that rasterises to a
    // null mask. If the qrc is not linked into telematrix_core the SetPatternPath
    // below still resolves to nothing, which is the same null -- so this pairs
    // with doodleRasterisesAtItsNativeSize (a real SVG) to prove the pipeline.
    void shippedThemesShipAnEmptyDoodle() {
        Theme::SetPatternPath(QStringLiteral(":/theme/dubai/pattern.svg"));
        QVERIFY(Theme::PatternAlpha().isNull());
        Theme::SetPatternPath(QStringLiteral(":/theme/andalusia/pattern.svg"));
        QVERIFY(Theme::PatternAlpha().isNull());
    }

    // --- per-doodle behaviour --------------------------------------------

    // Two distinct doodles must each rasterise, and to different artwork --
    // otherwise a theme switch would silently keep the old pattern.
    void eachDoodleRasterisesToDistinctArt() {
        Theme::SetPatternPath(_doodleA);
        const auto first = Theme::PatternAlpha();
        Theme::SetPatternPath(_doodleB);
        const auto second = Theme::PatternAlpha();

        QVERIFY(!first.isNull());
        QVERIFY(!second.isNull());
        QCOMPARE(first.size(), QSize(1440, 2960));
        QCOMPARE(second.size(), QSize(1440, 2960));
        QVERIFY2(first != second, "both fixtures rasterised the same doodle");
    }

    // The generation is what invalidates PatternTile and ChatBackgroundCache,
    // so it must move on a real change and stand still otherwise.
    void generationMovesOnlyOnAChangedPath() {
        Theme::SetPatternPath(_doodleA);
        const auto before = Theme::PatternGeneration();

        Theme::SetPatternPath(_doodleA);
        QCOMPARE(Theme::PatternGeneration(), before);

        Theme::SetPatternPath(_doodleB);
        QVERIFY(Theme::PatternGeneration() != before);
    }

    // The regression this guards: PatternTile's cache is keyed by
    // {height, inverted, dpr}. Without the generation in that key a theme
    // switch serves the previous theme's tile at an identical size.
    void switchingThemeRebuildsTheComposite() {
        Theme::SetPatternPath(_doodleA);
        const auto before = Theme::ComposeChatBackground(
            SolidGradient(QColor(kTopLeft)), QSize(120, 240), 1., false);

        Theme::SetPatternPath(_doodleB);
        const auto after = Theme::ComposeChatBackground(
            SolidGradient(QColor(kTopLeft)), QSize(120, 240), 1., false);

        QVERIFY(!before.isNull());
        QVERIFY(!after.isNull());
        QVERIFY2(before != after, "the doodle swap did not reach the tile");
    }

    // A cache built under one doodle is stale under the next, even at the same
    // area and dpr.
    void cacheGoesStaleOnAPatternSwap() {
        Theme::SetPatternPath(_doodleA);
        auto cache = Theme::ChatBackgroundCache();
        cache.setSource(QPixmap(), DefaultCorners());
        cache.rebuild(QSize(100, 200), 1.);
        QVERIFY(cache.matches(QSize(100, 200), 1.));

        Theme::SetPatternPath(_doodleB);
        QVERIFY(!cache.matches(QSize(100, 200), 1.));

        // ...and rebuilding under the new doodle settles again, rather than
        // recompositing on every paint.
        cache.rebuild(QSize(100, 200), 1.);
        QVERIFY(cache.matches(QSize(100, 200), 1.));
    }

    // The guard against a pattern.svg whose canvas is an opaque background rect:
    // reading alpha alone would yield a fully-covered mask, which soft-lights the
    // whole wallpaper uniformly and shows no doodles at all. Coverage is
    // alpha * (1 - luminance), so a light background must read as empty.
    void patternHasCoverageAndGaps() {
        const auto &alpha = Theme::PatternAlpha();
        QVERIFY(!alpha.isNull());
        auto covered = 0;
        auto clear = 0;
        for (auto y = 0; y < alpha.height(); y += 37) {
            const auto *line = alpha.constScanLine(y);
            for (auto x = 0; x < alpha.width(); x += 37) {
                (line[x] > 0) ? ++covered : ++clear;
            }
        }
        // Line art: mostly empty, but the doodles must actually be there.
        QVERIFY2(covered > 0, "pattern.svg rasterised to an empty mask");
        QVERIFY2(clear > covered, "pattern.svg rasterised to a full-bleed mask "
                                  "(is its background an opaque rect?)");
    }

    // --- tile geometry ---------------------------------------------------

    // Odd column count is what lets a tile sit dead centre
    // (cols = ((cx / 2) * 2) + 1).
    void columnsAreAlwaysOdd() {
        for (const auto width : {1, 320, 800, 801, 1440, 2561}) {
            const auto layout = Theme::ComputeTileLayout(
                QSize(width, 600), 1., QSize(292, 600));
            QVERIFY2(
                layout.columns % 2 == 1,
                qPrintable(QStringLiteral("width=%1 cols=%2")
                    .arg(width).arg(layout.columns)));
        }
    }

    void columnsCoverTheViewport() {
        const auto tile = QSize(292, 600);
        for (const auto width : {320, 800, 801, 1440}) {
            const auto layout = Theme::ComputeTileLayout(
                QSize(width, 600), 1., tile);
            QVERIFY(layout.columns * tile.width() >= width);
        }
    }

    // The run is centred: the overhang is split evenly left and right.
    void tileRunIsHorizontallyCentred() {
        const auto tile = QSize(292, 600);
        const auto area = QSize(800, 600);
        const auto layout = Theme::ComputeTileLayout(area, 1., tile);
        const auto left = layout.xshift;
        const auto right = area.width() - (layout.xshift + layout.columns * tile.width());
        QVERIFY(qAbs(left - right) < 1e-9);
        QVERIFY(left <= 0.); // an odd run always overhangs, never underfills
    }

    // A tile scaled to the viewport height needs exactly one row.
    void oneRowWhenTileMatchesHeight() {
        const auto layout = Theme::ComputeTileLayout(
            QSize(800, 600), 1., QSize(292, 600));
        QCOMPARE(layout.rows, 1);
    }

    // The dpr hazard: at dpr 2 the tile is twice as many device pixels, so it
    // must still occupy the same logical width -- i.e. the same column count.
    void layoutIsDprInvariant() {
        const auto one = Theme::ComputeTileLayout(
            QSize(800, 600), 1., QSize(292, 600));
        const auto two = Theme::ComputeTileLayout(
            QSize(800, 600), 2., QSize(584, 1200));
        QCOMPARE(two.columns, one.columns);
        QCOMPARE(two.rows, one.rows);
        QVERIFY(qAbs(two.xshift - one.xshift) < 1e-9);
    }

    void layoutRejectsDegenerateInput() {
        QCOMPARE(Theme::ComputeTileLayout(QSize(), 1., QSize(292, 600)).columns, 0);
        QCOMPARE(Theme::ComputeTileLayout(QSize(800, 600), 0., QSize(292, 600)).columns, 0);
        QCOMPARE(Theme::ComputeTileLayout(QSize(800, 600), 1., QSize()).columns, 0);
    }

    // --- inversion -------------------------------------------------------

    void invertsOnlyOnDarkBackgrounds() {
        // night/background.png is a single #0E1621 pixel.
        QVERIFY(Theme::IsPatternInverted(QColor(0x0E1621)));
        // st::historyBg, the average of the day gradient.
        QVERIFY(!Theme::IsPatternInverted(QColor(0xA9C595)));
        QVERIFY(!Theme::IsPatternInverted(QColor()));
    }

    void invertThresholdIsValue30Percent() {
        // QColor::value() is the HSV max component; the cut is at 0.3.
        QVERIFY(Theme::IsPatternInverted(QColor(76, 0, 0)));      // 76/255 = 0.298
        QVERIFY(!Theme::IsPatternInverted(QColor(77, 0, 0)));     // 77/255 = 0.302
    }

    void averageColorOfSolidIsItself() {
        const auto average = Theme::AverageColor(SolidGradient(QColor(0x0E1621)));
        QCOMPARE(average, QColor(0x0E1621));
    }

    void averageColorOfGradientMatchesStHistoryBg() {
        const auto gradient = Theme::GenerateCornerGradient(
            QSize(512, 512), DefaultCorners());
        const auto average = Theme::AverageColor(gradient);
        // st::historyBg is documented as "average of 4-color gradient".
        QVERIFY(qAbs(average.red() - 0xA9) <= 1);
        QVERIFY(qAbs(average.green() - 0xC5) <= 1);
        QVERIFY(qAbs(average.blue() - 0x95) <= 1);
        QVERIFY(!Theme::IsPatternInverted(average));
    }

    // --- corner gradient (regression guard on the de-duplication) --------

    void cornerGradientPinsItsCorners() {
        const auto size = QSize(64, 48);
        const auto image = Theme::GenerateCornerGradient(size, DefaultCorners());
        QCOMPARE(image.size(), size);
        QCOMPARE(image.pixelColor(0, 0), QColor(kTopLeft));
        QCOMPARE(image.pixelColor(size.width() - 1, 0), QColor(kTopRight));
        QCOMPARE(image.pixelColor(size.width() - 1, size.height() - 1), QColor(kBottomRight));
        QCOMPARE(image.pixelColor(0, size.height() - 1), QColor(kBottomLeft));
    }

    // Reproduces the math the two deleted ensureGradientBackground() copies used.
    void cornerGradientMatchesLegacyBilinearMath() {
        const auto size = QSize(16, 16);
        const auto c = DefaultCorners();
        const auto image = Theme::GenerateCornerGradient(size, c);
        for (auto y = 0; y != size.height(); ++y) {
            const auto fy = qreal(y) / qreal(qMax(size.height() - 1, 1));
            for (auto x = 0; x != size.width(); ++x) {
                const auto fx = qreal(x) / qreal(qMax(size.width() - 1, 1));
                const auto lerp = [&](int a, int b, int d, int e) {
                    const auto top = a + fx * (b - a);
                    const auto bottom = e + fx * (d - e);
                    return qBound(0, int(top + fy * (bottom - top)), 255);
                };
                const auto expected = QColor(
                    lerp(c.topLeft.red(), c.topRight.red(), c.bottomRight.red(), c.bottomLeft.red()),
                    lerp(c.topLeft.green(), c.topRight.green(), c.bottomRight.green(), c.bottomLeft.green()),
                    lerp(c.topLeft.blue(), c.topRight.blue(), c.bottomRight.blue(), c.bottomLeft.blue()));
                QCOMPARE(image.pixelColor(x, y), expected);
            }
        }
    }

    void cornerGradientRejectsEmptySize() {
        QVERIFY(Theme::GenerateCornerGradient(QSize(), DefaultCorners()).isNull());
        QVERIFY(Theme::GenerateCornerGradient(QSize(0, 10), DefaultCorners()).isNull());
    }

    // --- compositing -----------------------------------------------------

    void composeProducesDeviceSizedPremultipliedImage() {
        const auto area = QSize(400, 300);
        const auto composed = Theme::ComposeChatBackground(
            SolidGradient(QColor(0xA9C595)), area, 2., false);
        QVERIFY(!composed.isNull());
        QCOMPARE(composed.size(), QSize(800, 600));
        QCOMPARE(composed.devicePixelRatio(), 2.);
        QCOMPARE(composed.format(), QImage::Format_ARGB32_Premultiplied);
        // deviceIndependentSize() is what a QPainter/drawPixmap will honour.
        QCOMPARE(composed.deviceIndependentSize(), QSizeF(area));
    }

    void composeRejectsDegenerateInput() {
        QVERIFY(Theme::ComposeChatBackground(QImage(), QSize(10, 10), 1., false).isNull());
        QVERIFY(Theme::ComposeChatBackground(SolidGradient(Qt::red), QSize(), 1., false).isNull());
        QVERIFY(Theme::ComposeChatBackground(SolidGradient(Qt::red), QSize(10, 10), 0., false).isNull());
    }

    // Zero opacity is the soft-light identity: the doodle must vanish, leaving
    // the scaled gradient untouched.
    void zeroOpacityLeavesTheGradientUntouched() {
        const auto area = QSize(200, 150);
        const auto grey = QColor(0x80, 0x80, 0x80);
        const auto plain = Theme::ComposeChatBackground(
            SolidGradient(grey), area, 1., false, 0.);
        QVERIFY(!plain.isNull());
        for (auto y = 0; y < plain.height(); y += 17) {
            for (auto x = 0; x < plain.width(); x += 17) {
                QCOMPARE(plain.pixelColor(x, y), grey);
            }
        }
    }

    // ...and at the real opacity it must actually change pixels.
    void patternAltersTheGradient() {
        const auto area = QSize(200, 150);
        const auto grey = QColor(0x80, 0x80, 0x80);
        const auto source = SolidGradient(grey);
        const auto composed = Theme::ComposeChatBackground(source, area, 1., false);
        QVERIFY(!composed.isNull());
        auto changed = 0;
        for (auto y = 0; y < composed.height(); ++y) {
            for (auto x = 0; x < composed.width(); ++x) {
                if (composed.pixelColor(x, y) != grey) {
                    ++changed;
                }
            }
        }
        QVERIFY2(changed > 0, "soft-light pattern left the gradient unchanged");
    }

    // Black doodle darkens a light background; white doodle lightens a dark one.
    // This is the whole reason IsPatternInverted exists.
    void blackDarkensAndWhiteLightens() {
        const auto area = QSize(200, 150);

        const auto light = QColor(0xA9, 0xC5, 0x95);
        const auto overLight = Theme::ComposeChatBackground(
            SolidGradient(light), area, 1., false);
        auto darker = 0;
        auto lighter = 0;
        for (auto y = 0; y < overLight.height(); ++y) {
            for (auto x = 0; x < overLight.width(); ++x) {
                const auto v = overLight.pixelColor(x, y).value();
                if (v < light.value()) ++darker;
                if (v > light.value()) ++lighter;
            }
        }
        QVERIFY2(darker > 0, "black doodle did not darken a light background");
        QCOMPARE(lighter, 0);

        const auto dark = QColor(0x0E, 0x16, 0x21);
        const auto overDark = Theme::ComposeChatBackground(
            SolidGradient(dark), area, 1., true);
        auto lifted = 0;
        for (auto y = 0; y < overDark.height(); ++y) {
            for (auto x = 0; x < overDark.width(); ++x) {
                if (overDark.pixelColor(x, y).value() > dark.value()) {
                    ++lifted;
                }
            }
        }
        QVERIFY2(lifted > 0, "white doodle did not lighten a dark background");
    }

    // Soft-light is directional: a black source is D' = D^2 (never lightens) and
    // a white source is D' = sqrt(D) (never darkens). Over the night colour that
    // makes the un-inverted doodle a dark-on-dark smudge, which is exactly what
    // IsPatternInverted exists to avoid.
    void softLightIsDirectionalOverTheNightColour() {
        const auto area = QSize(120, 90);
        const auto dark = QColor(0x0E, 0x16, 0x21);

        const auto check = [&](bool inverted) {
            const auto composed = Theme::ComposeChatBackground(
                SolidGradient(dark), area, 1., inverted);
            auto moved = 0;
            for (auto y = 0; y < composed.height(); ++y) {
                for (auto x = 0; x < composed.width(); ++x) {
                    const auto v = composed.pixelColor(x, y).value();
                    // Never crosses the background in the wrong direction.
                    if (inverted) {
                        QVERIFY(v >= dark.value() - 1);
                    } else {
                        QVERIFY(v <= dark.value() + 1);
                    }
                    if (v != dark.value()) {
                        ++moved;
                    }
                }
            }
            QVERIFY(moved > 0);
        };
        check(false); // black: only darkens
        check(true);  // white: only lightens
    }

    // The night theme ships a literal 1x1 pixmap; compose must scale it to fill
    // the viewport without going through the smooth scaler. Opacity 0 isolates
    // the fill from the doodle.
    void onePixelSourceFillsTheViewport() {
        auto single = QImage(QSize(1, 1), QImage::Format_RGB32);
        single.fill(QColor(0x0E1621));
        const auto composed = Theme::ComposeChatBackground(
            single, QSize(64, 64), 1., false, 0.);
        QVERIFY(!composed.isNull());
        QCOMPARE(composed.size(), QSize(64, 64));
        for (auto y = 0; y < composed.height(); y += 7) {
            for (auto x = 0; x < composed.width(); x += 7) {
                QCOMPARE(composed.pixelColor(x, y), QColor(0x0E1621));
            }
        }
    }

    // --- the cache -------------------------------------------------------

    void cacheFallsBackToTheCornerGradient() {
        auto cache = Theme::ChatBackgroundCache();
        QVERIFY(cache.isNull());
        cache.setSource(QPixmap(), DefaultCorners());
        QVERIFY(cache.isNull()); // no rebuild yet

        cache.rebuild(QSize(200, 150), 1.);
        QVERIFY(!cache.isNull());
        QVERIFY(cache.matches(QSize(200, 150), 1.));
        QCOMPARE(cache.pixmap().deviceIndependentSize(), QSizeF(200, 150));
    }

    void cacheGoesStaleOnAreaAndDprChange() {
        auto cache = Theme::ChatBackgroundCache();
        cache.setSource(QPixmap(), DefaultCorners());
        cache.rebuild(QSize(200, 150), 1.);

        QVERIFY(!cache.matches(QSize(201, 150), 1.));
        QVERIFY(!cache.matches(QSize(200, 150), 2.));
        // Stale, but still usable for a stretched draw while a rebuild pends.
        QVERIFY(!cache.isNull());

        cache.rebuild(QSize(201, 150), 2.);
        QVERIFY(cache.matches(QSize(201, 150), 2.));
        QCOMPARE(cache.pixmap().size(), QSize(402, 300));
    }

    void cacheInvertsForADarkSource() {
        auto night = QPixmap(1, 1);
        night.fill(QColor(0x0E1621));

        auto cache = Theme::ChatBackgroundCache();
        cache.setSource(night, DefaultCorners());
        cache.rebuild(QSize(120, 90), 1.);
        QVERIFY(!cache.isNull());

        // Inverted: the white doodle lifts pixels above the flat night colour.
        const auto image = cache.pixmap().toImage();
        auto lifted = 0;
        for (auto y = 0; y < image.height(); ++y) {
            for (auto x = 0; x < image.width(); ++x) {
                if (image.pixelColor(x, y).value() > QColor(0x0E1621).value()) {
                    ++lifted;
                }
            }
        }
        QVERIFY2(lifted > 0, "dark theme source did not select the inverted doodle");
    }

    void cacheRebuildIsIdempotent() {
        auto cache = Theme::ChatBackgroundCache();
        cache.setSource(QPixmap(), DefaultCorners());
        cache.rebuild(QSize(200, 150), 1.);
        const auto first = cache.pixmap().cacheKey();
        cache.rebuild(QSize(200, 150), 1.); // matches(): must be a no-op
        QCOMPARE(cache.pixmap().cacheKey(), first);
    }

    void setSourceDropsTheComposite() {
        auto cache = Theme::ChatBackgroundCache();
        cache.setSource(QPixmap(), DefaultCorners());
        cache.rebuild(QSize(200, 150), 1.);
        QVERIFY(!cache.isNull());

        cache.setSource(QPixmap(), DefaultCorners());
        QVERIFY(cache.isNull());
        QVERIFY(!cache.matches(QSize(200, 150), 1.));
    }

    void togglingDoodlesRebuildsWithoutThePattern() {
        const auto area = QSize(200, 150);
        auto cache = Theme::ChatBackgroundCache();
        cache.setSource(QPixmap(), DefaultCorners());
        cache.rebuild(area, 1.);
        const auto withDoodles = cache.pixmap().toImage();

        cache.setDoodlesEnabled(false);
        QVERIFY(cache.isNull()); // the composite is stale, not merely dirty
        cache.rebuild(area, 1.);
        const auto withoutDoodles = cache.pixmap().toImage();
        QCOMPARE(withoutDoodles.size(), withDoodles.size());
        QVERIFY(withoutDoodles != withDoodles);

        // Doodles off is exactly the bare gradient. Normalise the format: a
        // QPixmap round-trip may hand back an opaque variant.
        const auto gradient =
            Theme::GenerateCornerGradient(QSize(512, 512), DefaultCorners());
        QCOMPARE(
            withoutDoodles.convertToFormat(QImage::Format_ARGB32),
            Theme::ComposeChatBackground(gradient, area, 1., false, 0.)
                .convertToFormat(QImage::Format_ARGB32));

        // Re-enabling restores the original composite; a no-op toggle keeps it.
        cache.setDoodlesEnabled(true);
        cache.rebuild(area, 1.);
        QCOMPARE(cache.pixmap().toImage(), withDoodles);
        cache.setDoodlesEnabled(true);
        QVERIFY(cache.matches(area, 1.));
    }
};

QTEST_MAIN(TestChatBackground)
#include "tst_chat_background.moc"

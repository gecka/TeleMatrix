// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>
#include <QFile>
#include <QSet>
#include <QSvgRenderer>

#include <array>
#include <cmath>
#include <vector>

#include "theme/theme_registry.h"

using namespace TeleMatrix;
using namespace Qt::Literals::StringLiterals;

namespace {

// ThemeManager reassigns mutable st:: globals from whichever keys a palette
// happens to define, so a token missing from one variant silently keeps the
// previous theme's colour. These are the ones the chat is unusable without.
const auto kCriticalTokens = {
    "windowBg",
    "windowFg",
    "windowBgActive",
    "msgInBg",
    "msgOutBg",
    "msgServiceBg",
    "historyBg",
    "historyBgTopLeft",
    "historyBgTopRight",
    "historyBgBottomRight",
    "historyBgBottomLeft",
    "splitterHandleBg",
    "toolbarSeparatorFg",
};

const auto kWallpaperTokens = {
    "historyBg",
    "historyBgTopLeft",
    "historyBgTopRight",
    "historyBgBottomRight",
    "historyBgBottomLeft",
};

double LinearChannel(double c) {
    return (c <= 0.04045) ? (c / 12.92) : std::pow((c + 0.055) / 1.055, 2.4);
}

double RelativeLuminance(const QColor &color) {
    return 0.2126 * LinearChannel(color.redF())
        + 0.7152 * LinearChannel(color.greenF())
        + 0.0722 * LinearChannel(color.blueF());
}

double LStar(const QColor &color) {
    const auto y = RelativeLuminance(color);
    return (y > 216. / 24389.) ? (116. * std::cbrt(y) - 16.) : (y * 24389. / 27.);
}

// CIE L*a*b* (D65). Once the outgoing bubble is muted it parts from the
// incoming one by chroma as much as by lightness, which L* alone cannot see.
std::array<double, 3> Lab(const QColor &color) {
    const auto f = [](double t) {
        return (t > 216. / 24389.)
            ? std::cbrt(t)
            : (24389. / 27. * t + 16.) / 116.;
    };
    const auto r = LinearChannel(color.redF());
    const auto g = LinearChannel(color.greenF());
    const auto b = LinearChannel(color.blueF());
    const auto fx = f((0.4124 * r + 0.3576 * g + 0.1805 * b) / 0.95047);
    const auto fy = f(0.2126 * r + 0.7152 * g + 0.0722 * b);
    const auto fz = f((0.0193 * r + 0.1192 * g + 0.9505 * b) / 1.08883);
    return { 116. * fy - 16., 500. * (fx - fy), 200. * (fy - fz) };
}

double DeltaE(const QColor &a, const QColor &b) {
    const auto x = Lab(a);
    const auto y = Lab(b);
    return std::sqrt((x[0] - y[0]) * (x[0] - y[0])
        + (x[1] - y[1]) * (x[1] - y[1])
        + (x[2] - y[2]) * (x[2] - y[2]));
}

// SourceOver at the colour's own alpha, the way the pill is painted.
QColor CompositedOver(const QColor &fg, const QColor &bg) {
    const auto a = fg.alphaF();
    return QColor::fromRgbF(
        float(a * fg.redF() + (1 - a) * bg.redF()),
        float(a * fg.greenF() + (1 - a) * bg.greenF()),
        float(a * fg.blueF() + (1 - a) * bg.blueF()));
}

} // namespace

class TestThemeRegistry : public QObject {
    Q_OBJECT
private slots:
    void oneThemePerRegionWithUniqueIds() {
        const auto &themes = Theme::AllThemes();
        QCOMPARE(themes.size(), 20);

        auto ids = QSet<QString>();
        for (const auto &theme : themes) {
            ids.insert(theme.id);
        }
        QCOMPARE(ids.size(), themes.size());
        QVERIFY(ids.contains(Theme::kDefaultThemeId));
    }

    // The registry order is incidental; the picker reads this one.
    void themesByNameAreSortedAndComplete() {
        const auto sorted = Theme::ThemesByName();
        QCOMPARE(sorted.size(), Theme::AllThemes().size());

        for (auto i = 1; i != int(sorted.size()); ++i) {
            const auto before = sorted[i - 1].displayName();
            const auto after = sorted[i].displayName();
            QVERIFY2(
                QString::localeAwareCompare(before, after) < 0,
                qPrintable(QStringLiteral("%1 before %2").arg(before, after)));
        }
    }

    // The default theme is TeleMatrix's original palette, verbatim: a fresh
    // install must look exactly the way the app always has.
    void defaultThemeKeepsTheOriginalColors() {
        const auto day = Theme::LoadPalette(
            Theme::ThemeById(Theme::kDefaultThemeId).palettePath(false));
        QCOMPARE(day.value("windowBgActive"_L1), QColor(0x40, 0xa7, 0xe3));
        QCOMPARE(day.value("msgOutBg"_L1), QColor(0xef, 0xfd, 0xde));
        QCOMPARE(day.value("historyBgTopLeft"_L1), QColor(0xdb, 0xdd, 0xbb));

        const auto night = Theme::LoadPalette(
            Theme::ThemeById(Theme::kDefaultThemeId).palettePath(true));
        QCOMPARE(night.value("windowBgActive"_L1), QColor(0x52, 0x88, 0xc1));
        QCOMPARE(night.value("windowBg"_L1), QColor(0x17, 0x21, 0x2b));
        QCOMPARE(night.value("historyBgTopLeft"_L1), QColor(0x0e, 0x16, 0x21));
    }

    // A settings file naming a theme this build doesn't have must still start.
    void unknownIdFallsBackToTheDefault() {
        QCOMPARE(Theme::ThemeById(QStringLiteral("nonsense")).id, Theme::kDefaultThemeId);
        QCOMPARE(Theme::ThemeById(QString()).id, Theme::kDefaultThemeId);
        QCOMPARE(Theme::ThemeById(QStringLiteral("andalusia")).id, QStringLiteral("andalusia"));
    }

    // Every discovered theme resolves a display name from its meta.json.
    void everyThemeHasAName() {
        for (const auto &theme : Theme::AllThemes()) {
            QVERIFY(!theme.displayName().isEmpty());
        }
    }

    // Names come from each theme's meta.json, UTF-8 intact.
    void namesAreReadFromMeta() {
        QCOMPARE(Theme::ThemeById(Theme::kDefaultThemeId).displayName(),
            QStringLiteral("Dubai"));
        QCOMPARE(Theme::ThemeById(QStringLiteral("castile-leon")).displayName(),
            QStringLiteral("Castile and León"));
    }

    void everyPaletteParsesWithItsCriticalTokens() {
        for (const auto &theme : Theme::AllThemes()) {
            for (const auto night : { false, true }) {
                const auto path = theme.palettePath(night);
                const auto palette = Theme::LoadPalette(path);
                QVERIFY2(!palette.isEmpty(), qPrintable(path));

                for (const auto *token : kCriticalTokens) {
                    const auto color = palette.value(QLatin1String(token));
                    QVERIFY2(
                        color.isValid(),
                        qPrintable(QStringLiteral("%1 misses %2")
                            .arg(path, QLatin1String(token))));
                }
            }
        }
    }

    // Doodles were removed: every theme ships an intentionally empty pattern.svg
    // (a zero-size SVG that rasterises to nothing), so the chat background is the
    // gradient alone. The doodle render path is kept, but no theme carries one.
    void everyThemeShipsAnEmptyDoodle() {
        for (const auto &theme : Theme::AllThemes()) {
            const auto path = theme.patternPath();
            QVERIFY2(QFile::exists(path), qPrintable(path));
            auto renderer = QSvgRenderer(path);
            QVERIFY2(renderer.defaultSize().isEmpty(), qPrintable(path));
        }
    }

    void previewColorsAreCompleteForEveryVariant() {
        for (const auto &theme : Theme::AllThemes()) {
            for (const auto night : { false, true }) {
                const auto preview = Theme::PreviewColors(theme.id, night);
                QVERIFY(preview.background.topLeft.isValid());
                QVERIFY(preview.background.topRight.isValid());
                QVERIFY(preview.background.bottomRight.isValid());
                QVERIFY(preview.background.bottomLeft.isValid());
                QVERIFY(preview.sent.isValid());
                QVERIFY(preview.received.isValid());
                QVERIFY(preview.accent.isValid());
            }
        }
    }

    // No two cards may look alike. Twenty regions do not fit around the hue
    // wheel -- eight of the flags are red-and-gold -- so this measures the
    // colours the card actually paints rather than trusting the hues to differ.
    // Same-variant pairs only: a theme's day and night card *should* differ a lot.
    void everyCardLooksDifferent() {
        constexpr auto kMinSeparation = 40.; // observed minimum is ~53

        struct Card {
            QString label;
            bool night = false;
            std::array<QColor, 5> colors;
        };
        auto cards = std::vector<Card>();
        for (const auto &theme : Theme::AllThemes()) {
            for (const auto night : { false, true }) {
                const auto c = Theme::PreviewColors(theme.id, night);
                cards.push_back({
                    QStringLiteral("%1/%2").arg(theme.id, night ? u"night"_s : u"day"_s),
                    night,
                    { c.background.topLeft, c.background.bottomRight,
                      c.sent, c.received, c.accent },
                });
            }
        }
        QCOMPARE(int(cards.size()), Theme::AllThemes().size() * 2);

        const auto separation = [](const Card &a, const Card &b) {
            auto total = 0.;
            for (auto i = 0; i != int(a.colors.size()); ++i) {
                const auto dr = a.colors[i].red() - b.colors[i].red();
                const auto dg = a.colors[i].green() - b.colors[i].green();
                const auto db = a.colors[i].blue() - b.colors[i].blue();
                total += std::sqrt(double((dr * dr) + (dg * dg) + (db * db)));
            }
            return total;
        };

        for (auto i = 0u; i != cards.size(); ++i) {
            for (auto j = i + 1; j != cards.size(); ++j) {
                if (cards[i].night != cards[j].night) {
                    continue;
                }
                const auto apart = separation(cards[i], cards[j]);
                QVERIFY2(
                    apart >= kMinSeparation,
                    qPrintable(QStringLiteral("%1 and %2 are only %3 apart")
                        .arg(cards[i].label, cards[j].label)
                        .arg(apart, 0, 'f', 1)));
            }
        }
    }

    // Bubbles and the service pill must clear the wallpaper they are painted
    // on. The floors are CIE L* gaps against the wallpaper's extreme corner,
    // sitting just under the baseline relationships (night in-bubble 7.1,
    // night pill 10.4, day out-bubble 10.4, day in-bubble 12.8, day pill 7.5;
    // pills measured composited at their own alpha), so verbatim Gibraltar
    // passes too. tools/theme/colorize.py enforces the same floors when the
    // palettes are generated; this re-asserts them on what actually shipped.
    void bubblesAndServicePillClearTheWallpaper() {
        constexpr auto kNightIn = 6.5;
        constexpr auto kNightService = 10.0;
        constexpr auto kDayOut = 8.0;
        constexpr auto kDayIn = 10.0;
        constexpr auto kDayService = 6.5;
        constexpr auto kEps = 0.01;

        for (const auto &theme : Theme::AllThemes()) {
            for (const auto night : { false, true }) {
                const auto palette = Theme::LoadPalette(theme.palettePath(night));
                auto bright = QColor();
                auto dark = QColor();
                for (const auto *token : kWallpaperTokens) {
                    const auto corner = palette.value(QLatin1String(token));
                    QVERIFY2(corner.isValid(), qPrintable(theme.id));
                    if (!bright.isValid()
                        || RelativeLuminance(corner) > RelativeLuminance(bright)) {
                        bright = corner;
                    }
                    if (!dark.isValid()
                        || RelativeLuminance(corner) < RelativeLuminance(dark)) {
                        dark = corner;
                    }
                }
                const auto in = palette.value("msgInBg"_L1);
                const auto out = palette.value("msgOutBg"_L1);
                const auto service = palette.value("msgServiceBg"_L1);
                const auto clears = [&](const char *token, double gap, double floor) {
                    QVERIFY2(
                        gap >= floor - kEps,
                        qPrintable(QStringLiteral(
                            "%1/%2 %3 clears the wallpaper by only %4 L* (floor %5)")
                            .arg(theme.id, night ? u"night"_s : u"day"_s,
                                QLatin1String(token))
                            .arg(gap, 0, 'f', 2)
                            .arg(floor)));
                };
                if (night) {
                    clears("msgInBg", LStar(in) - LStar(bright), kNightIn);
                    clears("msgServiceBg",
                        LStar(CompositedOver(service, bright)) - LStar(bright),
                        kNightService);
                } else {
                    clears("msgInBg", LStar(in) - LStar(bright), kDayIn);
                    clears("msgOutBg", LStar(out) - LStar(bright), kDayOut);
                    clears("msgServiceBg",
                        LStar(dark) - LStar(CompositedOver(service, dark)),
                        kDayService);
                }
            }
        }
    }

    // Links and code inside an outgoing bubble, against that bubble. The
    // generator maps a pale colour by its HLS lightness so it stays a tint
    // rather than going white, but the bubble under it is mapped by luminance
    // and lands on the same L* in every theme -- and at equal lightness a blue
    // carries a tenth of a green's light, so the gap used to collapse across
    // the blue-violet-red arc (link 2.56:1 on melilla, against the default's
    // 4.59:1) while the greens sat above the baseline. Paling the foreground to
    // fix it washes it into the near-white body text, so the generator instead
    // mutes and deepens the bubble -- which is why msgOutBg must also be checked
    // against msgInBg here: that is the separation the muting spends.
    // tools/theme/colorize.py enforces the same gaps when the palettes are
    // generated -- this re-asserts them on what actually shipped.
    void linksAndCodeClearTheOutgoingBubble() {
        constexpr auto kNightLink = 36.0;
        constexpr auto kNightMono = 40.0;
        constexpr auto kBubbles = 12.0;
        constexpr auto kEps = 0.01;

        for (const auto &theme : Theme::AllThemes()) {
            const auto palette = Theme::LoadPalette(theme.palettePath(true));
            const auto out = palette.value("msgOutBg"_L1);
            const auto in = palette.value("msgInBg"_L1);
            QVERIFY2(out.isValid() && in.isValid(), qPrintable(theme.id));
            const auto clears = [&](const char *token, double floor) {
                const auto fg = palette.value(QLatin1String(token));
                QVERIFY2(fg.isValid(), qPrintable(theme.id));
                const auto gap = LStar(fg) - LStar(out);
                QVERIFY2(
                    gap >= floor - kEps,
                    qPrintable(QStringLiteral(
                        "%1/night %2 clears msgOutBg by only %3 L* (floor %4)")
                        .arg(theme.id, QLatin1String(token))
                        .arg(gap, 0, 'f', 2)
                        .arg(floor)));
            };
            clears("historyLinkOutFg", kNightLink);
            clears("msgOutMonoFg", kNightMono);

            const auto bubbles = DeltaE(out, in);
            QVERIFY2(
                bubbles >= kBubbles - kEps,
                qPrintable(QStringLiteral(
                    "%1/night msgOutBg and msgInBg are only %2 dE apart "
                    "(floor %3)").arg(theme.id)
                    .arg(bubbles, 0, 'f', 2)
                    .arg(kBubbles)));
        }
    }

    // The rooms list and the timeline are two large flat fields meeting at a
    // 1px splitter. windowBg (which dialogsBg aliases) is luminance-preserved
    // per theme while the wallpaper is re-lit by wall_l, so at night six themes
    // land within 1 L* of the rooms list and six invert -- the line is the only
    // thing separating the columns, and it has to clear both neighbours. Only
    // the left-edge corners abut it. It was a hardcoded 9%-black constant,
    // invisible on every night theme (dL* 0.88-1.58), until it became a palette
    // token. tools/theme/colorize.py checks the same floor when generating.
    void columnsSeparateAcrossTheSplitter() {
        // Two floors: the splitter is bounded by Gibraltar's flat #0e1621
        // wallpaper (a pure-black line maxes out at 7.02 there), while shadowFg
        // only ever meets windowBg and clears it by ~8. kSeparator sits above
        // the old night alpha, which measured 4.00-4.19 and so would have
        // passed a 4.0 floor unnoticed. Night's seam floor is deliberately low:
        // the divider is meant to be barely-there at night (tdesktop parity),
        // the floor only guards against it vanishing entirely.
        constexpr auto kSeamLine = 4.0;
        constexpr auto kSeamLineNight = 1.2;
        constexpr auto kSeparator = 6.0;
        // The toolbar/composer separators follow the splitter at night:
        // barely-there by design, floored only against vanishing.
        constexpr auto kSeparatorNight = 1.2;
        constexpr auto kEps = 0.01;

        for (const auto &theme : Theme::AllThemes()) {
            for (const auto night : { false, true }) {
                const auto palette = Theme::LoadPalette(theme.palettePath(night));
                const auto label = QStringLiteral("%1/%2")
                    .arg(theme.id, night ? u"night"_s : u"day"_s);

                const auto dialogs = palette.value("dialogsBg"_L1);
                const auto line = palette.value("splitterHandleBg"_L1);
                QVERIFY2(dialogs.isValid(), qPrintable(label));
                QVERIFY2(line.isValid(), qPrintable(label));

                auto seam = QColor();
                for (const auto *token :
                        { "historyBgTopLeft", "historyBgBottomLeft" }) {
                    const auto corner = palette.value(QLatin1String(token));
                    QVERIFY2(corner.isValid(), qPrintable(label));
                    if (!seam.isValid()
                        || RelativeLuminance(corner) > RelativeLuminance(seam)) {
                        seam = corner;
                    }
                }

                // Painted over each surface in turn, so it must clear both.
                const auto seamFloor = night ? kSeamLineNight : kSeamLine;
                for (const auto &surface : { dialogs, seam }) {
                    const auto gap = std::abs(
                        LStar(surface) - LStar(CompositedOver(line, surface)));
                    QVERIFY2(
                        gap >= seamFloor - kEps,
                        qPrintable(QStringLiteral(
                            "%1 splitter line clears its neighbour by only "
                            "%2 L* (floor %3)").arg(label).arg(gap, 0, 'f', 2)
                            .arg(seamFloor)));
                }

                // The horizontal seams: under the rooms-list and room toolbars,
                // and over the composer. These shared shadowFg until it proved
                // half as visible at night (4.0) as in day (8.2); they now have
                // their own token so the fix does not touch the other 40-odd
                // shadowFg separators.
                const auto window = palette.value("windowBg"_L1);
                const auto toolbar = palette.value("toolbarSeparatorFg"_L1);
                QVERIFY2(window.isValid() && toolbar.isValid(),
                    qPrintable(label));
                const auto sepGap = std::abs(
                    LStar(window) - LStar(CompositedOver(toolbar, window)));
                const auto sepFloor = night ? kSeparatorNight : kSeparator;
                QVERIFY2(
                    sepGap >= sepFloor - kEps,
                    qPrintable(QStringLiteral(
                        "%1 toolbar separator clears windowBg by only %2 L* "
                        "(floor %3)").arg(label).arg(sepGap, 0, 'f', 2)
                        .arg(sepFloor)));
            }
        }
    }

    // Day and night are genuinely two variants of one theme, not the same file.
    void dayAndNightDifferWithinEachTheme() {
        for (const auto &theme : Theme::AllThemes()) {
            const auto day = Theme::PreviewColors(theme.id, false);
            const auto night = Theme::PreviewColors(theme.id, true);
            QVERIFY2(
                day.background.topLeft != night.background.topLeft,
                qPrintable(theme.id));
            QVERIFY2(day.received != night.received, qPrintable(theme.id));
        }
    }

    void missingPaletteYieldsAnEmptyMap() {
        QVERIFY(Theme::LoadPalette(QStringLiteral(":/theme/nope/day/colors.tdesktop-theme")).isEmpty());
    }
};

QTEST_MAIN(TestThemeRegistry)
#include "tst_theme_registry.moc"

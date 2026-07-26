// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "theme/theme_parser.h"

using namespace TeleMatrix::Theme;

class TestThemeParser : public QObject {
    Q_OBJECT

private slots:
    void parsesHexColors() {
        const auto colors = parseThemeColors(QStringLiteral(
            "windowBg: #112233;\n"
            "withAlpha: #11223344;\n"));
        QCOMPARE(colors.value("windowBg"), QColor(0x11, 0x22, 0x33));
        QCOMPARE(colors.value("withAlpha"), QColor(0x11, 0x22, 0x33, 0x44));
    }

    void resolvesAliasChains() {
        const auto colors = parseThemeColors(QStringLiteral(
            "base: #ff0000;\n"
            "mid: base;\n"
            "top: mid;\n"));
        const auto red = QColor(0xff, 0x00, 0x00);
        QCOMPARE(colors.value("base"), red);
        QCOMPARE(colors.value("mid"), red);
        QCOMPARE(colors.value("top"), red);
    }

    void ignoresCommentsAndBlankLines() {
        const auto colors = parseThemeColors(QStringLiteral(
            "// a leading comment\n"
            "\n"
            "windowBg: #010203; // trailing comment\n"));
        QCOMPARE(colors.value("windowBg"), QColor(1, 2, 3));
        QVERIFY(!colors.contains("//"));
    }

    void skipsMalformedEntries() {
        const auto colors = parseThemeColors(QStringLiteral(
            "noColon line\n"
            "emptyValue:\n"
            "good: #abcdef;\n"
            "badHex: #zzzzzz;\n"));
        QVERIFY(!colors.contains("noColon line"));
        QVERIFY(!colors.contains("emptyValue"));
        QCOMPARE(colors.value("good"), QColor(0xab, 0xcd, 0xef));
        // A malformed hex parses to an invalid QColor (not dropped from the map,
        // but never a usable colour).
        QVERIFY(!colors.value("badHex").isValid());
    }

    // Regression: a circular alias chain must terminate (cycle guard) and the
    // unresolvable keys must be dropped rather than hanging the parser forever.
    void circularAliasTerminatesAndIsDropped() {
        const auto colors = parseThemeColors(QStringLiteral(
            "a: b;\n"
            "b: a;\n"
            "real: #00ff00;\n"));
        QVERIFY(!colors.contains("a"));
        QVERIFY(!colors.contains("b"));
        // A real entry parsed in the same pass is unaffected.
        QCOMPARE(colors.value("real"), QColor(0x00, 0xff, 0x00));
    }

    void selfReferentialAliasIsDropped() {
        const auto colors = parseThemeColors(QStringLiteral("loop: loop;\n"));
        QVERIFY(!colors.contains("loop"));
    }
};

QTEST_GUILESS_MAIN(TestThemeParser)
#include "tst_theme_parser.moc"

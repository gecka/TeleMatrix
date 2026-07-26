// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include <QImage>
#include <QSet>

#include "history/history_video_placeholder.h"

using namespace TeleMatrix;

class TestVideoPlaceholder : public QObject {
    Q_OBJECT
private slots:
    void deterministicAndColored() {
        const auto a =
            syntheticBlurPlaceholder(QStringLiteral("$evt:server"), 32, 24);
        const auto b =
            syntheticBlurPlaceholder(QStringLiteral("$evt:server"), 32, 24);
        QVERIFY(!a.isNull());
        QCOMPARE(a.size(), QSize(32, 24));
        QCOMPARE(a, b); // deterministic for the same seed

        const auto c =
            syntheticBlurPlaceholder(QStringLiteral("$other:server"), 32, 24);
        QVERIFY(a != c); // varies by seed

        // Not a flat fill: more than one distinct color across the image.
        QSet<QRgb> colors;
        for (int y = 0; y < 24; y += 6) {
            for (int x = 0; x < 32; x += 8) {
                colors.insert(a.pixel(x, y));
            }
        }
        QVERIFY(colors.size() > 1);
    }

    void handlesTinyAndEmptySeed() {
        // Dimensions are clamped to >= 2; empty seed is still deterministic.
        QVERIFY(!syntheticBlurPlaceholder(QString(), 1, 1).isNull());
        QVERIFY(!syntheticBlurPlaceholder(QStringLiteral("x"), 8, 8).isNull());
    }
};

QTEST_GUILESS_MAIN(TestVideoPlaceholder)
#include "tst_video_placeholder.moc"

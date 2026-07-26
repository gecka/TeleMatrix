// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "history/bubble_corners_cache.h"

using namespace TeleMatrix;
using BubbleSprites::Key;
using HistoryMessage::BubbleCorners;

// Sprite rendering is verified visually (see the plan); these cover the pure
// key/clamp logic that decides which sprite set a bubble draws from.
class TestBubbleCorners : public QObject {
    Q_OBJECT
private slots:
    // clampCorners caps every radius at half the smaller dimension, matching
    // roundedBubblePath's clamp (int(min/2.0)).
    void clampsToHalfSmallerDimension() {
        const auto c = BubbleSprites::clampCorners({100, 100, 100, 100}, 40, 60);
        QCOMPARE(c.topLeft, 20);
        QCOMPARE(c.topRight, 20);
        QCOMPARE(c.bottomRight, 20);
        QCOMPARE(c.bottomLeft, 20);
    }
    void leavesSmallRadiiUntouched() {
        const auto c = BubbleSprites::clampCorners({5, 8, 3, 6}, 40, 60);
        QCOMPARE(c.topLeft, 5);
        QCOMPARE(c.topRight, 8);
        QCOMPARE(c.bottomRight, 3);
        QCOMPARE(c.bottomLeft, 6);
    }
    void floorsOddDimension() {
        // min(41,60)/2 == 20 (integer floor, matching int(20.5)).
        const auto c = BubbleSprites::clampCorners({100, 100, 100, 100}, 41, 60);
        QCOMPARE(c.topLeft, 20);
    }
    void clampsNegativeAndZero() {
        const auto c = BubbleSprites::clampCorners({10, 10, 10, 10}, 0, 60);
        QCOMPARE(c.topLeft, 0);
        QCOMPARE(c.bottomRight, 0);
        // A degenerate (negative) size must not underflow the limit below zero.
        const auto d = BubbleSprites::clampCorners({10, 10, 10, 10}, -4, 60);
        QCOMPARE(d.topLeft, 0);
    }

    // Key: value equality across every field and a stable hash.
    void keyEqualityAllFields() {
        const Key a{6, 12, 12, 0, 1, qRgb(0xef, 0xfd, 0xde), 200};
        const Key b{6, 12, 12, 0, 1, qRgb(0xef, 0xfd, 0xde), 200};
        QVERIFY(a == b);
        QCOMPARE(BubbleSprites::qHash(a, 0), BubbleSprites::qHash(b, 0));
    }
    void keyInequalityPerField() {
        const Key base{6, 12, 12, 0, 1, qRgb(0, 0, 0), 100};
        QVERIFY(!(base == Key{7, 12, 12, 0, 1, qRgb(0, 0, 0), 100}));   // topLeft
        QVERIFY(!(base == Key{6, 13, 12, 0, 1, qRgb(0, 0, 0), 100}));   // topRight
        QVERIFY(!(base == Key{6, 12, 11, 0, 1, qRgb(0, 0, 0), 100}));   // bottomRight
        QVERIFY(!(base == Key{6, 12, 12, 1, 1, qRgb(0, 0, 0), 100}));   // bottomLeft
        QVERIFY(!(base == Key{6, 12, 12, 0, 2, qRgb(0, 0, 0), 100}));   // tail
        QVERIFY(!(base == Key{6, 12, 12, 0, 1, qRgb(1, 0, 0), 100}));   // color
        QVERIFY(!(base == Key{6, 12, 12, 0, 1, qRgb(0, 0, 0), 200}));   // dpr
    }
    void keyUsableInQHashContainer() {
        // ADL must find qHash(Key, size_t) so Key works as a QHash key.
        QHash<Key, int> map;
        map.insert(Key{6, 12, 12, 0, 1, qRgb(0, 0, 0), 100}, 42);
        QCOMPARE(map.value(Key{6, 12, 12, 0, 1, qRgb(0, 0, 0), 100}), 42);
        QVERIFY(!map.contains(Key{6, 12, 12, 0, 1, qRgb(0, 0, 0), 200}));
    }
};

QTEST_APPLESS_MAIN(TestBubbleCorners)
#include "tst_bubble_corners.moc"

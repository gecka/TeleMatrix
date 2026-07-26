// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "history/jump_load_controller.h"

using namespace TeleMatrix;
using Action = JumpLoadController::Action;

class TestJumpLoadController : public QObject {
    Q_OBJECT
private slots:
    void inactiveByDefault() {
        JumpLoadController c;
        QVERIFY(!c.active());
        QCOMPARE(c.onFloorElapsed(), Action::None);
        QCOMPARE(c.onTargetArrived(), Action::None);
        QCOMPARE(c.onFetchFailed(), Action::None);
    }

    void beginActivates() {
        JumpLoadController c;
        c.begin();
        QVERIFY(c.active());
    }

    void floorThenTargetReveals() {
        JumpLoadController c;
        c.begin();
        QCOMPARE(c.onFloorElapsed(), Action::None);
        QCOMPARE(c.onTargetArrived(), Action::Reveal);
        QVERIFY(!c.active());
    }

    void targetThenFloorReveals() {
        JumpLoadController c;
        c.begin();
        QCOMPARE(c.onTargetArrived(), Action::None);
        QCOMPARE(c.onFloorElapsed(), Action::Reveal);
        QVERIFY(!c.active());
    }

    void failBeforeFloorFallsBack() {
        JumpLoadController c;
        c.begin();
        QCOMPARE(c.onFetchFailed(), Action::Fallback);
        QVERIFY(!c.active());
    }

    void failAfterTargetIsIgnored() {
        JumpLoadController c;
        c.begin();
        QCOMPARE(c.onTargetArrived(), Action::None); // floor not elapsed yet
        QCOMPARE(c.onFetchFailed(), Action::None);   // target already in hand
        QVERIFY(c.active());
        QCOMPARE(c.onFloorElapsed(), Action::Reveal);
    }

    void beginSupersedesResetsFloor() {
        JumpLoadController c;
        c.begin();
        QCOMPARE(c.onFloorElapsed(), Action::None);
        c.begin(); // supersede — floor must reset
        QCOMPARE(c.onTargetArrived(), Action::None);
        QCOMPARE(c.onFloorElapsed(), Action::Reveal);
    }

    void resetDeactivates() {
        JumpLoadController c;
        c.begin();
        c.reset();
        QVERIFY(!c.active());
        QCOMPARE(c.onTargetArrived(), Action::None);
    }
};

QTEST_APPLESS_MAIN(TestJumpLoadController)
#include "tst_jump_load_controller.moc"

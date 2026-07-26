// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "history/history_selection_state.h"

using namespace TeleMatrix;

class TestHistorySelectionState : public QObject {
    Q_OBJECT

private slots:
    // updateClickCount increments only when the next click is BOTH within the
    // time interval AND within a 3px manhattan radius of the previous click.
    void countsRapidClicksInPlace() {
        HistorySelectionState state;
        const int interval = 500;
        QCOMPARE(state.updateClickCount(QPoint(10, 10), 1000, interval), 1);
        // Same spot, 200ms later -> double click.
        QCOMPARE(state.updateClickCount(QPoint(10, 10), 1200, interval), 2);
        // 1px away, 100ms later -> triple click.
        QCOMPARE(state.updateClickCount(QPoint(11, 11), 1300, interval), 3);
    }

    void resetsWhenTooSlow() {
        HistorySelectionState state;
        const int interval = 500;
        QCOMPARE(state.updateClickCount(QPoint(10, 10), 1000, interval), 1);
        QCOMPARE(state.updateClickCount(QPoint(10, 10), 1200, interval), 2);
        // 700ms gap (>= interval) -> count restarts at 1.
        QCOMPARE(state.updateClickCount(QPoint(10, 10), 1900, interval), 1);
    }

    void resetsWhenTooFar() {
        HistorySelectionState state;
        const int interval = 500;
        QCOMPARE(state.updateClickCount(QPoint(10, 10), 1000, interval), 1);
        // 4px manhattan (> 3) within the interval -> restart at 1.
        QCOMPARE(state.updateClickCount(QPoint(14, 10), 1100, interval), 1);
    }

    void manhattanBoundaryIsInclusiveAtThree() {
        HistorySelectionState state;
        const int interval = 500;
        QCOMPARE(state.updateClickCount(QPoint(10, 10), 1000, interval), 1);
        // Exactly 3px (2+1) within the interval still counts as a repeat.
        QCOMPARE(state.updateClickCount(QPoint(12, 11), 1100, interval), 2);
    }

    void selectionModeAndToggle() {
        HistorySelectionState state;
        QVERIFY(!state.inSelectionMode());
        state.enterSelectionMode({QStringLiteral("$a")});
        QVERIFY(state.inSelectionMode());
        QVERIFY(state.selectedContains("$a"));
        QCOMPARE(state.selectedCount(), 1);
        // Toggle a second id on, then the first off.
        QVERIFY(state.toggleSelected("$b"));
        QCOMPARE(state.selectedCount(), 2);
        QVERIFY(state.toggleSelected("$a")); // still non-empty ($b remains)
        QVERIFY(!state.selectedContains("$a"));
        QVERIFY(state.exitSelectionMode());
        QVERIFY(!state.inSelectionMode());
        QVERIFY(state.selectedEmpty());
        // Exiting again is a no-op returning false.
        QVERIFY(!state.exitSelectionMode());
    }
};

QTEST_GUILESS_MAIN(TestHistorySelectionState)
#include "tst_history_selection_state.moc"

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "history/unread_read_progress.h"

using namespace TeleMatrix;

class TestUnreadReadProgress : public QObject {
    Q_OBJECT

private slots:
    // Normal downward scroll: from the frontier, advance through every row
    // whose bottom edge now ends inside the viewport, stop at the first that
    // overflows.
    void normalScrollAdvancesThroughVisibleRows() {
        const QVector<int> bottoms{100, 200, 300, 400, 500};
        // Frontier at index 1 (bottom 200); viewport ends at 350 → rows 1 and 2
        // fit, row 3 (400) overflows.
        QCOMPARE(UnreadRead::readTillIndex(bottoms, 1, 350, false), 2);
    }

    // The user has scrolled up so the unread frontier sits below the fold: its
    // own bottom already overflows the viewport → nothing is read.
    void scrolledUpPastFrontierDetectsNothing() {
        const QVector<int> bottoms{100, 200, 300, 400, 500};
        // Frontier at index 3 (bottom 400); viewport ends at 250.
        QCOMPARE(UnreadRead::readTillIndex(bottoms, 3, 250, false), -1);
    }

    // The reported bug: a window reset shrinks the content, the scroll clamps,
    // and the viewport spans the whole timeline (viewportBottom past the last
    // row). Unheld this sweeps to the newest row (the defect); held it must
    // detect nothing.
    void resetClampSweepIsSuppressedWhenHeld() {
        const QVector<int> bottoms{100, 200, 300, 400, 500};
        QCOMPARE(UnreadRead::readTillIndex(bottoms, 0, 10000, false), 4);
        QCOMPARE(UnreadRead::readTillIndex(bottoms, 0, 10000, true), -1);
    }

    // An anchor that is not in the loaded window (or absent) reads nothing.
    void unloadedFrontierDetectsNothing() {
        const QVector<int> bottoms{100, 200, 300};
        QCOMPARE(UnreadRead::readTillIndex(bottoms, -1, 10000, false), -1);
        QCOMPARE(UnreadRead::readTillIndex(bottoms, 3, 10000, false), -1);
        QCOMPARE(UnreadRead::readTillIndex({}, 0, 10000, false), -1);
    }
};

QTEST_APPLESS_MAIN(TestUnreadReadProgress)
#include "tst_unread_read_progress.moc"

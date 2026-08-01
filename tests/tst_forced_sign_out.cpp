// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "app/forced_sign_out.h"

using namespace TeleMatrix;

class TestForcedSignOut : public QObject {
    Q_OBJECT

private slots:
    // The account on screen gets the interactive path: the user is looking at
    // it, so they get told why it went away.
    void activeAccountSignsOutInteractively() {
        QCOMPARE(
            RouteForcedSignOut(/*accountIndex=*/1, /*activeIndex=*/1, false),
            ForcedSignOutRoute::Active);
    }

    // Another account's dead token must never put a dialog in front of the
    // account being used.
    void backgroundAccountSignsOutInPlace() {
        QCOMPARE(
            RouteForcedSignOut(/*accountIndex=*/2, /*activeIndex=*/0, false),
            ForcedSignOutRoute::Background);
    }

    // Every in-flight request against a dead token rejects, so the signal
    // repeats. Restarting a teardown already underway would double-wipe the
    // account and race its own bridge shutdown.
    void repeatSignalIsIgnoredWhileTearingDown() {
        QCOMPARE(
            RouteForcedSignOut(/*accountIndex=*/1, /*activeIndex=*/1, true),
            ForcedSignOutRoute::Ignore);
        QCOMPARE(
            RouteForcedSignOut(/*accountIndex=*/2, /*activeIndex=*/0, true),
            ForcedSignOutRoute::Ignore);
    }

    // The account can be gone by the time the signal lands (the user signed it
    // out by hand first). -1 is "no longer in the domain".
    void vanishedAccountIsIgnored() {
        QCOMPARE(
            RouteForcedSignOut(/*accountIndex=*/-1, /*activeIndex=*/0, false),
            ForcedSignOutRoute::Ignore);
    }
};

QTEST_MAIN(TestForcedSignOut)
#include "tst_forced_sign_out.moc"

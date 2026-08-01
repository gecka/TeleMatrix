// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "history/jump_routing.h"

using namespace TeleMatrix;
using JumpRouting::Route;

class TestJumpRouting : public QObject {
    Q_OBJECT

private slots:
    // The overwhelmingly common case: a toast for the room already on screen,
    // about the message that just arrived. It is in the live window, so no fetch
    // and no mode change — just scroll and flash it.
    void sameRoomLiveAndLoadedScrollsInstantly() {
        QCOMPARE(
            JumpRouting::routeNotificationJump(true, true, true),
            Route::InstantScroll);
    }

    // The regression this routing exists for: the room is on screen but detached
    // (an earlier search/permalink jump left it focused). Landing the toast there
    // would keep it detached, so drop the focused window first.
    void sameRoomFocusedReturnsToLive() {
        QCOMPARE(
            JumpRouting::routeNotificationJump(true, false, true),
            Route::ReturnToLiveThenHighlight);
        // Still return to live when the target is not in the focused window —
        // liveness is decided before the target is looked for.
        QCOMPARE(
            JumpRouting::routeNotificationJump(true, false, false),
            Route::ReturnToLiveThenHighlight);
    }

    // A different room: open it live like any rooms-list click. Whatever the
    // room on screen is doing says nothing about the target.
    void otherRoomAlwaysOpensLive() {
        for (const bool isLive : {true, false}) {
            for (const bool loaded : {true, false}) {
                QCOMPARE(
                    JumpRouting::routeNotificationJump(false, isLive, loaded),
                    Route::LiveOpenThenHighlight);
            }
        }
    }

    // An old toast clicked long after the fact: live, same room, but the target
    // has aged out of the window. This is a real jump — serve it focused.
    void sameRoomLiveButUnloadedFetchesFocused() {
        QCOMPARE(
            JumpRouting::routeNotificationJump(true, true, false),
            Route::FocusFetch);
    }

    // Escalation fires only on a live slice that still lacks the target...
    void escalatesOnceWhenLiveSliceLacksTarget() {
        QVERIFY(JumpRouting::shouldEscalateToFocusFetch(true, true, false));
        // ...never when it arrived,
        QVERIFY(!JumpRouting::shouldEscalateToFocusFetch(true, true, true));
        // ...never off a focused slice (that IS the fetch landing),
        QVERIFY(!JumpRouting::shouldEscalateToFocusFetch(true, false, false));
        // ...and never once the one-shot flag is spent, which is what stops a
        // room whose target never loads from re-escalating on every slice.
        QVERIFY(!JumpRouting::shouldEscalateToFocusFetch(false, true, false));
    }
};

QTEST_APPLESS_MAIN(TestJumpRouting)
#include "tst_jump_routing.moc"

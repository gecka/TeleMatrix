// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>
#include <QSignalSpy>

#include "app/unread_state_store.h"

using namespace TeleMatrix;

namespace {

RoomSummary room(
    const QString &id,
    int unread,
    int highlight = 0,
    bool muted = false,
    bool markedUnread = false) {
    RoomSummary r;
    r.roomId = id;
    r.unreadCount = unread;
    r.highlightCount = highlight;
    r.isMuted = muted;
    r.isMarkedUnread = markedUnread;
    r.notificationMode = muted ? RoomNotificationMode::Mute
                               : RoomNotificationMode::AllMessages;
    return r;
}

RoomSummary roomAt(const QString &id, int unread, qint64 timestamp) {
    auto r = room(id, unread);
    r.timestamp = timestamp;
    return r;
}

TimelineSlice liveSlice(int unread, const QString &firstUnread = QString()) {
    TimelineSlice s;
    s.isLive = true;
    s.unreadStateKnown = true;
    s.unreadCount = unread;
    s.firstUnreadEventId = firstUnread;
    return s;
}

RoomUnreadSnapshot unreadSnapshot(int unread) {
    RoomUnreadSnapshot s;
    s.unreadCount = unread;
    return s;
}

} // namespace

class TestUnreadStateStore : public QObject {
    Q_OBJECT

private slots:
    void appliesSnapshotAndComputesTotals() {
        UnreadStateStore store;
        store.applyRoomListSnapshot({
            room("!a:x", 5, 1),
            room("!b:x", 3, 0, /*muted=*/true),
        });

        const auto a = store.roomState("!a:x");
        QCOMPARE(a.serverUnreadCount, 5);
        QCOMPARE(a.effectiveUnreadCount, 5);
        QCOMPARE(a.effectiveHighlightCount, 1);

        // Muted rooms count toward the all-inclusive total but not the badge.
        QCOMPARE(store.totalUnreadCount(/*includeMuted=*/true), 8);
        QCOMPARE(store.totalUnreadCount(/*includeMuted=*/false), 5);
    }

    // "Any mute level" — a Mentions-only room is treated as muted for the badge just
    // like a fully-Muted one, even though its isMuted flag is false.
    void mentionsOnlyCountsAsMutedForBadge() {
        auto mentions = room("!m:x", 4);
        mentions.notificationMode = RoomNotificationMode::MentionsOnly;
        QVERIFY(!mentions.isMuted);

        UnreadStateStore store;
        store.applyRoomListSnapshot({
            room("!a:x", 5),               // default — counts toward the badge
            mentions,                      // mentions-only — excluded when muted is excluded
            room("!b:x", 3, 0, /*muted=*/true),
        });

        QCOMPARE(store.totalUnreadCount(/*includeMuted=*/true), 12);
        QCOMPARE(store.totalUnreadCount(/*includeMuted=*/false), 5);
    }

    void droppedRoomsAreForgotten() {
        UnreadStateStore store;
        store.applyRoomListSnapshot({room("!a:x", 5), room("!b:x", 2)});
        // Re-snapshot without B: it must be removed.
        store.applyRoomListSnapshot({room("!a:x", 5)});
        QCOMPARE(store.roomState("!b:x").effectiveUnreadCount, 0);
        QCOMPARE(store.totalUnreadCount(true), 5);
    }

    void optimisticMarkReadZeroesEffective() {
        UnreadStateStore store;
        store.applyRoomListSnapshot({room("!a:x", 5)});

        const auto rev = store.optimisticMarkRead("!a:x");
        QVERIFY(rev > 0);
        const auto a = store.roomState("!a:x");
        QVERIFY(a.pendingExplicitMarkRead);
        QCOMPARE(a.effectiveUnreadCount, 0);
        // Server count is untouched; only the effective (presented) value is 0.
        QCOMPARE(a.serverUnreadCount, 5);
        QCOMPARE(store.totalUnreadCount(true), 0);
    }

    // A FAILED mark-read must revert the optimistic zero back to the server count.
    void failedMarkReadRevertsCount() {
        UnreadStateStore store;
        store.applyRoomListSnapshot({room("!a:x", 5)});
        store.optimisticMarkRead("!a:x");
        QCOMPARE(store.roomState("!a:x").effectiveUnreadCount, 0);

        store.ackMarkRead("!a:x", /*read=*/true, /*requestId=*/0, /*success=*/false);
        const auto a = store.roomState("!a:x");
        QVERIFY(!a.pendingExplicitMarkRead);
        QCOMPARE(a.effectiveUnreadCount, 5);
    }

    // The realistic success path: optimistic zero, then the server snapshot
    // catches up with unread=0, which clears the pending flag and stays at 0.
    void serverSnapshotClearsPendingAfterMarkRead() {
        UnreadStateStore store;
        store.applyRoomListSnapshot({room("!a:x", 5)});
        store.optimisticMarkRead("!a:x");
        store.applyRoomListSnapshot({room("!a:x", 0)});
        const auto a = store.roomState("!a:x");
        QVERIFY(!a.pendingExplicitMarkRead);
        QCOMPARE(a.effectiveUnreadCount, 0);
    }

    void optimisticMarkUnreadSetsEffectiveFlag() {
        UnreadStateStore store;
        store.applyRoomListSnapshot({room("!a:x", 0)});
        const auto rev = store.optimisticMarkUnread("!a:x");
        QVERIFY(rev > 0);
        const auto a = store.roomState("!a:x");
        QVERIFY(a.pendingExplicitMarkUnread);
        QVERIFY(a.effectiveMarkedUnread);
    }

    void emitsRoomChangeSignals() {
        UnreadStateStore store;
        QSignalSpy spy(&store, &UnreadStateStore::roomUnreadStateChanged);
        store.applyRoomListSnapshot({room("!a:x", 5)});
        QVERIFY(spy.count() >= 1);
        spy.clear();
        store.optimisticMarkRead("!a:x");
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).toString(), QString("!a:x"));
    }

    // --- Fresh-count acceptance (docs/unread-counter-fixes-plan.md T3) ---

    // A freshly-recomputed live slice must be trusted downward: before the
    // max() ratchet was dropped this stuck at the stale-high 10 until reopen.
    void timelineSnapshotLowersStaleServerCount() {
        UnreadStateStore store;
        store.applyRoomListSnapshot({room("!r:x", 10)});
        QCOMPARE(store.roomState("!r:x").effectiveUnreadCount, 10);
        store.applyTimelineSnapshot("!r:x", liveSlice(1, "$e1"));
        QCOMPARE(store.roomState("!r:x").effectiveUnreadCount, 1);
    }

    // A partial server reduction below the optimistic value must win (another
    // device read further). A per-room unread snapshot lowers the server count
    // WITHOUT clearing the optimistic pending, so this isolates the qMin.
    void partialServerReductionBeatsPendingOptimistic() {
        UnreadStateStore store;
        store.applyRoomListSnapshot({room("!r:x", 10)});
        store.optimisticReadProgress("!r:x", "$read", "$next", 5);
        QCOMPARE(store.roomState("!r:x").effectiveUnreadCount, 5);
        store.applyRoomUnreadSnapshot("!r:x", unreadSnapshot(3));
        QCOMPARE(store.roomState("!r:x").effectiveUnreadCount, 3);
    }

    // The normal case is unchanged: optimistic still masks a stale-high server.
    void pendingOptimisticStillMasksStaleHighServer() {
        UnreadStateStore store;
        store.applyRoomListSnapshot({room("!r:x", 10)});
        store.optimisticReadProgress("!r:x", "$read", "$next", 2);
        QCOMPARE(store.roomState("!r:x").effectiveUnreadCount, 2);
    }

    // Genuinely new activity (timestamp past the read frontier) still reverts
    // the optimistic read to the server value — must survive the qMin change.
    void newActivityRevertPreserved() {
        UnreadStateStore store;
        store.applyRoomListSnapshot({roomAt("!r:x", 3, 100)});
        store.optimisticReadProgress("!r:x", "$read", "$next", 0);
        QCOMPARE(store.roomState("!r:x").effectiveUnreadCount, 0);
        store.applyRoomListSnapshot({roomAt("!r:x", 5, 200)});
        const auto state = store.roomState("!r:x");
        QCOMPARE(state.pendingOptimisticUnreadCount, -1);
        QCOMPARE(state.effectiveUnreadCount, 5);
    }

    // Explicit mark-read still forces zero regardless of the server value.
    void explicitMarkReadStillForcesZero() {
        UnreadStateStore store;
        store.applyRoomListSnapshot({room("!r:x", 7)});
        store.optimisticMarkRead("!r:x");
        QCOMPARE(store.roomState("!r:x").effectiveUnreadCount, 0);
    }

    // A failed read-receipt ack rolls the optimistic read back to the server
    // value (the success path deliberately keeps pending — anti-flicker).
    void failedReceiptAckRollsBack() {
        UnreadStateStore store;
        store.applyRoomListSnapshot({room("!r:x", 4)});
        const auto revision =
            store.optimisticReadProgress("!r:x", "$read", "$next", 1);
        QCOMPARE(store.roomState("!r:x").effectiveUnreadCount, 1);
        store.bindReadReceiptRequest("!r:x", "$read", revision, quint64(42));
        store.ackReadReceipt("!r:x", "$read", quint64(42), false);
        const auto state = store.roomState("!r:x");
        QCOMPARE(state.pendingOptimisticUnreadCount, -1);
        QCOMPARE(state.effectiveUnreadCount, 4);
    }

    // --- Bug A: timestamp-stamped read frontier (2026-07-04) ---

    // Reading a just-arrived message stamps the read frontier at that message's
    // timestamp, so the debounced room-list snapshot carrying the SAME message
    // (pre-receipt-echo count still 1) must NOT be classified as new activity.
    // Without the ts, readFrontierTs lagged at the prior activity time and the
    // revert fired → the badge flashed 0→1→0 (the reported blink).
    void readProgressTsBlocksRevertForReadMessage() {
        UnreadStateStore store;
        store.applyRoomListSnapshot({roomAt("!r:x", 0, 100)});
        // Message ts=200 arrives and is auto-read at the bottom.
        store.optimisticReadProgress("!r:x", "$new", "", 0, /*readTillTs=*/200);
        QCOMPARE(store.roomState("!r:x").effectiveUnreadCount, 0);
        // Debounced room-list snapshot: pre-echo count 1, timestamp of that
        // just-read message. The revert must not fire.
        store.applyRoomListSnapshot({roomAt("!r:x", 1, 200)});
        QCOMPARE(store.roomState("!r:x").effectiveUnreadCount, 0);
    }

    // Guard against over-suppression: a strictly newer message (past the read
    // frontier) must still revert the optimistic read and raise the badge —
    // this is what makes a scrolled-up arrival count.
    void strictlyNewerActivityStillReverts() {
        UnreadStateStore store;
        store.applyRoomListSnapshot({roomAt("!r:x", 0, 100)});
        store.optimisticReadProgress("!r:x", "$new", "", 0, /*readTillTs=*/200);
        QCOMPARE(store.roomState("!r:x").effectiveUnreadCount, 0);
        store.applyRoomListSnapshot({roomAt("!r:x", 2, 300)});
        const auto state = store.roomState("!r:x");
        QCOMPARE(state.pendingOptimisticUnreadCount, -1);
        QCOMPARE(state.effectiveUnreadCount, 2);
    }

    // Hygiene: a pure count raise (no event actually read → empty readTill)
    // must not mint pending read state or advance the read frontier. The count
    // itself flows through the server-count feeds; a raise is not a read.
    void emptyReadTillMintsNoPendingState() {
        UnreadStateStore store;
        store.applyRoomListSnapshot({room("!r:x", 3)});
        store.optimisticReadProgress("!r:x", "", "", 5);
        const auto state = store.roomState("!r:x");
        QCOMPARE(state.pendingOptimisticUnreadCount, -1);
        QCOMPARE(state.effectiveUnreadCount, 3);
    }

    // --- Read-consuming room gate (2026-07-10) ---
    //
    // While a room is the active, read-consuming one (window focused, parked at
    // the live bottom), an arriving message raises the count internally but must
    // never be DISPLAYED — the auto-read is about to zero it, and showing the
    // raise first is the 0→1→0 blink. The clamp is presentation-only: effective*
    // stays truthful (the read-detector/receipt machinery depends on it), only
    // display* is suppressed.

    // A raise into the gated room is masked in the display fields (and the total)
    // while the effective count still reflects the truth for the widget backflow.
    void readConsumingRoomClampsDisplayOnRaise() {
        UnreadStateStore store;
        store.applyRoomListSnapshot({roomAt("!r:x", 0, 100)});
        store.setActiveRoomId("!r:x");
        store.setReadConsumingRoom("!r:x");
        store.applyTimelineSnapshot("!r:x", liveSlice(1, "$m"));
        const auto state = store.roomState("!r:x");
        QCOMPARE(state.effectiveUnreadCount, 1);
        QCOMPARE(state.displayUnreadCount, 0);
        QCOMPARE(store.totalUnreadCount(/*includeMuted=*/true), 0);
    }

    // The clamp covers every feed, not just the timeline one — the debounced
    // room-list snapshot and the per-room snapshot must be masked too.
    void allThreeFeedsAreClamped() {
        UnreadStateStore store;
        store.applyRoomListSnapshot({roomAt("!r:x", 0, 100)});
        store.setActiveRoomId("!r:x");
        store.setReadConsumingRoom("!r:x");

        store.applyRoomListSnapshot({roomAt("!r:x", 2, 200)});
        QCOMPARE(store.roomState("!r:x").displayUnreadCount, 0);

        store.applyRoomUnreadSnapshot("!r:x", unreadSnapshot(3));
        QCOMPARE(store.roomState("!r:x").displayUnreadCount, 0);
    }

    // The store backflow to the history widget rides activeRoomUnreadStateChanged
    // and must carry the TRUTHFUL effective count (not the clamped display) — the
    // widget's read frontier / down-button count depend on it.
    void activeRoomBackflowStaysTruthful() {
        UnreadStateStore store;
        store.applyRoomListSnapshot({roomAt("!r:x", 0, 100)});
        store.setActiveRoomId("!r:x");
        store.setReadConsumingRoom("!r:x");
        QSignalSpy spy(&store, &UnreadStateStore::activeRoomUnreadStateChanged);
        store.applyTimelineSnapshot("!r:x", liveSlice(1, "$m"));
        QVERIFY(spy.count() >= 1);
        const auto last = spy.takeLast();
        QCOMPARE(last.at(0).toString(), QString("!r:x"));
        QCOMPARE(last.at(1).toInt(), 1);
    }

    // Turning the gate off reveals the real count and notifies listeners so the
    // dialogs row repaints (a display-only change must break StatePresentationEqual).
    void clearingReadConsumingRoomRevealsCount() {
        UnreadStateStore store;
        store.applyRoomListSnapshot({roomAt("!r:x", 0, 100)});
        store.setActiveRoomId("!r:x");
        store.setReadConsumingRoom("!r:x");
        store.applyTimelineSnapshot("!r:x", liveSlice(1, "$m"));
        QCOMPARE(store.roomState("!r:x").displayUnreadCount, 0);

        QSignalSpy spy(&store, &UnreadStateStore::roomUnreadStateChanged);
        store.setReadConsumingRoom(QString());
        QCOMPARE(store.roomState("!r:x").displayUnreadCount, 1);
        QVERIFY(spy.count() >= 1);
    }

    // Only the gated room is clamped; a raise elsewhere shows normally.
    void readConsumingDoesNotAffectOtherRooms() {
        UnreadStateStore store;
        store.applyRoomListSnapshot({roomAt("!a:x", 0, 100), roomAt("!b:x", 0, 100)});
        store.setActiveRoomId("!a:x");
        store.setReadConsumingRoom("!a:x");
        store.applyRoomListSnapshot({roomAt("!a:x", 1, 200), roomAt("!b:x", 4, 200)});
        QCOMPARE(store.roomState("!a:x").displayUnreadCount, 0);
        QCOMPARE(store.roomState("!b:x").displayUnreadCount, 4);
    }

    // Switching the active room away from the gated one drops the clamp for it.
    void switchingActiveRoomClearsGate() {
        UnreadStateStore store;
        store.applyRoomListSnapshot({roomAt("!a:x", 0, 100), roomAt("!b:x", 0, 100)});
        store.setActiveRoomId("!a:x");
        store.setReadConsumingRoom("!a:x");
        store.applyTimelineSnapshot("!a:x", liveSlice(1, "$m"));
        QCOMPARE(store.roomState("!a:x").displayUnreadCount, 0);

        store.setActiveRoomId("!b:x");
        QCOMPARE(store.roomState("!a:x").displayUnreadCount, 1);
    }

    // The marked-unread flag is user intent and is never clamped, even while the
    // room is the read-consuming one (otherwise the row's indicator would vanish
    // and the context menu would offer the wrong action).
    void explicitMarkUnreadVisibleWhileGated() {
        UnreadStateStore store;
        store.applyRoomListSnapshot({roomAt("!r:x", 0, 100)});
        store.setActiveRoomId("!r:x");
        store.setReadConsumingRoom("!r:x");
        store.optimisticMarkUnread("!r:x");
        QVERIFY(store.roomState("!r:x").effectiveMarkedUnread);
    }

    // Multi-account: each account owns a store, and the one dock badge the OS
    // gives us has to speak for all of them — including the accounts whose UI
    // isn't showing, since they keep syncing in the background. This pins the
    // sum (what AccountDomain::totalUnreadBadge does over its accounts) and, in
    // particular, that the muted filter is applied per store rather than after.
    void unreadTotalsFromSeveralAccountsAddUp() {
        UnreadStateStore active;
        active.applyRoomListSnapshot({roomAt("!a:x", 2, 100)});

        const auto muted = room("!c:x", 7, /*highlight=*/0, /*muted=*/true);
        UnreadStateStore background;
        background.applyRoomListSnapshot({roomAt("!b:x", 3, 100), muted});

        const auto sum = [&](bool includeMuted) {
            return active.totalUnreadCount(includeMuted)
                + background.totalUnreadCount(includeMuted);
        };

        // A background account's unread counts toward the badge.
        QCOMPARE(sum(/*includeMuted=*/false), 5);
        // ...and its muted rooms follow the same device-level toggle.
        QCOMPARE(sum(/*includeMuted=*/true), 12);

        // Reading the background account's room lowers the shared badge, even
        // though a different account is the one on screen.
        background.applyRoomListSnapshot({roomAt("!b:x", 0, 100), muted});
        QCOMPARE(sum(/*includeMuted=*/false), 2);
    }
};

QTEST_GUILESS_MAIN(TestUnreadStateStore)
#include "tst_unread_state_store.moc"

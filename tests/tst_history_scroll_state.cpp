// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "history/history_scroll_state.h"

using namespace TeleMatrix;

class TestHistoryScrollState : public QObject {
    Q_OBJECT

private slots:
    void missingRoomReturnsDefault() {
        HistoryScrollStateStore store;
        QVERIFY(!store.has("!none:x"));
        const auto value = store.value("!none:x");
        QVERIFY(value.anchorEventId.isEmpty());
        QCOMPARE(value.anchorPixelOffset, 0);
        QVERIFY(value.wasLive); // struct default
    }

    void savesAndReadsPerRoom() {
        HistoryScrollStateStore store;
        RoomScrollState a;
        a.anchorEventId = "$a";
        a.anchorPixelOffset = 42;
        a.wasLive = false;
        a.focusEventId = "$fa";
        store.save("!a:x", a);

        RoomScrollState b;
        b.anchorEventId = "$b";
        b.anchorPixelOffset = 7;
        store.save("!b:x", b);

        QVERIFY(store.has("!a:x"));
        QVERIFY(store.has("!b:x"));
        // Per-room keying: A and B don't leak into each other.
        QCOMPARE(store.value("!a:x").anchorEventId, QString("$a"));
        QCOMPARE(store.value("!a:x").anchorPixelOffset, 42);
        QCOMPARE(store.value("!a:x").wasLive, false);
        QCOMPARE(store.value("!b:x").anchorEventId, QString("$b"));
        QCOMPARE(store.value("!b:x").anchorPixelOffset, 7);
    }

    void pendingRestoreLifecycle() {
        HistoryScrollStateStore store;
        QVERIFY(!store.hasPendingRestore());

        RoomScrollState pending;
        pending.anchorEventId = "$p";
        pending.anchorPixelOffset = 99;
        store.setPendingRestore(pending);
        QVERIFY(store.hasPendingRestore());
        QCOMPARE(store.pendingRestore().anchorEventId, QString("$p"));
        QCOMPARE(store.pendingRestore().anchorPixelOffset, 99);

        store.clearPendingRestore();
        QVERIFY(!store.hasPendingRestore());
    }
};

QTEST_GUILESS_MAIN(TestHistoryScrollState)
#include "tst_history_scroll_state.moc"

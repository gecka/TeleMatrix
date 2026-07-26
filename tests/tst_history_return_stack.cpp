// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "history/history_return_stack.h"

using namespace TeleMatrix;

class TestHistoryReturnStack : public QObject {
    Q_OBJECT

private slots:
    void emptyStackGuards() {
        HistoryReturnStack stack;
        QVERIFY(!stack.hasReply());
        QCOMPARE(stack.takeReply(), QString());
        QVERIFY(!stack.hasPosition());
        QCOMPARE(stack.takePosition().anchorEventId, QString());
    }

    void ignoresEmptyPushes() {
        HistoryReturnStack stack;
        stack.pushReply(QString());
        QVERIFY(!stack.hasReply());
        stack.pushPosition(HistoryReturnPosition{}); // empty anchor
        QVERIFY(!stack.hasPosition());
    }

    void repliesAreLifo() {
        HistoryReturnStack stack;
        stack.pushReply("$a");
        stack.pushReply("$b");
        stack.pushReply("$c");
        QCOMPARE(stack.takeReply(), QString("$c"));
        QCOMPARE(stack.takeReply(), QString("$b"));
        QCOMPARE(stack.takeReply(), QString("$a"));
        QVERIFY(!stack.hasReply());
    }

    // The one real piece of logic: at capacity (50) the OLDEST entry is dropped.
    void repliesDropOldestAtCapacity() {
        HistoryReturnStack stack;
        // Push 51 distinct ids: e0 (oldest) should be evicted, e1..e50 retained.
        for (int i = 0; i <= 50; ++i) {
            stack.pushReply(QStringLiteral("$e%1").arg(i));
        }
        // LIFO drain yields e50 down to e1 (50 items); e0 was dropped.
        for (int i = 50; i >= 1; --i) {
            QCOMPARE(stack.takeReply(), QStringLiteral("$e%1").arg(i));
        }
        QVERIFY(!stack.hasReply());
    }

    void positionsRoundTripLifoAndDrop() {
        HistoryReturnStack stack;
        stack.pushPosition({"$x", 10});
        stack.pushPosition({"$y", 20});
        QVERIFY(stack.hasPosition());
        QCOMPARE(stack.lastPosition().anchorEventId, QString("$y"));
        QCOMPARE(stack.lastPosition().pixelOffset, 20);
        stack.dropLastPosition();
        QCOMPARE(stack.lastPosition().anchorEventId, QString("$x"));
        const auto taken = stack.takePosition();
        QCOMPARE(taken.anchorEventId, QString("$x"));
        QCOMPARE(taken.pixelOffset, 10);
        QVERIFY(!stack.hasPosition());
    }

    void positionsDropOldestAtCapacity() {
        HistoryReturnStack stack;
        for (int i = 0; i <= 50; ++i) {
            stack.pushPosition({QStringLiteral("$p%1").arg(i), i});
        }
        for (int i = 50; i >= 1; --i) {
            QCOMPARE(stack.takePosition().anchorEventId, QStringLiteral("$p%1").arg(i));
        }
        QVERIFY(!stack.hasPosition());
    }
};

QTEST_GUILESS_MAIN(TestHistoryReturnStack)
#include "tst_history_return_stack.moc"

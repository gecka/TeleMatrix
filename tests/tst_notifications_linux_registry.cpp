// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "window/notifications_linux_registry.h"

using namespace TeleMatrix::Notifications;

class TestNotificationsLinuxRegistry : public QObject {
    Q_OBJECT

private slots:
    void mapsIdToRoom() {
        LinuxNotificationRegistry reg;
        reg.add(QStringLiteral("!a:x"), 7);
        QCOMPARE(reg.roomForId(7), QStringLiteral("!a:x"));
        QVERIFY(reg.roomForId(99).isEmpty());
    }

    void takeRoomReturnsAllAndClears() {
        LinuxNotificationRegistry reg;
        reg.add(QStringLiteral("!a:x"), 1);
        reg.add(QStringLiteral("!a:x"), 2);
        reg.add(QStringLiteral("!b:x"), 3);

        const QList<uint> taken = reg.takeRoom(QStringLiteral("!a:x"));
        QCOMPARE(taken, (QList<uint>{1, 2}));
        // Room A is fully forgotten; B is untouched.
        QVERIFY(reg.roomForId(1).isEmpty());
        QVERIFY(reg.roomForId(2).isEmpty());
        QCOMPARE(reg.roomForId(3), QStringLiteral("!b:x"));
        QVERIFY(reg.takeRoom(QStringLiteral("!a:x")).isEmpty());
    }

    void forgetDropsOnlyThatId() {
        LinuxNotificationRegistry reg;
        reg.add(QStringLiteral("!a:x"), 1);
        reg.add(QStringLiteral("!a:x"), 2);
        reg.forget(1);
        QVERIFY(reg.roomForId(1).isEmpty());
        QCOMPARE(reg.roomForId(2), QStringLiteral("!a:x"));
        QCOMPARE(reg.takeRoom(QStringLiteral("!a:x")), (QList<uint>{2}));
    }

    void takeAllDrainsEverything() {
        LinuxNotificationRegistry reg;
        reg.add(QStringLiteral("!a:x"), 1);
        reg.add(QStringLiteral("!b:x"), 2);
        const QList<uint> all = reg.takeAll();
        QCOMPARE(all.size(), 2);
        QVERIFY(all.contains(1) && all.contains(2));
        QVERIFY(reg.roomForId(1).isEmpty());
        QVERIFY(reg.roomForId(2).isEmpty());
        QVERIFY(reg.takeAll().isEmpty());
    }

    void ignoresEmptyRoomOrZeroId() {
        LinuxNotificationRegistry reg;
        reg.add(QString(), 5);
        reg.add(QStringLiteral("!a:x"), 0);
        QVERIFY(reg.roomForId(5).isEmpty());
        QVERIFY(reg.takeRoom(QStringLiteral("!a:x")).isEmpty());
    }

    void reAddingIdMovesItToNewRoom() {
        LinuxNotificationRegistry reg;
        reg.add(QStringLiteral("!a:x"), 1);
        reg.add(QStringLiteral("!b:x"), 1); // daemon reused id 1
        QCOMPARE(reg.roomForId(1), QStringLiteral("!b:x"));
        QVERIFY(reg.takeRoom(QStringLiteral("!a:x")).isEmpty());
        QCOMPARE(reg.takeRoom(QStringLiteral("!b:x")), (QList<uint>{1}));
    }
};

QTEST_MAIN(TestNotificationsLinuxRegistry)
#include "tst_notifications_linux_registry.moc"

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest/QtTest>

#include "dialogs/saved_messages.h"
#include "protocol/protocol_types.h"

using namespace TeleMatrix;

namespace {
RoomSummary room(const QString &id, const QString &name) {
    RoomSummary r;
    r.roomId = id;
    r.displayName = name;
    return r;
}
} // namespace

class TestSavedMessagesTargets : public QObject {
    Q_OBJECT
private slots:
    void savedRoomMovesFirst() {
        const auto out = SavedMessages::arrangeForwardTargets(
            {room("!a:x", "A"), room("!saved:x", "Saved Messages"), room("!b:x", "B")},
            QStringLiteral("!saved:x"),
            QString());
        QCOMPARE(out.size(), 3);
        QCOMPARE(out[0].roomId, QStringLiteral("!saved:x"));
        QCOMPARE(out[1].roomId, QStringLiteral("!a:x"));
    }

    void missingSavedRoomIsSynthesizedFirst() {
        const auto out = SavedMessages::arrangeForwardTargets(
            {room("!a:x", "A")}, QString(), QString());
        QCOMPARE(out.size(), 2);
        QCOMPARE(out[0].roomId, QString(SavedMessages::kPendingRoomId));
        QCOMPARE(out[0].displayName, SavedMessages::displayName());
    }

    void knownButUncachedSavedRoomUsesRealId() {
        const auto out = SavedMessages::arrangeForwardTargets(
            {room("!a:x", "A")}, QStringLiteral("!saved:x"), QString());
        QCOMPARE(out[0].roomId, QStringLiteral("!saved:x"));
    }

    void currentRoomIsDropped() {
        const auto out = SavedMessages::arrangeForwardTargets(
            {room("!a:x", "A"), room("!saved:x", "S")},
            QStringLiteral("!saved:x"),
            QStringLiteral("!a:x"));
        QCOMPARE(out.size(), 1);
        QCOMPARE(out[0].roomId, QStringLiteral("!saved:x"));
    }

    void savedAsCurrentIsNotDuplicated() {
        const auto out = SavedMessages::arrangeForwardTargets(
            {room("!a:x", "A"), room("!saved:x", "S")},
            QStringLiteral("!saved:x"),
            QStringLiteral("!saved:x"));
        QCOMPARE(out.size(), 1);
        QCOMPARE(out[0].roomId, QStringLiteral("!a:x"));
    }
};

QTEST_MAIN(TestSavedMessagesTargets)
#include "tst_saved_messages_targets.moc"

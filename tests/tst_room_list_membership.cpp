// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "protocol/room_list_membership.h"

using namespace TeleMatrix;
using RoomListMembership::State;

namespace {

[[nodiscard]] RoomSummary room(const QString &id, MembershipState membership) {
    RoomSummary summary;
    summary.roomId = id;
    summary.membership = membership;
    return summary;
}

[[nodiscard]] QVector<RoomSummary> loadedList() {
    return {
        room(QStringLiteral("!joined:example.org"), MembershipState::Join),
        room(QStringLiteral("!left:example.org"), MembershipState::Leave),
    };
}

} // namespace

class TestRoomListMembership : public QObject {
    Q_OBJECT

private slots:
    // The bug this exists for: a background account has never built a room list,
    // so an empty one means "we have not looked", not "you are not a member".
    // Reading it as the latter opened a joined room as an un-joined peek.
    void emptyListIsUnknownNotAbsence() {
        QCOMPARE(
            RoomListMembership::classifyRoom({}, QStringLiteral("!joined:example.org")),
            State::Unknown);
    }

    void joinedRoomInALoadedListIsJoined() {
        QCOMPARE(
            RoomListMembership::classifyRoom(loadedList(), QStringLiteral("!joined:example.org")),
            State::Joined);
    }

    // Present but not a member — the genuine preview case.
    void nonJoinedMembershipIsNotJoined() {
        QCOMPARE(
            RoomListMembership::classifyRoom(loadedList(), QStringLiteral("!left:example.org")),
            State::NotJoined);
    }

    // Absent from a list we DO have is real evidence, unlike absence from no list.
    void absentFromALoadedListIsNotJoined() {
        QCOMPARE(
            RoomListMembership::classifyRoom(loadedList(), QStringLiteral("!never:example.org")),
            State::NotJoined);
    }

    // An empty id can't be looked up; it must not read as absence either.
    void emptyRoomIdIsUnknown() {
        QCOMPARE(RoomListMembership::classifyRoom(loadedList(), QString()), State::Unknown);
    }

    // Preview is only ever right for known non-membership. Unknown falls through
    // to a normal open, which self-corrects once the list loads.
    void onlyKnownNonMembershipPreviews() {
        QVERIFY(RoomListMembership::shouldOpenAsPreview(
            loadedList(), QStringLiteral("!never:example.org")));
        QVERIFY(RoomListMembership::shouldOpenAsPreview(
            loadedList(), QStringLiteral("!left:example.org")));
        QVERIFY(!RoomListMembership::shouldOpenAsPreview(
            loadedList(), QStringLiteral("!joined:example.org")));
        QVERIFY(!RoomListMembership::shouldOpenAsPreview(
            {}, QStringLiteral("!joined:example.org")));
    }
};

QTEST_MAIN(TestRoomListMembership)
#include "tst_room_list_membership.moc"

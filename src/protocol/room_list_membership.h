// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "protocol/protocol_types.h"

#include <QString>
#include <QVector>

#include <algorithm>

namespace TeleMatrix::RoomListMembership {

// Whether the cached room list says we are in a room, extracted so the
// three-way answer is explicit and unit-testable.
//
// The distinction that matters is Unknown vs NotJoined. A bridge's room list is
// empty until that account has been PRESENTED — background accounts sync without
// ever building one — so "the room is not in the list" can mean either "you are
// not a member" or "we have not looked yet". Reading the second as the first is
// what opened a joined room as an un-joined peek (no composer, a Join bar, and
// deliberately no unread-store registration, so no delimiter and no read
// marking) when a notification click switched onto a background account.
enum class State {
    Unknown,   // list not loaded — decide nothing from this
    Joined,
    NotJoined, // genuinely absent from, or non-joined in, a loaded list
};

[[nodiscard]] inline State classifyRoom(
        const QVector<RoomSummary> &rooms,
        const QString &roomId) {
    if (roomId.isEmpty() || rooms.isEmpty()) {
        return State::Unknown;
    }
    const auto it = std::find_if(rooms.cbegin(), rooms.cend(),
        [&](const RoomSummary &room) { return room.roomId == roomId; });
    if (it == rooms.cend()) {
        return State::NotJoined;
    }
    return (it->membership == MembershipState::Join)
        ? State::Joined
        : State::NotJoined;
}

// Preview is only ever right for a room we know we are not in. Unknown falls
// through to a normal open: showing the real room is the safer failure, and it
// self-corrects once the list loads.
[[nodiscard]] inline bool shouldOpenAsPreview(
        const QVector<RoomSummary> &rooms,
        const QString &roomId) {
    return classifyRoom(rooms, roomId) == State::NotJoined;
}

} // namespace TeleMatrix::RoomListMembership

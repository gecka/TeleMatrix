// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "protocol/protocol_types.h"

#include <QString>
#include <QVector>

namespace TeleMatrix::SavedMessages {

/// Sentinel roomId for the forward row shown before the room exists;
/// selecting it must trigger ensureSavedMessagesRoom() first.
inline constexpr QLatin1String kPendingRoomId{"::saved-messages-pending::"};

[[nodiscard]] QString displayName();

/// Forward-dialog target order, tdesktop-style: Saved Messages always first
/// (synthesized when absent), the room being forwarded from dropped.
[[nodiscard]] QVector<RoomSummary> arrangeForwardTargets(
    const QVector<RoomSummary> &rooms,
    const QString &savedRoomId,
    const QString &currentRoomId);

} // namespace TeleMatrix::SavedMessages

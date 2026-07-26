// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QVector>

#include "../protocol/protocol_types.h"

namespace TeleMatrix {

/// Filter a space's rooms by name or topic, case-insensitively. An empty needle keeps everything.
///
/// This exists because Matrix has no server-side search inside a space: `/hierarchy` lists a space's
/// children but takes no search term, so the client pages them in and matches locally. That makes
/// this the one piece of real logic in the space view — hence its own file, and its own test.
[[nodiscard]] QVector<RoomDirectoryEntry> filterRoomEntries(
    const QVector<RoomDirectoryEntry> &entries,
    const QString &needle);

} // namespace TeleMatrix

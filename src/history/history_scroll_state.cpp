// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_scroll_state.h"

namespace TeleMatrix {

void HistoryScrollStateStore::save(const QString &roomId, const RoomScrollState &state) {
    if (!roomId.isEmpty()) {
        _states.insert(roomId, state);
    }
}

bool HistoryScrollStateStore::has(const QString &roomId) const {
    return _states.contains(roomId);
}

RoomScrollState HistoryScrollStateStore::value(const QString &roomId) const {
    return _states.value(roomId);
}

void HistoryScrollStateStore::setPendingRestore(const RoomScrollState &state) {
    _pendingRestore = state;
}

void HistoryScrollStateStore::clearPendingRestore() {
    _pendingRestore.reset();
}

bool HistoryScrollStateStore::hasPendingRestore() const {
    return _pendingRestore.has_value();
}

RoomScrollState HistoryScrollStateStore::pendingRestore() const {
    return _pendingRestore.value();
}

} // namespace TeleMatrix

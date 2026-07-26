// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QVector>

namespace TeleMatrix::UnreadRead {

// Pure "how far has the user read" decision, extracted from
// HistoryList::checkReadProgress so the reset-while-scrolled-up hazard is
// unit-testable without a Qt widget.
//
// Given each loaded row's bottom edge (`rowBottoms[i]`, in the same content
// coordinates as the viewport, ascending) and the read-detection anchor index
// `frontierIdx`, return the LAST index at/after the anchor whose bottom edge
// still ends inside the viewport — i.e. the newest message the user has fully
// scrolled past. Returns -1 when nothing qualifies, when the anchor is not
// loaded (`frontierIdx < 0` or out of range), or when detection is `held`.
//
// `held` is the guard for programmatic viewport moves: a gappy sync resets the
// timeline to a short window, the scrollbar clamps a deep scroll offset to the
// new (small) maximum, and the viewport suddenly spans the whole content — which
// would otherwise sweep the frontier to the newest row and mark unseen messages
// read. While held, no read is detected until genuine user input clears it.
//
// The row bottoms are supplied through a `bottomAt(i)` accessor rather than a
// container so the caller (checkReadProgress, on the hot scroll path) reads them
// straight out of its layout with no allocation.
template <typename BottomAt>
[[nodiscard]] int readTillIndex(
        int rowCount,
        BottomAt &&bottomAt,
        int frontierIdx,
        int viewportBottom,
        bool held) {
    if (held || frontierIdx < 0 || frontierIdx >= rowCount) {
        return -1;
    }
    int readTill = -1;
    for (int i = frontierIdx; i < rowCount; ++i) {
        if (bottomAt(i) > viewportBottom) {
            break;
        }
        readTill = i;
    }
    return readTill;
}

// Convenience overload for callers/tests that already hold the bottoms.
[[nodiscard]] inline int readTillIndex(
        const QVector<int> &rowBottoms,
        int frontierIdx,
        int viewportBottom,
        bool held) {
    return readTillIndex(
        rowBottoms.size(),
        [&](int i) { return rowBottoms[i]; },
        frontierIdx,
        viewportBottom,
        held);
}

} // namespace TeleMatrix::UnreadRead

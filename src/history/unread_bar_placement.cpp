// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history/unread_bar_placement.h"

namespace TeleMatrix::UnreadBar {

bool canPlaceUnreadBarAt(
        const QVector<PlacementRow> &rows,
        const QString &anchor,
        bool boundaryConfirmed) {
    if (anchor.isEmpty()) {
        return false;
    }
    // True once a visible regular (non-service) message has been seen before the
    // anchor — proof the window has loaded past the read boundary.
    bool skippedRegularMessage = false;
    for (const auto &row : rows) {
        if (row.eventId == anchor) {
            // Confirmed boundary → place regardless; otherwise fall back to the
            // "a read message precedes it" heuristic.
            return boundaryConfirmed || skippedRegularMessage;
        }
        if (!row.eventId.isEmpty() && !row.isService) {
            skippedRegularMessage = true;
        }
    }
    return false; // anchor not among the loaded rows
}

Action decide(
        const QString &drawnBarId,
        bool drawnLoaded,
        int unreadCount) {
    const bool hasDrawn = !drawnBarId.isEmpty() && drawnLoaded;
    if (unreadCount > 0) {
        // Bar already shown and its anchor still loaded → keep it exactly where
        // it is (only the count changes). Otherwise (re)place at the frozen
        // anchor via resolveAnchor(); the executor withholds until placeable.
        return hasDrawn ? Action::KeepDrawn : Action::Place;
    }
    // unreadCount <= 0: keep the already-crossed bar after the count
    // clears. Crucially, when the drawn anchor is no longer loaded we drop only
    // the visible bar and KEEP the frozen session anchor, so the delimiter
    // re-materialises at the same event when it reloads — it must never be
    // re-derived from the drifting frontier (that was the "jumps down" bug).
    if (hasDrawn) {
        return Action::KeepDrawn;
    }
    if (!drawnBarId.isEmpty()) {
        return Action::ClearDrawnKeepAnchor;
    }
    return Action::NoOp;
}

} // namespace TeleMatrix::UnreadBar

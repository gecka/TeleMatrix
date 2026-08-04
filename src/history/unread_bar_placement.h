// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QString>
#include <QVector>

namespace TeleMatrix::UnreadBar {

// One loaded (already presentation-filtered) timeline row, reduced to what
// placement needs: its event id and whether it is a service/system message.
struct PlacementRow {
    QString eventId;
    bool isService = false;
};

// Whether the "Unread messages" bar may be placed at `anchor` given the loaded
// rows. Normally it requires a visible regular (non-service) message BEFORE the
// anchor — the anti-"jumping-bar" heuristic that withholds the bar while the
// window may not yet have loaded past the read boundary. But that heuristic is
// blind to service messages hidden by the "hide system messages in public
// rooms" filter: when the read region is service events they are erased from
// `rows`, so nothing regular precedes the anchor and the bar is wrongly
// withheld. `boundaryConfirmed` (the read marker IS loaded in the window)
// short-circuits the heuristic: a confirmed boundary means the anchor is the
// TRUE first-unread, so it is always safe to place. `anchor` must be one of
// `rows` (the caller checks it is loaded first); returns false otherwise.
[[nodiscard]] bool canPlaceUnreadBarAt(
    const QVector<PlacementRow> &rows,
    const QString &anchor,
    bool boundaryConfirmed);

// Pure decision logic for the "Unread messages" delimiter position, extracted
// from HistoryWidget so the freeze invariant is unit-testable (no Qt widget /
// timeline-list dependency; event ids are opaque strings).
//
// The delimiter is pinned per room session: once an anchor is captured it must
// never slide to the live (drifting) first-unread frontier — not when the user
// reads part of the room, not when the unread count reaches zero, and not when
// the anchor's message is transiently dropped from the loaded window. Only a
// genuine session boundary (leave/switch room, send, scroll-to-bottom / fully
// read) clears the anchor. See docs/unread-delimiter-freeze-fix-plan.md.

// Which event the bar should (re)place at. This is the single point where the
// drifting frontier may enter placement — and only *before* an anchor has been
// frozen. Once `frozenAnchor` is non-empty it always wins.
[[nodiscard]] inline QString resolveAnchor(
        const QString &frozenAnchor,
        const QString &frontier) {
    return frozenAnchor.isEmpty() ? frontier : frozenAnchor;
}

// Which event read *detection* anchors at. The advancing frontier drives it;
// only before the first frontier push (initial entry) does it fall back to the
// frozen visual bar. This is the mirror image of resolveAnchor() — detection
// follows the frontier so scroll-reading keeps decrementing the count even when
// the visual bar is frozen elsewhere, consumed, or not currently drawn.
[[nodiscard]] inline QString detectorAnchor(
        const QString &frontier,
        const QString &drawnBar) {
    return frontier.isEmpty() ? drawnBar : frontier;
}

enum class Action {
    NoOp,                 // leave the bar as-is
    KeepDrawn,            // redraw at the currently-drawn id (refresh count only)
    Place,                // (re)place via resolveAnchor(); executor applies gating
    ClearDrawnKeepAnchor, // undraw the list bar but KEEP the frozen anchor
};

// Decision for a store / live unread-state update. `drawnBarId` is the id the
// list currently draws the bar at (empty when none) and `drawnLoaded` whether
// that message is still in the loaded window. The frozen anchor is deliberately
// NOT an input: it is never cleared here — only a session boundary clears it.
[[nodiscard]] Action decide(
    const QString &drawnBarId,
    bool drawnLoaded,
    int unreadCount);

// Whether a live slice may latch the session's delimiter decision (after which
// placement is frozen for as long as the room stays open).
//
// Settling initial entry is necessary but NOT sufficient. Two ways the room can
// look settled while the delimiter is still undecided: a cold room's first slice
// is a live placeholder with no unread state (so "no unreads to mark" is
// unknown, not established), and initial entry force-settles after its attempt
// cap even when the anchor never loaded and nothing was drawn. Latching in
// either case freezes the room bar-less for the whole session — the reported
// "missing delimiter". So while the room has unreads and no bar is drawn, stay
// unlatched and let later slices place it once the anchor loads. Nothing slides:
// placement freezes the anchor on first draw (resolveAnchor).
[[nodiscard]] inline bool shouldResolveOnLiveSlice(
        bool unreadStateKnown,
        bool initialScrollNeeded,
        int unreadCount,
        const QString &drawnBarId) {
    return unreadStateKnown
        && !initialScrollNeeded
        && (unreadCount <= 0 || !drawnBarId.isEmpty());
}

// Whether the read detector may be armed at all. Entry-settled is load-bearing:
// the detector measures the viewport, and during initial room entry the
// viewport is transient — the fresh window renders at the pre-entry scroll
// offset while the entry scroll (bottom / saved position / unread bar / jump)
// is still queued. Arming there "reads" whatever rows happen to sit in that
// transient viewport: receipts go out for messages never seen, the frontier
// advances, and the delimiter is later placed at the drifted frontier (above
// only the newest remnant) or suppressed outright once the count hit zero.
[[nodiscard]] inline bool canMarkMessagesRead(
        bool windowActive,
        bool roomOpen,
        bool entryScrollSettled) {
    return windowActive && roomOpen && entryScrollSettled;
}

} // namespace TeleMatrix::UnreadBar

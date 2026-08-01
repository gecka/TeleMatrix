// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

namespace TeleMatrix::JumpRouting {

// Where a notification click should land, extracted from HistoryWidget so the
// live-vs-focused decision is unit-testable without a Qt widget or an SDK
// timeline.
//
// A notification target is not a normal jump target. Search hits, permalinks and
// pinned messages point deep into history, so they are served by a FOCUSED
// timeline (/context around the event, is_live = false). A toast is about the
// message that just arrived — it is the live tail. Serving it focused detaches
// the room from live sync: no new appends, no stick-to-bottom, the scroll-down
// button pinned visible, and the read paths that gate on live never fire. Worse,
// the focused window survives room switches by design, so one click leaves the
// room detached until the user presses scroll-down (which is returnToLive).
//
// So: prefer live, and only fall back to a focused fetch when the target really
// is outside the live window (an old toast clicked much later).
enum class Route {
    InstantScroll,             // already live and loaded: scroll + highlight now
    ReturnToLiveThenHighlight, // same room, focused: drop the focused window first
    LiveOpenThenHighlight,     // other room: plain live open, highlight on arrival
    FocusFetch,                // genuinely out of the live window: /context fetch
};

[[nodiscard]] constexpr Route routeNotificationJump(
        bool sameRoom,
        bool isLive,
        bool targetLoaded) {
    // isLive/targetLoaded describe the room on screen, so they say nothing about
    // a different one — that always starts as a plain live open.
    if (!sameRoom) {
        return Route::LiveOpenThenHighlight;
    }
    if (!isLive) {
        return Route::ReturnToLiveThenHighlight;
    }
    return targetLoaded ? Route::InstantScroll : Route::FocusFetch;
}

// Whether a live slice that arrived without the pending target should give up on
// staying live and fetch it focused instead. `preferLive` is the one-shot arming
// flag: the caller clears it here so a room whose target never loads escalates
// once rather than on every subsequent slice.
[[nodiscard]] constexpr bool shouldEscalateToFocusFetch(
        bool preferLive,
        bool sliceIsLive,
        bool targetLoaded) {
    return preferLive && sliceIsLive && !targetLoaded;
}

} // namespace TeleMatrix::JumpRouting

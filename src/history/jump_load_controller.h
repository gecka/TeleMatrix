// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

namespace TeleMatrix {

// Pure decision logic for one "jump to message" loading episode. The timeline
// shows an opaque "Loading…" cover until BOTH the minimum-display floor has
// elapsed AND the target event has arrived, then reveals. A fetch failure (or
// watchdog timeout) before the target arrives falls back to the live timeline.
// No Qt/timer dependency: the owner drives it with events and applies the
// returned Action. See docs/jump-to-message-redesign-design.md.
class JumpLoadController {
public:
    enum class Action {
        None,      // keep the cover up
        Reveal,    // drop the cover, scroll + highlight the target
        Fallback,  // drop the cover, return to live + toast
    };

    // Start a new episode (also supersedes any current one).
    void begin();

    // The minimum-display floor timer fired.
    [[nodiscard]] Action onFloorElapsed();
    // The focused slice arrived and the target event is now in the list.
    [[nodiscard]] Action onTargetArrived();
    // The focused fetch failed, or the visibility watchdog timed out.
    [[nodiscard]] Action onFetchFailed();

    [[nodiscard]] bool active() const { return _active; }
    void reset();

private:
    [[nodiscard]] Action maybeReveal();
    bool _active = false;
    bool _floorElapsed = false;
    bool _targetReady = false;
};

} // namespace TeleMatrix

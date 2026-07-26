// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history/jump_load_controller.h"

namespace TeleMatrix {

void JumpLoadController::begin() {
    _active = true;
    _floorElapsed = false;
    _targetReady = false;
}

JumpLoadController::Action JumpLoadController::onFloorElapsed() {
    if (!_active) {
        return Action::None;
    }
    _floorElapsed = true;
    return maybeReveal();
}

JumpLoadController::Action JumpLoadController::onTargetArrived() {
    if (!_active) {
        return Action::None;
    }
    _targetReady = true;
    return maybeReveal();
}

JumpLoadController::Action JumpLoadController::onFetchFailed() {
    // A failure after the target already arrived is spurious (the caller has
    // disarmed the watchdog) — keep the episode alive for the floor reveal.
    if (!_active || _targetReady) {
        return Action::None;
    }
    _active = false;
    return Action::Fallback;
}

JumpLoadController::Action JumpLoadController::maybeReveal() {
    if (_floorElapsed && _targetReady) {
        _active = false;
        return Action::Reveal;
    }
    return Action::None;
}

void JumpLoadController::reset() {
    _active = false;
    _floorElapsed = false;
    _targetReady = false;
}

} // namespace TeleMatrix

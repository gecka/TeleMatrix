// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "media/video_rebuffer.h"

namespace TeleMatrix {
namespace {
constexpr qint64 kInitialBufferMs = 2000;  // buffer this much before the first play
constexpr qint64 kRebufferLowMs = 1000;    // pause when < 1s is buffered ahead
constexpr qint64 kRebufferResumeMs = 5000; // resume (after a stall) when >= 5s ahead
} // namespace

VideoRebufferController::Action VideoRebufferController::evaluate(
        float downloadedFraction,
        qint64 positionMs,
        qint64 durationMs,
        bool streaming,
        bool userPaused,
        bool playbackReady) {
    if (!streaming || userPaused || !playbackReady || durationMs <= 0) {
        return Action::None;
    }
    if (downloadedFraction >= 1.0f) {
        // Fully downloaded — never (re)buffer; resume if we were waiting.
        _startedPlayback = true;
        if (_rebuffering) {
            _rebuffering = false;
            return Action::Play;
        }
        return Action::None;
    }
    const auto downloadedMs = qint64(double(downloadedFraction) * durationMs);
    const auto aheadMs = downloadedMs - positionMs;
    if (_rebuffering) {
        // Resume target: a small initial buffer for fast startup, a bigger one
        // after a mid-playback stall so it doesn't immediately re-stall.
        const auto resumeTarget =
            _startedPlayback ? kRebufferResumeMs : kInitialBufferMs;
        if (aheadMs >= resumeTarget) {
            _rebuffering = false;
            _startedPlayback = true;
            return Action::Play;
        }
        return Action::None;
    }
    if (aheadMs < kRebufferLowMs) {
        // Buffer is thin — pause and wait for it to fill. Before the first
        // sustained play this is the initial buffer.
        _rebuffering = true;
        return Action::Pause;
    }
    // Playing with a comfortable lead — the initial-buffer phase is over.
    _startedPlayback = true;
    return Action::None;
}

void VideoRebufferController::reset() {
    _rebuffering = false;
    _startedPlayback = false;
}

} // namespace TeleMatrix

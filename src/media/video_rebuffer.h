// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QtGlobal>

namespace TeleMatrix {

// Proactive (re)buffering state machine for progressive video streaming, shared
// by the inline timeline player and the fullscreen overlay. Pure logic (no Qt
// Multimedia): the caller polls evaluate() and applies the returned action to
// its QMediaPlayer, and uses waiting() to show a buffering spinner.
//
// ExoPlayer-style watermarks: a small initial buffer for fast startup, a bigger
// resume buffer after a mid-playback stall (hysteresis, so it doesn't
// immediately re-stall). The buffered-ahead time is estimated from the proxy's
// download fraction (which keeps growing even while the player is paused, since
// the on-disk cache fills independently of the player).
class VideoRebufferController {
public:
    enum class Action { None, Pause, Play };

    // downloadedFraction: 0..1 of the file the proxy has downloaded.
    // playbackReady: a frame has been shown (playback is underway).
    // Returns the action the caller should apply to its player.
    [[nodiscard]] Action evaluate(
        float downloadedFraction,
        qint64 positionMs,
        qint64 durationMs,
        bool streaming,
        bool userPaused,
        bool playbackReady);

    // True while paused waiting for the buffer to fill (show the spinner).
    [[nodiscard]] bool waiting() const { return _rebuffering; }

    void reset();

private:
    bool _rebuffering = false;
    bool _startedPlayback = false;
};

} // namespace TeleMatrix

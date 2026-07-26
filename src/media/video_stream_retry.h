// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "media/video_container.h"

#include <QtGlobal>

#include <chrono>

namespace TeleMatrix {

// Poll-driven wait state machine used after a streaming read error: wait for the
// background proxy download to catch up, then replay; restart a download that has
// gone dead; and bound the whole wait. Pure logic (no Qt Multimedia, no timers) so
// it is shared by the inline timeline player and the fullscreen overlay and is unit
// tested. The caller owns the poll timer, the QMediaPlayer teardown/replay, and the
// separate per-video errorOccurred budget (kMaxStreamRetries).
//
// Each poll the caller feeds the proxy's download state to tick() and applies the
// returned action:
//   Wait   — keep polling (optionally show a buffering spinner).
//   Replay — stop polling and re-open the source (from the now-complete cache, or
//            to restart a dead download).
//   GiveUp — stop polling and fall back (local file) or surface a terminal error.
class VideoStreamRetryController {
public:
    enum class Action { Wait, Replay, GiveUp };

    struct Tunables {
        // Consecutive no-progress polls before a stalled download is declared dead
        // and restarted. Keep the caller's poll cadence × stallTicks LONGER than the
        // Rust upstream READ_TIMEOUT (30s, see media_stream/upstream.rs) so a merely
        // slow-but-alive stream is never mistaken for dead.
        int stallTicks = 70;      // ~35s at a 500ms cadence
        int maxWaitTicks = 2400;  // ~20min backstop on a single wait
        int maxErrorRestarts = 2; // proxy-reported-error restarts per video
    };

    // Two constructors rather than a `= {}` default argument: a nested struct's
    // default member initializers can't be used in a default argument within the
    // same class definition.
    VideoStreamRetryController();
    explicit VideoStreamRetryController(Tunables tunables);

    // Enter a fresh wait (call when (re)starting the poll timer). Resets the
    // per-wait tick/stall counters but preserves the per-video error-restart budget.
    void beginWait();

    // Advance one poll. progressKnown=false means the proxy couldn't report bytes
    // (treated like no progress). entryErrored=true means the proxy's download has
    // failed outright (fast path: replay to restart it, then give up once the
    // per-video budget is spent). See the .cpp for the exact decision order.
    [[nodiscard]] Action tick(
        bool progressKnown,
        quint64 downloaded,
        quint64 total,
        bool entryErrored);

    // New video: reset everything, including the error-restart budget.
    void reset();

private:
    Tunables _t;
    int _waitTicks = 0;
    int _stallTicks = 0;
    quint64 _lastBytes = 0;
    int _errorRestarts = 0;
};

// FFmpeg's network read blocks until the linear download reaches the requested
// offset, so the worst-case idle wait scales with total bytes. The byte size isn't
// known before the stream opens, so use the clip length as a proxy: a base budget
// plus a per-second allowance, clamped. Shared by both players so their timeouts
// (and the retry controller's stall window that must stay longer) stay in lockstep.
[[nodiscard]] std::chrono::milliseconds streamNetworkTimeout(qint64 durationMs);

// Is a QMediaPlayer error on a proxy-streamed video worth waiting out the download
// and replaying, or is it hopeless? Called from errorOccurred, before the per-video
// retry budget is consulted. `isResourceError` is
// `error == QMediaPlayer::ResourceError` (the enum lives in Qt Multimedia, which
// this library doesn't link).
//
// Shared by both players — the two used to hand-roll this condition and drifted:
// the inline one shipped a guard that was dead code for three days.
[[nodiscard]] bool shouldRetryStreamError(
    bool isResourceError,
    bool hasFrame,
    VideoContainer container);

} // namespace TeleMatrix

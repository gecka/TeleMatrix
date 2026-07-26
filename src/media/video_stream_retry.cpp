// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "media/video_stream_retry.h"

namespace TeleMatrix {

VideoStreamRetryController::VideoStreamRetryController()
    : VideoStreamRetryController(Tunables{}) {}

VideoStreamRetryController::VideoStreamRetryController(Tunables tunables)
    : _t(tunables) {}

void VideoStreamRetryController::beginWait() {
    _waitTicks = 0;
    _stallTicks = 0;
    _lastBytes = 0;
    // _errorRestarts intentionally survives — it's a per-video budget cleared only
    // by reset() (a new video), not per wait.
}

VideoStreamRetryController::Action VideoStreamRetryController::tick(
        bool progressKnown, quint64 downloaded, quint64 total, bool entryErrored) {
    // 1. Total-wait backstop so a pathological trickle can't spin forever.
    if (++_waitTicks > _t.maxWaitTicks) {
        return Action::GiveUp;
    }
    // 2. Proxy reported an outright failure: replay to restart the download until
    //    the per-video budget is spent, then give up. This is the fast path that
    //    avoids waiting out the whole stall window on an already-dead stream.
    if (entryErrored) {
        if (_errorRestarts < _t.maxErrorRestarts) {
            ++_errorRestarts;
            return Action::Replay;
        }
        return Action::GiveUp;
    }
    // 3. Fully downloaded → replay from the complete cache (reads no longer block,
    //    so the read timeout can't recur).
    if (progressKnown && total > 0 && downloaded >= total) {
        return Action::Replay;
    }
    // 4. Forward byte progress → still alive, keep waiting and reset the stall count.
    if (progressKnown && downloaded > _lastBytes) {
        _lastBytes = downloaded;
        _stallTicks = 0;
        return Action::Wait;
    }
    // 5. No progress this tick (or the proxy couldn't report bytes). Past the stall
    //    window the download is dead, not merely slow → replay to restart it.
    if (++_stallTicks >= _t.stallTicks) {
        return Action::Replay;
    }
    return Action::Wait;
}

void VideoStreamRetryController::reset() {
    _waitTicks = 0;
    _stallTicks = 0;
    _lastBytes = 0;
    _errorRestarts = 0;
}

std::chrono::milliseconds streamNetworkTimeout(qint64 durationMs) {
    using namespace std::chrono;
    constexpr auto kBase = milliseconds(60000);
    constexpr auto kPerVideoSecond = milliseconds(2000);
    constexpr auto kMax = milliseconds(240000);
    auto budget = kBase;
    if (durationMs > 0) {
        budget += kPerVideoSecond * (durationMs / 1000);
    }
    return budget > kMax ? kMax : budget;
}

bool shouldRetryStreamError(
        bool isResourceError,
        bool hasFrame,
        VideoContainer container) {
    // A read timeout, or the aborted connection the proxy raises when a download
    // dies mid-body (media_stream/server.rs turns that into an io::Error frame
    // precisely so it surfaces here as ResourceError). Either way the bytes aren't
    // all here yet: wait for the download and replay.
    if (isResourceError) {
        return true;
    }
    // A frame already rendered, so the stream itself works; a later failure is
    // specific to this video. Fall back rather than loop.
    if (hasFrame) {
        return false;
    }
    // Pre-frame and not a ResourceError, i.e. FormatError ("Failed to load media").
    // Because a truncated or dead download arrives as ResourceError (above), a
    // header that walked as a real container means the player was handed sound
    // bytes and rejected them — an unsupported codec. A replay would re-download
    // byte-for-byte identical data and fail identically, so don't pay for it.
    switch (container) {
    case VideoContainer::Faststart:
    case VideoContainer::MoovAtEnd:
        return false;
    case VideoContainer::Unknown:
        break;
    }
    // Unclassified: the head may not have been read yet, or it decrypted to garbage
    // (an unknown MediaSource makes the proxy treat an encrypted file as plain).
    // Neither is proof the bytes are sound, so keep the bounded retry.
    return true;
}

} // namespace TeleMatrix

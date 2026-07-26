// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QtGlobal>

namespace TeleMatrix {

// Pure retry/backoff rules for the media cache's failure memory. No Qt widgets,
// no clock of its own (the caller supplies `nowMs`) so it is headless-testable.
// Failure classes:
//   - Transient resolve (network) failures — timeout, 5xx, rate-limit, and any
//     auth/proxy hiccup — back off exponentially but never become permanent; a
//     transiently-dead host recovers once it comes back.
//   - Permanent resolve failures — HTTP 404, surfaced as `terminal` from Rust —
//     never retry: the media is gone, so retrying only burns requests. Cleared only
//     when the source changes (new insert / invalidate / clearAll).
//   - Decode failures get a tiny fixed budget then stop, because a byte stream
//     that won't decode won't start decoding on its own; the budget resets only
//     when the source bytes/path change (new insert / invalidate / clearAll).
struct MediaRetryPolicy {
    // Backoff before the next resolve attempt is allowed, given how many have
    // already failed. 0 attempts -> 0ms. Doubles from a 2s base, capped at 10min.
    [[nodiscard]] static qint64 retryDelayMs(int attempts);

    // True when a resolve retry is due. A `permanent` failure is never retried.
    // Otherwise: never-failed, or `nowMs` has reached
    // `lastFailureAtMs + retryDelayMs(attempts)`. A backward clock never permits
    // an early retry.
    [[nodiscard]] static bool retryAllowed(
        bool permanent, int attempts, qint64 lastFailureAtMs, qint64 nowMs);

    // Consecutive decode failures tolerated before a key is declared unavailable.
    static constexpr int kDecodeAttemptBudget = 2;

    // True while decode attempts remain within the budget.
    [[nodiscard]] static bool decodeAllowed(int decodeFailures);

    // Resolve failures tolerated before a still-loading placeholder stops ANIMATING.
    // Retrying continues quietly underneath; this only bounds the cost of the pulse.
    static constexpr int kGlowAttemptBudget = 3;

    // True while a still-loading placeholder should animate. A pulsing placeholder
    // drives a ~60fps repaint of its row, and a transient failure never becomes
    // permanent — so a host that simply keeps timing out would otherwise pulse (and
    // repaint) forever. After a few failures the caller paints a static skeleton
    // instead while `retryAllowed` keeps retrying in the background.
    [[nodiscard]] static bool glowAllowed(bool permanent, int attempts);
};

} // namespace TeleMatrix

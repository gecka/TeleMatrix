// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "media_retry_policy.h"

namespace TeleMatrix {

qint64 MediaRetryPolicy::retryDelayMs(int attempts) {
    if (attempts <= 0) {
        return 0;
    }
    constexpr qint64 kBaseMs = 2000;    // first retry after 2s
    constexpr qint64 kMaxMs = 600000;   // cap at 10min
    // 2000 << 9 == 1024000 already exceeds the cap, so past attempt 9 the shift
    // would only grow toward overflow — short-circuit to the cap instead.
    if (attempts >= 10) {
        return kMaxMs;
    }
    return qMin(kBaseMs << (attempts - 1), kMaxMs);
}

bool MediaRetryPolicy::retryAllowed(
        bool permanent, int attempts, qint64 lastFailureAtMs, qint64 nowMs) {
    if (permanent) {
        return false;
    }
    if (attempts <= 0) {
        return true;
    }
    // A backward clock (nowMs < lastFailureAtMs) yields a negative elapsed, which
    // is correctly < any positive delay — no early retry.
    return (nowMs - lastFailureAtMs) >= retryDelayMs(attempts);
}

bool MediaRetryPolicy::decodeAllowed(int decodeFailures) {
    return decodeFailures < kDecodeAttemptBudget;
}

bool MediaRetryPolicy::glowAllowed(bool permanent, int attempts) {
    return !permanent && attempts < kGlowAttemptBudget;
}

} // namespace TeleMatrix

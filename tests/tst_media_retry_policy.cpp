// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "protocol/media_retry_policy.h"

using namespace TeleMatrix;

class TestMediaRetryPolicy : public QObject {
    Q_OBJECT

private slots:
    // No failures recorded yet -> no delay, retry allowed immediately.
    void zeroAttemptsHasNoDelay() {
        QCOMPARE(MediaRetryPolicy::retryDelayMs(0), qint64(0));
        QVERIFY(MediaRetryPolicy::retryAllowed(
            /*permanent=*/false, 0, /*lastFailureAt=*/0, /*now=*/0));
    }

    // A permanent (terminal 4xx) failure is never retried, regardless of attempt
    // count or how much time has elapsed — this is the 404 fix.
    void permanentFailureNeverRetries() {
        QVERIFY(!MediaRetryPolicy::retryAllowed(
            /*permanent=*/true, /*attempts=*/1, /*lastFailureAt=*/0, /*now=*/0));
        // Even far past any backoff window it stays suppressed.
        QVERIFY(!MediaRetryPolicy::retryAllowed(
            /*permanent=*/true, 1, 1000, 10'000'000));
        // And on the very first failure (attempts still 0 would be transient, but a
        // terminal classification overrides even that).
        QVERIFY(!MediaRetryPolicy::retryAllowed(
            /*permanent=*/true, 0, 0, 0));
    }

    // Exponential progression from a 2s base, doubling per attempt.
    void delayProgressionDoublesFromBase() {
        QCOMPARE(MediaRetryPolicy::retryDelayMs(1), qint64(2000));
        QCOMPARE(MediaRetryPolicy::retryDelayMs(2), qint64(4000));
        QCOMPARE(MediaRetryPolicy::retryDelayMs(3), qint64(8000));
        QCOMPARE(MediaRetryPolicy::retryDelayMs(4), qint64(16000));
        QCOMPARE(MediaRetryPolicy::retryDelayMs(5), qint64(32000));
    }

    // Caps at 10 minutes and never overflows for large attempt counts.
    void delayCapsAtTenMinutes() {
        QCOMPARE(MediaRetryPolicy::retryDelayMs(9), qint64(512000));
        QCOMPARE(MediaRetryPolicy::retryDelayMs(10), qint64(600000));
        QCOMPARE(MediaRetryPolicy::retryDelayMs(11), qint64(600000));
        QCOMPARE(MediaRetryPolicy::retryDelayMs(1000), qint64(600000));
    }

    // retryAllowed returns true only once `now` reaches lastFailureAt + delay.
    void retryAllowedRespectsBackoffWindow() {
        // attempt 1 -> 2000ms delay. Fail stamped at t=1000.
        QVERIFY(!MediaRetryPolicy::retryAllowed(false, 1, 1000, 1000));  // just failed
        QVERIFY(!MediaRetryPolicy::retryAllowed(false, 1, 1000, 2999));  // still waiting
        QVERIFY(MediaRetryPolicy::retryAllowed(false, 1, 1000, 3000));   // exactly at boundary
        QVERIFY(MediaRetryPolicy::retryAllowed(false, 1, 1000, 5000));   // well past
    }

    // A backward clock (now < lastFailureAt) must not permit an early retry.
    void retryAllowedGuardsBackwardClock() {
        QVERIFY(!MediaRetryPolicy::retryAllowed(false, 2, 10000, 9000));
    }

    // Glow budget: a still-loading placeholder animates for the first few failures,
    // then goes static so a host that merely keeps timing out can't pulse (and repaint
    // its row at ~60fps) forever. Retrying is governed separately by retryAllowed.
    void glowBudgetStopsAnimatingButNotRetrying() {
        QCOMPARE(MediaRetryPolicy::kGlowAttemptBudget, 3);
        QVERIFY(MediaRetryPolicy::glowAllowed(/*permanent=*/false, 0));
        QVERIFY(MediaRetryPolicy::glowAllowed(false, 2));
        QVERIFY(!MediaRetryPolicy::glowAllowed(false, 3));
        QVERIFY(!MediaRetryPolicy::glowAllowed(false, 10));
        // A permanent failure never animates, not even on the first hit.
        QVERIFY(!MediaRetryPolicy::glowAllowed(/*permanent=*/true, 0));
        // The glow stopping must not stop the retry: past the glow budget, a transient
        // failure whose backoff has elapsed is still retriable.
        QVERIFY(MediaRetryPolicy::retryAllowed(false, 3, 1000, 1000 + 100000));
    }

    // Decode budget: allowed for the first kDecodeAttemptBudget failures, then not.
    void decodeBudgetStopsAfterCap() {
        QCOMPARE(MediaRetryPolicy::kDecodeAttemptBudget, 2);
        QVERIFY(MediaRetryPolicy::decodeAllowed(0));
        QVERIFY(MediaRetryPolicy::decodeAllowed(1));
        QVERIFY(!MediaRetryPolicy::decodeAllowed(2));
        QVERIFY(!MediaRetryPolicy::decodeAllowed(3));
    }
};

QTEST_APPLESS_MAIN(TestMediaRetryPolicy)
#include "tst_media_retry_policy.moc"

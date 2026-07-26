// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest/QtTest>

#include "media/video_stream_retry.h"

using TeleMatrix::shouldRetryStreamError;
using TeleMatrix::streamNetworkTimeout;
using TeleMatrix::VideoContainer;
using TeleMatrix::VideoStreamRetryController;
using Action = TeleMatrix::VideoStreamRetryController::Action;

// QCOMPARE on an enum class needs an int cast for a readable diff on failure.
static int a(Action action) { return static_cast<int>(action); }

class TestVideoStreamRetry : public QObject {
    Q_OBJECT

private slots:
    // A download that makes no progress is declared dead and restarted after
    // exactly `stallTicks` polls.
    void stallReplaysAtThreshold() {
        VideoStreamRetryController c({.stallTicks = 3, .maxWaitTicks = 100, .maxErrorRestarts = 2});
        c.beginWait();
        QCOMPARE(a(c.tick(true, 0, 1000, false)), a(Action::Wait));   // stall 1
        QCOMPARE(a(c.tick(true, 0, 1000, false)), a(Action::Wait));   // stall 2
        QCOMPARE(a(c.tick(true, 0, 1000, false)), a(Action::Replay)); // stall 3 -> replay
    }

    // Any forward byte progress resets the stall counter, so a slow-but-alive
    // download is never mistaken for dead.
    void progressResetsStall() {
        VideoStreamRetryController c({.stallTicks = 3, .maxWaitTicks = 100, .maxErrorRestarts = 2});
        c.beginWait();
        QCOMPARE(a(c.tick(true, 0, 1000, false)), a(Action::Wait));   // stall 1
        QCOMPARE(a(c.tick(true, 500, 1000, false)), a(Action::Wait)); // progress -> stall reset
        QCOMPARE(a(c.tick(true, 500, 1000, false)), a(Action::Wait)); // stall 1
        QCOMPARE(a(c.tick(true, 500, 1000, false)), a(Action::Wait)); // stall 2
        QCOMPARE(a(c.tick(true, 500, 1000, false)), a(Action::Replay)); // stall 3 -> replay
    }

    // A fully-downloaded file replays immediately (replay from the complete cache,
    // where reads no longer block so the timeout can't recur).
    void completeReplaysImmediately() {
        VideoStreamRetryController c;
        c.beginWait();
        QCOMPARE(a(c.tick(true, 1000, 1000, false)), a(Action::Replay));
    }

    // A proxy-reported error restarts the download up to the budget, then gives up.
    void erroredReplaysThenGivesUp() {
        VideoStreamRetryController c({.stallTicks = 70, .maxWaitTicks = 100, .maxErrorRestarts = 2});
        c.beginWait();
        QCOMPARE(a(c.tick(false, 0, 0, true)), a(Action::Replay)); // restart 1
        QCOMPARE(a(c.tick(false, 0, 0, true)), a(Action::Replay)); // restart 2
        QCOMPARE(a(c.tick(false, 0, 0, true)), a(Action::GiveUp)); // budget spent
    }

    // The total-wait backstop forces a give-up even while bytes trickle in.
    void waitCapGivesUp() {
        VideoStreamRetryController c({.stallTicks = 1000, .maxWaitTicks = 3, .maxErrorRestarts = 2});
        c.beginWait();
        QCOMPARE(a(c.tick(true, 100, 1000, false)), a(Action::Wait)); // waitTick 1
        QCOMPARE(a(c.tick(true, 200, 1000, false)), a(Action::Wait)); // waitTick 2
        QCOMPARE(a(c.tick(true, 300, 1000, false)), a(Action::Wait)); // waitTick 3
        QCOMPARE(a(c.tick(true, 400, 1000, false)), a(Action::GiveUp)); // waitTick 4 > cap
    }

    // Unknown progress (proxy couldn't report bytes) is treated like no progress and
    // accrues stall.
    void unknownProgressAccruesStall() {
        VideoStreamRetryController c({.stallTicks = 2, .maxWaitTicks = 100, .maxErrorRestarts = 2});
        c.beginWait();
        QCOMPARE(a(c.tick(false, 0, 0, false)), a(Action::Wait));   // stall 1
        QCOMPARE(a(c.tick(false, 0, 0, false)), a(Action::Replay)); // stall 2 -> replay
    }

    // reset() restores the per-video error-restart budget; beginWait() does not.
    void resetRestoresErrorBudgetButBeginWaitDoesNot() {
        VideoStreamRetryController c({.stallTicks = 70, .maxWaitTicks = 100, .maxErrorRestarts = 2});
        c.beginWait();
        (void)c.tick(false, 0, 0, true); // restart 1
        (void)c.tick(false, 0, 0, true); // restart 2
        QCOMPARE(a(c.tick(false, 0, 0, true)), a(Action::GiveUp)); // spent

        c.beginWait(); // must NOT restore the error budget
        QCOMPARE(a(c.tick(false, 0, 0, true)), a(Action::GiveUp));

        c.reset(); // a new video restores it
        c.beginWait();
        QCOMPARE(a(c.tick(false, 0, 0, true)), a(Action::Replay));
    }

    // The FFmpeg network-timeout budget: base, scaled, and capped.
    void networkTimeoutScalesAndCaps() {
        QCOMPARE(streamNetworkTimeout(0), std::chrono::milliseconds(60000));
        QCOMPARE(streamNetworkTimeout(10000), std::chrono::milliseconds(80000)); // 60s + 2s*10
        QCOMPARE(streamNetworkTimeout(100000000), std::chrono::milliseconds(240000)); // capped
    }

    // A read timeout, or the connection abort the proxy raises for a dead download:
    // the bytes aren't all here. Always worth waiting out, frame or no frame.
    void resourceErrorAlwaysRetries() {
        for (auto c : { VideoContainer::Unknown,
                        VideoContainer::Faststart,
                        VideoContainer::MoovAtEnd }) {
            QVERIFY(shouldRetryStreamError(true, false, c));
            QVERIFY(shouldRetryStreamError(true, true, c));
        }
    }

    // A frame already rendered, so the stream works; a later FormatError is specific
    // to this video. Fall back rather than loop.
    void formatErrorAfterAFrameFallsBack() {
        QVERIFY(!shouldRetryStreamError(false, true, VideoContainer::Unknown));
        QVERIFY(!shouldRetryStreamError(false, true, VideoContainer::Faststart));
    }

    // A pre-frame FormatError on a recognised container: the proxy delivers a dead
    // or truncated download as ResourceError, so these bytes were sound and the
    // player rejected them — an unsupported codec. Replaying re-downloads identical
    // bytes, so fail fast instead of waiting out the whole download.
    void formatErrorOnARecognisedContainerFailsFast() {
        QVERIFY(!shouldRetryStreamError(false, false, VideoContainer::Faststart));
        QVERIFY(!shouldRetryStreamError(false, false, VideoContainer::MoovAtEnd));
    }

    // Unclassified: head not read yet, or it decrypted to garbage. Neither proves
    // the bytes are sound, so keep the bounded retry.
    void formatErrorOnAnUnknownContainerStillRetries() {
        QVERIFY(shouldRetryStreamError(false, false, VideoContainer::Unknown));
    }
};

QTEST_MAIN(TestVideoStreamRetry)
#include "tst_video_stream_retry.moc"

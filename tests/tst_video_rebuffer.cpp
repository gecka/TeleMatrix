// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "media/video_rebuffer.h"

using namespace TeleMatrix;
using Action = VideoRebufferController::Action;

// Duration fixed at 60s; downloadedFraction × 60s = downloaded ms.
class TestVideoRebuffer : public QObject {
    Q_OBJECT
private slots:
    void noActionWhenNotStreamingOrPausedOrNotReady() {
        VideoRebufferController c;
        QCOMPARE(c.evaluate(0.0f, 0, 60000, false, false, true), Action::None);
        QCOMPARE(c.evaluate(0.0f, 0, 60000, true, true, true), Action::None);
        QCOMPARE(c.evaluate(0.0f, 0, 60000, true, false, false), Action::None);
        QVERIFY(!c.waiting());
    }

    void initialBufferUsesSmallTarget() {
        VideoRebufferController c;
        // 1% = 0.6s ahead < 1s low → pause (initial buffering).
        QCOMPARE(c.evaluate(0.01f, 0, 60000, true, false, true), Action::Pause);
        QVERIFY(c.waiting());
        // 2.5% = 1.5s < 2s initial target → keep waiting.
        QCOMPARE(c.evaluate(0.025f, 0, 60000, true, false, true), Action::None);
        // 4% = 2.4s >= 2s initial target → play.
        QCOMPARE(c.evaluate(0.04f, 0, 60000, true, false, true), Action::Play);
        QVERIFY(!c.waiting());
    }

    void midPlayStallUsesBiggerResume() {
        VideoRebufferController c;
        // Start smoothly: 10% = 6s ahead > 1s → no pause, playback started.
        QCOMPARE(c.evaluate(0.10f, 0, 60000, true, false, true), Action::None);
        // Played to 5.5s, still 10% (6s) downloaded → 0.5s ahead < 1s → pause.
        QCOMPARE(c.evaluate(0.10f, 5500, 60000, true, false, true), Action::Pause);
        // Needs 5s now (started), 13% (7.8s) - 5.5s = 2.3s < 5s → wait.
        QCOMPARE(c.evaluate(0.13f, 5500, 60000, true, false, true), Action::None);
        // 20% (12s) - 5.5s = 6.5s >= 5s → play.
        QCOMPARE(c.evaluate(0.20f, 5500, 60000, true, false, true), Action::Play);
    }

    void fullyDownloadedResumesImmediately() {
        VideoRebufferController c;
        QCOMPARE(c.evaluate(0.01f, 0, 60000, true, false, true), Action::Pause);
        QVERIFY(c.waiting());
        QCOMPARE(c.evaluate(1.0f, 0, 60000, true, false, true), Action::Play);
        QVERIFY(!c.waiting());
    }

    void resetClearsState() {
        VideoRebufferController c;
        c.evaluate(0.01f, 0, 60000, true, false, true); // enter rebuffering
        QVERIFY(c.waiting());
        c.reset();
        QVERIFY(!c.waiting());
    }
};

QTEST_APPLESS_MAIN(TestVideoRebuffer)
#include "tst_video_rebuffer.moc"

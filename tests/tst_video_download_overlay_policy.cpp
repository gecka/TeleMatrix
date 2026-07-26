// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "media/video_download_overlay_policy.h"

using namespace TeleMatrix;

class TestVideoDownloadOverlayPolicy : public QObject {
    Q_OBJECT

private slots:
    // A moov-at-end video provably can't produce a frame until the download
    // finishes, so progress is determinate from the very first paint.
    void moovAtEndShowsProgressFromZero() {
        QVERIFY(showDeterminateDownload(VideoContainer::MoovAtEnd, false, true, 0.0f));
        QVERIFY(showDeterminateDownload(VideoContainer::MoovAtEnd, false, true, 0.01f));
        QVERIFY(showDeterminateDownload(VideoContainer::MoovAtEnd, false, true, 0.99f));
    }

    // Faststart suppresses the overlay entirely: a frame is imminent, and a
    // download bar would be a misleading flash on every play.
    void faststartNeverShowsProgress() {
        QVERIFY(!showDeterminateDownload(VideoContainer::Faststart, false, true, 0.0f));
        QVERIFY(!showDeterminateDownload(VideoContainer::Faststart, false, true, 0.5f));
        QVERIFY(!showDeterminateDownload(VideoContainer::Faststart, false, true, 0.9f));
    }

    // An unclassified container (mkv/webm, or bytes not yet read) keeps the
    // pre-existing "lots downloaded and still no frame" heuristic.
    void unknownKeepsTheLegacyHeuristic() {
        QVERIFY(!showDeterminateDownload(VideoContainer::Unknown, false, true, 0.0f));
        QVERIFY(!showDeterminateDownload(VideoContainer::Unknown, false, true, 0.2f));
        QVERIFY(showDeterminateDownload(VideoContainer::Unknown, false, true, 0.21f));
        QVERIFY(showDeterminateDownload(VideoContainer::Unknown, false, true, 0.9f));
    }

    // Once a frame is decoded the seek bar's buffered sub-bar takes over.
    void aDecodedFrameEndsTheOverlay() {
        QVERIFY(!showDeterminateDownload(VideoContainer::MoovAtEnd, true, true, 0.1f));
        QVERIFY(!showDeterminateDownload(VideoContainer::Unknown, true, true, 0.9f));
    }

    // A local file (nothing downloading through the proxy) never shows progress.
    void noPendingDownloadNeverShowsProgress() {
        QVERIFY(!showDeterminateDownload(VideoContainer::MoovAtEnd, false, false, 0.1f));
        QVERIFY(!showDeterminateDownload(VideoContainer::Unknown, false, false, 0.9f));
    }

    // A complete download is about to yield a frame; and a tiny moov-at-end clip
    // that lands in one poll must not flash a full bar. This, not the verdict, is
    // what protects very short videos.
    void completeDownloadEndsTheOverlay() {
        QVERIFY(!showDeterminateDownload(VideoContainer::MoovAtEnd, false, true, 1.0f));
        QVERIFY(!showDeterminateDownload(VideoContainer::Unknown, false, true, 1.0f));
        QVERIFY(!showDeterminateDownload(VideoContainer::MoovAtEnd, false, true, 1.5f));
    }
};

QTEST_MAIN(TestVideoDownloadOverlayPolicy)
#include "tst_video_download_overlay_policy.moc"

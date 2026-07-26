// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include <QBuffer>
#include <QByteArray>
#include <QImage>

#include "protocol/media_cache.h"

using namespace TeleMatrix;

namespace {

// imageCache caps at 400 entries; insert comfortably past that to force trim.
constexpr int kEvictionFloodCount = 800;

QImage tinyImage(Qt::GlobalColor color = Qt::white) {
    QImage img(1, 1, QImage::Format_ARGB32);
    img.fill(color);
    return img;
}

QByteArray encodePng(const QImage &img) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, "PNG");
    buffer.close();
    return bytes;
}

} // namespace

class TestMediaCache : public QObject {
    Q_OBJECT

private slots:
    // Each test mutates the process-global caches, so reset between them.
    void init() { MediaCache::clearAll(); }
    void cleanup() {
        MediaCache::setClockForTesting({}); // restore the real clock
        MediaCache::clearAll();
    }

    // A recorded resolve failure suppresses re-resolution until the backoff
    // window elapses; once `now` advances past it, needsResolution() reopens.
    void resolveFailureBacksOffThenReopens() {
        qint64 clock = 100000;
        MediaCache::setClockForTesting([&clock] { return clock; });

        const auto key = QStringLiteral("mxc://test/dead");
        QVERIFY(MediaCache::needsResolution(key)); // fresh: yes

        MediaCache::noteResolveFailed(key); // attempt 1 -> 2s backoff
        QVERIFY2(!MediaCache::needsResolution(key),
            "a just-failed URL must be suppressed within the backoff window");
        QVERIFY(MediaCache::retrySuppressed(key));

        clock += 1999; // still inside the 2s window
        QVERIFY(!MediaCache::needsResolution(key));

        clock += 1; // now exactly at lastFailure + 2000
        QVERIFY2(MediaCache::needsResolution(key),
            "backoff expiry must reopen resolution");
    }

    // A successful resolve (insertPath) clears the failure memory so the URL is
    // not throttled the next time it needs work.
    void resolveSuccessClearsFailureState() {
        qint64 clock = 100000;
        MediaCache::setClockForTesting([&clock] { return clock; });

        const auto key = QStringLiteral("mxc://test/heals");
        MediaCache::noteResolveFailed(key);
        MediaCache::noteResolveFailed(key);
        QVERIFY(MediaCache::retrySuppressed(key));

        MediaCache::insertPath(key, QStringLiteral("/nonexistent/path"));
        QVERIFY2(!MediaCache::retrySuppressed(key),
            "insertPath must clear resolve-failure state");
    }

    // Decode failures exhaust a small budget, after which the key is declared
    // unavailable (paint shows a static placeholder, no retry storm).
    void decodeBudgetMarksUnavailable() {
        const auto key = QStringLiteral("previewthumb:mxc://test/badimg");
        QVERIFY(!MediaCache::decodeSuppressed(key));
        QVERIFY(!MediaCache::mediaUnavailable(key));

        MediaCache::noteDecodeFailed(key); // 1
        QVERIFY(!MediaCache::decodeSuppressed(key));
        MediaCache::noteDecodeFailed(key); // 2 -> budget exhausted
        QVERIFY2(MediaCache::decodeSuppressed(key),
            "decode budget must be exhausted after kDecodeAttemptBudget failures");
        QVERIFY2(MediaCache::mediaUnavailable(key),
            "an undecodable key must read as unavailable for paint");
    }

    // The loading glow animates while a failure is fresh, then goes static so a host
    // that merely keeps timing out can't pulse a row at ~60fps forever — while the
    // key stays retriable (it is NOT "unavailable").
    void glowStopsAnimatingLongBeforeGivingUpRetries() {
        qint64 clock = 100000;
        MediaCache::setClockForTesting([&clock] { return clock; });

        const auto key = QStringLiteral("previewthumb:mxc://test/slowhost");
        QVERIFY(MediaCache::shouldGlowWhileLoading(key)); // never failed

        MediaCache::noteResolveFailed(key);
        QVERIFY2(MediaCache::shouldGlowWhileLoading(key),
            "a fresh transient failure still reads as loading");

        MediaCache::noteResolveFailed(key);
        MediaCache::noteResolveFailed(key); // 3 == kGlowAttemptBudget
        QVERIFY2(!MediaCache::shouldGlowWhileLoading(key),
            "past the glow budget the placeholder must stop animating");
        QVERIFY2(!MediaCache::mediaUnavailable(key),
            "but a transient failure is still not 'unavailable'");

        clock += 60'000'000; // long past the backoff
        QVERIFY2(MediaCache::needsResolution(key),
            "and it must still retry quietly once the backoff elapses");
    }

    // A permanent (terminal 4xx) failure suppresses retries for good — never
    // reopening no matter how far the clock advances — and reads as unavailable
    // (static skeleton) immediately. This is the 404 fix.
    void permanentFailureNeverReopensAndIsUnavailable() {
        qint64 clock = 100000;
        MediaCache::setClockForTesting([&clock] { return clock; });

        const auto key = QStringLiteral("previewthumb:mxc://test/gone");
        MediaCache::noteResolvePermanentlyFailed(key);
        QVERIFY2(!MediaCache::needsResolution(key),
            "a permanently-failed URL must never need resolution");
        QVERIFY(MediaCache::retrySuppressed(key));
        QVERIFY2(MediaCache::mediaUnavailable(key),
            "a 404 must read as unavailable from the first hit");

        clock += 60'000'000; // an hour later — still suppressed
        QVERIFY2(!MediaCache::needsResolution(key),
            "a permanent failure never reopens with elapsed time");
        QVERIFY(MediaCache::mediaUnavailable(key));
    }

    // A transient failure that is merely backing off is NOT unavailable: the
    // caller keeps the loading glow pulsing (it will retry) even after several
    // failures — only a permanent/decode-exhausted key goes static.
    void transientBackoffStaysLoadingNotUnavailable() {
        qint64 clock = 100000;
        MediaCache::setClockForTesting([&clock] { return clock; });

        const auto key = QStringLiteral("previewthumb:mxc://test/slow");
        MediaCache::noteResolveFailed(key);
        MediaCache::noteResolveFailed(key); // 2+ failures, backing off
        QVERIFY2(MediaCache::retrySuppressed(key),
            "a transient failure still backs off within its window");
        QVERIFY2(!MediaCache::mediaUnavailable(key),
            "a transient backoff must stay 'loading' so the glow keeps pulsing");
    }

    // Fresh source bytes reset the decode budget (the input changed, so it is
    // worth trying again).
    void decodeStateClearedByNewBytes() {
        const auto key = QStringLiteral("mxc://test/redecode");
        MediaCache::noteDecodeFailed(key);
        MediaCache::noteDecodeFailed(key);
        QVERIFY(MediaCache::decodeSuppressed(key));

        QVERIFY(MediaCache::insertImageBytes(
            key, encodePng(tinyImage(Qt::green)), QStringLiteral("image/png")));
        QVERIFY2(!MediaCache::decodeSuppressed(key),
            "insertImageBytes must reset the decode budget");
    }

    // F1: eviction is LRU, not FIFO. Two entries inserted at the very start;
    // the one that keeps getting accessed survives the flood while the
    // never-touched one is evicted. Under the old FIFO behavior BOTH would be
    // evicted (oldest-inserted first), so "keep" surviving proves the touch.
    void lruTouchSurvivesEviction() {
        MediaCache::insertImage(QStringLiteral("victim"), tinyImage());
        MediaCache::insertImage(QStringLiteral("keep"), tinyImage());

        for (int i = 0; i < kEvictionFloodCount; ++i) {
            MediaCache::insertImage(QStringLiteral("f%1").arg(i), tinyImage());
            if (i % 10 == 0) {
                // Access "keep" so it moves to the most-recently-used position.
                QVERIFY(!MediaCache::loadImage(QStringLiteral("keep")).isNull());
            }
        }

        QVERIFY2(!MediaCache::loadImage(QStringLiteral("keep")).isNull(),
            "recently-accessed entry must survive eviction (LRU)");
        QVERIFY2(MediaCache::loadImage(QStringLiteral("victim")).isNull(),
            "never-accessed early entry must be evicted");
    }

    // F4/F5: the original encoded bytes are retained so an evicted decoded
    // image rebuilds locally at full resolution, and isResolved() stays true
    // across eviction (no preloader flash / backend round-trip on reopen).
    void encodedBytesSurviveDecodedEviction() {
        const auto key = QStringLiteral("mxc://test/image");
        const auto bytes = encodePng(tinyImage(Qt::red).scaled(4, 4));

        QVERIFY(MediaCache::insertImageBytes(key, bytes, QStringLiteral("image/png")));
        QVERIFY(MediaCache::isResolved(key));
        QVERIFY(!MediaCache::loadImage(key).isNull());

        // Flood imageCache to evict the decoded frame (sourceBytesCache has its
        // own, separate budget and is untouched by insertImage).
        for (int i = 0; i < kEvictionFloodCount; ++i) {
            MediaCache::insertImage(QStringLiteral("blur:%1").arg(i), tinyImage());
        }

        // Decoded image is gone from imageCache, but the URL is still resolved
        // via retained bytes, and loadImage rebuilds it at full resolution.
        QVERIFY2(MediaCache::isResolved(key),
            "retained encoded bytes must keep the URL resolved after eviction");
        const auto rebuilt = MediaCache::loadImage(key);
        QVERIFY2(!rebuilt.isNull(), "image must rebuild from retained bytes");
        QCOMPARE(rebuilt.width(), 4);
        QCOMPARE(rebuilt.height(), 4);
    }

    // needsResolution must stay false while bytes are retained (so the host
    // does not re-request media it can rebuild locally).
    void retainedBytesSuppressReResolution() {
        const auto key = QStringLiteral("mxc://test/needs");
        QVERIFY(MediaCache::needsResolution(key));
        QVERIFY(MediaCache::insertImageBytes(
            key, encodePng(tinyImage(Qt::blue)), QStringLiteral("image/png")));
        QVERIFY(!MediaCache::needsResolution(key));

        for (int i = 0; i < kEvictionFloodCount; ++i) {
            MediaCache::insertImage(QStringLiteral("x%1").arg(i), tinyImage());
        }
        QVERIFY2(!MediaCache::needsResolution(key),
            "retained bytes must keep needsResolution() false after eviction");
    }
};

QTEST_GUILESS_MAIN(TestMediaCache)
#include "tst_media_cache.moc"

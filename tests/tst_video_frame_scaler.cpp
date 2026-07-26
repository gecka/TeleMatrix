// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest/QtTest>

#include "media/video_frame_scaler.h"

using TeleMatrix::downscaleVideoFrame;

class TestVideoFrameScaler : public QObject {
    Q_OBJECT

private slots:
    // A frame already at/below the target width keeps its dimensions.
    void passthroughWhenNotLarger() {
        QImage img(100, 50, QImage::Format_ARGB32);
        img.fill(Qt::red);
        QCOMPARE(downscaleVideoFrame(img, QSize(200, 100)).size(), QSize(100, 50));
    }

    // An empty/invalid target never scales.
    void emptyTargetReturnsFrame() {
        QImage img(100, 50, QImage::Format_ARGB32);
        img.fill(Qt::red);
        QCOMPARE(downscaleVideoFrame(img, QSize()).size(), QSize(100, 50));
        QCOMPARE(downscaleVideoFrame(img, QSize(0, 0)).size(), QSize(100, 50));
    }

    // An ordinary frame downscales to the target (KeepAspectRatio).
    void downscales8bitFrame() {
        QImage img(1280, 720, QImage::Format_ARGB32);
        img.fill(Qt::green);
        const auto out = downscaleVideoFrame(img, QSize(640, 360));
        QCOMPARE(out.width(), 640);
        QVERIFY(out.height() <= 360);
    }

    // A high-bit-depth (RGBA64) frame is flattened to 8-bit opaque (the over-black
    // composite handles any format), so nothing downstream sees a 64-bit image.
    void highBitDepthFlattenedToOpaque8bit() {
        QImage img(1280, 720, QImage::Format_RGBA64);
        img.fill(QColor(10, 200, 30));
        const auto out = downscaleVideoFrame(img, QSize(640, 360));
        QCOMPARE(out.width(), 640);
        QVERIFY(out.depth() <= 32);
        QVERIFY(!out.hasAlphaChannel());
    }

    // THE FIX: a premultiplied-alpha video frame (unreliable alpha from toImage())
    // is forced opaque so a light background can't bleed through, and so it stays
    // opaque on the translucent fullscreen overlay. Both the downscale and the
    // passthrough paths must produce an opaque result.
    void forcesFrameOpaque() {
        QImage img(1280, 720, QImage::Format_RGBA8888_Premultiplied);
        img.fill(QColor(200, 50, 10, 100)); // non-opaque alpha
        QVERIFY2(!downscaleVideoFrame(img, QSize(640, 360)).hasAlphaChannel(),
            "downscaled frame must be opaque");
        QVERIFY2(!downscaleVideoFrame(img, QSize(4000, 4000)).hasAlphaChannel(),
            "passthrough frame must be opaque too");
    }

    // Forcing opaque must NOT un-premultiply: a frame whose stored RGB is the true
    // colour (only mislabeled premultiplied) keeps that colour after the flatten.
    void opaqueKeepsTrueColorNoUnpremultiply() {
        QImage img(4, 4, QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < img.height(); ++y) {
            auto *line = img.scanLine(y);
            for (int x = 0; x < img.width(); ++x) {
                line[x * 4 + 0] = 200; // R
                line[x * 4 + 1] = 50;  // G
                line[x * 4 + 2] = 10;  // B
                line[x * 4 + 3] = 100; // A (unreliable)
            }
        }
        const auto out = downscaleVideoFrame(img, QSize(64, 64)); // no scale
        QVERIFY(!out.hasAlphaChannel());
        const auto c = out.pixelColor(0, 0);
        // Composited over black, the mislabeled-premultiplied RGB comes through as-is
        // (un-premultiplying by 100/255 would have blown these values out instead).
        QCOMPARE(c.red(), 200);
        QCOMPARE(c.green(), 50);
        QCOMPARE(c.blue(), 10);
        QCOMPARE(c.alpha(), 255);
    }

    // An opaque input for a downscale keeps its colour (sanity check on the scale).
    void downscalePreservesOpaqueColor() {
        QImage img(1280, 720, QImage::Format_RGBA8888_Premultiplied);
        img.fill(QColor(200, 50, 10)); // opaque
        const auto c = downscaleVideoFrame(img, QSize(640, 360))
                           .pixelColor(320, 180);
        QVERIFY(qAbs(c.red() - 200) < 4 && qAbs(c.green() - 50) < 4
            && qAbs(c.blue() - 10) < 4);
    }
};

QTEST_MAIN(TestVideoFrameScaler)
#include "tst_video_frame_scaler.moc"

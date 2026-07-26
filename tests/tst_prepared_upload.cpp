// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageIOHandler>
#include <QTemporaryDir>

#include "history/image_recompress.h"

using namespace TeleMatrix;

namespace {

// A non-trivial gradient image so it is a valid bitmap of an exact size.
QImage makeImage(int w, int h) {
    QImage img(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        auto *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            line[x] = qRgb((x * 255) / qMax(1, w), (y * 255) / qMax(1, h), 128);
        }
    }
    return img;
}

QString writePng(const QString &dir, const QString &name, const QImage &img) {
    const auto path = dir + QStringLiteral("/") + name;
    img.save(path, "PNG");
    return path;
}

} // namespace

class TestPreparedUpload : public QObject {
    Q_OBJECT

private slots:
    // --- applyOrientationToSize: which EXIF transforms swap width/height ---

    void orientationNoSwapForUprightTransforms() {
        const QSize raw(4000, 3000);
        QCOMPARE(applyOrientationToSize(raw, QImageIOHandler::TransformationNone), raw);
        QCOMPARE(applyOrientationToSize(raw, QImageIOHandler::TransformationMirror), raw);
        QCOMPARE(applyOrientationToSize(raw, QImageIOHandler::TransformationFlip), raw);
        QCOMPARE(applyOrientationToSize(raw, QImageIOHandler::TransformationRotate180), raw);
    }

    void orientationSwapsForQuarterTurns() {
        const QSize raw(4000, 3000);
        const QSize swapped(3000, 4000);
        QCOMPARE(applyOrientationToSize(raw, QImageIOHandler::TransformationRotate90), swapped);
        QCOMPARE(applyOrientationToSize(raw, QImageIOHandler::TransformationRotate270), swapped);
        QCOMPARE(applyOrientationToSize(raw, QImageIOHandler::TransformationMirrorAndRotate90), swapped);
        QCOMPARE(applyOrientationToSize(raw, QImageIOHandler::TransformationFlipAndRotate90), swapped);
    }

    // --- recompressImageForUpload ---

    void downscalesToLongestEdge() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const auto src = writePng(tmp.path(), QStringLiteral("src.png"), makeImage(2000, 1000));
        const auto out = tmp.path() + QStringLiteral("/out.jpg");

        // originalSize=0 bypasses the "must be smaller" guard, isolating the
        // downscale math.
        const auto r = recompressImageForUpload(src, out, /*maxEdge=*/1280, /*quality=*/85, /*originalSize=*/0);
        QVERIFY(r.ok);
        QCOMPARE(r.width, 1280);
        QCOMPARE(r.height, 640);
        QVERIFY(QFile::exists(out));
        QCOMPARE(QString::fromLatin1(QImageReader(out).format()), QStringLiteral("jpeg"));
    }

    void neverUpscalesSmallImages() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const auto src = writePng(tmp.path(), QStringLiteral("small.png"), makeImage(1000, 800));
        const auto out = tmp.path() + QStringLiteral("/out.jpg");

        const auto r = recompressImageForUpload(src, out, /*maxEdge=*/2560, /*quality=*/85, /*originalSize=*/0);
        QVERIFY(r.ok);
        QCOMPARE(r.width, 1000);
        QCOMPARE(r.height, 800);
    }

    void rejectsWhenNotSmallerThanOriginal() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const auto src = writePng(tmp.path(), QStringLiteral("src.png"), makeImage(1200, 900));
        const auto out = tmp.path() + QStringLiteral("/out.jpg");

        // originalSize=1: any JPEG is >= 1 byte, so the result is "not smaller".
        const auto r = recompressImageForUpload(src, out, /*maxEdge=*/2560, /*quality=*/85, /*originalSize=*/1);
        QVERIFY(!r.ok);
        QVERIFY(!QFile::exists(out)); // discarded, caller sends the original
    }

    void acceptsWhenSmallerThanOriginal() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const auto src = writePng(tmp.path(), QStringLiteral("src.png"), makeImage(1200, 900));
        const auto out = tmp.path() + QStringLiteral("/out.jpg");

        // A huge claimed original guarantees the JPEG is smaller.
        const auto r = recompressImageForUpload(src, out, /*maxEdge=*/2560, /*quality=*/85,
            /*originalSize=*/100ull * 1024 * 1024);
        QVERIFY(r.ok);
        QVERIFY(QFileInfo(out).size() > 0);
        QVERIFY(QFileInfo(out).size() < 100ull * 1024 * 1024);
    }

    void failsOnUndecodableSource() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const auto src = tmp.path() + QStringLiteral("/notimage.png");
        {
            QFile f(src);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("this is not an image");
        }
        const auto out = tmp.path() + QStringLiteral("/out.jpg");

        const auto r = recompressImageForUpload(src, out, /*maxEdge=*/2560, /*quality=*/85, /*originalSize=*/0);
        QVERIFY(!r.ok);
        QVERIFY(!QFile::exists(out));
    }
};

QTEST_GUILESS_MAIN(TestPreparedUpload)
#include "tst_prepared_upload.moc"

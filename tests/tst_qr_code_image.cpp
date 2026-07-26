// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include <QImage>
#include <QPainter>

#include "ui/qr_code_image.h"

using namespace TeleMatrix;

namespace {

// Render `modules` into a `side`x`side` image pre-filled red (the sentinel marks
// any area paintQrModules leaves untouched).
QImage render(const QByteArray &modules, int size, int side, int quiet) {
    QImage img(side, side, QImage::Format_RGB32);
    img.fill(Qt::red);
    QPainter p(&img);
    paintQrModules(
        p, QRect(0, 0, side, side), modules, size,
        QColor(Qt::black), QColor(Qt::white), quiet);
    p.end();
    return img;
}

} // namespace

class TestQrCodeImage : public QObject {
    Q_OBJECT

private slots:
    // Dark/light modules land at the right pixels, and indexing is row-major
    // (modules[y*size + x]).
    void mapsModulesRowMajor() {
        QByteArray m;                 // 2x2 diagonal: dark at (0,0) and (1,1)
        m.append(char(1)).append(char(0)).append(char(0)).append(char(1));
        const auto img = render(m, 2, 20, /*quiet=*/0); // 10px modules

        QCOMPARE(img.pixelColor(5, 5), QColor(Qt::black));    // (0,0)
        QCOMPARE(img.pixelColor(15, 15), QColor(Qt::black));  // (1,1)
        QCOMPARE(img.pixelColor(15, 5), QColor(Qt::white));   // (1,0)
        QCOMPARE(img.pixelColor(5, 15), QColor(Qt::white));   // (0,1)
    }

    // Quiet zone insets the code, and a non-multiple side keeps modules a whole
    // number of pixels and centers the result (leftover stays the red sentinel).
    void appliesQuietZoneAndCenters() {
        QByteArray m;
        m.append(char(1));            // single dark module, quiet=4 -> 9 cells
        const auto img = render(m, 1, 95, /*quiet=*/4); // module=floor(95/9)=10, qr=90, offset=2

        QCOMPARE(img.pixelColor(47, 47), QColor(Qt::black)); // the module (centered + inset)
        QCOMPARE(img.pixelColor(10, 10), QColor(Qt::white)); // quiet zone inside the code
        QCOMPARE(img.pixelColor(0, 0), QColor(Qt::red));     // 2px centering margin, untouched
    }

    // Invalid input must be a no-op (no crash, no out-of-bounds read/write):
    // the image stays fully red.
    void noOpOnInvalidInput() {
        QByteArray one;
        one.append(char(1));
        QCOMPARE(render(QByteArray(), 1, 20, 0).pixelColor(10, 10), QColor(Qt::red)); // empty
        QCOMPARE(render(one, 0, 20, 0).pixelColor(10, 10), QColor(Qt::red));          // size<=0
        QCOMPARE(render(one, 3, 30, 0).pixelColor(10, 10), QColor(Qt::red));          // short buffer
        QCOMPARE(render(one, 10, 2, 4).pixelColor(1, 1), QColor(Qt::red));            // target too small
    }
};

QTEST_GUILESS_MAIN(TestQrCodeImage)
#include "tst_qr_code_image.moc"

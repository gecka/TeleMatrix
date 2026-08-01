// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>
#include <QImage>
#include <QPainter>

#include "app/sign_out_curtain.h"

using namespace TeleMatrix;

namespace {

constexpr int kWidth = 900;
constexpr int kHeight = 600;

// A stand-in for a window full of private content: rows of small text, and
// filled discs where avatars would be. High-contrast on purpose — anything the
// curtain can hide here it can hide in a real timeline.
QImage syntheticScreenful() {
    QImage image(kWidth, kHeight, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing, true);
    QFont font;
    font.setPixelSize(12);
    p.setFont(font);
    p.setPen(Qt::black);
    for (int row = 0; row < 34; ++row) {
        const int y = 24 + row * 17;
        p.drawText(
            70, y,
            QStringLiteral("Meeting moved to 14:00, room name Payroll Q3 %1")
                .arg(row));
        p.setBrush(row % 2 ? Qt::darkBlue : Qt::darkRed);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPoint(36, y - 5), 13, 13);
        p.setPen(Qt::black);
    }
    p.end();
    return image;
}

// Sum of |Laplacian| per pixel over the luminance plane. Glyph edges and avatar
// rims are high-frequency energy; when it collapses there are no edges left to
// read. A blurred-beyond-recognition frame scores near zero, a legible one high.
//
// Steps of 8-bit quantization are ignored. A nearly flat field bands into ±1
// steps, and summing those swamps the measurement with noise that rises as the
// frame gets *smoother* — which had this metric reporting more "structure" in a
// blank paper field than in a half-blurred one. Real glyph edges are an order
// of magnitude above this cut.
constexpr int kQuantizationNoise = 4;

qreal edgeEnergy(const QImage &rgb) {
    const QImage image = rgb.convertToFormat(QImage::Format_Grayscale8);
    const int w = image.width();
    const int h = image.height();
    if (w < 3 || h < 3) {
        return 0.0;
    }
    qreal total = 0.0;
    for (int y = 1; y < h - 1; ++y) {
        const auto *above = image.constScanLine(y - 1);
        const auto *line = image.constScanLine(y);
        const auto *below = image.constScanLine(y + 1);
        for (int x = 1; x < w - 1; ++x) {
            const int laplacian = 4 * int(line[x])
                - int(line[x - 1]) - int(line[x + 1])
                - int(above[x]) - int(below[x]);
            if (qAbs(laplacian) > kQuantizationNoise) {
                total += qAbs(laplacian);
            }
        }
    }
    return total / (qreal(w - 2) * qreal(h - 2));
}

qreal meanLuminance(const QImage &rgb) {
    const QImage image = rgb.convertToFormat(QImage::Format_Grayscale8);
    qreal total = 0.0;
    for (int y = 0; y < image.height(); ++y) {
        const auto *line = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            total += line[x];
        }
    }
    return total / (qreal(image.width()) * qreal(image.height()) * 255.0);
}

// Render one curtain frame the way the widget does: the target starts as the
// live content, and PaintCurtain must overwrite every pixel of it.
QImage renderFrame(const CurtainPyramid &pyramid, const QImage &source, qreal progress) {
    QImage frame = source.copy();
    QPainter p(&frame);
    PaintCurtain(
        p,
        QRect(0, 0, kWidth, kHeight),
        QRect(0, 0, kWidth, kHeight),
        pyramid,
        progress,
        QColor(0xF4, 0xF1, 0xEA));
    p.end();
    return frame;
}

} // namespace

class TestSignOutCurtain : public QObject {
    Q_OBJECT

private slots:
    void startsFromAnUntouchedFrame() {
        const auto stage = CurtainStageAt(0.0);
        QCOMPARE(stage.downsample, 1.0);
        QCOMPARE(stage.washAlpha, 0.0);
        QCOMPARE(stage.scale, 1.0);
    }

    // THE requirement. Past 40% no frame may carry anything identifiable, so
    // every floor has to hold for the whole remainder of the transition — not
    // just at the endpoint.
    void holdsEveryFloorPastFortyPercent() {
        for (qreal p = kCurtainOpaqueFrom; p <= 1.0; p += 0.005) {
            const auto stage = CurtainStageAt(p);
            QVERIFY2(
                stage.downsample >= kCurtainFloorDownsample,
                qPrintable(QStringLiteral("downsample %1 at progress %2")
                    .arg(stage.downsample).arg(p)));
            QVERIFY2(
                stage.washAlpha >= kCurtainFloorWashAlpha,
                qPrintable(QStringLiteral("wash %1 at progress %2")
                    .arg(stage.washAlpha).arg(p)));
        }
    }

    // Nothing may come back into focus once it has gone: a frame that
    // re-sharpens is a frame that leaks, however briefly.
    void neverRecovers() {
        auto previous = CurtainStageAt(0.0);
        for (qreal p = 0.0; p <= 1.0; p += 0.005) {
            const auto stage = CurtainStageAt(p);
            QVERIFY(stage.downsample >= previous.downsample);
            QVERIFY(stage.washAlpha >= previous.washAlpha);
            QVERIFY(stage.scale <= previous.scale);
            previous = stage;
        }
    }

    // "Pulls back a notch" — a recede, not a zoom-out.
    void pullsBackOnlySlightly() {
        QVERIFY(CurtainStageAt(1.0).scale < 1.0);
        QVERIFY(CurtainStageAt(1.0).scale > 0.9);
    }

    // Rendered proof, through the real paint path rather than the policy alone:
    // by the floor, the edges that make text and faces readable are gone.
    void rendersNothingLegiblePastFortyPercent() {
        const auto source = syntheticScreenful();
        const auto pyramid = BuildCurtainPyramid(source);
        const auto original = edgeEnergy(source);
        QVERIFY2(original > 1.0, "the synthetic screenful should be busy");

        for (qreal p = kCurtainOpaqueFrom; p <= 1.0; p += 0.05) {
            const auto energy = edgeEnergy(renderFrame(pyramid, source, p));
            QVERIFY2(
                energy < original * 0.001,
                qPrintable(QStringLiteral("edge energy %1 (of %2) at progress %3")
                    .arg(energy).arg(original).arg(p)));
        }
    }

    // No level may be stored coarser than its own factor implies, or it carries
    // more softness than it claims and the cross-fade into it is a step rather
    // than a ramp. Checked at a Retina-sized source specifically: a fixed
    // working size looks fine against a small test image and silently turns the
    // first blurred level into a 2.8x one on a real 2x screen.
    void everyLevelIsAsSharpAsItsFactorClaims() {
        for (const auto longEdge : { 900, 1800, 3000 }) {
            QImage source(longEdge, longEdge * 2 / 3, QImage::Format_ARGB32_Premultiplied);
            source.fill(Qt::white);
            const auto pyramid = BuildCurtainPyramid(source);
            QVERIFY(!pyramid.isEmpty());
            for (int i = 0; i < pyramid.factors.size(); ++i) {
                const auto factor = pyramid.factors.at(i);
                const auto stored = pyramid.levels.at(i).width();
                const auto implied = source.width() / factor;
                QVERIFY2(
                    stored >= implied - 1,
                    qPrintable(QStringLiteral(
                        "factor %1 stored at %2px, its own factor implies %3px "
                        "(source %4px)")
                        .arg(factor).arg(stored).arg(implied).arg(source.width())));
            }
        }
    }

    // Not an assertion — a way to look at the thing. Tuning a transition from
    // numbers alone does not work, and the app must not be launched to see it,
    // so this writes the frames out for inspection:
    //   CURTAIN_DUMP_DIR=/tmp/curtain ./tst_sign_out_curtain dumpDebugFrames
    // Skipped (and silent) unless that variable is set.
    void dumpDebugFrames() {
        const auto source = syntheticScreenful();
        const auto pyramid = BuildCurtainPyramid(source);
        const auto dir = qEnvironmentVariable("CURTAIN_DUMP_DIR");
        if (dir.isEmpty()) {
            QSKIP("set CURTAIN_DUMP_DIR to dump frames");
        }
        source.save(dir + QStringLiteral("/curtain-source.png"));
        for (const auto p : { 0.0, 0.2, 0.4, 0.7, 1.0 }) {
            const auto frame = renderFrame(pyramid, source, p);
            frame.save(dir + QStringLiteral("/curtain-%1.png").arg(int(p * 100)));
            qInfo() << "progress" << p << "edge energy" << edgeEnergy(frame);
        }
    }

    // The window softens; it never blanks to black. Checked across the whole
    // transition, not just at the end.
    void neverGoesDark() {
        const auto source = syntheticScreenful();
        const auto pyramid = BuildCurtainPyramid(source);
        for (qreal p = 0.0; p <= 1.0; p += 0.05) {
            const auto luminance = meanLuminance(renderFrame(pyramid, source, p));
            QVERIFY2(
                luminance > 0.5,
                qPrintable(QStringLiteral("mean luminance %1 at progress %2")
                    .arg(luminance).arg(p)));
        }
    }

    // Every pixel is the curtain's responsibility: the widget is opaque, so a
    // region left unpainted would show the live window through the gap.
    void paintsTheWholeWidgetRect() {
        const auto source = syntheticScreenful();
        const auto pyramid = BuildCurtainPyramid(source);
        // Content inset well inside the widget: the margin still must be filled.
        QImage frame(kWidth, kHeight, QImage::Format_ARGB32_Premultiplied);
        frame.fill(Qt::magenta);
        QPainter p(&frame);
        PaintCurtain(
            p,
            QRect(0, 0, kWidth, kHeight),
            QRect(120, 90, kWidth - 240, kHeight - 180),
            pyramid,
            1.0,
            QColor(0xF4, 0xF1, 0xEA));
        p.end();

        for (int y = 0; y < kHeight; y += 7) {
            for (int x = 0; x < kWidth; x += 7) {
                QVERIFY2(
                    frame.pixelColor(x, y) != QColor(Qt::magenta),
                    qPrintable(QStringLiteral("unpainted pixel at %1,%2").arg(x).arg(y)));
            }
        }
    }
};

QTEST_MAIN(TestSignOutCurtain)
#include "tst_sign_out_curtain.moc"

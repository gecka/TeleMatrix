// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "app/sign_out_curtain.h"

#include <QLinearGradient>
#include <QPainter>

#include <cmath>

namespace TeleMatrix {
namespace {

// A fine geometric progression rather than powers of two: neighbours are ~1.4x
// apart, so the cross-fade between them is a small enough change to read as
// continuous defocus instead of a series of steps. The floor (32) and the
// settled level (64) must both appear here — see the header.
//
// The blurred chain starts at 2, matching kWorkingDivisor exactly, so the
// finest of them is stored at precisely the resolution its own factor implies
// and carries no softness it does not claim. Softening between 1 and 2 comes
// from cross-fading the untouched level into it.
const QList<qreal> kFactors = {
    1, 2, 2.8, 4, 5.6, 8, 11, 16, 22, 32, 45, 64,
};

// Softened levels are stored at the source's own size over this, NOT at a fixed
// pixel size. A fixed one is measured against whatever window the author had:
// at 2x it silently makes every level coarser than its factor says, so the
// first cross-fade jumps from razor sharp to visibly soft instead of ramping.
constexpr int kWorkingDivisor = 2;

// Coarsest the content is drawn at once the transition has settled. The frame
// keeps softening after the guarantee is already met, so the end of the
// animation reads as a settling rather than a stop.
constexpr qreal kSettledDownsample = 64.0;
// Deliberately well short of opaque. The wash is a veil over the softened
// window, not a replacement for it: at 0.92 the resting frame was 92% flat
// paper, so the defocused colour underneath contributed almost nothing and
// every change to the blur was invisible behind it. The privacy guarantee does
// not rest on the wash — resolution carries it — so the veil only has to read
// as a veil.
constexpr qreal kSettledWashAlpha = 0.45;
constexpr qreal kFloorScale = 0.975;
constexpr qreal kSettledScale = 0.96;
constexpr qreal kFloorWashFront = 0.6;

qreal smoothstep(qreal t) {
    t = qBound(qreal(0), t, qreal(1));
    return t * t * (3.0 - 2.0 * t);
}

// Rebuild a heavily reduced image back up to `target` in repeated doublings.
// One bilinear stretch straight from 1/32 to full size leaves the blocky
// diamond pattern that reads as pixelation; doubling repeatedly interpolates
// each time against an already-smooth source, which reads as defocus.
QImage smoothUpTo(QImage image, QSize target) {
    while (image.width() * 2 < target.width() && image.height() * 2 < target.height()) {
        image = image.scaled(
            image.width() * 2, image.height() * 2,
            Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    return image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

} // namespace

CurtainStage CurtainStageAt(qreal progress) {
    const auto p = qBound(qreal(0), progress, qreal(1));
    CurtainStage stage;

    if (p < kCurtainOpaqueFrom) {
        // Approach: barely touched at first, then collapsing hard into the
        // floor. The exponent biases the loss of resolution late in this phase
        // so the first frames still read as the window softening rather than as
        // a cut — while still arriving at the floor exactly at the boundary.
        const auto t = smoothstep(p / kCurtainOpaqueFrom);
        stage.downsample = std::pow(kCurtainFloorDownsample, std::pow(t, 1.3));
        stage.washAlpha = kCurtainFloorWashAlpha * t;
        stage.washFront = kFloorWashFront * t;
        stage.scale = 1.0 - (1.0 - kFloorScale) * t;
        stage.edgeFeather = t;
        return stage;
    }

    // Settle. Every term starts exactly on its floor, so the two phases meet
    // continuously and nothing can re-sharpen across the boundary.
    const auto u = smoothstep((p - kCurtainOpaqueFrom) / (1.0 - kCurtainOpaqueFrom));
    stage.downsample = kCurtainFloorDownsample
        * std::pow(kSettledDownsample / kCurtainFloorDownsample, u);
    stage.washAlpha = kCurtainFloorWashAlpha
        + (kSettledWashAlpha - kCurtainFloorWashAlpha) * u;
    stage.washFront = kFloorWashFront + (1.0 - kFloorWashFront) * u;
    stage.scale = kFloorScale - (kFloorScale - kSettledScale) * u;
    stage.edgeFeather = 1.0;
    return stage;
}

QColor CurtainPaper() {
    return QColor(0xF4, 0xF1, 0xEA);
}

CurtainPyramid BuildCurtainPyramid(const QImage &source) {
    CurtainPyramid pyramid;
    if (source.isNull()) {
        return pyramid;
    }
    pyramid.sourceSize = source.size();
    const auto rgb = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    const QSize working(
        qMax(1, rgb.width() / kWorkingDivisor),
        qMax(1, rgb.height() / kWorkingDivisor));

    for (const auto factor : kFactors) {
        if (factor <= 1.0) {
            // Untouched, at full resolution: the first frame has to be
            // indistinguishable from the window it covers.
            pyramid.factors.append(1.0);
            pyramid.levels.append(rgb);
            continue;
        }
        const QSize reduced(
            qMax(1, int(rgb.width() / factor)),
            qMax(1, int(rgb.height() / factor)));
        // Downscaling with SmoothTransformation area-averages, which is where
        // the information actually goes; the walk back up only reconstructs.
        auto level = rgb.scaled(
            reduced, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        pyramid.factors.append(factor);
        pyramid.levels.append(smoothUpTo(std::move(level), working));
    }
    return pyramid;
}

void PaintCurtain(
        QPainter &p,
        const QRect &widgetRect,
        const QRect &contentRect,
        const CurtainPyramid &pyramid,
        qreal progress,
        const QColor &paper) {
    // Every pixel, unconditionally and first: the curtain widget is opaque, and
    // anything left unpainted is a hole straight through to the live window.
    p.fillRect(widgetRect, paper);
    if (pyramid.isEmpty() || contentRect.isEmpty()) {
        return;
    }

    const auto stage = CurtainStageAt(progress);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // Pull back about the centre.
    const auto centre = QRectF(contentRect).center();
    QRectF target(0, 0, contentRect.width() * stage.scale, contentRect.height() * stage.scale);
    target.moveCenter(centre);

    // Bracket the requested softness with two prebuilt levels and cross-fade,
    // so the ramp is continuous instead of stepping between them. The FINER of
    // the two is never finer than `stage.downsample` — which is what makes the
    // floor in CurtainStageAt an actual guarantee about painted pixels.
    int fine = 0;
    for (int i = 0; i < pyramid.factors.size(); ++i) {
        if (pyramid.factors.at(i) <= stage.downsample) {
            fine = i;
        }
    }
    const auto coarse = qMin(fine + 1, int(pyramid.factors.size()) - 1);
    const auto fineFactor = pyramid.factors.at(fine);
    const auto coarseFactor = pyramid.factors.at(coarse);
    const auto blend = (coarseFactor > fineFactor)
        ? qBound(qreal(0), (stage.downsample - fineFactor) / (coarseFactor - fineFactor), qreal(1))
        : qreal(0);

    p.setOpacity(1.0);
    p.drawImage(target, pyramid.levels.at(fine));
    if (blend > 0.0) {
        p.setOpacity(blend);
        p.drawImage(target, pyramid.levels.at(coarse));
    }
    p.setOpacity(1.0);

    // Dissolve the content's outer edge into the paper, so the pulled-back
    // frame has no crisp border to read as a rectangle.
    if (stage.edgeFeather > 0.0) {
        const auto band = qMax(
            qreal(16),
            qMin(target.width(), target.height()) * 0.05);
        auto edge = paper;
        edge.setAlphaF(stage.edgeFeather);
        auto inner = paper;
        inner.setAlphaF(0.0);
        const struct { QPointF from, to; QRectF rect; } sides[] = {
            { target.topLeft(), target.topLeft() + QPointF(0, band),
              QRectF(target.left(), target.top(), target.width(), band) },
            { target.bottomLeft(), target.bottomLeft() - QPointF(0, band),
              QRectF(target.left(), target.bottom() - band, target.width(), band) },
            { target.topLeft(), target.topLeft() + QPointF(band, 0),
              QRectF(target.left(), target.top(), band, target.height()) },
            { target.topRight(), target.topRight() - QPointF(band, 0),
              QRectF(target.right() - band, target.top(), band, target.height()) },
        };
        for (const auto &side : sides) {
            QLinearGradient feather(side.from, side.to);
            feather.setColorAt(0.0, edge);
            feather.setColorAt(1.0, inner);
            p.fillRect(side.rect, feather);
        }
    }

    // The wash rises: full strength behind the front, thinning above it — but
    // never to nothing, so no band of the field is left unwashed.
    QLinearGradient wash(
        QPointF(0, widgetRect.bottom()),
        QPointF(0, widgetRect.top()));
    auto washed = paper;
    washed.setAlphaF(stage.washAlpha);
    auto thin = paper;
    thin.setAlphaF(stage.washAlpha * 0.55);
    wash.setColorAt(0.0, washed);
    wash.setColorAt(qBound(0.0, stage.washFront, 1.0), washed);
    wash.setColorAt(1.0, thin);
    p.fillRect(widgetRect, wash);
}

} // namespace TeleMatrix

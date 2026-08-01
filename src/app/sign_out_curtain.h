// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QColor>
#include <QImage>
#include <QList>
#include <QRect>
#include <QSize>

class QPainter;

// NB: deliberately NOT in a `TeleMatrix::Ui` namespace. Declaring one shadows
// the global `::Ui` namespace for every header included afterwards, which
// breaks compilation across unrelated files.

namespace TeleMatrix {

// The sign-out curtain: the app window drifts out of focus and pulls back a
// notch while a paper wash rises through it. Colour is kept — only sharpness
// goes.
//
// This is a privacy device, not decoration. Every surface that can capture the
// app — a screenshot, a screen recording, a taskbar/Exposé thumbnail, a
// screen-share — reads the window's composited buffer, so the requirement
// "nothing legible after 40%" reduces to: past `kCurtainOpaqueFrom`, the pixels
// the curtain paints must carry no legible structure.
//
// That is why this works by DESTROYING resolution rather than by applying a
// visual blur. A blur is a filter over content that is still there; a
// downsample throws the content away. Past the floor the frame is drawn from a
// level built at 1/32 of the original: on a 900px-wide window that is 28 pixels
// across, so 13px body text is a fraction of a pixel per glyph and a 40px
// avatar is barely one.
//
// The floor is deliberately a factor the pyramid actually holds. Painting
// cross-fades the two levels bracketing `downsample`, so a floor of, say, 24
// would still blit the 1/16 level at full opacity underneath — the guarantee is
// about the finest level that reaches the screen, not the nominal number.
//
// `CurtainStageAt` is deliberately separate from the painting so the guarantee
// is a property of a pure function, and can be asserted without a screen.

// Past this point in the transition the floors below are in force.
inline constexpr qreal kCurtainOpaqueFrom = 0.4;
// Coarsest resolution the source may still be drawn at once the floor applies.
// Must be a factor present in the pyramid — see the note above.
inline constexpr qreal kCurtainFloorDownsample = 32.0;
inline constexpr qreal kCurtainFloorWashAlpha = 0.28;

struct CurtainStage {
    // Source resolution divisor; 1 is pristine, larger is softer. Never < 1.
    qreal downsample = 1.0;
    // Strength of the paper wash across the whole field.
    qreal washAlpha = 0.0;
    // How far up the field the wash front has risen, 0 (bottom) to 1 (top).
    qreal washFront = 0.0;
    // Content pull-back, as a fraction of the content rect.
    qreal scale = 1.0;
    // How strongly the content's outer edge dissolves into the paper. Without
    // it the pulled-back frame meets the margin at a crisp rectangular border,
    // which reads as a pasted screenshot rather than a window softening away.
    qreal edgeFeather = 0.0;
};

[[nodiscard]] CurtainStage CurtainStageAt(qreal progress);

// The warm off-white the field settles into. Deliberately fixed rather than
// taken from the live theme: the transition always ends on the intro, which is
// always light, and a dark theme would otherwise settle a session into a dark
// field — the one ending the effect must never have.
[[nodiscard]] QColor CurtainPaper();

// Progressively softer copies of the frozen window, built once when the curtain
// is raised.
//
// Two things make the ramp read as smooth rather than as steps. The factors
// form a fine geometric progression (~1.5x apart, not 2x), so cross-fading
// between neighbours is a small change; and each level is rebuilt back up to a
// common working size in repeated 2x passes, because bilinear interpolation
// stretched straight from 1/32 to full size leaves the blocky diamond pattern
// that reads as pixelation rather than as defocus.
//
// Level 0 is the untouched source at full resolution, so the first frame is
// pixel-identical to the window it replaces.
struct CurtainPyramid {
    QList<qreal> factors;
    QList<QImage> levels;
    QSize sourceSize;

    [[nodiscard]] bool isEmpty() const { return factors.isEmpty(); }
};

[[nodiscard]] CurtainPyramid BuildCurtainPyramid(const QImage &source);

// Paint one frame. Responsible for EVERY pixel of `widgetRect` — the curtain
// widget is opaque, and a gap would show the live window underneath.
// `contentRect` is where the frozen content sits within it.
void PaintCurtain(
    QPainter &p,
    const QRect &widgetRect,
    const QRect &contentRect,
    const CurtainPyramid &pyramid,
    qreal progress,
    const QColor &paper);

} // namespace TeleMatrix

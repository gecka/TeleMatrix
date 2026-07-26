// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QImage>
#include <QSize>

namespace TeleMatrix {

// Prepare a decoded video frame for display: downscale it to `targetDeviceSize`
// (long-edge cap via KeepAspectRatio) when the source is larger, and force it
// opaque.
//
// The opacity step is the important one: QVideoFrame::toImage() can hand back a
// premultiplied-alpha format whose alpha byte is unreliable for an (alpha-less)
// video frame, which washes the frame out over a light background (the inline chat
// bubble). The frame is composited over black into a genuinely-opaque format so it
// renders correctly on both the opaque timeline and the translucent fullscreen
// overlay. See the .cpp for the full rationale.
//
// Shared by the inline timeline player and the fullscreen overlay so both get the
// fix.
[[nodiscard]] QImage downscaleVideoFrame(const QImage &frame, QSize targetDeviceSize);

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "media/video_frame_scaler.h"

#include <QPainter>

namespace TeleMatrix {
namespace {

// Flatten a decoded video frame to a genuinely-opaque image by compositing over
// black. QVideoFrame::toImage() can hand back a premultiplied-alpha format (e.g.
// Format_RGBA8888_Premultiplied) whose alpha byte is unreliable — a video has no
// transparency, but the conversion leaves garbage (observed alpha 71..255) there.
// Over a light background (the inline chat bubble) the (1 - alpha) term lets the
// background bleed through, washing the frame out in patches.
//
// Compositing over black with SourceOver yields the frame's own RGB at full opacity
// (for these frames the stored RGB is the true colour, only mislabeled premultiplied
// — over black that is exactly what the fullscreen viewer already shows). Crucially
// the result is a real opaque format (RGB32, alpha bytes 0xff), so it also stays
// opaque when blitted onto the *translucent* fullscreen-overlay window — unlike a
// bare reinterpret-to-RGBX, whose leftover alpha bytes made that window see-through.
QImage toOpaqueVideoFrame(const QImage &img) {
    if (!img.hasAlphaChannel()) {
        return img; // already opaque (e.g. RGB32 / RGBX8888)
    }
    QImage out(img.size(), QImage::Format_RGB32);
    out.fill(Qt::black);
    QPainter p(&out);
    p.drawImage(0, 0, img);
    p.end();
    return out;
}

} // namespace

QImage downscaleVideoFrame(const QImage &frame, QSize targetDeviceSize) {
    if (frame.isNull()) {
        return frame;
    }
    QImage src = frame;
    if (targetDeviceSize.isValid()
        && !targetDeviceSize.isEmpty()
        && src.width() > targetDeviceSize.width()) {
        // Downscale once (KeepAspectRatio). Smooth-scaling the premultiplied frame is
        // fine: it linearly averages the (straight) RGB channels, and the unreliable
        // alpha is discarded by toOpaqueVideoFrame below. Compositing after the scale
        // also keeps the flatten cheap (it runs on the smaller frame).
        src = src.scaled(
            targetDeviceSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return toOpaqueVideoFrame(src);
}

} // namespace TeleMatrix

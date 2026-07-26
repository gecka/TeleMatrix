// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_video_placeholder.h"

#include <QColor>

#include <array>

namespace TeleMatrix {
namespace {

// splitmix64 — a tiny deterministic PRNG; avoids std::random's distribution
// objects and stays reproducible across runs and platforms.
quint64 mix(quint64 &s) {
    s += 0x9E3779B97F4A7C15ULL;
    quint64 z = s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

} // namespace

QImage syntheticBlurPlaceholder(const QString &seed, int w, int h) {
    w = qMax(2, w);
    h = qMax(2, h);

    // Seed the PRNG from the event id (FNV-1a) so the same message always
    // produces the same placeholder.
    quint64 s = 1469598103934665603ULL;
    for (const auto ch : seed) {
        s = (s ^ ch.unicode()) * 1099511628211ULL;
    }

    // Three soft color "blobs" at random centers; each pixel is their
    // distance-weighted blend → a smooth multi-color gradient.
    struct Blob {
        qreal cx;
        qreal cy;
        QColor c;
    };
    std::array<Blob, 3> blobs{};
    for (auto &b : blobs) {
        b.cx = (mix(s) % 1000) / 1000.0;
        b.cy = (mix(s) % 1000) / 1000.0;
        // Mid saturation/value so it reads as a soft photo blur, not neon.
        b.c = QColor::fromHsv(
            int(mix(s) % 360), 90 + int(mix(s) % 80), 90 + int(mix(s) % 80));
    }

    QImage img(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        const qreal ny = qreal(y) / (h - 1);
        for (int x = 0; x < w; ++x) {
            const qreal nx = qreal(x) / (w - 1);
            qreal wr = 0, wg = 0, wb = 0, wsum = 0;
            for (const auto &b : blobs) {
                const qreal dx = nx - b.cx;
                const qreal dy = ny - b.cy;
                const qreal wgt = 1.0 / (0.02 + dx * dx + dy * dy);
                wr += b.c.red() * wgt;
                wg += b.c.green() * wgt;
                wb += b.c.blue() * wgt;
                wsum += wgt;
            }
            img.setPixel(
                x, y, qRgb(int(wr / wsum), int(wg / wsum), int(wb / wsum)));
        }
    }
    return img;
}

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QColor>
#include <QPainter>
#include <QRectF>

#include <cmath>

namespace Ui {

/// Draw one frame of lib_ui's InfiniteRadialAnimation (verbatim steady-state port,
/// connecting params): rotates once/sec clockwise while the arc pulses 22.5°↔270°
/// on a 3s cycle. `nowMs` is monotonic ms since the spinner started; the arc is
/// stroked inside `box` with the given `thickness`/`color`. Shared by the
/// bottom-left connecting pill and the chat-header connecting status.
inline void DrawConnectingRadial(
        QPainter &p,
        const QRectF &box,
        qreal thickness,
        const QColor &color,
        qint64 nowMs) {
    constexpr int kFull = 360 * 16; // 5760, 1/16°
    constexpr int linearPeriod = 1000;
    constexpr int sinePeriod = 3000;
    constexpr int sineDuration = 1000;
    constexpr int sineShift = 1500;
    constexpr double kPi = 3.14159265358979323846;

    const int minLen = int(std::lround(kFull * 0.0625)); // 360 (22.5°)
    const int maxLen = int(std::lround(kFull * 0.75));    // 4320 (270°)
    const int linear = kFull - int((nowMs * qint64(kFull) / linearPeriod) % kFull);

    const auto sineInOut = [](double dt) { return -0.5 * (std::cos(kPi * dt) - 1.0); };
    const auto lerp = [](int a, int b, double r) {
        return int(std::lround(a + double(b - a) * r));
    };

    const qint64 cycles = nowMs / sinePeriod;
    const qint64 relative = nowMs % sinePeriod;
    const int smallDuration = sineShift - sineDuration; // 500
    const int basic = int((linear + minLen
        + cycles * (kFull + minLen - maxLen)) % kFull);

    int arcFrom = basic - minLen;
    int arcLength = minLen;
    if (relative <= smallDuration) {
        // held at min
    } else if (relative <= smallDuration + sineDuration) {
        const auto pr = sineInOut((relative - smallDuration) / double(sineDuration));
        arcLength = lerp(minLen, maxLen, pr);
        arcFrom = basic - arcLength;
    } else if (relative <= sinePeriod - sineDuration) {
        arcLength = maxLen;
        arcFrom = basic - maxLen;
    } else {
        const auto pr = sineInOut(
            (relative - (sinePeriod - sineDuration)) / double(sineDuration));
        arcLength = maxLen - lerp(0, maxLen - minLen, pr);
        arcFrom = basic - maxLen;
    }

    QPen pen(color);
    pen.setWidthF(thickness);
    pen.setCapStyle(Qt::RoundCap);
    const auto wasAA = p.testRenderHint(QPainter::Antialiasing);
    const auto wasPen = p.pen();
    const auto wasBrush = p.brush();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawArc(box, arcFrom, arcLength);
    p.setPen(wasPen);
    p.setBrush(wasBrush);
    p.setRenderHint(QPainter::Antialiasing, wasAA);
}

} // namespace Ui

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "ui/style/runtime_scale.h"

namespace TeleMatrix::Style {
namespace {

int DevicePixelRatioValue = 1;
int ScaleValue = kScaleDefault;

} // namespace

int DevicePixelRatio() {
    return DevicePixelRatioValue;
}

void SetDevicePixelRatio(int ratio) {
    DevicePixelRatioValue = std::clamp(ratio, 1, kScaleMax / kScaleMin);
}

int Scale() {
    return ScaleValue;
}

void SetScale(int scale) {
    ScaleValue = (scale == kScaleAuto) ? kScaleDefault : scale;
}

int MaxScaleForRatio(int ratio) {
    return (ratio > 0)
        ? std::max(kScaleMax / ratio, kScaleAlwaysAllowMax)
        : kScaleAlwaysAllowMax;
}

int CheckScale(int scale) {
    return (scale == kScaleAuto)
        ? kScaleAuto
        : std::clamp(scale, kScaleMin, MaxScaleForRatio(DevicePixelRatio()));
}

} // namespace TeleMatrix::Style

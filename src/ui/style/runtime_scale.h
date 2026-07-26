// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QSize>
#include <algorithm>
#include <cmath>

namespace TeleMatrix::Style {

inline constexpr auto kScaleAuto = 0;
inline constexpr auto kScaleMin = 50;
inline constexpr auto kScaleDefault = 100;
inline constexpr auto kScaleMax = 300;
inline constexpr auto kScaleAlwaysAllowMax = 200;

[[nodiscard]] int DevicePixelRatio();
void SetDevicePixelRatio(int ratio);

[[nodiscard]] int Scale();
void SetScale(int scale);

[[nodiscard]] int MaxScaleForRatio(int ratio);
[[nodiscard]] int CheckScale(int scale);

template <typename T>
[[nodiscard]] inline T ConvertScale(T value, int scale) {
    if (value < 0.) {
        return -ConvertScale(-value, scale);
    }
    const auto result = T(std::round(
        (double(value) * scale / 100.) - 0.01));
    if constexpr (std::is_integral_v<T>) {
        return (!value || result) ? result : 1;
    } else {
        return result;
    }
}

template <typename T>
[[nodiscard]] inline T ConvertScale(T value) {
    return ConvertScale(value, Scale());
}

template <typename T>
[[nodiscard]] inline T ConvertScaleExact(T value, int scale) {
    return (value < 0.)
        ? (-ConvertScale(-value, scale))
        : T(double(value) * scale / 100.);
}

template <typename T>
[[nodiscard]] inline T ConvertScaleExact(T value) {
    return ConvertScaleExact(value, Scale());
}

[[nodiscard]] inline QSize ConvertScale(QSize size) {
    return QSize(ConvertScale(size.width()), ConvertScale(size.height()));
}

} // namespace TeleMatrix::Style

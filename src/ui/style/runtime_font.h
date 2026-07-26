// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QString>

namespace TeleMatrix::Style {

/// Set the custom font family before any UI is created.
/// Empty = default (Open Sans), "system" = system font, else = specific family.
void SetCustomFont(const QString &family);

/// Get the currently configured custom font family.
[[nodiscard]] const QString &CustomFont();

/// Returns true if a custom font has been set (non-empty).
[[nodiscard]] bool HasCustomFont();

/// Returns the effective font family to use for base fonts.
/// Resolves "system" to the actual system font family.
[[nodiscard]] QString EffectiveFontFamily();

} // namespace TeleMatrix::Style

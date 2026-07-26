// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "styles/style_constants.h"
#include "theme/chat_background.h"

// The st:: bridge for chat_background.h, which stays palette-free so it can be
// unit-tested outside the app. Kept inline (not in telematrix_core) because the
// st:: colours are mutated by ThemeManager at runtime and must be read late.
namespace TeleMatrix::Theme {

// Default wallpaper corner colours, used when the theme ships no background
// image of its own.
[[nodiscard]] inline CornerColors DefaultCornerColors() {
	return {
		st::historyBgTopLeft,
		st::historyBgTopRight,
		st::historyBgBottomRight,
		st::historyBgBottomLeft,
	};
}

} // namespace TeleMatrix::Theme

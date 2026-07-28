// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

// Stand-ins for the handful of Desktop App Toolkit names that the vendored emoji
// sources expect — `ui/emoji_config.*` (copied from desktop-app/lib_ui) and the
// `codegen_emoji` output, which we do not control.
//
// Supplying them here is what lets those files stay textually close to upstream, so
// re-syncing them later is a readable diff, without dragging lib_base, lib_crl and
// lib_rpl into the build for six identifiers.

#include <QtCore/QLatin1String>
#include <QtCore/QString>
#include <QtCore/qglobal.h>

#include <cstddef>

// gsl/gsl also defines Expects()/Ensures(), which the copied code uses; taking them
// from here rather than redefining keeps one owner for the assertion macros.
#include <gsl/gsl>

using uint32 = quint32;
using uint64 = quint64;

namespace base {

template <typename Type, std::size_t Size>
[[nodiscard]] inline constexpr std::size_t array_size(
		const Type (&)[Size]) noexcept {
	return Size;
}

} // namespace base

template <std::size_t Size>
[[nodiscard]] inline QLatin1String qstr(const char (&string)[Size]) {
	return QLatin1String(string, int(Size) - 1);
}

// lib_base spells these Assert/Unexpected; gsl covers Expects.
#ifndef Assert
#define Assert(condition) Expects(condition)
#endif // Assert

#ifndef Unexpected
#define Unexpected(message) Q_UNREACHABLE()
#endif // Unexpected

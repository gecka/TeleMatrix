// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QObject>

#include <optional>

namespace TeleMatrix::Theme {

// Watches the OS dark/light mode and emits changes.
// Uses Qt 6.5+ QStyleHints::colorSchemeChanged when available,
// with fallback to palette lightness heuristic.
class SystemThemeWatcher : public QObject {
	Q_OBJECT

public:
	explicit SystemThemeWatcher(QObject *parent = nullptr);

	// Current system dark mode state.
	[[nodiscard]] std::optional<bool> isDark() const;

Q_SIGNALS:
	void systemDarkModeChanged(bool isDark);

private:
	void checkCurrentScheme();

	std::optional<bool> _isDark;
};

} // namespace TeleMatrix::Theme

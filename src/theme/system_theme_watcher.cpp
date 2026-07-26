// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "system_theme_watcher.h"

#include <QApplication>
#include <QPalette>
#include <QStyleHints>

namespace TeleMatrix::Theme {

SystemThemeWatcher::SystemThemeWatcher(QObject *parent)
	: QObject(parent)
{
	// Qt 6.5+ provides QStyleHints::colorSchemeChanged.
	if (auto *hints = QGuiApplication::styleHints()) {
		connect(hints, &QStyleHints::colorSchemeChanged,
				this, [this](Qt::ColorScheme scheme) {
			const bool dark = (scheme == Qt::ColorScheme::Dark);
			if (!_isDark.has_value() || _isDark.value() != dark) {
				_isDark = dark;
				emit systemDarkModeChanged(dark);
			}
		});
	}

	checkCurrentScheme();
}

std::optional<bool> SystemThemeWatcher::isDark() const {
	return _isDark;
}

void SystemThemeWatcher::checkCurrentScheme() {
	if (auto *hints = QGuiApplication::styleHints()) {
		const auto scheme = hints->colorScheme();
		if (scheme != Qt::ColorScheme::Unknown) {
			_isDark = (scheme == Qt::ColorScheme::Dark);
			return;
		}
	}
	// Fallback: check palette lightness.
	const auto windowBg = QGuiApplication::palette().color(QPalette::Window);
	_isDark = (windowBg.lightnessF() < 0.5);
}

} // namespace TeleMatrix::Theme

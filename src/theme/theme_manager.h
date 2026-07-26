// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QColor>
#include <QHash>
#include <QObject>
#include <QString>

#include <optional>

namespace TeleMatrix::Theme {

enum class ThemeMode {
	Day,
	Night,
	System,
};

// Central runtime authority for theme mode, palette application, and
// notifications. Singleton owned by AppController.
class ThemeManager : public QObject {
	Q_OBJECT

public:
	explicit ThemeManager(QObject *parent = nullptr);
	~ThemeManager() override;

	// Initialize from persisted settings. Call before first window show.
	void initializeFromSettings(
		const QString &themeId,
		ThemeMode mode,
		bool systemDarkModeEnabled);

	// Current state.
	[[nodiscard]] ThemeMode mode() const { return _mode; }
	[[nodiscard]] bool isNight() const { return _isNight; }
	[[nodiscard]] const QString &themeId() const { return _themeId; }
	// Whether the theme follows the OS dark mode ("Auto-night mode").
	[[nodiscard]] bool systemDarkModeEnabled() const {
		return _systemDarkModeEnabled;
	}

	// Mode control.
	void setMode(ThemeMode mode);
	void toggleNightMode();

	// Picking a theme card: swaps family and pins the variant in one apply, so
	// consumers see a single themeChanged. Unknown ids fall back to the default.
	void setThemeAndMode(const QString &id, ThemeMode mode);

	// System dark mode state (from OS watcher).
	void setSystemDarkModeEnabled(bool enabled);
	void setSystemDarkState(std::optional<bool> isDark);

Q_SIGNALS:
	void themeChanged(bool isNight, ThemeMode mode);

private:
	// Loads the current theme's day+night palettes and points the doodle at its
	// pattern. Does not apply anything.
	void loadPalettes();
	void applyTheme(bool isNight);
	void applyPalette(const QHash<QString, QColor> &palette);
	void applyAppPalette(bool isNight);
	// Resolves a mode against the OS dark state.
	[[nodiscard]] bool nightForMode(ThemeMode mode) const;

	QString _themeId;
	ThemeMode _mode = ThemeMode::Day;
	bool _isNight = false;
	bool _systemDarkModeEnabled = true;
	std::optional<bool> _systemDark;

	QHash<QString, QColor> _dayPalette;
	QHash<QString, QColor> _nightPalette;
};

} // namespace TeleMatrix::Theme

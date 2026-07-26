// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "theme/chat_background.h"

#include <QColor>
#include <QHash>
#include <QString>
#include <QVector>

// The named theme families the app ships. Each has a day and a night palette
// plus its own doodle pattern. Kept free of st:: and QtWidgets so the settings
// preview and the unit tests can read a theme's colours without applying it.
namespace TeleMatrix::Theme {

// Dubai carries TeleMatrix's original colours, so a fresh install looks the
// way it always has.
inline const auto kDefaultThemeId = QStringLiteral("dubai");

struct ThemeDefinition {
	QString id;
	QString name; // display name, read from the theme's meta.json

	[[nodiscard]] QString displayName() const;
	[[nodiscard]] QString palettePath(bool night) const;
	[[nodiscard]] QString patternPath() const;
};

// Every theme the app ships, discovered from the bundled resources rather than
// a hard-coded list. Order is incidental -- the picker sorts by display name.
[[nodiscard]] const QVector<ThemeDefinition> &AllThemes();

// Sorted by the theme's name as the user reads it, so the picker's order
// follows the active translation rather than the ids.
[[nodiscard]] QVector<ThemeDefinition> ThemesByName();

// Unknown ids resolve to the default theme, so a settings file naming a theme
// this build doesn't have still starts.
[[nodiscard]] const ThemeDefinition &ThemeById(const QString &id);

// Read and parse a palette file (qrc or filesystem). Empty on failure.
[[nodiscard]] QHash<QString, QColor> LoadPalette(const QString &path);

// The handful of colours the settings theme cards draw, read straight from a
// palette file without applying it to st::.
struct ThemePreviewColors {
	CornerColors background; // historyBg{TopLeft,TopRight,BottomRight,BottomLeft}
	QColor sent;             // msgOutBg
	QColor received;         // msgInBg
	QColor accent;           // windowBgActive
};

[[nodiscard]] ThemePreviewColors PreviewColors(const QString &themeId, bool night);

} // namespace TeleMatrix::Theme

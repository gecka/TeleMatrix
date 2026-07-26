// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "theme/theme_registry.h"
#include "theme/theme_parser.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace TeleMatrix::Theme {

QString ThemeDefinition::displayName() const {
	if (!name.isEmpty()) {
		return name;
	}
	// Fallback for a theme dir whose meta.json is missing or unreadable: a
	// legible name from the id, e.g. "castile-leon" -> "Castile Leon".
	auto pretty = id;
	pretty.replace(u'-', u' ');
	auto atWordStart = true;
	for (auto &ch : pretty) {
		if (ch.isSpace()) {
			atWordStart = true;
		} else if (atWordStart) {
			ch = ch.toUpper();
			atWordStart = false;
		}
	}
	return pretty;
}

QString ThemeDefinition::palettePath(bool night) const {
	return QStringLiteral(":/theme/%1/%2/colors.tdesktop-theme")
		.arg(id, night ? QStringLiteral("night") : QStringLiteral("day"));
}

QString ThemeDefinition::patternPath() const {
	return QStringLiteral(":/theme/%1/pattern.svg").arg(id);
}

const QVector<ThemeDefinition> &AllThemes() {
	// Discovered from the bundled resources: every :/theme/<id>/ directory is a
	// theme, its display name read from that theme's meta.json. Adding a theme
	// is therefore a generate.py concern (it writes the palettes, meta.json and
	// the qrc) -- there is no list to edit here. Cached: the bundled set is fixed
	// for the life of the process.
	static const QVector<ThemeDefinition> themes = [] {
		QVector<ThemeDefinition> discovered;
		const auto ids = QDir(QStringLiteral(":/theme"))
			.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
		discovered.reserve(ids.size());
		for (const auto &id : ids) {
			QString name;
			QFile meta(QStringLiteral(":/theme/%1/meta.json").arg(id));
			if (meta.open(QIODevice::ReadOnly)) {
				name = QJsonDocument::fromJson(meta.readAll())
					.object()
					.value(QStringLiteral("name"))
					.toString();
			}
			discovered.push_back({ id, name });
		}
		return discovered;
	}();
	return themes;
}

const ThemeDefinition &ThemeById(const QString &id) {
	const auto &themes = AllThemes();
	const auto find = [&themes](const QString &wanted) {
		return std::find_if(themes.begin(), themes.end(),
			[&wanted](const ThemeDefinition &theme) { return theme.id == wanted; });
	};
	auto it = find(id);
	if (it == themes.end()) {
		it = find(kDefaultThemeId);
	}
	return (it != themes.end()) ? *it : themes.first();
}

QVector<ThemeDefinition> ThemesByName() {
	auto themes = AllThemes();
	std::sort(themes.begin(), themes.end(),
		[](const ThemeDefinition &a, const ThemeDefinition &b) {
			return QString::localeAwareCompare(a.displayName(), b.displayName()) < 0;
		});
	return themes;
}

QHash<QString, QColor> LoadPalette(const QString &path) {
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		return {};
	}
	return parseThemeColors(QString::fromUtf8(file.readAll()));
}

ThemePreviewColors PreviewColors(const QString &themeId, bool night) {
	const auto palette = LoadPalette(ThemeById(themeId).palettePath(night));
	const auto color = [&](const char *token) {
		return palette.value(QLatin1String(token));
	};
	return {
		.background = {
			color("historyBgTopLeft"),
			color("historyBgTopRight"),
			color("historyBgBottomRight"),
			color("historyBgBottomLeft"),
		},
		.sent = color("msgOutBg"),
		.received = color("msgInBg"),
		.accent = color("windowBgActive"),
	};
}

} // namespace TeleMatrix::Theme

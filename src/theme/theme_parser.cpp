// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "theme_parser.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringList>

namespace TeleMatrix::Theme {

namespace {

// Parse a hex color string: #rrggbb or #rrggbbaa.
QColor parseHexColor(const QString &value) {
	if (value.size() != 7 && value.size() != 9) {
		return {};
	}
	bool ok = false;
	if (value.size() == 7) {
		// #rrggbb
		const auto r = value.mid(1, 2).toInt(&ok, 16); if (!ok) return {};
		const auto g = value.mid(3, 2).toInt(&ok, 16); if (!ok) return {};
		const auto b = value.mid(5, 2).toInt(&ok, 16); if (!ok) return {};
		return QColor(r, g, b);
	}
	// #rrggbbaa
	const auto r = value.mid(1, 2).toInt(&ok, 16); if (!ok) return {};
	const auto g = value.mid(3, 2).toInt(&ok, 16); if (!ok) return {};
	const auto b = value.mid(5, 2).toInt(&ok, 16); if (!ok) return {};
	const auto a = value.mid(7, 2).toInt(&ok, 16); if (!ok) return {};
	return QColor(r, g, b, a);
}

} // namespace

QHash<QString, QColor> parseThemeColors(const QString &data) {
	// Two-pass approach:
	// Pass 1: collect raw key->value strings (hex or alias name).
	// Pass 2: resolve aliases.

	struct RawEntry {
		QString value;
		bool isAlias = false;
	};
	QHash<QString, RawEntry> raw;

	const auto lines = data.split(u'\n');
	for (const auto &rawLine : lines) {
		auto line = rawLine.trimmed();
		if (line.isEmpty() || line.startsWith(u"//")) {
			continue;
		}
		// Strip trailing comment: "key: value; // comment"
		const auto commentPos = line.indexOf(u"//");
		if (commentPos >= 0) {
			line = line.left(commentPos).trimmed();
		}
		// Strip trailing semicolon.
		if (line.endsWith(u';')) {
			line.chop(1);
			line = line.trimmed();
		}
		// Split on first ':'.
		const auto colonPos = line.indexOf(u':');
		if (colonPos <= 0) {
			continue;
		}
		const auto key = line.left(colonPos).trimmed();
		const auto value = line.mid(colonPos + 1).trimmed();
		if (key.isEmpty() || value.isEmpty()) {
			continue;
		}
		const bool isHex = value.startsWith(u'#');
		raw.insert(key, { value, !isHex });
	}

	// Resolve all entries.
	QHash<QString, QColor> result;
	result.reserve(raw.size());

	// Iterative resolution (handles chains of aliases).
	for (auto it = raw.cbegin(); it != raw.cend(); ++it) {
		const auto &key = it.key();
		if (result.contains(key)) {
			continue;
		}

		// Walk the alias chain.
		QStringList chain;
		QSet<QString> visited;
		QString current = key;
		while (true) {
			auto rawIt = raw.constFind(current);
			if (rawIt == raw.cend()) {
				break;
			}
			if (!rawIt->isAlias) {
				// Hex value — resolve the whole chain.
				const auto color = parseHexColor(rawIt->value);
				for (const auto &chainKey : chain) {
					result.insert(chainKey, color);
				}
				result.insert(current, color);
				break;
			}
			// Alias — check if already resolved.
			if (result.contains(rawIt->value)) {
				const auto color = result.value(rawIt->value);
				for (const auto &chainKey : chain) {
					result.insert(chainKey, color);
				}
				result.insert(current, color);
				break;
			}
			// Cycle guard: a self-referential or circular alias chain
			// (e.g. `a: b`, `b: a`) never reaches a hex value and would
			// otherwise loop forever. If we've already walked through this
			// key in the current chain, drop the unresolvable chain.
			if (visited.contains(current)) {
				break;
			}
			visited.insert(current);
			chain.append(current);
			current = rawIt->value;
		}
	}

	return result;
}

} // namespace TeleMatrix::Theme

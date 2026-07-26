// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QColor>
#include <QHash>
#include <QString>

namespace TeleMatrix::Theme {

// Parse a colors.tdesktop-theme file into a key -> QColor map.
// Handles:
//   - hex colors: #rrggbb, #rrggbbaa
//   - alias references: key: otherKey;
// Returns the resolved palette map.
[[nodiscard]] QHash<QString, QColor> parseThemeColors(const QString &data);

} // namespace TeleMatrix::Theme

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "ui/format_bytes.h"

namespace TeleMatrix {

QString formatBytes(quint64 size) {
    static const char *const units[] = { "B", "KB", "MB", "GB", "TB" };
    auto value = static_cast<double>(size);
    auto unitIndex = 0;
    while (value >= 1024.0 && unitIndex < 4) {
        value /= 1024.0;
        ++unitIndex;
    }
    if (unitIndex == 0) {
        return QString::number(static_cast<quint64>(value))
            + QStringLiteral(" ")
            + QString::fromLatin1(units[unitIndex]);
    }
    return QString::number(value, 'f', 1)
        + QStringLiteral(" ")
        + QString::fromLatin1(units[unitIndex]);
}

} // namespace TeleMatrix

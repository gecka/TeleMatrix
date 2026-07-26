// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "ui/recovery_key_format.h"

namespace TeleMatrix {
namespace {

constexpr int kGroupSize = 4;

// Separators standing before the n-th key character (0 before the first group).
[[nodiscard]] int SeparatorsBefore(int keyChars) {
    return keyChars > 0 ? (keyChars - 1) / kGroupSize : 0;
}

} // namespace

RecoveryKeyText FormatRecoveryKey(const QString &input, int cursor) {
    QString key;
    key.reserve(input.size());
    int keyCharsBeforeCursor = 0;
    for (int i = 0; i < input.size(); ++i) {
        const auto ch = input.at(i);
        if (ch.isSpace()) {
            continue; // regrouped below; the user's own spaces don't survive
        }
        if (i < cursor) {
            ++keyCharsBeforeCursor;
        }
        key.append(ch);
    }

    QString formatted;
    formatted.reserve(key.size() + key.size() / kGroupSize);
    for (int i = 0; i < key.size(); ++i) {
        if (i > 0 && i % kGroupSize == 0) {
            formatted.append(QLatin1Char(' '));
        }
        formatted.append(key.at(i));
    }

    return RecoveryKeyText{
        formatted,
        keyCharsBeforeCursor + SeparatorsBefore(keyCharsBeforeCursor),
    };
}

} // namespace TeleMatrix

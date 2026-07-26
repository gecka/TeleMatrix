// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QString>

namespace TeleMatrix {

/// A recovery key regrouped for display, plus where the caret ended up.
struct RecoveryKeyText {
    QString text;
    int cursor = 0;
};

/// Regroup a recovery key into space-separated groups of four, the way the key
/// is shown to the user when it is created (the spec's display format; decoders
/// strip the whitespace again). `cursor` is a caret offset into `input` and comes
/// back translated into the reformatted text, so a field can rewrite itself while
/// the user types without the caret jumping.
[[nodiscard]] RecoveryKeyText FormatRecoveryKey(const QString &input, int cursor);

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QString>

namespace TeleMatrix::MentionTrigger {

struct Result {
    int atPos = -1;   // index of '@' within blockText, -1 = no trigger
    QString query;    // text between '@' and the cursor
};

// Scan backward from posInBlock within one paragraph's text for an
// active @mention trigger. Allowed after any non-word
// boundary ("(@name"), rejected for e-mail/word tails ("mail@host").
[[nodiscard]] Result Scan(const QString &blockText, int posInBlock);

} // namespace TeleMatrix::MentionTrigger

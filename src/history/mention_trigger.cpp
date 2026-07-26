// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "mention_trigger.h"

namespace TeleMatrix::MentionTrigger {

Result Scan(const QString &blockText, int posInBlock) {
    int atPos = -1;
    for (int i = posInBlock - 1; i >= 0; --i) {
        const auto ch = blockText.at(i);
        if (ch == QLatin1Char('@')) {
            const auto beforeOk = i == 0
                || !(blockText.at(i - 1).isLetterOrNumber()
                    || blockText.at(i - 1) == QLatin1Char('_'));
            const auto afterOk = (posInBlock - i - 1 < 1)
                || blockText.at(i + 1).isLetter();
            if (beforeOk && afterOk) {
                atPos = i;
            }
            break;
        }
        if (ch.isSpace()) break;
    }
    if (atPos < 0) {
        return {};
    }
    return {atPos, blockText.mid(atPos + 1, posInBlock - atPos - 1)};
}

} // namespace TeleMatrix::MentionTrigger

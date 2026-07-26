// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "ui/widgets/block_format_presence.h"

#include <QTextBlock>

namespace Ui {

BlockFormatsPresence ScanBlockFormats(const QTextDocument *doc) {
    auto result = BlockFormatsPresence();
    for (auto block = doc->begin(); block != doc->end(); block = block.next()) {
        const auto &bfmt = block.blockFormat();
        if (bfmt.property(QTextFormat::BlockQuoteLevel).toInt() > 0) {
            result.quotes = true;
        }
        if (bfmt.nonBreakableLines()) {
            result.pre = true;
        }
        if (result.quotes && result.pre) break;
    }
    return result;
}

const BlockFormatsPresence &BlockFormatsMemo::get(const QTextDocument *doc) {
    const auto revision = doc->revision();
    if (revision != _revision) {
        _revision = revision;
        _value = ScanBlockFormats(doc);
    }
    return _value;
}

} // namespace Ui

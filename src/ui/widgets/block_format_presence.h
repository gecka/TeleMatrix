// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QTextDocument>

namespace Ui {

struct BlockFormatsPresence {
    bool quotes = false;
    bool pre = false;
    [[nodiscard]] bool any() const { return quotes || pre; }
};

// Full O(blocks) scan for blockquote/pre block formats.
[[nodiscard]] BlockFormatsPresence ScanBlockFormats(const QTextDocument *doc);

// Revision-keyed memo: rescans only when the document changed since the
// last call, so repeated reads within one keystroke (height check ×2 +
// paint) cost one scan total, and scroll-only repaints cost none.
class BlockFormatsMemo {
public:
    const BlockFormatsPresence &get(const QTextDocument *doc);

private:
    int _revision = -1;
    BlockFormatsPresence _value;
};

} // namespace Ui

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QTextEdit>

#include "ui/text/quote_paint.h"
#include "ui/widgets/block_format_presence.h"

namespace Ui {

/// QTextEdit subclass that paints blockquote decorations (vertical bar +
/// background tint) behind the text.
class FormattedTextEdit : public QTextEdit {
    Q_OBJECT

public:
    explicit FormattedTextEdit(QWidget *parent = nullptr);

    // Memoized quote/pre presence (revision-keyed; cheap on every call).
    [[nodiscard]] const BlockFormatsPresence &blockFormats() {
        return _blockFormats.get(document());
    }

protected:
    void paintEvent(QPaintEvent *e) override;
    void insertFromMimeData(const QMimeData *source) override;

private:
    Ui::Text::QuotePaintCache _blockquoteCache;
    Ui::Text::QuotePaintCache _preCache;
    BlockFormatsMemo _blockFormats;
};

} // namespace Ui

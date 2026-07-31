// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "ui/widgets/formatted_text_edit.h"

#include <QAbstractTextDocumentLayout>
#include <QMimeData>
#include <QPainter>
#include <QPaintEvent>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextFormat>
#include <QTextLayout>

#include "styles/style_constants.h"
#include "ui/text/quote_paint.h"
#include "ui/text/emoji_text.h"
#include "ui/widgets/emoji_objects.h"

#include <QInputMethodEvent>
#include <QTextCursor>
#include <QTextDocumentFragment>
#include <QTextDocument>

namespace Ui {

FormattedTextEdit::FormattedTextEdit(QWidget *parent)
    : QTextEdit(parent)
{
    _emoji = EmojiObjects::Install(this);
}

QVariant FormattedTextEdit::loadResource(int type, const QUrl &name) {
    if (type == QTextDocument::ImageResource) {
        const auto url = name.toString();
        if (EmojiObjects::IsEmojiUrl(url)) {
            return EmojiObjects::Resource(url);
        }
    }
    return QTextEdit::loadResource(type, name);
}

QMimeData *FormattedTextEdit::createMimeDataFromSelection() const {
    auto result = QTextEdit::createMimeDataFromSelection();
    if (!result) {
        return result;
    }
    // The document holds object-replacement characters where the emoji are; the
    // clipboard has to carry the emoji themselves.
    auto cursor = textCursor();
    if (cursor.hasSelection()) {
        auto fragment = QTextDocument();
        fragment.setHtml(cursor.selection().toHtml());
        result->setText(TeleMatrix::EmojiText::DocumentText(&fragment));
    }
    return result;
}

void FormattedTextEdit::paintEvent(QPaintEvent *e) {
    // Paint block decorations (quotes, code blocks) BEFORE text.
    const auto *doc = document();
    const auto &presence = blockFormats();
    const bool hasQuotes = presence.quotes;
    const bool hasPre = presence.pre;

    if (hasQuotes || hasPre) {
        QPainter p(viewport());
        p.setClipRegion(e->region());

        const auto scrollY = verticalScrollBar()->value();
        const auto &bqStyle = st::historyBlockquoteStyle;
        const auto &preStyle = st::historyPreStyle;

        // --- Blockquote decorations (blue bar + tinted background) ---
        if (hasQuotes) {
            Ui::Text::SetQuoteCacheColors(_blockquoteCache, st::activeLineFg);
            Ui::Text::ValidateQuotePaintCache(_blockquoteCache, bqStyle);

            auto block = doc->begin();
            while (block.isValid()) {
                if (block.blockFormat().property(QTextFormat::BlockQuoteLevel).toInt() <= 0) {
                    block = block.next();
                    continue;
                }
                auto firstBlock = block;
                auto lastBlock = block;
                while (block.next().isValid()
                       && block.next().blockFormat().property(QTextFormat::BlockQuoteLevel).toInt() > 0) {
                    block = block.next();
                    lastBlock = block;
                }
                const auto firstRect = doc->documentLayout()->blockBoundingRect(firstBlock);
                const auto lastRect = doc->documentLayout()->blockBoundingRect(lastBlock);
                const int left = 0;
                const int top = qRound(firstRect.top() - scrollY - bqStyle.verticalSkip);
                const int width = viewport()->width();
                const int bottom = qRound(lastRect.top() + lastRect.height() - scrollY + bqStyle.verticalSkip);
                const QRect decorRect(left, top, width, bottom - top);

                Ui::Text::FillQuotePaint(p, decorRect, _blockquoteCache, bqStyle);

                block = block.next();
            }
        }

        // --- Code block decorations (green bar + header + tinted background) ---
        if (hasPre) {
            // Compose-field pre color comes from the mono text palette.
            const QColor preBaseColor = st::msgInMonoFg;
            Ui::Text::SetQuoteCacheColors(_preCache, preBaseColor);
            Ui::Text::ValidateQuotePaintCache(_preCache, preStyle);

            auto block = doc->begin();
            while (block.isValid()) {
                if (!block.blockFormat().nonBreakableLines()) {
                    block = block.next();
                    continue;
                }
                auto firstBlock = block;
                auto lastBlock = block;
                while (block.next().isValid()
                       && block.next().blockFormat().nonBreakableLines()) {
                    block = block.next();
                    lastBlock = block;
                }
                // Use blockBoundingRect for reliable positioning.
                const auto firstRect = doc->documentLayout()->blockBoundingRect(firstBlock);
                const auto lastRect = doc->documentLayout()->blockBoundingRect(lastBlock);
                const int left = 0;
                const int top = qRound(firstRect.top() - scrollY - preStyle.header - preStyle.verticalSkip);
                const int width = viewport()->width();
                const int bottom = qRound(lastRect.top() + lastRect.height() - scrollY + preStyle.verticalSkip);
                const QRect decorRect(left, top, width, bottom - top);

                Ui::Text::FillQuotePaint(p, decorRect, _preCache, preStyle);
                block = block.next();
            }
        }
    }

    // Draw text on top.
    QTextEdit::paintEvent(e);
}

void FormattedTextEdit::insertFromMimeData(const QMimeData *source) {
    // Strip all formatting on paste.
    // Insert plain text only, regardless of HTML/RTF on the clipboard.
    if (source->hasText()) {
        insertPlainText(source->text());
    }
}

} // namespace Ui

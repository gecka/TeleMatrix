// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history/compose_html.h"

#include <QTextBlock>
#include <QTextDocument>

namespace TeleMatrix {

QString buildCleanHtml(const QTextDocument *doc) {
    QString html;
    bool inPre = false;          // an open <pre><code> we may extend (coalescing)
    bool inQuote = false;        // an open <blockquote> we may extend (coalescing)
    bool prevWasInline = false;  // previous emitted block was a plain paragraph
    bool firstBlock = true;

    for (auto block = doc->begin(); block != doc->end(); block = block.next()) {
        const auto bfmt = block.blockFormat();
        bool isPre = bfmt.nonBreakableLines();
        const bool isQuote = bfmt.property(QTextFormat::BlockQuoteLevel).toInt() > 0;

        // If every fragment in this block is monospace, treat the whole
        // block as <pre><code> (code block with decoration) rather than
        // wrapping each fragment in inline <code>.
        if (!isPre && !isQuote && block.length() > 1) {
            bool allMono = true;
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const auto frag = it.fragment();
                if (frag.isValid() && !frag.charFormat().fontFixedPitch()) {
                    allMono = false;
                    break;
                }
            }
            if (allMono) isPre = true;
        }

        const bool isInline = !isPre && !isQuote;

        // Close an open block-level group when this block leaves it, so that
        // consecutive code/quote blocks each coalesce into ONE element. Otherwise
        // a multi-line code block or quote is emitted as several <pre>/<blockquote>
        // elements, which some clients render as separate one-line boxes.
        if (inPre && !isPre) {
            html += QStringLiteral("</code></pre>");
            inPre = false;
        }
        if (inQuote && !isQuote) {
            html += QStringLiteral("</blockquote>");
            inQuote = false;
        }

        if (isPre) {
            if (inPre) {
                html += QStringLiteral("\n"); // continue the same code block
            } else {
                html += QStringLiteral("<pre><code>");
                inPre = true;
            }
        } else if (isQuote) {
            if (inQuote) {
                html += QStringLiteral("<br>"); // continue the same quote
            } else {
                html += QStringLiteral("<blockquote>");
                inQuote = true;
            }
        } else {
            // <br> only between two consecutive inline paragraphs; block-level
            // elements (<blockquote>, and a just-closed <pre>) are self-separating
            // in HTML — adding <br> there creates spurious empty lines.
            if (!firstBlock && isInline && prevWasInline) {
                html += QStringLiteral("<br>");
            }
        }
        firstBlock = false;

        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto frag = it.fragment();
            if (!frag.isValid()) continue;

            const auto fmt = frag.charFormat();
            // Qt stores soft line breaks as U+2028; convert them so newlines
            // survive a serialize -> setHtml -> serialize round-trip (e.g. message
            // edit). Inside a code block they become literal '\n', elsewhere <br>.
            auto text = frag.text().toHtmlEscaped();
            text.replace(QChar::LineSeparator,
                isPre ? QStringLiteral("\n") : QStringLiteral("<br>"));

            // Wrap text in formatting tags as needed.
            QString wrapped = text;

            const auto mentionUserId = fmt.property(kMentionUserIdProperty).toString();
            if (!mentionUserId.isEmpty()) {
                wrapped = QStringLiteral("<a href=\"https://matrix.to/#/")
                    + mentionUserId.toHtmlEscaped()
                    + QStringLiteral("\">") + wrapped
                    + QStringLiteral("</a>");
            } else if (fmt.isAnchor() && !fmt.anchorHref().isEmpty()) {
                wrapped = QStringLiteral("<a href=\"")
                    + fmt.anchorHref().toHtmlEscaped()
                    + QStringLiteral("\">") + wrapped
                    + QStringLiteral("</a>");
            }
            // Inline <code> only when part of the block is mono (not all).
            if (fmt.fontFixedPitch() && !isPre) {
                wrapped = QStringLiteral("<code>") + wrapped
                    + QStringLiteral("</code>");
            }
            if (fmt.fontStrikeOut()) {
                wrapped = QStringLiteral("<del>") + wrapped
                    + QStringLiteral("</del>");
            }
            if (fmt.fontUnderline() && !fmt.isAnchor()) {
                wrapped = QStringLiteral("<u>") + wrapped
                    + QStringLiteral("</u>");
            }
            if (fmt.fontItalic()) {
                wrapped = QStringLiteral("<i>") + wrapped
                    + QStringLiteral("</i>");
            }
            if (fmt.fontWeight() >= QFont::Bold) {
                wrapped = QStringLiteral("<b>") + wrapped
                    + QStringLiteral("</b>");
            }
            html += wrapped;
        }

        prevWasInline = isInline;
    }
    // Close any block-level group still open at the end of the document.
    if (inPre) {
        html += QStringLiteral("</code></pre>");
    }
    if (inQuote) {
        html += QStringLiteral("</blockquote>");
    }
    return html;
}

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest/QtTest>
#include <QFont>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include "history/compose_html.h"

using namespace TeleMatrix;

namespace {

void quoteEveryBlock(QTextDocument &doc) {
    for (auto b = doc.firstBlock(); b.isValid(); b = b.next()) {
        QTextCursor c(b);
        auto bf = c.blockFormat();
        bf.setProperty(QTextFormat::BlockQuoteLevel, 1);
        c.setBlockFormat(bf);
    }
}

void preEveryBlock(QTextDocument &doc) {
    for (auto b = doc.firstBlock(); b.isValid(); b = b.next()) {
        QTextCursor c(b);
        auto bf = c.blockFormat();
        bf.setNonBreakableLines(true);
        c.setBlockFormat(bf);
    }
}

} // namespace

class TestComposeHtml : public QObject {
    Q_OBJECT
private slots:
    void plainParagraphsJoinWithBr() {
        QTextDocument doc;
        doc.setPlainText(QStringLiteral("hello\nworld"));
        QCOMPARE(buildCleanHtml(&doc), QStringLiteral("hello<br>world"));
    }
    void singleQuoteBlock() {
        QTextDocument doc;
        doc.setPlainText(QStringLiteral("quoted"));
        quoteEveryBlock(doc);
        QCOMPARE(buildCleanHtml(&doc),
            QStringLiteral("<blockquote>quoted</blockquote>"));
    }
    // The bug: a UI-applied multi-line quote must serialize to ONE
    // <blockquote>, not one per line (clients render each as a separate box).
    void multilineQuoteCoalesces() {
        QTextDocument doc;
        doc.setPlainText(QStringLiteral("a\nb\nc"));
        quoteEveryBlock(doc);
        QCOMPARE(buildCleanHtml(&doc),
            QStringLiteral("<blockquote>a<br>b<br>c</blockquote>"));
    }
    // Regression: consecutive pre blocks already coalesce; keep them working.
    void multilinePreCoalesces() {
        QTextDocument doc;
        doc.setPlainText(QStringLiteral("x\ny"));
        preEveryBlock(doc);
        QCOMPARE(buildCleanHtml(&doc),
            QStringLiteral("<pre><code>x\ny</code></pre>"));
    }
    // Two quotes split by a normal paragraph must stay two blockquotes.
    void quoteParagraphQuoteStayDistinct() {
        QTextDocument doc;
        doc.setPlainText(QStringLiteral("q1\nmid\nq2"));
        auto quoteBlock = [&](const QTextBlock &b) {
            QTextCursor c(b);
            auto bf = c.blockFormat();
            bf.setProperty(QTextFormat::BlockQuoteLevel, 1);
            c.setBlockFormat(bf);
        };
        quoteBlock(doc.firstBlock());
        quoteBlock(doc.firstBlock().next().next());
        QCOMPARE(buildCleanHtml(&doc),
            QStringLiteral("<blockquote>q1</blockquote>mid<blockquote>q2</blockquote>"));
    }
    void boldRunWrapsInB() {
        QTextDocument doc;
        QTextCursor c(&doc);
        QTextCharFormat bold;
        bold.setFontWeight(QFont::Bold);
        c.insertText(QStringLiteral("hi"), bold);
        QCOMPARE(buildCleanHtml(&doc), QStringLiteral("<b>hi</b>"));
    }
};

QTEST_MAIN(TestComposeHtml)
#include "tst_compose_html.moc"

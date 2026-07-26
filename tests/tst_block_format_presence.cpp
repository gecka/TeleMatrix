// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest/QtTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextBlockFormat>

#include "ui/widgets/block_format_presence.h"

class TestBlockFormatPresence : public QObject {
    Q_OBJECT
private slots:
    void plainDocHasNone() {
        QTextDocument doc;
        doc.setPlainText(QStringLiteral("one\ntwo\nthree"));
        const auto p = Ui::ScanBlockFormats(&doc);
        QVERIFY(!p.quotes); QVERIFY(!p.pre); QVERIFY(!p.any());
    }
    void detectsQuoteAndPre() {
        QTextDocument doc;
        doc.setPlainText(QStringLiteral("quoted\ncode"));
        auto cursor = QTextCursor(doc.firstBlock());
        auto quoteFmt = cursor.blockFormat();
        quoteFmt.setProperty(QTextFormat::BlockQuoteLevel, 1);
        cursor.setBlockFormat(quoteFmt);
        auto cursor2 = QTextCursor(doc.firstBlock().next());
        auto preFmt = cursor2.blockFormat();
        preFmt.setNonBreakableLines(true);
        cursor2.setBlockFormat(preFmt);
        const auto p = Ui::ScanBlockFormats(&doc);
        QVERIFY(p.quotes); QVERIFY(p.pre);
    }
    // Pins the Qt behavior the memo relies on: format-only edits bump revision().
    void formatOnlyEditBumpsRevision() {
        QTextDocument doc;
        doc.setPlainText(QStringLiteral("text"));
        const auto before = doc.revision();
        auto cursor = QTextCursor(doc.firstBlock());
        auto fmt = cursor.blockFormat();
        fmt.setProperty(QTextFormat::BlockQuoteLevel, 1);
        cursor.setBlockFormat(fmt);
        QVERIFY(doc.revision() != before);
    }
    void memoTracksChangesAndUndo() {
        QTextDocument doc;
        doc.setUndoRedoEnabled(true);
        doc.setPlainText(QStringLiteral("text"));
        Ui::BlockFormatsMemo memo;
        QVERIFY(!memo.get(&doc).any());
        auto cursor = QTextCursor(doc.firstBlock());
        auto fmt = cursor.blockFormat();
        fmt.setProperty(QTextFormat::BlockQuoteLevel, 1);
        cursor.setBlockFormat(fmt);
        QVERIFY(memo.get(&doc).quotes);
        doc.undo();
        QVERIFY(!memo.get(&doc).any());
    }
};

QTEST_MAIN(TestBlockFormatPresence)
#include "tst_block_format_presence.moc"

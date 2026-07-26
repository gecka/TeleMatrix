// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "history/message_index.h"

using namespace TeleMatrix;

namespace {

QVector<QString> ids(std::initializer_list<const char *> list) {
    QVector<QString> out;
    for (const auto *s : list) {
        out.append(QString::fromUtf8(s));
    }
    return out;
}

} // namespace

class TestMessageIndex : public QObject {
    Q_OBJECT

private slots:
    void emptyReturnsMinusOne() {
        MessageIndex index;
        QCOMPARE(index.physicalIndexOf("$a"), -1);
        QVERIFY(!index.contains("$a"));
    }

    void rebuildMapsPositions() {
        MessageIndex index;
        index.rebuild(ids({"$a", "$b", "$c"}));
        QCOMPARE(index.physicalIndexOf("$a"), 0);
        QCOMPARE(index.physicalIndexOf("$b"), 1);
        QCOMPARE(index.physicalIndexOf("$c"), 2);
        QCOMPARE(index.physicalIndexOf("$missing"), -1);
        QVERIFY(index.contains("$b"));
    }

    void rebuildSkipsEmptyIdsButKeepsPositions() {
        MessageIndex index;
        index.rebuild(ids({"$a", "", "$c"}));
        QCOMPARE(index.physicalIndexOf("$a"), 0);
        QCOMPARE(index.physicalIndexOf("$c"), 2); // empty slot at 1 still counted
        QVERIFY(!index.contains(""));
    }

    void prependShiftsExistingWithoutRebuild() {
        MessageIndex index;
        index.rebuild(ids({"$c", "$d"})); // [c=0, d=1]
        index.prependFront(ids({"$a", "$b"})); // -> [a, b, c, d]
        QCOMPARE(index.physicalIndexOf("$a"), 0);
        QCOMPARE(index.physicalIndexOf("$b"), 1);
        QCOMPARE(index.physicalIndexOf("$c"), 2);
        QCOMPARE(index.physicalIndexOf("$d"), 3);
    }

    void repeatedPrependsAccumulate() {
        MessageIndex index;
        index.rebuild(ids({"$e"})); // [e=0]
        index.prependFront(ids({"$c", "$d"})); // [c, d, e]
        index.prependFront(ids({"$a", "$b"})); // [a, b, c, d, e]
        QCOMPARE(index.physicalIndexOf("$a"), 0);
        QCOMPARE(index.physicalIndexOf("$b"), 1);
        QCOMPARE(index.physicalIndexOf("$c"), 2);
        QCOMPARE(index.physicalIndexOf("$d"), 3);
        QCOMPARE(index.physicalIndexOf("$e"), 4);
    }

    void prependSkipsEmptyIdsButShiftsByFullCount() {
        MessageIndex index;
        index.rebuild(ids({"$c"})); // [c=0]
        index.prependFront(ids({"$a", ""})); // -> [a, <empty>, c]
        QCOMPARE(index.physicalIndexOf("$a"), 0);
        QCOMPARE(index.physicalIndexOf("$c"), 2);
        QVERIFY(!index.contains(""));
    }

    void setAtRecordsBackAppend() {
        MessageIndex index;
        index.rebuild(ids({"$a", "$b"})); // [a, b]
        index.setAt("$c", 2); // appended at the back
        QCOMPARE(index.physicalIndexOf("$c"), 2);
        QCOMPARE(index.physicalIndexOf("$a"), 0);
    }

    void setAtWorksAfterPrepend() {
        MessageIndex index;
        index.rebuild(ids({"$b"})); // [b=0]
        index.prependFront(ids({"$a"})); // [a, b] (base now negative)
        index.setAt("$c", 2); // append [a, b, c]
        QCOMPARE(index.physicalIndexOf("$a"), 0);
        QCOMPARE(index.physicalIndexOf("$b"), 1);
        QCOMPARE(index.physicalIndexOf("$c"), 2);
    }

    void removeDropsKey() {
        MessageIndex index;
        index.rebuild(ids({"$a", "$b", "$c"}));
        index.remove("$b");
        QVERIFY(!index.contains("$b"));
        QCOMPARE(index.physicalIndexOf("$b"), -1);
        QCOMPARE(index.physicalIndexOf("$a"), 0); // others untouched
    }

    void idReplacementViaRemoveThenSetAt() {
        // updateMessageSendState: local-echo id -> server id at the same slot.
        MessageIndex index;
        index.rebuild(ids({"$a", "$local", "$c"}));
        index.remove("$local");
        index.setAt("$server", 1);
        QCOMPARE(index.physicalIndexOf("$server"), 1);
        QCOMPARE(index.physicalIndexOf("$local"), -1);
        QCOMPARE(index.physicalIndexOf("$a"), 0);
        QCOMPARE(index.physicalIndexOf("$c"), 2);
    }

    void clearResets() {
        MessageIndex index;
        index.rebuild(ids({"$a", "$b"}));
        index.prependFront(ids({"$z"}));
        index.clear();
        QCOMPARE(index.physicalIndexOf("$a"), -1);
        QVERIFY(!index.contains("$z"));
        // Reusable after clear: base must be reset to 0.
        index.rebuild(ids({"$x", "$y"}));
        QCOMPARE(index.physicalIndexOf("$x"), 0);
        QCOMPARE(index.physicalIndexOf("$y"), 1);
    }
};

QTEST_GUILESS_MAIN(TestMessageIndex)
#include "tst_message_index.moc"

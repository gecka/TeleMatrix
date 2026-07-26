// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest/QtTest>

#include "history/mention_trigger.h"

using namespace TeleMatrix;

class TestMentionTrigger : public QObject {
    Q_OBJECT
private slots:
    void triggersAtStart() {
        const auto r = MentionTrigger::Scan(QStringLiteral("@al"), 3);
        QCOMPARE(r.atPos, 0);
        QCOMPARE(r.query, QStringLiteral("al"));
    }
    void triggersMidText() {
        const auto r = MentionTrigger::Scan(QStringLiteral("hello @al"), 9);
        QCOMPARE(r.atPos, 6);
        QCOMPARE(r.query, QStringLiteral("al"));
    }
    void triggersAfterPunctuation() {
        const auto r = MentionTrigger::Scan(QStringLiteral("(@name"), 6);
        QCOMPARE(r.atPos, 1);
        QCOMPARE(r.query, QStringLiteral("name"));
    }
    void rejectsEmailTail() {
        QCOMPARE(MentionTrigger::Scan(QStringLiteral("mail@host"), 9).atPos, -1);
    }
    void rejectsNonLetterQueryStart() {
        QCOMPARE(MentionTrigger::Scan(QStringLiteral("@1abc"), 5).atPos, -1);
    }
    void emptyQueryAtBareAt() {
        const auto r = MentionTrigger::Scan(QStringLiteral("@"), 1);
        QCOMPARE(r.atPos, 0);
        QVERIFY(r.query.isEmpty());
    }
    void stopsAtSpace() {
        QCOMPARE(MentionTrigger::Scan(QStringLiteral("@name x"), 7).atPos, -1);
    }
    void noAtNoTrigger() {
        QCOMPARE(MentionTrigger::Scan(QStringLiteral("hello world"), 11).atPos, -1);
    }
};

QTEST_GUILESS_MAIN(TestMentionTrigger)
#include "tst_mention_trigger.moc"

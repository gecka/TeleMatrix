// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "ui/recovery_key_format.h"

using namespace TeleMatrix;

class TestRecoveryKeyFormat : public QObject {
    Q_OBJECT

private slots:
    void groupsByFour_data() {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("formatted");

        QTest::newRow("empty") << "" << "";
        QTest::newRow("partial-group") << "EsT" << "EsT";
        QTest::newRow("exact-group") << "EsTk" << "EsTk";
        // The separator appears together with the character that opens the next
        // group — never as a trailing space the user then has to delete.
        QTest::newRow("group-plus-one") << "EsTkq" << "EsTk q";
        QTest::newRow("two-groups") << "EsTkqfWe" << "EsTk qfWe";
        // A pasted key already in display form must survive unchanged.
        QTest::newRow("already-grouped") << "EsTk qfWe N7mf" << "EsTk qfWe N7mf";
        // Pasted with the wrong grouping (or none) gets regrouped.
        QTest::newRow("regrouped") << "EsTkq fWeN7mf" << "EsTk qfWe N7mf";
        QTest::newRow("strips-tabs") << "EsTk\tqfWe" << "EsTk qfWe";
    }

    void groupsByFour() {
        QFETCH(QString, input);
        QFETCH(QString, formatted);

        QCOMPARE(FormatRecoveryKey(input, 0).text, formatted);
    }

    void keepsTheCaretWithTheSameCharacter_data() {
        QTest::addColumn<QString>("input");
        QTest::addColumn<int>("cursor");
        QTest::addColumn<int>("expected");

        // Typing the 5th character: the caret must land after it, past the space
        // the formatter just inserted ("EsTkq" -> "EsTk q").
        QTest::newRow("typing-fifth") << "EsTkq" << 5 << 6;
        QTest::newRow("start") << "EsTkq" << 0 << 0;
        // End of a complete group: no separator has been added yet.
        QTest::newRow("end-of-group") << "EsTk" << 4 << 4;
        // Caret mid-key, with an earlier separator already in the text.
        QTest::newRow("after-two-groups") << "EsTk qfWe" << 9 << 9;
        // Editing in the middle: caret sits after the 6th key character, which in
        // display form is preceded by one separator.
        QTest::newRow("mid-key") << "EsTkqfWe" << 6 << 7;
    }

    void keepsTheCaretWithTheSameCharacter() {
        QFETCH(QString, input);
        QFETCH(int, cursor);
        QFETCH(int, expected);

        QCOMPARE(FormatRecoveryKey(input, cursor).cursor, expected);
    }

    void formattingIsIdempotent() {
        const auto key = QStringLiteral(
            "EsTkqfWeN7mf32WEmEVnbKNE5YPaWBFqX1ZA5eLnYLb2VwqV");
        const auto once = FormatRecoveryKey(key, key.size());
        const auto twice = FormatRecoveryKey(once.text, once.cursor);

        QCOMPARE(once.text, QStringLiteral(
            "EsTk qfWe N7mf 32WE mEVn bKNE 5YPa WBFq X1ZA 5eLn YLb2 VwqV"));
        QCOMPARE(twice.text, once.text);
        QCOMPARE(twice.cursor, once.cursor);
        // 48 key characters + 11 separators — exactly the field's maxLength.
        QCOMPARE(once.text.size(), 59);
    }
};

QTEST_MAIN(TestRecoveryKeyFormat)
#include "tst_recovery_key_format.moc"

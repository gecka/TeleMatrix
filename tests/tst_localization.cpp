// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "core/localization.h"

using namespace TeleMatrix::Core;

class TestLocalization : public QObject {
    Q_OBJECT

private slots:
    void normalizeLanguageId_data() {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");

        QTest::newRow("plain") << "en" << "en";
        QTest::newRow("dash-region") << "en-US" << "en";
        QTest::newRow("underscore-region") << "en_US" << "en";
        QTest::newRow("uppercase") << "EN" << "en";
        QTest::newRow("spanish") << "es-ES" << "es";
        // Unsupported languages normalize to empty (not their subtag).
        QTest::newRow("unsupported-fr") << "fr" << "";
        QTest::newRow("unsupported-de-region") << "de-DE" << "";
        QTest::newRow("empty") << "" << "";
    }

    void normalizeLanguageId() {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        QCOMPARE(TeleMatrix::Core::normalizeLanguageId(input), expected);
    }

    void isSupportedLanguage() {
        QVERIFY(TeleMatrix::Core::isSupportedLanguage("en"));
        QVERIFY(TeleMatrix::Core::isSupportedLanguage("es_ES"));
        QVERIFY(!TeleMatrix::Core::isSupportedLanguage("fr"));
        QVERIFY(!TeleMatrix::Core::isSupportedLanguage(""));
    }

    void resolveSupportedIdPassesThrough() {
        QCOMPARE(TeleMatrix::Core::resolveLanguageId("es"), QString("es"));
        QCOMPARE(TeleMatrix::Core::resolveLanguageId("en-GB"), QString("en"));
    }

    void resolveUnsupportedFallsBackToSupported() {
        // The fallback consults the host locale (and ultimately "en"); we don't
        // pin the exact value, only that it is always a supported language.
        const auto resolved = TeleMatrix::Core::resolveLanguageId("zz-ZZ");
        QVERIFY(TeleMatrix::Core::isSupportedLanguage(resolved));
    }

    void exposesKnownLanguages() {
        const auto langs = TeleMatrix::Core::supportedLanguages();
        QCOMPARE(langs.size(), 2);
        QCOMPARE(TeleMatrix::Core::languageName("en"), QString("English"));
        QCOMPARE(TeleMatrix::Core::languageNativeName("es"), QString::fromUtf8("Español"));
    }
};

QTEST_GUILESS_MAIN(TestLocalization)
#include "tst_localization.moc"

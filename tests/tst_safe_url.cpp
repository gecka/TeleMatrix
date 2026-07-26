// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "ui/safe_url.h"

using namespace TeleMatrix;

class TestSafeUrl : public QObject {
    Q_OBJECT

private slots:
    void allowsWebSchemes_data() {
        QTest::addColumn<QString>("url");
        QTest::addColumn<bool>("safe");

        // Allowed schemes.
        QTest::newRow("https") << "https://example.com/path?q=1" << true;
        QTest::newRow("http") << "http://example.com" << true;
        QTest::newRow("ftp") << "ftp://files.example.com/f.txt" << true;
        // fromUserInput fallback promotes a bare host to http://.
        QTest::newRow("bare-host") << "example.com" << true;

        // Dangerous / non-web schemes must be rejected — this is the security
        // contract: a message link must never be able to run script or open
        // a local file.
        QTest::newRow("javascript") << "javascript:alert(1)" << false;
        QTest::newRow("file") << "file:///etc/passwd" << false;
        QTest::newRow("data") << "data:text/html,<script>" << false;
        QTest::newRow("mailto") << "mailto:a@b.com" << false;

        // Malformed / empty.
        QTest::newRow("empty") << "" << false;
        QTest::newRow("whitespace") << "   " << false;
    }

    void allowsWebSchemes() {
        QFETCH(QString, url);
        QFETCH(bool, safe);
        QCOMPARE(IsSafeExternalUrl(url), safe);
    }

    void trimsSurroundingWhitespace() {
        QVERIFY(IsSafeExternalUrl("  https://example.com  "));
    }
};

QTEST_GUILESS_MAIN(TestSafeUrl)
#include "tst_safe_url.moc"

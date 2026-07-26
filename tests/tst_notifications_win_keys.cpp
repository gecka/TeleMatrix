// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "window/notifications_win_keys.h"

using namespace TeleMatrix::Notifications;

class TestNotificationsWinKeys : public QObject {
    Q_OBJECT

private slots:
    void deterministic() {
        QCOMPARE(toastGroupKey(QStringLiteral("!abc:example.org")),
                 toastGroupKey(QStringLiteral("!abc:example.org")));
        QCOMPARE(toastTagKey(QStringLiteral("$ev1:example.org")),
                 toastTagKey(QStringLiteral("$ev1:example.org")));
    }

    void alwaysSixteenLowercaseHex() {
        const QStringList ids = {
            QStringLiteral("!a:b"),
            QStringLiteral("$AAAA+bbbb/cccc=dddd:matrix.org"), // base64-ish event id
            QString(),                                          // empty
            QStringLiteral("!verylongroomid000000000000000000000000000000:server.example.com"),
        };
        for (const QString &id : ids) {
            const QString key = toastGroupKey(id);
            QCOMPARE(key.size(), 16);
            for (const QChar c : key) {
                QVERIFY2(c.isDigit() || (c >= QLatin1Char('a') && c <= QLatin1Char('f')),
                         qPrintable(QStringLiteral("non-hex char in %1").arg(key)));
            }
        }
    }

    void distinctIdsGiveDistinctKeys() {
        QVERIFY(toastGroupKey(QStringLiteral("!a:x"))
                != toastGroupKey(QStringLiteral("!b:x")));
        QVERIFY(toastTagKey(QStringLiteral("$1:x"))
                != toastTagKey(QStringLiteral("$2:x")));
    }

    void knownVectorPinsTheAlgorithm() {
        // FNV-1a/64 of the empty input is the offset basis 0xcbf29ce484222325.
        // Pinning it guards against an accidental algorithm/format change that
        // would orphan already-shown toasts (clear would compute a new key).
        QCOMPARE(toastGroupKey(QString()), QStringLiteral("cbf29ce484222325"));
        QCOMPARE(toastTagKey(QString()), QStringLiteral("cbf29ce484222325"));
    }
};

QTEST_MAIN(TestNotificationsWinKeys)
#include "tst_notifications_win_keys.moc"

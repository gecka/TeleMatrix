// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "ui/empty_userpic.h"

// The placeholder avatar's initial-letter extraction. The load-bearing case is
// a raw MXID (no display name) like "@irc_...:server": the leading "@" must not
// swallow the first letter, or the timeline shows a blank circle while the
// profile popup (which uses the localpart) shows a lettered one.
class TestEmptyUserpic : public QObject {
    Q_OBJECT

private slots:
    void rawMxidYieldsFirstLetterAfterAt() {
        QCOMPARE(
            Ui::EmptyUserpic::extractInitials(
                QStringLiteral("@irc_irc.libera.chat_DarkTrick:mailstation.de")),
            QStringLiteral("I"));
        QCOMPARE(
            Ui::EmptyUserpic::extractInitials(QStringLiteral("@alice:server")),
            QStringLiteral("A"));
    }

    void plainNames() {
        QCOMPARE(Ui::EmptyUserpic::extractInitials(QStringLiteral("Alice")),
            QStringLiteral("A"));
        QCOMPARE(Ui::EmptyUserpic::extractInitials(QStringLiteral("Alice Bob")),
            QStringLiteral("AB"));
        QCOMPARE(Ui::EmptyUserpic::extractInitials(QStringLiteral("Jean-Pierre")),
            QStringLiteral("JP"));
    }

    void punctuationInWordIsNotASeparator() {
        // Dots/underscores inside a single token do not start a new word.
        QCOMPARE(Ui::EmptyUserpic::extractInitials(QStringLiteral("irc_irc.libera")),
            QStringLiteral("I"));
    }

    void degenerateInputs() {
        QCOMPARE(Ui::EmptyUserpic::extractInitials(QString()), QString());
        QCOMPARE(Ui::EmptyUserpic::extractInitials(QStringLiteral("@")), QString());
        QCOMPARE(Ui::EmptyUserpic::extractInitials(QStringLiteral("...")), QString());
    }

    void savedMessagesPaintsDiscAndBookmark() {
        QImage image(64, 64, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        {
            QPainter p(&image);
            Ui::EmptyUserpic::paintSavedMessages(p, 0, 0, 64);
        }
        // Disc pixel (near left edge, mid-height) takes the accent color.
        QVERIFY(image.pixelColor(6, 32).alpha() > 200);
        // tdesktop-style vertical gradient: top of the disc is lighter than
        // the bottom.
        QVERIFY(image.pixelColor(32, 4).lightness()
            > image.pixelColor(32, 60).lightness());
        // At size 64 the tdesktop geometry puts the bookmark's top bar at
        // y=20 (stroke 4px), centred horizontally.
        QCOMPARE(image.pixelColor(32, 20).rgb(),
            QColor(st::historyPeerUserpicFg).rgb());
        // The bookmark is an outline: its inside stays disc-coloured (the
        // gradient interior), never the glyph color.
        QVERIFY(image.pixelColor(32, 30).rgb()
            != QColor(st::historyPeerUserpicFg).rgb());
    }
};

// Not GUILESS: the paint test needs a QGuiApplication for QPainter on QImage.
QTEST_MAIN(TestEmptyUserpic)
#include "tst_empty_userpic.moc"

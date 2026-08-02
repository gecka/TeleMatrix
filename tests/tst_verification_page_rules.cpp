// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "protocol/verification_page_rules.h"

using namespace TeleMatrix;
using R = VerificationPageRules;

class TestVerificationPageRules : public QObject {
    Q_OBJECT

private slots:
    // Element's split: exactly these three codes are security failures.
    void securityCodes() {
        QVERIFY(R::isSecurityCancelCode(QStringLiteral("m.key_mismatch")));
        QVERIFY(R::isSecurityCancelCode(QStringLiteral("m.user_error")));
        QVERIFY(R::isSecurityCancelCode(QStringLiteral("m.mismatched_sas")));
        QVERIFY(!R::isSecurityCancelCode(QStringLiteral("m.user")));
        QVERIFY(!R::isSecurityCancelCode(QStringLiteral("m.timeout")));
        QVERIFY(!R::isSecurityCancelCode(QString()));
    }

    // The QR page never latches from RequestingVerification: Rust emits it
    // before tagging the new flow, so on a 2nd+ flow it carries the previous
    // flow's id. Latching it made the abandoned flow's Cancelled "match" and
    // flash a failure over the new flow (element-alignment, Task 3 finding).
    void staleRequestingNeverLatches() {
        QVERIFY(!R::qrLatchesFromState(R::kRequesting, QStringLiteral("$a")));
        QVERIFY(R::qrLatchesFromState(R::kWaitingForReady, QStringLiteral("$a")));
        QVERIFY(R::qrLatchesFromState(R::kReady, QStringLiteral("$a")));
        QVERIFY(R::qrLatchesFromState(R::kQrCodeReady, QStringLiteral("$a")));
        QVERIFY(R::qrLatchesFromState(R::kQrCodeScanned, QStringLiteral("$a")));
        QVERIFY(!R::qrLatchesFromState(R::kSasStarted, QStringLiteral("$a")));
        QVERIFY(!R::qrLatchesFromState(R::kReady, QString()));
    }

    // An unlatched page never owns a Done — firing verified() on any untagged
    // Done was how a page could claim success for a flow it never ran.
    void doneRequiresALatchedOwnedFlow() {
        QVERIFY(!R::acceptDone(QString(), QString()));
        QVERIFY(!R::acceptDone(QStringLiteral("$a"), QString()));
        QVERIFY(R::acceptDone(QString(), QStringLiteral("$a")));
        QVERIFY(R::acceptDone(QStringLiteral("$a"), QStringLiteral("$a")));
        QVERIFY(!R::acceptDone(QStringLiteral("$b"), QStringLiteral("$a")));
    }

    // THE STRAND GUARD. The emoji page has no early latch (until the next
    // task lands one): a peer declining before emojis arrives with the page
    // unlatched, and with no timeouts anywhere this Cancelled is the only
    // terminal state the page will ever get. It MUST be shown. Two separate
    // reviews rejected "fixes" that swallowed it.
    void peerDeclineBeforeEmojisStillShowsFailure() {
        QVERIFY(R::showCancelledOnEmojiPage(
            QStringLiteral("$a"), QString(), QString()));
        QVERIFY(R::showCancelledOnEmojiPage(QString(), QString(), QString()));
    }

    // Once latched, a foreign flow's Cancelled is not ours to display; the
    // deliberately-abandoned flow's Cancelled is swallowed even unlatched.
    void emojiCancelledScoping() {
        QVERIFY(!R::showCancelledOnEmojiPage(
            QStringLiteral("$b"), QStringLiteral("$a"), QString()));
        QVERIFY(!R::showCancelledOnEmojiPage(
            QStringLiteral("$ignored"), QString(), QStringLiteral("$ignored")));
        QVERIFY(R::showCancelledOnEmojiPage(
            QStringLiteral("$a"), QStringLiteral("$a"), QString()));
        QVERIFY(R::showCancelledOnEmojiPage(
            QString(), QStringLiteral("$a"), QString()));
    }

    // The QR page is the mirror image: it latches early from states, so an
    // unlatched QR page genuinely does not own anything yet.
    void qrCancelledRequiresLatch() {
        QVERIFY(!R::showCancelledOnQrPage(QStringLiteral("$a"), QString()));
        QVERIFY(R::showCancelledOnQrPage(
            QStringLiteral("$a"), QStringLiteral("$a")));
        QVERIFY(!R::showCancelledOnQrPage(
            QStringLiteral("$b"), QStringLiteral("$a")));
        QVERIFY(R::showCancelledOnQrPage(QString(), QStringLiteral("$a")));
    }

    // Emojis for the ignored flow prove the cancel lost the race — the flow is
    // alive and serving this page. Ignoring must end so its OWN later
    // Cancelled is reported (element-alignment, Task 3 fix round).
    void ignoredFlowEmojisRevive() {
        QVERIFY(R::emojisReviveIgnoredFlow(
            QStringLiteral("$a"), QStringLiteral("$a")));
        QVERIFY(!R::emojisReviveIgnoredFlow(
            QStringLiteral("$b"), QStringLiteral("$a")));
        QVERIFY(!R::emojisReviveIgnoredFlow(QString(), QStringLiteral("$a")));
        QVERIFY(!R::emojisReviveIgnoredFlow(QStringLiteral("$a"), QString()));
    }

    // A foreign code adopted while unlatched mislabels the next failure's
    // severity — the strict rule requires a real match on both sides
    // (element-alignment, Task 4 fix round). The QR page's loose variant is
    // pre-existing and shielded by its display guard.
    void cancelCodeAdoption() {
        QVERIFY(!R::adoptCancelCodeStrict(QStringLiteral("$a"), QString()));
        QVERIFY(!R::adoptCancelCodeStrict(QString(), QStringLiteral("$a")));
        QVERIFY(!R::adoptCancelCodeStrict(
            QStringLiteral("$b"), QStringLiteral("$a")));
        QVERIFY(R::adoptCancelCodeStrict(
            QStringLiteral("$a"), QStringLiteral("$a")));
        QVERIFY(R::adoptCancelCodeLoose(QStringLiteral("$a"), QString()));
        QVERIFY(R::adoptCancelCodeLoose(QString(), QStringLiteral("$a")));
        QVERIFY(!R::adoptCancelCodeLoose(
            QStringLiteral("$b"), QStringLiteral("$a")));
    }

    // Only a flow whose emojis this dialog actually showed may claim the user
    // is verified (final-review I4): _emojisShown gates everything, then the
    // usual foreign-flow scoping applies.
    void dialogDoneGate() {
        QVERIFY(!R::dialogAcceptsDone(
            QStringLiteral("$a"), QStringLiteral("$a"), false));
        QVERIFY(R::dialogAcceptsDone(
            QStringLiteral("$a"), QStringLiteral("$a"), true));
        QVERIFY(!R::dialogAcceptsDone(
            QStringLiteral("$b"), QStringLiteral("$a"), true));
        QVERIFY(R::dialogAcceptsDone(QString(), QStringLiteral("$a"), true));
    }
};

QTEST_APPLESS_MAIN(TestVerificationPageRules)
#include "tst_verification_page_rules.moc"

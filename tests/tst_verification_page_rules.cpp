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

    // With the primary latch now the start-call's own acknowledgement, the
    // state latch is a fallback only: never overwrites an existing latch, and
    // never adopts WaitingForReady — the incoming handlers emit that state
    // tagged with ANY incoming request's id, so it is foreign-taggable.
    // kRequesting stays excluded (stale id — see Task 1's history note).
    void qrStateLatchIsAFallbackOnly() {
        QVERIFY(!R::qrLatchesFromState(
            R::kWaitingForReady, QStringLiteral("$a"), QString()));
        QVERIFY(!R::qrLatchesFromState(
            R::kRequesting, QStringLiteral("$a"), QString()));
        QVERIFY(R::qrLatchesFromState(
            R::kReady, QStringLiteral("$a"), QString()));
        QVERIFY(R::qrLatchesFromState(
            R::kQrCodeReady, QStringLiteral("$a"), QString()));
        QVERIFY(R::qrLatchesFromState(
            R::kQrCodeScanned, QStringLiteral("$a"), QString()));
        QVERIFY(!R::qrLatchesFromState(
            R::kSasStarted, QStringLiteral("$a"), QString()));
        // Never overwrite an existing latch.
        QVERIFY(!R::qrLatchesFromState(
            R::kReady, QStringLiteral("$b"), QStringLiteral("$a")));
        QVERIFY(!R::qrLatchesFromState(R::kReady, QString(), QString()));
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

    // THE DECEIVING-ACTION GUARD (final review C1). Emojis the emoji page
    // accepts are RENDERED with "They match" armed, so a foreign flow's
    // payload does not merely display a lie — clicking confirms whatever SAS
    // the backend holds. Unlike every other rule here, an unlatched page is
    // therefore not a wildcard. The ignored-flow arm is what keeps that from
    // stranding the "compare emoji instead" revival, which legitimately
    // arrives with _flowId cleared by resetForAttempt().
    void emojiPayloadOwnership_data() {
        QTest::addColumn<QString>("eventFlow");
        QTest::addColumn<QString>("pageFlow");
        QTest::addColumn<QString>("ignoredFlow");
        QTest::addColumn<bool>("accepted");

        QTest::newRow("untagged, latched")
            << QString() << QStringLiteral("$a") << QString() << true;
        QTest::newRow("untagged, unlatched")
            << QString() << QString() << QString() << true;
        // Pins the untagged arm itself: without it, an untagged emission is
        // accepted only while _flowId or _ignoredFlowId is still empty, so a
        // latched page that is also still ignoring a flow would reject its own
        // emojis and hang with no timeout to save it.
        QTest::newRow("untagged, latched, a flow still ignored")
            << QString() << QStringLiteral("$a") << QStringLiteral("$qr") << true;
        QTest::newRow("own flow")
            << QStringLiteral("$a") << QStringLiteral("$a") << QString() << true;
        // ignoreFlow($qr) -> showStep(kStepEmoji) -> resetForAttempt() clears
        // _flowId but not _ignoredFlowId: the QR flow's late emojis land here.
        QTest::newRow("revived ignored flow, unlatched")
            << QStringLiteral("$qr") << QString() << QStringLiteral("$qr") << true;
        // Same revival after our own fresh start already latched.
        QTest::newRow("revived ignored flow, latched elsewhere")
            << QStringLiteral("$qr") << QStringLiteral("$new")
            << QStringLiteral("$qr") << true;
        QTest::newRow("foreign, latched")
            << QStringLiteral("$b") << QStringLiteral("$a") << QString() << false;
        // The case the unconditional adoption made reachable: a cross-user
        // dialog's SAS rendering on an unlatched session-verification page.
        QTest::newRow("foreign, unlatched")
            << QStringLiteral("$b") << QString() << QString() << false;
        QTest::newRow("foreign while an unrelated flow is ignored")
            << QStringLiteral("$b") << QStringLiteral("$a")
            << QStringLiteral("$qr") << false;
    }
    void emojiPayloadOwnership() {
        QFETCH(QString, eventFlow);
        QFETCH(QString, pageFlow);
        QFETCH(QString, ignoredFlow);
        QFETCH(bool, accepted);
        QCOMPARE(
            R::emojisBelongToPage(eventFlow, pageFlow, ignoredFlow), accepted);
    }

    // A foreign code adopted while unlatched mislabels the next failure's
    // severity — the strict rule requires a real match on both sides
    // (element-alignment, Task 4 fix round). Both pages use it now: the QR
    // page's loose variant is gone, since with an early latch on every page
    // there is no window it bought anything in.
    void cancelCodeAdoption() {
        QVERIFY(!R::adoptCancelCodeStrict(QStringLiteral("$a"), QString()));
        QVERIFY(!R::adoptCancelCodeStrict(QString(), QStringLiteral("$a")));
        QVERIFY(!R::adoptCancelCodeStrict(
            QStringLiteral("$b"), QStringLiteral("$a")));
        // The normal path: a flow adopting its own code, which drives the
        // security-vs-routine split on its own later Cancelled.
        QVERIFY(R::adoptCancelCodeStrict(
            QStringLiteral("$a"), QStringLiteral("$a")));
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
        // An unlatched dialog (no flow id yet — the outgoing path has none up
        // front) still accepts Done once its emojis were shown: this is the
        // dialog's own anti-strand path through the terminal-state guard.
        QVERIFY(R::dialogAcceptsDone(QStringLiteral("$a"), QString(), true));
    }
};

QTEST_APPLESS_MAIN(TestVerificationPageRules)
#include "tst_verification_page_rules.moc"

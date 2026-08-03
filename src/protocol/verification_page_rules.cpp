// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "protocol/verification_page_rules.h"

namespace TeleMatrix {

bool VerificationPageRules::isSecurityCancelCode(const QString &code) {
    // intro_verify_emoji.cpp / intro_verify_qr.cpp: Element's cancel-code
    // severity split.
    return code == QLatin1String("m.key_mismatch")
        || code == QLatin1String("m.user_error")
        || code == QLatin1String("m.mismatched_sas");
}

bool VerificationPageRules::flowMatches(
        const QString &eventFlow, const QString &pageFlow) {
    return eventFlow.isEmpty() || pageFlow.isEmpty() || eventFlow == pageFlow;
}

bool VerificationPageRules::acceptDone(
        const QString &eventFlow, const QString &pageFlow) {
    // intro_verify_emoji.cpp / intro_verify_qr.cpp: Done guard mirrors the
    // Cancelled guard — an unlatched page owns nothing.
    if (pageFlow.isEmpty()) {
        return false;
    }
    return eventFlow.isEmpty() || eventFlow == pageFlow;
}

bool VerificationPageRules::showCancelledOnEmojiPage(
        const QString &eventFlow,
        const QString &pageFlow,
        const QString &ignoredFlow) {
    // intro_verify_emoji.cpp: swallow the deliberately-abandoned flow, ignore
    // a foreign flow once latched.
    if (!eventFlow.isEmpty() && eventFlow == ignoredFlow) {
        return false;
    }
    if (!eventFlow.isEmpty() && !pageFlow.isEmpty() && eventFlow != pageFlow) {
        return false;
    }
    return true;
}

bool VerificationPageRules::showCancelledOnQrPage(
        const QString &eventFlow, const QString &pageFlow) {
    // intro_verify_qr.cpp: unlike the emoji page, an unlatched QR page does
    // not yet own anything (it latches early from states).
    if (pageFlow.isEmpty()) {
        return false;
    }
    return eventFlow.isEmpty() || eventFlow == pageFlow;
}

bool VerificationPageRules::emojisReviveIgnoredFlow(
        const QString &eventFlow, const QString &ignoredFlow) {
    // intro_verify_emoji.cpp: emojis for the ignored flow prove the cancel
    // lost the race.
    return !eventFlow.isEmpty() && eventFlow == ignoredFlow;
}

bool VerificationPageRules::emojisBelongToPage(
        const QString &eventFlow,
        const QString &pageFlow,
        const QString &ignoredFlow) {
    // intro_verify_emoji.cpp: an unlatched page is not a wildcard — accepting a
    // foreign flow's emojis there renders them with "They match" armed.
    if (eventFlow.isEmpty()) {
        return true;
    }
    return eventFlow == pageFlow || eventFlow == ignoredFlow;
}

bool VerificationPageRules::adoptCancelCodeStrict(
        const QString &eventFlow, const QString &pageFlow) {
    // intro_verify_emoji.cpp / intro_verify_qr.cpp: require a real match on
    // both sides so a foreign flow's code can't mislabel this page's own
    // later Cancelled.
    return !eventFlow.isEmpty() && !pageFlow.isEmpty() && eventFlow == pageFlow;
}

bool VerificationPageRules::qrLatchesFromState(
        int state, const QString &eventFlow, const QString &pageFlow) {
    // intro_verify_qr.cpp: fallback only. Never overwrites the latch the
    // start-call reply set, and excludes the two foreign-taggable states —
    // kWaitingForReady (any incoming request's id) and kRequesting (the
    // previous flow's id, since Rust emits it before tagging the new one).
    if (!pageFlow.isEmpty() || eventFlow.isEmpty()) {
        return false;
    }
    return state == kReady || state == kQrCodeReady || state == kQrCodeScanned;
}

bool VerificationPageRules::dialogAcceptsDone(
        const QString &eventFlow, const QString &dialogFlow, bool emojisShown) {
    // verify_user_dialog.cpp: terminal-state guard + _emojisShown gate.
    if (!emojisShown) {
        return false;
    }
    return flowMatches(eventFlow, dialogFlow);
}

} // namespace TeleMatrix

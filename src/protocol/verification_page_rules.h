// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QString>

namespace TeleMatrix {

// Pure event-routing rules for the verification UI pages (intro emoji/QR pages
// and VerifyUserDialog). No widgets, no bridge — every decision is a function of
// the event's flow id and the page's latched state, so the whole guard surface
// is table-testable. History: both merge blockers of the element-alignment
// branch lived in these decisions while they were inline in the pages.
struct VerificationPageRules {
    // VerificationState discriminants (FFI contract — never renumber).
    static constexpr int kRequesting = 0;
    static constexpr int kWaitingForReady = 1;
    static constexpr int kReady = 2;
    static constexpr int kSasStarted = 3;
    static constexpr int kSasEmojisAvailable = 4;
    static constexpr int kSasWaitingForConfirm = 5;
    static constexpr int kQrCodeReady = 6;
    static constexpr int kQrCodeScanned = 7;
    static constexpr int kDone = 8;
    static constexpr int kCancelled = 9;
    static constexpr int kSkipped = 10;

    // Element's severity split: these cancel codes mean the keys did not match
    // and the failure is security-relevant, not routine.
    [[nodiscard]] static bool isSecurityCancelCode(const QString &code);

    // An event tagged `eventFlow` is not foreign to a page that owns `pageFlow`.
    // Empty on either side is a wildcard (pre-latch grace / untagged emission).
    [[nodiscard]] static bool flowMatches(
        const QString &eventFlow, const QString &pageFlow);

    // Terminal Done on the intro pages: fires verified() only for a latched,
    // matching (or untagged) flow. An unlatched page never owns a completion.
    [[nodiscard]] static bool acceptDone(
        const QString &eventFlow, const QString &pageFlow);

    // Cancelled on the emoji page: swallow the deliberately-abandoned flow,
    // ignore a foreign flow once latched — but an UNLATCHED page must still
    // show the failure (peer may decline before emojis; with no timeouts
    // anywhere, swallowing it would strand the page forever).
    [[nodiscard]] static bool showCancelledOnEmojiPage(
        const QString &eventFlow,
        const QString &pageFlow,
        const QString &ignoredFlow);

    // Cancelled on the QR page: requires a latched, matching (or untagged)
    // flow — the QR page latches early from states, so unlatched means "not
    // ours yet" there, unlike the emoji page.
    [[nodiscard]] static bool showCancelledOnQrPage(
        const QString &eventFlow, const QString &pageFlow);

    // Emoji payloads arriving for the flow the page marked ignored prove the
    // cancel lost a race and that flow now serves this page: stop ignoring it,
    // so its own later Cancelled is reported rather than swallowed.
    [[nodiscard]] static bool emojisReviveIgnoredFlow(
        const QString &eventFlow, const QString &ignoredFlow);

    // Cancel-code adoption for later severity classification. Strict requires
    // a real match on both sides (emoji page + dialog); loose also adopts on
    // either side empty (QR page's current behavior — its display guard is
    // what keeps an unlatched adoption from ever being read).
    [[nodiscard]] static bool adoptCancelCodeStrict(
        const QString &eventFlow, const QString &pageFlow);
    [[nodiscard]] static bool adoptCancelCodeLoose(
        const QString &eventFlow, const QString &pageFlow);

    // QR page's latch from state broadcasts. Never kRequesting: Rust emits it
    // before tagging the new flow's id, so from the second flow of a session
    // onward it carries the previous flow's id.
    [[nodiscard]] static bool qrLatchesFromState(
        int state, const QString &eventFlow);

    // VerifyUserDialog's Done gate: only a flow whose emojis this dialog
    // actually showed may claim the user is verified.
    [[nodiscard]] static bool dialogAcceptsDone(
        const QString &eventFlow, const QString &dialogFlow, bool emojisShown);
};

} // namespace TeleMatrix

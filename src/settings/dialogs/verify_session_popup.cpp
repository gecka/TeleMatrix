// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "verify_session_popup.h"

#include "dialogs/dialogs_intro_box.h"
#include "intro/intro_colors.h"
#include "intro/verification_flow.h"
#include "styles/style_constants.h"

#include <QPointer>

namespace TeleMatrix {

namespace {

// The backend keeps a single active self-verification flow, so a second popup
// would strand the first; raise the existing one instead of opening another.
QPointer<QWidget> g_activeVerifyBox;

} // namespace

bool ShowVerifySessionPopup(
        ProtocolBridge *bridge,
        QWidget *parent,
        const QString &incomingFlowId) {
    if (!bridge) {
        return false;
    }
    if (g_activeVerifyBox) {
        g_activeVerifyBox->raise();
        g_activeVerifyBox->activateWindow();
        return false;
    }

    // The popup sits over the running (themed) app, so render the verification
    // screens in the live theme rather than the intro's fixed light palette.
    // Must run BEFORE the widgets are built — their palette caches and the card
    // background read the intro colors at construction.
    intro::applyCurrentTheme();

    auto *flow = new VerificationFlow(bridge);
    auto *box = new DialogsIntroBox(flow, parent);
    g_activeVerifyBox = box;
    // Shorter than the sign-in card: these screens end well above its bottom.
    box->setPreferredHeight(st::verifyBoxHeight);

    QObject::connect(flow, &VerificationFlow::done, box, [box] {
        box->accept();
    });
    QObject::connect(flow, &VerificationFlow::skipped, box, [box] {
        box->reject();
    });

    // Shown before the flow starts: activating a step kicks off backend work
    // whose results the steps only act on while visible.
    box->show();
    flow->start(
        incomingFlowId.isEmpty()
            ? VerificationFlow::Entry::Choice
            : VerificationFlow::Entry::Emoji,
        incomingFlowId);
    box->exec();

    const auto verified = flow->isVerified();
    if (!verified) {
        // Dismissed mid-flow (Escape, ×, click outside, "Skip for now") — don't
        // leave a request running that nobody is going to answer.
        flow->cancel();
    }
    box->deleteLater();
    return verified;
}

} // namespace TeleMatrix

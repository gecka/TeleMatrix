// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QString>

class QWidget;

namespace TeleMatrix {

class ProtocolBridge;

/// Show the in-app "Verify this session" popup — the intro verification screens
/// (VerificationFlow) in a popup card — and block until it closes. Returns
/// whether the session ended up verified, which is not the same as how the
/// popup was closed: verifying and then dismissing the success screen with the
/// × still counts.
///
/// `incomingFlowId` opens straight on emoji comparison attached to that request,
/// for a verification started by another of our own sessions. Verifying ANOTHER
/// user is a different flow — see VerifyUserDialog.
bool ShowVerifySessionPopup(
    ProtocolBridge *bridge,
    QWidget *parent,
    const QString &incomingFlowId = QString());

} // namespace TeleMatrix

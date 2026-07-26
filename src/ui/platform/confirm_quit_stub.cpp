// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "ui/platform/confirm_quit_mac.h"

// Non-Apple fallbacks for the quit-confirmation helpers. macOS shows a native
// "Hold ⌘Q to Quit" HUD; Windows/Linux have no such idiom, so quitting proceeds
// immediately and the menu hint uses the conventional Ctrl+Q.

namespace TeleMatrix::Platform {

bool ConfirmQuitRunModal(const QString &text) {
    (void)text;
    return true;
}

QString QuitKeysString() {
    return QStringLiteral("Ctrl+Q");
}

} // namespace TeleMatrix::Platform

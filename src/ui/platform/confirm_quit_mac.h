// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QString>

namespace TeleMatrix::Platform {

/// Show a native macOS "Hold ⌘Q to Quit" HUD panel.
/// Returns true if the user confirmed the quit (held long enough or double-tapped).
/// Returns false if the user released early (quit should be cancelled).
[[nodiscard]] bool ConfirmQuitRunModal(const QString &text);

/// Returns the keyboard shortcut string for the Quit menu item (e.g. "⌘Q").
[[nodiscard]] QString QuitKeysString();

} // namespace TeleMatrix::Platform

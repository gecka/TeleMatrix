// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <Qt>
#include <QString>

namespace TeleMatrix {

/// Input submit mode: which key sends a message vs. inserts a newline.
enum class InputSubmitSettings {
    Enter = 0,      // Enter sends, Shift+Enter inserts newline
    CtrlEnter = 1,  // Ctrl/Cmd+Enter sends, Enter inserts newline
};

/// Returns true if the given key event should trigger a message send.
/// This respects the current submit setting and properly handles:
/// - Mention autocomplete (caller must check that first)
/// - Shift+Enter for newlines in Enter mode
/// - Plain Enter for newlines in CtrlEnter mode
[[nodiscard]] bool ShouldSubmit(
    int key,
    Qt::KeyboardModifiers modifiers,
    InputSubmitSettings setting);

/// Returns a human-readable label for the submit setting.
/// On macOS: "Send messages with Enter" / "Send messages with Cmd+Enter"
/// On other platforms: "Send messages with Enter" / "Send messages with Ctrl+Enter"
[[nodiscard]] QString LabelForSubmitSetting(InputSubmitSettings setting);

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

namespace TeleMatrix::EmojiPickerKeys {

/// Whether a key press that surfaced on the open emoji popup should close the
/// panel and be replayed into the message field.
///
/// The popup grabs the keyboard app-wide, so ordinary typing lands on it rather
/// than on the composer and has to be handed back. Two kinds of press must not
/// be: Escape, which the picker closes itself on, and bare modifier presses,
/// which carry no text and only surface here because the focused search field
/// ignored them and Qt propagated them up to the popup.
[[nodiscard]] bool ShouldForwardToComposer(int key);

} // namespace TeleMatrix::EmojiPickerKeys

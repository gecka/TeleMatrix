// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "emoji_picker_keys.h"

#include <QtCore/qnamespace.h>

namespace TeleMatrix::EmojiPickerKeys {

bool ShouldForwardToComposer(int key) {
    switch (key) {
    case Qt::Key_Escape:
    case Qt::Key_Shift:
    case Qt::Key_Control: // Command on macOS (Qt swaps it with Meta).
    case Qt::Key_Meta:
    case Qt::Key_Alt: // Option on macOS.
    case Qt::Key_AltGr:
    case Qt::Key_CapsLock:
    case Qt::Key_NumLock:
    case Qt::Key_ScrollLock:
    case Qt::Key_Super_L:
    case Qt::Key_Super_R:
    case Qt::Key_Hyper_L:
    case Qt::Key_Hyper_R:
    case Qt::Key_Mode_switch:
        return false;
    default:
        return true;
    }
}

} // namespace TeleMatrix::EmojiPickerKeys

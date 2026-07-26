// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_message_layout.h"

namespace TeleMatrix::HistoryMessage {

QRect BubbleGeometry::rect() const {
    return valid ? QRect(left, top, width, height) : QRect();
}

} // namespace TeleMatrix::HistoryMessage

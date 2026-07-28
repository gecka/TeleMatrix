// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "app/dialogs_width.h"

#include <algorithm>

namespace TeleMatrix::Dialogs {

int RestoredWidth(
        int savedWidth,
        int defaultWidth,
        int minWidth,
        int maxWidth) {
    const auto width = (savedWidth > 0) ? savedWidth : defaultWidth;
    return std::clamp(width, minWidth, maxWidth);
}

} // namespace TeleMatrix::Dialogs

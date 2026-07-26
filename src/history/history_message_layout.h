// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QRect>

#include "../styles/style_constants.h"

namespace TeleMatrix::HistoryMessage {

enum class BubbleTailSide {
    None,
    Left,
    Right,
};

struct BubbleCorners {
    int topLeft = st::bubbleRadiusLarge;
    int topRight = st::bubbleRadiusLarge;
    int bottomRight = st::bubbleRadiusLarge;
    int bottomLeft = st::bubbleRadiusLarge;
};

struct BubbleGeometry {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
    BubbleCorners corners;
    BubbleTailSide tail = BubbleTailSide::None;
    bool valid = false;

    [[nodiscard]] QRect rect() const;
};

} // namespace TeleMatrix::HistoryMessage

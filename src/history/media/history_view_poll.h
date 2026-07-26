// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QPainter>
#include <QString>

#include "../../protocol/protocol_types.h"
#include "../history_message.h"

namespace TeleMatrix {
namespace HistoryViewPoll {

/// Calculate the total content height for a poll bubble (excluding outer bubble padding).
int contentHeight(const TimelineItem &item, int innerWidth);

/// Paint the poll content inside an already-drawn bubble.
/// `x`, `y` is the top-left of the content area (inside bubble padding).
void paintContent(
    QPainter &p,
    const TimelineItem &item,
    const MessagePaintContext &ctx,
    int x,
    int y,
    int width,
    bool isOutgoing);

/// Hit-test: returns the option ID if the click position hits an answer row.
QString optionAt(
    const TimelineItem &item,
    const MessagePaintContext &ctx,
    int contentX,
    int contentY,
    int width,
    QPoint pos);

/// Hit-test: returns true if the click hit the "Vote" button (multi-choice only).
bool voteButtonAt(
    const TimelineItem &item,
    const MessagePaintContext &ctx,
    int contentX,
    int contentY,
    int width,
    QPoint pos);

} // namespace HistoryViewPoll
} // namespace TeleMatrix

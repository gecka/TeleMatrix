// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QPainter>
#include <QRect>

#include "../history_message.h"

namespace TeleMatrix {
namespace HistoryViewAudio {

int bubbleHeight(
    const TimelineItem &item,
    int maxWidth,
    const MessagePaintContext &ctx);

void paint(
    QPainter &p,
    const TimelineItem &item,
    const MessagePaintContext &ctx,
    bool isOut,
    bool showSender,
    int bubbleLeft,
    int bubbleWidth,
    qreal dpr,
    const std::function<int(int, int, int, bool)> &drawReactions);

QRect playButtonRect(
    const TimelineItem &item,
    const MessagePaintContext &ctx);

double waveformSeekAt(
    const TimelineItem &item,
    const MessagePaintContext &ctx,
    QPoint pos,
    int x,
    int y,
    int width);

int audioBubbleWidth(
    const TimelineItem &item,
    int maxWidth,
    bool isOut);

} // namespace HistoryViewAudio
} // namespace TeleMatrix

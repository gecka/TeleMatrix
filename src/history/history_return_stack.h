// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QString>
#include <QVector>

namespace TeleMatrix {

struct HistoryReturnPosition {
    QString anchorEventId;
    int pixelOffset = 0;
};

class HistoryReturnStack {
public:
    void clear();

    void pushReply(const QString &eventId);
    [[nodiscard]] bool hasReply() const;
    [[nodiscard]] QString takeReply();

    void pushPosition(const HistoryReturnPosition &position);
    [[nodiscard]] bool hasPosition() const;
    [[nodiscard]] const HistoryReturnPosition &lastPosition() const;
    void dropLastPosition();
    [[nodiscard]] HistoryReturnPosition takePosition();

private:
    static constexpr int kMaxStackSize = 50;

    QVector<QString> _replyEvents;
    QVector<HistoryReturnPosition> _positions;
};

} // namespace TeleMatrix

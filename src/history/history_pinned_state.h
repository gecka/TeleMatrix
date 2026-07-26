// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "../protocol/protocol_types.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace TeleMatrix {

class HistoryPinnedState {
public:
    [[nodiscard]] const QVector<TimelineItem> &messages() const;
    [[nodiscard]] bool isEmpty() const;
    [[nodiscard]] int size() const;

    void clear();
    void setFetchedMessages(const QVector<TimelineItem> &fetched);
    void updateFromSlice(
        const QStringList &pinnedEventIds,
        const QVector<TimelineItem> &messages);
    void applyLocalPinState(
        const QString &eventId,
        bool pinned,
        const QVector<TimelineItem> &timelineMessages);

    [[nodiscard]] QVector<TimelineItem> resolvedMessages() const;
    [[nodiscard]] bool hasUnresolvedMessages() const;
    [[nodiscard]] bool hasUnresolvedPlaceholders() const;

    [[nodiscard]] int currentIndex() const;
    [[nodiscard]] int clampedIndexForCount(int count) const;
    void resetCurrentIndex();
    void clampCurrentIndex();
    void advanceCurrentIndex();

    [[nodiscard]] bool startJump(const QString &eventId);
    [[nodiscard]] bool jumpInFlight() const;
    [[nodiscard]] QString jumpEventId() const;
    [[nodiscard]] bool completeJump();
    [[nodiscard]] bool cancelJump(const QString &eventId);
    [[nodiscard]] bool consumeManualScrollReset();

    void setFetchRoomId(const QString &roomId);
    void clearFetchRoomId();
    [[nodiscard]] bool acceptsFetchForRoom(const QString &roomId) const;

private:
    [[nodiscard]] static bool isResolved(const TimelineItem &message);
    [[nodiscard]] static bool isUnresolvedPlaceholder(const TimelineItem &message);

    QVector<TimelineItem> _messages;
    int _currentIndex = 0;
    bool _jumpInFlight = false;
    QString _jumpEventId;
    bool _jumpCompleted = false;
    QString _fetchRoomId;
};

} // namespace TeleMatrix

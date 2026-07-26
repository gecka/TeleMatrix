// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_pinned_state.h"

#include <QHash>
#include <QSet>

#include <algorithm>

namespace TeleMatrix {

const QVector<TimelineItem> &HistoryPinnedState::messages() const {
    return _messages;
}

bool HistoryPinnedState::isEmpty() const {
    return _messages.isEmpty();
}

int HistoryPinnedState::size() const {
    return _messages.size();
}

void HistoryPinnedState::clear() {
    _messages.clear();
    _currentIndex = 0;
    _jumpInFlight = false;
    _jumpEventId.clear();
    _jumpCompleted = false;
    _fetchRoomId.clear();
}

void HistoryPinnedState::setFetchedMessages(const QVector<TimelineItem> &fetched) {
    QHash<QString, const TimelineItem*> fetchedById;
    for (const auto &item : fetched) {
        if (!item.eventId.isEmpty()) {
            fetchedById.insert(item.eventId, &item);
        }
    }

    QVector<TimelineItem> ordered;
    QSet<QString> usedIds;
    ordered.reserve(fetched.size());
    for (const auto &existing : _messages) {
        const auto it = fetchedById.constFind(existing.eventId);
        if (it == fetchedById.constEnd() || usedIds.contains(existing.eventId)) {
            continue;
        }
        ordered.push_back(*it.value());
        usedIds.insert(existing.eventId);
    }
    for (const auto &item : fetched) {
        if (item.eventId.isEmpty() || usedIds.contains(item.eventId)) {
            continue;
        }
        ordered.push_back(item);
        usedIds.insert(item.eventId);
    }

    // Order like the timeline: oldest first, newest last (by message datetime).
    std::stable_sort(ordered.begin(), ordered.end(),
        [](const TimelineItem &a, const TimelineItem &b) {
            return a.timestamp < b.timestamp;
        });
    _messages = ordered;
    clampCurrentIndex();
}

void HistoryPinnedState::updateFromSlice(
        const QStringList &pinnedEventIds,
        const QVector<TimelineItem> &messages) {
    QHash<QString, const TimelineItem*> existingById;
    for (const auto &existing : _messages) {
        existingById.insert(existing.eventId, &existing);
    }

    QVector<TimelineItem> newPinned;
    QSet<QString> foundIds;
    QStringList previousIds;
    previousIds.reserve(_messages.size());
    for (const auto &msg : _messages) {
        previousIds.push_back(msg.eventId);
    }

    QHash<QString, const TimelineItem*> sliceById;
    for (const auto &msg : messages) {
        if (!msg.eventId.isEmpty()) {
            sliceById.insert(msg.eventId, &msg);
        }
    }

    for (const auto &id : pinnedEventIds) {
        if (foundIds.contains(id)) {
            continue;
        }
        if (sliceById.contains(id)) {
            auto pinned = *sliceById[id];
            pinned.isPinned = true;
            newPinned.push_back(pinned);
            foundIds.insert(id);
            continue;
        }
        if (existingById.contains(id)) {
            newPinned.push_back(*existingById[id]);
            foundIds.insert(id);
            continue;
        }

        TimelineItem unresolved;
        unresolved.eventId = id;
        unresolved.isPinned = true;
        unresolved.content = TimelineTextContent{ .body = QStringLiteral("Loading...") };
        newPinned.push_back(unresolved);
        foundIds.insert(id);
    }

    // Order like the timeline: oldest first, newest last (by message datetime).
    // Unresolved placeholders (timestamp 0) sort to the top until fetched.
    std::stable_sort(newPinned.begin(), newPinned.end(),
        [](const TimelineItem &a, const TimelineItem &b) {
            return a.timestamp < b.timestamp;
        });

    QStringList newIds;
    newIds.reserve(newPinned.size());
    for (const auto &msg : newPinned) {
        newIds.push_back(msg.eventId);
    }

    const auto changed = (newIds != previousIds);
    _messages = newPinned;
    if (changed) {
        resetCurrentIndex();
    } else {
        clampCurrentIndex();
    }
}

void HistoryPinnedState::applyLocalPinState(
        const QString &eventId,
        bool pinned,
        const QVector<TimelineItem> &timelineMessages) {
    if (pinned) {
        const auto alreadyPinned = std::any_of(
            _messages.cbegin(),
            _messages.cend(),
            [&](const TimelineItem &message) { return message.eventId == eventId; });
        if (!alreadyPinned) {
            for (const auto &message : timelineMessages) {
                if (message.eventId == eventId) {
                    _messages.push_back(message);
                    break;
                }
            }
            std::sort(
                _messages.begin(),
                _messages.end(),
                [](const TimelineItem &a, const TimelineItem &b) {
                    return a.timestamp < b.timestamp;
                });
        }
    } else {
        _messages.erase(
            std::remove_if(
                _messages.begin(),
                _messages.end(),
                [&](const TimelineItem &message) { return message.eventId == eventId; }),
            _messages.end());
    }
    clampCurrentIndex();
}

QVector<TimelineItem> HistoryPinnedState::resolvedMessages() const {
    QVector<TimelineItem> result;
    result.reserve(_messages.size());
    for (const auto &message : _messages) {
        if (isResolved(message)) {
            result.push_back(message);
        }
    }
    return result;
}

bool HistoryPinnedState::hasUnresolvedMessages() const {
    return std::any_of(
        _messages.cbegin(),
        _messages.cend(),
        [](const TimelineItem &message) {
            return !isResolved(message);
        });
}

bool HistoryPinnedState::hasUnresolvedPlaceholders() const {
    return std::any_of(
        _messages.cbegin(),
        _messages.cend(),
        [](const TimelineItem &message) {
            return isUnresolvedPlaceholder(message);
        });
}

int HistoryPinnedState::currentIndex() const {
    return _currentIndex;
}

int HistoryPinnedState::clampedIndexForCount(int count) const {
    if (count <= 0) {
        return 0;
    }
    return qBound(0, _currentIndex, count - 1);
}

void HistoryPinnedState::resetCurrentIndex() {
    _currentIndex = 0;
}

void HistoryPinnedState::clampCurrentIndex() {
    _currentIndex = clampedIndexForCount(_messages.size());
}

void HistoryPinnedState::advanceCurrentIndex() {
    if (_messages.size() > 1) {
        _currentIndex = (_currentIndex + 1) % _messages.size();
    }
}

bool HistoryPinnedState::startJump(const QString &eventId) {
    if (_messages.isEmpty() || _jumpInFlight || eventId.isEmpty()) {
        return false;
    }
    _jumpInFlight = true;
    _jumpEventId = eventId;
    _jumpCompleted = false;
    return true;
}

bool HistoryPinnedState::jumpInFlight() const {
    return _jumpInFlight;
}

QString HistoryPinnedState::jumpEventId() const {
    return _jumpEventId;
}

bool HistoryPinnedState::completeJump() {
    if (!_jumpInFlight) {
        return false;
    }
    _jumpInFlight = false;
    _jumpCompleted = true;
    _jumpEventId.clear();
    advanceCurrentIndex();
    return true;
}

bool HistoryPinnedState::cancelJump(const QString &eventId) {
    if (!_jumpInFlight) {
        return false;
    }
    if (!eventId.isEmpty() && _jumpEventId != eventId) {
        return false;
    }
    _jumpInFlight = false;
    _jumpCompleted = false;
    _jumpEventId.clear();
    return true;
}

bool HistoryPinnedState::consumeManualScrollReset() {
    if (!_jumpCompleted) {
        return false;
    }
    _jumpCompleted = false;
    if (_currentIndex == 0 || _messages.isEmpty()) {
        return false;
    }
    _currentIndex = 0;
    return true;
}

void HistoryPinnedState::setFetchRoomId(const QString &roomId) {
    _fetchRoomId = roomId;
}

void HistoryPinnedState::clearFetchRoomId() {
    _fetchRoomId.clear();
}

bool HistoryPinnedState::acceptsFetchForRoom(const QString &roomId) const {
    return _fetchRoomId == roomId;
}

bool HistoryPinnedState::isResolved(const TimelineItem &message) {
    return message.timestamp != 0
        || !formattedText(message).isEmpty()
        || !isTextMessage(message);
}

bool HistoryPinnedState::isUnresolvedPlaceholder(const TimelineItem &message) {
    return bodyText(message) == QStringLiteral("Loading...")
        && formattedText(message).isEmpty()
        && isTextMessage(message)
        && message.timestamp == 0;
}

} // namespace TeleMatrix

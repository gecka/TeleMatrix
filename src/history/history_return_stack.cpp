// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_return_stack.h"

namespace TeleMatrix {

void HistoryReturnStack::clear() {
    _replyEvents.clear();
    _positions.clear();
}

void HistoryReturnStack::pushReply(const QString &eventId) {
    if (eventId.isEmpty()) {
        return;
    }
    if (_replyEvents.size() >= kMaxStackSize) {
        _replyEvents.removeFirst();
    }
    _replyEvents.append(eventId);
}

bool HistoryReturnStack::hasReply() const {
    return !_replyEvents.isEmpty();
}

QString HistoryReturnStack::takeReply() {
    return _replyEvents.isEmpty() ? QString() : _replyEvents.takeLast();
}

void HistoryReturnStack::pushPosition(const HistoryReturnPosition &position) {
    if (position.anchorEventId.isEmpty()) {
        return;
    }
    if (_positions.size() >= kMaxStackSize) {
        _positions.removeFirst();
    }
    _positions.append(position);
}

bool HistoryReturnStack::hasPosition() const {
    return !_positions.isEmpty();
}

const HistoryReturnPosition &HistoryReturnStack::lastPosition() const {
    return _positions.back();
}

void HistoryReturnStack::dropLastPosition() {
    if (!_positions.isEmpty()) {
        _positions.removeLast();
    }
}

HistoryReturnPosition HistoryReturnStack::takePosition() {
    return _positions.isEmpty() ? HistoryReturnPosition() : _positions.takeLast();
}

} // namespace TeleMatrix

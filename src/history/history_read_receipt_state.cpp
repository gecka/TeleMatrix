// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_read_receipt_state.h"

namespace TeleMatrix {

bool HistoryReadReceiptState::canRequest(const QString &eventId) const {
    return !eventId.isEmpty()
        && eventId != _lastEventId
        && eventId != _requestedEventId;
}

void HistoryReadReceiptState::setPending(const QString &eventId) {
    _pendingEventId = eventId;
}

void HistoryReadReceiptState::clearPending() {
    _pendingEventId.clear();
}

bool HistoryReadReceiptState::hasPending() const {
    return !_pendingEventId.isEmpty();
}

QString HistoryReadReceiptState::pendingEventId() const {
    return _pendingEventId;
}

void HistoryReadReceiptState::markRequested(const QString &eventId) {
    _requestedEventId = eventId;
}

void HistoryReadReceiptState::clearRequested() {
    _requestedEventId.clear();
}

bool HistoryReadReceiptState::isRequested(const QString &eventId) const {
    return !eventId.isEmpty()
        && eventId == _requestedEventId;
}

void HistoryReadReceiptState::confirmRequested(const QString &eventId) {
    _lastEventId = eventId;
    if (_pendingEventId == eventId) {
        _pendingEventId.clear();
    }
    _requestedEventId.clear();
}

void HistoryReadReceiptState::resetForRoom() {
    _lastEventId.clear();
    _requestedEventId.clear();
    _pendingEventId.clear();
}

} // namespace TeleMatrix

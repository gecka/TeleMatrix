// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history/message_index.h"

namespace TeleMatrix {

void MessageIndex::clear() {
    _logical.clear();
    _base = 0;
}

void MessageIndex::rebuild(const QVector<QString> &eventIds) {
    _logical.clear();
    _base = 0;
    _logical.reserve(eventIds.size());
    for (int i = 0; i < eventIds.size(); ++i) {
        if (!eventIds[i].isEmpty()) {
            _logical.insert(eventIds[i], i);
        }
    }
}

void MessageIndex::prependFront(const QVector<QString> &frontIds) {
    // Existing entries shift right by the prepended count implicitly via the
    // base offset; only the new front entries need inserting.
    _base -= frontIds.size();
    for (int i = 0; i < frontIds.size(); ++i) {
        if (!frontIds[i].isEmpty()) {
            _logical.insert(frontIds[i], i + _base);
        }
    }
}

void MessageIndex::setAt(const QString &eventId, int physicalIndex) {
    if (!eventId.isEmpty()) {
        _logical.insert(eventId, physicalIndex + _base);
    }
}

void MessageIndex::remove(const QString &eventId) {
    _logical.remove(eventId);
}

int MessageIndex::physicalIndexOf(const QString &eventId) const {
    const auto it = _logical.constFind(eventId);
    return (it == _logical.cend()) ? -1 : (it.value() - _base);
}

bool MessageIndex::contains(const QString &eventId) const {
    return _logical.contains(eventId);
}

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_typing_state.h"

#include <QCoreApplication>

namespace TeleMatrix {

void HistoryTypingState::setOutgoingSent(bool sent) {
    _outgoingSent = sent;
}

bool HistoryTypingState::outgoingSent() const {
    return _outgoingSent;
}

bool HistoryTypingState::setIncomingUsers(const QStringList &userIds, qint64 now) {
    _incomingUsers = userIds;
    if (_incomingUsers.isEmpty()) {
        _animationStart = 0;
        return false;
    }
    if (_animationStart == 0) {
        _animationStart = now;
    }
    return true;
}

void HistoryTypingState::clearIncomingUsers() {
    _incomingUsers.clear();
    _animationStart = 0;
}

bool HistoryTypingState::hasIncomingUsers() const {
    return !_incomingUsers.isEmpty();
}

qint64 HistoryTypingState::animationStart() const {
    return _animationStart;
}

QString HistoryTypingState::subtitleText(bool directChat) const {
    if (_incomingUsers.isEmpty()) {
        return QString();
    }
    if (_incomingUsers.size() == 1) {
        if (directChat) {
            return QCoreApplication::translate("HistoryWidget", "typing");
        }
        auto name = _incomingUsers.first();
        if (name.startsWith(QLatin1Char('@'))) {
            name = name.mid(1).split(QLatin1Char(':')).first();
        }
        return QCoreApplication::translate("HistoryWidget", "%1 is typing").arg(name);
    }
    if (_incomingUsers.size() == 2) {
        auto first = _incomingUsers[0];
        auto second = _incomingUsers[1];
        if (first.startsWith(QLatin1Char('@'))) {
            first = first.mid(1).split(QLatin1Char(':')).first();
        }
        if (second.startsWith(QLatin1Char('@'))) {
            second = second.mid(1).split(QLatin1Char(':')).first();
        }
        return QCoreApplication::translate("HistoryWidget", "%1 and %2 are typing").arg(first, second);
    }
    return QCoreApplication::translate(
        "HistoryWidget",
        "%n people are typing",
        nullptr,
        _incomingUsers.size());
}

} // namespace TeleMatrix

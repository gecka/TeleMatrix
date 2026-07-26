// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QString>
#include <QStringList>

namespace TeleMatrix {

class HistoryTypingState {
public:
    void setOutgoingSent(bool sent);
    [[nodiscard]] bool outgoingSent() const;

    [[nodiscard]] bool setIncomingUsers(const QStringList &userIds, qint64 now);
    void clearIncomingUsers();
    [[nodiscard]] bool hasIncomingUsers() const;
    [[nodiscard]] qint64 animationStart() const;
    [[nodiscard]] QString subtitleText(bool directChat) const;

private:
    bool _outgoingSent = false;
    QStringList _incomingUsers;
    qint64 _animationStart = 0;
};

} // namespace TeleMatrix

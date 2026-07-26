// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QString>

namespace TeleMatrix {

class HistoryReadReceiptState {
public:
    [[nodiscard]] bool canRequest(const QString &eventId) const;

    void setPending(const QString &eventId);
    void clearPending();
    [[nodiscard]] bool hasPending() const;
    [[nodiscard]] QString pendingEventId() const;

    void markRequested(const QString &eventId);
    void clearRequested();
    [[nodiscard]] bool isRequested(const QString &eventId) const;

    void confirmRequested(const QString &eventId);
    void resetForRoom();

private:
    QString _lastEventId;
    QString _requestedEventId;
    QString _pendingEventId;
};

} // namespace TeleMatrix

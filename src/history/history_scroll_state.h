// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "../protocol/protocol_types.h"

#include <QHash>
#include <QString>
#include <optional>

namespace TeleMatrix {

class HistoryScrollStateStore {
public:
    void save(const QString &roomId, const RoomScrollState &state);
    [[nodiscard]] bool has(const QString &roomId) const;
    [[nodiscard]] RoomScrollState value(const QString &roomId) const;

    void setPendingRestore(const RoomScrollState &state);
    void clearPendingRestore();
    [[nodiscard]] bool hasPendingRestore() const;
    [[nodiscard]] RoomScrollState pendingRestore() const;

private:
    QHash<QString, RoomScrollState> _states;
    std::optional<RoomScrollState> _pendingRestore;
};

} // namespace TeleMatrix

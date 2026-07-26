// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "../protocol/protocol_types.h"

#include <QHash>
#include <QVector>
#include <QString>
#include <optional>

namespace TeleMatrix {

struct PendingLocalMediaUpload {
    ContentType type = ContentType::File;
    QString mediaPath;
    // The file whose bytes were handed to the last sendMedia — `mediaPath`,
    // except for a compressed image, where it is the recompressed temp. Kept
    // apart from `mediaPath` (which resends and the optimistic preview use, and
    // which must never be deleted) so the sent event's mxc can be resolved to
    // the exact bytes the server has.
    QString uploadPath;
    QString thumbPath;
    // Room this upload belongs to. Pending uploads now outlive room switches
    // (a direct upload has no SDK echo to persist the bubble), so the room is
    // tracked to re-show the optimistic echo only in its own room.
    QString roomId;
    // Full send params, retained so a Failed upload can be re-sent with the same
    // transaction id (a direct upload has no send-queue retry).
    QString mime;
    QString filename;
    QString caption;
    quint64 size = 0;
    int width = 0;
    int height = 0;
    quint64 durationMs = 0;
};

class HistoryPendingLocalMediaState {
public:
    void insert(
        const QString &eventId,
        const PendingLocalMediaUpload &upload);

    /// Record the file the upload actually reads, once it is known (an image is
    /// recompressed after its bubble is already on screen).
    void setUploadPath(const QString &eventId, const QString &path);

    [[nodiscard]] bool isEmpty() const;
    [[nodiscard]] bool hasPendingForRoom(const QString &roomId) const;
    [[nodiscard]] bool contains(const QString &eventId) const;
    [[nodiscard]] std::optional<PendingLocalMediaUpload> upload(const QString &eventId) const;

    [[nodiscard]] std::optional<PendingLocalMediaUpload> take(const QString &eventId);
    [[nodiscard]] QVector<PendingLocalMediaUpload> takeAll();

private:
    QHash<QString, PendingLocalMediaUpload> _uploadsByEventId;
};

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_pending_local_media.h"

#include <utility>

namespace TeleMatrix {

void HistoryPendingLocalMediaState::insert(
        const QString &eventId,
        const PendingLocalMediaUpload &upload) {
    _uploadsByEventId.insert(eventId, upload);
}

void HistoryPendingLocalMediaState::setUploadPath(
        const QString &eventId,
        const QString &path) {
    const auto it = _uploadsByEventId.find(eventId);
    if (it != _uploadsByEventId.end()) {
        it->uploadPath = path;
    }
}

bool HistoryPendingLocalMediaState::isEmpty() const {
    return _uploadsByEventId.isEmpty();
}

bool HistoryPendingLocalMediaState::hasPendingForRoom(const QString &roomId) const {
    for (const auto &upload : std::as_const(_uploadsByEventId)) {
        if (upload.roomId == roomId) {
            return true;
        }
    }
    return false;
}

bool HistoryPendingLocalMediaState::contains(const QString &eventId) const {
    return _uploadsByEventId.contains(eventId);
}

std::optional<PendingLocalMediaUpload>
HistoryPendingLocalMediaState::upload(const QString &eventId) const {
    const auto it = _uploadsByEventId.constFind(eventId);
    if (it == _uploadsByEventId.constEnd()) {
        return std::nullopt;
    }
    return it.value();
}

std::optional<PendingLocalMediaUpload>
HistoryPendingLocalMediaState::take(const QString &eventId) {
    const auto it = _uploadsByEventId.find(eventId);
    if (it == _uploadsByEventId.end()) {
        return std::nullopt;
    }
    const auto upload = it.value();
    _uploadsByEventId.erase(it);
    return upload;
}

QVector<PendingLocalMediaUpload> HistoryPendingLocalMediaState::takeAll() {
    QVector<PendingLocalMediaUpload> uploads;
    uploads.reserve(_uploadsByEventId.size());
    for (const auto &upload : std::as_const(_uploadsByEventId)) {
        uploads.push_back(upload);
    }
    _uploadsByEventId.clear();
    return uploads;
}

} // namespace TeleMatrix

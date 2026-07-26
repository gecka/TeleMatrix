// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_video_thumbnail_probe_state.h"

#include "protocol/media_cache.h"

#include <utility>

namespace TeleMatrix {

HistoryVideoThumbnailProbeState::HistoryVideoThumbnailProbeState(
        QObject *parent,
        const QVector<TimelineItem> &messages,
        std::function<void(const QString &eventId, const QString &mxcUrl)> requestThumbnail)
    : QObject(parent)
    , _messages(messages)
    , _requestThumbnail(std::move(requestThumbnail)) {
}

void HistoryVideoThumbnailProbeState::clear() {
    _probedEventIds.clear();
}

void HistoryVideoThumbnailProbeState::queueMessage(int messageIndex) {
    if (messageIndex < 0 || messageIndex >= _messages.size()) {
        return;
    }
    const auto &msg = _messages[messageIndex];
    const auto url = mediaUrl(msg);
    if (!isVideoMessage(msg)
        || !mediaThumbUrl(msg).isEmpty()
        || url.isEmpty()
        || msg.eventId.isEmpty()
        || _probedEventIds.contains(msg.eventId)) {
        return;
    }
    const auto probeKey = QStringLiteral("vidthumb:") + msg.eventId;
    if (!MediaCache::loadImage(probeKey).isNull()) {
        _probedEventIds.insert(msg.eventId);
        return;
    }
    // Request a thumbnail for any visible video lacking a sender/server preview.
    // The Rust side fetches only a small partial (first ~2MB) to extract a frame,
    // so this neither waits for nor triggers a full video download.
    _probedEventIds.insert(msg.eventId);
    if (_requestThumbnail) {
        _requestThumbnail(msg.eventId, url);
    }
}

void HistoryVideoThumbnailProbeState::queueResolved(int firstIndex) {
    firstIndex = qBound(0, firstIndex, _messages.size());
    for (int i = firstIndex; i < _messages.size(); ++i) {
        queueMessage(i);
    }
}

} // namespace TeleMatrix

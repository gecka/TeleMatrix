// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "../protocol/protocol_types.h"

#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

#include <functional>

namespace TeleMatrix {

/// Drives opportunistic per-message video-thumbnail extraction. Frames are
/// produced by the Rust FFI (FFmpeg + persistent encrypted cache); this class
/// only decides which messages need one and fires the request via the supplied
/// callback. Per-event dedup keeps each video probed at most once per session.
class HistoryVideoThumbnailProbeState final : public QObject {
public:
    HistoryVideoThumbnailProbeState(
        QObject *parent,
        const QVector<TimelineItem> &messages,
        std::function<void(const QString &eventId, const QString &mxcUrl)> requestThumbnail);

    void clear();
    void queueMessage(int messageIndex);
    void queueResolved(int firstIndex = 0);

private:
    const QVector<TimelineItem> &_messages;
    std::function<void(const QString &eventId, const QString &mxcUrl)> _requestThumbnail;

    QSet<QString> _probedEventIds;
};

} // namespace TeleMatrix

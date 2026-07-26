// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "history_message.h"

#include <QString>

namespace TeleMatrix {

class HistoryAudioPlaybackState {
public:
    void startFile(const QString &eventId, qint64 knownDurationMs);
    void startMemory(
        const QString &eventId,
        const QString &mediaUrl,
        qint64 knownDurationMs);
    void stop();
    void pause();
    void resume();
    void setPosition(qint64 positionMs);
    void setDuration(qint64 durationMs);

    [[nodiscard]] QString eventId() const;
    [[nodiscard]] QString mediaUrl() const;
    [[nodiscard]] bool memoryPlaybackActive() const;
    [[nodiscard]] qint64 positionMs() const;
    [[nodiscard]] qint64 durationMs() const;
    [[nodiscard]] bool paused() const;

    [[nodiscard]] AudioPlaybackState paintState() const;

private:
    QString _eventId;
    QString _mediaUrl;
    qint64 _positionMs = 0;
    qint64 _durationMs = 0;
    bool _paused = false;
    bool _memoryPlaybackActive = false;
};

} // namespace TeleMatrix

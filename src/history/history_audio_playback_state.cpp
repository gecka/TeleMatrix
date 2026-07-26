// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_audio_playback_state.h"

#include <QtGlobal>

namespace TeleMatrix {

void HistoryAudioPlaybackState::startFile(
        const QString &eventId,
        qint64 knownDurationMs) {
    _eventId = eventId;
    _mediaUrl.clear();
    _positionMs = 0;
    _durationMs = qMax<qint64>(0, knownDurationMs);
    _paused = false;
    _memoryPlaybackActive = false;
}

void HistoryAudioPlaybackState::startMemory(
        const QString &eventId,
        const QString &mediaUrl,
        qint64 knownDurationMs) {
    _eventId = eventId;
    _mediaUrl = mediaUrl;
    _positionMs = 0;
    _durationMs = qMax<qint64>(0, knownDurationMs);
    _paused = false;
    _memoryPlaybackActive = true;
}

void HistoryAudioPlaybackState::stop() {
    _eventId.clear();
    _mediaUrl.clear();
    _positionMs = 0;
    _durationMs = 0;
    _paused = false;
    _memoryPlaybackActive = false;
}

void HistoryAudioPlaybackState::pause() {
    _paused = true;
}

void HistoryAudioPlaybackState::resume() {
    _paused = false;
}

void HistoryAudioPlaybackState::setPosition(qint64 positionMs) {
    _positionMs = qMax<qint64>(0, positionMs);
}

void HistoryAudioPlaybackState::setDuration(qint64 durationMs) {
    _durationMs = qMax<qint64>(0, durationMs);
}

QString HistoryAudioPlaybackState::eventId() const {
    return _eventId;
}

QString HistoryAudioPlaybackState::mediaUrl() const {
    return _mediaUrl;
}

bool HistoryAudioPlaybackState::memoryPlaybackActive() const {
    return _memoryPlaybackActive;
}

qint64 HistoryAudioPlaybackState::positionMs() const {
    return _positionMs;
}

qint64 HistoryAudioPlaybackState::durationMs() const {
    return _durationMs;
}

bool HistoryAudioPlaybackState::paused() const {
    return _paused;
}

AudioPlaybackState HistoryAudioPlaybackState::paintState() const {
    AudioPlaybackState state;
    state.playingEventId = _eventId;
    state.positionMs = _positionMs;
    state.durationMs = _durationMs;
    state.isPaused = _paused;
    return state;
}

} // namespace TeleMatrix

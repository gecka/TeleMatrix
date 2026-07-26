// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_media_request_state.h"

namespace TeleMatrix {

bool HistoryMediaRequestState::DeferredUpdateBatch::isEmpty() const {
    return progress.isEmpty()
        && invalidations.isEmpty()
        && !updateOnly;
}

void HistoryMediaRequestState::deferProgress(
        const QString &mxcUrl,
        quint64 receivedBytes,
        quint64 totalBytes,
        uint phase) {
    _deferredProgress.insert(
        mxcUrl,
        DeferredProgress{ receivedBytes, totalBytes, phase });
}

void HistoryMediaRequestState::removeDeferredProgress(const QString &mxcUrl) {
    _deferredProgress.remove(mxcUrl);
}

void HistoryMediaRequestState::deferInvalidation(const QString &mxcUrl) {
    _deferredInvalidations.insert(mxcUrl);
}

void HistoryMediaRequestState::deferUpdate() {
    _deferredUpdate = true;
}

bool HistoryMediaRequestState::hasDeferredUpdates() const {
    return !_deferredProgress.isEmpty()
        || !_deferredInvalidations.isEmpty()
        || _deferredUpdate;
}

HistoryMediaRequestState::DeferredUpdateBatch
HistoryMediaRequestState::takeDeferredUpdates() {
    DeferredUpdateBatch batch;
    batch.progress = _deferredProgress;
    batch.invalidations = _deferredInvalidations;
    batch.updateOnly = _deferredUpdate;

    _deferredProgress.clear();
    _deferredInvalidations.clear();
    _deferredUpdate = false;

    return batch;
}

void HistoryMediaRequestState::cancelDownload(const QString &mxcUrl) {
    _cancelledDownloads.insert(mxcUrl);
}

void HistoryMediaRequestState::resumeDownload(const QString &mxcUrl) {
    _cancelledDownloads.remove(mxcUrl);
}

bool HistoryMediaRequestState::isCancelled(const QString &mxcUrl) const {
    return _cancelledDownloads.contains(mxcUrl);
}

bool HistoryMediaRequestState::consumeCancelled(const QString &mxcUrl) {
    return _cancelledDownloads.remove(mxcUrl) > 0;
}

void HistoryMediaRequestState::requestFileOpen(const QString &mxcUrl) {
    _pendingFileOpen.insert(mxcUrl);
}

bool HistoryMediaRequestState::takePendingFileOpen(const QString &mxcUrl) {
    return _pendingFileOpen.remove(mxcUrl) > 0;
}

void HistoryMediaRequestState::requestAudioPlay(
        const QString &mxcUrl,
        const QString &eventId) {
    _pendingAudioPlay.insert(mxcUrl, eventId);
}

bool HistoryMediaRequestState::hasPendingAudioPlay(const QString &mxcUrl) const {
    return _pendingAudioPlay.contains(mxcUrl);
}

QString HistoryMediaRequestState::takePendingAudioPlay(const QString &mxcUrl) {
    return _pendingAudioPlay.take(mxcUrl);
}

void HistoryMediaRequestState::requestVideoOpen(
        const QString &mxcUrl,
        const QString &eventId) {
    _pendingVideoOpen.insert(mxcUrl, eventId);
}

bool HistoryMediaRequestState::hasPendingVideoOpen(const QString &mxcUrl) const {
    return _pendingVideoOpen.contains(mxcUrl);
}

QString HistoryMediaRequestState::takePendingVideoOpen(const QString &mxcUrl) {
    return _pendingVideoOpen.take(mxcUrl);
}

void HistoryMediaRequestState::clearPendingVideoOpen(const QString &mxcUrl) {
    _pendingVideoOpen.remove(mxcUrl);
}

void HistoryMediaRequestState::clearPendingOpenRequests(const QString &mxcUrl) {
    _pendingFileOpen.remove(mxcUrl);
    _pendingAudioPlay.remove(mxcUrl);
    _pendingVideoOpen.remove(mxcUrl);
}

void HistoryMediaRequestState::clearPendingPlaybackRequests(const QString &mxcUrl) {
    _pendingAudioPlay.remove(mxcUrl);
    _pendingVideoOpen.remove(mxcUrl);
}

} // namespace TeleMatrix

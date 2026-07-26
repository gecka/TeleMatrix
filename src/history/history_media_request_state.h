// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QHash>
#include <QSet>
#include <QString>

namespace TeleMatrix {

class HistoryMediaRequestState {
public:
    struct DeferredProgress {
        quint64 receivedBytes = 0;
        quint64 totalBytes = 0;
        uint phase = 0;
    };

    struct DeferredUpdateBatch {
        QHash<QString, DeferredProgress> progress;
        QSet<QString> invalidations;
        bool updateOnly = false;

        [[nodiscard]] bool isEmpty() const;
    };

    void deferProgress(
        const QString &mxcUrl,
        quint64 receivedBytes,
        quint64 totalBytes,
        uint phase);
    void removeDeferredProgress(const QString &mxcUrl);
    void deferInvalidation(const QString &mxcUrl);
    void deferUpdate();

    [[nodiscard]] bool hasDeferredUpdates() const;
    [[nodiscard]] DeferredUpdateBatch takeDeferredUpdates();

    void cancelDownload(const QString &mxcUrl);
    void resumeDownload(const QString &mxcUrl);
    [[nodiscard]] bool isCancelled(const QString &mxcUrl) const;
    [[nodiscard]] bool consumeCancelled(const QString &mxcUrl);

    void requestFileOpen(const QString &mxcUrl);
    [[nodiscard]] bool takePendingFileOpen(const QString &mxcUrl);
    void requestAudioPlay(const QString &mxcUrl, const QString &eventId);
    [[nodiscard]] bool hasPendingAudioPlay(const QString &mxcUrl) const;
    [[nodiscard]] QString takePendingAudioPlay(const QString &mxcUrl);
    void requestVideoOpen(const QString &mxcUrl, const QString &eventId);
    [[nodiscard]] bool hasPendingVideoOpen(const QString &mxcUrl) const;
    [[nodiscard]] QString takePendingVideoOpen(const QString &mxcUrl);
    void clearPendingVideoOpen(const QString &mxcUrl);
    void clearPendingOpenRequests(const QString &mxcUrl);
    void clearPendingPlaybackRequests(const QString &mxcUrl);

private:
    QHash<QString, DeferredProgress> _deferredProgress;
    QSet<QString> _deferredInvalidations;
    bool _deferredUpdate = false;
    QSet<QString> _cancelledDownloads;
    QSet<QString> _pendingFileOpen;
    QHash<QString, QString> _pendingAudioPlay;
    QHash<QString, QString> _pendingVideoOpen;
};

} // namespace TeleMatrix

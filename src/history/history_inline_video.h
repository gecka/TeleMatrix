// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>

#include <functional>

#include "media/video_container.h"
#include "media/video_rebuffer.h"
#include "media/video_stream_retry.h"

class QMediaPlayer;
class QVideoSink;
class QAudioOutput;
class QTimer;

namespace TeleMatrix {

// Single shared inline video player for the timeline: plays one video at a time
// inside its message bubble. Ported from MediaViewOverlay's player — same
// cache-first -> loopback-stream -> full-download fallback, the same
// _streamingEverWorked session rule, and a 300ms proxy-progress poll. Emits
// frameChanged/stateChanged so the owning HistoryList repaints the bubble.
class HistoryInlineVideoPlayer : public QObject {
    Q_OBJECT

public:
    explicit HistoryInlineVideoPlayer(QObject *parent = nullptr);
    ~HistoryInlineVideoPlayer() override;

    // Bridge-dependent hooks, wired from AppMainWidget (like the overlay).
    void setStreamUrlCallback(std::function<QString(const QString &)> cb);
    void setStreamProgressCallback(std::function<float(const QString &)> cb);
    void setStreamProgressBytesCallback(
        std::function<bool(const QString &, quint64 &, quint64 &)> cb);
    // Reports whether the proxy's download for an mxc has failed outright, so the
    // retry wait can fail fast to the local fallback instead of stalling out.
    void setStreamErroredCallback(std::function<bool(const QString &)> cb);
    // Reports the video's container verdict, so a moov-at-end video shows a real
    // download bar from the start instead of a spinner (see showDeterminateDownload).
    void setStreamContainerCallback(std::function<VideoContainer(const QString &)> cb);
    void setResolveRequester(std::function<void(const QString &)> cb);

    // Start `eventId`'s video, or pause/resume it if it is already the active one.
    void toggle(
        const QString &eventId,
        const QString &mxc,
        const QString &filename,
        const QString &mime,
        qint64 durationMs);
    void stop();
    void pause();
    void resume();
    void toggleMute();
    void seekToFraction(qreal fraction); // clamped to the downloaded fraction
    // Pause and keep the player alive when handing off to the fullscreen viewer, so
    // returning lands on a paused frame with controls (see seekPausedTo).
    void pauseForFullscreen();
    // Seek to an absolute position while staying paused (used to re-sync to the
    // fullscreen position on viewer close). Renders that frame under the pause.
    void seekPausedTo(qint64 positionMs);

    // Downscale decoded frames to this device-pixel size (long-edge cap) so the
    // retained frame and per-paint blit are ~1:1 instead of full-resolution.
    // Set from the paint path once the on-screen video rect is known.
    void setDisplaySize(QSize deviceSize);

    // Auto-pause when the active video scrolls out of the viewport and resume on
    // return, without overriding an explicit user pause (see toggle()).
    void pauseForViewport(bool visible);

    [[nodiscard]] QString activeEventId() const { return _eventId; }
    [[nodiscard]] QString currentMxc() const { return _mxc; }
    [[nodiscard]] const QImage &currentFrame() const { return _currentFrame; }
    [[nodiscard]] qint64 positionMs() const { return _positionMs; }
    [[nodiscard]] qint64 durationMs() const { return _durationMs; }
    [[nodiscard]] bool paused() const { return _paused; }
    [[nodiscard]] bool buffering() const { return _buffering; }
    // Terminal: this video couldn't be played (stream + local fallback failed).
    // The bubble shows an error instead of the preloader; a click retries.
    [[nodiscard]] bool failed() const { return _failed; }

    /// Called when the network comes back (e.g. after sleep): clears a transient
    /// streaming-disabled latch and re-attempts a failed/stuck active video with a
    /// fresh proxy download, instead of waiting for a user re-click.
    void onNetworkOnline();
    [[nodiscard]] float downloadedFraction() const { return _downloadedFraction; }
    [[nodiscard]] quint64 downloadedBytes() const { return _streamDownloadedBytes; }
    [[nodiscard]] quint64 totalBytes() const { return _streamTotalBytes; }
    [[nodiscard]] int videoRotation() const { return _videoRotation; }
    [[nodiscard]] bool streaming() const { return _videoStreamActive; }
    // True from the first stream attempt until playback starts or the video fails
    // terminally — unlike streaming(), it stays true across a retry wait, where the
    // player is torn down but the proxy download is still running. This is what the
    // download overlay keys on, so progress doesn't revert to a spinner mid-wait.
    [[nodiscard]] bool streamDownloadPending() const { return _streamDownloadPending; }
    [[nodiscard]] VideoContainer container() const { return _streamContainer; }
    [[nodiscard]] bool muted() const { return _muted; }

signals:
    void frameChanged(const QString &eventId);
    void stateChanged(const QString &eventId);

private:
    void load();
    bool loadLocal();
    void setupPlayer();
    void teardownPlayer();
    void markFailed(); // terminal failure: keep state, show error, stop the player
    bool updateRebuffering(); // proactive rebuffer state machine; true if changed

    // Transient stream read-timeout recovery: after a timeout, wait for the
    // background download to catch up (or restart a stalled one), then replay —
    // the automatic form of the manual "try again" that succeeds.
    void scheduleStreamRetry();
    void pollStreamRetry();
    void abortStreamRetry();
    void failStreamRetry(); // give up: local fallback if present, else markFailed

    QString _eventId;
    QString _mxc;
    QString _filename;
    QString _mime;

    QMediaPlayer *_player = nullptr;
    QVideoSink *_videoSink = nullptr;
    QAudioOutput *_audioOutput = nullptr;
    QTimer *_progressTimer = nullptr;
    QTimer *_retryTimer = nullptr; // polls the download while awaiting a replay

    QImage _currentFrame;
    QSize _displaySize; // device-px cap for decoded frames (null = full-res)
    bool _userPaused = false;     // explicit user pause — viewport must not resume
    bool _viewportPaused = false; // auto-paused because scrolled offscreen
    bool _viewportVisible = true; // last viewport visibility (recorded even with no
                                  // player, so a retry replay knows to start paused)
    qint64 _positionMs = 0;
    qint64 _durationMs = 0;
    qint64 _startPositionMs = 0; // resume position handed off from the overlay
    int _videoRotation = 0;
    bool _videoReady = false;
    bool _paused = false;
    bool _failed = false; // terminal: video couldn't be played

    bool _videoStreamActive = false;
    bool _streamDownloadPending = false; // proxy download in flight (survives teardown)
    VideoContainer _streamContainer = VideoContainer::Unknown; // latched once known
    bool _streamingUnavailable = false;
    bool _streamingEverWorked = false;
    bool _buffering = false;
    bool _muted = false; // session mute preference, persists across videos
    VideoRebufferController _rebuffer; // proactive (re)buffering state machine
    float _downloadedFraction = 1.0f;
    quint64 _streamDownloadedBytes = 0;
    quint64 _streamTotalBytes = 0;
    qreal _volume = 0.9;

    // Stream read-timeout recovery state (see scheduleStreamRetry). The stall/wait
    // tick accounting lives in _streamRetry; _streamRetries is the separate
    // errorOccurred-triggered replay budget.
    int _streamRetries = 0;                  // per-video errorOccurred replay budget
    VideoStreamRetryController _streamRetry; // in-wait poll state machine

    std::function<QString(const QString &)> _streamUrlCallback;
    std::function<float(const QString &)> _streamProgressCallback;
    std::function<bool(const QString &, quint64 &, quint64 &)>
        _streamProgressBytesCallback;
    std::function<bool(const QString &)> _streamErroredCallback;
    std::function<VideoContainer(const QString &)> _streamContainerCallback;
    std::function<void(const QString &)> _resolveRequester;
};

} // namespace TeleMatrix

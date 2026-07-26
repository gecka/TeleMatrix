// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <functional>
#include <QElapsedTimer>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QVariantAnimation>
#include <QVector>
#include <QWidget>

#include "../protocol/protocol_types.h"
#include "media/video_container.h"
#include "media/video_rebuffer.h"
#include "media/video_stream_retry.h"

class QKeyEvent;
class QMouseEvent;
class QAudioOutput;
class QMediaPlayer;
class QPaintEvent;
class QResizeEvent;
class QTimer;
class QVideoSink;
class QWheelEvent;

namespace TeleMatrix {

/// Evict the theme-dependent tinted-icon cache used by the media viewer.
/// Must be called on theme change so stale (old-palette) entries don't
/// accumulate unbounded across theme switches.
void clearMediaviewTintCache();

/// Full-window media viewer overlay for image and video messages.
class MediaViewOverlay : public QWidget {
    Q_OBJECT

public:
    explicit MediaViewOverlay(QWidget *parent = nullptr);

    void showImage(
        const QString &mediaUrl,
        const QString &caption = QString(),
        const QString &senderName = QString(),
        qint64 timestamp = 0);

    void showImageWithContext(
        const QVector<TimelineItem> &items,
        int currentIndex);

    void showMediaWithContext(
        const QVector<TimelineItem> &items,
        int currentIndex);

    /// Reload the current entry if this mxc:// URL has just been resolved.
    void mediaResolved(const QString &mxcUrl);
    void setResolveMediaCallback(std::function<void(const QString &, bool)> callback);
    void setExportMediaCallback(std::function<void(const QString &, const QString &)> callback);
    /// Inject the video-stream-URL provider so the overlay can query the Rust
    /// loopback proxy without holding a direct ProtocolBridge pointer.
    void setVideoStreamUrlCallback(std::function<QString(const QString &)> callback);
    void setVideoStreamProgressCallback(std::function<float(const QString &)> callback);
    void setVideoStreamProgressBytesCallback(
        std::function<bool(const QString &, quint64 &, quint64 &)> callback);
    void setVideoStreamErroredCallback(std::function<bool(const QString &)> callback);
    /// Reports the video's container verdict, so a moov-at-end video shows a real
    /// download bar from the start instead of an indeterminate spinner.
    void setVideoStreamContainerCallback(
        std::function<VideoContainer(const QString &)> callback);

    /// Persisted "last save directory" wiring (injected so the overlay stays
    /// decoupled from the settings store). The provider returns the remembered
    /// dir (empty = none yet); the remember callback stores a freshly chosen one.
    void setSavedMediaDirProvider(std::function<QString()> provider);
    void setRememberSaveDir(std::function<void(const QString &)> remember);

    void closeViewer();

signals:
    // Emitted when the viewer closes while showing a video, carrying the final
    // playback position. Lets an inline player that handed off to fullscreen re-sync
    // to that position (paused) instead of reverting to the poster.
    void videoClosed(const QString &mxcUrl, qint64 positionMs);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    enum class Over {
        None,
        Close,
        Save,
        PlayPause,
        Seekbar,
        VolumeToggle,
        VolumeSlider,
        FullScreen,
    };

    struct MediaEntry {
        ContentType type = ContentType::Image;
        QString mediaUrl;
        QString mediaFilename;
        QString mediaMime;
        QString caption;
        QString senderName;
        qint64 timestamp = 0;
        qint64 durationMs = 0;
    };

    void updateImageGeometry();
    void updateControlGeometry();
    void updateHover(const QPoint &pos);
    QRect targetScreenGeometry() const;
    void syncToTargetScreenGeometry();
    void showOverlay();
    void navigateTo(int index);
    void activateControls();
    void hideControls();
    void saveImage();
    // Run a save dialog rooted at the remembered dir (or Downloads), restore the
    // overlay's keyboard focus after the modal dialog closes, and remember the
    // chosen directory. Returns the chosen path, or empty if cancelled.
    QString runSaveDialog(
        const QString &title,
        const QString &suggestedName,
        const QString &filter);
    [[nodiscard]] QString initialSaveDir() const;
    void copyImageToClipboard();
    bool loadImage(const QString &mediaUrl);
    bool loadVideo(const QString &mediaUrl, const QString &filename, const QString &mime, qint64 durationMs);
    // The full-download/local-file playback path. Invoked directly, and as the
    // fallback when streaming fails on a media backend that can't play the
    // loopback http source (e.g. macOS AVFoundation).
    bool loadVideoLocal(const QString &mediaUrl, const QString &filename, const QString &mime);
    void setupVideoPlayer();
    void teardownVideoPlayer();
    // Transient stream read-timeout recovery, ported from the inline player: after a
    // ResourceError, wait for the background download to catch up (or restart a dead
    // one) via VideoStreamRetryController, then replay — instead of a duplicate full
    // download through resolveMedia. Falls back to the local file on give-up.
    void scheduleStreamRetry();
    void pollStreamRetry();
    void abortStreamRetry();
    void paintPlaybackControls(QPainter &p);
    // Draw the animated buffering spinner (a rotating arc) centred in _imageRect.
    void paintVideoSpinner(QPainter &p);
    // Determinate progress ring + "received / total" badge, shown in place of the
    // spinner while a video that can't stream downloads in full.
    void paintVideoDownloadProgress(QPainter &p);
    // Start/stop the spinner repaint timer to match whether the spinner is shown.
    void updateSpinnerTimer(bool running);
    void updateSeekFromX(int x);
    void updateVolumeFromX(int x);
    void playbackToggle();
    void seekRelative(qint64 deltaMs);
    void seekToPercent(qreal percent);
    void setVolume(qreal volume);
    void toggleMute();
    void toggleFullScreenVideo();
    void requestResolveIfNeeded(const QString &mediaUrl);
    QString formatPlayTime(qint64 ms) const;
    QString currentMediaPath() const;

    QImage _image;
    QImage _currentFrame;
    int _videoRotation = 0;        // cached rotation from first frame (0/90/180/270)
    QRect _imageRect;
    QString _caption;
    QString _senderName;
    qint64 _timestamp = 0;
    qint64 _duration = 0;
    qint64 _position = 0;
    qint64 _videoStartPositionMs = 0; // resume position handed off from inline
    bool _isVideo = false;
    bool _videoReady = false;
    bool _fullScreenVideo = false;
    bool _firstShowSettled = false;

    QVector<MediaEntry> _mediaEntries;
    int _currentIndex = -1;

    Over _over = Over::None;
    QPoint _lastMousePressPos;
    bool _seekDragging = false;
    bool _volumeDragging = false;
    qreal _seekDragValue = 0.0;
    qreal _volume = 0.9;
    qreal _lastPositiveVolume = 0.9;
    bool _wasPlayingBeforeSeek = false;

    QTimer *_hideTimer = nullptr;
    QTimer *_progressTimer = nullptr;
    // Drives ~60fps repaints of the loading/buffering spinner; runs only while
    // the spinner is on screen (stopped otherwise to avoid idle CPU). The phase
    // is wall-clock based so the arc keeps spinning regardless of repaint timing.
    QTimer *_spinnerTimer = nullptr;
    QElapsedTimer _spinnerClock;
    qreal _controlsOpacity = 1.0;
    QVariantAnimation *_controlsAnimation = nullptr;
    bool _controlsShown = true;

    QRect _closeRect;
    QRect _saveRect;
    QRect _controllerRect;
    QRect _playPauseRect;
    QRect _seekbarRect;
    QRect _volumeToggleRect;
    QRect _volumeSliderRect;
    QRect _fullScreenRect;

    QMediaPlayer *_player = nullptr;
    QVideoSink *_videoSink = nullptr;
    QAudioOutput *_audioOutput = nullptr;
    // Video-stream fallback state: the source params to retry via download, a
    // flag marking the player is loaded from the loopback proxy (so a player
    // error triggers the fallback), and a session-sticky flag set once a backend
    // proves it can't stream the http source (skip streaming thereafter).
    bool _videoStreamActive = false;
    // Proxy download in flight: unlike _videoStreamActive this survives the player
    // teardown of a retry wait, so the download overlay doesn't revert to a spinner
    // exactly while the full download it is reporting on is running.
    bool _streamDownloadPending = false;
    VideoContainer _streamContainer = VideoContainer::Unknown; // latched once known
    quint64 _streamDownloadedBytes = 0;
    quint64 _streamTotalBytes = 0;
    bool _streamingUnavailable = false;
    bool _streamingEverWorked = false;
    // Stream read-timeout recovery (ported from the inline player). _streamRetries is
    // the per-video errorOccurred replay budget; _streamRetry owns the in-wait poll
    // accounting; _retryTimer polls the download while a replay is pending.
    int _streamRetries = 0;
    VideoStreamRetryController _streamRetry;
    QTimer *_retryTimer = nullptr;
    QString _pendingVideoMxc;
    QString _pendingVideoFilename;
    QString _pendingVideoMime;
    std::function<void(const QString &, bool)> _resolveMediaCallback;
    std::function<void(const QString &, const QString &)> _exportMediaCallback;
    std::function<QString(const QString &)> _videoStreamUrlCallback;
    std::function<float(const QString &)> _videoStreamProgressCallback;
    std::function<bool(const QString &, quint64 &, quint64 &)>
        _videoStreamProgressBytesCallback;
    std::function<bool(const QString &)> _videoStreamErroredCallback;
    std::function<VideoContainer(const QString &)> _videoStreamContainerCallback;
    // Fraction (0–1) of the streaming video downloaded by the proxy; 1.0 for a
    // fully-available local file. Bounds seeking and draws the buffered bar.
    float _downloadedFraction = 1.0f;
    // Proactive (re)buffering state machine (shared with the inline player) and
    // its derived spinner flag. _userPausedVideo tracks the USER pause toggle,
    // kept distinct from a rebuffer pause so the controller doesn't auto-resume a
    // video the user paused.
    VideoRebufferController _videoRebuffer;
    bool _videoBuffering = false;
    bool _userPausedVideo = false;
    std::function<QString()> _savedMediaDirProvider;
    std::function<void(const QString &)> _rememberSaveDir;
};

} // namespace TeleMatrix

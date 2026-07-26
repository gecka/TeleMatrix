// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_inline_video.h"

#include "media/video_frame_scaler.h"
#include "protocol/media_cache.h"

#include <QAudioOutput>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QPlaybackOptions>
#include <QTimer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>

#include <chrono>

namespace TeleMatrix {
namespace {

// errorOccurred-triggered replays per video. Distinct from the in-wait retry budget
// (VideoStreamRetryController::Tunables::maxErrorRestarts): this bounds how many
// times a player error restarts the stream, that bounds restarts within one wait.
// The stall window + total-wait backstop + the FFmpeg network-timeout budget
// (streamNetworkTimeout) all live in media/video_stream_retry.h now, shared with
// the fullscreen overlay so the two stay in lockstep.
constexpr int kMaxStreamRetries = 3;
constexpr int kRetryPollIntervalMs = 500; // download-progress poll cadence

// Read the first bytes of a media file and return "<container guess> magic=<hex>".
// Lets a "Failed to load media" be told apart: an UNRECOGNISED header means the
// decrypted bytes are garbage (key/iv mismatch); a recognised container that
// still won't play points at an unsupported codec in the FFmpeg backend.
QString describeMediaHeader(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return QStringLiteral("<unreadable>");
    }
    const QByteArray head = f.read(16);
    QString guess;
    if (head.size() >= 8 && head.mid(4, 4) == QByteArrayLiteral("ftyp")) {
        guess = QStringLiteral("ISO-BMFF (mp4/mov)");
    } else if (head.startsWith(QByteArrayLiteral("\x1A\x45\xDF\xA3"))) {
        guess = QStringLiteral("EBML (mkv/webm)");
    } else if (head.startsWith(QByteArrayLiteral("RIFF"))) {
        guess = QStringLiteral("RIFF (avi/wav)");
    } else if (head.startsWith(QByteArrayLiteral("OggS"))) {
        guess = QStringLiteral("Ogg");
    } else if (head.startsWith(QByteArrayLiteral("FLV"))) {
        guess = QStringLiteral("FLV");
    } else {
        guess = QStringLiteral("UNRECOGNISED");
    }
    return guess + QStringLiteral(" magic=")
        + QString::fromLatin1(head.toHex(' '));
}

} // namespace

HistoryInlineVideoPlayer::HistoryInlineVideoPlayer(QObject *parent)
    : QObject(parent) {
}

HistoryInlineVideoPlayer::~HistoryInlineVideoPlayer() {
    teardownPlayer();
}

void HistoryInlineVideoPlayer::setStreamUrlCallback(
        std::function<QString(const QString &)> cb) {
    _streamUrlCallback = std::move(cb);
}

void HistoryInlineVideoPlayer::setStreamProgressCallback(
        std::function<float(const QString &)> cb) {
    _streamProgressCallback = std::move(cb);
}

void HistoryInlineVideoPlayer::setStreamProgressBytesCallback(
        std::function<bool(const QString &, quint64 &, quint64 &)> cb) {
    _streamProgressBytesCallback = std::move(cb);
}

void HistoryInlineVideoPlayer::setStreamErroredCallback(
        std::function<bool(const QString &)> cb) {
    _streamErroredCallback = std::move(cb);
}

void HistoryInlineVideoPlayer::setStreamContainerCallback(
        std::function<VideoContainer(const QString &)> cb) {
    _streamContainerCallback = std::move(cb);
}

void HistoryInlineVideoPlayer::setResolveRequester(
        std::function<void(const QString &)> cb) {
    _resolveRequester = std::move(cb);
}

void HistoryInlineVideoPlayer::toggle(
        const QString &eventId,
        const QString &mxc,
        const QString &filename,
        const QString &mime,
        qint64 durationMs) {
    if (eventId == _eventId && _player) {
        if (_paused) {
            _userPaused = false;
            _viewportPaused = false;
            resume();
        } else {
            _userPaused = true;
            pause();
        }
        return;
    }
    stop(); // stop any other inline video — only one plays at a time
    _eventId = eventId;
    _mxc = mxc;
    _filename = filename;
    _mime = mime;
    _durationMs = qMax<qint64>(durationMs, 0);
    // Resume position handed off from the overlay (or a prior session).
    const auto resume = MediaCache::takePlaybackPosition(mxc);
    _startPositionMs = (resume > 0) ? resume : 0;
    _positionMs = _startPositionMs;
    _videoReady = false;
    _paused = false;
    _userPaused = false;
    _viewportPaused = false;
    _viewportVisible = true;
    _displaySize = QSize();
    _buffering = false;
    _failed = false;
    _streamRetries = 0;
    _streamRetry.reset();
    _rebuffer.reset();
    _videoStreamActive = false;
    _streamDownloadPending = false;
    _streamContainer = VideoContainer::Unknown; // new video, re-seeded by load()
    _streamDownloadedBytes = 0;
    _streamTotalBytes = 0;
    _currentFrame = QImage();
    load();
    emit stateChanged(_eventId);
}

void HistoryInlineVideoPlayer::load() {
    // Prefer an already fully-downloaded local copy (instant) — the thumbnail
    // path leaves complete decrypted files in the cache for non-faststart videos.
    if (const auto cached = MediaCache::localPath(_mxc);
        !cached.isEmpty() && QFileInfo(cached).exists()) {
        loadLocal();
        return;
    }
    // Stream progressively via the loopback proxy. Skipped when there is no URL
    // (no session) or the backend already proved it cannot play the http source.
    if (_streamUrlCallback && !_streamingUnavailable) {
        const auto streamUrl = _streamUrlCallback(_mxc);
        if (!streamUrl.isEmpty()) {
            setupPlayer();
            if (_player) {
                _videoStreamActive = true;
                _streamDownloadPending = true;
                // A verdict persisted by an earlier session (or by this session's
                // thumbnail extraction) is available before a single byte streams,
                // so a moov-at-end video shows its download bar from the first paint.
                if (_streamContainer == VideoContainer::Unknown
                    && _streamContainerCallback) {
                    _streamContainer = _streamContainerCallback(_mxc);
                }
                _downloadedFraction = 0.0f; // grows as the proxy downloads
                _player->setSource(QUrl(streamUrl));
                _player->setPosition(_startPositionMs);
                _player->play();
                return;
            }
        }
    }
    loadLocal();
}

bool HistoryInlineVideoPlayer::loadLocal() {
    _downloadedFraction = 1.0f; // a local file is fully available → unrestricted seek
    _streamDownloadPending = false; // nothing is downloading through the proxy
    auto path = MediaCache::localPath(_mxc);
    auto clearPlayer = [this] {
        if (_player) {
            _player->stop();
            _player->setSource(QUrl());
        }
    };
    if (path.isEmpty()) {
        clearPlayer();
        // Stash any pending resume position across the async resolve round-trip; the
        // user's next click (toggle -> takePlaybackPosition) takes it back.
        if (_startPositionMs > 0) {
            MediaCache::setPlaybackPosition(_mxc, _startPositionMs);
        }
        if (_resolveRequester && _mxc.startsWith(QStringLiteral("mxc://"))) {
            _resolveRequester(_mxc);
        }
        return false;
    }
    if (!QFileInfo(path).exists()) {
        clearPlayer();
        return false;
    }
    setupPlayer();
    if (!_player) {
        return false;
    }
    // macOS AVFoundation needs a file extension for codec detection.
    path = MediaCache::resolvedPathForPlayback(path, _filename, _mime);
    _player->setSource(QUrl::fromLocalFile(path));
    _player->setPosition(_startPositionMs);
    _player->play();
    return true;
}

void HistoryInlineVideoPlayer::setupPlayer() {
    if (_player) {
        return;
    }
    _player = new QMediaPlayer(this);
    // The loopback stream downloads linearly (no homeserver Range support), so a
    // forward read ahead of the downloaded point blocks until the download catches
    // up. Widen the FFmpeg backend's network timeout (default 20s, then ETIMEDOUT
    // is fatal) so such a read buffers instead of failing, scaling it with the clip
    // length (a proxy for total bytes). A timeout that still fires is recovered by
    // scheduleStreamRetry(). Qt 6.10+.
    {
        auto opts = _player->playbackOptions();
        opts.setNetworkTimeout(streamNetworkTimeout(_durationMs));
        _player->setPlaybackOptions(opts);
    }
    _audioOutput = new QAudioOutput(this);
    _videoSink = new QVideoSink(this);
    _audioOutput->setVolume(_volume);
    _audioOutput->setMuted(_muted);
    _player->setAudioOutput(_audioOutput);
    _player->setVideoOutput(_videoSink);

    connect(_videoSink, &QVideoSink::videoFrameChanged, this,
        [this](const QVideoFrame &frame) {
            const auto image = frame.toImage();
            if (image.isNull()) {
                return;
            }
            if (!_videoReady) {
                _videoRotation = static_cast<int>(frame.rotation());
            }
            // Downscale to the on-screen size once, at frame rate, so we retain a
            // small frame and the per-paint blit is ~1:1 (no full-res rescale). The
            // shared helper flattens 10-bit/HDR frames to 8-bit sRGB before the
            // smooth scale, which otherwise corrupts their colors.
            _currentFrame = downscaleVideoFrame(image, _displaySize);
            if (!_videoReady) {
                _videoReady = true;
                // A frame rendered → playback recovered; refresh the replay budget
                // so an unrelated later stall gets its own retries.
                _streamRetries = 0;
                // Playback started: the download overlay hands off to the seek
                // bar's buffered sub-bar, however much is still downloading.
                _streamDownloadPending = false;
                // Streaming produced a frame → the backend CAN stream; never
                // disable streaming this session on a later video-specific error.
                if (_videoStreamActive) {
                    _streamingEverWorked = true;
                }
                // Playback has actually started — apply a handed-off resume
                // position now. Seeking earlier (during load) is ignored on a
                // network stream; seeks only take once frames are flowing.
                if (_startPositionMs > 0 && _player && _player->isSeekable()) {
                    _player->setPosition(_startPositionMs);
                    _startPositionMs = 0;
                }
            }
            if (!_rebuffer.waiting()) {
                _buffering = false; // a frame arrived → not stalled
            }
            emit frameChanged(_eventId);
        });

    connect(_player, &QMediaPlayer::positionChanged, this,
        [this](qint64 position) {
            _positionMs = qMax<qint64>(0, position);
            emit stateChanged(_eventId);
        });

    connect(_player, &QMediaPlayer::seekableChanged, this,
        [this](bool seekable) {
            // Second application point for a handed-off resume position: on a
            // network stream seekability can arrive AFTER the first frame, where
            // the one-shot first-frame seek has already run and would otherwise
            // drop the position. Whichever fires last with a pending value applies.
            if (seekable && _videoReady && _startPositionMs > 0 && _player) {
                _player->setPosition(_startPositionMs);
                _startPositionMs = 0;
            }
        });

    connect(_player, &QMediaPlayer::durationChanged, this,
        [this](qint64 duration) {
            if (duration > 0) {
                _durationMs = duration;
                emit stateChanged(_eventId);
            }
        });

    connect(_player, &QMediaPlayer::playbackStateChanged, this,
        [this](QMediaPlayer::PlaybackState) { emit stateChanged(_eventId); });

    connect(_player, &QMediaPlayer::mediaStatusChanged, this,
        [this](QMediaPlayer::MediaStatus status) {
            if (status == QMediaPlayer::EndOfMedia) {
                if (_player) {
                    _player->setPosition(0);
                    _player->pause();
                }
                _positionMs = 0;
                _paused = true;
                emit stateChanged(_eventId);
            } else if (status == QMediaPlayer::StalledMedia
                       || status == QMediaPlayer::BufferingMedia) {
                // Playback outran the linear download — show the buffering
                // spinner until frames resume (the frame handler clears it).
                _buffering = true;
                emit stateChanged(_eventId);
            } else if (status == QMediaPlayer::BufferedMedia) {
                _buffering = false;
                emit stateChanged(_eventId);
            }
        });

    connect(_player, &QMediaPlayer::errorOccurred, this,
        [this](QMediaPlayer::Error error, const QString &msg) {
            // Classify the failure from the local decrypted file (when present):
            // an UNRECOGNISED header => decryption produced garbage; a recognised
            // container that still won't play => unsupported codec.
            QString localHeader;
            if (const auto local = MediaCache::localPath(_mxc);
                !local.isEmpty() && QFileInfo(local).exists()) {
                localHeader = describeMediaHeader(local);
            }
            qWarning() << "Inline video error:" << error << msg
                       << "| mime=" << _mime
                       << "| streaming=" << _videoStreamActive
                       << "| frameSeen=" << _videoReady
                       << "| localHeader=" << localHeader;
            if (_videoStreamActive) {
                _videoStreamActive = false;
                // Preserve the reached position so the retry/fallback resumes there
                // instead of restarting at 0:00 (load()/loadLocal() seek to
                // _startPositionMs). Guard so a pre-first-frame error can't clobber a
                // still-pending handoff position with 0.
                if (_positionMs > 0) {
                    _startPositionMs = _positionMs;
                }
                // Take the freshest container verdict: the proxy classifies from the
                // head of its download, which may have landed since the last poll.
                // Without this a fast failure would be judged on a stale Unknown.
                if (_streamContainer == VideoContainer::Unknown
                    && _streamContainerCallback) {
                    _streamContainer = _streamContainerCallback(_mxc);
                }
                // Wait for the download and replay when that can plausibly help (the
                // automatic form of the manual "try again" that succeeds). Bounded
                // per video. See shouldRetryStreamError for the reasoning.
                if (shouldRetryStreamError(
                        error == QMediaPlayer::ResourceError,
                        _videoReady,
                        _streamContainer)
                    && _streamRetries < kMaxStreamRetries) {
                    ++_streamRetries;
                    teardownPlayer();
                    scheduleStreamRetry();
                    return;
                }
                // Not a transient timeout, or retries exhausted. Disable streaming
                // for the session ONLY if it never worked (the backend genuinely
                // can't play the loopback stream); once a frame arrived this session
                // a later failure is video-specific → fall back for just this video.
                if (!_videoReady && !_streamingEverWorked) {
                    _streamingUnavailable = true;
                }
                teardownPlayer();
                failStreamRetry();
                return;
            }
            // A local file that still won't decode → terminal failure.
            markFailed();
        });

    // Poll the proxy download progress (~3/s) to drive the buffered bar and bound
    // seeking. For a local file (not stream-active) the fraction stays 1.0.
    if (!_progressTimer) {
        _progressTimer = new QTimer(this);
        _progressTimer->setInterval(300);
        connect(_progressTimer, &QTimer::timeout, this, [this] {
            const float f = (_videoStreamActive && _streamProgressCallback)
                ? _streamProgressCallback(_mxc)
                : 1.0f;
            if (_videoStreamActive && _streamProgressBytesCallback) {
                quint64 d = 0, t = 0;
                if (_streamProgressBytesCallback(_mxc, d, t)) {
                    _streamDownloadedBytes = d;
                    _streamTotalBytes = t;
                }
            }
            auto changed = false;
            // The proxy classifies the container from the head of its download, so
            // an unknown verdict at load() usually resolves within a poll or two.
            if (_videoStreamActive && _streamContainer == VideoContainer::Unknown
                && _streamContainerCallback) {
                const auto container = _streamContainerCallback(_mxc);
                if (container != VideoContainer::Unknown) {
                    _streamContainer = container;
                    changed = true;
                }
            }
            if (qAbs(f - _downloadedFraction) > 0.0005f) {
                _downloadedFraction = f;
                changed = true;
            }
            if (updateRebuffering()) {
                changed = true;
            }
            if (changed) {
                emit stateChanged(_eventId);
            }
        });
    }
    _progressTimer->start();
}

void HistoryInlineVideoPlayer::teardownPlayer() {
    if (_progressTimer) {
        _progressTimer->stop();
    }
    if (!_player) {
        return;
    }
    _player->stop();
    // deleteLater (NOT delete): teardownPlayer can run from the player's own
    // errorOccurred slot, where deleting it synchronously is a use-after-free.
    // Detach the audio output first so the deferred destruction can't touch a
    // freed QAudioOutput.
    _player->setAudioOutput(nullptr);
    _player->setVideoOutput(nullptr);
    _player->deleteLater();
    _player = nullptr;
    // The sink is parented to `this` (not _player), so zeroing the pointer leaves
    // it alive + still connected — a final in-flight frame from the old player
    // would corrupt _currentFrame for the next one. Disconnect + delete it.
    if (_videoSink) {
        disconnect(_videoSink, nullptr, this, nullptr);
        _videoSink->deleteLater();
        _videoSink = nullptr;
    }
    if (_audioOutput) {
        _audioOutput->deleteLater();
        _audioOutput = nullptr;
    }
    _currentFrame = QImage();
    _videoRotation = 0;
}

void HistoryInlineVideoPlayer::markFailed() {
    // Terminal: stream + local fallback exhausted, the video can't be played.
    // Keep _eventId/_mxc so the bubble can render an error in place of the
    // preloader; a click (toggle) resets _failed and retries from scratch.
    abortStreamRetry();
    // Persist the reached position so the manual click-retry (toggle, which calls
    // takePlaybackPosition) resumes where it failed instead of at 0:00.
    if (!_mxc.isEmpty() && _positionMs > 0) {
        MediaCache::setPlaybackPosition(_mxc, _positionMs);
    }
    _failed = true;
    _streamDownloadPending = false;
    teardownPlayer();
    emit stateChanged(_eventId);
}

void HistoryInlineVideoPlayer::scheduleStreamRetry() {
    // The player is already torn down; show the buffering affordance while the
    // background download (which keeps running independently) catches up.
    _buffering = true;
    emit stateChanged(_eventId);
    if (!_retryTimer) {
        _retryTimer = new QTimer(this);
        _retryTimer->setInterval(kRetryPollIntervalMs);
        connect(_retryTimer, &QTimer::timeout, this,
            [this] { pollStreamRetry(); });
    }
    _streamRetry.beginWait();
    _retryTimer->start();
}

void HistoryInlineVideoPlayer::pollStreamRetry() {
    quint64 downloaded = 0, total = 0;
    const bool known = _streamProgressBytesCallback
        && _streamProgressBytesCallback(_mxc, downloaded, total);
    const bool errored = _streamErroredCallback && _streamErroredCallback(_mxc);
    // The player is torn down during a wait, so the 300ms progress poll is stopped:
    // feed the download overlay from here instead. Without this the bubble reverts
    // to an indeterminate spinner for the whole wait — which, on a slow link, IS the
    // wait for a non-faststart video's full download.
    if (known && total > 0) {
        _streamDownloadedBytes = downloaded;
        _streamTotalBytes = total;
        const auto fraction = float(qreal(downloaded) / qreal(total));
        if (qAbs(fraction - _downloadedFraction) > 0.0005f) {
            _downloadedFraction = fraction;
            emit stateChanged(_eventId);
        }
    }
    if (_streamContainer == VideoContainer::Unknown && _streamContainerCallback) {
        _streamContainer = _streamContainerCallback(_mxc);
    }
    switch (_streamRetry.tick(known, downloaded, total, errored)) {
    case VideoStreamRetryController::Action::Replay:
        // Download complete, or dead and needing a restart → re-open. load() prefers
        // a now-complete local copy, and get_or_start drops a failed entry and
        // downloads afresh; a replay from the complete cache no longer blocks so the
        // read timeout can't recur.
        abortStreamRetry();
        load();
        // If the row scrolled offscreen during the wait, don't start playing
        // audibly — pause and let the viewport-return path resume it.
        if (!_viewportVisible && _player && !_paused) {
            _viewportPaused = true;
            pause();
        }
        break;
    case VideoStreamRetryController::Action::GiveUp:
        abortStreamRetry();
        failStreamRetry();
        break;
    case VideoStreamRetryController::Action::Wait:
        break;
    }
}

void HistoryInlineVideoPlayer::abortStreamRetry() {
    if (_retryTimer) {
        _retryTimer->stop();
    }
}

void HistoryInlineVideoPlayer::failStreamRetry() {
    // A complete local copy can sometimes play when the http stream can't; try it.
    // With no local copy this video is unplayable here — surface an error instead
    // of spinning forever.
    if (const auto local = MediaCache::localPath(_mxc);
        !local.isEmpty() && QFileInfo(local).exists()) {
        loadLocal();
    } else {
        markFailed();
    }
}

void HistoryInlineVideoPlayer::stop() {
    const auto prev = _eventId;
    abortStreamRetry();
    teardownPlayer();
    _eventId.clear();
    _mxc.clear();
    _filename.clear();
    _mime.clear();
    _positionMs = 0;
    _durationMs = 0;
    _videoReady = false;
    _paused = false;
    _userPaused = false;
    _viewportPaused = false;
    _viewportVisible = true;
    _displaySize = QSize();
    _buffering = false;
    _failed = false;
    _streamRetries = 0;
    _streamRetry.reset();
    _rebuffer.reset();
    _videoStreamActive = false;
    _streamDownloadPending = false;
    _streamContainer = VideoContainer::Unknown;
    _downloadedFraction = 1.0f;
    if (!prev.isEmpty()) {
        emit stateChanged(prev);
    }
}

void HistoryInlineVideoPlayer::onNetworkOnline() {
    // A transient network drop (e.g. sleep) may have latched streaming off or left
    // the active video failed/stuck. Clear the latch and re-attempt with a fresh
    // proxy download instead of waiting for a user re-click.
    _streamingUnavailable = false;
    if (_mxc.isEmpty()) {
        return;
    }
    const bool stuck = _videoStreamActive && !_videoReady;
    if (_failed || stuck) {
        abortStreamRetry();
        teardownPlayer();
        _failed = false;
        _buffering = false;
        _streamRetries = 0;
        load();
    }
}

void HistoryInlineVideoPlayer::setDisplaySize(QSize deviceSize) {
    _displaySize = deviceSize;
}

void HistoryInlineVideoPlayer::pauseForViewport(bool visible) {
    // Record unconditionally: during a retry wait there is no player, but a replay
    // that lands while offscreen must know to start paused (see pollStreamRetry).
    _viewportVisible = visible;
    if (!_player || _userPaused) {
        return; // no player, or the user paused on purpose — leave it be
    }
    if (visible) {
        if (_viewportPaused) {
            _viewportPaused = false;
            resume();
        }
    } else if (!_paused) {
        _viewportPaused = true;
        pause();
    }
}

void HistoryInlineVideoPlayer::pause() {
    if (_player) {
        _player->pause();
        _paused = true;
        _rebuffer.reset(); // user took over — no auto-resume
        _buffering = false;
        emit stateChanged(_eventId);
    }
}

void HistoryInlineVideoPlayer::resume() {
    if (_player) {
        _player->play();
        _paused = false;
        emit stateChanged(_eventId);
    }
}

void HistoryInlineVideoPlayer::pauseForFullscreen() {
    // Stay paused until the user explicitly resumes (viewport logic must not
    // auto-resume it while the fullscreen viewer is up).
    _userPaused = true;
    _viewportPaused = false;
    pause();
}

void HistoryInlineVideoPlayer::seekPausedTo(qint64 positionMs) {
    if (!_player || _durationMs <= 0) {
        return;
    }
    // Returning from fullscreen: the position was already played there, so its data
    // is downloaded — seek straight to it (no downloaded-fraction clamp). The player
    // stays paused and renders the frame at that position.
    _positionMs = qBound<qint64>(0, positionMs, _durationMs);
    _player->setPosition(_positionMs);
    emit stateChanged(_eventId);
}

void HistoryInlineVideoPlayer::toggleMute() {
    _muted = !_muted;
    if (_audioOutput) {
        _audioOutput->setMuted(_muted);
    }
    emit stateChanged(_eventId);
}

void HistoryInlineVideoPlayer::seekToFraction(qreal fraction) {
    if (!_player || _durationMs <= 0) {
        return;
    }
    auto f = qBound<qreal>(0.0, fraction, 1.0);
    // Streaming: only seek within the downloaded portion (the proxy downloads
    // linearly; seeking past it would block/fail).
    if (_videoStreamActive && _downloadedFraction < 1.0f) {
        f = qMin<qreal>(f, qreal(_downloadedFraction));
    }
    _positionMs = qint64(f * _durationMs);
    _player->setPosition(_positionMs);
    emit stateChanged(_eventId);
}

bool HistoryInlineVideoPlayer::updateRebuffering() {
    if (!_player) {
        return false;
    }
    const auto action = _rebuffer.evaluate(
        _downloadedFraction, _positionMs, _durationMs, _videoStreamActive,
        _paused, _videoReady);
    if (action == VideoRebufferController::Action::Pause) {
        _buffering = true;
        _player->pause();
        return true;
    }
    if (action == VideoRebufferController::Action::Play) {
        _buffering = false;
        _player->play();
        return true;
    }
    return false;
}

} // namespace TeleMatrix

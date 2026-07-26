// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "media_view_overlay.h"

#ifdef Q_OS_MAC
#include "ui/platform/ui_utility_mac.h"
#endif

#include <QAudioOutput>
#include <QApplication>
#include <QClipboard>
#include <QColorSpace>
#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QImageReader>
#include <QKeyEvent>
#include <QLocale>
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlaybackOptions>
#include <QResizeEvent>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include "styles/style_constants.h"
#include "media/video_download_overlay_policy.h"
#include "ui/format_bytes.h"
#include "ui/painter.h"
#include "ui/style/icon_provider.h"
#include "ui/toast_widget.h"
#include "media/video_frame_scaler.h"
#include "../protocol/media_cache.h"

namespace TeleMatrix {

namespace {

// Stream read-timeout recovery tunables (mirrors the inline player). The stall
// window + total-wait backstop live in VideoStreamRetryController's Tunables.
constexpr int kMaxStreamRetries = 3;      // errorOccurred replays per video
constexpr int kRetryPollIntervalMs = 500; // download-progress poll cadence

QImage normalizeForDisplay(QImage image) {
    if (image.isNull()) {
        return image;
    }
    const auto cs = image.colorSpace();
    if (cs.isValid()) {
        const auto iccData = cs.iccProfile();
        const auto iccStr = QString::fromLatin1(iccData);
        if (iccStr.contains(QStringLiteral("P3"))
            || iccStr.contains(QStringLiteral("Display"))
            || iccStr.contains(QStringLiteral("Color LCD"))
            || iccStr.contains(QStringLiteral("Studio Display"))) {
            image.setColorSpace(QColorSpace::fromIccProfile(iccData));
            image.convertToColorSpace(QColorSpace::SRgb);
        }
    }
    return image;
}

[[nodiscard]] QString resolvedLocalPath(const QString &mediaUrl) {
    if (mediaUrl.isEmpty()) {
        return {};
    }
    if (mediaUrl.startsWith(QStringLiteral("mxc://"))) {
        return MediaCache::localPath(mediaUrl);
    }
    const auto url = QUrl(mediaUrl);
    if (url.isLocalFile()) {
        return url.toLocalFile();
    }
    return mediaUrl;
}

[[nodiscard]] QString footerDateText(qint64 timestamp) {
    if (timestamp <= 0) {
        return {};
    }
    const auto dt = QDateTime::fromSecsSinceEpoch(timestamp).toLocalTime();
    return QLocale().toString(dt, QLocale::ShortFormat);
}

[[nodiscard]] QColor withOpacity(const QColor &color, qreal opacity) {
    auto result = color;
    result.setAlphaF(result.alphaF() * std::clamp(opacity, 0.0, 1.0));
    return result;
}

[[nodiscard]] QString mediaviewIconResourcePath(
        const QString &name,
        qreal dpr) {
    const auto base = QStringLiteral(":/telematrix/icons/mediaview/") + name;
    if (dpr >= 2.5) {
        return base + QStringLiteral("@3x.png");
    }
    if (dpr >= 1.5) {
        return base + QStringLiteral("@2x.png");
    }
    return base + QStringLiteral(".png");
}

// Cache of colorized icon masks. The key embeds the (theme-dependent) color,
// so entries from previous themes would otherwise never be evicted. Lifted to
// namespace scope so clearMediaviewTintCache() can flush it on theme change.
QHash<QString, QImage> g_tintedMediaviewIconCache;

[[nodiscard]] QImage tintedMediaviewIcon(
        const QString &name,
        qreal dpr,
        const QColor &color,
        bool mirrored = false) {
    auto &cache = g_tintedMediaviewIconCache;
    const auto key = name
        + QLatin1Char('|')
        + QString::number(dpr, 'f', 1)
        + QLatin1Char('|')
        + QString::number(color.rgba(), 16)
        + QLatin1Char('|')
        + (mirrored ? QLatin1Char('1') : QLatin1Char('0'));
    if (const auto i = cache.constFind(key); i != cache.cend()) {
        return i.value();
    }

    auto mask = QImage(mediaviewIconResourcePath(name, dpr));
    if (mask.isNull()) {
        mask = QImage(QStringLiteral(":/telematrix/icons/mediaview/") + name + QStringLiteral(".png"));
    }
    if (mask.isNull()) {
        return {};
    }

    if (dpr >= 2.5) {
        mask.setDevicePixelRatio(3.0);
    } else if (dpr >= 1.5) {
        mask.setDevicePixelRatio(2.0);
    } else {
        mask.setDevicePixelRatio(1.0);
    }

    auto tinted = TeleMatrix::Style::IconProvider::colorizeMask(mask, color);
    if (mirrored) {
        auto mirroredImage = tinted.flipped(Qt::Horizontal);
        mirroredImage.setDevicePixelRatio(tinted.devicePixelRatio());
        tinted = mirroredImage;
    }
    cache.insert(key, tinted);
    return tinted;
}

void drawCenteredImage(
        QPainter &p,
        const QRect &slot,
        const QImage &image,
        qreal opacity) {
    if (image.isNull() || slot.isEmpty() || opacity <= 0.0) {
        return;
    }
    const auto w = int(image.width() / image.devicePixelRatio());
    const auto h = int(image.height() / image.devicePixelRatio());
    const auto pos = QPoint(
        slot.x() + (slot.width() - w) / 2,
        slot.y() + (slot.height() - h) / 2);
    const auto was = p.opacity();
    p.setOpacity(was * opacity);
    p.drawImage(pos, image);
    p.setOpacity(was);
}

[[nodiscard]] int iconPixelHeight(const QImage &image) {
    return image.isNull()
        ? 0
        : int(image.height() / image.devicePixelRatio());
}

[[nodiscard]] qreal normalizedPosition(qint64 position, qint64 duration) {
    if (duration <= 0) {
        return 0.0;
    }
    return std::clamp(qreal(position) / qreal(duration), 0.0, 1.0);
}

[[nodiscard]] bool isRangeKey(int key) {
    return key >= Qt::Key_0 && key <= Qt::Key_9;
}

} // namespace

void clearMediaviewTintCache() {
    g_tintedMediaviewIconCache.clear();
}

MediaViewOverlay::MediaViewOverlay(QWidget *parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::Tool)
{
    if (parent) {
        parent->installEventFilter(this);
    }
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setAttribute(Qt::WA_TranslucentBackground, true);

    _hideTimer = new QTimer(this);
    _hideTimer->setSingleShot(true);
    _hideTimer->setInterval(st::mediaviewWaitHide);
    connect(_hideTimer, &QTimer::timeout, this, &MediaViewOverlay::hideControls);

    _controlsAnimation = new QVariantAnimation(this);
    _controlsAnimation->setDuration(st::mediaviewShowDuration);
    connect(_controlsAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        _controlsOpacity = value.toReal();
        update();
    });
}

void MediaViewOverlay::showImage(
        const QString &mediaUrl,
        const QString &caption,
        const QString &senderName,
        qint64 timestamp) {
    _mediaEntries = {
        MediaEntry {
            .type = ContentType::Image,
            .mediaUrl = mediaUrl,
            .caption = caption,
            .senderName = senderName,
            .timestamp = timestamp,
            .durationMs = 0,
        }
    };
    navigateTo(0);
    showOverlay();
}

void MediaViewOverlay::showImageWithContext(
        const QVector<TimelineItem> &items,
        int currentIndex) {
    QVector<TimelineItem> imageOnly;
    imageOnly.reserve(items.size());
    auto mappedIndex = -1;
    for (auto i = 0; i < items.size(); ++i) {
        if (!isImageMessage(items[i])) {
            continue;
        }
        if (i == currentIndex) {
            mappedIndex = imageOnly.size();
        }
        imageOnly.push_back(items[i]);
    }
    if (imageOnly.isEmpty()) {
        return;
    }
    if (mappedIndex < 0 || mappedIndex >= imageOnly.size()) {
        mappedIndex = std::clamp(currentIndex, 0, int(imageOnly.size()) - 1);
    }
    showMediaWithContext(imageOnly, mappedIndex);
}

void MediaViewOverlay::showMediaWithContext(
        const QVector<TimelineItem> &items,
        int currentIndex) {
    QVector<MediaEntry> entries;
    entries.reserve(items.size());

    auto mappedIndex = -1;
    for (auto i = 0; i < items.size(); ++i) {
        const auto &item = items[i];
        if ((!isImageMessage(item) && !isVideoMessage(item))
            || mediaUrl(item).isEmpty()) {
            continue;
        }
        if (i == currentIndex) {
            mappedIndex = entries.size();
        }
        entries.push_back({
            .type = contentType(item),
            .mediaUrl = mediaUrl(item),
            .mediaFilename = mediaFilename(item),
            .mediaMime = mediaMime(item),
            .caption = !captionText(item).isEmpty() ? captionText(item) : bodyText(item),
            .senderName = item.sender.name,
            .timestamp = item.timestamp,
            .durationMs = qMax<qint64>(0, qint64(mediaDurationMs(item))),
        });
    }

    if (entries.isEmpty()) {
        return;
    }

    if (mappedIndex < 0 || mappedIndex >= entries.size()) {
        mappedIndex = std::clamp(currentIndex, 0, int(entries.size()) - 1);
    }

    _mediaEntries = std::move(entries);
    navigateTo(mappedIndex);
    showOverlay();
}

void MediaViewOverlay::closeViewer() {
    _hideTimer->stop();
    _controlsAnimation->stop();
    _seekDragging = false;
    _volumeDragging = false;
    _over = Over::None;
    // Preserve the playback position so re-opening (inline or fullscreen) resumes,
    // and notify so an inline player that handed off to fullscreen re-syncs to it.
    if (_isVideo && !_pendingVideoMxc.isEmpty() && _duration > 0) {
        MediaCache::setPlaybackPosition(_pendingVideoMxc, _position);
        emit videoClosed(_pendingVideoMxc, _position);
    }
    // Stop any pending stream-retry wait, then fully release the player (also stops
    // the progress-poll + spinner timers, so a hidden viewer can't keep polling the
    // proxy or auto-resume playback). The re-open path (loadVideo) recreates it.
    abortStreamRetry();
    teardownVideoPlayer();
    hide();
    // Release large image/video memory when the viewer is closed.
    _image = QImage();
    _currentFrame = QImage();
}

void MediaViewOverlay::mediaResolved(const QString &mxcUrl) {
    if (!isVisible() || mxcUrl.isEmpty()) {
        return;
    }
    if (_currentIndex < 0 || _currentIndex >= _mediaEntries.size()) {
        return;
    }
    const auto &entry = _mediaEntries[_currentIndex];
    if (entry.mediaUrl != mxcUrl) {
        return;
    }

    if (entry.type == ContentType::Video) {
        loadVideo(entry.mediaUrl, entry.mediaFilename, entry.mediaMime, entry.durationMs);
    } else {
        loadImage(entry.mediaUrl);
    }
    updateImageGeometry();
    update();
}

void MediaViewOverlay::setResolveMediaCallback(
        std::function<void(const QString &, bool)> callback) {
    _resolveMediaCallback = std::move(callback);
}

void MediaViewOverlay::setExportMediaCallback(
        std::function<void(const QString &, const QString &)> callback) {
    _exportMediaCallback = std::move(callback);
}

void MediaViewOverlay::setVideoStreamUrlCallback(
        std::function<QString(const QString &)> callback) {
    _videoStreamUrlCallback = std::move(callback);
}

void MediaViewOverlay::setVideoStreamProgressCallback(
        std::function<float(const QString &)> callback) {
    _videoStreamProgressCallback = std::move(callback);
}

void MediaViewOverlay::setVideoStreamProgressBytesCallback(
        std::function<bool(const QString &, quint64 &, quint64 &)> callback) {
    _videoStreamProgressBytesCallback = std::move(callback);
}

void MediaViewOverlay::setVideoStreamErroredCallback(
        std::function<bool(const QString &)> callback) {
    _videoStreamErroredCallback = std::move(callback);
}

void MediaViewOverlay::setVideoStreamContainerCallback(
        std::function<VideoContainer(const QString &)> callback) {
    _videoStreamContainerCallback = std::move(callback);
}

void MediaViewOverlay::setSavedMediaDirProvider(std::function<QString()> provider) {
    _savedMediaDirProvider = std::move(provider);
}

void MediaViewOverlay::setRememberSaveDir(std::function<void(const QString &)> remember) {
    _rememberSaveDir = std::move(remember);
}

QString MediaViewOverlay::initialSaveDir() const {
    if (_savedMediaDirProvider) {
        const auto remembered = _savedMediaDirProvider();
        if (!remembered.isEmpty() && QDir(remembered).exists()) {
            return remembered;
        }
    }
    return QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
}

QString MediaViewOverlay::runSaveDialog(
        const QString &title,
        const QString &suggestedName,
        const QString &filter) {
    const auto dir = initialSaveDir();
    const auto initialPath = dir.isEmpty()
        ? suggestedName
        : QDir(dir).filePath(suggestedName);

    const auto targetPath = QFileDialog::getSaveFileName(
        window(),
        title,
        initialPath,
        filter);

    // The modal file dialog steals window activation, and this Qt::Tool overlay
    // does not reliably regain keyboard focus when the dialog closes — which left
    // Escape (and every other shortcut) no longer reaching keyPressEvent after a
    // save. Re-activate and refocus so key handling resumes. Done before the
    // empty-path check so cancelling the dialog also restores focus.
    activateWindow();
    setFocus(Qt::ActiveWindowFocusReason);

    if (!targetPath.isEmpty() && _rememberSaveDir) {
        _rememberSaveDir(QFileInfo(targetPath).absolutePath());
    }
    return targetPath;
}

void MediaViewOverlay::requestResolveIfNeeded(const QString &mediaUrl) {
    if (!_resolveMediaCallback) {
        return;
    }
    if (!mediaUrl.startsWith(QStringLiteral("mxc://"))) {
        return;
    }
    if (!MediaCache::localPath(mediaUrl).isEmpty()) {
        return;
    }
    _resolveMediaCallback(mediaUrl, !_isVideo);
}

bool MediaViewOverlay::eventFilter(QObject *watched, QEvent *event) {
    if (watched == parentWidget()) {
        switch (event->type()) {
        case QEvent::Move:
        case QEvent::Resize:
        case QEvent::Show:
        case QEvent::WindowStateChange:
            if (isVisible()) {
                syncToTargetScreenGeometry();
                updateControlGeometry();
                updateImageGeometry();
                update();
            }
            break;
        case QEvent::Hide:
            if (isVisible()) {
                // Route through closeViewer (not a bare hide) so hiding/minimizing
                // the main window while a video plays saves the position and tears
                // the player down — otherwise it keeps decoding + playing audio with
                // no visible UI to stop it.
                closeViewer();
            }
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void MediaViewOverlay::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), st::mediaviewBg);

    if (!_firstShowSettled) {
        return;
    }

    if (_isVideo) {
        const auto hasFrame = !_currentFrame.isNull() && !_imageRect.isEmpty();
        if (hasFrame) {
            if (_videoRotation != 0) {
                p.save();
                const auto cx = _imageRect.center().x();
                const auto cy = _imageRect.center().y();
                p.translate(cx, cy);
                p.rotate(_videoRotation);
                // After rotation the raw frame dimensions map to the rotated rect.
                const auto drawW = (_videoRotation == 90 || _videoRotation == 270)
                    ? _imageRect.height() : _imageRect.width();
                const auto drawH = (_videoRotation == 90 || _videoRotation == 270)
                    ? _imageRect.width() : _imageRect.height();
                p.drawImage(QRect(-drawW / 2, -drawH / 2, drawW, drawH), _currentFrame);
                p.restore();
            } else {
                p.drawImage(_imageRect, _currentFrame);
            }
        }
        // A video that can't stream (moov at the end) downloads in full before its
        // first frame: show real progress rather than a spinner that would sit there
        // for the whole download.
        const auto showDownload = !_imageRect.isEmpty()
            && showDeterminateDownload(
                _streamContainer, hasFrame, _streamDownloadPending, _downloadedFraction);
        // Spinner while the first frame is still loading, or while a mid-playback
        // rebuffer is in progress (drawn over the last frame if we have one).
        const auto showSpinner = !showDownload
            && !_imageRect.isEmpty()
            && (!hasFrame || _videoBuffering);
        if (showDownload) {
            p.fillRect(_imageRect, st::mediaviewVideoLoadingBg);
            paintVideoDownloadProgress(p);
        } else if (showSpinner) {
            if (!hasFrame) {
                p.fillRect(_imageRect, st::mediaviewVideoLoadingBg);
            }
            paintVideoSpinner(p);
        }
        // The download ring is determinate: it repaints on progress, not on a clock.
        updateSpinnerTimer(showSpinner);
    } else if (!_image.isNull() && !_imageRect.isEmpty()) {
        PainterHighQualityEnabler hq(p);
        p.drawImage(_imageRect, _image);
    }

    p.save();
    p.setOpacity(_controlsOpacity);

    const auto dpr = devicePixelRatioF();

    // Top and bottom gradient shadows behind the media viewer controls.
    const auto topShadow = tintedMediaviewIcon(
        QStringLiteral("shadow_top"),
        dpr,
        st::windowShadowFg);
    const auto bottomShadow = tintedMediaviewIcon(
        QStringLiteral("shadow_bottom"),
        dpr,
        st::windowShadowFg);
    const auto topHeight = iconPixelHeight(topShadow);
    if (topHeight > 0) {
        p.drawImage(QRect(0, 0, width(), topHeight), topShadow);
    }
    const auto bottomHeight = iconPixelHeight(bottomShadow);
    if (bottomHeight > 0) {
        p.drawImage(
            QRect(0, qMax(0, height() - bottomHeight), width(), bottomHeight),
            bottomShadow);
    }

    auto paintHoverCircle = [&](const QRect &slot) {
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        const auto hoverBg = withOpacity(st::windowShadowFg, st::mediaviewOverBgOpacity);
        p.setBrush(hoverBg);
        const auto radius = st::mediaviewIconOver / 2;
        p.drawEllipse(slot.center(), radius, radius);
    };

    auto paintCloseButton = [&] {
        if (_over == Over::Close) {
            paintHoverCircle(_closeRect);
        }
        const auto opacity = (_over == Over::Close) ? 1.0 : st::mediaviewMaxIconOpacity;
        const auto shadow = tintedMediaviewIcon(
            QStringLiteral("title_viewer_shadow_close"),
            dpr,
            st::windowShadowFg);
        const auto icon = tintedMediaviewIcon(
            QStringLiteral("title_viewer_button_close"),
            dpr,
            st::mediaviewControlFg);
        drawCenteredImage(p, _closeRect, shadow, opacity);
        drawCenteredImage(p, _closeRect, icon, opacity);
    };

    auto paintSaveButton = [&] {
        if (_over == Over::Save) {
            paintHoverCircle(_saveRect);
        }
        const auto opacity = (_over == Over::Save) ? 1.0 : st::mediaviewMaxIconOpacity;
        const auto icon = tintedMediaviewIcon(
            QStringLiteral("download"),
            dpr,
            st::mediaviewControlFg);
        drawCenteredImage(p, _saveRect, icon, opacity);
    };

    const auto hideOverlayMeta = (_isVideo && _fullScreenVideo);
    if (!hideOverlayMeta) {
        paintCloseButton();
        paintSaveButton();

        // Footer (bottom-left): sender/date line.
        const auto footerColor = withOpacity(st::mediaviewControlFg, st::mediaviewNormalIconOpacity);
        p.setPen(footerColor);

        p.setFont(st::normalFont);
        const auto infoY = height() - st::mediaviewTextTop + st::normalFont->ascent;
        auto infoX = st::mediaviewTextLeft;
        if (!_senderName.isEmpty()) {
            p.drawText(infoX, infoY, _senderName);
            infoX += st::normalFont->width(_senderName) + st::mediaviewTextSkip;
        }
        const auto dateText = footerDateText(_timestamp);
        if (!dateText.isEmpty()) {
            p.drawText(infoX, infoY, dateText);
        }

        if (!_caption.isEmpty()) {
            p.setFont(st::normalFont);
            const auto maxHeight = qMax(st::normalFont->height, height() / 4);
            const auto maxWidth = qMax(200, width() - (2 * st::mediaviewControlSize));
            const auto textWidth = qMax(1, maxWidth - 2 * st::mediaviewCaptionPaddingH);
            const auto textHeight = qMax(1, maxHeight - 2 * st::mediaviewCaptionPaddingV);
            const QRect textBounds = QFontMetrics(static_cast<const QFont &>(st::normalFont)).boundingRect(
                QRect(0, 0, textWidth, textHeight),
                Qt::TextWordWrap,
                _caption);
            const auto bubbleWidth = qMin(maxWidth, textBounds.width() + 2 * st::mediaviewCaptionPaddingH);
            const auto bubbleHeight = qMin(maxHeight, textBounds.height() + 2 * st::mediaviewCaptionPaddingV);
            const auto bubbleLeft = (width() - bubbleWidth) / 2;
            const auto bubbleBottom = height() - st::mediaviewHeaderTop - st::mediaviewCaptionMargin;
            const auto bubbleTop = qMax(0, bubbleBottom - bubbleHeight);
            const QRect bubbleRect(bubbleLeft, bubbleTop, bubbleWidth, bubbleHeight);

            {
                PainterHighQualityEnabler hq(p);
                p.setPen(Qt::NoPen);
                p.setBrush(st::mediaviewCaptionBg);
                p.drawRoundedRect(
                    bubbleRect,
                    st::mediaviewCaptionRadius,
                    st::mediaviewCaptionRadius);
            }

            p.setPen(st::mediaviewCaptionFg);
            p.drawText(
                bubbleRect.adjusted(
                    st::mediaviewCaptionPaddingH,
                    st::mediaviewCaptionPaddingV,
                    -st::mediaviewCaptionPaddingH,
                    -st::mediaviewCaptionPaddingV),
                Qt::TextWordWrap,
                _caption);
        }
    }

    if (_isVideo) {
        paintPlaybackControls(p);
    }

    p.restore();
}

void MediaViewOverlay::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) {
        return;
    }

    _lastMousePressPos = e->pos();

    if (_isVideo) {
        if (_seekbarRect.contains(e->pos())) {
            _seekDragging = true;
            _wasPlayingBeforeSeek = (_player && _player->playbackState() == QMediaPlayer::PlayingState);
            if (_wasPlayingBeforeSeek && _player) {
                _player->pause();
            }
            updateSeekFromX(e->pos().x());
            return;
        }
        if (_volumeSliderRect.adjusted(0, -4, 0, 4).contains(e->pos())) {
            _volumeDragging = true;
            updateVolumeFromX(e->pos().x());
            return;
        }
    }
}

void MediaViewOverlay::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) {
        return;
    }

    const auto pos = e->pos();

    if (_seekDragging) {
        _seekDragging = false;
        seekToPercent(_seekDragValue);
        if (_wasPlayingBeforeSeek && _player) {
            _player->play();
        }
        _wasPlayingBeforeSeek = false;
        return;
    }

    if (_volumeDragging) {
        _volumeDragging = false;
        return;
    }

    if ((pos - _lastMousePressPos).manhattanLength() > QApplication::startDragDistance()) {
        return;
    }

    if (_isVideo && _controllerRect.contains(pos)) {
        if (_playPauseRect.contains(pos)) {
            playbackToggle();
            return;
        }
        if (_volumeToggleRect.contains(pos)) {
            toggleMute();
            return;
        }
        if (_volumeSliderRect.adjusted(0, -4, 0, 4).contains(pos)) {
            updateVolumeFromX(pos.x());
            return;
        }
        if (_fullScreenRect.contains(pos)) {
            toggleFullScreenVideo();
            return;
        }
        if (_seekbarRect.contains(pos)) {
            updateSeekFromX(pos.x());
            seekToPercent(_seekDragValue);
            return;
        }
    }

    if (_closeRect.contains(pos) && !(_isVideo && _fullScreenVideo)) {
        closeViewer();
        return;
    }
    if (_saveRect.contains(pos) && !(_isVideo && _fullScreenVideo)) {
        saveImage();
        return;
    }

    if (!_imageRect.contains(pos)
        && !(_isVideo && _controllerRect.contains(pos))) {
        closeViewer();
    }
}

void MediaViewOverlay::mouseMoveEvent(QMouseEvent *e) {
    activateControls();
    if (_seekDragging) {
        updateSeekFromX(e->pos().x());
        return;
    }
    if (_volumeDragging) {
        updateVolumeFromX(e->pos().x());
        return;
    }
    updateHover(e->pos());
}

void MediaViewOverlay::keyPressEvent(QKeyEvent *e) {
    const auto hasFullScreenToggleModifier = e->modifiers().testFlag(Qt::AltModifier)
        || e->modifiers().testFlag(Qt::ControlModifier);

    if (_isVideo) {
        if ((e->key() == Qt::Key_Enter || e->key() == Qt::Key_Return) && hasFullScreenToggleModifier) {
            toggleFullScreenVideo();
            return;
        }
        if (e->key() == Qt::Key_Space) {
            playbackToggle();
            return;
        }
        if (_fullScreenVideo && isRangeKey(e->key())) {
            seekToPercent(qreal(e->key() - Qt::Key_0) / 10.0);
            return;
        }
    }

    switch (e->key()) {
    case Qt::Key_Escape:
        if (_isVideo && _fullScreenVideo) {
            toggleFullScreenVideo();
        } else {
            closeViewer();
        }
        return;
    case Qt::Key_Left:
        if (_isVideo && _fullScreenVideo) {
            seekRelative(-st::mediaviewSeekStepMs);
        }
        return;
    case Qt::Key_Right:
        if (_isVideo && _fullScreenVideo) {
            seekRelative(st::mediaviewSeekStepMs);
        }
        return;
    case Qt::Key_S:
        if (e->modifiers().testFlag(Qt::ControlModifier)) {
            saveImage();
            return;
        }
        break;
    case Qt::Key_C:
        if (e->modifiers().testFlag(Qt::ControlModifier)) {
            copyImageToClipboard();
            return;
        }
        break;
    default:
        break;
    }
    QWidget::keyPressEvent(e);
}

void MediaViewOverlay::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    updateControlGeometry();
    updateImageGeometry();
}

void MediaViewOverlay::updateImageGeometry() {
    auto sourceWidth = 0;
    auto sourceHeight = 0;

    if (_isVideo) {
        if (!_currentFrame.isNull()) {
            sourceWidth = _currentFrame.width();
            sourceHeight = _currentFrame.height();
            // Swap dimensions for 90°/270° rotation so the rect fits the
            // rotated image, not the raw frame.
            if (_videoRotation == 90 || _videoRotation == 270) {
                std::swap(sourceWidth, sourceHeight);
            }
        } else {
            sourceWidth = 1280;
            sourceHeight = 720;
        }
    } else {
        if (_image.isNull()) {
            _imageRect = QRect();
            return;
        }
        sourceWidth = _image.width() / _image.devicePixelRatio();
        sourceHeight = _image.height() / _image.devicePixelRatio();
    }

    if (sourceWidth <= 0 || sourceHeight <= 0 || width() <= 0 || height() <= 0) {
        _imageRect = QRect();
        return;
    }

    auto availableHeight = height();
    if (_isVideo && !_fullScreenVideo) {
        availableHeight -= (st::mediaviewControllerHeight + st::mediaviewControllerBottom);
    }
    availableHeight = qMax(1, availableHeight);

    auto scale = std::min(
        static_cast<double>(width()) / static_cast<double>(sourceWidth),
        static_cast<double>(availableHeight) / static_cast<double>(sourceHeight));
    if (!_isVideo || !_fullScreenVideo) {
        if (scale > 1.0) {
            scale = 1.0;
        }
    }

    const auto w = int(std::round(sourceWidth * scale));
    const auto h = int(std::round(sourceHeight * scale));
    const auto x = (width() - w) / 2;
    const auto y = (availableHeight - h) / 2;
    _imageRect = QRect(x, y, w, h);
}

void MediaViewOverlay::updateControlGeometry() {
    _closeRect = QRect(width() - st::mediaviewIconW, 0, st::mediaviewIconW, st::mediaviewIconH);
    _saveRect = QRect(
        width() - st::mediaviewIconW,
        height() - st::mediaviewIconH,
        st::mediaviewIconW,
        st::mediaviewIconH);

    if (_isVideo) {
        const auto controllerWidth = qMin(st::mediaviewControllerWidth, qMax(220, width() - 24));
        const auto controllerX = (width() - controllerWidth) / 2;
        const auto controllerY = qMax(0, height() - st::mediaviewControllerBottom - st::mediaviewControllerHeight);
        _controllerRect = QRect(
            controllerX,
            controllerY,
            controllerWidth,
            st::mediaviewControllerHeight);

        _playPauseRect = QRect(
            _controllerRect.center().x() - st::mediaviewPlayButtonSize / 2,
            _controllerRect.y() + st::mediaviewPlayButtonTop,
            st::mediaviewPlayButtonSize,
            st::mediaviewPlayButtonSize);
        _volumeToggleRect = QRect(
            _controllerRect.x() + st::mediaviewVolumeLeft,
            _controllerRect.y() + st::mediaviewButtonsTop,
            st::mediaviewSmallButtonSize,
            st::mediaviewSmallButtonSize);
        _volumeSliderRect = QRect(
            _volumeToggleRect.right() + st::mediaviewVolumeSkip,
            _controllerRect.y() + st::mediaviewButtonsTop + 12,
            st::mediaviewVolumeWidth,
            8);
        _fullScreenRect = QRect(
            _controllerRect.right() - st::mediaviewButtonsRight - st::mediaviewSmallButtonSize,
            _controllerRect.y() + st::mediaviewButtonsTop,
            st::mediaviewSmallButtonSize,
            st::mediaviewSmallButtonSize);

        const auto seekLeft = _volumeSliderRect.right() + 30;
        const auto seekRight = _fullScreenRect.left() - 30;
        _seekbarRect = QRect(
            seekLeft,
            _controllerRect.y() + st::mediaviewPlaybackTop,
            qMax(60, seekRight - seekLeft),
            st::mediaviewSeekHandleSize);
    } else {
        _controllerRect = QRect();
        _playPauseRect = QRect();
        _seekbarRect = QRect();
        _volumeToggleRect = QRect();
        _volumeSliderRect = QRect();
        _fullScreenRect = QRect();
    }
}

void MediaViewOverlay::updateHover(const QPoint &pos) {
    auto over = Over::None;

    if (_isVideo && _controllerRect.contains(pos)) {
        if (_playPauseRect.contains(pos)) {
            over = Over::PlayPause;
        } else if (_seekbarRect.contains(pos)) {
            over = Over::Seekbar;
        } else if (_volumeToggleRect.contains(pos)) {
            over = Over::VolumeToggle;
        } else if (_volumeSliderRect.adjusted(0, -4, 0, 4).contains(pos)) {
            over = Over::VolumeSlider;
        } else if (_fullScreenRect.contains(pos)) {
            over = Over::FullScreen;
        }
    } else if (!(_isVideo && _fullScreenVideo)) {
        if (_closeRect.contains(pos)) {
            over = Over::Close;
        } else if (_saveRect.contains(pos) && (_isVideo || !_image.isNull())) {
            over = Over::Save;
        }
    }

    if (_over != over) {
        _over = over;
        setCursor(over == Over::None ? Qt::ArrowCursor : Qt::PointingHandCursor);
        update();
    }
}

QRect MediaViewOverlay::targetScreenGeometry() const {
    const auto screenForWidget = [](QWidget *widget) -> QScreen* {
        if (!widget) {
            return nullptr;
        }
        if (auto *handle = widget->windowHandle()) {
            if (auto *screen = handle->screen()) {
                return screen;
            }
        }
        const auto global = widget->mapToGlobal(widget->rect().center());
        return QGuiApplication::screenAt(global);
    };

    // Use the parent window's frame geometry so the overlay covers the
    // entire app window, not the screen.
    if (auto *host = parentWidget()) {
        if (auto *win = host->window()) {
            return win->frameGeometry();
        }
    }
    if (auto *active = QApplication::activeWindow()) {
        return active->frameGeometry();
    }
    if (auto *focusWindow = QGuiApplication::focusWindow()) {
        if (auto *screen = focusWindow->screen()) {
            return screen->availableGeometry();
        }
    }
    if (auto *screen = QGuiApplication::screenAt(QCursor::pos())) {
        return screen->availableGeometry();
    }
    if (auto *screen = QGuiApplication::primaryScreen()) {
        return screen->availableGeometry();
    }
    return {};
}

void MediaViewOverlay::syncToTargetScreenGeometry() {
    const auto geometry = targetScreenGeometry();
    if (geometry.isValid() && geometry != this->geometry()) {
        setGeometry(geometry);
    }
}

void MediaViewOverlay::showOverlay() {
    syncToTargetScreenGeometry();
    if (!geometry().isValid()) {
        resize(st::mediaviewDefaultWidth, st::mediaviewDefaultHeight);
    }

    show();
#ifdef Q_OS_MAC
    Platform::ForceWindowSRGB(this);
#endif
    updateControlGeometry();
    updateImageGeometry();
    raise();
    activateWindow();
    setFocus(Qt::ActiveWindowFocusReason);
    activateControls();

    // On macOS, frameGeometry() may not be finalized until after the
    // first event loop iteration (window decoration metrics). Re-sync
    // geometry deferred so the overlay covers the full window.
    QMetaObject::invokeMethod(this, [this] {
        syncToTargetScreenGeometry();
        updateControlGeometry();
        updateImageGeometry();
        _firstShowSettled = true;
        update();
    }, Qt::QueuedConnection);
}

void MediaViewOverlay::navigateTo(int index) {
    if (index < 0 || index >= _mediaEntries.size()) {
        return;
    }
    _currentIndex = index;
    // New media item → fresh retry budget (loadVideo/loadImage below reuse it via
    // the retry replay path, so the reset must happen here, not in loadVideo).
    _streamRetries = 0;
    _streamRetry.reset();

    const auto &entry = _mediaEntries[index];
    _caption = entry.caption;
    _senderName = entry.senderName;
    _timestamp = entry.timestamp;
    _duration = entry.durationMs;
    _position = 0;
    _fullScreenVideo = false;

    if (entry.type == ContentType::Video) {
        loadVideo(entry.mediaUrl, entry.mediaFilename, entry.mediaMime, entry.durationMs);
    } else {
        loadImage(entry.mediaUrl);
    }

    updateControlGeometry();
    updateImageGeometry();
    update();
    activateControls();
}

void MediaViewOverlay::activateControls() {
    _controlsShown = true;
    _hideTimer->start();
    _controlsAnimation->stop();
    _controlsAnimation->setDuration(st::mediaviewShowDuration);
    _controlsAnimation->setStartValue(_controlsOpacity);
    _controlsAnimation->setEndValue(1.0);
    _controlsAnimation->start();
}

void MediaViewOverlay::hideControls() {
    _controlsShown = false;
    _controlsAnimation->stop();
    _controlsAnimation->setDuration(st::mediaviewHideDuration);
    _controlsAnimation->setStartValue(_controlsOpacity);
    _controlsAnimation->setEndValue(0.0);
    _controlsAnimation->start();
}

void MediaViewOverlay::saveImage() {
    if (_currentIndex < 0 || _currentIndex >= _mediaEntries.size()) {
        return;
    }
    const auto entry = _mediaEntries[_currentIndex];
    const auto sourcePath = currentMediaPath();

    if (_isVideo) {
        if (sourcePath.isEmpty()
            && (!entry.mediaUrl.startsWith(QStringLiteral("mxc://"))
                || !_exportMediaCallback)) {
            return;
        }
        auto suggestedName = QFileInfo(sourcePath).fileName();
        if (suggestedName.isEmpty()) {
            suggestedName = entry.mediaFilename;
        }
        if (suggestedName.isEmpty()) {
            suggestedName = QStringLiteral("video-%1.mp4").arg(QDateTime::currentSecsSinceEpoch());
        }
        const auto targetPath = runSaveDialog(
            tr("Save Video"),
            suggestedName,
            QStringLiteral("Videos (*.mp4 *.mov *.mkv *.webm);;All Files (*)"));
        if (targetPath.isEmpty()) {
            return;
        }
        if (sourcePath.isEmpty()
            && entry.mediaUrl.startsWith(QStringLiteral("mxc://"))
            && _exportMediaCallback) {
            _exportMediaCallback(entry.mediaUrl, targetPath);
            return;
        }
        if (sourcePath != targetPath) {
            QFile::remove(targetPath);
            QFile::copy(sourcePath, targetPath);
        }
        return;
    }

    if (_image.isNull()) {
        return;
    }

    auto suggestedName = QFileInfo(sourcePath).fileName();
    if (suggestedName.isEmpty()) {
        suggestedName = entry.mediaFilename;
    }
    if (suggestedName.isEmpty()) {
        suggestedName = QStringLiteral("image-%1.png").arg(QDateTime::currentSecsSinceEpoch());
    }

    const auto targetPath = runSaveDialog(
        tr("Save Image"),
        suggestedName,
        QStringLiteral("Images (*.png *.jpg *.jpeg *.webp *.bmp);;All Files (*)"));
    if (targetPath.isEmpty()) {
        return;
    }

    if (entry.mediaUrl.startsWith(QStringLiteral("mxc://")) && _exportMediaCallback) {
        _exportMediaCallback(entry.mediaUrl, targetPath);
        return;
    }

    _image.save(targetPath);
}

void MediaViewOverlay::copyImageToClipboard() {
    if (_isVideo || _image.isNull()) {
        return;
    }
    if (auto *clipboard = QGuiApplication::clipboard()) {
        clipboard->setImage(_image);
        ::Ui::ShowToast(tr("Image copied to clipboard"));
    }
}

bool MediaViewOverlay::loadImage(const QString &mediaUrl) {
    _isVideo = false;
    _videoReady = false;
    _videoRotation = 0;
    _currentFrame = QImage();
    _controllerRect = QRect();

    // Leaving video for an image: stop any pending retry wait and its progress poll,
    // and clear the stream-active flag so the poll timer (if running) goes idle.
    abortStreamRetry();
    _videoStreamActive = false;
    if (_progressTimer) {
        _progressTimer->stop();
    }
    _pendingVideoMxc.clear();
    _pendingVideoFilename.clear();
    _pendingVideoMime.clear();

    if (_player) {
        _player->stop();
    }

    if (mediaUrl.startsWith(QStringLiteral("mxc://"))) {
        // Prefer a full-resolution decode (bytes/disk, never the downscaled
        // timeline cache); fall back to loadImage if nothing is available yet.
        auto cached = normalizeForDisplay(MediaCache::loadFullImage(mediaUrl));
        if (cached.isNull()) {
            cached = normalizeForDisplay(MediaCache::loadImage(mediaUrl));
        }
        if (!cached.isNull()) {
            _image = std::move(cached);
            return true;
        }
        requestResolveIfNeeded(mediaUrl);
        _image = QImage();
        return false;
    }

    const auto path = resolvedLocalPath(mediaUrl);
    if (path.isEmpty()) {
        _image = QImage();
        return false;
    }
    QImageReader reader(path);
    reader.setAutoTransform(true);
    auto image = normalizeForDisplay(reader.read());
    if (!image.isNull()) {
        // Convert non-sRGB images to sRGB for correct color management.
        if (image.colorSpace().isValid()
                && image.colorSpace() != QColorSpace(QColorSpace::SRgb)) {
            image.convertToColorSpace(QColorSpace::SRgb);
        }
        _image = std::move(image);
        return true;
    }

    auto fallback = normalizeForDisplay(QImage(path));
    if (!fallback.isNull()) {
        if (fallback.colorSpace().isValid()
                && fallback.colorSpace() != QColorSpace(QColorSpace::SRgb)) {
            fallback.convertToColorSpace(QColorSpace::SRgb);
        }
        _image = std::move(fallback);
        return true;
    }

    _image = QImage();
    return false;
}

bool MediaViewOverlay::loadVideo(const QString &mediaUrl, const QString &filename, const QString &mime, qint64 durationMs) {
    // Stop any pending retry wait (from a previous video or the prior stream). The
    // per-video retry budget (_streamRetries/_streamRetry) is reset in navigateTo,
    // not here, so a retry replay re-entering loadVideo keeps its budget.
    abortStreamRetry();
    _isVideo = true;
    _videoReady = false;
    _videoRotation = 0;
    _image = QImage();
    _currentFrame = QImage();
    _duration = qMax<qint64>(durationMs, 0);
    _position = 0;
    _seekDragging = false;
    _volumeDragging = false;
    _videoStreamActive = false;
    _streamDownloadPending = false;
    // Only a genuinely different video invalidates the container verdict and the
    // byte counts: a retry replay re-enters here for the same mxc, and resetting
    // would blank its download overlay until the next poll.
    if (mediaUrl != _pendingVideoMxc) {
        _streamContainer = VideoContainer::Unknown;
        _streamDownloadedBytes = 0;
        _streamTotalBytes = 0;
    }
    _videoRebuffer.reset();
    _videoBuffering = false;
    _userPausedVideo = false;
    // Remember the source so a streaming failure can fall back to download.
    _pendingVideoMxc = mediaUrl;
    _pendingVideoFilename = filename;
    _pendingVideoMime = mime;
    // Resume position handed off from the inline player (or a prior fullscreen
    // session); both share the proxy cache, so seeking into it is free.
    const auto resume = MediaCache::takePlaybackPosition(mediaUrl);
    _videoStartPositionMs = (resume > 0) ? resume : 0;
    _position = _videoStartPositionMs;

    // Prefer an already fully-downloaded local copy (instant). The thumbnail path
    // fully downloads non-faststart videos (moov at the end → a 2 MB prefix can't
    // decode them) to extract a frame, leaving a complete decrypted file in the
    // cache; playing that avoids re-streaming a video the proxy would otherwise
    // have to fully download again just to reach the trailing moov.
    if (const auto cached = resolvedLocalPath(mediaUrl);
        !cached.isEmpty() && QFileInfo(cached).exists()) {
        return loadVideoLocal(mediaUrl, filename, mime);
    }

    // Stream progressively via the Rust loopback proxy (Range/seek) instead of
    // waiting for a full download. Skipped when the proxy has no URL (no session)
    // or when this session's media backend already proved it can't play the http
    // source; a backend FormatError on a stream falls back from errorOccurred.
    if (_videoStreamUrlCallback && !_streamingUnavailable) {
        const auto streamUrl = _videoStreamUrlCallback(mediaUrl);
        if (!streamUrl.isEmpty()) {
            setupVideoPlayer();
            if (_player) {
                _videoStreamActive = true;
                _streamDownloadPending = true;
                // A verdict persisted by an earlier session (or by this session's
                // thumbnail extraction) lands before a single byte streams.
                if (_streamContainer == VideoContainer::Unknown
                    && _videoStreamContainerCallback) {
                    _streamContainer = _videoStreamContainerCallback(mediaUrl);
                }
                _downloadedFraction = 0.0f; // grows as the proxy downloads
                _player->setSource(QUrl(streamUrl));
                _player->setPosition(_videoStartPositionMs);
                _player->play();
                return true;
            }
        }
    }
    return loadVideoLocal(mediaUrl, filename, mime);
}

bool MediaViewOverlay::loadVideoLocal(const QString &mediaUrl, const QString &filename, const QString &mime) {
    _downloadedFraction = 1.0f; // a local file is fully available → unrestricted seek
    _streamDownloadPending = false; // nothing is downloading through the proxy
    auto path = resolvedLocalPath(mediaUrl);
    auto clearPlayer = [this] {
        if (_player) {
            _player->stop();
            _player->setSource(QUrl());
        }
    };
    if (path.isEmpty()) {
        clearPlayer();
        // Stash the pending resume position across the async resolve round-trip; the
        // mediaResolved -> loadVideo -> takePlaybackPosition path takes it back.
        if (_videoStartPositionMs > 0) {
            MediaCache::setPlaybackPosition(mediaUrl, _videoStartPositionMs);
        }
        requestResolveIfNeeded(mediaUrl);
        return false;
    }
    if (!QFileInfo(path).exists()) {
        clearPlayer();
        return false;
    }

    setupVideoPlayer();
    if (!_player) {
        return false;
    }

    // macOS AVFoundation needs a file extension for codec detection.
    path = MediaCache::resolvedPathForPlayback(path, filename, mime);
    _player->setSource(QUrl::fromLocalFile(path));
    _player->setPosition(_videoStartPositionMs);
    _player->play();
    return true;
}

void MediaViewOverlay::setupVideoPlayer() {
    if (_player) {
        return;
    }

    _player = new QMediaPlayer(this);
    // The loopback stream downloads linearly (the homeserver has no Range
    // support), so a forward seek ahead of the downloaded point blocks until the
    // download catches up. Widen the FFmpeg backend's network read timeout
    // (default 20s, after which ETIMEDOUT is fatal) so such a seek buffers instead
    // of failing, scaling it with the clip length like the inline player (a fixed
    // 60s was too short for long / non-faststart videos). A timeout that still
    // fires is recovered by scheduleStreamRetry(). QPlaybackOptions is Qt 6.10+.
    {
        auto opts = _player->playbackOptions();
        opts.setNetworkTimeout(streamNetworkTimeout(_duration));
        _player->setPlaybackOptions(opts);
    }
    _audioOutput = new QAudioOutput(this);
    _videoSink = new QVideoSink(this);

    _audioOutput->setVolume(_volume);
    _player->setAudioOutput(_audioOutput);
    _player->setVideoOutput(_videoSink);

    connect(_videoSink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
        const auto image = frame.toImage();
        if (image.isNull()) {
            return;
        }
        // Cache rotation angle from the first frame (constant for the stream).
        if (!_videoReady) {
            _videoRotation = static_cast<int>(frame.rotation());
        }
        // Downscale to the on-screen size once, at frame rate, so we retain a small
        // frame and the per-paint blit is ~1:1 (no full-res rescale every repaint,
        // and no 4K-frame retention). For a rotated video the pre-rotation frame
        // maps to the transposed box. The first frame keeps full-res because
        // _imageRect isn't known until updateImageGeometry() runs just below.
        auto target = QSizeF(_imageRect.size()) * devicePixelRatioF();
        if (_videoRotation == 90 || _videoRotation == 270) {
            target.transpose();
        }
        // Shared helper: flattens 10-bit/HDR frames to 8-bit sRGB before the smooth
        // scale (which corrupts those formats), and returns the frame unscaled when
        // the display is >= source, matching the fullscreen full-res path.
        _currentFrame = downscaleVideoFrame(image, target.toSize());
        if (!_videoReady) {
            _videoReady = true;
            // Playback started: the download overlay hands off to the seek bar's
            // buffered sub-bar, however much is still downloading.
            _streamDownloadPending = false;
            // A frame rendered → playback recovered; refresh the errorOccurred replay
            // budget so an unrelated later stall gets its own retries.
            _streamRetries = 0;
            // Streaming produced a frame → the backend CAN stream; never disable
            // streaming for the session on a later (video-specific) failure.
            if (_videoStreamActive) {
                _streamingEverWorked = true;
            }
            updateImageGeometry();
            // Playback has actually started — apply a handed-off resume position
            // now. Seeking during load is ignored on a network stream; seeks only
            // take once frames are flowing (matching the working manual seek).
            if (_videoStartPositionMs > 0 && _player && _player->isSeekable()) {
                _player->setPosition(_videoStartPositionMs);
                _videoStartPositionMs = 0;
            }
        }
        // A frame arrived → not stalled, unless the rebuffer controller is still
        // deliberately holding playback to refill the buffer.
        if (!_videoRebuffer.waiting()) {
            _videoBuffering = false;
        }
        // Repaint just the video area at frame rate; the lower-rate positionChanged
        // handler covers the controls/seekbar with a full update.
        update(_imageRect.isEmpty() ? rect() : _imageRect);
    });

    connect(_player, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
        _position = qMax<qint64>(0, position);
        if (!_seekDragging) {
            update();
        }
    });

    connect(_player, &QMediaPlayer::seekableChanged, this, [this](bool seekable) {
        // Second application point for a handed-off resume position: on a network
        // stream seekability can arrive AFTER the first frame, where the one-shot
        // first-frame seek has already run and would otherwise drop the position.
        if (seekable && _videoReady && _videoStartPositionMs > 0 && _player) {
            _player->setPosition(_videoStartPositionMs);
            _videoStartPositionMs = 0;
        }
    });

    connect(_player, &QMediaPlayer::durationChanged, this, [this](qint64 duration) {
        if (duration > 0) {
            _duration = duration;
            update();
        }
    });

    connect(_player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState) {
        update();
    });

    connect(_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia) {
            _player->setPosition(0);
            _player->pause();
            _position = 0;
            update();
        }
    });

    connect(_player, &QMediaPlayer::errorOccurred,
        this, [this](QMediaPlayer::Error error, const QString &msg) {
        qWarning() << "Fullscreen video playback error:" << error << msg;
        if (_videoStreamActive) {
            _videoStreamActive = false;
            // Preserve the reached position across teardown so neither the retry
            // replay (loadVideo re-reads it via takePlaybackPosition) nor the local
            // fallback (loadVideoLocal seeks _videoStartPositionMs) restarts at 0:00.
            if (_position > 0) {
                _videoStartPositionMs = _position;
                MediaCache::setPlaybackPosition(_pendingVideoMxc, _position);
            }
            // Take the freshest container verdict: the proxy classifies from the head
            // of its download, which may have landed since the last poll. Without this
            // a fast failure would be judged on a stale Unknown.
            if (_streamContainer == VideoContainer::Unknown
                && _videoStreamContainerCallback) {
                _streamContainer = _videoStreamContainerCallback(_pendingVideoMxc);
            }
            // Wait for the download and replay from its cache when that can plausibly
            // help. Bounded per video. See shouldRetryStreamError for the reasoning.
            if (shouldRetryStreamError(
                    error == QMediaPlayer::ResourceError,
                    _videoReady,
                    _streamContainer)
                && _streamRetries < kMaxStreamRetries) {
                ++_streamRetries;
                teardownVideoPlayer();
                scheduleStreamRetry();
                return;
            }
            // Not a transient timeout, or retries exhausted. Disable streaming for
            // the rest of the session ONLY if it has NEVER worked — i.e. the backend
            // genuinely can't play the loopback stream (e.g. macOS AVFoundation,
            // which needs a real file/extension). Once streaming has produced a frame
            // for any video this session, a later failure is video-specific — fall
            // back for just this video and keep streaming enabled for the rest.
            if (!_videoReady && !_streamingEverWorked) {
                _streamingUnavailable = true;
            }
            teardownVideoPlayer();
            loadVideoLocal(_pendingVideoMxc, _pendingVideoFilename, _pendingVideoMime);
            return;
        }
        teardownVideoPlayer();
        _isVideo = false;
        _videoReady = false;
        update();
    });

    // Poll the proxy download progress (~3/s) to draw the buffered bar and bound
    // seeking. For a local file (not stream-active) the fraction stays 1.0.
    if (!_progressTimer) {
        _progressTimer = new QTimer(this);
        _progressTimer->setInterval(300);
        connect(_progressTimer, &QTimer::timeout, this, [this] {
            const float f = (_videoStreamActive && _videoStreamProgressCallback)
                ? _videoStreamProgressCallback(_pendingVideoMxc)
                : 1.0f;
            auto changed = false;
            if (qAbs(f - _downloadedFraction) > 0.0005f) {
                _downloadedFraction = f;
                changed = true;
            }
            if (_videoStreamActive && _videoStreamProgressBytesCallback) {
                quint64 d = 0, t = 0;
                if (_videoStreamProgressBytesCallback(_pendingVideoMxc, d, t)) {
                    _streamDownloadedBytes = d;
                    _streamTotalBytes = t;
                }
            }
            // The proxy classifies the container from the head of its download, so
            // an unknown verdict at loadVideo() usually resolves within a poll or two.
            if (_videoStreamActive && _streamContainer == VideoContainer::Unknown
                && _videoStreamContainerCallback) {
                const auto container = _videoStreamContainerCallback(_pendingVideoMxc);
                if (container != VideoContainer::Unknown) {
                    _streamContainer = container;
                    changed = true;
                }
            }
            // Proactive (re)buffering: pause when the buffered-ahead margin runs
            // low, resume once it has refilled (the controller's hysteresis avoids
            // immediate re-stalls). userPaused is the USER flag — a video the user
            // paused must not be auto-resumed. Skipped during a seek drag, when
            // the player is transiently paused for scrubbing.
            if (_player && !_seekDragging) {
                const auto action = _videoRebuffer.evaluate(
                    _downloadedFraction, _position, _duration, _videoStreamActive,
                    _userPausedVideo, _videoReady);
                if (action == VideoRebufferController::Action::Pause) {
                    _videoBuffering = true;
                    _player->pause();
                    changed = true;
                } else if (action == VideoRebufferController::Action::Play) {
                    _videoBuffering = false;
                    _player->play();
                    changed = true;
                }
            }
            if (changed) {
                update();
            }
        });
    }
    _progressTimer->start();
}

void MediaViewOverlay::teardownVideoPlayer() {
    if (_progressTimer) {
        _progressTimer->stop();
    }
    if (!_player) {
        return;
    }
    _player->stop();
    // deleteLater (NOT delete): teardownVideoPlayer can run from the player's own
    // errorOccurred slot, where deleting it synchronously is a use-after-free
    // (segfault). Detach the audio output first so the deferred destruction can't
    // touch a freed QAudioOutput.
    _player->setAudioOutput(nullptr);
    _player->setVideoOutput(nullptr);
    _player->deleteLater();
    _player = nullptr;
    // Sink is parented to `this`, not _player — disconnect + delete it so a final
    // in-flight frame from the old player can't corrupt _currentFrame.
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
    _videoRebuffer.reset();
    _videoBuffering = false;
    _userPausedVideo = false;
    updateSpinnerTimer(false);
}

void MediaViewOverlay::scheduleStreamRetry() {
    // The player is torn down; show the buffering spinner (paintEvent starts its
    // timer from _videoBuffering) while the background download catches up.
    _videoBuffering = true;
    if (!_retryTimer) {
        _retryTimer = new QTimer(this);
        _retryTimer->setInterval(kRetryPollIntervalMs);
        connect(_retryTimer, &QTimer::timeout, this, [this] { pollStreamRetry(); });
    }
    _streamRetry.beginWait();
    _retryTimer->start();
    update();
}

void MediaViewOverlay::pollStreamRetry() {
    quint64 downloaded = 0, total = 0;
    const bool known = _videoStreamProgressBytesCallback
        && _videoStreamProgressBytesCallback(_pendingVideoMxc, downloaded, total);
    const bool errored = _videoStreamErroredCallback
        && _videoStreamErroredCallback(_pendingVideoMxc);
    // The player is torn down during a wait, so the 300ms progress poll is stopped:
    // keep the download overlay fed from here instead of freezing it.
    if (known && total > 0) {
        _streamDownloadedBytes = downloaded;
        _streamTotalBytes = total;
        const auto fraction = float(qreal(downloaded) / qreal(total));
        if (qAbs(fraction - _downloadedFraction) > 0.0005f) {
            _downloadedFraction = fraction;
            update();
        }
    }
    if (_streamContainer == VideoContainer::Unknown && _videoStreamContainerCallback) {
        _streamContainer = _videoStreamContainerCallback(_pendingVideoMxc);
    }
    switch (_streamRetry.tick(known, downloaded, total, errored)) {
    case VideoStreamRetryController::Action::Replay:
        // Download complete, or dead and needing a restart → re-open. loadVideo
        // prefers a now-complete local copy and restores the saved position.
        abortStreamRetry();
        loadVideo(
            _pendingVideoMxc, _pendingVideoFilename, _pendingVideoMime, _duration);
        break;
    case VideoStreamRetryController::Action::GiveUp:
        // A complete local copy can sometimes play when the http stream can't.
        abortStreamRetry();
        loadVideoLocal(_pendingVideoMxc, _pendingVideoFilename, _pendingVideoMime);
        break;
    case VideoStreamRetryController::Action::Wait:
        break;
    }
}

void MediaViewOverlay::abortStreamRetry() {
    if (_retryTimer) {
        _retryTimer->stop();
    }
}

void MediaViewOverlay::paintVideoSpinner(QPainter &p) {
    if (_imageRect.isEmpty()) {
        return;
    }
    // Wall-clock phase → one full revolution per second, so the arc spins at a
    // steady rate independent of how often the widget actually repaints.
    if (!_spinnerClock.isValid()) {
        _spinnerClock.start();
    }
    const auto phase = (_spinnerClock.elapsed() % 1000) / 1000.0; // 0..1
    const auto startAngleDeg = -phase * 360.0;   // negative → clockwise sweep
    const auto spanDeg = 270.0;

    const auto side = std::min(_imageRect.width(), _imageRect.height());
    const auto diameter = std::clamp(side / 8, 24, 48);
    const auto c = _imageRect.center();
    const QRectF arcRect(
        c.x() - diameter / 2.0,
        c.y() - diameter / 2.0,
        diameter,
        diameter);

    PainterHighQualityEnabler hq(p);
    auto pen = QPen(QColor(255, 255, 255, 200));
    pen.setWidthF(3.0);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    // QPainter angles are in 1/16th of a degree, measured counter-clockwise.
    p.drawArc(
        arcRect,
        static_cast<int>(std::lround(startAngleDeg * 16.0)),
        static_cast<int>(std::lround(spanDeg * 16.0)));
}

void MediaViewOverlay::paintVideoDownloadProgress(QPainter &p) {
    if (_imageRect.isEmpty()) {
        return;
    }
    // Same geometry as the spinner it replaces, so the affordance doesn't jump if
    // the verdict lands a poll late.
    const auto side = std::min(_imageRect.width(), _imageRect.height());
    const auto diameter = std::clamp(side / 8, 24, 48);
    const auto c = _imageRect.center();
    const QRectF arcRect(
        c.x() - diameter / 2.0,
        c.y() - diameter / 2.0,
        diameter,
        diameter);

    PainterHighQualityEnabler hq(p);
    auto pen = QPen(QColor(255, 255, 255, 200));
    pen.setWidthF(3.0);
    pen.setCapStyle(Qt::RoundCap);
    p.setBrush(Qt::NoBrush);

    // Faint full ring for the total, then a clockwise arc from 12 o'clock.
    auto track = pen;
    track.setColor(QColor(255, 255, 255, 60));
    p.setPen(track);
    p.drawEllipse(arcRect);

    const auto progress = std::clamp(_downloadedFraction, 0.0f, 1.0f);
    p.setPen(pen);
    // QPainter angles are 1/16th of a degree, counter-clockwise from 3 o'clock.
    p.drawArc(
        arcRect,
        90 * 16,
        -static_cast<int>(std::lround(double(progress) * 360.0 * 16.0)));

    // "12.4 MB / 700.0 MB" under the ring. The proxy's byte counts are exact; the
    // event's own info.size is often absent for videos, so don't fall back to it.
    if (_streamTotalBytes == 0) {
        return;
    }
    const auto text = formatBytes(_streamDownloadedBytes)
        + QStringLiteral(" / ")
        + formatBytes(_streamTotalBytes);
    p.setPen(QColor(255, 255, 255, 200));
    const auto textRect = QRect(
        _imageRect.left(),
        int(arcRect.bottom()) + 12,
        _imageRect.width(),
        QFontMetrics(p.font()).height());
    p.drawText(textRect, Qt::AlignHCenter | Qt::AlignTop, text);
}

void MediaViewOverlay::updateSpinnerTimer(bool running) {
    if (!running) {
        if (_spinnerTimer) {
            _spinnerTimer->stop();
        }
        return;
    }
    if (!_spinnerClock.isValid()) {
        _spinnerClock.start();
    }
    if (!_spinnerTimer) {
        _spinnerTimer = new QTimer(this);
        _spinnerTimer->setInterval(16); // ~60fps
        connect(_spinnerTimer, &QTimer::timeout, this, [this] { update(); });
    }
    if (!_spinnerTimer->isActive()) {
        _spinnerTimer->start();
    }
}

void MediaViewOverlay::paintPlaybackControls(QPainter &p) {
    if (!_isVideo || _controllerRect.isEmpty()) {
        return;
    }

    PainterHighQualityEnabler hq(p);
    p.setPen(Qt::NoPen);
    p.setBrush(st::mediaviewPlaybackBg);
    p.drawRoundedRect(_controllerRect, st::mediaviewControllerRadius, st::mediaviewControllerRadius);

    const auto iconColorFor = [this](Over over) {
        return (_over == over)
            ? st::mediaviewPlaybackIconFgOver
            : st::mediaviewPlaybackIconFg;
    };

    // Play / Pause.
    p.setBrush(iconColorFor(Over::PlayPause));
    if (_player && _player->playbackState() == QMediaPlayer::PlayingState && !_seekDragging) {
        const auto c = _playPauseRect.center();
        p.drawRoundedRect(QRect(c.x() - 6, c.y() - 8, 4, 16), 1, 1);
        p.drawRoundedRect(QRect(c.x() + 2, c.y() - 8, 4, 16), 1, 1);
    } else {
        QPainterPath tri;
        const auto c = _playPauseRect.center();
        tri.moveTo(c.x() - 5, c.y() - 9);
        tri.lineTo(c.x() - 5, c.y() + 9);
        tri.lineTo(c.x() + 9, c.y());
        tri.closeSubpath();
        p.drawPath(tri);
    }

    // Volume toggle icon.
    p.setBrush(iconColorFor(Over::VolumeToggle));
    p.setPen(Qt::NoPen);
    {
        const auto c = _volumeToggleRect.center();
        QPolygon speaker;
        speaker << QPoint(c.x() - 9, c.y() - 4)
                << QPoint(c.x() - 4, c.y() - 4)
                << QPoint(c.x() + 1, c.y() - 9)
                << QPoint(c.x() + 1, c.y() + 9)
                << QPoint(c.x() - 4, c.y() + 4)
                << QPoint(c.x() - 9, c.y() + 4);
        p.drawPolygon(speaker);

        const auto muted = _volume <= 0.001;
        if (muted) {
            p.setPen(QPen(iconColorFor(Over::VolumeToggle), 2.0, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(c.x() + 5, c.y() - 6, c.x() + 11, c.y() + 6);
        } else {
            p.setPen(QPen(iconColorFor(Over::VolumeToggle), 2.0, Qt::SolidLine, Qt::RoundCap));
            if (_volume > 0.05) {
                p.drawArc(QRect(c.x() + 2, c.y() - 7, 8, 14), -40 * 16, 80 * 16);
            }
            if (_volume > 0.5) {
                p.drawArc(QRect(c.x() + 4, c.y() - 10, 12, 20), -40 * 16, 80 * 16);
            }
        }
    }

    // Volume slider.
    {
        const auto level = std::clamp(_volume, 0.0, 1.0);
        const auto activeW = int(std::round(_volumeSliderRect.width() * level));
        const QRect track(
            _volumeSliderRect.x(),
            _volumeSliderRect.center().y() - (st::mediaviewSeekTrackHeight / 2),
            _volumeSliderRect.width(),
            st::mediaviewSeekTrackHeight);

        p.setPen(Qt::NoPen);
        p.setBrush(_over == Over::VolumeSlider
            ? st::mediaviewPlaybackInactiveOver
            : st::mediaviewPlaybackInactive);
        p.drawRoundedRect(track, st::mediaviewSeekTrackHeight / 2.0, st::mediaviewSeekTrackHeight / 2.0);

        p.setBrush(_over == Over::VolumeSlider
            ? st::mediaviewPlaybackActiveOver
            : st::mediaviewPlaybackActive);
        p.drawRoundedRect(
            QRect(track.x(), track.y(), qMax(0, activeW), track.height()),
            st::mediaviewSeekTrackHeight / 2.0,
            st::mediaviewSeekTrackHeight / 2.0);

        const auto handleX = std::clamp(track.x() + activeW, track.left(), track.right());
        const auto handleColor = _over == Over::VolumeSlider
            ? st::mediaviewPlaybackActiveOver
            : st::mediaviewPlaybackIconFg;
        p.setBrush(handleColor);
        p.drawEllipse(QPoint(handleX, track.center().y()), 4, 4);
    }

    const auto played = _seekDragging
        ? _seekDragValue
        : normalizedPosition(_position, _duration);
    const auto elapsed = _duration > 0
        ? qint64(std::round(played * _duration))
        : _position;
    const auto remaining = qMax<qint64>(0, _duration - elapsed);

    const auto elapsedText = formatPlayTime(elapsed);
    const auto remainingText = QStringLiteral("-") + formatPlayTime(remaining);

    const auto timeFont = st::baseFont(12, true);
    p.setFont(timeFont);
    p.setPen(st::mediaviewPlaybackProgressFg);
    const QFontMetrics fm(timeFont);
    const auto elapsedWidth = fm.horizontalAdvance(elapsedText);
    const auto remainingWidth = fm.horizontalAdvance(remainingText);

    const auto elapsedX = _seekbarRect.left() - st::mediaviewProgressSkip - elapsedWidth;
    const auto remainingX = _seekbarRect.right() + st::mediaviewProgressSkip;
    const auto textY = _controllerRect.y() + st::mediaviewProgressTop + fm.ascent();
    p.drawText(elapsedX, textY, elapsedText);
    p.drawText(remainingX, textY, remainingText);

    // Seekbar track + handle.
    const QRect track(
        _seekbarRect.x(),
        _seekbarRect.center().y() - (st::mediaviewSeekTrackHeight / 2),
        _seekbarRect.width(),
        st::mediaviewSeekTrackHeight);
    const auto playedWidth = int(std::round(track.width() * played));

    p.setPen(Qt::NoPen);
    p.setBrush(_over == Over::Seekbar
        ? st::mediaviewPlaybackInactiveOver
        : st::mediaviewPlaybackInactive);
    p.drawRoundedRect(track, st::mediaviewSeekTrackHeight / 2.0, st::mediaviewSeekTrackHeight / 2.0);

    // Downloaded/buffered portion (streaming only; 1.0 for a local file). Drawn
    // bolder than the track but under the played fill, like a buffer indicator.
    const auto downloaded = std::clamp(_downloadedFraction, 0.0f, 1.0f);
    if (downloaded > 0.0f && downloaded < 0.999f) {
        const auto bufferedWidth = int(std::round(track.width() * downloaded));
        p.setBrush(st::withAlpha(st::mediaviewPlaybackActive, 90));
        p.drawRoundedRect(
            QRect(track.x(), track.y(), qMax(0, bufferedWidth), track.height()),
            st::mediaviewSeekTrackHeight / 2.0,
            st::mediaviewSeekTrackHeight / 2.0);
    }

    p.setBrush(_over == Over::Seekbar
        ? st::mediaviewPlaybackActiveOver
        : st::mediaviewPlaybackActive);
    p.drawRoundedRect(
        QRect(track.x(), track.y(), qMax(0, playedWidth), track.height()),
        st::mediaviewSeekTrackHeight / 2.0,
        st::mediaviewSeekTrackHeight / 2.0);

    const auto handleX = std::clamp(track.x() + playedWidth, track.left(), track.right());
    const auto handleColor = _over == Over::Seekbar
        ? st::mediaviewPlaybackActiveOver
        : st::mediaviewPlaybackIconFg;
    p.setBrush(handleColor);
    p.drawEllipse(
        QPoint(handleX, track.center().y()),
        st::mediaviewSeekHandleSize / 2,
        st::mediaviewSeekHandleSize / 2);

    // Full-screen toggle.
    p.setPen(QPen(iconColorFor(Over::FullScreen), 2.0, Qt::SolidLine, Qt::RoundCap));
    const auto r = _fullScreenRect.adjusted(9, 9, -9, -9);
    if (_fullScreenVideo) {
        p.drawLine(r.left(), r.top() + 3, r.left(), r.top());
        p.drawLine(r.left(), r.top(), r.left() + 3, r.top());

        p.drawLine(r.right() - 3, r.top(), r.right(), r.top());
        p.drawLine(r.right(), r.top(), r.right(), r.top() + 3);

        p.drawLine(r.left(), r.bottom() - 3, r.left(), r.bottom());
        p.drawLine(r.left(), r.bottom(), r.left() + 3, r.bottom());

        p.drawLine(r.right() - 3, r.bottom(), r.right(), r.bottom());
        p.drawLine(r.right(), r.bottom() - 3, r.right(), r.bottom());
    } else {
        p.drawLine(r.left(), r.top() + 5, r.left(), r.top());
        p.drawLine(r.left(), r.top(), r.left() + 5, r.top());

        p.drawLine(r.right() - 5, r.top(), r.right(), r.top());
        p.drawLine(r.right(), r.top(), r.right(), r.top() + 5);

        p.drawLine(r.left(), r.bottom() - 5, r.left(), r.bottom());
        p.drawLine(r.left(), r.bottom(), r.left() + 5, r.bottom());

        p.drawLine(r.right() - 5, r.bottom(), r.right(), r.bottom());
        p.drawLine(r.right(), r.bottom() - 5, r.right(), r.bottom());
    }
}

void MediaViewOverlay::updateSeekFromX(int x) {
    if (_seekbarRect.width() <= 0) {
        _seekDragValue = 0.0;
        return;
    }
    const auto clampedX = std::clamp(x, _seekbarRect.left(), _seekbarRect.right());
    _seekDragValue = qreal(clampedX - _seekbarRect.left()) / qreal(_seekbarRect.width());
    // Bound the drag handle to the downloaded part (streaming); 1.0 for a local file.
    _seekDragValue = std::clamp(_seekDragValue, 0.0,
        double(std::clamp(_downloadedFraction, 0.0f, 1.0f)));
    update();
}

void MediaViewOverlay::updateVolumeFromX(int x) {
    if (_volumeSliderRect.width() <= 0) {
        return;
    }
    const auto clampedX = std::clamp(x, _volumeSliderRect.left(), _volumeSliderRect.right());
    const auto value = qreal(clampedX - _volumeSliderRect.left()) / qreal(_volumeSliderRect.width());
    setVolume(value);
}

void MediaViewOverlay::playbackToggle() {
    if (!_isVideo || !_player) {
        return;
    }
    if (_player->playbackState() == QMediaPlayer::PlayingState) {
        _player->pause();
        // The user took over: record the explicit pause and drop any pending
        // rebuffer wait so the controller won't auto-resume behind their back.
        _userPausedVideo = true;
        _videoRebuffer.reset();
        _videoBuffering = false;
    } else {
        _player->play();
        _userPausedVideo = false;
    }
    activateControls();
    update();
}

void MediaViewOverlay::seekRelative(qint64 deltaMs) {
    if (!_isVideo || !_player) {
        return;
    }
    const auto maxMs = qint64(std::round(
        double(std::clamp(_downloadedFraction, 0.0f, 1.0f)) * _duration));
    const auto target = std::clamp(
        _player->position() + deltaMs, qint64(0), qMax<qint64>(0, maxMs));
    _player->setPosition(target);
    _position = target;
    activateControls();
    update();
}

void MediaViewOverlay::seekToPercent(qreal percent) {
    if (!_isVideo || !_player || _duration <= 0) {
        return;
    }
    // Streaming: only seek within the downloaded part (a local file is 1.0).
    percent = std::clamp(percent, 0.0, double(std::clamp(_downloadedFraction, 0.0f, 1.0f)));
    const auto target = qint64(std::round(percent * _duration));
    _player->setPosition(target);
    _position = target;
    activateControls();
    update();
}

void MediaViewOverlay::setVolume(qreal volume) {
    volume = std::clamp(volume, 0.0, 1.0);
    _volume = volume;
    if (_volume > 0.001) {
        _lastPositiveVolume = _volume;
    }
    if (_audioOutput) {
        _audioOutput->setVolume(_volume);
    }
    activateControls();
    update();
}

void MediaViewOverlay::toggleMute() {
    if (_volume <= 0.001) {
        setVolume(_lastPositiveVolume <= 0.001 ? 0.9 : _lastPositiveVolume);
    } else {
        _lastPositiveVolume = _volume;
        setVolume(0.0);
    }
}

void MediaViewOverlay::toggleFullScreenVideo() {
    if (!_isVideo) {
        return;
    }
    _fullScreenVideo = !_fullScreenVideo;
    updateControlGeometry();
    updateImageGeometry();
    update();
    activateControls();
}

QString MediaViewOverlay::formatPlayTime(qint64 ms) const {
    ms = qMax<qint64>(0, ms);
    const auto totalSeconds = ms / 1000;
    const auto hours = totalSeconds / 3600;
    const auto minutes = (totalSeconds % 3600) / 60;
    const auto seconds = totalSeconds % 60;

    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QChar('0'))
            .arg(seconds, 2, 10, QChar('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes)
        .arg(seconds, 2, 10, QChar('0'));
}

QString MediaViewOverlay::currentMediaPath() const {
    if (_currentIndex < 0 || _currentIndex >= _mediaEntries.size()) {
        return {};
    }
    return resolvedLocalPath(_mediaEntries[_currentIndex].mediaUrl);
}

} // namespace TeleMatrix

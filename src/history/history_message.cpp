// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_message.h"
#include "history_message_layout.h"
#include "bubble_corners_cache.h"
#include "history_large_emoji.h"
#include "history_inline_video.h"
#include "history_video_placeholder.h"
#include "media/history_view_audio.h"
#include "media/history_view_poll.h"
#include "media/video_download_overlay_policy.h"
#include "ui/format_bytes.h"

#include "../protocol/media_cache.h"
#include "ui/empty_userpic.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QTimer>
#include <QFontMetrics>
#include <QHash>
#include <QImage>
#include <QPainterPath>
#include <QPixmap>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTextFormat>
#include <QTextFragment>
#include <QTextLayout>
#include <QRegularExpression>
#include <QStack>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>

#include "ui/painter.h"
#include "ui/text/quote_paint.h"
#include "ui/style/icon_provider.h"
#include "ui/style/runtime_scale.h"
#include "styles/style_chat.h"
#include "styles/style_font_metrics.h"

namespace TeleMatrix {
namespace HistoryMessage {

// OS-level network reachability, pushed in from the network monitor. When false,
// an in-flight upload shows "Waiting for network..." (direct uploads have no
// send-queue retry, so they otherwise stall silently until the request times out).
static bool g_uploadsNetworkOnline = true;

void setUploadsNetworkOnline(bool online) {
    g_uploadsNetworkOnline = online;
}

namespace {

void scheduleContextRepaint(const MessagePaintContext &context) {
    if (!context.paintTarget || context.suppressAnimationScheduling) {
        return;
    }
    const auto rect = context.repaintTargetRect;
    QTimer::singleShot(16, context.paintTarget, [
            w = QPointer<QWidget>(context.paintTarget),
            rect] {
        if (!w) {
            return;
        }
        if (rect.isValid()) {
            w->update(rect);
        } else {
            w->update();
        }
    });
}

// --- Blurhash decoder (MSC2448) ---
// Decodes a blurhash string to a small QImage that can be scaled up as placeholder.

constexpr char kBase83Chars[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz#$%*+,-.:;=?@[]^_{|}~";

[[nodiscard]] int base83Decode(const QByteArray &str, int from, int count) {
    int value = 0;
    for (int i = from; i < from + count && i < str.size(); ++i) {
        const auto *pos = strchr(kBase83Chars, str[i]);
        if (!pos) return 0;
        value = value * 83 + int(pos - kBase83Chars);
    }
    return value;
}

[[nodiscard]] float sRGBToLinear(int v) {
    const float s = v / 255.0f;
    return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
}

[[nodiscard]] int linearToSRGB(float v) {
    const float s = std::max(0.0f, std::min(1.0f, v));
    return int(std::round(
        (s <= 0.0031308f ? s * 12.92f : 1.055f * std::pow(s, 1.0f / 2.4f) - 0.055f) * 255.0f));
}

[[nodiscard]] QImage decodeBlurhash(const QString &hash, int width, int height) {
    const auto bytes = hash.toLatin1();
    if (bytes.size() < 6) return {};

    const int sizeFlag = base83Decode(bytes, 0, 1);
    const int numX = (sizeFlag % 9) + 1;
    const int numY = (sizeFlag / 9) + 1;
    const int expected = 4 + 2 * numX * numY - 2;
    if (bytes.size() < expected) return {};

    const int quantMaxVal = base83Decode(bytes, 1, 1);
    const float maxVal = (quantMaxVal + 1) / 166.0f;

    // Decode colors.
    QVector<std::array<float, 3>> colors(numX * numY);
    {
        const int dcValue = base83Decode(bytes, 2, 4);
        colors[0] = { sRGBToLinear((dcValue >> 16) & 0xFF),
                       sRGBToLinear((dcValue >> 8) & 0xFF),
                       sRGBToLinear(dcValue & 0xFF) };
    }
    for (int i = 1; i < numX * numY; ++i) {
        const int acValue = base83Decode(bytes, 4 + 2 * (i - 1), 2);
        colors[i] = {
            (float(acValue / (19 * 19)) - 9.0f) / 9.0f * maxVal,
            (float((acValue / 19) % 19) - 9.0f) / 9.0f * maxVal,
            (float(acValue % 19) - 9.0f) / 9.0f * maxVal,
        };
    }

    // Render.
    QImage image(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < width; ++x) {
            float r = 0, g = 0, b = 0;
            for (int j = 0; j < numY; ++j) {
                for (int i = 0; i < numX; ++i) {
                    const float basis =
                        std::cos(M_PI * i * x / float(width)) *
                        std::cos(M_PI * j * y / float(height));
                    const auto &c = colors[j * numX + i];
                    r += c[0] * basis;
                    g += c[1] * basis;
                    b += c[2] * basis;
                }
            }
            line[x] = qRgb(linearToSRGB(r), linearToSRGB(g), linearToSRGB(b));
        }
    }
    return image;
}

/// Paint a blurhash or themed-color placeholder for an image/video rect.
/// Returns true if something was painted.
bool paintMediaPlaceholder(
    QPainter &p,
    const QRect &rect,
    const QString &blurhash,
    const QString &cacheKey)
{
    if (!blurhash.isEmpty()) {
        // Check in-memory cache first.
        const auto key = QStringLiteral("blur:") + cacheKey;
        auto cached = MediaCache::loadImage(key);
        if (cached.isNull()) {
            // Decode at 32x24, then upscale with smooth transform to
            // produce a soft Gaussian-like blur effect.
            auto raw = decodeBlurhash(blurhash, 32, 24);
            if (!raw.isNull()) {
                cached = raw.scaled(64, 48, Qt::IgnoreAspectRatio,
                                    Qt::SmoothTransformation);
                MediaCache::insertImage(key, cached);
            }
        }
        if (!cached.isNull()) {
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            p.drawImage(rect, cached);
            return true;
        }
    }
    // Fallback: neutral gray placeholder (avoids the jarring blue flash
    // of st::msgInBgSelected between placeholder and loaded image).
    p.fillRect(rect, st::mediaPlaceholderBg);
    return true;
}

/// Paint a soft, deterministic random-color blur (seeded by `seed`, the event
/// id) into `rect` — the video placeholder when there's no server thumbnail or
/// blurhash, instead of a flat near-black fill. Cached + upscaled like blurhash.
void paintSyntheticBlurPlaceholder(
    QPainter &p,
    const QRect &rect,
    const QString &seed)
{
    const auto key = QStringLiteral("synthblur:") + seed;
    auto cached = MediaCache::loadImage(key);
    if (cached.isNull()) {
        auto raw = syntheticBlurPlaceholder(seed, 32, 24);
        if (!raw.isNull()) {
            cached = raw.scaled(64, 48, Qt::IgnoreAspectRatio,
                                Qt::SmoothTransformation);
            MediaCache::insertImage(key, cached);
        }
    }
    if (!cached.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.drawImage(rect, cached);
    } else {
        p.fillRect(rect, st::historyVideoPlaceholderBg);
    }
}

/// Generate a color for a sender name based on user ID.
/// Char-code-sum hash (matching the Matrix web client) over an 8-color palette.
QColor senderColor(const QString &senderId) {
    const QColor colors[] = {
        st::historyPeer1NameFg, // #c03d33 red
        st::historyPeer2NameFg, // #4fad2d green
        st::historyPeer3NameFg, // #d09306 yellow
        st::historyPeer4NameFg, // #168acd blue
        st::historyPeer5NameFg, // #8544d6 purple
        st::historyPeer6NameFg, // #cd4073 pink
        st::historyPeer7NameFg, // #2996ad sea
        st::historyPeer8NameFg, // #ce671b orange
    };
    uint charCodeSum = 0;
    for (const auto &ch : senderId) {
        charCodeSum += ch.unicode();
    }
    return colors[charCodeSum % 8];
}

/// Format a timestamp into "HH:mm" for display inside the bubble.
/// Cached: avoids QDateTime construction on every paint frame.
QString formatTime(qint64 timestamp) {
    static QHash<qint64, QString> cache;
    if (const auto it = cache.constFind(timestamp); it != cache.cend()) {
        return it.value();
    }
    auto str = QDateTime::fromSecsSinceEpoch(timestamp).toString(u"HH:mm");
    cache.insert(timestamp, str);
    return str;
}

QString formatDuration(quint64 durationMs) {
    const auto totalSec = static_cast<quint64>(durationMs / 1000);
    const auto hours = totalSec / 3600;
    const auto mins = (totalSec % 3600) / 60;
    const auto secs = totalSec % 60;
    if (hours > 0) {
        // H:MM:SS for anything an hour or longer (minutes zero-padded).
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(mins, 2, 10, QChar('0'))
            .arg(secs, 2, 10, QChar('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(mins)
        .arg(secs, 2, 10, QChar('0'));
}

/// Returns true if this file item has an audio MIME type or audio file extension.
static bool isAudioFile(const TimelineItem &item) {
    if (mediaMime(item).startsWith(QStringLiteral("audio/"))) return true;
    static const QStringList audioExts = {
        QStringLiteral("mp3"), QStringLiteral("m4a"), QStringLiteral("ogg"),
        QStringLiteral("opus"), QStringLiteral("wav"), QStringLiteral("flac"),
        QStringLiteral("aac")
    };
    const auto ext = QFileInfo(mediaFilename(item)).suffix().toLower();
    return audioExts.contains(ext);
}

/// Compose "Forwarded from {name}" text for the forwarded header.
QString forwardedText(const TimelineItem &item) {
    return QCoreApplication::translate("HistoryMessage", "Forwarded from %1")
        .arg(forwardedSenderName(item));
}

/// Height of the "Forwarded from" header (0 if not forwarded).
/// 1 or 2 lines of st::msgServiceFont.
int forwardedHeaderHeight(const TimelineItem &item, int bubbleInnerWidth) {
    if (forwardedSenderName(item).isEmpty()) {
        return 0;
    }
    const auto text = forwardedText(item);
    const auto &fm = st::fontMetrics(st::msgServiceFont);
    const auto textWidth = fm.horizontalAdvance(text);
    const auto lines = (textWidth > bubbleInnerWidth) ? 2 : 1;
    return lines * st::msgServiceFont->height;
}

constexpr auto kFullArcLength = 360 * 16;
constexpr auto kQuarterArcLength = kFullArcLength / 4;
constexpr auto kMinArcLength = kFullArcLength / 360;
constexpr auto kAlmostFullArcLength = kFullArcLength - kMinArcLength;
constexpr auto kUploadFinishedProgress = 0.999;

[[nodiscard]] bool hasRemoteMediaSource(const TimelineItem &item) {
    const auto isRemoteMxc = [](const QString &url) {
        return url.startsWith(QStringLiteral("mxc://"))
            && !url.startsWith(QStringLiteral("mxc://send-queue.localhost/"));
    };
    return isRemoteMxc(mediaUrl(item)) || isRemoteMxc(mediaThumbUrl(item));
}

bool showUploadOverlay(const TimelineItem &item) {
    if (item.delivery.sendState != SendState::Sending || !item.delivery.outgoing) {
        return false;
    }
    // Show overlay for any Sending item with a local send-queue URL,
    // regardless of progress value. The overlay disappears when
    // sendState transitions away from Sending (to Sent or Failed).
    return !hasRemoteMediaSource(item);
}

qreal uploadAnimationPhase() {
    const auto period = qMax(1, st::radialPeriod);
    const auto now = QDateTime::currentMSecsSinceEpoch();
    return qreal(now % period) / qreal(period);
}

// Phase 0..1 within the link-preview image glow cycle. Time-based and
// self-driving (like uploadAnimationPhase); scheduleContextRepaint keeps the
// row repainting while the image is still downloading.
[[nodiscard]] qreal linkImageGlowPhase() {
    constexpr qint64 kCycleMs = 1400; // breathe period (tunable)
    const auto now = QDateTime::currentMSecsSinceEpoch();
    return qreal(now % kCycleMs) / qreal(kCycleMs);
}

// Pulsing "glow" placeholder for a still-loading OG-card image: a steady neutral
// base with a soft highlight whose opacity breathes in and out (no moving
// parts). `radius` rounds the clip (0 = square, used for large media).
void paintLinkImageGlow(
        QPainter &p,
        const QRect &rect,
        int radius,
        QColor base,
        qreal phase) {
    if (rect.isEmpty()) {
        return;
    }
    p.save();
    QPainterPath clip;
    if (radius > 0) {
        clip.addRoundedRect(QRectF(rect), radius, radius);
    } else {
        clip.addRect(QRectF(rect));
    }
    p.setClipPath(clip);

    p.fillRect(rect, base); // steady neutral base

    // Breathing glow: a soft highlight whose opacity oscillates dim<->bright.
    constexpr qreal kPi = 3.14159265358979323846;
    const auto t = 0.5 - 0.5 * std::cos(phase * 2.0 * kPi); // 0 -> 1 -> 0 over the cycle
    auto highlight = base.lighter(150);
    highlight.setAlpha(int(30.0 + t * 120.0)); // ~0.12..0.59 opacity pulse
    p.fillRect(rect, highlight);

    p.restore();
}

void paintCenteredChatIcon(
    QPainter &p,
    const QRect &rect,
    const QString &name,
    [[maybe_unused]] qreal dpr,
    const QColor &color,
    qreal opacity = 1.0)
{
    const auto icon = TeleMatrix::Style::IconProvider::tintedIcon(QStringLiteral(":/telematrix/icons/chat/"), name, color);
    if (icon.isNull()) {
        return;
    }
    const auto iconW = int(icon.width() / icon.devicePixelRatio());
    const auto iconH = int(icon.height() / icon.devicePixelRatio());
    const auto x = rect.x() + (rect.width() - iconW) / 2;
    const auto y = rect.y() + (rect.height() - iconH) / 2;
    const auto previousOpacity = p.opacity();
    p.setOpacity(previousOpacity * opacity);
    p.drawImage(QPoint(x, y), icon);
    p.setOpacity(previousOpacity);
}

QPixmap blurredUploadPixmap(const QPixmap &source, const QString &cacheKeyBase) {
    static QHash<QString, QPixmap> cache;
    const auto key = cacheKeyBase
        + QLatin1Char('|')
        + QString::number(source.width())
        + QLatin1Char('x')
        + QString::number(source.height())
        + QLatin1Char('|')
        + QString::number(source.devicePixelRatio(), 'f', 1);
    if (const auto i = cache.constFind(key); i != cache.cend()) {
        return i.value();
    }

    auto image = source.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        return {};
    }

    const auto firstPass = QSize(
        qMax(1, image.width() / 12),
        qMax(1, image.height() / 12));
    image = image.scaled(firstPass, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .scaled(image.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    const auto secondPass = QSize(
        qMax(1, image.width() / 8),
        qMax(1, image.height() / 8));
    image = image.scaled(secondPass, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .scaled(image.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    auto result = QPixmap::fromImage(image);
    result.setDevicePixelRatio(source.devicePixelRatio());
    if (cache.size() > 64) {
        cache.clear();
    }
    cache.insert(key, result);
    return result;
}

void paintDocumentIcon(QPainter &p, const QRect &circle, bool isOutgoing, qreal dpr) {
    const auto color = isOutgoing ? st::historyFileOutIconFg : st::historyFileInIconFg;
    paintCenteredChatIcon(
        p,
        circle,
        QStringLiteral("history_file_document"),
        dpr,
        color);
}

void paintUploadOverlay(
    QPainter &p,
    const QRect &buttonRect,
    double uploadProgress,
    qreal dpr,
    const QColor &iconColor,
    const QColor *backgroundFill)
{
    PainterHighQualityEnabler hq(p);
    if (backgroundFill) {
        p.setPen(Qt::NoPen);
        p.setBrush(*backgroundFill);
        p.drawEllipse(buttonRect);
    }

    paintCenteredChatIcon(
        p,
        buttonRect,
        QStringLiteral("history_file_cancel"),
        dpr,
        iconColor);

    const auto progress = (uploadProgress >= 0.0)
        ? qBound(0.0, uploadProgress, 1.0)
        : -1.0;
    const auto arcRect = QRectF(buttonRect).adjusted(
        st::uploadRadialLine,
        st::uploadRadialLine,
        -st::uploadRadialLine,
        -st::uploadRadialLine);

    QPen arcPen(iconColor);
    arcPen.setWidthF(st::uploadRadialLine);
    arcPen.setCapStyle(Qt::RoundCap);
    p.setPen(arcPen);

    int arcLength;
    int arcFrom;
    if (progress >= 0.0) {
        arcLength = -qRound(progress * kFullArcLength);
        arcFrom = kQuarterArcLength;
    } else {
        arcLength = -(kFullArcLength / 4);
        // Rotate clockwise (subtract), matching paintLoadingSpinner/Arc.
        arcFrom = kQuarterArcLength
            - qRound(uploadAnimationPhase() * kFullArcLength);
    }
    p.drawArc(arcRect, arcFrom, arcLength);
}

void paintProgressArc(
    QPainter &p,
    const QRect &buttonRect,
    double progress,
    const QColor &iconColor)
{
    PainterHighQualityEnabler hq(p);
    const auto arcRect = QRectF(buttonRect).adjusted(
        st::uploadRadialLine,
        st::uploadRadialLine,
        -st::uploadRadialLine,
        -st::uploadRadialLine);

    QPen arcPen(iconColor);
    arcPen.setWidthF(st::uploadRadialLine);
    arcPen.setCapStyle(Qt::RoundCap);
    p.setPen(arcPen);

    const auto bounded = qBound(0.0, progress, 1.0);
    const auto arcLength = -qRound(bounded * kFullArcLength);
    p.drawArc(arcRect, kQuarterArcLength, arcLength);
}

struct DownloadVisualState {
    bool active = false;
    bool determinate = false;
    bool decrypting = false;
    quint64 receivedBytes = 0;
    quint64 totalBytes = 0;
    double progress = 0.0;
};

DownloadVisualState downloadVisualState(const TimelineItem &item) {
    DownloadVisualState result;
    const auto url = mediaUrl(item);
    const auto unresolvedMxc = !url.isEmpty()
        && url.startsWith(QStringLiteral("mxc://"))
        && !MediaCache::isResolved(url);
    if (!unresolvedMxc || !MediaCache::isRequested(url)) {
        return result;
    }

    result.active = true;
    if (!MediaCache::hasDownloadState(url)) {
        return result;
    }

    const auto state = MediaCache::downloadState(url);
    result.decrypting = (state.phase == MediaCache::DownloadPhase::Decrypting);
    result.receivedBytes = state.receivedBytes;
    result.totalBytes = state.totalBytes;
    if (result.decrypting) {
        result.determinate = (result.totalBytes > 0);
        result.progress = result.determinate ? 1.0 : 0.0;
    } else if (result.totalBytes > 0) {
        result.determinate = true;
        result.progress = qBound(
            0.0,
            static_cast<double>(result.receivedBytes)
                / static_cast<double>(result.totalBytes),
            1.0);
    }
    return result;
}

QString downloadStatusText(const TimelineItem &item, const DownloadVisualState &state) {
    if (state.decrypting) {
        return QCoreApplication::translate("HistoryMessage", "Decrypting...");
    }
    if (state.determinate && state.totalBytes > 0) {
        return formatBytes(state.receivedBytes)
            + QStringLiteral(" / ")
            + formatBytes(state.totalBytes);
    }
    if (state.receivedBytes > 0) {
        return formatBytes(state.receivedBytes)
            + QCoreApplication::translate("HistoryMessage", " · Downloading");
    }
    const auto size = mediaSize(item);
    if (size > 0) {
        return formatBytes(size)
            + QCoreApplication::translate("HistoryMessage", " · Downloading");
    }
    return QCoreApplication::translate("HistoryMessage", "Downloading...");
}

QString fileIdleStatusText(const TimelineItem &item) {
    const auto url = mediaUrl(item);
    const auto size = mediaSize(item);
    if (!url.isEmpty()
        && url.startsWith(QStringLiteral("mxc://"))
        && !MediaCache::isResolved(url)) {
        if (size == 0) {
            return QCoreApplication::translate("HistoryMessage", "Download");
        }
        return formatBytes(size)
            + QCoreApplication::translate("HistoryMessage", " · Download");
    }
    return formatBytes(size);
}

/// Spinning preloader for images being downloaded (no cross icon).
/// Spins clockwise from 12 o'clock.
void paintLoadingSpinner(
    QPainter &p,
    const QRect &buttonRect,
    const QColor &iconColor)
{
    PainterHighQualityEnabler hq(p);

    // Semi-transparent circle background.
    p.setPen(Qt::NoPen);
    p.setBrush(st::historyMediaOverlayBg);
    p.drawEllipse(buttonRect);

    // Spinning clockwise quarter-arc starting from 12 o'clock (top).
    // Qt arc angles: 0 = 3 o'clock, 90*16 = 12 o'clock.
    // Clockwise = negative arc length in Qt's coordinate system.
    const auto phase = uploadAnimationPhase(); // 0..1
    const auto arcLength = -(kFullArcLength / 4); // quarter arc, clockwise
    // Start at 12 o'clock, rotate clockwise with time.
    const auto arcFrom = kQuarterArcLength
        - qRound(phase * kFullArcLength);
    const auto arcRect = QRectF(buttonRect).adjusted(
        st::uploadRadialLine,
        st::uploadRadialLine,
        -st::uploadRadialLine,
        -st::uploadRadialLine);

    QPen arcPen(iconColor);
    arcPen.setWidthF(st::uploadRadialLine);
    arcPen.setCapStyle(Qt::RoundCap);
    p.setPen(arcPen);
    p.drawArc(arcRect, arcFrom, arcLength);
}

void paintLoadingArc(
    QPainter &p,
    const QRect &buttonRect,
    const QColor &iconColor)
{
    PainterHighQualityEnabler hq(p);

    const auto phase = uploadAnimationPhase();
    const auto arcLength = -(kFullArcLength / 4);
    const auto arcFrom = kQuarterArcLength
        - qRound(phase * kFullArcLength);
    const auto arcRect = QRectF(buttonRect).adjusted(
        st::uploadRadialLine,
        st::uploadRadialLine,
        -st::uploadRadialLine,
        -st::uploadRadialLine);

    QPen arcPen(iconColor);
    arcPen.setWidthF(st::uploadRadialLine);
    arcPen.setCapStyle(Qt::RoundCap);
    p.setPen(arcPen);
    p.drawArc(arcRect, arcFrom, arcLength);
}

QRect uploadOverlayButtonRect(const QRect &mediaRect) {
    const auto circleSize = st::uploadRadialSize;
    return QRect(
        mediaRect.x() + (mediaRect.width() - circleSize) / 2,
        mediaRect.y() + (mediaRect.height() - circleSize) / 2,
        circleSize,
        circleSize);
}

void paintVideoDownloadOverlay(
        QPainter &p,
        const QRect &videoRect,
        const TimelineItem &item,
        const DownloadVisualState &state,
        const MessagePaintContext &context,
        qreal dpr) {
    const auto buttonRect = uploadOverlayButtonRect(videoRect);
    {
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(st::historyMediaOverlayBg);
        p.drawEllipse(buttonRect);
    }

    paintCenteredChatIcon(
        p,
        buttonRect,
        QStringLiteral("history_file_cancel"),
        dpr,
        st::historyIconFgInverted);

    if (state.determinate) {
        paintProgressArc(
            p,
            buttonRect,
            state.progress,
            st::historyIconFgInverted);
    } else {
        paintLoadingArc(p, buttonRect, st::historyIconFgInverted);
        scheduleContextRepaint(context);
    }

    const auto status = downloadStatusText(item, state);
    if (status.isEmpty()) {
        return;
    }

    p.setFont(st::msgDateFont);
    const auto &fm = st::fontMetrics(st::msgDateFont);
    const auto badgeMaxWidth = qMax(
        1,
        videoRect.width() - 2 * st::msgDateImgDelta);
    const auto textMaxWidth = qMax(
        1,
        badgeMaxWidth - 2 * st::msgDateImgPadding.x());
    const auto text = fm.elidedText(status, Qt::ElideRight, textMaxWidth);
    const auto badgeWidth = qMin(
        badgeMaxWidth,
        fm.horizontalAdvance(text) + 2 * st::msgDateImgPadding.x());
    const auto badgeHeight = fm.height() + 2 * st::msgDateImgPadding.y();
    const auto minY = videoRect.top() + st::msgDateImgDelta;
    const auto maxY = videoRect.bottom() - st::msgDateImgDelta - badgeHeight;
    auto badgeY = buttonRect.bottom() + st::msgDateImgDelta;
    if (badgeY > maxY) {
        badgeY = buttonRect.top() - st::msgDateImgDelta - badgeHeight;
    }
    if (maxY >= minY) {
        badgeY = qBound(minY, badgeY, maxY);
    } else {
        badgeY = minY;
    }

    const QRect badgeRect(
        videoRect.left() + (videoRect.width() - badgeWidth) / 2,
        badgeY,
        badgeWidth,
        badgeHeight);
    {
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(st::msgDateImgBg);
        p.drawRoundedRect(badgeRect, badgeHeight / 2.0, badgeHeight / 2.0);
    }
    p.setPen(st::msgDateImgFg);
    p.drawText(
        badgeRect.adjusted(
            st::msgDateImgPadding.x(),
            st::msgDateImgPadding.y(),
            -st::msgDateImgPadding.x(),
            -st::msgDateImgPadding.y()),
        Qt::AlignCenter,
        text);
}

// Text-only badge shown in place of the preloader when inline playback fails
// terminally (HistoryInlineVideoPlayer::failed()) — a click retries.
void paintVideoErrorOverlay(QPainter &p, const QRect &videoRect) {
    const auto status = QCoreApplication::translate(
        "HistoryMessage", "Can't play this video — click to retry");
    p.setFont(st::msgDateFont);
    const auto &fm = st::fontMetrics(st::msgDateFont);
    const auto badgeMaxWidth = qMax(
        1, videoRect.width() - 2 * st::msgDateImgDelta);
    const auto textMaxWidth = qMax(
        1, badgeMaxWidth - 2 * st::msgDateImgPadding.x());
    const auto text = fm.elidedText(status, Qt::ElideRight, textMaxWidth);
    const auto badgeWidth = qMin(
        badgeMaxWidth,
        fm.horizontalAdvance(text) + 2 * st::msgDateImgPadding.x());
    const auto badgeHeight = fm.height() + 2 * st::msgDateImgPadding.y();
    const QRect badgeRect(
        videoRect.left() + (videoRect.width() - badgeWidth) / 2,
        videoRect.top() + (videoRect.height() - badgeHeight) / 2,
        badgeWidth,
        badgeHeight);
    {
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(st::msgDateImgBg);
        p.drawRoundedRect(badgeRect, badgeHeight / 2.0, badgeHeight / 2.0);
    }
    p.setPen(st::msgDateImgFg);
    p.drawText(
        badgeRect.adjusted(
            st::msgDateImgPadding.x(),
            st::msgDateImgPadding.y(),
            -st::msgDateImgPadding.x(),
            -st::msgDateImgPadding.y()),
        Qt::AlignCenter,
        text);
}

QString uploadStatusText(const TimelineItem &item) {
    // Offline: a direct upload has no send-queue retry and will just stall until
    // the request times out, so name the reason instead of a frozen byte count.
    if (!g_uploadsNetworkOnline) {
        return QCoreApplication::translate("HistoryMessage", "Waiting for network...");
    }
    // Before the upload reports any progress (file is being read/queued; the SDK
    // folds encryption into the upload % once it starts), show a neutral
    // "Preparing…" rather than a static "0 B / total".
    if (item.delivery.uploadProgress < 0.0) {
        return QCoreApplication::translate("HistoryMessage", "Preparing...");
    }
    const auto size = mediaSize(item);
    if (size > 0) {
        const auto progress = qBound(0.0, item.delivery.uploadProgress, 1.0);
        const auto uploaded = quint64(qRound64(progress * double(size)));
        return formatBytes(uploaded)
            + QStringLiteral(" / ")
            + formatBytes(size);
    }
    return QCoreApplication::translate("HistoryMessage", "Sending...");
}

// Byte progress ("X / Y") centered under the cancel button on a thumbnail
// (image/video), giving them the same upload feedback as the file/audio bubble.
void paintUploadStatusBadge(
    QPainter &p,
    const QRect &mediaRect,
    const TimelineItem &item) {
    const auto status = uploadStatusText(item);
    if (status.isEmpty()) {
        return;
    }
    const auto buttonRect = uploadOverlayButtonRect(mediaRect);
    const auto &fm = st::fontMetrics(st::msgDateFont);
    const auto badgeW = fm.horizontalAdvance(status) + 2 * st::msgDateImgPadding.x();
    const auto badgeH = fm.height() + 2 * st::msgDateImgPadding.y();
    const QRect badgeRect(
        mediaRect.center().x() - badgeW / 2,
        buttonRect.bottom() + st::msgDateImgDelta,
        badgeW,
        badgeH);
    {
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(st::msgDateImgBg);
        p.drawRoundedRect(badgeRect, badgeH / 2.0, badgeH / 2.0);
    }
    p.setFont(st::msgDateFont);
    p.setPen(st::historyFileInIconFg);
    p.drawText(
        badgeRect.left() + st::msgDateImgPadding.x(),
        badgeRect.top() + st::msgDateImgPadding.y() + fm.ascent(),
        status);
}

constexpr int kEditedInfoSkip = 4;
constexpr int kPinInfoSkip = 3;

const QString &editedLabelText() {
    static const auto kEditedLabel = QCoreApplication::translate("HistoryMessage", "edited");
    return kEditedLabel;
}

struct BottomInfoPalette {
    QColor text;
    QColor pin;
    QColor sent;
    QColor sending;
    QColor failed;
};

BottomInfoPalette normalBottomInfoPalette(bool isOutgoing) {
    return {
        isOutgoing ? st::msgOutDateFg : st::msgInDateFg,
        isOutgoing ? st::historyOutIconFg : st::historyPinIconFg,
        st::historyOutIconFg,
        isOutgoing ? st::historySendingOutIconFg : st::historySendingInIconFg,
        st::attentionButtonFg,
    };
}

BottomInfoPalette mediaOverlayBottomInfoPalette() {
    return {
        st::msgDateImgFg,
        st::historyIconFgInverted,
        st::historyIconFgInverted,
        st::historySendingInvertedIconFg,
        st::attentionButtonFg,
    };
}

/// Paint the send-state icon (check marks / clock) using actual icon PNGs.
///
/// The icon is positioned at:
///   QPoint(right, firstLineBottom) + (-17, -19)
/// with internal offsets: sent/received point(2,4), sending point(5,5).
///
/// We receive (x, top, fontHeight) from paintBottomInfo where x is
/// the left edge of the kSendStateSpace area and top is the top of the
/// date font line.
void paintSendStateIcon(
    QPainter &p,
    SendState state,
    int x,
    int top,
    int fontHeight,
    [[maybe_unused]] qreal dpr,
    const BottomInfoPalette &palette)
{
    const auto color = (state == SendState::Sending)
        ? palette.sending
        : (state == SendState::Failed)
        ? palette.failed
        : palette.sent;

    if (state == SendState::Failed) {
        // Red filled circle with white "!" inside.
        PainterHighQualityEnabler hq(p);
        const auto circleR = 7.0;
        const auto cx = qreal(x) + qreal(kSendStateSpace) / 2.0;
        const auto cy = qreal(top) + qreal(fontHeight) / 2.0;
        // Red circle.
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(QPointF(cx, cy), circleR, circleR);
        // White exclamation mark.
        const auto markFg = st::historyIconFgInverted;
        p.setPen(QPen(markFg, 1.6, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(cx, cy - 4.0), QPointF(cx, cy + 1.0));
        p.setPen(Qt::NoPen);
        p.setBrush(markFg);
        p.drawEllipse(QPointF(cx, cy + 3.2), 1.0, 1.0);
        return;
    }

    // Icon painted at (right - 17, firstLineBottom - 19)
    // with per-icon internal offset added to get pixel position.
    const auto right = x + kSendStateSpace;
    const auto firstLineBottom = top + fontHeight;

    QString iconName;
    int offsetX, offsetY;
    if (state == SendState::Sending) {
        iconName = QStringLiteral("dialogs_sending");
        offsetX = 5;
        offsetY = 5;
    } else if (state == SendState::Sent) {
        iconName = QStringLiteral("history_sent");
        offsetX = 2;
        offsetY = 4;
    } else {
        iconName = QStringLiteral("history_received");
        offsetX = 2;
        offsetY = 4;
    }

    const auto icon = TeleMatrix::Style::IconProvider::tintedIcon(QStringLiteral(":/telematrix/icons/chat/"), iconName, color);
    if (icon.isNull()) {
        return;
    }
    // historySendStatePosition = point(-17, -19)
    const auto drawX = right - 17 + offsetX;
    const auto drawY = firstLineBottom - 19 + offsetY;
    p.drawImage(QPoint(drawX, drawY), icon);
}

void paintBottomInfo(
    QPainter &p,
    const TimelineItem &item,
    int left,
    int top,
    int baseline,
    const QString &timeStr,
    const QFontMetrics &timeFm,
    qreal dpr,
    const BottomInfoPalette &palette,
    qreal sendingAnimationProgress)
{
    auto x = left;
    p.setFont(st::msgDateFont);
    p.setPen(palette.text);

    if (item.isEdited) {
        const auto &edited = editedLabelText();
        p.drawText(x, baseline, edited);
        x += timeFm.horizontalAdvance(edited) + kEditedInfoSkip;
    }

    if (item.isPinned) {
        const auto pinIcon = TeleMatrix::Style::IconProvider::tintedIcon(QStringLiteral(":/telematrix/icons/menu/"), QStringLiteral("history_pin"), palette.pin);
        if (!pinIcon.isNull()) {
            const auto pinW = int(pinIcon.width() / pinIcon.devicePixelRatio());
            const auto pinH = int(pinIcon.height() / pinIcon.devicePixelRatio());
            p.drawImage(QPoint(x, top + (timeFm.height() - pinH) / 2), pinIcon);
            x += pinW + kPinInfoSkip;
        }
    }

    p.drawText(x, baseline, timeStr);
    x += timeFm.horizontalAdvance(timeStr);

    if (item.delivery.outgoing) {
        paintSendStateIcon(
            p,
            item.delivery.sendState,
            x,
            top,
            timeFm.height(),
            dpr,
            palette);
    }
}

const QString &fastReplyText() {
    static const auto value = QCoreApplication::translate("HistoryMessage", "Reply");
    return value;
}

int fastReplyTextWidth() {
    const auto &fm = st::fontMetrics(st::msgDateFont);
    return fm.horizontalAdvance(fastReplyText());
}

bool hasFastReplyAction(const TimelineItem &item, bool /*showSender*/) {
    // Shown on ALL incoming messages, not just the first in a group.
    return !item.delivery.outgoing
        && contentType(item) != ContentType::Service
        && !item.eventId.isEmpty()
        && !item.delivery.deleted;
}

int senderRowRequiredWidth(const TimelineItem &item, bool reserveFastReply) {
    const auto &nameFm = st::fontMetrics(st::msgNameFont);
    auto width = nameFm.horizontalAdvance(item.sender.name);
    if (reserveFastReply) {
        width += kBubblePaddingH + fastReplyTextWidth();
    }
    return width;
}

QString elidedSenderName(
        const TimelineItem &item,
        int bubbleWidth,
        bool reserveFastReply) {
    const auto &nameFm = st::fontMetrics(st::msgNameFont);
    auto available = bubbleWidth - (2 * kBubblePaddingH);
    if (reserveFastReply) {
        available -= kBubblePaddingH + fastReplyTextWidth();
    }
    return nameFm.elidedText(item.sender.name, Qt::ElideRight, qMax(1, available));
}

// Floating pill button at top-right of bubble.
constexpr int kReplyCornerHeight = 20;
constexpr int kReplyCornerPaddingH = 6;
constexpr int kReplyCornerShadow = 4;
constexpr int kReplyCornerOffsetX = 7;
// Pill center at bubble top border: top = -(height/2) = -10
constexpr int kReplyCornerOffsetY = -(kReplyCornerHeight / 2);

int fastReplyPillWidth() {
    return kReplyCornerPaddingH + fastReplyTextWidth() + kReplyCornerPaddingH;
}

QRect fastReplyActionRect(
        int bubbleLeft,
        int bubbleWidth,
        int /*senderTop*/) {
    // Positioned at top-right of the bubble, slightly overlapping.
    const auto pillW = fastReplyPillWidth() + 2 * kReplyCornerShadow;
    const auto pillH = kReplyCornerHeight + 2 * kReplyCornerShadow;
    const auto x = bubbleLeft + bubbleWidth + kReplyCornerOffsetX - pillW + kReplyCornerShadow;
    const auto y = kReplyCornerOffsetY - kReplyCornerShadow;
    return QRect(x, y, pillW, pillH);
}

void paintFastReplyAction(
        QPainter &p,
        [[maybe_unused]] bool isOut,
        [[maybe_unused]] bool over,
        int bubbleLeft,
        int bubbleWidth,
        int /*senderTop*/) {
    PainterHighQualityEnabler hq(p);

    const auto pillW = fastReplyPillWidth();
    const auto radius = kReplyCornerHeight / 2;
    const auto x = bubbleLeft + bubbleWidth + kReplyCornerOffsetX - pillW;
    const auto y = kReplyCornerOffsetY;
    const QRectF pill(x, y, pillW, kReplyCornerHeight);

    // Shadow.
    {
        auto shadow = pill.adjusted(-1, -1, 1, 1);
        p.setPen(Qt::NoPen);
        p.setBrush(st::historyFastReplyShadowBg);
        p.drawRoundedRect(shadow.adjusted(0, 1, 0, 1), radius + 1, radius + 1);
    }

    // Background (no hover state — kept flat white).
    p.setPen(Qt::NoPen);
    p.setBrush(st::windowBg);
    p.drawRoundedRect(pill, radius, radius);

    // Border.
    p.setPen(QPen(st::historyFastReplyBorderFg, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(pill, radius, radius);

    // Text.
    const auto &fm = st::fontMetrics(st::msgDateFont);
    const auto textY = y + (kReplyCornerHeight - fm.height()) / 2 + fm.ascent();
    p.setFont(st::msgDateFont);
    p.setPen(st::msgInDateFg);
    p.drawText(x + kReplyCornerPaddingH, textY, fastReplyText());
}

int replyPreviewHeight(bool hasThumb = false) {
    // Base reply-preview height metric (32).
    // Keep it as minimum and expand only if active font metrics require more.
    const auto topPadding = st::historyBlockquoteStyle.padding.top();
    const auto bottomPadding = st::historyBlockquoteStyle.padding.bottom();
    const auto minByFont = topPadding
        + st::msgServiceNameFont->height
        + st::msgFont->height
        + bottomPadding;
    if (hasThumb) {
        const auto thumbTotal = topPadding
            + st::historyReplyPreviewMargin.top()
            + st::historyReplyPreview
            + st::historyReplyPreviewMargin.bottom()
            + bottomPadding;
        return qMax(thumbTotal, minByFont);
    }
    return qMax(kReplyPreviewHeight, minByFont);
}

int replyDeletedHeight() {
    // Single line "Deleted message" with padding, vertically centered.
    const auto topPadding = st::historyBlockquoteStyle.padding.top();
    const auto bottomPadding = st::historyBlockquoteStyle.padding.bottom();
    const auto minByFont = topPadding
        + st::msgFont->height
        + bottomPadding;
    return qMax(kReplyPreviewHeight, minByFont);
}

struct ReplyPreviewData {
    QString name;
    QString text;
    QString thumbUrl;      // file path for thumbnail (from target's media)
    bool hasThumb = false;  // true if target is Image/Video with thumbnail
    bool isTextColorized = false; // true if text is a media type label ("Photo", "Video", "File")
    bool deleted = false;   // original message not found (deleted)
};

struct ReplyPreviewText {
    QString text;
    bool colorized = false; // true if text is a media type label
};

ReplyPreviewText previewTextForReplyTarget(const TimelineItem &target) {
    switch (contentType(target)) {
    case ContentType::Image:
        if (!captionText(target).isEmpty()) {
            return { captionText(target), false };
        }
        if (!bodyText(target).isEmpty()) {
            return { bodyText(target), false };
        }
        return { QCoreApplication::translate("HistoryMessage", "Photo"), true };
    case ContentType::File:
        if (!mediaFilename(target).isEmpty()) {
            return { mediaFilename(target), false };
        }
        if (!bodyText(target).isEmpty()) {
            return { bodyText(target), false };
        }
        return { QCoreApplication::translate("HistoryMessage", "File"), true };
    case ContentType::Audio:
        if (!mediaFilename(target).isEmpty()) {
            return { mediaFilename(target), false };
        }
        if (!bodyText(target).isEmpty()) {
            return { bodyText(target), false };
        }
        return { QCoreApplication::translate("HistoryMessage", "Audio"), true };
    case ContentType::Video:
        if (!captionText(target).isEmpty()) {
            return { captionText(target), false };
        }
        if (!bodyText(target).isEmpty()) {
            return { bodyText(target), false };
        }
        return { QCoreApplication::translate("HistoryMessage", "Video"), true };
    case ContentType::Service:
        if (!bodyText(target).isEmpty()) {
            return { bodyText(target), false };
        }
        return { QCoreApplication::translate("HistoryMessage", "Service message"), false };
    case ContentType::Poll:
        if (const auto poll = pollContent(target); poll && !poll->question.isEmpty()) {
            return { poll->question, false };
        }
        if (!bodyText(target).isEmpty()) {
            return { bodyText(target), false };
        }
        return { QCoreApplication::translate("HistoryMessage", "Poll"), true };
    case ContentType::Text:
    default: {
        // Convert Matrix user IDs (@user:server) to just @user.
        static const QRegularExpression matrixUserId(
            QStringLiteral("@([^:]+):[^\\s]+"));

        // Prefer formattedBody (HTML) → plain text.
        const auto formatted = formattedText(target);
        if (!formatted.isEmpty()) {
            QTextDocument doc;
            doc.setHtml(formatted);
            auto plain = doc.toPlainText().simplified();
            // HTML anchor text may still be full Matrix user IDs.
            plain.replace(matrixUserId, QStringLiteral("@\\1"));
            if (!plain.isEmpty()) {
                return { plain, false };
            }
        }
        const auto body = bodyText(target);
        if (!body.isEmpty()) {
            // Strip markdown links: [display](url) → display
            static const QRegularExpression mdLink(
                QStringLiteral("\\[([^\\]]+)\\]\\([^)]+\\)"));
            auto stripped = body;
            stripped.replace(mdLink, QStringLiteral("\\1"));
            // Then convert any remaining Matrix user IDs.
            stripped.replace(matrixUserId, QStringLiteral("@\\1"));
            return { stripped.simplified(), false };
        }
        return { QCoreApplication::translate("HistoryMessage", "Message"), false };
    }
    }
}

ReplyPreviewData resolveReplyData(
    const TimelineItem &item,
    const MessagePaintContext &context) {
    ReplyPreviewData result;
    bool missingFromLoadedSlice = false;
    const auto replyId = replyEventId(item);
    if (replyId.isEmpty()) {
        return result;
    }
    if (context.timelineIndex) {
        if (const auto *parent = context.timelineIndex->find(replyId)) {
            if (parent->delivery.deleted) {
                // Message exists in timeline but is deleted.
                result.deleted = true;
                result.text = QCoreApplication::translate("HistoryMessage", "Deleted message");
                return result;
            }
            result.name = parent->sender.name;
            const auto preview = previewTextForReplyTarget(*parent);
            result.text = preview.text;
            result.isTextColorized = preview.colorized;
            if (isImageMessage(*parent) || isVideoMessage(*parent)) {
                result.thumbUrl = !mediaThumbUrl(*parent).isEmpty()
                    ? mediaThumbUrl(*parent) : mediaUrl(*parent);
                result.hasThumb = !result.thumbUrl.isEmpty();
            }
        } else {
            missingFromLoadedSlice = true;
        }
    }

    if (result.name.isEmpty()
        && result.text.isEmpty()
        && !result.deleted) {
        const auto reply = replyInfo(item);
        if (reply && reply->isDeleted) {
            result.deleted = true;
            result.text = QCoreApplication::translate("HistoryMessage", "Deleted message");
            return result;
        }
        if (reply && (!reply->senderName.isEmpty()
            || !reply->text.isEmpty()
            || reply->hasThumb)) {
            result.name = reply->senderName;
            result.text = reply->text;
            result.thumbUrl = reply->thumbUrl;
            result.hasThumb = reply->hasThumb;
            result.isTextColorized = reply->isTextColorized;
        }
    }

    if (result.name.isEmpty()
        && result.text.isEmpty()
        && !result.deleted
        && (missingFromLoadedSlice
            || (replyInfo(item) && replyInfo(item)->isUnavailable)
            || !context.timelineIndex)) {
        result.text = QStringLiteral("...");
        return result;
    }

    if (result.name.isEmpty()) {
        result.name = QCoreApplication::translate("HistoryMessage", "Reply");
    }
    if (result.text.isEmpty()) {
        result.text = QCoreApplication::translate("HistoryMessage", "Message");
    }
    return result;
}

int replyHeightFor(
        const TimelineItem &item,
        const MessagePaintContext &context) {
    if (!hasReply(item)) {
        return 0;
    }
    const auto data = resolveReplyData(item, context);
    return data.deleted ? replyDeletedHeight() : replyPreviewHeight(data.hasThumb);
}

/// Calculate the maximum bubble width available in the row.
int bubbleMaxWidth(int maxWidth, [[maybe_unused]] bool isOutgoing) {
    const auto reserved = kMarginLeft + kMarginRight + kPhotoSkip;
    return qMax(kMinBubbleWidth, qMin(kMaxBubbleWidth, maxWidth - reserved));
}

/// Calculate the text width available inside the bubble.
int textAvailableWidth(int maxWidth, bool isOutgoing) {
    return qMax(1, bubbleMaxWidth(maxWidth, isOutgoing) - 2 * kBubblePaddingH);
}

int fileBubbleWidth(
    const TimelineItem &item,
    int maxWidth,
    bool isOutgoing) {
    const auto filename = mediaFilename(item);
    const auto fileName = !filename.isEmpty()
        ? filename
        : bodyText(item);
    const auto nameWidth = st::fontMetrics(st::semiboldFont).horizontalAdvance(fileName);
    const auto neededWidth = st::docNameLeft + nameWidth + st::docPaddingRight;
    const auto maxBubble = bubbleMaxWidth(maxWidth, isOutgoing);
    const auto minBubble = qMin(st::docMinWidth, maxBubble);
    return qBound(minBubble, neededWidth, maxBubble);
}

int bubbleLeftFor(
    [[maybe_unused]] const MessagePaintContext &context,
    [[maybe_unused]] bool isOutgoing,
    [[maybe_unused]] int bubbleWidth) {
    return kMarginLeft + kPhotoSkip;
}

BubbleTailSide bubbleTailFor(
    bool isOutgoing,
    bool sameSenderBelow,
    bool skipTail = false) {
    if (sameSenderBelow || skipTail) {
        return BubbleTailSide::None;
    }
    return BubbleTailSide::Left;
}

BubbleCorners bubbleCornersFor(
    bool isOutgoing,
    bool sameSenderAbove,
    bool sameSenderBelow,
    BubbleTailSide tail) {
    BubbleCorners result;
    result.topLeft = sameSenderAbove
        ? st::bubbleRadiusSmall
        : st::bubbleRadiusLarge;
    result.topRight = st::bubbleRadiusLarge;

    result.bottomLeft = (tail == BubbleTailSide::Left)
        ? 0
        : (sameSenderBelow
            ? st::bubbleRadiusSmall
            : st::bubbleRadiusLarge);
    result.bottomRight = st::bubbleRadiusLarge;
    return result;
}

struct MediaClipPlacement {
    bool onTop = true;
    bool onBottom = true;
};

MediaClipPlacement imageMediaPlacement(
        const TimelineItem &item,
        bool showSender,
        bool hasCaption) {
    // Media-in-bubble layout for a photo:
    // - Above block: sender row or forwarded header (for incoming group chat).
    // - Below block: caption text.
    // For media-only messages (no caption), reactions are rendered as
    // a separate strip below the media bubble, so media keeps bottom rounding.
    const auto hasAbove = showSender
        || !forwardedSenderName(item).isEmpty()
        || hasReply(item);
    const auto hasBelow = hasCaption;
    return {
        .onTop = !hasAbove,
        .onBottom = !hasBelow,
    };
}

BubbleCorners adjustedMediaCorners(
        BubbleCorners corners,
        MediaClipPlacement placement) {
    if (!placement.onTop) {
        corners.topLeft = 0;
        corners.topRight = 0;
    }
    if (!placement.onBottom) {
        corners.bottomLeft = 0;
        corners.bottomRight = 0;
    }
    return corners;
}

QPainterPath roundedBubblePath(const QRectF &rect, BubbleCorners corners) {
    QPainterPath path;
    if (rect.isEmpty()) {
        return path;
    }

    const auto l = rect.x();
    const auto t = rect.y();
    const auto r = rect.x() + rect.width();
    const auto b = rect.y() + rect.height();
    const auto limit = std::min(rect.width(), rect.height()) / 2.0;
    corners.topLeft = qBound(0, corners.topLeft, int(limit));
    corners.topRight = qBound(0, corners.topRight, int(limit));
    corners.bottomRight = qBound(0, corners.bottomRight, int(limit));
    corners.bottomLeft = qBound(0, corners.bottomLeft, int(limit));

    path.moveTo(l + corners.topLeft, t);
    path.lineTo(r - corners.topRight, t);
    if (corners.topRight > 0) {
        path.quadTo(r, t, r, t + corners.topRight);
    }
    path.lineTo(r, b - corners.bottomRight);
    if (corners.bottomRight > 0) {
        path.quadTo(r, b, r - corners.bottomRight, b);
    }
    path.lineTo(l + corners.bottomLeft, b);
    if (corners.bottomLeft > 0) {
        path.quadTo(l, b, l, b - corners.bottomLeft);
    }
    path.lineTo(l, t + corners.topLeft);
    if (corners.topLeft > 0) {
        path.quadTo(l, t, l + corners.topLeft, t);
    }
    path.closeSubpath();
    return path;
}

QPainterPath bubbleTailPath(const QRectF &bubbleRect, BubbleTailSide side) {
    QPainterPath path;
    if (side == BubbleTailSide::None || bubbleRect.isEmpty()) {
        return path;
    }

    const auto bottom = bubbleRect.y() + bubbleRect.height();
    const auto top = bottom - kBubbleTailHeight;
    if (side == BubbleTailSide::Left) {
        const auto x = bubbleRect.x();
        path.moveTo(x, top);
        path.quadTo(x - 1.0, bottom - 4.0, x - kBubbleTailWidth, bottom);
        path.lineTo(x, bottom);
    } else {
        const auto x = bubbleRect.x() + bubbleRect.width();
        path.moveTo(x, top);
        path.quadTo(x + 1.0, bottom - 4.0, x + kBubbleTailWidth, bottom);
        path.lineTo(x, bottom);
    }
    path.closeSubpath();
    return path;
}

QPainterPath bubblePath(
    const QRectF &bubbleRect,
    const BubbleCorners &corners,
    BubbleTailSide tail) {
    auto path = roundedBubblePath(bubbleRect, corners);
    // WindingFill unions body+tail; OddEvenFill seams their shared edge.
    path.setFillRule(Qt::WindingFill);
    if (tail != BubbleTailSide::None) {
        path.addPath(bubbleTailPath(bubbleRect, tail));
    }
    return path;
}

void paintBubbleLayer(
    QPainter &p,
    const QRectF &bubbleRect,
    const BubbleCorners &corners,
    BubbleTailSide tail,
    const QColor &color,
    qreal yShift = 0.0,
    // Set only where an OPAQUE bubble body is painted over the same rect right
    // after this layer — it lets the drop shadow clip away the ~90% of itself
    // that the body hides. Must stay false wherever the body is translucent
    // (e.g. the UTD decrypting glow), which would let the shadow show through.
    bool occludedByOpaqueBody = false) {
    if (bubbleRect.isEmpty()) {
        return;
    }
    // Fast path: opaque single-colour bubbles render from cached corner sprites
    // (a few fillRects + blits) instead of an AA path fill. Translucent layers
    // (e.g. the drop shadow) and the rare right-side tail keep the path fill so
    // the non-overlapping tile decomposition never double-blends.
    //
    // MEASURED 2026-07-22: routing translucent layers through the sprite path
    // too (the shadow is alpha 41, so every row was doing a full-bubble AA path
    // fill) does NOT help — `phase rows=` was 2.87ms before and 3.01ms after,
    // i.e. 0.30 -> 0.34ms per row. The sprite win comes from *opaque* fillRects
    // being blit-fast; with alpha every pixel needs read-modify-write blending
    // either way, so splitting one path fill into 7 fills + 4 blits + a cache
    // lookup only adds overhead. Don't retry this without new evidence.
    if (color.alpha() == 255 && tail != BubbleTailSide::Right) {
        const auto rect = qFuzzyIsNull(yShift)
            ? bubbleRect
            : bubbleRect.translated(0.0, yShift);
        BubbleSprites::paintBubble(
            p, rect, corners, tail, color,
            kBubbleTailWidth, kBubbleTailHeight);
        return;
    }
    auto path = bubblePath(bubbleRect, corners, tail);
    if (path.isEmpty()) {
        return;
    }
    if (!qFuzzyIsNull(yShift)) {
        path.translate(0.0, yShift);
    }
    PainterHighQualityEnabler hq(p);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    if (occludedByOpaqueBody && !qFuzzyIsNull(yShift)) {
        // An opaque body is drawn over this same rect immediately after, so the
        // only visible part of the shadow is what yShift exposes: a band along
        // the bottom edge, plus the bottom corner arcs and the tail, whose
        // curves dip below the body's. Clipping to that band is pixel-identical
        // and shrinks the antialiased fill several-fold.
        //
        // Profiled 2026-07-22: this drawPath was the single largest wall-clock
        // item in the row paint — 322 samples blocked in QSemaphore::acquire
        // inside QRasterPaintEngine::fill, i.e. the main thread waiting on Qt's
        // rasterisation of a shape that is ~90% hidden.
        const auto lift = qMax(
            qreal(qMax(corners.bottomLeft, corners.bottomRight)),
            qreal(kBubbleTailHeight));
        const auto top = bubbleRect.bottom() - lift - 1.0;
        const QRectF visible(
            bubbleRect.left() - kBubbleTailWidth - 1.0,
            top,
            bubbleRect.width() + 2.0 * kBubbleTailWidth + 2.0,
            bubbleRect.bottom() + yShift + 1.0 - top);
        p.save();
        p.setClipRect(visible, Qt::IntersectClip);
        p.drawPath(path);
        p.restore();
        return;
    }
    p.drawPath(path);
}

/// Calculate the info width (timestamp + optional checkmarks).
int infoWidth(const TimelineItem &item) {
    static const auto kPinStateSpace = [] {
        auto icon = TeleMatrix::Style::IconProvider::loadScaledMask(QStringLiteral(":/telematrix/icons/menu/"), QStringLiteral("history_pin"));
        if (icon.isNull()) {
            return 13;
        }
        if (icon.devicePixelRatio() <= 0.0) {
            icon.setDevicePixelRatio(1.0);
        }
        return int(icon.width() / icon.devicePixelRatio()) + kPinInfoSkip;
    }();
    const auto &timeFm = st::fontMetrics(st::msgDateFont);
    const auto timeStr = formatTime(item.timestamp);
    auto w = timeFm.horizontalAdvance(timeStr);
    if (item.isEdited) {
        w += timeFm.horizontalAdvance(editedLabelText()) + kEditedInfoSkip;
    }
    if (item.isPinned) {
        w += kPinStateSpace;
    }
    if (item.delivery.outgoing) {
        w += kSendStateSpace;
    }
    return w;
}

/// The "skip block" width — space reserved at the end of text
/// for the inline timestamp.
int skipBlockWidth(const TimelineItem &item) {
    return kDateSpace + infoWidth(item) - kDateDeltaX;
}

/// The "skip block" height — how much vertical space the timestamp
/// needs when it wraps to its own line.
int skipBlockHeight() {
    return st::msgDateFont->height - kDateDeltaY;
}

QString imageCaptionText(const TimelineItem &item) {
    // Only use the explicit media caption, NOT bodyText(item).
    // In Matrix, body is typically the filename for media messages,
    // and treating it as a caption would force a bubble on standalone photos.
    return captionText(item);
}

bool hidesSenderNameForMedia(const TimelineItem &item) {
    switch (contentType(item)) {
    case ContentType::File:
        return captionText(item).simplified().isEmpty();
    case ContentType::Audio:
        return true;
    default:
        return false;
    }
}

bool computeShowSenderName(
        const TimelineItem &item,
        const MessagePaintContext &context) {
    return context.isGroup
        && !context.sameSenderAbove
        && !item.delivery.outgoing
        && !hidesSenderNameForMedia(item);
}

/// Whether a photo/media message needs a bubble.
/// Returns true if the media message should be drawn inside a bubble
/// (has caption text, is a reply, is forwarded, or shows sender name).
bool mediaNeedsBubble(const TimelineItem &item, bool showSender) {
    if (!imageCaptionText(item).isEmpty()) return true;
    if (hasReply(item)) return true;
    if (!forwardedSenderName(item).isEmpty()) return true;
    if (showSender) return true;
    return false;
}

// ─── Photo sizing helpers ────────────────────────────────────────────

QSize nonEmptySize(QSize size) {
    return QSize(qMax(1, size.width()), qMax(1, size.height()));
}

/// Only scale DOWN to fit in box, never up.
QSize downscaledSize(QSize size, QSize box) {
    return nonEmptySize(
        ((size.width() > box.width() || size.height() > box.height())
            ? size.scaled(box, Qt::KeepAspectRatio)
            : size));
}

/// Downscale original to fit the maxMediaSize box.
/// No UI-scale conversion since TeleMatrix doesn't have a UI scale system.
QSize countDesiredMediaSize(QSize original) {
    return downscaledSize(original, { st::maxMediaSize, st::maxMediaSize });
}

QSize countMediaSize(QSize desired, int newWidth) {
    desired = nonEmptySize(desired);
    newWidth = qMax(1, newWidth);
    return (desired.width() <= newWidth)
        ? desired
        : nonEmptySize(
            desired.scaled(newWidth, desired.height(), Qt::KeepAspectRatio));
}

QSize countPhotoMediaSize(QSize desired, int newWidth, int maxWidth) {
    const auto media = countMediaSize(desired, qMin(newWidth, maxWidth));
    return (media.height() <= newWidth)
        ? media
        : nonEmptySize(
            media.scaled(media.width(), newWidth, Qt::KeepAspectRatio));
}

int minWidthForPhotoBubble(const TimelineItem &item, int maxBubbleWidth, bool hasBubble) {
    const auto overlayWidth = infoWidth(item)
        + 2 * (st::msgDateImgDelta + st::msgDateImgPadding.x());
    const auto minPhotoBubble = hasBubble
        ? st::historyPhotoBubbleMinWidth
        : st::minPhotoSize;
    // For non-bubble photos, use minPhotoSize (100) as absolute floor
    // instead of kMinBubbleWidth (160).
    const auto absMin = hasBubble ? kMinBubbleWidth : st::minPhotoSize;
    const auto desiredMin = qMax(minPhotoBubble, overlayWidth);
    return qBound(absMin, desiredMin, qMax(absMin, maxBubbleWidth));
}

/// Whether a frame resize may expand.
/// Returns true if expanding (cover-crop) would show at least ~75% of original.
bool frameResizeMayExpand(
    QSize outer,
    QSize original,
    int minVisibleNom = 3,
    int minVisibleDen = 4)
{
    if (outer.isEmpty() || original.isEmpty()) {
        return false;
    }
    const auto minDim = std::min({
        outer.width(), outer.height(),
        original.width(), original.height()
    });
    // Quick bounds check.
    if (2 * minVisibleNom * minDim
        < 2 * minVisibleDen + minVisibleDen * minDim) {
        return false;
    }
    // DecideFrameResize logic with adjusted numerator/denominator.
    const auto adjNom = minVisibleNom * minDim - minVisibleDen;
    const auto adjDen = minVisibleDen * minDim;
    const auto big = original.scaled(outer, Qt::KeepAspectRatioByExpanding);
    return (big.width() <= outer.width())
        && (big.height() * adjNom <= outer.height() * adjDen);
}

/// Adjust height to reduce cropping.
/// Only expands if a cover-crop resize is acceptable (see frameResizeMayExpand).
int adjustHeightForLessCrop(QSize original, QSize current) {
    if (original.isEmpty() || current.isEmpty()) {
        return current.height();
    }
    if (!frameResizeMayExpand(current, original)) {
        return current.height();
    }
    return qMax(current.height(),
        current.width() * original.height() / original.width());
}

struct PhotoBubbleMetrics {
    int mediaWidth = 0;
    int mediaHeight = 0;
    int bubbleWidth = 0;
};

QSize preferredMediaSize(const TimelineItem &item) {
    auto width = mediaWidth(item);
    auto height = mediaHeight(item);

    // A video's event info dimensions can be the raw (un-rotated) frame size, so
    // a portrait clip reports landscape WxH — the bubble then comes out
    // horizontal and the correctly-oriented thumbnail gets cropped into it. The
    // fetched thumbnail carries the true display orientation, so once it's
    // decoded, if its orientation disagrees with the info dims they are
    // transposed: swap them so the bubble matches. No-op for correctly-tagged
    // videos and until the thumbnail is available.
    if (width > 0 && height > 0 && isVideoMessage(item)
        && !mediaThumbUrl(item).isEmpty()) {
        // Decode-free: use the probe size so layout never sync-decodes. Before
        // the thumb is decoded this returns invalid and the swap waits for the
        // media-resolved relayout (correct orientation lands then).
        const auto thumb = MediaCache::peekImageSize(mediaThumbUrl(item));
        if (thumb.width() > 0 && thumb.height() > 0
            && (width >= height) != (thumb.width() >= thumb.height())) {
            const auto swapTmp = width;
            width = height;
            height = swapTmp;
        }
    }

    if (width > 0 && height > 0) {
        return QSize(width, height);
    }

    QString probeUrl;
    if (isImageMessage(item)) {
        probeUrl = mediaUrl(item);
    } else if (isVideoMessage(item)) {
        probeUrl = !mediaThumbUrl(item).isEmpty() ? mediaThumbUrl(item) : mediaUrl(item);
    }
    if (!probeUrl.isEmpty()) {
        const auto probed = MediaCache::peekImageSize(probeUrl);
        if (!probed.isEmpty()) {
            return probed;
        }
    }

    if (width <= 0 && height <= 0) {
        return QSize(1, 1);
    }
    if (width <= 0) {
        width = height;
    }
    if (height <= 0) {
        height = width;
    }
    return QSize(width, height);
}

/// Two-phase photo sizing:
///   Phase 1 = compute optimal max width / min height
///   Phase 2 = finalize at the actual available width
PhotoBubbleMetrics photoBubbleMetrics(const TimelineItem &item, int maxBubbleWidth, bool showSender = false) {
    PhotoBubbleMetrics result;
    const auto original = preferredMediaSize(item);
    const auto desired = countDesiredMediaSize(original);
    // Use bubble sizing only when caption text needs horizontal room.
    // Forwarding / reply / sender headers go above the image and don't
    // require inflated photo dimensions (TeleMatrix always renders photos
    // inside bubbles, without a bare-vs-bubble distinction).
    const auto bubble = !imageCaptionText(item).isEmpty();

    // ── Phase 1: compute optimal maxWidth / minHeight ──
    const auto minPhotoBubble = bubble
        ? st::historyPhotoBubbleMinWidth
        : st::minPhotoSize;
    const auto minWidth1 = std::clamp(
        minWidthForPhotoBubble(item, st::maxMediaSize, bubble),
        minPhotoBubble,
        st::maxMediaSize);
    const auto maxActualWidth = qMax(desired.width(), minWidth1);
    // Square bias: maxWidth is at least desired.height, preventing
    // extreme portrait images from being too narrow.
    auto optMaxWidth = qMax(maxActualWidth, desired.height());
    auto optMinHeight = qMax(desired.height(), st::minPhotoSize);
    if (bubble) {
        // Cap at msgMaxWidth (430) only when inside a bubble.
        optMaxWidth = qMin(optMaxWidth, st::msgMaxWidth);
        optMinHeight = adjustHeightForLessCrop(original, { optMaxWidth, optMinHeight });
    }

    // ── Phase 2: finalize at actual available width ──
    const auto thumbMaxWidth = qMin(maxBubbleWidth, st::maxMediaSize);
    const auto minWidth2 = std::clamp(
        minWidthForPhotoBubble(item, thumbMaxWidth, bubble),
        qMin(thumbMaxWidth, minPhotoBubble),
        thumbMaxWidth);

    auto pix = countPhotoMediaSize(desired, maxBubbleWidth, optMaxWidth);
    auto width = qMax(pix.width(), minWidth2);
    auto height = qMax(pix.height(), st::minPhotoSize);

    if (bubble) {
        height = adjustHeightForLessCrop(original, { width, height });
    }

    // If at full optimal width, cap height to Phase 1 minimum.
    if (width >= optMaxWidth) {
        height = qMin(height, optMinHeight);
    }

    result.mediaWidth = width;
    result.mediaHeight = height;
    result.bubbleWidth = width;
    return result;
}

// ─── Block-level formatting types and constants ───
// (quote-block and text styling metrics)

enum class BlockType { Pre, Blockquote };

struct BlockRange {
    BlockType type;
    int start;  ///< start position in plain text (inclusive)
    int end;    ///< end position in plain text (exclusive)
};

/// Computed block decoration rect for painting.
struct BlockRenderInfo {
    BlockType type;
    int topY;      ///< top Y relative to text origin
    int bottomY;   ///< bottom Y relative to text origin
};

// Style references for block-level formatting.
const auto &kBqStyle = st::historyBlockquoteStyle;
const auto &kPreStyle = st::historyPreStyle;

/// A link in the formatted text (character range + URL).
struct LinkInfo {
    int start;
    int length;
    QString url;
};

/// Parsed formatted text: plain string + format ranges + links + blocks.
struct FormattedText {
    QString plain;
    QList<QTextLayout::FormatRange> formats;
    QList<LinkInfo> links;
    QList<BlockRange> blocks;
};

// Forward declarations for cache helpers.
FormattedText resolveText(const TimelineItem &item);

struct BlockAwareLayoutResult {
    int textHeight;
    int lastLineWidth;
    int maxLineWidth;
    int lineCount;
    QList<BlockRenderInfo> blockInfos;
    int preMaxNaturalWidth = 0; // Natural (unwrapped) width of widest pre-block line + padding.
};

BlockAwareLayoutResult layoutWithBlocks(
    QTextLayout &layout,
    int availWidth,
    const QList<BlockRange> &blocks);

// ─── Paint cache: avoid QTextDocument::setHtml() and QTextLayout per frame ───
// Keyed by eventId + body hash.  Cleared on setMessages() via clearPaintCache().

/// Cached parsed text: avoids QTextDocument::setHtml() on every paint frame.
struct CachedResolvedText {
    FormattedText resolved;
    QList<QTextLayout::FormatRange> baseFormats; // with mono color applied
    bool formatsIsOut = false;
    uint bodyHash = 0;
};

/// Cached layout metrics: avoids QTextLayout::beginLayout/endLayout per frame.
struct CachedLayoutMetrics {
    int textHeight = 0;
    int lastLineWidth = 0;
    int maxLineWidth = 0;
    QList<BlockRenderInfo> blockInfos;
    int availWidth = -1;
    uint bodyHash = 0;
    int preMaxNaturalWidth = 0; // Natural width of widest pre-block line.
};

static QHash<QString, CachedResolvedText> s_textCache;
// Keyed by (eventId, availWidth) so narrow and wide layouts coexist.
static QHash<QPair<QString, int>, CachedLayoutMetrics> s_metricsCache;
struct CachedDrawableLayout {
    std::shared_ptr<QTextLayout> layout;
    int availWidth = -1;
    uint bodyHash = 0;
    bool formatsIsOut = false;
};
static QHash<QPair<QString, int>, CachedDrawableLayout> s_drawLayoutCache;

/// Cached link preview article thumbnail dimensions from the shrink loop.
/// Avoids running the same iterative sizing loop in both height calc and paint.
struct CachedLinkPreviewLayout {
    int thumbW = -1;
    int thumbH = -1;
    int textHeight = -1; // text-side height from the last loop iteration
    int innerWidth = -1; // cache key: innerWidth used for the computation
    int textWidth = -1;
    uint previewHash = 0;
    bool article = false;
    QStringList siteNameLines;
    QStringList titleLines;
    QStringList descriptionLines;
};
static QHash<QString, CachedLinkPreviewLayout> s_linkPreviewCache;

// Bound the per-message paint caches so scrolling a very long history within one
// room can't grow them without limit (they are otherwise only cleared wholesale on
// room switch via clearPaintCache()). The viewport needs only a few dozen live
// entries, so clearing past a high watermark is safe — visible messages repopulate
// on the next paint — and the high watermark avoids thrashing near the limit.
static constexpr int kPaintCacheMaxEntries = 2048;
template <typename Cache>
static void capPaintCache(Cache &cache) {
    if (cache.size() > kPaintCacheMaxEntries) {
        cache.clear();
    }
}

/// Cached 2-line forwarded-header layout (the only forwarded case that needs a
/// QTextLayout; the 1-line case is a plain elided drawText). Avoids rebuilding the
/// layout every paint. Keyed by eventId; invalidated on width or header-text change.
struct CachedForwardedLayout {
    std::shared_ptr<QTextLayout> layout;
    int innerWidth = -1;
    uint strHash = 0;
};
static QHash<QString, CachedForwardedLayout> s_fwdLayoutCache;

static QTextLayout &cachedForwardedLayout(
        const QString &eventId, const QString &fwdStr, int innerWidth) {
    const auto h = qHash(fwdStr);
    auto it = s_fwdLayoutCache.find(eventId);
    if (it != s_fwdLayoutCache.end()
        && it->innerWidth == innerWidth
        && it->strHash == h
        && it->layout) {
        return *it->layout;
    }
    auto layout = std::make_shared<QTextLayout>(fwdStr, st::msgServiceFont);
    // Same reason as cachedDrawableLayout: retain the shaped glyphs, otherwise
    // this cached layout re-shapes on every paint.
    layout->setCacheEnabled(true);
    layout->beginLayout();
    int y = 0;
    for (int i = 0; i < 2; ++i) {
        QTextLine line = layout->createLine();
        if (!line.isValid()) {
            break;
        }
        line.setLineWidth(innerWidth);
        line.setPosition(QPointF(0, y));
        y += qRound(line.height());
    }
    layout->endLayout();
    CachedForwardedLayout entry;
    entry.layout = std::move(layout);
    entry.innerWidth = innerWidth;
    entry.strHash = h;
    capPaintCache(s_fwdLayoutCache);
    return *s_fwdLayoutCache.insert(eventId, std::move(entry))->layout;
}

/// Body hash for cache validation (detects edits).
static uint bodyHashFor(const TimelineItem &item) {
    return qHash(formattedText(item).isEmpty() ? bodyText(item) : formattedText(item));
}

/// Get cached resolved text + base formats for a message.
/// Populates cache on first access; invalidated by body hash change.
static const CachedResolvedText &cachedText(
        const TimelineItem &item, bool isOut) {
    const auto bh = bodyHashFor(item);
    auto it = s_textCache.find(item.eventId);
    if (it != s_textCache.end()
        && it->bodyHash == bh
        && it->formatsIsOut == isOut) {
        return *it;
    }
    CachedResolvedText entry;
    entry.resolved = resolveText(item);
    entry.bodyHash = bh;
    entry.formatsIsOut = isOut;
    entry.baseFormats = entry.resolved.formats;
    const auto monoColor = isOut ? st::msgOutMonoFg : st::msgInMonoFg;
    const auto linkColor = isOut ? st::historyLinkOutFg : st::historyLinkInFg;
    for (auto &fr : entry.baseFormats) {
        if (fr.format.fontFixedPitch()
            && !fr.format.hasProperty(QTextFormat::ForegroundBrush)) {
            fr.format.setForeground(monoColor);
        }
        // Re-apply link color for in/out context (resolveText always
        // uses historyLinkInFg; override to the correct variant here).
        if (fr.format.isAnchor()
            || fr.format.hasProperty(QTextFormat::AnchorHref)) {
            fr.format.setForeground(linkColor);
        }
    }
    capPaintCache(s_textCache);
    return *s_textCache.insert(item.eventId, std::move(entry));
}

/// Get cached layout metrics for a message at a given width.
/// If cache misses, runs QTextLayout to compute metrics.
static const CachedLayoutMetrics &cachedMetrics(
        const TimelineItem &item,
        bool isOut,
        int availWidth) {
    const auto bh = bodyHashFor(item);
    const auto cacheKey = qMakePair(item.eventId, availWidth);
    auto it = s_metricsCache.find(cacheKey);
    if (it != s_metricsCache.end()
        && it->bodyHash == bh) {
        return *it;
    }
    const auto &ct = cachedText(item, isOut);
    QTextLayout layout(ct.resolved.plain, QFont(st::msgFont));
    {
        QTextOption opt;
        opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(opt);
    }
    if (!ct.baseFormats.isEmpty()) {
        layout.setFormats(ct.baseFormats);
    }
    CachedLayoutMetrics m;
    m.bodyHash = bh;
    m.availWidth = availWidth;
    if (ct.resolved.blocks.isEmpty()) {
        layout.beginLayout();
        m.textHeight = 0;
        m.lastLineWidth = 0;
        m.maxLineWidth = 0;
        while (true) {
            QTextLine line = layout.createLine();
            if (!line.isValid()) break;
            line.setLineWidth(availWidth);
            m.textHeight += line.leading();
            line.setPosition(QPointF(0, m.textHeight));
            m.textHeight += qRound(line.height());
            m.lastLineWidth = int(std::ceil(line.naturalTextWidth()));
            m.maxLineWidth = qMax(m.maxLineWidth, m.lastLineWidth);
        }
        layout.endLayout();
    } else {
        const auto lr = layoutWithBlocks(layout, availWidth, ct.resolved.blocks);
        m.textHeight = lr.textHeight;
        m.lastLineWidth = lr.lastLineWidth;
        m.maxLineWidth = lr.maxLineWidth;
        m.blockInfos = lr.blockInfos;
        m.preMaxNaturalWidth = lr.preMaxNaturalWidth;
    }
    capPaintCache(s_metricsCache);
    return *s_metricsCache.insert(cacheKey, std::move(m));
}

static const QTextLayout &cachedDrawableLayout(
        const TimelineItem &item,
        bool isOut,
        int availWidth) {
    const auto bh = bodyHashFor(item);
    const auto cacheKey = qMakePair(item.eventId, availWidth);
    auto it = s_drawLayoutCache.find(cacheKey);
    if (it != s_drawLayoutCache.end()
        && it->bodyHash == bh
        && it->formatsIsOut == isOut
        && it->layout) {
        return *it->layout;
    }

    const auto &ct = cachedText(item, isOut);
    auto layout = std::make_shared<QTextLayout>(ct.resolved.plain, QFont(st::msgFont));
    // Without this, endLayout() calls QTextEngine::freeMemory() and throws away
    // every shaped glyph — so caching the QTextLayout saves the line breaking but
    // NOT the shaping, and each draw() re-runs HarfBuzz over the whole message.
    // Profiled 2026-07-22: ~40% of the text-draw time in the hottest row was
    // QTextLine::draw_internal -> shapeLine -> shapeTextWithHarfbuzzNG. Costs the
    // retained glyph arrays (a few KB per cached message, capped by
    // kPaintCacheMaxEntries).
    layout->setCacheEnabled(true);
    {
        QTextOption opt;
        opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout->setTextOption(opt);
    }
    if (!ct.baseFormats.isEmpty()) {
        layout->setFormats(ct.baseFormats);
    }
    if (ct.resolved.blocks.isEmpty()) {
        layout->beginLayout();
        auto textHeight = 0;
        while (true) {
            QTextLine line = layout->createLine();
            if (!line.isValid()) {
                break;
            }
            line.setLineWidth(availWidth);
            textHeight += line.leading();
            line.setPosition(QPointF(0, textHeight));
            textHeight += qRound(line.height());
        }
        layout->endLayout();
    } else {
        layoutWithBlocks(*layout, availWidth, ct.resolved.blocks);
    }

    CachedDrawableLayout entry;
    entry.layout = std::move(layout);
    entry.availWidth = availWidth;
    entry.bodyHash = bh;
    entry.formatsIsOut = isOut;
    capPaintCache(s_drawLayoutCache);
    auto inserted = s_drawLayoutCache.insert(cacheKey, std::move(entry));
    return *inserted.value().layout;
}

/// Replace \n with <br/> inside <pre> blocks so QTextDocument
/// preserves line breaks (Qt6 collapses \n in <pre>).
QString preprocessPreNewlines(const QString &html) {
    QString result = html;
    int searchFrom = 0;
    while (true) {
        int preStart = result.indexOf(
            QLatin1String("<pre"), searchFrom, Qt::CaseInsensitive);
        if (preStart < 0) break;
        int preEnd = result.indexOf(
            QLatin1String("</pre>"), preStart, Qt::CaseInsensitive);
        if (preEnd < 0) break;
        for (int i = preStart; i < preEnd; ++i) {
            if (result[i] == u'\n') {
                result.replace(i, 1, QLatin1String("<br/>"));
                preEnd += 4; // "<br/>" is 5 chars replacing 1
            }
        }
        searchFrom = preEnd + 6;
    }
    return result;
}

/// Replace inline <code>...</code> (outside <pre>) with an explicit
/// monospace font span so QTextDocument always applies the right font
/// (its built-in <code> mapping uses system fonts that may not match
/// our detection strings on all platforms).
QString preprocessInlineCode(const QString &html) {
    // Skip if no inline <code> tags.
    if (!html.contains(QLatin1String("<code"))) return html;

    auto monoFamily = st::monospaceFamily();
    monoFamily.replace(u'\'', QStringLiteral("\\'"));
    const auto monoSpan = QStringLiteral(
        "<span style=\"font-family:'%1', monospace;\">").arg(monoFamily);

    QString result;
    result.reserve(html.size() + 64);
    int i = 0;
    bool inPre = false;
    while (i < html.size()) {
        // Track <pre>...</pre> blocks — leave their <code> untouched.
        if (html.mid(i, 4).compare(QLatin1String("<pre"), Qt::CaseInsensitive) == 0) {
            inPre = true;
        }
        if (html.mid(i, 6).compare(QLatin1String("</pre>"), Qt::CaseInsensitive) == 0) {
            inPre = false;
        }
        // Replace inline <code> with explicit monospace family span.
        if (!inPre
            && html.mid(i, 6).compare(QLatin1String("<code>"), Qt::CaseInsensitive) == 0) {
            result += monoSpan;
            i += 6;
            continue;
        }
        if (!inPre
            && html.mid(i, 7).compare(QLatin1String("</code>"), Qt::CaseInsensitive) == 0) {
            result += QLatin1String("</span>");
            i += 7;
            continue;
        }
        result += html[i];
        ++i;
    }
    return result;
}

/// Replace <del>...</del> with <s>...</s> so QTextDocument recognises
/// strikethrough.  Qt's HTML parser handles <s> but not <del>.
QString preprocessStrikethrough(const QString &html) {
    QString result = html;
    result.replace(QLatin1String("<del>"), QLatin1String("<s>"), Qt::CaseInsensitive);
    result.replace(QLatin1String("</del>"), QLatin1String("</s>"), Qt::CaseInsensitive);
    return result;
}

/// Parse HTML into plain text + format ranges + block ranges.
/// Detects <pre> and <blockquote> blocks via QTextDocument format properties.
FormattedText parseFormattedBody(const QString &html) {
    using namespace Qt::Literals::StringLiterals;
    QTextDocument doc;
    doc.setHtml(preprocessStrikethrough(
        preprocessInlineCode(preprocessPreNewlines(html))));

    FormattedText result;
    int pos = 0;
    bool stripNextColon = false;

    for (auto block = doc.begin(); block != doc.end(); block = block.next()) {
        if (pos > 0) {
            // Insert a line separator between QTextDocument blocks.
            // Must use LineSeparator (U+2028) — QTextLayout does NOT
            // treat '\n' (U+000A) as a hard line break, only LineSeparator.
            if (!result.plain.isEmpty()
                && result.plain.back() != u'\n'
                && result.plain.back() != QChar::LineSeparator) {
                result.plain += QChar::LineSeparator;
                ++pos;
            }
        }

        const int blockStart = pos;

        for (auto it = block.begin(); !it.atEnd(); ++it) {
            QTextFragment frag = it.fragment();
            if (!frag.isValid()) continue;

            QString text = frag.text();
            const QTextCharFormat src = frag.charFormat();

            // Strip ": " after a mention replacement (@user replaces "Display Name").
            if (stripNextColon && !src.isAnchor()) {
                if (text.startsWith(u": "_s)) {
                    text = u" "_s + text.mid(2);
                } else if (text.startsWith(u':')) {
                    text = u" "_s + text.mid(1);
                }
                stripNextColon = false;
            }

            // Build a clean format with only meaningful overrides.
            QTextCharFormat fmt;
            bool hasFmt = false;

            if (src.fontWeight() >= QFont::Bold) {
                fmt.setFontWeight(QFont::Bold);
                hasFmt = true;
            }
            if (src.fontItalic()) {
                fmt.setFontItalic(true);
                hasFmt = true;
            }
            if (src.fontStrikeOut()) {
                fmt.setFontStrikeOut(true);
                hasFmt = true;
            }
            // Detect monospace: fontFixedPitch flag (from QTextEdit)
            // or monospace font family (from <code>/<tt> HTML tags).
            {
                bool isMono = src.fontFixedPitch();
                if (!isMono) {
                    const auto families = src.fontFamilies().toStringList();
                    for (const auto &f : families) {
                        const auto lower = f.toLower();
                        if (lower.contains(u"mono"_s)
                            || lower.contains(u"courier"_s)
                            || lower.contains(u"menlo"_s)
                            || lower.contains(u"consolas"_s)
                            || lower.contains(u"cascadia"_s)
                            || lower.contains(u"liberation"_s)) {
                            isMono = true;
                            break;
                        }
                    }
                }
                if (isMono) {
                    fmt.setFontFamilies({ st::monospaceFamily() });
                    fmt.setFontFixedPitch(true);
                    hasFmt = true;
                }
            }
            if (src.isAnchor()) {
                fmt.setForeground(st::historyLinkInFg);
                // No underline by default — added on hover.
                hasFmt = true;

                const auto href = src.anchorHref();
                // Matrix user mentions: convert "Display Name" to "@username"
                // for matrix.to/#/@user:server links.
                auto mentionText = text;
                if (href.contains(u"matrix.to"_s) && href.contains(u"/@"_s)) {
                    const auto hashIdx = href.indexOf(u"/#/"_s);
                    if (hashIdx >= 0) {
                        auto userId = href.mid(hashIdx + 3);
                        const auto qIdx = userId.indexOf(u'?');
                        if (qIdx >= 0) userId = userId.left(qIdx);
                        if (userId.startsWith(u'@')) {
                            const auto colonIdx = userId.indexOf(u':');
                            mentionText = (colonIdx > 0)
                                ? userId.left(colonIdx) : userId;
                        }
                    }
                }

                result.links.append({ pos, int(mentionText.length()), href });
                result.formats.append({ pos, int(mentionText.length()), fmt });
                result.plain += mentionText;
                pos += mentionText.length();
                // Flag: strip leading ": " from the next text fragment.
                if (mentionText != text) {
                    stripNextColon = true;
                }
                continue;
            } else if (src.fontUnderline()) {
                fmt.setFontUnderline(true);
                hasFmt = true;
            }

            if (hasFmt) {
                result.formats.append({ pos, int(text.length()), fmt });
            }
            result.plain += text;
            pos += text.length();
        }

        const int blockEnd = pos;

        // Detect block-level formatting from QTextDocument.
        auto bfmt = block.blockFormat();
        if (bfmt.nonBreakableLines()) {
            result.blocks.append({ BlockType::Pre, blockStart, blockEnd });
        } else if (bfmt.property(QTextFormat::BlockQuoteLevel).toInt() > 0) {
            result.blocks.append({ BlockType::Blockquote, blockStart, blockEnd });
        }
    }

    // Merge adjacent blocks of the same type (e.g. consecutive <pre> lines)
    // so layoutWithBlocks treats them as one code block / blockquote.
    for (int i = result.blocks.size() - 1; i > 0; --i) {
        if (result.blocks[i].type == result.blocks[i - 1].type
            && result.blocks[i].start <= result.blocks[i - 1].end + 2) {
            result.blocks[i - 1].end = result.blocks[i].end;
            result.blocks.removeAt(i);
        }
    }

    return result;
}

// ─── URL detection helpers ───────────────────────────────────────────

/// Punctuation that might end a link.
bool isAlmostLinkEnd(QChar ch) {
    switch (ch.unicode()) {
    case '?': case ',': case '.': case '"': case ':': case '!': case '\'':
        return true;
    default:
        return false;
    }
}

/// Characters that definitely end a link.
bool isLinkEnd(QChar ch) {
    if (ch.isSpace()) return true;
    if (ch.isLowSurrogate() || ch.isHighSurrogate()) return true;
    const auto c = ch.unicode();
    return c == 0
        || (c < 32)
        || (c >= 127 && c < 160)
        || (c >= 8232 && c < 8237)
        || c == QChar::ObjectReplacementCharacter;
}

/// Check protocol against whitelist.
bool isValidUrlProtocol(const QString &protocol) {
    return protocol == QLatin1String("http")
        || protocol == QLatin1String("https")
        || protocol == QLatin1String("ftp");
}

/// Check TLD against known list.
bool isValidTopDomain(const QString &tld) {
    // Full TLD list.
    static const QSet<QString> kTLDs = {
        // Country codes
        QStringLiteral("ac"), QStringLiteral("ad"), QStringLiteral("ae"),
        QStringLiteral("af"), QStringLiteral("ag"), QStringLiteral("ai"),
        QStringLiteral("al"), QStringLiteral("am"), QStringLiteral("an"),
        QStringLiteral("ao"), QStringLiteral("aq"), QStringLiteral("ar"),
        QStringLiteral("as"), QStringLiteral("at"), QStringLiteral("au"),
        QStringLiteral("aw"), QStringLiteral("ax"), QStringLiteral("az"),
        QStringLiteral("ba"), QStringLiteral("bb"), QStringLiteral("bd"),
        QStringLiteral("be"), QStringLiteral("bf"), QStringLiteral("bg"),
        QStringLiteral("bh"), QStringLiteral("bi"), QStringLiteral("bj"),
        QStringLiteral("bm"), QStringLiteral("bn"), QStringLiteral("bo"),
        QStringLiteral("br"), QStringLiteral("bs"), QStringLiteral("bt"),
        QStringLiteral("bv"), QStringLiteral("bw"), QStringLiteral("by"),
        QStringLiteral("bz"), QStringLiteral("ca"), QStringLiteral("cc"),
        QStringLiteral("cd"), QStringLiteral("cf"), QStringLiteral("cg"),
        QStringLiteral("ch"), QStringLiteral("ci"), QStringLiteral("ck"),
        QStringLiteral("cl"), QStringLiteral("cm"), QStringLiteral("cn"),
        QStringLiteral("co"), QStringLiteral("cr"), QStringLiteral("cu"),
        QStringLiteral("cv"), QStringLiteral("cx"), QStringLiteral("cy"),
        QStringLiteral("cz"), QStringLiteral("de"), QStringLiteral("dj"),
        QStringLiteral("dk"), QStringLiteral("dm"), QStringLiteral("do"),
        QStringLiteral("dz"), QStringLiteral("ec"), QStringLiteral("ee"),
        QStringLiteral("eg"), QStringLiteral("eh"), QStringLiteral("er"),
        QStringLiteral("es"), QStringLiteral("et"), QStringLiteral("eu"),
        QStringLiteral("fi"), QStringLiteral("fj"), QStringLiteral("fk"),
        QStringLiteral("fm"), QStringLiteral("fo"), QStringLiteral("fr"),
        QStringLiteral("ga"), QStringLiteral("gd"), QStringLiteral("ge"),
        QStringLiteral("gf"), QStringLiteral("gg"), QStringLiteral("gh"),
        QStringLiteral("gi"), QStringLiteral("gl"), QStringLiteral("gm"),
        QStringLiteral("gn"), QStringLiteral("gp"), QStringLiteral("gq"),
        QStringLiteral("gr"), QStringLiteral("gs"), QStringLiteral("gt"),
        QStringLiteral("gu"), QStringLiteral("gw"), QStringLiteral("gy"),
        QStringLiteral("hk"), QStringLiteral("hm"), QStringLiteral("hn"),
        QStringLiteral("hr"), QStringLiteral("ht"), QStringLiteral("hu"),
        QStringLiteral("id"), QStringLiteral("ie"), QStringLiteral("il"),
        QStringLiteral("im"), QStringLiteral("in"), QStringLiteral("io"),
        QStringLiteral("iq"), QStringLiteral("ir"), QStringLiteral("is"),
        QStringLiteral("it"), QStringLiteral("je"), QStringLiteral("jm"),
        QStringLiteral("jo"), QStringLiteral("jp"), QStringLiteral("ke"),
        QStringLiteral("kg"), QStringLiteral("kh"), QStringLiteral("ki"),
        QStringLiteral("km"), QStringLiteral("kn"), QStringLiteral("kp"),
        QStringLiteral("kr"), QStringLiteral("kw"), QStringLiteral("ky"),
        QStringLiteral("kz"), QStringLiteral("la"), QStringLiteral("lb"),
        QStringLiteral("lc"), QStringLiteral("li"), QStringLiteral("lk"),
        QStringLiteral("lr"), QStringLiteral("ls"), QStringLiteral("lt"),
        QStringLiteral("lu"), QStringLiteral("lv"), QStringLiteral("ly"),
        QStringLiteral("ma"), QStringLiteral("mc"), QStringLiteral("md"),
        QStringLiteral("me"), QStringLiteral("mg"), QStringLiteral("mh"),
        QStringLiteral("mk"), QStringLiteral("ml"), QStringLiteral("mm"),
        QStringLiteral("mn"), QStringLiteral("mo"), QStringLiteral("mp"),
        QStringLiteral("mq"), QStringLiteral("mr"), QStringLiteral("ms"),
        QStringLiteral("mt"), QStringLiteral("mu"), QStringLiteral("mv"),
        QStringLiteral("mw"), QStringLiteral("mx"), QStringLiteral("my"),
        QStringLiteral("mz"), QStringLiteral("na"), QStringLiteral("nc"),
        QStringLiteral("ne"), QStringLiteral("nf"), QStringLiteral("ng"),
        QStringLiteral("ni"), QStringLiteral("nl"), QStringLiteral("no"),
        QStringLiteral("np"), QStringLiteral("nr"), QStringLiteral("nu"),
        QStringLiteral("nz"), QStringLiteral("om"), QStringLiteral("pa"),
        QStringLiteral("pe"), QStringLiteral("pf"), QStringLiteral("pg"),
        QStringLiteral("ph"), QStringLiteral("pk"), QStringLiteral("pl"),
        QStringLiteral("pm"), QStringLiteral("pn"), QStringLiteral("pr"),
        QStringLiteral("ps"), QStringLiteral("pt"), QStringLiteral("pw"),
        QStringLiteral("py"), QStringLiteral("qa"), QStringLiteral("re"),
        QStringLiteral("ro"), QStringLiteral("ru"), QStringLiteral("rs"),
        QStringLiteral("rw"), QStringLiteral("sa"), QStringLiteral("sb"),
        QStringLiteral("sc"), QStringLiteral("sd"), QStringLiteral("se"),
        QStringLiteral("sg"), QStringLiteral("sh"), QStringLiteral("si"),
        QStringLiteral("sj"), QStringLiteral("sk"), QStringLiteral("sl"),
        QStringLiteral("sm"), QStringLiteral("sn"), QStringLiteral("so"),
        QStringLiteral("sr"), QStringLiteral("ss"), QStringLiteral("st"),
        QStringLiteral("su"), QStringLiteral("sv"), QStringLiteral("sx"),
        QStringLiteral("sy"), QStringLiteral("sz"), QStringLiteral("tc"),
        QStringLiteral("td"), QStringLiteral("tf"), QStringLiteral("tg"),
        QStringLiteral("th"), QStringLiteral("tj"), QStringLiteral("tk"),
        QStringLiteral("tl"), QStringLiteral("tm"), QStringLiteral("tn"),
        QStringLiteral("to"), QStringLiteral("tp"), QStringLiteral("tr"),
        QStringLiteral("tt"), QStringLiteral("tv"), QStringLiteral("tw"),
        QStringLiteral("tz"), QStringLiteral("ua"), QStringLiteral("ug"),
        QStringLiteral("uk"), QStringLiteral("um"), QStringLiteral("us"),
        QStringLiteral("uy"), QStringLiteral("uz"), QStringLiteral("va"),
        QStringLiteral("vc"), QStringLiteral("ve"), QStringLiteral("vg"),
        QStringLiteral("vi"), QStringLiteral("vn"), QStringLiteral("vu"),
        QStringLiteral("wf"), QStringLiteral("ws"), QStringLiteral("ye"),
        QStringLiteral("yt"), QStringLiteral("yu"), QStringLiteral("za"),
        QStringLiteral("zm"), QStringLiteral("zw"),
        // Generic TLDs
        QStringLiteral("arpa"), QStringLiteral("aero"), QStringLiteral("asia"),
        QStringLiteral("biz"), QStringLiteral("cat"), QStringLiteral("com"),
        QStringLiteral("coop"), QStringLiteral("info"), QStringLiteral("int"),
        QStringLiteral("jobs"), QStringLiteral("mobi"), QStringLiteral("museum"),
        QStringLiteral("name"), QStringLiteral("net"), QStringLiteral("org"),
        QStringLiteral("post"), QStringLiteral("pro"), QStringLiteral("tel"),
        QStringLiteral("travel"), QStringLiteral("xxx"), QStringLiteral("edu"),
        QStringLiteral("gov"), QStringLiteral("mil"), QStringLiteral("local"),
        // IDN TLDs
        QStringLiteral("xn--lgbbat1ad8j"), QStringLiteral("xn--54b7fta0cc"),
        QStringLiteral("xn--fiqs8s"), QStringLiteral("xn--fiqz9s"),
        QStringLiteral("xn--wgbh1c"), QStringLiteral("xn--node"),
        QStringLiteral("xn--j6w193g"), QStringLiteral("xn--h2brj9c"),
        QStringLiteral("xn--mgbbh1a71e"), QStringLiteral("xn--fpcrj9c3d"),
        QStringLiteral("xn--gecrj9c"), QStringLiteral("xn--s9brj9c"),
        QStringLiteral("xn--xkc2dl3a5ee0h"), QStringLiteral("xn--45brj9c"),
        QStringLiteral("xn--mgba3a4f16a"), QStringLiteral("xn--mgbayh7gpa"),
        QStringLiteral("xn--80ao21a"), QStringLiteral("xn--mgbx4cd0ab"),
        QStringLiteral("xn--l1acc"), QStringLiteral("xn--mgbc0a9azcg"),
        QStringLiteral("xn--mgb9awbf"), QStringLiteral("xn--mgbai9azgqp6j"),
        QStringLiteral("xn--ygbi2ammx"), QStringLiteral("xn--wgbl6a"),
        QStringLiteral("xn--p1ai"), QStringLiteral("xn--mgberp4a5d4ar"),
        QStringLiteral("xn--90a3ac"), QStringLiteral("xn--yfro4i67o"),
        QStringLiteral("xn--clchc0ea0b2g2a9gcd"), QStringLiteral("xn--3e0b707e"),
        QStringLiteral("xn--fzc2c9e2c"), QStringLiteral("xn--xkc2al3hye2a"),
        QStringLiteral("xn--mgbtf8fl"), QStringLiteral("xn--kprw13d"),
        QStringLiteral("xn--kpry57d"), QStringLiteral("xn--o3cw4h"),
        QStringLiteral("xn--pgbs0dh"), QStringLiteral("xn--j1amh"),
        QStringLiteral("xn--mgbaam7a8h"), QStringLiteral("xn--mgb2ddes"),
        QStringLiteral("xn--ogbpf8fl"),
        QString::fromUtf8("\xd1\x80\xd1\x84"), // рф
    };
    return kTLDs.contains(tld);
}

/// Matches a domain with optional protocol.
/// Captures: (1) protocol, (2) full domain+port, (3) TLD, (4) port.
const QRegularExpression &domainRegExp() {
    static const auto re = QRegularExpression(
        QString::fromUtf8(
            "(?<![\\w\\$\\-\\_%=\\.])"
            "(?:([a-zA-Z]+)://)?"
            "((?:[A-Za-z"
            "\xD0\x90-\xD0\xAF\xD0\x81"
            "\xD0\xB0-\xD1\x8F\xD1\x91"
            "0-9\\-\\_]+\\.){1,10}"
            "([A-Za-z"
            "\xD1\x80\xD1\x84"
            "\\-\\d]{2,22})"
            "(\\:\\d+)?)"),
        QRegularExpression::UseUnicodePropertiesOption);
    return re;
}

/// Matches a domain with required protocol.
/// Allows 0 dots (e.g. "http://localhost:8080").
const QRegularExpression &explicitDomainRegExp() {
    static const auto re = QRegularExpression(
        QString::fromUtf8(
            "(?<![\\w\\$\\-\\_%=\\.])"
            "(?:([a-zA-Z]+)://)"
            "((?:[A-Za-z"
            "\xD0\x90-\xD0\xAF\xD0\x81"
            "\xD0\xB0-\xD1\x8F\xD1\x91"
            "0-9\\-\\_]+\\.){0,10}"
            "([A-Za-z"
            "\xD1\x80\xD1\x84"
            "\\-\\d]{2,22})"
            "(\\:\\d+)?)"),
        QRegularExpression::UseUnicodePropertiesOption);
    return re;
}

// ─── End URL detection helpers ────────────────────────────────────────

/// Auto-detect URLs in plain text and add link format ranges.
void autoDetectLinks(FormattedText &ft) {
    const auto &text = ft.plain;
    const int len = text.size();
    if (len == 0) return;
    const auto textStart = text.constData();
    const auto textEnd = textStart + len;

    for (int offset = 0, matchOffset = 0; offset < len;) {
        auto mDomain = domainRegExp().match(text, matchOffset);
        auto mExplicit = explicitDomainRegExp().match(text, matchOffset);

        if (!mDomain.hasMatch() && !mExplicit.hasMatch()) break;

        constexpr int kNotFound = std::numeric_limits<int>::max();
        int domStart = mDomain.hasMatch()
            ? mDomain.capturedStart() : kNotFound;
        int domEnd = mDomain.hasMatch()
            ? mDomain.capturedEnd() : kNotFound;
        int explStart = mExplicit.hasMatch()
            ? mExplicit.capturedStart() : kNotFound;
        int explEnd = mExplicit.hasMatch()
            ? mExplicit.capturedEnd() : kNotFound;

        // Prefer explicit domain match if it starts earlier.
        if (explStart < domStart) {
            domStart = explStart;
            domEnd = explEnd;
            mDomain = mExplicit;
        }

        auto protocol = mDomain.captured(1).toLower();
        auto topDomain = mDomain.captured(3).toLower();

        // Validate protocol (when present) or TLD (when schemeless).
        auto isProtocolValid = protocol.isEmpty()
            || isValidUrlProtocol(protocol);
        auto isTopDomainValid = !protocol.isEmpty()
            || isValidTopDomain(topDomain);

        if (!isProtocolValid || !isTopDomainValid) {
            matchOffset = domEnd;
            continue;
        }

        // Skip email addresses (@ directly before domain).
        if (protocol.isEmpty() && domStart > 0
            && *(textStart + domStart - 1) == QChar('@')) {
            matchOffset = domEnd;
            continue;
        }

        int lnkStart = domStart;

        // Extend link through path/query with balanced-paren tracking.
        QStack<const QChar*> parenth;
        const QChar *domEndPtr = textStart + domEnd;
        const QChar *p = domEndPtr;
        for (; p < textEnd; ++p) {
            QChar ch(*p);
            if (isLinkEnd(ch)) {
                break;
            } else if (isAlmostLinkEnd(ch)) {
                const QChar *endTest = p + 1;
                while (endTest < textEnd && isAlmostLinkEnd(*endTest)) {
                    ++endTest;
                }
                if (endTest >= textEnd || isLinkEnd(*endTest)) {
                    break; // trailing punctuation — exclude from link
                }
                p = endTest;
                ch = *p;
            }
            if (ch == '(' || ch == '[' || ch == '{' || ch == '<') {
                parenth.push(p);
            } else if (ch == ')' || ch == ']' || ch == '}' || ch == '>') {
                if (parenth.isEmpty()) break;
                const QChar *q = parenth.pop(), open(*q);
                if ((ch == ')' && open != '(')
                    || (ch == ']' && open != '[')
                    || (ch == '}' && open != '{')
                    || (ch == '>' && open != '<')) {
                    p = q;
                    break;
                }
            }
        }
        // For schemeless URLs, path must start with / or ?
        if (p > domEndPtr) {
            if (domEndPtr->unicode() != '/' && domEndPtr->unicode() != '?') {
                matchOffset = domEnd;
                continue;
            }
        }

        int lnkLength = static_cast<int>(p - textStart) - lnkStart;

        // Skip if already covered by an existing HTML link.
        bool covered = false;
        for (const auto &link : ft.links) {
            if (lnkStart >= link.start
                && lnkStart < link.start + link.length) {
                covered = true;
                break;
            }
        }
        // Skip URLs inside code blocks (Pre).
        if (!covered) {
            for (const auto &blk : ft.blocks) {
                if (blk.type == BlockType::Pre
                    && lnkStart >= blk.start
                    && lnkStart < blk.end) {
                    covered = true;
                    break;
                }
            }
        }
        if (covered) {
            offset = matchOffset = lnkStart + lnkLength;
            continue;
        }

        // Build URL (prepend https:// for schemeless URLs).
        QString linkText = text.mid(lnkStart, lnkLength);
        QString url = protocol.isEmpty()
            ? QStringLiteral("https://") + linkText
            : linkText;

        QTextCharFormat fmt;
        fmt.setForeground(st::historyLinkInFg);
        ft.formats.append({ lnkStart, lnkLength, fmt });
        ft.links.append({ lnkStart, lnkLength, url });

        offset = matchOffset = lnkStart + lnkLength;
    }
}

/// Resolve a TimelineItem to formatted text.
/// If formattedBody is present, parses HTML; otherwise returns plain body.
FormattedText resolveText(const TimelineItem &item) {
    const auto formatted = formattedText(item);
    if (!formatted.isEmpty()) {
        auto ft = parseFormattedBody(formatted);
        autoDetectLinks(ft);
        return ft;
    }
    // QTextLayout only breaks lines at QChar::LineSeparator (U+2028),
    // not at '\n' (U+000A).  Replace so plain-text messages wrap correctly.
    QString text = bodyText(item);
    text.replace(u'\n', QChar::LineSeparator);
    FormattedText ft = { text, {}, {}, {} };
    autoDetectLinks(ft);
    return ft;
}

bool formatRangeIsMonospace(const QTextLayout::FormatRange &range) {
    if (range.format.fontFixedPitch()) {
        return true;
    }
    const auto families = range.format.fontFamilies().toStringList();
    for (const auto &family : families) {
        const auto lower = family.toLower();
        if (lower.contains(QStringLiteral("mono"))
            || lower.contains(QStringLiteral("courier"))
            || lower.contains(QStringLiteral("menlo"))
            || lower.contains(QStringLiteral("consolas"))
            || lower.contains(QStringLiteral("cascadia"))
            || lower.contains(QStringLiteral("liberation"))) {
            return true;
        }
    }
    return false;
}

bool rangeStartsInPreBlock(const FormattedText &text, int start) {
    for (const auto &block : text.blocks) {
        if (block.type == BlockType::Pre
            && start >= block.start
            && start < block.end) {
            return true;
        }
    }
    return false;
}

bool rangeCoveredByMonospaceFormat(
        const FormattedText &text,
        int start,
        int end) {
    if (start >= end) {
        return false;
    }
    // Fast path: messages with no monospace formats at all (the common case)
    // can't be covered — skip the coverage walk entirely.
    auto hasMono = false;
    for (const auto &format : text.formats) {
        if (formatRangeIsMonospace(format)) {
            hasMono = true;
            break;
        }
    }
    if (!hasMono) {
        return false;
    }
    auto coveredUntil = start;
    while (coveredUntil < end) {
        auto extended = coveredUntil;
        for (const auto &format : text.formats) {
            if (!formatRangeIsMonospace(format)) {
                continue;
            }
            const auto formatStart = format.start;
            const auto formatEnd = format.start + format.length;
            if (formatStart <= coveredUntil && formatEnd > coveredUntil) {
                extended = qMax(extended, qMin(formatEnd, end));
            }
        }
        if (extended == coveredUntil) {
            return false;
        }
        coveredUntil = extended;
    }
    return true;
}

bool rangeIsMonospace(const FormattedText &text, int start, int length) {
    if (length <= 0) {
        return false;
    }
    const auto end = start + length;
    return rangeStartsInPreBlock(text, start)
        || rangeCoveredByMonospaceFormat(text, start, end);
}

bool previewUrlsEquivalent(QString a, QString b) {
    a = a.trimmed();
    b = b.trimmed();
    if (a == b) {
        return true;
    }
    while (a.endsWith(u'/')) {
        a.chop(1);
    }
    while (b.endsWith(u'/')) {
        b.chop(1);
    }
    return a == b;
}

bool hasRenderableLinkPreview(const TimelineItem &item) {
    const auto preview = urlPreviewInfo(item);
    if (!isTextMessage(item)
        || !preview
        || preview->url.isEmpty()) {
        return false;
    }

    const auto &resolved = cachedText(item, item.delivery.outgoing).resolved;
    const auto previewUrl = preview->url.trimmed();
    auto matchedSource = false;
    auto hasNonMonospaceSource = false;
    const auto consider = [&](int start, int length) {
        if (start >= 0 && length > 0) {
            matchedSource = true;
            if (!rangeIsMonospace(resolved, start, length)) {
                hasNonMonospaceSource = true;
            }
        }
    };

    for (const auto &link : resolved.links) {
        if (previewUrlsEquivalent(link.url, previewUrl)) {
            consider(link.start, link.length);
        }
    }

    auto from = 0;
    while (!previewUrl.isEmpty()) {
        const auto at = resolved.plain.indexOf(previewUrl, from, Qt::CaseSensitive);
        if (at < 0) {
            break;
        }
        consider(at, previewUrl.size());
        from = at + previewUrl.size();
    }

    // If the metadata URL can no longer be matched against the rendered text,
    // keep the card. The suppression is only for known monospace-only sources.
    return !matchedSource || hasNonMonospaceSource;
}

// ─── Block-aware text layout ───

/// Find which block index (into `blocks`) a text position belongs to.
/// Returns -1 if not inside any block.
int findBlock(int textPos, const QList<BlockRange> &blocks) {
    for (int i = 0; i < blocks.size(); ++i) {
        if (textPos >= blocks[i].start && textPos < blocks[i].end) {
            return i;
        }
    }
    return -1;
}

/// Layout text with per-line block-aware width/offset adjustments.
/// Uses the provided QTextLayout (which must have text + formats already set).
/// Calls beginLayout/endLayout on the layout.
BlockAwareLayoutResult layoutWithBlocks(
    QTextLayout &layout,
    int availWidth,
    const QList<BlockRange> &blocks)
{
    BlockAwareLayoutResult result{};
    int totalHeight = 0;
    int currentBlockIdx = -1;

    auto styleFor = [](BlockType type) -> const st::QuoteStyle & {
        return (type == BlockType::Pre) ? kPreStyle : kBqStyle;
    };

    layout.beginLayout();
    while (true) {
        QTextLine line = layout.createLine();
        if (!line.isValid()) break;

        const int lineStart = line.textStart();
        const int newBlockIdx = findBlock(lineStart, blocks);

        // Skip trailing empty line that QTextLayout creates when text ends
        // at a block boundary (e.g. code block is last element). The line
        // starts at the block's exclusive end, so findBlock returns -1.
        // Without this, the "leaving block" transition adds ~24px of dead
        // space (2×verticalSkip + empty line height).
        if (currentBlockIdx >= 0 && newBlockIdx < 0
            && line.textLength() == 0
            && !blocks.isEmpty()
            && lineStart >= blocks.last().end) {
            break;
        }

        // Handle transition: leaving a block.
        // Matches input field: verticalSkip inside decoration below text,
        // then verticalSkip gap outside decoration.
        if (currentBlockIdx >= 0 && newBlockIdx != currentBlockIdx) {
            const auto &style = styleFor(blocks[currentBlockIdx].type);
            totalHeight += style.verticalSkip;
            result.blockInfos.last().bottomY = totalHeight;
            totalHeight += style.verticalSkip;
        }

        // Handle transition: entering a block.
        // Matches input field: verticalSkip gap above decoration,
        // header inside decoration, verticalSkip inside before text.
        if (newBlockIdx >= 0 && newBlockIdx != currentBlockIdx) {
            const auto &blk = blocks[newBlockIdx];
            const auto &style = styleFor(blk.type);
            totalHeight += style.verticalSkip;
            const int topY = totalHeight;

            if (style.header > 0) {
                totalHeight += style.header;
            }
            totalHeight += style.verticalSkip;

            result.blockInfos.append({ blk.type, topY, 0 });
        }

        currentBlockIdx = newBlockIdx;

        // Set line width and position based on block membership.
        int lineWidth = availWidth;
        int lineX = 0;
        if (currentBlockIdx >= 0) {
            const auto &style = styleFor(blocks[currentBlockIdx].type);
            const int padLeft = style.padding.left() + style.outline;
            const int padRight = style.padding.right();
            // Both Pre and Blockquote use available width minus padding.
            // Pre blocks expand the bubble via preMaxNaturalWidth, so
            // availWidth is already the viewport-capped expanded width
            // when called from the paint path. Lines only wrap if they
            // exceed the viewport.
            lineWidth = qMax(1, availWidth - padLeft - padRight);
            lineX = padLeft;
        }

        line.setLineWidth(lineWidth);
        totalHeight += line.leading();
        line.setPosition(QPointF(lineX, totalHeight));
        totalHeight += qRound(line.height());

        // Track widths. For block lines, include the padding.
        int natWidth = int(std::ceil(line.naturalTextWidth()));
        if (currentBlockIdx >= 0) {
            const auto &style = styleFor(blocks[currentBlockIdx].type);
            const int padLeft = style.padding.left() + style.outline;
            const int padRight = style.padding.right();
            natWidth += padLeft + padRight;
        }
        result.lastLineWidth = natWidth;
        result.maxLineWidth = qMax(result.maxLineWidth, natWidth);
        ++result.lineCount;
    }
    layout.endLayout();

    // Measure natural (unwrapped) width of pre-block content.
    // QTextLine::naturalTextWidth() after wrapping gives per-wrapped-line width,
    // not the unwrapped width. So we measure the source text directly
    // using QFontMetrics on the monospace font.
    if (!blocks.isEmpty()) {
        const auto plainText = layout.text();
        static const QFontMetrics monoFm(st::monospaceFont(st::fsize));
        for (const auto &blk : blocks) {
            if (blk.type != BlockType::Pre) continue;

            const auto &style = kPreStyle;
            const int padLeft = style.padding.left() + style.outline;
            const int padRight = style.padding.right();

            const auto blockText = plainText.mid(blk.start, blk.end - blk.start);
            int lineStart = 0;
            for (int j = 0; j <= blockText.size(); ++j) {
                const bool atEnd = (j == blockText.size());
                const bool atSep = !atEnd
                    && (blockText[j] == QChar::LineSeparator
                        || blockText[j] == u'\n');
                if (atEnd || atSep) {
                    const auto lineText = blockText.mid(lineStart, j - lineStart);
                    const auto lineW = monoFm.horizontalAdvance(lineText)
                        + padLeft + padRight;
                    result.preMaxNaturalWidth = qMax(
                        result.preMaxNaturalWidth, lineW);
                    lineStart = j + 1;
                }
            }
        }
    }

    // Close final block if text ends inside one.
    if (currentBlockIdx >= 0) {
        const auto &style = styleFor(blocks[currentBlockIdx].type);
        totalHeight += style.verticalSkip;
        result.blockInfos.last().bottomY = totalHeight;

        // Force timestamp onto its own line below the block decoration,
        // so it doesn't overlap the block background.
        result.lastLineWidth = availWidth;
    }

    result.textHeight = totalHeight;
    return result;
}

/// Fallback: simple layout for messages without blocks.
struct TextLayoutResult {
    int height;
    int lastLineWidth;
    int maxLineWidth;
    int lineCount;
};

TextLayoutResult layoutText(
    const QString &text,
    int availWidth,
    const QList<QTextLayout::FormatRange> &formats = {})
{
    QFont bodyFont(st::msgFont);
    QTextLayout layout(text, bodyFont);
    if (!formats.isEmpty()) {
        layout.setFormats(formats);
    }

    layout.beginLayout();
    int totalHeight = 0;
    int lastWidth = 0;
    int maxWidth = 0;
    int lines = 0;
    while (true) {
        QTextLine line = layout.createLine();
        if (!line.isValid()) break;
        line.setLineWidth(availWidth);
        totalHeight += line.leading();
        line.setPosition(QPointF(0, totalHeight));
        totalHeight += qRound(line.height());
        lastWidth = int(std::ceil(line.naturalTextWidth()));
        maxWidth = qMax(maxWidth, lastWidth);
        ++lines;
    }
    layout.endLayout();

    return { totalHeight, lastWidth, maxWidth, lines };
}

// ─── Block decoration painting ───

/// Paint block decoration using cached corner images.
/// Uses static caches indexed by [blockType][isOutgoing].
void paintBlockDecoration(
    QPainter &p,
    QRect rect,
    BlockType type,
    bool isOutgoing)
{
    // Static caches: [isPre][isOutgoing]
    static Ui::Text::QuotePaintCache caches[2][2];

    const QColor baseColor = (type == BlockType::Pre)
        ? (isOutgoing ? st::msgOutMonoFg : st::msgInMonoFg)
        : (isOutgoing ? st::msgOutReplyBarColor : st::msgInReplyBarColor);

    auto &cache = caches[type == BlockType::Pre ? 1 : 0][isOutgoing ? 1 : 0];
    const auto &style = (type == BlockType::Pre) ? kPreStyle : kBqStyle;

    Ui::Text::SetQuoteCacheColors(cache, baseColor);
    Ui::Text::ValidateQuotePaintCache(cache, style);
    Ui::Text::FillQuotePaint(p, rect, cache, style);

    // Code block header label (lowercase "copy").
    if (type == BlockType::Pre && style.header > 0) {
        static const QString kCopyText = QCoreApplication::translate("HistoryMessage", "copy");
        p.setFont(st::normalFont);
        const auto &fm = st::fontMetrics(st::normalFont);
        p.setPen(cache.icon);
        p.drawText(
            rect.left() + style.headerPosition.x(),
            rect.top() + style.headerPosition.y() + fm.ascent(),
            kCopyText);
    }
}

/// Paint reply preview decoration using messageQuoteStyle (padding.right=4, no icon).
void paintReplyDecoration(
    QPainter &p,
    QRect rect,
    bool isOutgoing)
{
    static Ui::Text::QuotePaintCache caches[2];

    const QColor baseColor = isOutgoing
        ? st::msgOutReplyBarColor
        : st::msgInReplyBarColor;
    auto &cache = caches[isOutgoing ? 1 : 0];

    const auto &style = st::messageQuoteStyle;

    Ui::Text::SetQuoteCacheColors(cache, baseColor);
    Ui::Text::ValidateQuotePaintCache(cache, style);
    Ui::Text::FillQuotePaint(p, rect, cache, style);
}

// ─── Link Preview Card ───────────────────────────────────────────────

// --- Photo sizing helpers ---

/// Downscale to fit box, keeping aspect ratio; never upscale.
QSize lpDownscaledSize(QSize size, QSize box) {
    auto result = ((size.width() > box.width() || size.height() > box.height())
        ? size.scaled(box, Qt::KeepAspectRatio)
        : size);
    return QSize(qMax(result.width(), 1), qMax(result.height(), 1));
}

/// Compute a photo's media size within newWidth/maxWidth bounds.
QSize lpCountPhotoMediaSize(QSize desired, int newWidth, int maxWidth) {
    const auto limit = qMin(newWidth, maxWidth);
    const auto media = (desired.width() <= limit)
        ? desired
        : desired.scaled(limit, desired.height(), Qt::KeepAspectRatio);
    const auto mediaSafe = QSize(qMax(media.width(), 1), qMax(media.height(), 1));
    if (mediaSafe.height() <= newWidth) {
        return mediaSafe;
    }
    const auto scaled = mediaSafe.scaled(mediaSafe.width(), newWidth, Qt::KeepAspectRatio);
    return QSize(qMax(scaled.width(), 1), qMax(scaled.height(), 1));
}

/// Grow height toward the proportional height to reduce cropping.
int lpAdjustHeightForLessCrop(QSize dimensions, QSize current) {
    if (dimensions.isEmpty()) {
        return current.height();
    }
    const auto proportionalH = current.width() * dimensions.height() / dimensions.width();
    return qMax(current.height(), proportionalH);
}

/// Compute photo display size for LP card large media.
/// Photo fills the full inner width; height is proportional, clamped to maxMediaSize.
QSize linkPreviewPhotoSize(int origW, int origH, int innerWidth) {
    if (origW <= 0 || origH <= 0) {
        return {innerWidth, 100};
    }
    constexpr int kMaxMediaSize = 430; // st::maxMediaSize
    constexpr int kMinPhotoHeight = 100; // st::minPhotoSize

    const int w = innerWidth;
    int h = qRound((double)origH / origW * w);
    h = qMax(h, kMinPhotoHeight);
    h = qMin(h, kMaxMediaSize);

    return {w, h};
}

/// Compute the line height for link preview text (max of semibold/normal fonts).
int linkPreviewLineHeight() {
    const auto &semiboldFm = st::fontMetrics(st::semiboldFont);
    const auto &normalFm = st::fontMetrics(st::msgFont);
    return qMax(semiboldFm.height(), normalFm.height());
}

void configureLinkPreviewTextLayout(QTextLayout &layout) {
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
}

const TimelineUrlPreviewInfo &linkPreviewInfoFor(const TimelineItem &item) {
    static const TimelineUrlPreviewInfo kEmptyPreview;
    if (const auto preview = urlPreviewInfo(item)) {
        return *preview;
    }
    return kEmptyPreview;
}

uint linkPreviewHashFor(const TimelineItem &item) {
    const auto &preview = linkPreviewInfoFor(item);
    auto result = uint(0);
    const auto mix = [&result](uint value) {
        result ^= value + 0x9e3779b9U + (result << 6) + (result >> 2);
    };
    mix(qHash(preview.url));
    mix(qHash(preview.siteName));
    mix(qHash(preview.title));
    mix(qHash(preview.description));
    mix(qHash(preview.imageUrl));
    mix(qHash(preview.imageWidth));
    mix(qHash(preview.imageHeight));
    mix(qHash(static_cast<int>(preview.type)));
    mix(qHash(preview.duration));
    mix(qHash(preview.author));
    mix(qHash(preview.hasLargeMedia));
    mix(qHash(preview.siteNameCanonical));
    return result;
}

QString linkPreviewCacheKeyFor(const TimelineItem &item) {
    if (!item.eventId.isEmpty()) {
        return item.eventId;
    }
    if (!item.transactionId.isEmpty()) {
        return QStringLiteral("tx:") + item.transactionId;
    }
    return QStringLiteral("preview:") + QString::number(linkPreviewHashFor(item));
}

QStringList linkPreviewTextLines(
        const QString &text,
        const QFont &font,
        int width,
        int maxLines) {
    if (text.isEmpty() || width <= 0 || maxLines <= 0) {
        return {};
    }

    QTextLayout layout(text, font);
    configureLinkPreviewTextLayout(layout);
    layout.beginLayout();
    QStringList result;
    while (result.size() < maxLines) {
        auto line = layout.createLine();
        if (!line.isValid()) {
            break;
        }
        line.setLineWidth(width);
        const auto lineEnd = line.textStart() + line.textLength();
        const auto truncated = (result.size() + 1 == maxLines)
            && (lineEnd < text.size());
        if (truncated) {
            const auto tail = text.mid(line.textStart());
            result.push_back(QFontMetrics(font).elidedText(tail, Qt::ElideRight, width));
        } else {
            result.push_back(text.mid(line.textStart(), line.textLength()));
        }
    }
    layout.endLayout();
    return result;
}

int drawCachedLinkPreviewText(
        QPainter &p,
        const QStringList &lines,
        const QFont &font,
        int x,
        int y,
        int lineHeight) {
    if (lines.isEmpty()) {
        return 0;
    }

    const auto fm = QFontMetrics(font);

    p.save();
    p.setFont(font);
    for (auto i = 0; i != lines.size(); ++i) {
        p.drawText(x, y + (i * lineHeight) + fm.ascent(), lines[i]);
    }
    p.restore();

    return lines.size();
}

// ─── Media caption wrapping ──────────────────────────────────────────
// Media captions (photo/video/file) wrap to as many lines as needed,
// unlike link-preview text which is capped+elided.
struct CaptionLayout {
    QStringList lines;
    int lastLineWidth = 0; // natural width of the final line (for timestamp fit)
};

CaptionLayout layoutCaptionLines(const QString &caption, int availWidth) {
    CaptionLayout result;
    if (caption.isEmpty() || availWidth <= 0) {
        return result;
    }
    // QTextLayout only hard-breaks on QChar::LineSeparator (U+2028), not '\n'.
    auto text = caption;
    text.replace(u'\n', QChar::LineSeparator);

    QTextLayout layout(text, QFont(st::msgFont));
    layout.beginLayout();
    while (true) {
        auto line = layout.createLine();
        if (!line.isValid()) {
            break;
        }
        line.setLineWidth(availWidth);
        result.lines.push_back(text.mid(line.textStart(), line.textLength()));
        result.lastLineWidth = int(std::ceil(line.naturalTextWidth()));
    }
    layout.endLayout();
    return result;
}

// Total vertical space a caption block occupies inside the bubble. Mirrors
// paintTextMessage: reserves one extra line for the timestamp when it can't
// sit after the last caption line (and there are no reactions, which carry
// the timestamp on their own row).
int captionBlockHeight(
        const CaptionLayout &layout,
        const TimelineItem &item,
        int availWidth,
        bool hasReactions) {
    if (layout.lines.isEmpty()) {
        return 0;
    }
    auto height = int(layout.lines.size()) * st::msgFont->height;
    const auto timeFits =
        (layout.lastLineWidth + skipBlockWidth(item) <= availWidth);
    if (!hasReactions && !timeFits) {
        height += skipBlockHeight();
    }
    return height;
}

/// Whether a link preview has a renderable image. Only an mxc:// image resolves
/// (M2: a raw http(s) og:image never loads — Rust now drops those, but rows
/// persisted before that fix may still carry one). Dimensions may be absent
/// (L5); the article/large-media sizing falls back to a square, so width==0 is
/// still renderable.
bool linkPreviewHasImage(const TimelineUrlPreviewInfo &preview) {
    return preview.imageUrl.startsWith(QStringLiteral("mxc://"));
}

/// Type-driven article detection: returns true when the preview should use
/// article mode (small thumbnail top-right) instead of large media mode.
bool isLinkPreviewArticle(const TimelineItem &item) {
    const auto &preview = linkPreviewInfoFor(item);
    if (!linkPreviewHasImage(preview)) {
        return false;
    }
    const bool hasTextInfo = !preview.siteName.isEmpty()
        || !preview.title.isEmpty()
        || !preview.description.isEmpty();
    if (!hasTextInfo) {
        return false;
    }

    // hasLargeMedia flag forces large media (non-article) mode.
    // Provider rules set this for Twitter, Facebook, Instagram, YouTube.
    if (preview.hasLargeMedia) {
        return false;
    }

    // Photo/Video/Document types → large media mode.
    if (preview.type == PreviewType::Photo
        || preview.type == PreviewType::Video
        || preview.type == PreviewType::Document) {
        return false;
    }

    // Profile type → always small article thumb.
    if (preview.type == PreviewType::Profile) {
        return true;
    }

    // Default (Article, None, etc.) → article mode (small thumb).
    return true;
}

/// Returns thumbnail width from height, clamped so it doesn't exceed height (square max).
/// Unknown dimensions (L5: an og:image with no og:image:width) fall back to a
/// square cell rather than a 1px sliver.
int articleThumbWidth(int imageW, int imageH, int height) {
    if (imageW <= 0 || imageH <= 0) {
        return height;
    }
    return std::max(std::min(height * imageW / imageH, height), 1);
}

/// Returns thumbnail height from width.
int articleThumbHeight(int imageW, int imageH, int width) {
    return imageW
        ? std::max(imageH * width / imageW, 1)
        : 1;
}

const CachedLinkPreviewLayout &cachedLinkPreviewLayout(
        const TimelineItem &item,
        int innerWidth) {
    const auto &preview = linkPreviewInfoFor(item);
    const auto hash = linkPreviewHashFor(item);
    capPaintCache(s_linkPreviewCache);
    auto &entry = s_linkPreviewCache[linkPreviewCacheKeyFor(item)];
    const auto article = isLinkPreviewArticle(item);
    if (entry.innerWidth == innerWidth
        && entry.previewHash == hash
        && entry.article == article
        && entry.textHeight >= 0) {
        return entry;
    }

    entry = CachedLinkPreviewLayout();
    entry.innerWidth = innerWidth;
    entry.previewHash = hash;
    entry.article = article;

    const auto lineHeight = linkPreviewLineHeight();
    constexpr int kLinesMax = 5;
    auto fillText = [&](int textWidth) {
        textWidth = qMax(0, textWidth);
        entry.textWidth = textWidth;
        entry.siteNameLines = preview.siteName.isEmpty()
            ? QStringList()
            : linkPreviewTextLines(preview.siteName, st::semiboldFont, textWidth, 1);
        entry.titleLines = linkPreviewTextLines(
            preview.title,
            st::semiboldFont,
            textWidth,
            2);
        const auto restLines = qMax(
            0,
            kLinesMax - entry.siteNameLines.size() - entry.titleLines.size());
        entry.descriptionLines = linkPreviewTextLines(
            preview.description,
            st::msgFont,
            textWidth,
            restLines);
        entry.textHeight = (entry.siteNameLines.size()
            + entry.titleLines.size()
            + entry.descriptionLines.size()) * lineHeight;
    };

    if (!article) {
        fillText(innerWidth);
        return entry;
    }

    int pixh = lineHeight * kLinesMax;
    int pixw = 0;
    auto newHeight = 0;

    do {
        pixw = articleThumbWidth(
            preview.imageWidth, preview.imageHeight, pixh);
        const auto wleft = innerWidth
            - st::webPagePhotoDelta
            - std::max(pixw, lineHeight);

        const auto siteLines = preview.siteName.isEmpty()
            ? QStringList()
            : linkPreviewTextLines(preview.siteName, st::semiboldFont, wleft, 1);
        const auto titleLines = linkPreviewTextLines(
            preview.title,
            st::semiboldFont,
            wleft,
            2);
        const auto restLines = qMax(
            0,
            kLinesMax - siteLines.size() - titleLines.size());
        const auto descriptionLines = linkPreviewTextLines(
            preview.description,
            st::msgFont,
            wleft,
            restLines);
        newHeight = (siteLines.size()
            + titleLines.size()
            + descriptionLines.size()) * lineHeight;

        if (newHeight >= pixh) {
            break;
        }
        pixh -= lineHeight;
    } while (pixh > lineHeight);

    pixw = articleThumbWidth(
        preview.imageWidth, preview.imageHeight, pixh);

    entry.thumbW = pixw;
    entry.thumbH = pixh;
    fillText(innerWidth - st::webPagePhotoDelta - std::max(pixw, lineHeight));
    return entry;
}

/// Calculate the height of a link preview block (inside bubble).
int linkPreviewBlockHeight(const TimelineItem &item, int availWidth) {
    const auto &preview = linkPreviewInfoFor(item);
    const auto &style = st::historyPagePreviewStyle;
    const auto innerWidth = availWidth - style.padding.left() - style.padding.right();
    const auto &cached = cachedLinkPreviewLayout(item, innerWidth);

    if (isLinkPreviewArticle(item)) {
        // --- Article mode: cached sizing loop ---
        // Height is max(text height, thumb height) + padding.
        const int contentH = qMax(cached.textHeight, cached.thumbH);
        return style.padding.top() + contentH + style.padding.bottom();
    }

    // --- Large media mode (or no image) ---
    int totalH = style.padding.top() + cached.textHeight + style.padding.bottom();

    // Image below text (large media mode).
    if (linkPreviewHasImage(preview)) {
        const auto photoSz = linkPreviewPhotoSize(
            preview.imageWidth, preview.imageHeight, innerWidth);
        totalH += st::mediaInBubbleSkip + photoSz.height();
    }

    return totalH;
}

/// Minimum bubble width needed for a link preview card.
/// A web-page preview forces the max bubble width (msgMaxWidth).
int linkPreviewMinWidth(const TimelineItem &) {
    return kMaxBubbleWidth - 2 * kBubblePaddingH;
}

/// Load and cache a preview image (supports mxc:// via MediaCache).
/// Paint a link preview card (quote decoration + site name + title + description + image).
/// Supports two modes:
///   - Article mode: small thumbnail top-right, text beside it (default for most pages).
///   - Large media mode: full-width image below text (fallback when no text info).
bool paintLinkPreviewBlock(
    QPainter &p,
    const TimelineItem &item,
    const QRect &rect,
    bool isOut,
    qreal dpr,
    QWidget *repaintTarget,
    const QRect &repaintRect)
{
    const auto &preview = linkPreviewInfoFor(item);
    const auto &style = st::historyPagePreviewStyle;

    // --- Accent bar + rounded background decoration ---
    const QColor accentColor = isOut
        ? st::msgOutReplyBarColor
        : senderColor(item.sender.id);

    // Per-accent cache: the accent is per-sender (senderColor), so a fixed
    // 2-slot [isOut] cache thrashed — two visible cards from different senders
    // regenerated the corner pixmaps every paint (and ~60fps under the image
    // glow driver). Key by accent rgba instead (which also distinguishes the
    // fixed outgoing color); bound like the other paint caches.
    static QHash<QRgb, Ui::Text::QuotePaintCache> lpCaches;
    if (lpCaches.size() > 64) {
        lpCaches.clear();
    }
    auto &cache = lpCaches[accentColor.rgba()];
    Ui::Text::SetQuoteCacheColors(cache, accentColor);
    Ui::Text::ValidateQuotePaintCache(cache, style);
    Ui::Text::FillQuotePaint(p, rect, cache, style);

    // Inner rect.
    const auto inner = rect.marginsRemoved(
        QMargins(style.padding.left(), style.padding.top(),
                 style.padding.right(), style.padding.bottom()));

    const auto lineHeight = linkPreviewLineHeight();
    const bool hasImage = linkPreviewHasImage(preview);
    const bool article = isLinkPreviewArticle(item);
    const auto &cachedLayout = cachedLinkPreviewLayout(item, inner.width());

    int paintW = inner.width();
    int textTop = inner.top();
    bool loadingImage = false;

    // --- Article mode: draw thumbnail top-right FIRST, reduce paintW ---
    if (article) {
        constexpr int kRoundSmall = 3; // st::roundRadiusSmall
        const auto previewImageKey = MediaCache::previewImageKey(preview.imageUrl);

        // Use cached article thumb dimensions (same sizing loop as height calc).
        const int articlePixW = cachedLayout.thumbW;
        const int articlePixH = cachedLayout.thumbH;

        // pw = max(_pixw, lineHeight) — min thumb cell is at least lineHeight.
        const int pw = qMax(articlePixW, lineHeight);
        const int ph = articlePixH;

        // Draw thumbnail at top-right with RoundSmall (3px) corners.
        const QRect thumbRect(
            inner.left() + paintW - pw,
            textTop,
            pw,
            ph);

        const auto thumbPix = MediaCache::loadPixmapAsync(
            previewImageKey,
            thumbRect.size(),
            dpr,
            repaintTarget,
            repaintRect);
        if (!thumbPix.isNull()) {
            p.save();
            QPainterPath clipPath;
            clipPath.addRoundedRect(QRectF(thumbRect), kRoundSmall, kRoundSmall);
            p.setClipPath(clipPath);
            p.drawPixmap(thumbRect, thumbPix);
            p.restore();
        } else if (MediaCache::shouldGlowWhileLoading(previewImageKey)) {
            // Glowing skeleton placeholder while the thumbnail loads.
            paintLinkImageGlow(
                p, thumbRect, kRoundSmall, cache.bg, linkImageGlowPhase());
            loadingImage = true;
        } else {
            // Gone/undecodable, or failed too often to keep animating: static
            // skeleton, no pulse, no repaint scheduling. Any retry continues quietly.
            paintLinkImageGlow(p, thumbRect, kRoundSmall, cache.bg, /*phase=*/0.0);
        }

        // Reduce text width by thumbnail + delta.
        paintW -= pw + st::webPagePhotoDelta;
    }

    paintW = cachedLayout.textWidth >= 0 ? cachedLayout.textWidth : paintW;

    // --- Site name (accent color, semibold, 1 line) ---
    if (!cachedLayout.siteNameLines.isEmpty()) {
        p.setPen(cache.icon); // accent color at 60% opacity
        textTop += drawCachedLinkPreviewText(
            p,
            cachedLayout.siteNameLines,
            st::semiboldFont,
            inner.left(),
            textTop,
            lineHeight) * lineHeight;
    }

    // --- Title (semibold, text color, 1-2 lines) ---
    if (!cachedLayout.titleLines.isEmpty()) {
        p.setPen(isOut ? st::historyTextOutFg : st::historyTextInFg);
        textTop += drawCachedLinkPreviewText(
            p,
            cachedLayout.titleLines,
            st::semiboldFont,
            inner.left(),
            textTop,
            lineHeight) * lineHeight;
    }

    // --- Description (normal font, text color, dynamic line count) ---
    if (!cachedLayout.descriptionLines.isEmpty()) {
        p.setPen(isOut ? st::historyTextOutFg : st::historyTextInFg);
        textTop += drawCachedLinkPreviewText(
            p,
            cachedLayout.descriptionLines,
            st::msgFont,
            inner.left(),
            textTop,
            lineHeight) * lineHeight;
    }

    // --- Large media mode: image below text, no corner rounding ---
    if (hasImage && !article) {
        textTop += st::mediaInBubbleSkip;
        const auto photoSz = linkPreviewPhotoSize(
            preview.imageWidth, preview.imageHeight, inner.width());
        const int imgW = photoSz.width();
        const int imgH = photoSz.height();
        const QRect imgRect(inner.left(), textTop, imgW, imgH);
        const auto previewImageKey = MediaCache::previewImageKey(preview.imageUrl);

        const auto imgPix = MediaCache::loadPixmapAsync(
            previewImageKey,
            imgRect.size(),
            dpr,
            repaintTarget,
            repaintRect);
        if (!imgPix.isNull()) {
            p.drawPixmap(imgRect, imgPix);
        } else if (MediaCache::shouldGlowWhileLoading(previewImageKey)) {
            // Glowing skeleton placeholder while the image loads.
            paintLinkImageGlow(
                p, imgRect, 0, cache.bg, linkImageGlowPhase());
            loadingImage = true;
        } else {
            // Gone/undecodable, or failed too often to keep animating: static
            // skeleton, no pulse, no repaint scheduling.
            paintLinkImageGlow(p, imgRect, 0, cache.bg, /*phase=*/0.0);
        }

        // YouTube/video play icon overlay.
        // Icons are white masks colorized per these colors:
        //   YouTube bg: #e83131c8  fg: white
        //   Video   bg: #0000007f  fg: white
        const bool isVideoPreview = (preview.type == PreviewType::Video);
        if (isVideoPreview && !imgPix.isNull()) {
            const bool isYouTube = (preview.siteNameCanonical == QStringLiteral("youtube"));
            const auto bgName = isYouTube
                ? QStringLiteral("media_youtube_play_bg")
                : QStringLiteral("media_video_play_bg");
            const auto fgName = isYouTube
                ? QStringLiteral("media_youtube_play")
                : QStringLiteral("media_video_play");
            const auto bgColor = isYouTube
                ? st::historyMediaYoutubePlayBg
                : st::historyMediaVideoPlayBg;
            const auto fgColor = st::historyIconFgInverted;

            const auto bgImg = TeleMatrix::Style::IconProvider::tintedIcon(QStringLiteral(":/telematrix/icons/chat/"), bgName, bgColor);
            const auto fgImg = TeleMatrix::Style::IconProvider::tintedIcon(QStringLiteral(":/telematrix/icons/chat/"), fgName, fgColor);
            if (!bgImg.isNull() && !fgImg.isNull()) {
                const int bgW = int(bgImg.width() / bgImg.devicePixelRatio());
                const int bgH = int(bgImg.height() / bgImg.devicePixelRatio());
                const int fgW = int(fgImg.width() / fgImg.devicePixelRatio());
                const int fgH = int(fgImg.height() / fgImg.devicePixelRatio());
                const int bgX = imgRect.center().x() - bgW / 2;
                const int bgY = imgRect.center().y() - bgH / 2;
                const int fgX = bgX + (bgW - fgW) / 2;
                const int fgY = bgY + (bgH - fgH) / 2;
                const auto bgRect = QRect(bgX, bgY, bgW, bgH);
                if (isYouTube) {
                    PainterHighQualityEnabler hq(p);
                    p.setPen(Qt::NoPen);
                    p.setBrush(bgColor);
                    p.drawRoundedRect(
                        QRectF(bgRect),
                        bgRect.height() / 4.0,
                        bgRect.height() / 4.0);
                } else {
                    p.drawImage(bgRect, bgImg);
                }
                p.drawImage(QRect(fgX, fgY, fgW, fgH), fgImg);
            }
        }

        // Duration badge on video previews (bottom-right corner).
        if (isVideoPreview && preview.duration > 0) {
            const auto duration = formatDuration(static_cast<quint64>(preview.duration) * 1000);
            const auto &dfm = st::fontMetrics(st::msgDateFont);
            const int durW = dfm.horizontalAdvance(duration);
            constexpr int kPadX = 8; // st::msgDateImgPadding.x()
            constexpr int kPadY = 2; // st::msgDateImgPadding.y()
            constexpr int kDelta = 4; // st::msgDateImgDelta
            const int badgeW = durW + 2 * kPadX;
            const int badgeH = dfm.height() + 2 * kPadY;
            const int badgeX = imgRect.right() - badgeW - kDelta + 1;
            const int badgeY = imgRect.bottom() - badgeH - kDelta + 1;
            p.setPen(Qt::NoPen);
            p.setBrush(st::historyMediaDurationBg);
            p.drawRoundedRect(QRect(badgeX, badgeY, badgeW, badgeH), 4, 4);
            p.setFont(st::msgDateFont);
            p.setPen(st::msgDateImgFg);
            p.drawText(badgeX + kPadX, badgeY + kPadY + dfm.ascent(), duration);
        }
    }

    return loadingImage;
}

// ─── End Link Preview Card ───────────────────────────────────────────

/// Paint a complete reply block (decoration + optional thumbnail + name + text).
void paintReplyBlock(
    QPainter &p,
    const ReplyPreviewData &data,
    QRect replyRect,
    bool isOutgoing,
    qreal dpr,
    QWidget *repaintTarget = nullptr,
    const QRect &repaintRect = QRect())
{
    const auto replyHeight = replyRect.height();
    paintReplyDecoration(p, replyRect, isOutgoing);

    const auto &decoStyle = st::messageQuoteStyle;
    const auto &replyPadding = st::historyReplyPadding;
    const auto thumbSize = st::historyReplyPreview;
    const auto &thumbMargin = st::historyReplyPreviewMargin;

    // Compute text left edge using historyReplyPadding (11px from rect left).
    int textLeft = replyRect.left() + replyPadding.left();
    const int textRight = replyRect.right() - replyPadding.right();

    if (data.hasThumb) {
        // Draw thumbnail (aspect-ratio-preserving, centered in 32×32).
        const int thumbX = replyRect.left() + decoStyle.outline + thumbMargin.left();
        const int thumbY = replyRect.top() + decoStyle.padding.top() + thumbMargin.top();
        const QRect thumbRect(thumbX, thumbY, thumbSize, thumbSize);

        // Cache prepared reply thumbnails keyed by URL + DPR.
        // Only successful thumbnails are cached; null results are
        // not stored so that a later MediaCache resolve can recover.
        static QHash<QString, QImage> replyThumbCache;
        const auto cacheKey = data.thumbUrl + QChar(':') + QString::number(dpr);
        auto it = replyThumbCache.find(cacheKey);
        if (it == replyThumbCache.end()) {
            // Use MediaCache so mxc:// URLs resolve through the path cache.
            QImage raw = MediaCache::loadImage(data.thumbUrl);
            if (raw.isNull() && repaintTarget) {
                // Not cached — kick an async decode that repaints this row when
                // ready (populates imageCache), instead of sync-decoding on the
                // paint thread. The compose below runs on that repaint.
                MediaCache::loadScaledImageAsync(
                    data.thumbUrl,
                    QSize(thumbSize, thumbSize),
                    dpr,
                    repaintTarget,
                    repaintRect);
            }
            if (!raw.isNull()) {
                // Scale preserving aspect ratio, center in 32×32 container.
                const int tw = raw.width(), th = raw.height();
                const auto fitSize = (tw > th)
                    ? QSize(tw * thumbSize / th, thumbSize)
                    : QSize(thumbSize, th * thumbSize / tw);
                auto scaled = raw.scaled(
                    fitSize * dpr,
                    Qt::KeepAspectRatio,
                    Qt::SmoothTransformation);
                // Compose into a 32×32 transparent container, centered.
                const int outW = qRound(thumbSize * dpr);
                const int outH = qRound(thumbSize * dpr);
                QImage container(outW, outH, QImage::Format_ARGB32_Premultiplied);
                container.fill(Qt::transparent);
                container.setDevicePixelRatio(dpr);
                {
                    QPainter cp(&container);
                    scaled.setDevicePixelRatio(dpr);
                    const int dx = (thumbSize - fitSize.width()) / 2;
                    const int dy = (thumbSize - fitSize.height()) / 2;
                    cp.drawImage(dx, dy, scaled);
                }
                // Bound this image cache independently of the text caches
                // (composed 32×32·dpr thumbnails are heavier per entry); clearing
                // wholesale past the limit is safe — `it` is end() here and is
                // reassigned by the insert below.
                if (replyThumbCache.size() > 512) {
                    replyThumbCache.clear();
                }
                it = replyThumbCache.insert(cacheKey, container);
            }
            // Do not cache null — retry on next paint after media resolves.
        }
        if (it != replyThumbCache.end() && !it->isNull()) {
            PainterHighQualityEnabler hq(p);
            QPainterPath clipPath;
            clipPath.addRoundedRect(QRectF(thumbRect), 4, 4);
            p.save();
            p.setClipPath(clipPath);
            it->setDevicePixelRatio(dpr);
            p.drawImage(thumbRect.topLeft(), *it);
            p.restore();
        } else {
            PainterHighQualityEnabler hq(p);
            p.setPen(Qt::NoPen);
            p.setBrush(isOutgoing ? st::msgOutBgSelected : st::msgInBgSelected);
            p.drawRoundedRect(thumbRect, 4, 4);
        }

        // Shift text to the right of the thumbnail.
        textLeft = thumbX + thumbSize + thumbMargin.right();
    }

    const int textWidth = qMax(0, textRight - textLeft + 1);

    if (data.deleted) {
        // Deleted message: single line, vertically centered, no author.
        p.setFont(st::msgFont);
        p.setPen(st::windowSubTextFg);
        const auto baseline = replyRect.top()
            + (replyHeight - st::msgFont->height) / 2
            + st::msgFont->ascent;
        p.drawText(
            textLeft,
            baseline,
            st::fontMetrics(st::msgFont).elidedText(
                data.text,
                Qt::ElideRight,
                textWidth));
    } else {
        // Name uses service color (msgInServiceFg / msgOutServiceFg).
        const auto nameColor = isOutgoing ? st::msgOutServiceFg : st::msgInServiceFg;

        // Vertically center name + text alongside thumbnail when thumb present.
        int contentStartY;
        if (data.hasThumb) {
            const int thumbAreaTop = replyRect.top() + decoStyle.padding.top() + thumbMargin.top();
            const int textBlockH = st::msgServiceNameFont->height + st::msgFont->height;
            contentStartY = thumbAreaTop + (thumbSize - textBlockH) / 2;
        } else {
            contentStartY = replyRect.top() + replyPadding.top();
        }

        const int titleBaseline = contentStartY + st::msgServiceNameFont->ascent;
        const int previewBaseline = contentStartY
            + st::msgServiceNameFont->height
            + st::msgFont->ascent;

        p.setFont(st::msgServiceNameFont);
        p.setPen(nameColor);
        p.drawText(
            textLeft,
            titleBaseline,
            st::fontMetrics(st::msgServiceNameFont).elidedText(
                data.name,
                Qt::ElideRight,
                textWidth));

        p.setFont(st::msgFont);
        // Colorized text ("Photo", "Video", "File") uses date color
        // (colorized reply text uses msgInDateFg / msgOutDateFg).
        const auto textColor = data.isTextColorized
            ? (isOutgoing ? st::msgOutDateFg : st::msgInDateFg)
            : (isOutgoing ? st::historyTextOutFg : st::historyTextInFg);
        p.setPen(textColor);
        p.drawText(
            textLeft,
            previewBaseline,
            st::fontMetrics(st::msgFont).elidedText(
                data.text.simplified(),
                Qt::ElideRight,
                textWidth));
    }
}

void paintSelectionOverlay(
    QPainter &p,
    const QRectF &bubbleRect,
    BubbleCorners corners,
    BubbleTailSide tail)
{
    PainterHighQualityEnabler hq(p);
    p.setPen(Qt::NoPen);
    p.setBrush(st::msgSelectOverlay);
    p.drawPath(bubblePath(bubbleRect, corners, tail));
}

struct ReactionBarMetrics {
    int bubbleLeft = 0;
    int bubbleWidth = 0;
    int top = 0;
    bool valid = false;
};

// Emoji image cache: renders system emoji text into QImages at
// reactionInlineImage size (32px at 100% scale). Keyed by emoji+scale+dpr.
QHash<QString, QImage> s_emojiImageCache;

struct ReactionPillLayout {
    QRect rect;
    QString key;        // emoji string
    QString countText;  // "1", "2", etc.
    int countWidth = 0; // cached text width
    bool isSelf = false;
};

struct ReactionPillColors {
    QColor bg;
    QColor textFg;
    qreal bgOpacity = 1.0;
};

// Render system emoji into a cached QImage at reactionInlineSize.
// A pre-rendered-sprite approach would use a 32px image with a -7px overflow
// offset. System emoji fonts fill the entire glyph box (no padding), so we
// render at the visual container size (reactionInlineSize = 18px) and draw
// without offset — producing the same visual result.
[[nodiscard]] QImage renderEmojiImage(const QString &emoji) {
    using TeleMatrix::Style::Scale;
    using TeleMatrix::Style::DevicePixelRatio;

    const auto key = emoji
        + QLatin1Char('|')
        + QString::number(Scale())
        + QLatin1Char('|')
        + QString::number(DevicePixelRatio());
    if (const auto it = s_emojiImageCache.constFind(key); it != s_emojiImageCache.cend()) {
        return it.value();
    }

    const auto dpr = DevicePixelRatio();
    const auto size = st::reactionInlineSize; // 18px at 100% (scaled)
    const auto pixelSize = size * dpr;

    QImage img(pixelSize, pixelSize, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    img.setDevicePixelRatio(dpr);
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        // System emoji fonts fill the glyph box. Scale from 14px base
        // (visual emoji size at 100% scale).
        QFont emojiFont;
        emojiFont.setPixelSize(TeleMatrix::Style::ConvertScale(12));
        p.setFont(emojiFont);
        p.drawText(QRect(0, 0, size, size), Qt::AlignCenter, emoji);
    }

    // Bound the emoji sprite cache (tiny 18px entries, but distinct scale/dpr
    // combos could grow it unbounded). Clearing wholesale past the watermark is
    // safe — visible reaction emoji re-render on the next paint.
    if (s_emojiImageCache.size() >= 1024) {
        s_emojiImageCache.clear();
    }
    s_emojiImageCache.insert(key, img);
    return img;
}

// pill bg = msgFileBg, opacity varies by chosen state.
[[nodiscard]] ReactionPillColors pillColors(bool chosen, bool outgoing) {
    if (chosen) {
        return {
            outgoing ? st::msgFileOutBg : st::msgFileInBg,
            outgoing ? st::historyFileOutIconFg : st::historyFileInIconFg,
            1.0,
        };
    }
    return {
        outgoing ? st::msgFileOutBg : st::msgFileInBg,
        outgoing ? st::msgOutServiceFg : st::msgInServiceFg,
        outgoing ? st::reactionOutNonChosenOpacity : st::reactionInNonChosenOpacity,
    };
}

QVector<ReactionPillLayout> layoutReactionPills(
        const TimelineItem &item,
        int bubbleLeft,
        int bubbleWidth,
        int top) {
    QVector<ReactionPillLayout> layouts;
    if (item.reactions.isEmpty()) {
        return layouts;
    }

    const auto &padding = st::reactionInlinePadding;
    const auto size = st::reactionInlineSize;
    const auto pillH = HistoryMessage::reactionPillHeight();
    const auto countFont = static_cast<const QFont &>(st::semiboldFont);
    const auto countFm = QFontMetrics(countFont);

    const auto leftStart = bubbleLeft + kBubblePaddingH + st::reactionInlineInBubbleLeft;
    const auto rightLimit = bubbleLeft + bubbleWidth - kBubblePaddingH;
    auto x = leftStart;
    auto y = top;

    layouts.reserve(item.reactions.size());
    for (const auto &reaction : item.reactions) {
        const auto countText = QString::number(reaction.count);
        const auto countW = countFm.horizontalAdvance(countText);
        const auto pillW = padding.left()
            + size
            + st::reactionInlineSkip
            + countW
            + padding.right();

        if (x + pillW > rightLimit + 1 && x != leftStart) {
            x = leftStart;
            y += pillH + st::reactionInlineBetween;
        }

        layouts.push_back({
            QRect(x, y, pillW, pillH),
            reaction.key,
            countText,
            countW,
            reaction.isSelf,
        });

        x += pillW + st::reactionInlineBetween;
    }
    return layouts;
}

/// Return the total height occupied by reaction pill rows.
int reactionPillsHeight(const QVector<ReactionPillLayout> &layouts) {
    if (layouts.isEmpty()) {
        return 0;
    }
    return layouts.back().rect.bottom() - layouts.front().rect.top() + 1;
}

int reactionPillsWidth(const QVector<ReactionPillLayout> &layouts) {
    if (layouts.isEmpty()) {
        return 0;
    }
    return layouts.back().rect.right() - layouts.front().rect.left() + 1;
}

struct CachedReactionLayout {
    QVector<ReactionPillLayout> pills;
    int bubbleLeft = -1;
    int bubbleWidth = -1;
    int reactionCount = -1;
    size_t contentHash = 0;
};
static QHash<QString, CachedReactionLayout> s_reactionCache;

static const QVector<ReactionPillLayout> &cachedReactionPills(
        const TimelineItem &item,
        int bubbleLeft,
        int bubbleWidth,
        int top) {
    // Content hash: combine emoji keys, counts, and isSelf.
    size_t contentHash = 0;
    for (const auto &r : item.reactions) {
        contentHash ^= qHash(r.key) + 0x9e3779b9 + (contentHash << 6) + (contentHash >> 2);
        contentHash ^= qHash(r.count) + 0x9e3779b9 + (contentHash << 6) + (contentHash >> 2);
        contentHash ^= qHash(r.isSelf) + 0x9e3779b9 + (contentHash << 6) + (contentHash >> 2);
    }

    // Bound growth like the sibling paint caches (s_textCache etc.): otherwise
    // scrolling a heavily-reacted room accumulates one entry per reacted event id
    // until the next room switch. Cap BEFORE taking the entry reference, since
    // capPaintCache may clear the whole map (which would dangle it). See PERF-10.
    capPaintCache(s_reactionCache);
    auto &entry = s_reactionCache[item.eventId];
    if (entry.bubbleLeft == bubbleLeft
        && entry.bubbleWidth == bubbleWidth
        && entry.contentHash == contentHash
        && !entry.pills.isEmpty()) {
        // Cache hit — adjust the top offset if it differs.
        const auto cachedTop = entry.pills.first().rect.top();
        if (cachedTop != top) {
            const auto dy = top - cachedTop;
            for (auto &pill : entry.pills) {
                pill.rect.translate(0, dy);
            }
        }
        return entry.pills;
    }
    // Cache miss — compute and store.
    entry.pills = layoutReactionPills(item, bubbleLeft, bubbleWidth, top);
    entry.bubbleLeft = bubbleLeft;
    entry.bubbleWidth = bubbleWidth;
    entry.reactionCount = item.reactions.size();
    entry.contentHash = contentHash;
    return entry.pills;
}

int paintReactionPills(
        QPainter &p,
        const TimelineItem &item,
        bool isOut,
        int bubbleLeft,
        int bubbleWidth,
        int top,
        bool mediaOnlyNoCaption) {
    if (item.reactions.isEmpty()) {
        return 0;
    }

    const auto &padding = st::reactionInlinePadding;
    const auto size = st::reactionInlineSize;
    const auto pillH = HistoryMessage::reactionPillHeight();
    const auto radius = qreal(pillH) / 2.0; // pill radius = height / 2
    const auto reactionTop = top + st::mediaInBubbleSkip;

    const auto countFont = static_cast<const QFont &>(st::semiboldFont);
    const auto countFm = QFontMetrics(countFont);

    const auto &layouts = cachedReactionPills(
        item,
        bubbleLeft,
        bubbleWidth,
        reactionTop);
    for (const auto &layout : layouts) {
        // 1. Pill background (rounded rect, radius = height/2)
        p.setPen(Qt::NoPen);
        if (mediaOnlyNoCaption && !layout.isSelf) {
            // Non-chosen pill on media-only message: white pill with shadow.
            PainterHighQualityEnabler hq(p);
            p.setBrush(st::reactionMediaShadowBg);
            p.drawRoundedRect(layout.rect.translated(0, 1), radius, radius);
            p.setBrush(st::windowBg);
            p.drawRoundedRect(layout.rect, radius, radius);
        } else {
            // Chosen pill (any context) or in-bubble non-chosen.
            const auto colors = pillColors(layout.isSelf, isOut);
            PainterHighQualityEnabler hq(p);
            // Compose with (and restore) the inherited opacity rather than
            // forcing 1.0: the caller dims a whole message being deleted, and a
            // hard reset would un-fade this pill and everything drawn after it.
            const auto previousOpacity = p.opacity();
            p.setOpacity(previousOpacity * colors.bgOpacity);
            p.setBrush(colors.bg);
            p.drawRoundedRect(layout.rect, radius, radius);
            p.setOpacity(previousOpacity);
        }

        // 2. Emoji image — rendered at reactionInlineSize, drawn at inner origin
        const auto innerLeft = layout.rect.left() + padding.left();
        const auto innerTop = layout.rect.top() + padding.top();
        const auto emojiImg = renderEmojiImage(layout.key);
        if (!emojiImg.isNull()) {
            p.drawImage(innerLeft, innerTop, emojiImg);
        }

        // 3. Count text (semiboldFont, vertically centered)
        p.setFont(countFont);
        if (mediaOnlyNoCaption && !layout.isSelf) {
            p.setPen(st::windowSubTextFg);
        } else {
            const auto colors = pillColors(layout.isSelf, isOut);
            p.setPen(colors.textFg);
        }
        const auto textLeft = innerLeft + size + st::reactionInlineSkip;
        const auto textTop = layout.rect.top()
            + ((layout.rect.height() - countFm.height()) / 2)
            + countFm.ascent();
        p.drawText(textLeft, textTop, layout.countText);
    }

    const auto pillsH = layouts.isEmpty() ? pillH : reactionPillsHeight(layouts);
    return mediaOnlyNoCaption
        ? (st::mediaInBubbleSkip + pillsH + st::mediaInBubbleSkip)
        : (st::mediaInBubbleSkip + pillsH);
}

bool reactionsShareInfoLine(
        const TimelineItem &item,
        int bubbleLeft,
        int bubbleWidth) {
    if (item.reactions.isEmpty()) {
        return false;
    }
    const auto &layouts = cachedReactionPills(item, bubbleLeft, bubbleWidth, 0);
    if (layouts.isEmpty()) {
        return false;
    }
    const auto available = qMax(1, bubbleWidth - (2 * kBubblePaddingH));
    return (reactionPillsWidth(layouts) + infoWidth(item)) <= available;
}

int reactionBlockHeight(
        const TimelineItem &item,
        int bubbleLeft,
        int bubbleWidth,
        bool includeInfo = true) {
    if (item.reactions.isEmpty()) {
        return 0;
    }
    const auto &layouts = cachedReactionPills(item, bubbleLeft, bubbleWidth, 0);
    const auto pillsH = layouts.isEmpty() ? HistoryMessage::reactionPillHeight() : reactionPillsHeight(layouts);
    if (!includeInfo) {
        return st::mediaInBubbleSkip + pillsH + st::mediaInBubbleSkip;
    }
    return st::mediaInBubbleSkip + pillsH + (reactionsShareInfoLine(item, bubbleLeft, bubbleWidth)
        ? 0
        : skipBlockHeight());
}

int mediaOnlyOutsideReactionHeight(
        const TimelineItem &item,
        int bubbleLeft,
        int bubbleWidth,
        bool hasCaption) {
    if (hasCaption || item.reactions.isEmpty()) {
        return 0;
    }
    return reactionBlockHeight(item, bubbleLeft, bubbleWidth, false);
}

bool computeTextBubbleMetrics(
        const TimelineItem &item,
        const MessagePaintContext &context,
        int &bubbleLeft,
        int &bubbleWidth,
        int &textHeight) {
    const auto isOut = item.delivery.outgoing;
    const auto showSender = showSenderName(item, context);
    const auto availWidth = textAvailableWidth(context.width, isOut);
    const auto skipW = skipBlockWidth(item);
    const auto skipH = skipBlockHeight();
    const auto hasReactions = !item.reactions.isEmpty();

    // ─── Use cached layout metrics ───
    const auto &cm = cachedMetrics(item, isOut, availWidth);
    auto lastLineWidth = cm.lastLineWidth;
    auto maxLineWidth = cm.maxLineWidth;
    textHeight = cm.textHeight;

    // ─── Variable-width code blocks (monospace max width) ───
    const auto preNatural = cm.preMaxNaturalWidth;
    const auto preBubbleWidth = preNatural + 2 * kBubblePaddingH;
    // For code block expansion, use minimal margins (just avatar space)
    // so the bubble can use nearly the full viewport width.
    // contentWidth = newWidth - msgMargin.left - wideSkip
    const auto codeReserved = kMarginLeft + kPhotoSkip + 8; // small right gap
    const auto viewportMaxBubble = qMax(kMinBubbleWidth, context.width - codeReserved);
    const auto effectiveMaxBubble = qMin(
        viewportMaxBubble,
        qMax(kMaxBubbleWidth, preBubbleWidth));

    if (preNatural > 0 && preBubbleWidth > kMaxBubbleWidth) {
        const auto widerAvail = effectiveMaxBubble - 2 * kBubblePaddingH;
        if (widerAvail > availWidth) {
            const auto &cm2 = cachedMetrics(item, isOut, widerAvail);
            lastLineWidth = cm2.lastLineWidth;
            maxLineWidth = cm2.maxLineWidth;
            textHeight = cm2.textHeight;
        }
    }

    const auto hasLinkPreview = hasRenderableLinkPreview(item);
    const auto timeInline = !hasReactions && !hasLinkPreview
        && (lastLineWidth + skipW <= availWidth);
    if (!hasReactions && !hasLinkPreview && !timeInline) {
        textHeight += skipH;
    }

    auto bubbleContentWidth = maxLineWidth;
    if (!hasReactions && timeInline) {
        bubbleContentWidth = qMax(bubbleContentWidth, lastLineWidth + skipW);
    } else if (!hasReactions) {
        bubbleContentWidth = qMax(bubbleContentWidth, infoWidth(item));
    }
    if (showSender) {
        bubbleContentWidth = qMax(
            bubbleContentWidth,
            senderRowRequiredWidth(item, false));
    }
    if (hasLinkPreview) {
        bubbleContentWidth = qMax(bubbleContentWidth, linkPreviewMinWidth(item));
    }
    bubbleWidth = qBound(
        kMinBubbleWidth,
        bubbleContentWidth + 2 * kBubblePaddingH,
        effectiveMaxBubble);
    bubbleLeft = bubbleLeftFor(context, isOut, bubbleWidth);
    return true;
}

ReactionBarMetrics computeReactionBarMetrics(
        const TimelineItem &item,
        const MessagePaintContext &context) {
    ReactionBarMetrics result;
    if (item.reactions.isEmpty() || contentType(item) == ContentType::Service) {
        return result;
    }

    const auto isOut = item.delivery.outgoing;
    const auto showSender = showSenderName(item, context);

    switch (contentType(item)) {
    case ContentType::Image: {
        const auto media = photoBubbleMetrics(item, kMaxBubbleWidth, showSender);
        const auto caption = imageCaptionText(item);
        const auto hasCaption = !caption.isEmpty();
        const auto imgFwdH = forwardedHeaderHeight(item, media.bubbleWidth - 2 * kBubblePaddingH);
        auto contentTop = showSender ? (kBubblePaddingV + kSenderNameHeight) : 0;
        if (!showSender && (imgFwdH > 0 || hasReply(item))) {
            contentTop = kBubblePaddingV;
        }
        contentTop += imgFwdH;
        if (hasReply(item)) {
            contentTop += replyHeightFor(item, context) + kReplyPreviewBottomSkip;
        }
        if (contentTop > 0) {
            contentTop += st::mediaInBubbleSkip;
        }
        contentTop += media.mediaHeight;
        if (hasCaption) {
            const auto captionLayout = layoutCaptionLines(
                caption, media.bubbleWidth - 2 * kBubblePaddingH);
            contentTop += st::mediaCaptionSkip
                + int(captionLayout.lines.size()) * st::msgFont->height;
        }

        result.bubbleLeft = bubbleLeftFor(context, isOut, media.bubbleWidth);
        result.bubbleWidth = media.bubbleWidth;
        result.top = contentTop + st::mediaInBubbleSkip;
        result.valid = true;
        return result;
    }
    case ContentType::File: {
        result.bubbleWidth = fileBubbleWidth(item, context.width, isOut);
        auto contentTop = showSender ? kSenderNameHeight : 0;
        contentTop += forwardedHeaderHeight(item, result.bubbleWidth - 2 * kBubblePaddingH);
        if (hasReply(item)) {
            contentTop += replyHeightFor(item, context) + kReplyPreviewBottomSkip;
        }
        contentTop += st::docPaddingTop + st::docThumbSize + st::docPaddingBottom;
        if (!captionText(item).simplified().isEmpty()) {
            const auto captionLayout = layoutCaptionLines(
                captionText(item).simplified(),
                result.bubbleWidth - 2 * kBubblePaddingH);
            contentTop += st::mediaCaptionSkip
                + int(captionLayout.lines.size()) * st::msgFont->height;
        }

        result.bubbleLeft = bubbleLeftFor(context, isOut, result.bubbleWidth);
        result.top = contentTop + st::mediaInBubbleSkip;
        result.valid = true;
        return result;
    }
    case ContentType::Audio: {
        result.bubbleWidth = HistoryViewAudio::audioBubbleWidth(item, context.width, isOut);
        auto contentTop = HistoryViewAudio::bubbleHeight(item, context.width, context);
        contentTop += forwardedHeaderHeight(item, result.bubbleWidth - 2 * kBubblePaddingH);
        if (hasReply(item)) {
            contentTop += replyHeightFor(item, context) + kReplyPreviewBottomSkip;
        }

        result.bubbleLeft = bubbleLeftFor(context, isOut, result.bubbleWidth);
        result.top = contentTop + st::mediaInBubbleSkip;
        result.valid = true;
        return result;
    }
    case ContentType::Video: {
        const auto media = photoBubbleMetrics(item, kMaxBubbleWidth, showSender);
        const auto caption = imageCaptionText(item);
        const auto hasCaption = !caption.isEmpty();
        const auto vidRxFwdH = forwardedHeaderHeight(item, media.bubbleWidth - 2 * kBubblePaddingH);

        auto contentTop = 0;
        if (showSender) {
            contentTop += kBubblePaddingV + kSenderNameHeight;
        }
        if (!showSender && (vidRxFwdH > 0 || hasReply(item))) {
            contentTop = kBubblePaddingV;
        }
        contentTop += vidRxFwdH;
        if (hasReply(item)) {
            contentTop += replyHeightFor(item, context) + kReplyPreviewBottomSkip;
        }
        if (contentTop > 0) {
            contentTop += st::mediaInBubbleSkip;
        }
        contentTop += media.mediaHeight;
        if (hasCaption) {
            const auto captionLayout = layoutCaptionLines(
                caption, media.bubbleWidth - 2 * kBubblePaddingH);
            contentTop += st::mediaCaptionSkip
                + int(captionLayout.lines.size()) * st::msgFont->height;
        }

        result.bubbleWidth = media.bubbleWidth;
        result.bubbleLeft = bubbleLeftFor(context, isOut, result.bubbleWidth);
        result.top = contentTop + st::mediaInBubbleSkip;
        result.valid = true;
        return result;
    }
    case ContentType::Text: {
        auto textHeight = 0;
        if (!computeTextBubbleMetrics(
                item,
                context,
                result.bubbleLeft,
                result.bubbleWidth,
                textHeight)) {
            return result;
        }

        auto contentTop = kBubblePaddingV;
        if (showSender) {
            contentTop += kSenderNameHeight;
        }
        contentTop += forwardedHeaderHeight(item, result.bubbleWidth - 2 * kBubblePaddingH);
        if (hasReply(item)) {
            contentTop += replyHeightFor(item, context) + kReplyPreviewBottomSkip;
        }
        contentTop += textHeight;
        const auto hasLinkPreview = hasRenderableLinkPreview(item);
        if (hasLinkPreview) {
            const auto lpW = result.bubbleWidth - 2 * kBubblePaddingH;
            contentTop += st::mediaInBubbleSkip + linkPreviewBlockHeight(item, lpW);
        }
        result.top = contentTop + st::mediaInBubbleSkip;
        result.valid = true;
        return result;
    }
    case ContentType::Poll: {
        const auto bubbleWidth = bubbleMaxWidth(context.width, isOut);
        const auto bubbleLeft = bubbleLeftFor(context, isOut, bubbleWidth);
        auto contentTop = kBubblePaddingV;
        if (showSender) {
            contentTop += kSenderNameHeight;
        }
        contentTop += forwardedHeaderHeight(item, bubbleWidth - 2 * kBubblePaddingH);
        if (hasReply(item)) {
            contentTop += replyHeightFor(item, context) + kReplyPreviewBottomSkip;
        }
        contentTop += HistoryViewPoll::contentHeight(item, bubbleWidth - 2 * kBubblePaddingH);

        result.bubbleLeft = bubbleLeft;
        result.bubbleWidth = bubbleWidth;
        result.top = contentTop + st::mediaInBubbleSkip;
        result.valid = true;
        return result;
    }
    default:
        return result;
    }
}

QString serviceMessageText(const TimelineItem &item) {
    const auto text = bodyText(item).simplified();
    return text.isEmpty()
        ? QCoreApplication::translate("HistoryMessage", "Service message")
        : text;
}

BubbleGeometry computeBubbleGeometry(
        const TimelineItem &item,
        const MessagePaintContext &context) {
    BubbleGeometry result;

    if (contentType(item) == ContentType::Service) {
        const auto text = serviceMessageText(item);
        const auto &serviceFm = st::fontMetrics(st::msgServiceFont);
        const auto availableWidth = qMax(
            1,
            context.width
                - st::msgServiceMargin.left()
                - st::msgServiceMargin.right());
        const auto maxTextWidth = qMax(
            1,
            availableWidth
                - st::msgServicePadding.left()
                - st::msgServicePadding.right());
        // Service text WRAPS onto multiple centered lines rather than eliding,
        // so "<name> started a call" is never truncated with an ellipsis on a
        // narrow window or long display name.
        const auto bounds = serviceFm.boundingRect(
            QRect(0, 0, maxTextWidth, 20000),
            Qt::TextWordWrap | Qt::AlignHCenter,
            text);
        // Shrink-to-fit for short (single-line) text; wrapped text takes the
        // full available width so the paint re-wraps identically to this
        // measure. +2px absorbs advance rounding so one line never re-wraps.
        const auto textWidth = qMin(
            serviceFm.horizontalAdvance(text) + 2,
            maxTextWidth);
        const auto textHeight = qMax(
            st::msgServiceFont->height,
            bounds.height());
        const auto minBubbleWidth = qMin(
            availableWidth,
            st::msgServicePadding.left() + st::msgServicePadding.right());
        result.width = qBound(
            minBubbleWidth,
            textWidth
                + st::msgServicePadding.left()
                + st::msgServicePadding.right(),
            availableWidth);
        result.height = st::msgServicePadding.top()
            + textHeight
            + st::msgServicePadding.bottom();
        result.top = qMax(0, st::msgServiceMargin.top() - kMarginTop);
        result.left = st::msgServiceMargin.left()
            + ((availableWidth - result.width) / 2);
        result.corners = BubbleCorners{
            st::bubbleRadiusSmall,
            st::bubbleRadiusSmall,
            st::bubbleRadiusSmall,
            st::bubbleRadiusSmall,
        };
        result.tail = BubbleTailSide::None;
        result.valid = true;
        return result;
    }

    const auto isOut = item.delivery.outgoing;
    const auto showSender = showSenderName(item, context);
    result.tail = bubbleTailFor(isOut, context.sameSenderBelow);
    result.corners = bubbleCornersFor(
        isOut,
        context.sameSenderAbove,
        context.sameSenderBelow,
        result.tail);

    switch (contentType(item)) {
    case ContentType::Image: {
        // Photo messages use a rounded bubble bottom (no tail).
        result.tail = bubbleTailFor(isOut, context.sameSenderBelow, true);
        result.corners = bubbleCornersFor(
            isOut,
            context.sameSenderAbove,
            context.sameSenderBelow,
            result.tail);

        const auto media = photoBubbleMetrics(item, kMaxBubbleWidth, showSender);
        const auto caption = imageCaptionText(item);
        const auto hasCaption = !caption.isEmpty();
        const auto bubbleWidth = media.bubbleWidth;
        const auto captionLayout = layoutCaptionLines(
            caption, bubbleWidth - 2 * kBubblePaddingH);
        const auto captionHeight = captionBlockHeight(
            captionLayout, item, bubbleWidth - 2 * kBubblePaddingH,
            !item.reactions.isEmpty());
        const auto bubbleLeft = bubbleLeftFor(context, isOut, bubbleWidth);
        const auto geoFwdH = forwardedHeaderHeight(item, bubbleWidth - 2 * kBubblePaddingH);
        auto contentTop = showSender ? (kBubblePaddingV + kSenderNameHeight) : 0;
        if (!showSender && (geoFwdH > 0 || hasReply(item))) {
            contentTop = kBubblePaddingV;
        }
        contentTop += geoFwdH;
        if (hasReply(item)) {
            contentTop += replyHeightFor(item, context) + kReplyPreviewBottomSkip;
        }
        if (contentTop > 0) {
            contentTop += st::mediaInBubbleSkip;
        }
        auto bubbleH = contentTop + media.mediaHeight;
        if (hasCaption) {
            bubbleH += st::mediaCaptionSkip
                + captionHeight
                + kBubblePaddingV;
        }
        bubbleH += reactionBlockHeight(item, bubbleLeft, bubbleWidth, hasCaption);
        bubbleH -= mediaOnlyOutsideReactionHeight(item, bubbleLeft, bubbleWidth, hasCaption);

        result.left = bubbleLeft;
        result.width = bubbleWidth;
        result.height = bubbleH;
        result.valid = true;
        return result;
    }
    case ContentType::File: {
        const auto bubbleWidth = fileBubbleWidth(item, context.width, isOut);
        const auto bubbleLeft = bubbleLeftFor(context, isOut, bubbleWidth);
        const auto innerH = st::docPaddingTop + st::docThumbSize + st::docPaddingBottom;
        const auto hasFileCaption = !captionText(item).simplified().isEmpty();
        const auto fwdH = forwardedHeaderHeight(item, bubbleWidth - 2 * kBubblePaddingH);
        const auto hasHeaderAboveFile = showSender || fwdH > 0 || hasReply(item);
        auto bubbleH = innerH + (hasHeaderAboveFile ? kBubblePaddingV : 0);
        if (showSender) {
            bubbleH += kSenderNameHeight;
        }
        bubbleH += fwdH;
        if (hasReply(item)) {
            bubbleH += replyHeightFor(item, context) + kReplyPreviewBottomSkip;
        }
        if (hasFileCaption) {
            const auto fileCaptionLayout = layoutCaptionLines(
                captionText(item).simplified(),
                bubbleWidth - 2 * kBubblePaddingH);
            bubbleH += st::mediaCaptionSkip
                + captionBlockHeight(
                    fileCaptionLayout, item,
                    bubbleWidth - 2 * kBubblePaddingH,
                    !item.reactions.isEmpty())
                + kBubblePaddingV;
        }
        bubbleH += reactionBlockHeight(item, bubbleLeft, bubbleWidth);
        result.left = bubbleLeft;
        result.width = bubbleWidth;
        result.height = bubbleH;
        result.valid = true;
        return result;
    }
    case ContentType::Audio: {
        const auto bubbleWidth = HistoryViewAudio::audioBubbleWidth(item, context.width, isOut);
        const auto bubbleLeft = bubbleLeftFor(context, isOut, bubbleWidth);
        auto bubbleH = HistoryViewAudio::bubbleHeight(item, context.width, context);
        bubbleH += forwardedHeaderHeight(item, bubbleWidth - 2 * kBubblePaddingH);
        if (hasReply(item)) {
            bubbleH += replyHeightFor(item, context) + kReplyPreviewBottomSkip;
        }
        bubbleH += reactionBlockHeight(item, bubbleLeft, bubbleWidth);
        result.left = bubbleLeft;
        result.width = bubbleWidth;
        result.height = bubbleH;
        result.valid = true;
        return result;
    }
    case ContentType::Video: {
        result.tail = bubbleTailFor(isOut, context.sameSenderBelow, true);
        result.corners = bubbleCornersFor(
            isOut,
            context.sameSenderAbove,
            context.sameSenderBelow,
            result.tail);

        const auto media = photoBubbleMetrics(item, kMaxBubbleWidth, showSender);
        const auto caption = imageCaptionText(item);
        const auto hasCaption = !caption.isEmpty();
        const auto bubbleWidth = media.bubbleWidth;
        const auto captionLayout = layoutCaptionLines(
            caption, bubbleWidth - 2 * kBubblePaddingH);
        const auto captionHeight = captionBlockHeight(
            captionLayout, item, bubbleWidth - 2 * kBubblePaddingH,
            !item.reactions.isEmpty());
        const auto bubbleLeft = bubbleLeftFor(context, isOut, bubbleWidth);

        const auto vidFwdH = forwardedHeaderHeight(item, bubbleWidth - 2 * kBubblePaddingH);
        auto contentTop = showSender ? (kBubblePaddingV + kSenderNameHeight) : 0;
        if (!showSender && (vidFwdH > 0 || hasReply(item))) {
            contentTop = kBubblePaddingV;
        }
        contentTop += vidFwdH;
        if (hasReply(item)) {
            contentTop += replyHeightFor(item, context) + kReplyPreviewBottomSkip;
        }
        if (contentTop > 0) {
            contentTop += st::mediaInBubbleSkip;
        }
        auto bubbleH = contentTop + media.mediaHeight;
        if (hasCaption) {
            bubbleH += st::mediaCaptionSkip
                + captionHeight
                + kBubblePaddingV;
        }
        bubbleH += reactionBlockHeight(item, bubbleLeft, bubbleWidth, hasCaption);
        bubbleH -= mediaOnlyOutsideReactionHeight(item, bubbleLeft, bubbleWidth, hasCaption);

        result.left = bubbleLeft;
        result.width = bubbleWidth;
        result.height = bubbleH;
        result.valid = true;
        return result;
    }
    case ContentType::Text: {
        const auto hasLinkPreview = hasRenderableLinkPreview(item);
        // Large emoji: return content area (no real bubble).
        if (context.largeEmojiEnabled
            && !hasReply(item)
            && forwardedSenderName(item).isEmpty()
            && !hasLinkPreview
            && shouldRenderLargeEmoji(bodyText(item), true)) {
            const auto iso = detectIsolatedEmoji(bodyText(item));
            if (iso.valid) {
                const auto emojiW = largeEmojiWidth(iso);
                const auto pillW = infoWidth(item) + 2 * st::msgDateImgPadding.x();
                // Pill is inline to the right of emoji.
                const auto contentW = emojiW + st::msgDateImgDelta + pillW;
                const auto emojiH = largeEmojiHeight();
                auto totalH = (showSender ? kSenderNameHeight : 0) + emojiH;
                totalH += reactionBlockHeight(item, bubbleLeftFor(context, isOut, contentW), contentW);
                result.left = bubbleLeftFor(context, isOut, contentW);
                result.width = contentW;
                result.height = totalH;
                result.tail = BubbleTailSide::None;
                result.valid = true;
                return result;
            }
        }

        auto textHeight = 0;
        auto bubbleLeft = 0;
        auto bubbleWidth = 0;
        if (!computeTextBubbleMetrics(item, context, bubbleLeft, bubbleWidth, textHeight)) {
            return result;
        }
        auto bubbleH = kBubblePaddingV + textHeight + kBubblePaddingV;
        if (showSender) {
            bubbleH += kSenderNameHeight;
        }
        bubbleH += forwardedHeaderHeight(item, bubbleWidth - 2 * kBubblePaddingH);
        if (hasReply(item)) {
            bubbleH += replyHeightFor(item, context) + kReplyPreviewBottomSkip;
        }
        if (hasLinkPreview) {
            const auto lpWidth = bubbleWidth - 2 * kBubblePaddingH;
            bubbleH += st::mediaInBubbleSkip + linkPreviewBlockHeight(item, lpWidth);
            if (item.reactions.isEmpty()) {
                bubbleH += skipBlockHeight();
            }
        }
        bubbleH += reactionBlockHeight(item, bubbleLeft, bubbleWidth);

        result.left = bubbleLeft;
        result.width = bubbleWidth;
        result.height = bubbleH;
        result.valid = true;
        return result;
    }
    case ContentType::Poll: {
        const auto bubbleWidth = bubbleMaxWidth(context.width, isOut);
        const auto bubbleLeft = bubbleLeftFor(context, isOut, bubbleWidth);
        auto bubbleH = kBubblePaddingV;
        if (showSender) {
            bubbleH += kSenderNameHeight;
        }
        bubbleH += forwardedHeaderHeight(item, bubbleWidth - 2 * kBubblePaddingH);
        if (hasReply(item)) {
            bubbleH += replyHeightFor(item, context) + kReplyPreviewBottomSkip;
        }
        bubbleH += HistoryViewPoll::contentHeight(item, bubbleWidth - 2 * kBubblePaddingH);
        bubbleH += reactionBlockHeight(item, bubbleLeft, bubbleWidth);

        result.left = bubbleLeft;
        result.width = bubbleWidth;
        result.height = bubbleH;
        result.valid = true;
        return result;
    }
    default:
        return result;
    }
}

void paintServiceMessage(
        QPainter &p,
        const TimelineItem &item,
        const MessagePaintContext &context) {
    const auto geometry = computeBubbleGeometry(item, context);
    if (!geometry.valid) {
        return;
    }

    {
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(st::msgServiceBg);
        p.drawRoundedRect(
            geometry.left,
            geometry.top,
            geometry.width,
            geometry.height,
            st::bubbleRadiusSmall,
            st::bubbleRadiusSmall);
    }

    // Wrapped, centered service text — matches computeBubbleGeometry's measure
    // (short text stays one line; long text wraps instead of being elided).
    const QRect textRect(
        geometry.left + st::msgServicePadding.left(),
        geometry.top + st::msgServicePadding.top(),
        qMax(1,
            geometry.width
                - st::msgServicePadding.left()
                - st::msgServicePadding.right()),
        qMax(1,
            geometry.height
                - st::msgServicePadding.top()
                - st::msgServicePadding.bottom()));
    p.setFont(st::msgServiceFont);
    p.setPen(st::msgServiceFg);
    p.drawText(
        textRect,
        Qt::TextWordWrap | Qt::AlignHCenter | Qt::AlignVCenter,
        serviceMessageText(item));
}

const QFont &italicStatusMessageFont() {
    static const auto font = [] {
        QFont result(st::msgFont);
        result.setItalic(true);
        return result;
    }();
    return font;
}

// Layout metrics for a status-message bubble (UTD / deleted). Short text stays on
// a single line with the timestamp inline (original behaviour). Longer text (e.g.
// a cause-specific "unable to decrypt" explanation) wraps, reserving room on the
// right of each line for the bottom-info timestamp block so they don't overlap.
struct StatusTextLayout {
    int drawWidth = 0;           // width the text is wrapped/drawn within
    int bubbleContentWidth = 0;  // drawWidth plus the timestamp skip block
    int textHeight = 0;          // total text height (at least one line)
    bool wraps = false;
};

[[nodiscard]] StatusTextLayout statusTextLayout(
        const QString &text,
        const QFont &font,
        int skipW) {
    const auto maxContentWidth = kMaxBubbleWidth - 2 * kBubblePaddingH;
    const auto &fm = st::fontMetrics(font);
    const auto naturalWidth = fm.horizontalAdvance(text);
    if (naturalWidth + skipW <= maxContentWidth) {
        return { naturalWidth, naturalWidth + skipW, st::msgFont->height, false };
    }
    const auto drawWidth = qMax(kMinBubbleWidth, maxContentWidth - skipW);
    const auto bounds = fm.boundingRect(
        QRect(0, 0, drawWidth, 0),
        Qt::TextWordWrap,
        text);
    return { drawWidth, maxContentWidth, qMax(bounds.height(), st::msgFont->height), true };
}

void paintStatusMessage(
        QPainter &p,
        const TimelineItem &item,
        const MessagePaintContext &context,
        qreal dpr,
        const QString &text,
        const QFont &font,
        const QColor &textColor,
        bool shimmer) {
    const auto isOut = item.delivery.outgoing;
    const auto showSender = showSenderName(item, context);
    const auto tail = bubbleTailFor(isOut, context.sameSenderBelow);
    const auto corners = bubbleCornersFor(
        isOut,
        context.sameSenderAbove,
        context.sameSenderBelow,
        tail);
    const auto skipW = skipBlockWidth(item);
    const auto layout = statusTextLayout(text, font, skipW);
    auto bubbleContentWidth = layout.bubbleContentWidth;
    if (showSender) {
        bubbleContentWidth = qMax(
            bubbleContentWidth,
            senderRowRequiredWidth(item, false /* reply pill is floating, no sender-row reservation */));
    }
    const auto bubbleWidth = qBound(
        kMinBubbleWidth,
        bubbleContentWidth + 2 * kBubblePaddingH,
        kMaxBubbleWidth);
    const auto bubbleLeft = bubbleLeftFor(context, isOut, bubbleWidth);
    const auto bubbleHeight = kBubblePaddingV
        + (showSender ? kSenderNameHeight : 0)
        + layout.textHeight
        + kBubblePaddingV;
    const QRectF bubbleRect(bubbleLeft, 0, bubbleWidth, bubbleHeight);

    paintBubbleLayer(
        p,
        bubbleRect,
        corners,
        tail,
        isOut ? st::msgOutShadow : st::msgInShadow,
        kBubbleShadow);
    paintBubbleLayer(
        p,
        bubbleRect,
        corners,
        tail,
        isOut ? st::msgOutBg : st::msgInBg);

    if (shimmer) {
        auto shimmerColor = isOut ? st::msgOutBg : st::msgInBg;
        shimmerColor = shimmerColor.lighter(115);
        shimmerColor.setAlpha(40);
        p.save();
        p.setClipRect(bubbleRect);
        p.fillRect(bubbleRect, shimmerColor);
        p.restore();
    }

    auto contentTop = kBubblePaddingV;
    if (showSender) {
        const auto reserveFastReply = false /* reply pill is floating, no sender-row reservation */;
        p.setFont(st::msgNameFont);
        p.setPen(senderColor(item.sender.id));
        p.drawText(
            bubbleLeft + kBubblePaddingH,
            contentTop + st::msgNameFont->ascent,
            elidedSenderName(item, bubbleWidth, reserveFastReply));
        contentTop += kSenderNameHeight;
    }

    p.setFont(font);
    p.setPen(textColor);
    if (layout.wraps) {
        p.drawText(
            QRect(bubbleLeft + kBubblePaddingH, contentTop, layout.drawWidth, layout.textHeight),
            Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop,
            text);
    } else {
        p.drawText(
            bubbleLeft + kBubblePaddingH,
            contentTop + st::msgFont->ascent,
            text);
    }

    const auto timeStr = formatTime(item.timestamp);
    const auto &timeFm = st::fontMetrics(st::msgDateFont);
    const auto bottomInfoW = infoWidth(item);
    const auto infoRight = bubbleLeft + bubbleWidth
        - kBubblePaddingH + kDateDeltaX;
    const auto infoBottom = bubbleHeight
        - kBubblePaddingV + kDateDeltaY;
    const auto dateTop = infoBottom - timeFm.height();
    const auto dateX = infoRight - bottomInfoW;
    const auto dateY = dateTop + timeFm.ascent();
    paintBottomInfo(
        p,
        item,
        dateX,
        dateTop,
        dateY,
        timeStr,
        timeFm,
        dpr,
        normalBottomInfoPalette(isOut),
        context.sendingAnimationProgress);
}

constexpr int kUtdTitleGap = 5;
constexpr int kUtdLinkGap = 7;
constexpr int kUtdTimeGap = 3;

[[nodiscard]] bool utdCauseAllowsVerification(int cause) {
    return cause == 2
        || cause == 3
        || cause == 6
        || cause == 8;
}

[[nodiscard]] QString utdDecryptingText(int dotPhase) {
    return QCoreApplication::translate("HistoryMessage", "Decrypting message")
        + QString(QChar('.')).repeated((dotPhase % 3) + 1);
}

struct UtdCardLayout {
    bool showBody = false;
    bool showLink = false;
    QString title;
    QString body;
    QString linkText;
    int contentWidth = 0;
    int titleHeight = 0;
    int bodyHeight = 0;
    int linkHeight = 0;
    int bubbleWidth = 0;
    int bubbleHeight = 0;
};

[[nodiscard]] int utdWrappedHeight(const QString &text, const QFont &font, int width) {
    if (text.isEmpty() || width <= 0) {
        return st::msgFont->height;
    }
    const auto h = st::fontMetrics(font)
        .boundingRect(QRect(0, 0, width, 0), Qt::TextWordWrap, text)
        .height();
    return qMax(h, st::msgFont->height);
}

[[nodiscard]] UtdCardLayout computeUtdCardLayout(const TimelineItem &item) {
    UtdCardLayout L;
    const auto maxContentWidth = kMaxBubbleWidth - 2 * kBubblePaddingH;

    const auto *utd = unableToDecryptContent(item);
    const auto cause = utd ? utd->cause : 0;
    L.title = QCoreApplication::translate("HistoryMessage", "Unable to decrypt message");
    L.showBody = utd && !utd->body.isEmpty();
    if (L.showBody) {
        L.body = utd->body;
    }
    L.showLink = utdCauseAllowsVerification(cause);
    if (L.showLink) {
        L.linkText = QCoreApplication::translate("HistoryMessage", "Verify this device");
    }

    L.titleHeight = st::msgNameFont->height;
    const auto timeStr = formatTime(item.timestamp);
    auto naturalW = st::fontMetrics(st::msgNameFont).horizontalAdvance(L.title);
    naturalW = qMax(naturalW,
        st::fontMetrics(st::msgNameFont).horizontalAdvance(utdDecryptingText(2)));
    naturalW = qMax(naturalW,
        st::fontMetrics(st::msgDateFont).horizontalAdvance(timeStr));
    if (L.showBody) {
        naturalW = qMax(naturalW,
            st::fontMetrics(st::msgFont).horizontalAdvance(L.body));
    }
    if (L.showLink) {
        naturalW = qMax(naturalW,
            st::fontMetrics(st::msgFont).horizontalAdvance(L.linkText));
    }
    L.contentWidth = qMin(naturalW, maxContentWidth);
    L.bodyHeight = L.showBody ? utdWrappedHeight(L.body, st::msgFont, L.contentWidth) : 0;
    L.linkHeight = L.showLink ? st::msgFont->height : 0;
    L.bubbleWidth = qBound(
        kMinBubbleWidth, L.contentWidth + 2 * kBubblePaddingH, kMaxBubbleWidth);
    L.bubbleHeight = kBubblePaddingV
        + L.titleHeight
        + (L.showBody ? (kUtdTitleGap + L.bodyHeight) : 0)
        + (L.showLink ? (kUtdLinkGap + L.linkHeight) : 0)
        + kUtdTimeGap + st::msgDateFont->height
        + kBubblePaddingV;
    return L;
}

// A decrypting message renders as a contentless "preloader" bubble whose width
// varies message-to-message so the column reads as a skeleton loader. Only the
// width varies: the height must stay the card's, see paintUnableToDecryptMessage.
// Deterministic per event id so it doesn't jitter between repaints.
[[nodiscard]] int decryptingSkeletonWidth(const TimelineItem &item) {
    const auto minW = kMaxBubbleWidth / 4;
    const auto span = kMaxBubbleWidth / 2;
    return minW + int(qHash(item.eventId, 0x9E3779B9u) % uint(span + 1));
}

// Glowing "preloader" fill for a message being decrypted — a path-shift
// gradient effect using msgServiceBg / msgServiceBgSelected, which are themed +
// semi-transparent so they blend with the chat background. A 3-stop gradient
// {bg, selected@0.5, bg} slides across
// over 1000ms, then waits 1000ms (flat bg) — a 2s cycle. The band is
// msgMaxWidth/2 wide and sweeps the whole column, so every placeholder glows
// in sync (one light passing down the list). No text/time is drawn.
void paintDecryptingGlow(
        QPainter &p,
        const QRectF &bubbleRect,
        const BubbleCorners &corners,
        BubbleTailSide tail,
        int viewportLeft,
        qreal progress) {
    auto path = bubblePath(bubbleRect, corners, tail);
    if (path.isEmpty()) {
        return;
    }
    PainterHighQualityEnabler hq(p);
    p.setPen(Qt::NoPen);
    constexpr qreal kSlidePortion = 0.5; // 1000ms slide of the 2000ms cycle
    if (progress < kSlidePortion) {
        const auto slide = progress / kSlidePortion;
        const auto viewportWidth = qreal(kMaxBubbleWidth);
        const auto gradientWidth = qreal(kMaxBubbleWidth) / 2.0;
        const auto startX = (viewportLeft - gradientWidth)
            + slide * (viewportWidth + gradientWidth);
        QLinearGradient gradient(startX, 0, startX + gradientWidth, 0);
        gradient.setColorAt(0.0, st::msgServiceBg);
        gradient.setColorAt(0.5, st::msgServiceBgSelected);
        gradient.setColorAt(1.0, st::msgServiceBg);
        p.setBrush(gradient);
    } else {
        p.setBrush(st::msgServiceBg);
    }
    p.drawPath(path);
}

void paintUnableToDecryptMessage(
        QPainter &p,
        const TimelineItem &item,
        const MessagePaintContext &context,
        qreal dpr) {
    const auto isOut = item.delivery.outgoing;
    const auto tail = bubbleTailFor(isOut, context.sameSenderBelow);
    const auto corners = bubbleCornersFor(
        isOut, context.sameSenderAbove, context.sameSenderBelow, tail);

    const auto L = computeUtdCardLayout(item);

    if (context.itemGlowActive) {
        // Transient "decrypting" state → contentless glowing preloader bubble.
        // The height MUST be the card's: messageHeight() reserves the card height
        // for both states so the 55s give-up is a repaint rather than a relayout,
        // and painting a shorter bubble here just leaves dead space under every
        // shimmer. Only the width varies, which is what gives the skeleton look.
        const auto skeletonW = decryptingSkeletonWidth(item);
        const auto skeletonLeft = bubbleLeftFor(context, isOut, skeletonW);
        const QRectF skeletonRect(skeletonLeft, 0, skeletonW, L.bubbleHeight);
        paintBubbleLayer(
            p, skeletonRect, corners, tail,
            isOut ? st::msgOutShadow : st::msgInShadow, kBubbleShadow);
        paintDecryptingGlow(
            p, skeletonRect, corners, tail,
            skeletonLeft, context.decryptingGlowProgress);
        return;
    }

    const auto bubbleLeft = bubbleLeftFor(context, isOut, L.bubbleWidth);
    const QRectF bubbleRect(bubbleLeft, 0, L.bubbleWidth, L.bubbleHeight);

    paintBubbleLayer(
        p, bubbleRect, corners, tail,
        isOut ? st::msgOutShadow : st::msgInShadow, kBubbleShadow);
    paintBubbleLayer(p, bubbleRect, corners, tail, st::utdBg);

    const auto textLeft = bubbleLeft + kBubblePaddingH;

    const auto timeStr = formatTime(item.timestamp);
    const auto &timeFm = st::fontMetrics(st::msgDateFont);
    const auto timeW = timeFm.horizontalAdvance(timeStr);
    auto timeColor = st::utdBodyFg;
    timeColor.setAlpha(150);
    p.setFont(st::msgDateFont);
    p.setPen(timeColor);
    p.drawText(
        bubbleLeft + L.bubbleWidth - kBubblePaddingH - timeW,
        L.bubbleHeight - kBubblePaddingV - timeFm.descent(),
        timeStr);

    auto y = kBubblePaddingV;
    p.setFont(st::msgNameFont);
    p.setPen(st::utdTitleFg);
    p.drawText(
        QRect(textLeft, y, L.contentWidth, L.titleHeight),
        Qt::AlignLeft | Qt::AlignTop,
        L.title);
    y += L.titleHeight;

    if (L.showBody) {
        y += kUtdTitleGap;
        p.setFont(st::msgFont);
        p.setPen(st::utdBodyFg);
        p.drawText(
            QRect(textLeft, y, L.contentWidth, L.bodyHeight),
            Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop,
            L.body);
        y += L.bodyHeight;
    }

    if (L.showLink) {
        y += kUtdLinkGap;
        p.setFont(st::msgFont);
        p.setPen(st::utdLinkFg);
        p.drawText(
            QRect(textLeft, y, L.contentWidth, L.linkHeight),
            Qt::AlignLeft | Qt::AlignTop,
            L.linkText);
    }
}

void paintPollMessage(
        QPainter &p,
        const TimelineItem &item,
        const MessagePaintContext &context,
        qreal dpr,
        const std::function<int(int, int, int, bool)> &drawReactions) {
    const auto isOut = item.delivery.outgoing;
    const auto showSender = showSenderName(item, context);
    const auto tail = bubbleTailFor(isOut, context.sameSenderBelow);
    const auto corners = bubbleCornersFor(
        isOut,
        context.sameSenderAbove,
        context.sameSenderBelow,
        tail);
    const auto bubbleWidth = bubbleMaxWidth(context.width, isOut);
    const auto bubbleLeft = bubbleLeftFor(context, isOut, bubbleWidth);
    const auto innerWidth = bubbleWidth - 2 * kBubblePaddingH;
    const auto pollContentHeight = HistoryViewPoll::contentHeight(item, innerWidth);
    const auto fwdHeight = forwardedHeaderHeight(item, innerWidth);
    const auto replyData = hasReply(item)
        ? resolveReplyData(item, context)
        : ReplyPreviewData{};
    const auto replyHeight = !hasReply(item)
        ? 0
        : (replyData.deleted ? replyDeletedHeight() : replyPreviewHeight(replyData.hasThumb));

    auto bubbleH = kBubblePaddingV;
    if (showSender) {
        bubbleH += kSenderNameHeight;
    }
    bubbleH += fwdHeight;
    if (replyHeight > 0) {
        bubbleH += replyHeight + kReplyPreviewBottomSkip;
    }
    bubbleH += pollContentHeight;
    bubbleH += reactionBlockHeight(item, bubbleLeft, bubbleWidth);
    if (!item.reactions.isEmpty()) {
        bubbleH += kBubblePaddingV; // bottom padding after reactions
    }

    const QRectF pollBubbleRect(bubbleLeft, 0, bubbleWidth, bubbleH);
    paintBubbleLayer(
        p,
        pollBubbleRect,
        corners,
        tail,
        isOut ? st::msgOutShadow : st::msgInShadow,
        kBubbleShadow);
    paintBubbleLayer(
        p,
        pollBubbleRect,
        corners,
        tail,
        isOut ? st::msgOutBg : st::msgInBg);

    auto contentTop = kBubblePaddingV;
    if (showSender) {
        const auto reserveFastReply = false /* reply pill is floating, no sender-row reservation */;
        p.setFont(st::msgNameFont);
        p.setPen(senderColor(item.sender.id));
        p.drawText(
            bubbleLeft + kBubblePaddingH,
            contentTop + st::msgNameFont->ascent,
            elidedSenderName(item, bubbleWidth, reserveFastReply));
        if (reserveFastReply && context.isHovered && !context.selectionMode) {
            paintFastReplyAction(
                p,
                isOut,
                context.hoveredFastReply,
                bubbleLeft,
                bubbleWidth,
                contentTop);
        }
        contentTop += kSenderNameHeight;
    }

    if (fwdHeight > 0) {
        const auto fwdStr = forwardedText(item);
        p.setFont(st::msgServiceFont);
        p.setPen(isOut ? st::msgOutServiceFg : st::msgInServiceFg);
        const auto &fwdFm = st::fontMetrics(st::msgServiceFont);
        p.drawText(
            bubbleLeft + kBubblePaddingH,
            contentTop + st::msgServiceFont->ascent,
            fwdFm.elidedText(fwdStr, Qt::ElideRight, innerWidth));
        contentTop += fwdHeight;
    }

    if (replyHeight > 0) {
        const QRect replyRect(
            bubbleLeft + kBubblePaddingH,
            contentTop,
            innerWidth,
            replyHeight);
        paintReplyBlock(p, replyData, replyRect, isOut, dpr,
            context.paintTarget, context.repaintTargetRect);
        contentTop += replyHeight + kReplyPreviewBottomSkip;
    }

    HistoryViewPoll::paintContent(
        p,
        item,
        context,
        bubbleLeft + kBubblePaddingH,
        contentTop,
        innerWidth,
        isOut);

    const auto afterPollTop = contentTop + pollContentHeight;
    if (!item.reactions.isEmpty()) {
        drawReactions(bubbleLeft, bubbleWidth, afterPollTop, false);
    }

    const auto timeStr = formatTime(item.timestamp);
    const auto &timeFm = st::fontMetrics(st::msgDateFont);
    const auto bottomInfoWidth = infoWidth(item);
    const auto infoRight = bubbleLeft + bubbleWidth - kBubblePaddingH + kDateDeltaX;
    const auto infoBottom = bubbleH - kBubblePaddingV + kDateDeltaY;
    const auto dateTop = infoBottom - timeFm.height();
    const auto dateX = infoRight - bottomInfoWidth;
    const auto dateY = dateTop + timeFm.ascent();
    paintBottomInfo(
        p,
        item,
        dateX,
        dateTop,
        dateY,
        timeStr,
        timeFm,
        dpr,
        normalBottomInfoPalette(isOut),
        context.sendingAnimationProgress);

    if (context.selectionMode && context.messageSelected) {
        paintSelectionOverlay(p, pollBubbleRect, corners, tail);
    }
}

bool tryPaintLargeEmojiMessage(
        QPainter &p,
        const TimelineItem &item,
        const MessagePaintContext &context,
        qreal dpr,
        const std::function<int(int, int, int, bool)> &drawReactions) {
    if (!context.largeEmojiEnabled
        || contentType(item) != ContentType::Text
        || hasReply(item)
        || !forwardedSenderName(item).isEmpty()
        || hasRenderableLinkPreview(item)
        || !shouldRenderLargeEmoji(bodyText(item), true)) {
        return false;
    }

    const auto isolated = detectIsolatedEmoji(bodyText(item));
    if (!isolated.valid) {
        return false;
    }

    const auto isOut = item.delivery.outgoing;
    const auto showSender = showSenderName(item, context);
    const auto emojiW = largeEmojiWidth(isolated);
    const auto emojiH = largeEmojiHeight();
    const auto timeStr = formatTime(item.timestamp);
    const auto &timeFm = st::fontMetrics(st::msgDateFont);
    const auto bottomInfoW = infoWidth(item);
    const auto pillW = bottomInfoW + 2 * st::msgDateImgPadding.x();
    const auto pillH = timeFm.height() + 2 * st::msgDateImgPadding.y();
    // Pill is inline to the right of emoji.
    const auto contentW = emojiW + st::msgDateImgDelta + pillW;

    // Position like a bubble but without drawing one.
    const auto contentLeft = bubbleLeftFor(context, isOut, contentW);

    auto contentTop = 0;

    // Sender name (for group incoming).
    if (showSender) {
        const auto reserveFastReply = false /* reply pill is floating, no sender-row reservation */;
        p.setFont(st::msgNameFont);
        p.setPen(senderColor(item.sender.id));
        p.drawText(
            contentLeft,
            contentTop + st::msgNameFont->ascent,
            elidedSenderName(item, contentW, reserveFastReply));
        contentTop += kSenderNameHeight;
    }

    // Paint the large emoji and timestamp pill (isolated painter state).
    {
        p.save();
        paintLargeEmoji(
            p,
            isolated,
            contentLeft,
            contentTop,
            emojiW);

        // Floating timestamp pill — inline to the right, bottom-aligned.
        // Show only on hover.
        const auto showInfo = context.isHovered;
        if (showInfo) {
            const auto pillLeft = contentLeft + emojiW + st::msgDateImgDelta;
            const auto pillTop = contentTop + emojiH - pillH;
            const QRect dateRect(pillLeft, pillTop, pillW, pillH);
            {
                PainterHighQualityEnabler hq(p);
                p.setPen(Qt::NoPen);
                p.setBrush(st::msgServiceBg);
                p.drawRoundedRect(dateRect, pillH / 2.0, pillH / 2.0);
            }
            const auto infoTop = dateRect.top() + st::msgDateImgPadding.y();
            const auto infoBaseline = infoTop + timeFm.ascent();
            paintBottomInfo(
                p,
                item,
                dateRect.left() + st::msgDateImgPadding.x(),
                infoTop,
                infoBaseline,
                timeStr,
                timeFm,
                dpr,
                mediaOverlayBottomInfoPalette(),
                context.sendingAnimationProgress);
        }
        p.restore();
    }

    contentTop += emojiH;

    // Reactions below the emoji row.
    if (!item.reactions.isEmpty()) {
        drawReactions(contentLeft, contentW, contentTop, false);
    }

    // Avatar — same rule as regular messages (display the sender photo):
    return true;
}

void paintTextMessage(
        QPainter &p,
        const TimelineItem &item,
        const MessagePaintContext &context,
        qreal dpr,
        const std::function<int(int, int, int, bool)> &drawReactions) {
    const auto isOut = item.delivery.outgoing;
    const auto showSender = showSenderName(item, context);
    const auto tail = bubbleTailFor(isOut, context.sameSenderBelow);
    const auto corners = bubbleCornersFor(
        isOut,
        context.sameSenderAbove,
        context.sameSenderBelow,
        tail);
    const auto hasLinkPreview = hasRenderableLinkPreview(item);
    auto availWidth = textAvailableWidth(context.width, isOut);
    const auto skipW = skipBlockWidth(item);
    const auto skipH = skipBlockHeight();

    // ─── Cached text + metrics: avoid QTextDocument::setHtml() per frame.
    const auto &ct = cachedText(item, isOut);
    const auto &resolved = ct.resolved;

    // Variable-width code blocks: check if pre content needs a wider layout.
    {
        const auto &cmNarrow = cachedMetrics(item, isOut, availWidth);
        const auto preNatural = cmNarrow.preMaxNaturalWidth;
        if (preNatural > 0) {
            const auto preBubble = preNatural + 2 * kBubblePaddingH;
            if (preBubble > kMaxBubbleWidth) {
                const auto codeReserved = kMarginLeft + kPhotoSkip + 8;
                const auto viewportMax = qMax(kMinBubbleWidth, context.width - codeReserved);
                const auto effectiveMax = qMin(viewportMax, preBubble);
                const auto widerAvail = effectiveMax - 2 * kBubblePaddingH;
                if (widerAvail > availWidth) {
                    availWidth = widerAvail;
                }
            }
        }
    }

    const auto &cm = cachedMetrics(item, isOut, availWidth);
    const auto lastLineWidth = cm.lastLineWidth;
    const auto maxLineWidth = cm.maxLineWidth;
    const auto &blockInfos = cm.blockInfos;
    auto textHeight = cm.textHeight;

    const auto &bodyLayout = cachedDrawableLayout(item, isOut, availWidth);

    const auto hasReactions = !item.reactions.isEmpty();
    // Does the timestamp fit inline with the last line?
    const auto timeInline = !hasReactions && !hasLinkPreview
        && (lastLineWidth + skipW <= availWidth);

    if (!hasReactions && !hasLinkPreview && !timeInline) {
        textHeight += skipH;
    }

    // Info dimensions.
    const auto timeStr = formatTime(item.timestamp);
    const auto &timeFm = st::fontMetrics(st::msgDateFont);
    const auto bottomInfoWidth = infoWidth(item);

    // Bubble width: use maxLineWidth from the layout (with ceil rounding).
    auto bubbleContentWidth = maxLineWidth;
    if (!hasReactions && timeInline) {
        bubbleContentWidth = qMax(bubbleContentWidth, lastLineWidth + skipW);
    } else if (!hasReactions) {
        // Timestamp on its own line — bubble must fit it.
        bubbleContentWidth = qMax(bubbleContentWidth, bottomInfoWidth);
    }

    if (showSender) {
        bubbleContentWidth = qMax(
            bubbleContentWidth,
            senderRowRequiredWidth(item, false /* reply pill is floating, no sender-row reservation */));
    }
    if (!forwardedSenderName(item).isEmpty()) {
        const auto &fwdFm = st::fontMetrics(st::msgServiceFont);
        bubbleContentWidth = qMax(
            bubbleContentWidth,
            fwdFm.horizontalAdvance(forwardedText(item)));
    }
    if (hasLinkPreview) {
        bubbleContentWidth = qMax(bubbleContentWidth, linkPreviewMinWidth(item));
    }
    // Use availWidth (which may have been expanded for variable-width code blocks).
    const auto effectiveBubbleMax = availWidth + 2 * kBubblePaddingH;
    const auto bubbleWidth = qBound(
        kMinBubbleWidth,
        bubbleContentWidth + 2 * kBubblePaddingH,
        effectiveBubbleMax);
    const auto bubbleLeft = bubbleLeftFor(context, isOut, bubbleWidth);

    const auto fwdHeight = forwardedHeaderHeight(
        item, bubbleWidth - 2 * kBubblePaddingH);
    const auto lpAvailWidth = bubbleWidth - 2 * kBubblePaddingH;
    const auto lpHeight = hasLinkPreview
        ? linkPreviewBlockHeight(item, lpAvailWidth)
        : 0;
    auto bubbleH = kBubblePaddingV + textHeight + kBubblePaddingV;
    if (showSender) {
        bubbleH += kSenderNameHeight;
    }
    bubbleH += fwdHeight;
    if (lpHeight > 0) {
        bubbleH += st::mediaInBubbleSkip + lpHeight;
        if (item.reactions.isEmpty()) {
            bubbleH += skipBlockHeight();
        }
    }
    if (hasReply(item)) {
        bubbleH += replyHeightFor(item, context) + kReplyPreviewBottomSkip;
    }
    bubbleH += reactionBlockHeight(item, bubbleLeft, bubbleWidth);

    const QRectF bubbleRect(bubbleLeft, 0, bubbleWidth, bubbleH);

    // --- Paint bubble shadow (2px below bubble) ---
    // Clipped: the opaque background below covers all but the exposed sliver.
    paintBubbleLayer(
        p,
        bubbleRect,
        corners,
        tail,
        isOut ? st::msgOutShadow : st::msgInShadow,
        kBubbleShadow,
        true);

    // --- Paint bubble background ---
    paintBubbleLayer(
        p,
        bubbleRect,
        corners,
        tail,
        isOut ? st::msgOutBg : st::msgInBg);

    auto contentTop = kBubblePaddingV;

    // --- Paint sender name (for group incoming) ---
    if (showSender) {
        const auto reserveFastReply = false /* reply pill is floating, no sender-row reservation */;
        p.setFont(st::msgNameFont);
        p.setPen(senderColor(item.sender.id));
        p.drawText(
            bubbleLeft + kBubblePaddingH,
            contentTop + st::msgNameFont->ascent,
            elidedSenderName(item, bubbleWidth, reserveFastReply));
        if (reserveFastReply && context.isHovered && !context.selectionMode) {
            paintFastReplyAction(
                p,
                isOut,
                context.hoveredFastReply,
                bubbleLeft,
                bubbleWidth,
                contentTop);
        }

        contentTop += kSenderNameHeight;
    }

    // --- Paint forwarded header (after sender name, before reply) ---
    if (fwdHeight > 0) {
        const auto fwdStr = forwardedText(item);
        p.setFont(st::msgServiceFont);
        p.setPen(isOut ? st::msgOutServiceFg : st::msgInServiceFg);
        const auto fwdInnerW = bubbleWidth - 2 * kBubblePaddingH;
        const auto &fwdFm = st::fontMetrics(st::msgServiceFont);
        const auto fwdTextWidth = fwdFm.horizontalAdvance(fwdStr);
        const auto fwdLines = (fwdTextWidth > fwdInnerW) ? 2 : 1;
        if (fwdLines == 1) {
            p.drawText(
                bubbleLeft + kBubblePaddingH,
                contentTop + st::msgServiceFont->ascent,
                fwdFm.elidedText(fwdStr, Qt::ElideRight, fwdInnerW));
        } else {
            // Two lines with word wrap; layout cached per (eventId, width) so it
            // isn't rebuilt every paint.
            auto &fwdLayout = cachedForwardedLayout(item.eventId, fwdStr, fwdInnerW);
            fwdLayout.draw(&p, QPointF(bubbleLeft + kBubblePaddingH, contentTop));
        }
        contentTop += fwdHeight;
    }

    if (hasReply(item)) {
        const auto replyData = resolveReplyData(item, context);
        const auto replyHeight = replyData.deleted
            ? replyDeletedHeight()
            : replyPreviewHeight(replyData.hasThumb);
        const QRect replyRect(
            bubbleLeft + kBubblePaddingH,
            contentTop,
            bubbleWidth - 2 * kBubblePaddingH,
            replyHeight);
        paintReplyBlock(p, replyData, replyRect, isOut, dpr,
            context.paintTarget, context.repaintTargetRect);
        contentTop += replyHeight + kReplyPreviewBottomSkip;
    }

    // --- Paint block decorations (behind text) ---
    const auto textLeft = bubbleLeft + kBubblePaddingH;
    const auto decorWidth = bubbleWidth - 2 * kBubblePaddingH;
    for (const auto &bi : blockInfos) {
        const QRect decorRect(
            textLeft,
            contentTop + bi.topY,
            decorWidth,
            bi.bottomY - bi.topY);
        paintBlockDecoration(p, decorRect, bi.type, isOut);
    }

    // --- Paint message text using cached QTextLayout::draw() ---
    p.setFont(st::msgFont);
    p.setPen(isOut ? st::historyTextOutFg : st::historyTextInFg);
    {
        // Build overlay format ranges for hover underline + selection.
        // These don't require re-layout — they're purely visual overlays.
        QList<QTextLayout::FormatRange> overlays;
        if (context.hoveredLinkStart >= 0) {
            for (const auto &link : resolved.links) {
                if (link.start == context.hoveredLinkStart) {
                    QTextLayout::FormatRange underline;
                    underline.start = link.start;
                    underline.length = link.length;
                    underline.format.setFontUnderline(true);
                    overlays.append(underline);
                    break;
                }
            }
        }
        if (context.selectionStart >= 0 && context.selectionEnd > context.selectionStart) {
            QTextLayout::FormatRange selRange;
            selRange.start = context.selectionStart;
            selRange.length = context.selectionEnd - context.selectionStart;
            selRange.format.setBackground(st::msgSelectOverlay);
            overlays.append(selRange);
        }
        // Glow the URL while its link-preview is being fetched: pulse the link's
        // TEXT color (no background) between its normal color and a brighter tint.
        if (context.urlPreviewFetching && !resolved.links.isEmpty()) {
            constexpr qreal kPi = 3.14159265358979323846;
            const auto phase = linkImageGlowPhase();
            const auto t = 0.5 - 0.5 * std::cos(phase * 2.0 * kPi); // 0 -> 1 -> 0
            const QColor base = st::historyLinkInFg; // same glow color for in/out
            const QColor bright = base.lighter(150);
            const QColor mixed(
                base.red()   + int((bright.red()   - base.red())   * t),
                base.green() + int((bright.green() - base.green()) * t),
                base.blue()  + int((bright.blue()  - base.blue())  * t));
            const auto &link = resolved.links.front();
            QTextLayout::FormatRange glowRange;
            glowRange.start = link.start;
            glowRange.length = link.length;
            glowRange.format.setForeground(mixed);
            overlays.append(glowRange);
        }
        bodyLayout.draw(&p, QPointF(textLeft, contentTop), overlays);
    }
    if (context.urlPreviewFetching && !resolved.links.isEmpty()) {
        scheduleContextRepaint(context); // keep the URL glow breathing
    }

    // --- Paint link preview card (after text, before reactions) ---
    auto afterTextTop = contentTop + textHeight;
    if (lpHeight > 0) {
        afterTextTop += st::mediaInBubbleSkip;
        const QRect lpRect(
            bubbleLeft + kBubblePaddingH,
            afterTextTop,
            bubbleWidth - 2 * kBubblePaddingH,
            lpHeight);
        const auto loadingImage = paintLinkPreviewBlock(
            p,
            item,
            lpRect,
            isOut,
            dpr,
            context.paintTarget,
            context.repaintTargetRect);
        if (loadingImage) {
            scheduleContextRepaint(context);
        }
        afterTextTop += lpHeight;
    }

    if (!item.reactions.isEmpty()) {
        drawReactions(bubbleLeft, bubbleWidth, afterTextTop, false);
    }

    // --- Paint timestamp (bottom-right inside bubble) ---
    const auto infoRight = bubbleLeft + bubbleWidth
        - kBubblePaddingH + kDateDeltaX;
    const auto infoBottom = bubbleH
        - kBubblePaddingV + kDateDeltaY;
    const auto dateTop = infoBottom - timeFm.height();
    const auto dateX = infoRight - bottomInfoWidth;
    const auto dateY = dateTop + timeFm.ascent();
    paintBottomInfo(
        p,
        item,
        dateX,
        dateTop,
        dateY,
        timeStr,
        timeFm,
        dpr,
        normalBottomInfoPalette(isOut),
        context.sendingAnimationProgress);

    if (context.selectionMode && context.messageSelected) {
        paintSelectionOverlay(p, bubbleRect, corners, tail);
    }
}

/// Paint a generic (non-audio) file attachment bubble.
/// Extracted from the inline ContentType::File paint path.
void paintGenericAttachment(
    QPainter &p,
    const TimelineItem &item,
    const MessagePaintContext &context,
    bool isOut,
    bool showSender,
    const BubbleCorners &corners,
    BubbleTailSide tail,
    qreal dpr,
    const std::function<int(int, int, int, bool)> &drawReactions)
{
    const auto bubbleWidth = fileBubbleWidth(item, context.width, isOut);
    const auto bubbleLeft = bubbleLeftFor(context, isOut, bubbleWidth);
    const auto innerH = st::docPaddingTop + st::docThumbSize + st::docPaddingBottom;
    const auto fileCaption = captionText(item).simplified();
    const auto hasFileCaption = !fileCaption.isEmpty();
    const auto fileCaptionLayout = layoutCaptionLines(
        fileCaption, bubbleWidth - 2 * kBubblePaddingH);
    const auto fileCaptionHeight = captionBlockHeight(
        fileCaptionLayout, item, bubbleWidth - 2 * kBubblePaddingH,
        !item.reactions.isEmpty());
    const auto fwdHeight = forwardedHeaderHeight(
        item, bubbleWidth - 2 * kBubblePaddingH);
    const auto fileReplyData = hasReply(item)
        ? resolveReplyData(item, context) : ReplyPreviewData{};
    const auto fileReplyH = !hasReply(item) ? 0
        : (fileReplyData.deleted ? replyDeletedHeight() : replyPreviewHeight(fileReplyData.hasThumb));
    const auto hasHeaderAboveFile = showSender || fwdHeight > 0 || fileReplyH > 0;
    auto bubbleH = innerH + (hasHeaderAboveFile ? kBubblePaddingV : 0);
    if (showSender) {
        bubbleH += kSenderNameHeight;
    }
    bubbleH += fwdHeight;
    if (fileReplyH > 0) {
        bubbleH += fileReplyH + kReplyPreviewBottomSkip;
    }
    if (hasFileCaption) {
        bubbleH += st::mediaCaptionSkip + fileCaptionHeight + kBubblePaddingV;
    }
    bubbleH += reactionBlockHeight(item, bubbleLeft, bubbleWidth);
    if (!item.reactions.isEmpty()) {
        bubbleH += kBubblePaddingV; // bottom padding after reactions
    }
    const QRectF bubbleRect(bubbleLeft, 0, bubbleWidth, bubbleH);
    paintBubbleLayer(
        p,
        bubbleRect,
        corners,
        tail,
        isOut ? st::msgOutShadow : st::msgInShadow,
        kBubbleShadow);
    paintBubbleLayer(
        p,
        bubbleRect,
        corners,
        tail,
        isOut ? st::msgOutBg : st::msgInBg);

    // contentTop tracks vertical position inside the bubble.
    // When sender/fwd/reply appear above the file, start with top padding.
    auto contentTop = hasHeaderAboveFile ? kBubblePaddingV : 0;
    if (showSender) {
        p.setFont(st::msgNameFont);
        p.setPen(senderColor(item.sender.id));
        p.drawText(
            bubbleLeft + kBubblePaddingH,
            contentTop + st::msgNameFont->ascent,
            elidedSenderName(item, bubbleWidth, false));
        contentTop += kSenderNameHeight;
    }
    if (fwdHeight > 0) {
        const auto fwdStr = forwardedText(item);
        p.setFont(st::msgServiceFont);
        p.setPen(isOut ? st::msgOutServiceFg : st::msgInServiceFg);
        const auto fwdInnerW = bubbleWidth - 2 * kBubblePaddingH;
        const auto &fwdFm = st::fontMetrics(st::msgServiceFont);
        p.drawText(
            bubbleLeft + kBubblePaddingH,
            contentTop + st::msgServiceFont->ascent,
            fwdFm.elidedText(fwdStr, Qt::ElideRight, fwdInnerW));
        contentTop += fwdHeight;
    }
    if (fileReplyH > 0) {
        const QRect replyRect(
            bubbleLeft + kBubblePaddingH,
            contentTop,
            bubbleWidth - 2 * kBubblePaddingH,
            fileReplyH);
        paintReplyBlock(p, fileReplyData, replyRect, isOut, dpr,
            context.paintTarget, context.repaintTargetRect);
        contentTop += fileReplyH + kReplyPreviewBottomSkip;
    }

    const auto fileName = !mediaFilename(item).isEmpty() ? mediaFilename(item) : bodyText(item);
    const QRect iconRect(
        bubbleLeft + st::docPaddingLeft,
        contentTop + st::docPaddingTop,
        st::docThumbSize,
        st::docThumbSize);
    {
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(isOut ? st::msgFileOutBg : st::msgFileInBg);
        p.drawEllipse(iconRect);
    }
    const auto uploading = showUploadOverlay(item);
    const auto downloadState = downloadVisualState(item);
    const auto downloading = downloadState.active;
    if (uploading) {
        paintUploadOverlay(
            p,
            iconRect,
            item.delivery.uploadProgress,
            dpr,
            st::historyIconFgInverted,
            &st::historyMediaOverlayBg);
    } else if (downloading && downloadState.determinate) {
        const auto color = isOut ? st::historyFileOutIconFg : st::historyFileInIconFg;
        paintCenteredChatIcon(
            p, iconRect, QStringLiteral("history_file_cancel"), dpr, color);
        paintProgressArc(p, iconRect, downloadState.progress, color);
    } else if (downloading) {
        const auto color = isOut ? st::historyFileOutIconFg : st::historyFileInIconFg;
        paintCenteredChatIcon(
            p, iconRect, QStringLiteral("history_file_cancel"), dpr, color);
        paintLoadingArc(p, iconRect, color);
        scheduleContextRepaint(context);
    } else if (!mediaUrl(item).isEmpty()
               && mediaUrl(item).startsWith(QStringLiteral("mxc://"))
               && !MediaCache::isResolved(mediaUrl(item))) {
        // Not yet downloaded — draw download arrow on the circle.
        // history_file_download icon — download arrow.
        const auto color = isOut ? st::historyFileOutIconFg : st::historyFileInIconFg;
        paintCenteredChatIcon(
            p, iconRect, QStringLiteral("history_file_download"), dpr, color);
    } else {
        paintDocumentIcon(p, iconRect, isOut, dpr);
    }

    const auto textLeft = bubbleLeft + st::docNameLeft;
    const auto textWidth = qMax(1, bubbleWidth - st::docNameLeft - st::docPaddingRight);
    p.setFont(st::semiboldFont);
    p.setPen(isOut ? st::historyTextOutFg : st::historyTextInFg);
    const auto elidedName = st::fontMetrics(st::semiboldFont).elidedText(
        fileName,
        Qt::ElideMiddle,
        textWidth);
    p.drawText(
        textLeft,
        contentTop + st::docNameTop + st::semiboldFont->ascent,
        elidedName);

    p.setFont(st::msgFont);
    p.setPen(isOut ? st::mediaOutFg : st::mediaInFg);
    const auto sizeText = uploading
        ? uploadStatusText(item)
        : downloading
            ? downloadStatusText(item, downloadState)
            : fileIdleStatusText(item);
    const auto elidedSize = st::fontMetrics(st::msgFont).elidedText(
        sizeText,
        Qt::ElideRight,
        textWidth);
    p.drawText(
        textLeft,
        contentTop + st::docStatusTop + st::msgFont->ascent,
        elidedSize);

    contentTop += innerH;

    if (hasFileCaption) {
        contentTop += st::mediaCaptionSkip;
        p.setPen(isOut ? st::historyTextOutFg : st::historyTextInFg);
        drawCachedLinkPreviewText(
            p,
            fileCaptionLayout.lines,
            st::msgFont,
            bubbleLeft + kBubblePaddingH,
            contentTop,
            st::msgFont->height);
        contentTop += fileCaptionHeight;
    }

    contentTop += drawReactions(bubbleLeft, bubbleWidth, contentTop, false);

    const auto timeStr = formatTime(item.timestamp);
    const auto &timeFm = st::fontMetrics(st::msgDateFont);
    const auto bottomInfoWidth = infoWidth(item);
    const auto infoTop = bubbleH - kBubblePaddingV + kDateDeltaY - timeFm.height();
    const auto infoLeft = bubbleLeft + bubbleWidth - kBubblePaddingH + kDateDeltaX - bottomInfoWidth;
    const auto infoBaseline = infoTop + timeFm.ascent();
    paintBottomInfo(
        p,
        item,
        infoLeft,
        infoTop,
        infoBaseline,
        timeStr,
        timeFm,
        dpr,
        normalBottomInfoPalette(isOut),
        context.sendingAnimationProgress);

    if (context.selectionMode && context.messageSelected) {
        paintSelectionOverlay(p, bubbleRect, corners, tail);
    }
}

/// Paint an audio file attachment bubble with play/pause and seek bar.
/// Draws an audio/song file attachment.
void paintAudioAttachment(
    QPainter &p,
    const TimelineItem &item,
    const MessagePaintContext &context,
    bool isOut,
    bool showSender,
    const BubbleCorners &corners,
    BubbleTailSide tail,
    qreal dpr,
    const std::function<int(int, int, int, bool)> &drawReactions)
{
    const auto bubbleWidth = fileBubbleWidth(item, context.width, isOut);
    const auto bubbleLeft = bubbleLeftFor(context, isOut, bubbleWidth);
    const auto innerH = st::docPaddingTop + st::docThumbSize + st::docPaddingBottom;
    const auto fwdHeight = forwardedHeaderHeight(
        item, bubbleWidth - 2 * kBubblePaddingH);
    const auto audioReplyData = hasReply(item)
        ? resolveReplyData(item, context) : ReplyPreviewData{};
    const auto audioReplyH = !hasReply(item) ? 0
        : (audioReplyData.deleted ? replyDeletedHeight() : replyPreviewHeight(audioReplyData.hasThumb));
    auto bubbleH = innerH;
    if (showSender) {
        bubbleH += kSenderNameHeight;
    }
    bubbleH += fwdHeight;
    if (audioReplyH > 0) {
        bubbleH += audioReplyH + kReplyPreviewBottomSkip;
    }
    bubbleH += reactionBlockHeight(item, bubbleLeft, bubbleWidth);
    const QRectF bubbleRect(bubbleLeft, 0, bubbleWidth, bubbleH);
    paintBubbleLayer(
        p,
        bubbleRect,
        corners,
        tail,
        isOut ? st::msgOutShadow : st::msgInShadow,
        kBubbleShadow);
    paintBubbleLayer(
        p,
        bubbleRect,
        corners,
        tail,
        isOut ? st::msgOutBg : st::msgInBg);

    auto contentTop = 0;
    if (showSender) {
        p.setFont(st::msgNameFont);
        p.setPen(senderColor(item.sender.id));
        p.drawText(
            bubbleLeft + kBubblePaddingH,
            kBubblePaddingV + st::msgNameFont->ascent,
            elidedSenderName(item, bubbleWidth, false));
        contentTop += kSenderNameHeight;
    }
    if (fwdHeight > 0) {
        const auto fwdStr = forwardedText(item);
        p.setFont(st::msgServiceFont);
        p.setPen(isOut ? st::msgOutServiceFg : st::msgInServiceFg);
        const auto fwdInnerW = bubbleWidth - 2 * kBubblePaddingH;
        const auto &fwdFm = st::fontMetrics(st::msgServiceFont);
        p.drawText(
            bubbleLeft + kBubblePaddingH,
            contentTop + st::msgServiceFont->ascent,
            fwdFm.elidedText(fwdStr, Qt::ElideRight, fwdInnerW));
        contentTop += fwdHeight;
    }
    if (audioReplyH > 0) {
        const QRect replyRect(
            bubbleLeft + kBubblePaddingH,
            contentTop,
            bubbleWidth - 2 * kBubblePaddingH,
            audioReplyH);
        paintReplyBlock(p, audioReplyData, replyRect, isOut, dpr,
            context.paintTarget, context.repaintTargetRect);
        contentTop += audioReplyH + kReplyPreviewBottomSkip;
    }

    // --- Play/Pause circle (same position as document icon) ---
    const QRect iconRect(
        bubbleLeft + st::docPaddingLeft,
        contentTop + st::docPaddingTop,
        st::docThumbSize,
        st::docThumbSize);
    {
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(isOut ? st::msgFileOutBg : st::msgFileInBg);
        p.drawEllipse(iconRect);
    }

    // Determine if this message is currently playing.
    const auto isPlaying = context.audioState
        && context.audioState->playingEventId == item.eventId;
    const auto isPaused = isPlaying && context.audioState->isPaused;
    const auto showPlayIcon = !isPlaying || isPaused;
    const auto uploading = showUploadOverlay(item);

    // Draw play or pause icon inside the circle.
    const auto downloadState = downloadVisualState(item);
    const auto downloading = downloadState.active;
    if (uploading) {
        paintUploadOverlay(
            p,
            iconRect,
            item.delivery.uploadProgress,
            dpr,
            st::historyIconFgInverted,
            &st::historyMediaOverlayBg);
    } else if (downloading && downloadState.determinate) {
        const auto iconColor = isOut ? st::historyFileOutIconFg : st::historyFileInIconFg;
        paintCenteredChatIcon(p, iconRect, QStringLiteral("history_file_cancel"), dpr, iconColor);
        paintProgressArc(p, iconRect, downloadState.progress, iconColor);
    } else if (downloading) {
        const auto iconColor = isOut ? st::historyFileOutIconFg : st::historyFileInIconFg;
        paintCenteredChatIcon(p, iconRect, QStringLiteral("history_file_cancel"), dpr, iconColor);
        paintLoadingArc(p, iconRect, iconColor);
        scheduleContextRepaint(context);
    } else {
        const auto iconColor = isOut ? st::historyFileOutIconFg : st::historyFileInIconFg;
        const auto iconName = showPlayIcon
            ? QStringLiteral("history_file_play")
            : QStringLiteral("history_file_pause");
        paintCenteredChatIcon(p, iconRect, iconName, dpr, iconColor);
    }

    // --- Filename (line 1) ---
    const auto fileName = !mediaFilename(item).isEmpty() ? mediaFilename(item) : bodyText(item);
    const auto textLeft = bubbleLeft + st::docNameLeft;
    const auto textWidth = qMax(1, bubbleWidth - st::docNameLeft - st::docPaddingRight);
    p.setFont(st::semiboldFont);
    p.setPen(isOut ? st::historyTextOutFg : st::historyTextInFg);
    const auto elidedName = st::fontMetrics(st::semiboldFont).elidedText(
        fileName,
        Qt::ElideMiddle,
        textWidth);
    p.drawText(
        textLeft,
        contentTop + st::docNameTop + st::semiboldFont->ascent,
        elidedName);

    // --- Duration / progress text (line 2) ---
    p.setFont(st::msgFont);
    p.setPen(isOut ? st::mediaOutFg : st::mediaInFg);
    QString statusText;
    if (uploading) {
        statusText = uploadStatusText(item);
    } else if (downloading) {
        statusText = downloadStatusText(item, downloadState);
    } else if (isPlaying && context.audioState->durationMs > 0) {
        statusText = formatDuration(context.audioState->positionMs)
            + QStringLiteral(" / ")
            + formatDuration(context.audioState->durationMs);
    } else if (mediaDurationMs(item) > 0) {
        statusText = formatDuration(mediaDurationMs(item));
    } else if (mediaSize(item) > 0) {
        statusText = formatBytes(mediaSize(item));
    }
    // Otherwise leave the status line blank — don't show the file type. The
    // duration fills in once the audio is played/probed (and is then persisted
    // in the state store, so it shows immediately on later loads/restarts).
    const auto elidedStatus = st::fontMetrics(st::msgFont).elidedText(
        statusText,
        Qt::ElideRight,
        textWidth);
    p.drawText(
        textLeft,
        contentTop + st::docStatusTop + st::msgFont->ascent,
        elidedStatus);

    contentTop += innerH;
    contentTop += drawReactions(bubbleLeft, bubbleWidth, contentTop, false);

    const auto timeStr = formatTime(item.timestamp);
    const auto &timeFm = st::fontMetrics(st::msgDateFont);
    const auto bottomInfoWidth = infoWidth(item);
    const auto infoTop = bubbleH - kBubblePaddingV + kDateDeltaY - timeFm.height();
    const auto infoLeft = bubbleLeft + bubbleWidth - kBubblePaddingH + kDateDeltaX - bottomInfoWidth;
    const auto infoBaseline = infoTop + timeFm.ascent();
    paintBottomInfo(
        p,
        item,
        infoLeft,
        infoTop,
        infoBaseline,
        timeStr,
        timeFm,
        dpr,
        normalBottomInfoPalette(isOut),
        context.sendingAnimationProgress);

    if (context.selectionMode && context.messageSelected) {
        paintSelectionOverlay(p, bubbleRect, corners, tail);
    }
}

} // namespace

bool showSenderName(
        const TimelineItem &item,
        const MessagePaintContext &context) {
    return computeShowSenderName(item, context);
}

void paintSenderAvatar(
    QPainter &p,
    const TimelineItem &item,
    int avatarLeft,
    int avatarTop,
    int avatarSize,
    qreal dpr,
    QWidget *repaintTarget,
    const QRect &repaintRect)
{
    bool paintedAvatar = false;
    if (!item.sender.avatarUrl.isEmpty()) {
        const auto avatarPix = MediaCache::loadAvatarPixmapAsync(
            item.sender.avatarUrl,
            avatarSize,
            dpr,
            repaintTarget,
            repaintRect);
        if (!avatarPix.isNull()) {
            p.drawPixmap(avatarLeft, avatarTop, avatarPix);
            paintedAvatar = true;
        }
    }
    if (!paintedAvatar) {
        // Empty-userpic paint: gradient background + 2-char initials.
        Ui::EmptyUserpic::paint(
            p, item.sender.id, item.sender.name,
            avatarLeft, avatarTop, avatarSize);
    }
}

int bubbleHeight(
    const TimelineItem &item,
    int maxWidth,
    const MessagePaintContext &context)
{
    if (item.delivery.deleted) {
        // Deleted message: simple bubble with "Deleted message" text.
        const auto showSender = showSenderName(item, context);
        return kBubblePaddingV
            + (showSender ? kSenderNameHeight : 0)
            + st::msgFont->height
            + kBubblePaddingV;
    }

    if (contentType(item) == ContentType::UnableToDecrypt) {
        // Height must NOT depend on the glow. The glow is time-based (it gives up
        // after kUtdGlowSafetyMs), so letting it pick the height means every still-
        // undecrypted row silently changes height at the 55s mark — a change no
        // slice diff reports and no layout-cache key covers, so it lands later,
        // unanchored, on hundreds of rows at once. Reserving the card height for
        // both states costs a slightly taller shimmer and makes the give-up moment
        // a pure repaint instead of a viewport-shifting relayout.
        return computeUtdCardLayout(item).bubbleHeight;
    }

    if (contentType(item) == ContentType::Service) {
        const auto geometry = computeBubbleGeometry(item, context);
        const auto outsideCompensation = qMax(
            0,
            st::msgServiceMargin.top()
                + st::msgServiceMargin.bottom()
                - kMarginTop
                - kMarginBottom);
        return geometry.height + outsideCompensation;
    }

    if (contentType(item) == ContentType::Image) {
        const auto showSender = showSenderName(item, context);
        const auto media = photoBubbleMetrics(item, kMaxBubbleWidth, showSender);
        const auto caption = imageCaptionText(item);
        const auto hasCaption = !caption.isEmpty();
        const auto bubbleWidth = media.bubbleWidth;
        const auto bubbleLeft = bubbleLeftFor(context, item.delivery.outgoing, bubbleWidth);
        const auto fwdH = forwardedHeaderHeight(item, bubbleWidth - 2 * kBubblePaddingH);
        auto height = showSender ? (kBubblePaddingV + kSenderNameHeight) : 0;
        if (!showSender && (fwdH > 0 || hasReply(item))) {
            height = kBubblePaddingV;
        }
        height += fwdH;
        if (hasReply(item)) {
            height += replyHeightFor(item, context) + kReplyPreviewBottomSkip;
        }
        if (height > 0) {
            height += st::mediaInBubbleSkip;
        }
        height += media.mediaHeight;
        if (hasCaption) {
            const auto captionLayout = layoutCaptionLines(
                caption, bubbleWidth - 2 * kBubblePaddingH);
            // Match the painted bubble (computeBubbleGeometry / paintImageMessage):
            // captionBlockHeight only reserves a timestamp line when it wraps, so
            // don't add an unconditional msgDateFont line (that surplus showed as
            // empty padding below image/video+caption bubbles).
            height += st::mediaCaptionSkip
                + captionBlockHeight(
                    captionLayout, item,
                    bubbleWidth - 2 * kBubblePaddingH,
                    !item.reactions.isEmpty())
                + kBubblePaddingV;
        }
        height += reactionBlockHeight(item, bubbleLeft, bubbleWidth, hasCaption);
        return height;
    }

    if (contentType(item) == ContentType::Audio) {
        const auto bubbleWidth = HistoryViewAudio::audioBubbleWidth(item, maxWidth, item.delivery.outgoing);
        const auto bubbleLeft = bubbleLeftFor(context, item.delivery.outgoing, bubbleWidth);
        auto height = HistoryViewAudio::bubbleHeight(item, maxWidth, context);
        height += forwardedHeaderHeight(item, bubbleWidth - 2 * kBubblePaddingH);
        if (hasReply(item)) {
            height += replyHeightFor(item, context) + kReplyPreviewBottomSkip;
        }
        height += reactionBlockHeight(item, bubbleLeft, bubbleWidth);
        if (!item.reactions.isEmpty()) {
            height += kBubblePaddingV; // bottom padding after reactions
        }
        return height;
    }

    if (contentType(item) == ContentType::File) {
        const auto showSender = showSenderName(item, context);
        const auto bubbleWidth = fileBubbleWidth(item, maxWidth, item.delivery.outgoing);
        const auto bubbleLeft = bubbleLeftFor(context, item.delivery.outgoing, bubbleWidth);
        const auto innerH = st::docPaddingTop + st::docThumbSize + st::docPaddingBottom;
        const auto hasFileCaption = !captionText(item).simplified().isEmpty();
        const auto fwdH = forwardedHeaderHeight(item, bubbleWidth - 2 * kBubblePaddingH);
        const auto hasReplyPreview = hasReply(item);
        const auto hasHeaderAboveFile = showSender || fwdH > 0 || hasReplyPreview;
        auto height = innerH + (hasHeaderAboveFile ? kBubblePaddingV : 0);
        if (hasFileCaption) {
            const auto fileCaptionLayout = layoutCaptionLines(
                captionText(item).simplified(),
                bubbleWidth - 2 * kBubblePaddingH);
            height += st::mediaCaptionSkip
                + captionBlockHeight(
                    fileCaptionLayout, item,
                    bubbleWidth - 2 * kBubblePaddingH,
                    !item.reactions.isEmpty())
                + kBubblePaddingV;
        }
        height += reactionBlockHeight(item, bubbleLeft, bubbleWidth);
        if (!item.reactions.isEmpty()) {
            height += kBubblePaddingV;
        }
        if (showSender) {
            height += kSenderNameHeight;
        }
        height += fwdH;
        if (hasReplyPreview) {
            height += replyHeightFor(item, context) + kReplyPreviewBottomSkip;
        }
        return height;
    }

    if (contentType(item) == ContentType::Video) {
        const auto showSender = showSenderName(item, context);
        const auto media = photoBubbleMetrics(item, kMaxBubbleWidth, showSender);
        const auto bubbleWidth = media.bubbleWidth;
        const auto bubbleLeft = bubbleLeftFor(context, item.delivery.outgoing, bubbleWidth);
        const auto caption = imageCaptionText(item);
        const auto hasCaption = !caption.isEmpty();
        const auto fwdH = forwardedHeaderHeight(item, bubbleWidth - 2 * kBubblePaddingH);
        auto height = showSender ? (kBubblePaddingV + kSenderNameHeight) : 0;
        if (!showSender && (fwdH > 0 || hasReply(item))) {
            height = kBubblePaddingV;
        }
        height += fwdH;
        if (hasReply(item)) {
            height += replyHeightFor(item, context) + kReplyPreviewBottomSkip;
        }
        if (height > 0) {
            height += st::mediaInBubbleSkip;
        }
        height += media.mediaHeight;
        if (hasCaption) {
            const auto captionLayout = layoutCaptionLines(
                caption, bubbleWidth - 2 * kBubblePaddingH);
            // Match the painted bubble (computeBubbleGeometry / paintImageMessage):
            // captionBlockHeight only reserves a timestamp line when it wraps, so
            // don't add an unconditional msgDateFont line (that surplus showed as
            // empty padding below image/video+caption bubbles).
            height += st::mediaCaptionSkip
                + captionBlockHeight(
                    captionLayout, item,
                    bubbleWidth - 2 * kBubblePaddingH,
                    !item.reactions.isEmpty())
                + kBubblePaddingV;
        }
        height += reactionBlockHeight(item, bubbleLeft, bubbleWidth, hasCaption);
        return height;
    }

    if (contentType(item) == ContentType::Poll) {
        const auto showSender = showSenderName(item, context);
        const auto bubbleWidth = bubbleMaxWidth(maxWidth, item.delivery.outgoing);
        const auto bubbleLeft = bubbleLeftFor(context, item.delivery.outgoing, bubbleWidth);
        auto height = kBubblePaddingV;
        if (showSender) {
            height += kSenderNameHeight;
        }
        height += forwardedHeaderHeight(item, bubbleWidth - 2 * kBubblePaddingH);
        if (hasReply(item)) {
            height += replyHeightFor(item, context) + kReplyPreviewBottomSkip;
        }
        height += HistoryViewPoll::contentHeight(item, bubbleWidth - 2 * kBubblePaddingH);
        height += reactionBlockHeight(item, bubbleLeft, bubbleWidth);
        if (!item.reactions.isEmpty()) {
            height += kBubblePaddingV; // bottom padding after reactions
        }
        return height;
    }

    const auto hasLinkPreview = contentType(item) == ContentType::Text
        && hasRenderableLinkPreview(item);

    // --- Large emoji special case: no bubble, floating timestamp pill ---
    if (context.largeEmojiEnabled
        && contentType(item) == ContentType::Text
        && !hasReply(item)
        && forwardedSenderName(item).isEmpty()
        && !hasLinkPreview
        && shouldRenderLargeEmoji(bodyText(item), true)) {
        const auto isolated = detectIsolatedEmoji(bodyText(item));
        if (isolated.valid) {
            const auto isOut = item.delivery.outgoing;
            const auto showSender = showSenderName(item, context);
            const auto emojiH = largeEmojiHeight();
            const auto emojiW = largeEmojiWidth(isolated);
            const auto pillW = infoWidth(item) + 2 * st::msgDateImgPadding.x();
            // Pill is inline to the right of emoji.
            const auto contentW = emojiW + st::msgDateImgDelta + pillW;
            // Height: sender name (optional) + emoji row (pill is inline)
            // + extra bottom padding (no bubble provides visual separation).
            auto height = (showSender ? kSenderNameHeight : 0)
                + emojiH
                + kBubblePaddingV;
            const auto bl = bubbleLeftFor(context, isOut, contentW);
            height += reactionBlockHeight(item, bl, contentW);
            return height;
        }
    }

    auto bubbleLeft = 0;
    auto bubbleWidth = 0;
    auto textHeight = 0;
    if (!computeTextBubbleMetrics(item, context, bubbleLeft, bubbleWidth, textHeight)) {
        return 0;
    }

    auto height = kBubblePaddingV
        + textHeight
        + kBubblePaddingV;

    if (hasReply(item)) {
        height += replyHeightFor(item, context) + kReplyPreviewBottomSkip;
    }
    height += forwardedHeaderHeight(item, bubbleWidth - 2 * kBubblePaddingH);
    if (hasLinkPreview) {
        const auto lpW = bubbleWidth - 2 * kBubblePaddingH;
        height += st::mediaInBubbleSkip + linkPreviewBlockHeight(item, lpW);
        if (item.reactions.isEmpty()) {
            height += skipBlockHeight();
        }
    }
    height += reactionBlockHeight(item, bubbleLeft, bubbleWidth);

    // Sender name for group chats (not if same sender collapsed).
    if (showSenderName(item, context)) {
        height += kSenderNameHeight;
    }

    return height;
}

void paintImageMessage(
        QPainter &p,
        const TimelineItem &item,
        const MessagePaintContext &context,
        bool isOut,
        bool showSender,
        qreal dpr,
        const std::function<int(int, int, int, bool)> &drawReactions) {
    const auto imageTail = bubbleTailFor(isOut, context.sameSenderBelow, true);
    const auto imageCorners = bubbleCornersFor(
        isOut,
        context.sameSenderAbove,
        context.sameSenderBelow,
        imageTail);
    const auto media = photoBubbleMetrics(item, kMaxBubbleWidth, showSender);
    const auto caption = imageCaptionText(item);
    const auto hasCaption = !caption.isEmpty();
    const auto mediaPlacement = imageMediaPlacement(item, showSender, hasCaption);
    const auto mediaCorners = adjustedMediaCorners(imageCorners, mediaPlacement);
    const auto bubbleWidth = media.bubbleWidth;
    const auto captionLayout = layoutCaptionLines(
        caption, bubbleWidth - 2 * kBubblePaddingH);
    const auto captionHeight = captionBlockHeight(
        captionLayout, item, bubbleWidth - 2 * kBubblePaddingH,
        !item.reactions.isEmpty());
    const auto bubbleLeft = bubbleLeftFor(context, isOut, bubbleWidth);
    const auto fwdHeight = forwardedHeaderHeight(
        item, bubbleWidth - 2 * kBubblePaddingH);
    const auto imgReplyData = hasReply(item)
        ? resolveReplyData(item, context) : ReplyPreviewData{};
    const auto imgReplyH = !hasReply(item) ? 0
        : (imgReplyData.deleted ? replyDeletedHeight() : replyPreviewHeight(imgReplyData.hasThumb));
    auto contentTop = showSender ? (kBubblePaddingV + kSenderNameHeight) : 0;
    if (!showSender && (fwdHeight > 0 || imgReplyH > 0)) {
        contentTop = kBubblePaddingV;
    }
    contentTop += fwdHeight;
    if (imgReplyH > 0) {
        contentTop += imgReplyH + kReplyPreviewBottomSkip;
    }
    if (contentTop > 0) {
        contentTop += st::mediaInBubbleSkip;
    }
    auto bubbleH = contentTop + media.mediaHeight;
    if (hasCaption) {
        bubbleH += st::mediaCaptionSkip
            + captionHeight
            + kBubblePaddingV;
    }
    const auto reactionAreaH = reactionBlockHeight(item, bubbleLeft, bubbleWidth, hasCaption);
    bubbleH += reactionAreaH;
    const auto outsideReactionH = mediaOnlyOutsideReactionHeight(item, bubbleLeft, bubbleWidth, hasCaption);
    const QRectF bubbleRect(bubbleLeft, 0, bubbleWidth, bubbleH - outsideReactionH);
    paintBubbleLayer(
        p,
        bubbleRect,
        imageCorners,
        imageTail,
        isOut ? st::msgOutShadow : st::msgInShadow,
        kBubbleShadow);
    paintBubbleLayer(
        p,
        bubbleRect,
        imageCorners,
        imageTail,
        isOut ? st::msgOutBg : st::msgInBg);

    {
        auto senderTop = kBubblePaddingV;
        if (showSender) {
            const auto reserveFastReply = false /* reply pill is floating, no sender-row reservation */;
            p.setFont(st::msgNameFont);
            p.setPen(senderColor(item.sender.id));
            p.drawText(
                bubbleLeft + kBubblePaddingH,
                senderTop + st::msgNameFont->ascent,
                elidedSenderName(item, bubbleWidth, reserveFastReply));
            if (reserveFastReply && context.isHovered && !context.selectionMode) {
                paintFastReplyAction(
                    p,
                    isOut,
                    context.hoveredFastReply,
                    bubbleLeft,
                    bubbleWidth,
                    senderTop);
            }
            senderTop += kSenderNameHeight;
        }
        if (fwdHeight > 0) {
            const auto fwdStr = forwardedText(item);
            p.setFont(st::msgServiceFont);
            p.setPen(isOut ? st::msgOutServiceFg : st::msgInServiceFg);
            const auto fwdInnerW = bubbleWidth - 2 * kBubblePaddingH;
            const auto &fwdFm = st::fontMetrics(st::msgServiceFont);
            p.drawText(
                bubbleLeft + kBubblePaddingH,
                senderTop + st::msgServiceFont->ascent,
                fwdFm.elidedText(fwdStr, Qt::ElideRight, fwdInnerW));
            senderTop += fwdHeight;
        }
        if (imgReplyH > 0) {
            const QRect replyRect(
                bubbleLeft + kBubblePaddingH,
                senderTop,
                bubbleWidth - 2 * kBubblePaddingH,
                imgReplyH);
            paintReplyBlock(p, imgReplyData, replyRect, isOut, dpr,
            context.paintTarget, context.repaintTargetRect);
        }
    }

    const QRect imageRect(
        bubbleLeft,
        contentTop,
        bubbleWidth,
        media.mediaHeight);
    const auto uploading = showUploadOverlay(item);
    {
        p.save();
        p.setClipPath(roundedBubblePath(QRectF(imageRect), mediaCorners));
        // Use drawImage so Qt passes the sRGB color space tag to CoreGraphics,
        // which then correctly converts sRGB→display profile (matching Preview.app).
        const auto img = MediaCache::loadScaledImageAsync(
            mediaUrl(item),
            imageRect.size(),
            dpr,
            context.paintTarget,
            context.repaintTargetRect);
        if (!img.isNull()) {
            if (uploading) {
                const auto pixmap = QPixmap::fromImage(img);
                const auto blurred = blurredUploadPixmap(
                    pixmap,
                    mediaUrl(item)
                        + QLatin1Char('|')
                        + QString::number(imageRect.width())
                        + QLatin1Char('x')
                        + QString::number(imageRect.height()));
                p.drawPixmap(imageRect, blurred.isNull() ? pixmap : blurred);
            } else {
                p.drawImage(imageRect, img);
            }
        } else {
            paintMediaPlaceholder(p, imageRect, mediaBlurhash(item), item.eventId);
            // Show the spinner only while a download is genuinely in flight —
            // not for a resolved image that is merely being re-decoded after a
            // cache eviction (its async load repaints when ready). Mirrors the
            // video path's downloadVisualState gating; avoids a misleading
            // "downloading" affordance on already-received images.
            if (!uploading && downloadVisualState(item).active) {
                paintLoadingSpinner(
                    p,
                    uploadOverlayButtonRect(imageRect),
                    st::historyMediaSpinnerFg);
                // Keep animating the spinner while the download runs.
                scheduleContextRepaint(context);
            }
        }
        if (uploading) {
            paintUploadOverlay(
                p,
                uploadOverlayButtonRect(imageRect),
                item.delivery.uploadProgress,
                dpr,
                st::historyFileInIconFg,
                &st::msgDateImgBg);
            paintUploadStatusBadge(p, imageRect, item);
        }
        p.restore();
    }

    const auto timeStr = formatTime(item.timestamp);
    const auto &timeFm = st::fontMetrics(st::msgDateFont);
    if (!hasCaption) {
        const auto bottomInfoWidth = infoWidth(item);
        const int dateW = bottomInfoWidth + 2 * st::msgDateImgPadding.x();
        const int dateH = timeFm.height() + 2 * st::msgDateImgPadding.y();
        const QRect dateRect(
            imageRect.x() + imageRect.width() - st::msgDateImgDelta - dateW,
            imageRect.y() + imageRect.height() - st::msgDateImgDelta - dateH,
            dateW,
            dateH);
        {
            PainterHighQualityEnabler hq(p);
            p.setPen(Qt::NoPen);
            p.setBrush(st::msgDateImgBg);
            p.drawRoundedRect(dateRect, dateH / 2.0, dateH / 2.0);
        }
        const auto infoTop = dateRect.top() + st::msgDateImgPadding.y();
        const auto infoBaseline = infoTop + timeFm.ascent();
        paintBottomInfo(
            p,
            item,
            dateRect.left() + st::msgDateImgPadding.x(),
            infoTop,
            infoBaseline,
            timeStr,
            timeFm,
            dpr,
            mediaOverlayBottomInfoPalette(),
            context.sendingAnimationProgress);
    }
    contentTop += media.mediaHeight;

    if (hasCaption) {
        contentTop += st::mediaCaptionSkip;
        p.setPen(isOut ? st::historyTextOutFg : st::historyTextInFg);
        drawCachedLinkPreviewText(
            p,
            captionLayout.lines,
            st::msgFont,
            bubbleLeft + kBubblePaddingH,
            contentTop,
            st::msgFont->height);
        contentTop += captionHeight;
    }

    contentTop += drawReactions(bubbleLeft, bubbleWidth, contentTop, !hasCaption);

    if (hasCaption) {
        const auto bottomInfoWidth = infoWidth(item);
        const auto infoTop = bubbleH - kBubblePaddingV + kDateDeltaY - timeFm.height();
        const auto infoLeft = bubbleLeft + bubbleWidth - kBubblePaddingH + kDateDeltaX - bottomInfoWidth;
        const auto infoBaseline = infoTop + timeFm.ascent();
        paintBottomInfo(
            p,
            item,
            infoLeft,
            infoTop,
            infoBaseline,
            timeStr,
            timeFm,
            dpr,
            normalBottomInfoPalette(isOut),
            context.sendingAnimationProgress);
    }

    if (context.selectionMode && context.messageSelected) {
        paintSelectionOverlay(p, bubbleRect, imageCorners, imageTail);
    }
}

// Draw a player control glyph (player_fullscreen / player_volume_*) as a bare
// white mask fit into `button` (never upscaled past native), centered. The caller
// supplies any translucent backing (the shared time pill, or the fullscreen chip).
static void drawVideoControlGlyph(
        QPainter &p,
        const QRect &button,
        const QString &name) {
    if (button.isNull()) {
        return;
    }
    const auto glyph = TeleMatrix::Style::IconProvider::tintedIcon(
        QStringLiteral(":/telematrix/icons/player/"), name,
        st::historyIconFgInverted);
    if (glyph.isNull()) {
        return;
    }
    const auto nativeW = int(glyph.width() / glyph.devicePixelRatio());
    const auto nativeH = int(glyph.height() / glyph.devicePixelRatio());
    const auto side = qMin(
        qMin(button.width(), button.height()), qMin(nativeW, nativeH));
    const QRect target(
        button.center().x() - side / 2,
        button.center().y() - side / 2,
        side,
        side);
    PainterHighQualityEnabler hq(p);
    p.drawImage(target, glyph);
}

// Geometry of the top-left "time + mute" pill, corner-status style:
// duration/countdown text on the left, mute glyph on the
// right, one fully-rounded msgDateImgBg pill sized font.height + 2*padY. The text
// column is RESERVED at the full-duration width so the mute glyph doesn't shift as
// the countdown ticks. One source of truth for both paint and the mute hit-test.
struct VideoTimePill {
    QRect pill;
    QRect text;
    QRect mute; // null unless withMute
};
static VideoTimePill videoTimePillGeometry(
        const QRect &mediaRect,
        qint64 durationMs,
        bool withMute) {
    VideoTimePill r;
    if (mediaRect.isNull() || durationMs <= 0) {
        return r;
    }
    const auto &fm = st::fontMetrics(st::msgDateFont);
    const auto inset = TeleMatrix::Style::ConvertScale(8);
    const auto padX = st::msgDateImgPadding.x();
    const auto padY = st::msgDateImgPadding.y();
    const auto gap = TeleMatrix::Style::ConvertScale(6);
    const auto pillH = fm.height() + 2 * padY;
    // Match the fullscreen glyph, which fills its pillH chip — same visual size.
    const auto muteSize = pillH;
    const auto textW = fm.horizontalAdvance(
        formatDuration(static_cast<quint64>(durationMs)));
    const auto pillLeft = mediaRect.left() + inset;
    const auto pillTop = mediaRect.top() + inset;
    // The mute glyph hugs the right edge: a tighter right inset than the left text
    // pad so it sits close to the pill's right side.
    const auto rightPad = withMute ? TeleMatrix::Style::ConvertScale(4) : padX;
    const auto pillW = padX + textW + (withMute ? (gap + muteSize) : 0) + rightPad;
    r.pill = QRect(pillLeft, pillTop, pillW, pillH);
    r.text = QRect(pillLeft + padX, pillTop, textW, pillH);
    if (withMute) {
        r.mute = QRect(
            pillLeft + padX + textW + gap,
            pillTop + (pillH - muteSize) / 2,
            muteSize,
            muteSize);
    }
    return r;
}

void paintVideoMessage(
        QPainter &p,
        const TimelineItem &item,
        const MessagePaintContext &context,
        bool isOut,
        bool showSender,
        qreal dpr,
        const std::function<int(int, int, int, bool)> &drawReactions) {
    const auto videoTail = bubbleTailFor(isOut, context.sameSenderBelow, true);
    const auto videoCorners = bubbleCornersFor(
        isOut,
        context.sameSenderAbove,
        context.sameSenderBelow,
        videoTail);
    const auto media = photoBubbleMetrics(item, kMaxBubbleWidth, showSender);
    const auto caption = imageCaptionText(item);
    const auto hasCaption = !caption.isEmpty();
    const auto mediaPlacement = imageMediaPlacement(item, showSender, hasCaption);
    const auto mediaCorners = adjustedMediaCorners(videoCorners, mediaPlacement);
    const auto bubbleWidth = media.bubbleWidth;
    const auto captionLayout = layoutCaptionLines(
        caption, bubbleWidth - 2 * kBubblePaddingH);
    const auto captionHeight = captionBlockHeight(
        captionLayout, item, bubbleWidth - 2 * kBubblePaddingH,
        !item.reactions.isEmpty());
    const auto bubbleLeft = bubbleLeftFor(context, isOut, bubbleWidth);
    const auto fwdHeight = forwardedHeaderHeight(
        item, bubbleWidth - 2 * kBubblePaddingH);
    const auto vidReplyData = hasReply(item)
        ? resolveReplyData(item, context) : ReplyPreviewData{};
    const auto vidReplyH = !hasReply(item) ? 0
        : (vidReplyData.deleted ? replyDeletedHeight() : replyPreviewHeight(vidReplyData.hasThumb));
    auto contentTop = showSender ? (kBubblePaddingV + kSenderNameHeight) : 0;
    if (!showSender && (fwdHeight > 0 || vidReplyH > 0)) {
        contentTop = kBubblePaddingV;
    }
    contentTop += fwdHeight;
    if (vidReplyH > 0) {
        contentTop += vidReplyH + kReplyPreviewBottomSkip;
    }
    if (contentTop > 0) {
        contentTop += st::mediaInBubbleSkip;
    }
    auto bubbleH = contentTop + media.mediaHeight;
    if (hasCaption) {
        bubbleH += st::mediaCaptionSkip
            + captionHeight
            + kBubblePaddingV;
    }
    const auto reactionAreaH = reactionBlockHeight(item, bubbleLeft, bubbleWidth, hasCaption);
    bubbleH += reactionAreaH;
    const auto outsideReactionH = mediaOnlyOutsideReactionHeight(item, bubbleLeft, bubbleWidth, hasCaption);
    const QRectF bubbleRect(bubbleLeft, 0, bubbleWidth, bubbleH - outsideReactionH);
    paintBubbleLayer(
        p,
        bubbleRect,
        videoCorners,
        videoTail,
        isOut ? st::msgOutShadow : st::msgInShadow,
        kBubbleShadow);
    paintBubbleLayer(
        p,
        bubbleRect,
        videoCorners,
        videoTail,
        isOut ? st::msgOutBg : st::msgInBg);

    {
        auto senderTop = kBubblePaddingV;
        if (showSender) {
            const auto reserveFastReply = false /* reply pill is floating, no sender-row reservation */;
            p.setFont(st::msgNameFont);
            p.setPen(senderColor(item.sender.id));
            p.drawText(
                bubbleLeft + kBubblePaddingH,
                senderTop + st::msgNameFont->ascent,
                elidedSenderName(item, bubbleWidth, reserveFastReply));
            if (reserveFastReply && context.isHovered && !context.selectionMode) {
                paintFastReplyAction(
                    p,
                    isOut,
                    context.hoveredFastReply,
                    bubbleLeft,
                    bubbleWidth,
                    senderTop);
            }
            senderTop += kSenderNameHeight;
        }
        if (fwdHeight > 0) {
            const auto fwdStr = forwardedText(item);
            p.setFont(st::msgServiceFont);
            p.setPen(isOut ? st::msgOutServiceFg : st::msgInServiceFg);
            const auto fwdInnerW = bubbleWidth - 2 * kBubblePaddingH;
            const auto &fwdFm = st::fontMetrics(st::msgServiceFont);
            p.drawText(
                bubbleLeft + kBubblePaddingH,
                senderTop + st::msgServiceFont->ascent,
                fwdFm.elidedText(fwdStr, Qt::ElideRight, fwdInnerW));
            senderTop += fwdHeight;
        }
        if (vidReplyH > 0) {
            const QRect replyRect(
                bubbleLeft + kBubblePaddingH,
                senderTop,
                bubbleWidth - 2 * kBubblePaddingH,
                vidReplyH);
            paintReplyBlock(p, vidReplyData, replyRect, isOut, dpr,
            context.paintTarget, context.repaintTargetRect);
        }
    }

    // Single source of truth for the media rect (shared with hit-testing).
    const QRect videoRect = videoMediaRect(item, context);
    const auto uploading = showUploadOverlay(item);
    // Only once the server has the file is there anything to play or expand —
    // and a message on its way out must not offer affordances it won't honour.
    const auto playable = hasRemoteMediaSource(item) && !context.deleting;
    const auto downloadState = downloadVisualState(item);
    const auto downloading = downloadState.active;
    const auto *inlineVid = context.inlineVideo;
    const auto inlineActive =
        inlineVid && inlineVid->activeEventId() == item.eventId;
    if (inlineActive) {
        // Tell the player the on-screen size so it downscales decoded frames
        // once instead of the paint rescaling full-res frames every tick. For a
        // rotated video the pre-rotation frame maps to the transposed box.
        auto target = QSizeF(videoRect.size()) * dpr;
        if (const auto rot = inlineVid->videoRotation();
                rot == 90 || rot == 270) {
            target.transpose();
        }
        context.inlineVideo->setDisplaySize(target.toSize());
    }
    auto thumbLoaded = false;
    {
        p.save();
        p.setClipPath(roundedBubblePath(QRectF(videoRect), mediaCorners));
        // Inline playback: draw the live frame in place of the poster.
        if (inlineActive && !inlineVid->currentFrame().isNull()) {
            const auto &frame = inlineVid->currentFrame();
            // Rotate via the painter (cheap) rather than QImage::transformed(),
            // which is a full per-frame pixel copy — this runs at ~frame rate.
            if (const auto rot = inlineVid->videoRotation(); rot != 0) {
                p.save();
                p.translate(videoRect.center().x(), videoRect.center().y());
                p.rotate(rot);
                const auto swap = (rot == 90 || rot == 270);
                const auto w = swap ? videoRect.height() : videoRect.width();
                const auto h = swap ? videoRect.width() : videoRect.height();
                p.drawImage(QRect(-w / 2, -h / 2, w, h), frame);
                p.restore();
            } else {
                p.drawImage(videoRect, frame);
            }
            thumbLoaded = true;
        }
        // Try server-provided thumbnail, then fall back to a locally
        // extracted video frame (key: "vidthumb:<eventId>" in MediaCache).
        if (!thumbLoaded && !mediaThumbUrl(item).isEmpty()) {
            const auto thumbImg = MediaCache::loadScaledImageAsync(
                mediaThumbUrl(item),
                videoRect.size(),
                dpr,
                context.paintTarget,
                context.repaintTargetRect);
            if (!thumbImg.isNull()) {
                if (uploading) {
                    const auto px = QPixmap::fromImage(thumbImg);
                    const auto blurred = blurredUploadPixmap(
                        px,
                        mediaThumbUrl(item)
                            + QLatin1Char('|')
                            + QString::number(videoRect.width())
                            + QLatin1Char('x')
                            + QString::number(videoRect.height()));
                    p.drawPixmap(videoRect, blurred.isNull() ? px : blurred);
                } else {
                    p.drawImage(videoRect, thumbImg);
                }
                thumbLoaded = true;
            }
        }
        // Try server-generated thumbnail (Matrix thumbnail API).
        if (!thumbLoaded && !mediaUrl(item).isEmpty()) {
            const auto srvKey = QStringLiteral("srvthumb:") + mediaUrl(item);
            const auto srvImg = MediaCache::loadScaledImageAsync(
                srvKey,
                videoRect.size(),
                dpr,
                context.paintTarget,
                context.repaintTargetRect);
            if (!srvImg.isNull()) {
                if (uploading) {
                    const auto px = QPixmap::fromImage(srvImg);
                    const auto blurred = blurredUploadPixmap(
                        px,
                        srvKey
                            + QLatin1Char('|')
                            + QString::number(videoRect.width())
                            + QLatin1Char('x')
                            + QString::number(videoRect.height()));
                    p.drawPixmap(videoRect, blurred.isNull() ? px : blurred);
                } else {
                    p.drawImage(videoRect, srvImg);
                }
                thumbLoaded = true;
            }
        }
        // Try locally extracted video frame (from partial download probe).
        if (!thumbLoaded) {
            const auto probeKey = QStringLiteral("vidthumb:") + item.eventId;
            const auto vidImg = MediaCache::loadScaledImageAsync(
                probeKey,
                videoRect.size(),
                dpr,
                context.paintTarget,
                context.repaintTargetRect);
            if (!vidImg.isNull()) {
                p.drawImage(videoRect, vidImg);
                thumbLoaded = true;
            }
        }
        if (!thumbLoaded) {
            if (!mediaBlurhash(item).isEmpty()) {
                paintMediaPlaceholder(p, videoRect, mediaBlurhash(item), item.eventId);
            } else {
                // No server thumbnail or blurhash yet — a soft deterministic
                // random-color blur reads better than a flat near-black fill
                // while the first frame is fetched.
                paintSyntheticBlurPlaceholder(p, videoRect, item.eventId);
            }
        }
        p.restore();
    }

    const auto drawPlayButton = [&] {
        if (context.deleting) {
            return; // clicks are swallowed while the redaction is in flight
        }
        PainterHighQualityEnabler hq(p);
        const auto buttonRect = uploadOverlayButtonRect(videoRect);
        p.setBrush(st::historyVideoControlBg);
        p.setPen(Qt::NoPen);
        p.drawEllipse(buttonRect);
        paintCenteredChatIcon(
            p,
            buttonRect,
            QStringLiteral("history_file_play"),
            dpr,
            st::historyIconFgInverted);
    };
    if (inlineActive) {
        const auto hasFrame = !inlineVid->currentFrame().isNull();
        const auto streamFrac = inlineVid->downloadedFraction();
        if (inlineVid->failed()) {
            // Terminal playback failure — show an error in place of the spinner.
            paintVideoErrorOverlay(p, videoRect);
        } else if (downloading) {
            // Streaming-unavailable fallback: resolveMedia download/decryption.
            paintVideoDownloadOverlay(
                p, videoRect, item, downloadState, context, dpr);
        } else if (showDeterminateDownload(
                       inlineVid->container(),
                       hasFrame,
                       inlineVid->streamDownloadPending(),
                       streamFrac)) {
            // The whole file downloads before the first frame (non-faststart, or
            // unclassified-and-past-the-heuristic). Surface the proxy's progress as
            // a determinate download overlay — it decrypts inline, so there's no
            // separate decrypt phase.
            DownloadVisualState dl;
            dl.active = true;
            dl.determinate = true;
            dl.progress = streamFrac;
            // Prefer the proxy's real byte counts (the event often carries no
            // file size); otherwise fall back to mediaSize scaled by the fraction.
            const auto proxyTotal = inlineVid->totalBytes();
            if (proxyTotal > 0) {
                dl.totalBytes = proxyTotal;
                dl.receivedBytes = inlineVid->downloadedBytes();
            } else {
                dl.totalBytes = mediaSize(item);
                dl.receivedBytes =
                    quint64(double(streamFrac) * double(mediaSize(item)));
            }
            paintVideoDownloadOverlay(p, videoRect, item, dl, context, dpr);
            scheduleContextRepaint(context);
        } else if (!hasFrame || inlineVid->buffering()) {
            // Starting (no frame yet) or buffering (playback outran the linear
            // download) — show the loading spinner over the placeholder/frame.
            paintLoadingSpinner(
                p, uploadOverlayButtonRect(videoRect), st::historyMediaSpinnerFg);
            scheduleContextRepaint(context);
        } else if (inlineVid->paused()) {
            // Playing inline: clean frame; resume button only when paused.
            drawPlayButton();
        }
    } else if (uploading) {
        paintUploadOverlay(
            p,
            uploadOverlayButtonRect(videoRect),
            item.delivery.uploadProgress,
            dpr,
            st::historyFileInIconFg,
            &st::msgDateImgBg);
        paintUploadStatusBadge(p, videoRect, item);
    } else if (downloading) {
        paintVideoDownloadOverlay(
            p,
            videoRect,
            item,
            downloadState,
            context,
            dpr);
    } else if (!thumbLoaded) {
        // Thumbnail / first frame still loading — show the same spinner as image
        // messages; the play button appears once the poster is ready.
        paintLoadingSpinner(
            p, uploadOverlayButtonRect(videoRect), st::historyMediaSpinnerFg);
        scheduleContextRepaint(context);
    } else {
        drawPlayButton();
    }

    // Inline playback controls: seek bar, elapsed/total time, fullscreen button.
    // Only once playback has a frame (not while downloading/buffering/starting).
    if (inlineActive && !downloading && !inlineVid->currentFrame().isNull()) {
        const auto durationMs = inlineVid->durationMs();
        const auto positionMs = inlineVid->positionMs();
        const auto playedFraction = (durationMs > 0)
            ? std::clamp(qreal(positionMs) / qreal(durationMs), qreal(0), qreal(1))
            : qreal(0);
        const auto bufferedFraction = std::clamp(
            qreal(inlineVid->downloadedFraction()), qreal(0), qreal(1));

        const auto seekRect = videoSeekBarRect(videoRect);
        {
            PainterHighQualityEnabler hq(p);
            p.setPen(Qt::NoPen);
            const auto trackH = TeleMatrix::Style::ConvertScale(3);
            const auto trackY = seekRect.top()
                + (seekRect.height() - trackH) / 2;
            const auto radius = trackH / 2.0;
            const QRectF fullTrack(
                seekRect.left(), trackY, seekRect.width(), trackH);
            auto whiteAlpha = [](int a) {
                auto c = QColor(Qt::white);
                c.setAlpha(a);
                return c;
            };
            auto blackAlpha = [](int a) {
                auto c = QColor(Qt::black);
                c.setAlpha(a);
                return c;
            };
            // The bar sits straight on video pixels, and a 28%-white track over a
            // white frame is invisible. Unlike the pills it has no msgDateImgBg
            // backdrop, so give it a soft dark shadow: black over dark content
            // composites away to nothing, while over bright content it both
            // outlines the bar and turns the unplayed track into a legible groove.
            // Kept faint and tight — enough to separate the bar from the frame,
            // not so much that it reads as a second, darker bar.
            auto dropShadow = [&](auto drawShape) {
                p.setBrush(blackAlpha(24));
                drawShape(1.25);
                p.setBrush(blackAlpha(56));
                drawShape(0.5);
            };
            dropShadow([&](qreal grow) {
                const auto s = fullTrack.adjusted(-grow, -grow, grow, grow);
                p.drawRoundedRect(s, s.height() / 2.0, s.height() / 2.0);
            });
            // Background track.
            p.setBrush(whiteAlpha(72)); // ~28%
            p.drawRoundedRect(fullTrack, radius, radius);
            // Buffered (downloaded) portion.
            if (bufferedFraction > 0.0) {
                const QRectF buffered(
                    seekRect.left(),
                    trackY,
                    seekRect.width() * bufferedFraction,
                    trackH);
                p.setBrush(whiteAlpha(115)); // ~45%
                p.drawRoundedRect(buffered, radius, radius);
            }
            // Played portion (accent).
            const auto playedW = seekRect.width() * playedFraction;
            if (playedW > 0.0) {
                const QRectF played(seekRect.left(), trackY, playedW, trackH);
                p.setBrush(st::mediaviewPlaybackActive);
                p.drawRoundedRect(played, radius, radius);
            }
            // Handle at the played position.
            const auto handleR = TeleMatrix::Style::ConvertScale(5);
            const auto handleX = seekRect.left() + playedW;
            const auto handleY = trackY + trackH / 2.0;
            dropShadow([&](qreal grow) {
                p.drawEllipse(
                    QPointF(handleX, handleY), handleR + grow, handleR + grow);
            });
            p.setBrush(st::mediaviewPlaybackActive);
            p.drawEllipse(QPointF(handleX, handleY), handleR, handleR);
        }
    }

    // Timer+mute pill (top-left) + fullscreen chip (top-right): drawn in EVERY
    // state — poster, startup/buffering, and playback — so they never blink when
    // playback starts. The still→playing switch used to leave a gap: `inlineActive`
    // flips true immediately (poster stops drawing) but the playback branch needed
    // a decoded frame, so during the startup/buffer window neither branch drew the
    // controls. Pill GEOMETRY always uses the metadata duration, keeping the pill
    // width and mute hit-test stable across the transition; only the timer TEXT
    // changes — full duration until a frame is actually playing, then a remaining-
    // time countdown. Suppressed on terminal failure (error overlay stands in), and
    // whenever there is nothing to play or expand: an upload the server hasn't got
    // yet, or a message whose redaction is already in flight.
    if (playable && (!inlineActive || !inlineVid->failed())) {
        const auto metaDurationMs = qint64(mediaDurationMs(item));
        const auto playing = inlineActive && !downloading
            && !inlineVid->currentFrame().isNull();
        const auto geo = videoTimePillGeometry(videoRect, metaDurationMs, true);
        if (!geo.pill.isNull()) {
            const auto remainingMs = (playing && inlineVid->durationMs() > 0)
                ? std::max<qint64>(
                    0, inlineVid->durationMs() - inlineVid->positionMs())
                : metaDurationMs;
            {
                PainterHighQualityEnabler hq(p);
                p.setPen(Qt::NoPen);
                p.setBrush(st::msgDateImgBg);
                p.drawRoundedRect(
                    geo.pill,
                    geo.pill.height() / 2.0,
                    geo.pill.height() / 2.0);
            }
            p.setFont(st::msgDateFont);
            p.setPen(st::msgDateImgFg);
            p.drawText(
                geo.text,
                Qt::AlignLeft | Qt::AlignVCenter,
                formatDuration(
                    static_cast<quint64>(std::max<qint64>(0, remainingMs))));
            drawVideoControlGlyph(
                p,
                geo.mute,
                (inlineVid && inlineVid->muted())
                    ? QStringLiteral("player_volume_off")
                    : QStringLiteral("player_volume_small"));
        }
        // Fullscreen-expand chip (top-right).
        const auto fsRect = videoFullscreenButtonRect(videoRect);
        {
            PainterHighQualityEnabler hq(p);
            p.setPen(Qt::NoPen);
            p.setBrush(st::msgDateImgBg);
            p.drawEllipse(fsRect);
        }
        drawVideoControlGlyph(
            p, fsRect, QStringLiteral("player_fullscreen"));
    }

    const auto timeStr = formatTime(item.timestamp);
    const auto &timeFm = st::fontMetrics(st::msgDateFont);
    // On-video date overlay (bottom-right): hidden only while the video is actually
    // playing (not when paused or stopped), so it doesn't clutter moving frames.
    const auto videoPlaying = inlineActive && inlineVid && !inlineVid->paused();
    if (!hasCaption && !videoPlaying) {
        const auto bottomInfoWidth = infoWidth(item);
        const int dateW = bottomInfoWidth + 2 * st::msgDateImgPadding.x();
        const int dateH = timeFm.height() + 2 * st::msgDateImgPadding.y();
        const QRect dateRect(
            videoRect.x() + videoRect.width() - st::msgDateImgDelta - dateW,
            videoRect.y() + videoRect.height() - st::msgDateImgDelta - dateH,
            dateW,
            dateH);
        {
            PainterHighQualityEnabler hq(p);
            p.setPen(Qt::NoPen);
            p.setBrush(st::msgDateImgBg);
            p.drawRoundedRect(dateRect, dateH / 2.0, dateH / 2.0);
        }
        const auto infoTop = dateRect.top() + st::msgDateImgPadding.y();
        const auto infoBaseline = infoTop + timeFm.ascent();
        paintBottomInfo(
            p,
            item,
            dateRect.left() + st::msgDateImgPadding.x(),
            infoTop,
            infoBaseline,
            timeStr,
            timeFm,
            dpr,
            mediaOverlayBottomInfoPalette(),
            context.sendingAnimationProgress);
    }
    contentTop += media.mediaHeight;
    if (hasCaption) {
        contentTop += st::mediaCaptionSkip;
        p.setPen(isOut ? st::historyTextOutFg : st::historyTextInFg);
        drawCachedLinkPreviewText(
            p,
            captionLayout.lines,
            st::msgFont,
            bubbleLeft + kBubblePaddingH,
            contentTop,
            st::msgFont->height);
        contentTop += captionHeight;
    }

    contentTop += drawReactions(bubbleLeft, bubbleWidth, contentTop, !hasCaption);

    if (hasCaption) {
        const auto bottomInfoWidth = infoWidth(item);
        const auto infoTop = bubbleH - kBubblePaddingV + kDateDeltaY - timeFm.height();
        const auto infoLeft = bubbleLeft + bubbleWidth - kBubblePaddingH + kDateDeltaX - bottomInfoWidth;
        const auto infoBaseline = infoTop + timeFm.ascent();
        paintBottomInfo(
            p,
            item,
            infoLeft,
            infoTop,
            infoBaseline,
            timeStr,
            timeFm,
            dpr,
            normalBottomInfoPalette(isOut),
            context.sendingAnimationProgress);
    }

    if (context.selectionMode && context.messageSelected) {
        paintSelectionOverlay(p, bubbleRect, videoCorners, videoTail);
    }
}

void paintAudioMessage(
        QPainter &p,
        const TimelineItem &item,
        const MessagePaintContext &context,
        bool isOut,
        bool showSender,
        const BubbleCorners &corners,
        BubbleTailSide tail,
        qreal dpr,
        const std::function<int(int, int, int, bool)> &drawReactions) {
    const auto bubbleWidth = HistoryViewAudio::audioBubbleWidth(item, context.width, isOut);
    const auto bubbleLeft = bubbleLeftFor(context, isOut, bubbleWidth);
    const auto fwdHeight = forwardedHeaderHeight(item, bubbleWidth - 2 * kBubblePaddingH);
    const auto audioReplyData = hasReply(item)
        ? resolveReplyData(item, context) : ReplyPreviewData{};
    const auto audioReplyH = !hasReply(item) ? 0
        : (audioReplyData.deleted ? replyDeletedHeight() : replyPreviewHeight(audioReplyData.hasThumb));
    auto bubbleH = HistoryViewAudio::bubbleHeight(item, context.width, context);
    if (audioReplyH > 0) {
        bubbleH += audioReplyH + kReplyPreviewBottomSkip;
    }
    bubbleH += fwdHeight;
    bubbleH += reactionBlockHeight(item, bubbleLeft, bubbleWidth);
    if (!item.reactions.isEmpty()) {
        bubbleH += kBubblePaddingV; // bottom padding after reactions
    }

    const QRectF bubbleRect(bubbleLeft, 0, bubbleWidth, bubbleH);
    paintBubbleLayer(
        p,
        bubbleRect,
        corners,
        tail,
        isOut ? st::msgOutShadow : st::msgInShadow,
        kBubbleShadow);
    paintBubbleLayer(
        p,
        bubbleRect,
        corners,
        tail,
        isOut ? st::msgOutBg : st::msgInBg);

    auto contentTop = 0;
    if (showSender) {
        const auto reserveFastReply = false /* reply pill is floating, no sender-row reservation */;
        p.setFont(st::msgNameFont);
        p.setPen(senderColor(item.sender.id));
        p.drawText(
            bubbleLeft + kBubblePaddingH,
            kBubblePaddingV + st::msgNameFont->ascent,
            elidedSenderName(item, bubbleWidth, reserveFastReply));
        if (reserveFastReply && context.isHovered && !context.selectionMode) {
            paintFastReplyAction(
                p,
                isOut,
                context.hoveredFastReply,
                bubbleLeft,
                bubbleWidth,
                kBubblePaddingV);
        }
        contentTop += kSenderNameHeight;
    }
    if (fwdHeight > 0) {
        const auto fwdStr = forwardedText(item);
        p.setFont(st::msgServiceFont);
        p.setPen(isOut ? st::msgOutServiceFg : st::msgInServiceFg);
        const auto fwdInnerW = bubbleWidth - 2 * kBubblePaddingH;
        const auto &fwdFm = st::fontMetrics(st::msgServiceFont);
        p.drawText(
            bubbleLeft + kBubblePaddingH,
            contentTop + st::msgServiceFont->ascent,
            fwdFm.elidedText(fwdStr, Qt::ElideRight, fwdInnerW));
        contentTop += fwdHeight;
    }
    if (audioReplyH > 0) {
        const QRect replyRect(
            bubbleLeft + kBubblePaddingH,
            contentTop,
            bubbleWidth - 2 * kBubblePaddingH,
            audioReplyH);
        paintReplyBlock(p, audioReplyData, replyRect, isOut, dpr,
            context.paintTarget, context.repaintTargetRect);
    }

    HistoryViewAudio::paint(
        p,
        item,
        context,
        isOut,
        showSender,
        bubbleLeft,
        bubbleWidth,
        dpr,
        drawReactions);

    const auto timeStr = formatTime(item.timestamp);
    const auto &timeFm = st::fontMetrics(st::msgDateFont);
    const auto bottomInfoWidth = infoWidth(item);
    const auto infoTop = bubbleH - kBubblePaddingV + kDateDeltaY - timeFm.height();
    const auto infoLeft = bubbleLeft + bubbleWidth - kBubblePaddingH + kDateDeltaX - bottomInfoWidth;
    const auto infoBaseline = infoTop + timeFm.ascent();
    paintBottomInfo(
        p,
        item,
        infoLeft,
        infoTop,
        infoBaseline,
        timeStr,
        timeFm,
        dpr,
        normalBottomInfoPalette(isOut),
        context.sendingAnimationProgress);

    if (context.selectionMode && context.messageSelected) {
        paintSelectionOverlay(p, bubbleRect, corners, tail);
    }
}

void paint(
    QPainter &p,
    const TimelineItem &item,
    const MessagePaintContext &context)
{
    const auto dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
    const auto isOut = item.delivery.outgoing;
    const auto showSender = showSenderName(item, context);
    const auto tail = bubbleTailFor(isOut, context.sameSenderBelow);
    const auto corners = bubbleCornersFor(
        isOut,
        context.sameSenderAbove,
        context.sameSenderBelow,
        tail);

    const auto drawReactions = [&p, &item, isOut](
            int bubbleLeft,
            int bubbleWidth,
            int top,
            bool mediaOnlyNoCaption = false) {
        return paintReactionPills(
            p,
            item,
            isOut,
            bubbleLeft,
            bubbleWidth,
            top,
            mediaOnlyNoCaption);
    };

    if (contentType(item) == ContentType::Service) {
        paintServiceMessage(p, item, context);
        return;
    }

    if (item.delivery.deleted) {
        paintStatusMessage(
            p,
            item,
            context,
            dpr,
            QCoreApplication::translate("HistoryMessage", "Deleted message"),
            italicStatusMessageFont(),
            st::windowSubTextFg,
            false);
        return;
    }

    if (contentType(item) == ContentType::UnableToDecrypt) {
        paintUnableToDecryptMessage(p, item, context, dpr);
        return;
    }

    if (contentType(item) == ContentType::Image) {
        paintImageMessage(p, item, context, isOut, showSender, dpr, drawReactions);
        return;
    }

    if (contentType(item) == ContentType::Audio) {
        paintAudioMessage(p, item, context, isOut, showSender, corners, tail, dpr, drawReactions);
        return;
    }

    if (contentType(item) == ContentType::File) {
        if (isAudioFile(item)) {
            paintAudioAttachment(p, item, context, isOut, showSender, corners, tail, dpr, drawReactions);
        } else {
            paintGenericAttachment(p, item, context, isOut, showSender, corners, tail, dpr, drawReactions);
        }
        return;
    }

    if (contentType(item) == ContentType::Video) {
        paintVideoMessage(p, item, context, isOut, showSender, dpr, drawReactions);
        return;
    }

    if (contentType(item) == ContentType::Poll) {
        paintPollMessage(p, item, context, dpr, drawReactions);
        return;
    }

    if (tryPaintLargeEmojiMessage(p, item, context, dpr, drawReactions)) {
        return;
    }

    paintTextMessage(p, item, context, dpr, drawReactions);

}

int cursorAt(
    const TimelineItem &item,
    const MessagePaintContext &context,
    QPoint pos,
    bool clamp)
{
    if (contentType(item) != ContentType::Text) {
        return -1;
    }
    // Large emoji messages are not text-selectable.
    if (context.largeEmojiEnabled
        && !hasReply(item)
        && forwardedSenderName(item).isEmpty()
        && !hasRenderableLinkPreview(item)
        && shouldRenderLargeEmoji(bodyText(item), true)) {
        return -1;
    }
    const auto isOut = item.delivery.outgoing;
    const auto &resolved = cachedText(item, isOut).resolved;
    if (resolved.plain.isEmpty()) {
        return -1;
    }

    const auto showSender = showSenderName(item, context);

    // Use computeTextBubbleMetrics for the correct (potentially widened)
    // bubble width, matching the paint function's variable-width expansion.
    auto bubbleLeft = 0;
    auto bubbleWidth = 0;
    auto textHeight = 0;
    if (!computeTextBubbleMetrics(item, context, bubbleLeft, bubbleWidth, textHeight)) {
        return -1;
    }

    // Match the variable-width expansion from the paint function so the
    // QTextLayout wraps text at the same positions as painting.
    auto availWidth = textAvailableWidth(context.width, isOut);
    {
        const auto &cmNarrow = cachedMetrics(item, isOut, availWidth);
        const auto preNatural = cmNarrow.preMaxNaturalWidth;
        if (preNatural > 0) {
            const auto preBubble = preNatural + 2 * kBubblePaddingH;
            if (preBubble > kMaxBubbleWidth) {
                const auto codeReserved = kMarginLeft + kPhotoSkip + 8;
                const auto viewportMax = qMax(
                    kMinBubbleWidth, context.width - codeReserved);
                const auto effectiveMax = qMin(viewportMax, preBubble);
                const auto widerAvail = effectiveMax - 2 * kBubblePaddingH;
                if (widerAvail > availWidth) {
                    availWidth = widerAvail;
                }
            }
        }
    }

    const auto &layout = cachedDrawableLayout(item, isOut, availWidth);

    // Text origin within the bubble (same calculation as paint()).
    const auto textLeft = bubbleLeft + kBubblePaddingH;
    auto textTop = kBubblePaddingV;
    if (showSender) {
        textTop += kSenderNameHeight;
    }
    textTop += forwardedHeaderHeight(item, bubbleWidth - 2 * kBubblePaddingH);
    if (hasReply(item)) {
        textTop += replyHeightFor(item, context) + kReplyPreviewBottomSkip;
    }

    // Check horizontal bubble bounds — reject points outside the bubble.
    if (!clamp && (pos.x() < bubbleLeft || pos.x() >= bubbleLeft + bubbleWidth)) {
        return -1;
    }

    const qreal localX = pos.x() - textLeft;
    const qreal localY = pos.y() - textTop;

    // Find which line the y falls on.
    for (int i = 0; i < layout.lineCount(); ++i) {
        const auto line = layout.lineAt(i);
        if (localY >= line.y() && localY < line.y() + line.height()) {
            return line.xToCursor(localX);
        }
    }

    if (clamp && layout.lineCount() > 0) {
        // Above all lines → start of first line.
        if (localY < layout.lineAt(0).y()) {
            return layout.lineAt(0).textStart();
        }
        // Below all lines → end of last line.
        const auto last = layout.lineAt(layout.lineCount() - 1);
        return last.textStart() + last.textLength();
    }

    return -1;
}

bool isOverText(
    const TimelineItem &item,
    const MessagePaintContext &context,
    QPoint pos)
{
    if (contentType(item) != ContentType::Text) {
        return false;
    }
    // Large emoji messages are not text-selectable.
    if (context.largeEmojiEnabled
        && !hasReply(item)
        && forwardedSenderName(item).isEmpty()
        && !hasRenderableLinkPreview(item)
        && shouldRenderLargeEmoji(bodyText(item), true)) {
        return false;
    }
    // Not over text if over timestamp area.
    if (timestampAt(item, context, pos)) {
        return false;
    }
    return cursorAt(item, context, pos, false) >= 0;
}

QString selectedText(
    const TimelineItem &item,
    int start,
    int end)
{
    if (contentType(item) != ContentType::Text) {
        return {};
    }
    if (start < 0 || end < 0 || start >= end) {
        return {};
    }
    const auto &resolved = cachedText(item, item.delivery.outgoing).resolved;
    const int len = qMin(end, resolved.plain.length()) - start;
    if (len <= 0) {
        return {};
    }
    QString text = resolved.plain.mid(start, len);
    text.replace(QChar::LineSeparator, u'\n');
    return text;
}

QString plainText(const TimelineItem &item) {
    if (contentType(item) != ContentType::Text) {
        return !captionText(item).isEmpty() ? captionText(item) : bodyText(item);
    }
    return cachedText(item, item.delivery.outgoing).resolved.plain;
}

QString linkAt(
    const TimelineItem &item,
    const MessagePaintContext &context,
    QPoint pos,
    int &outStart)
{
    if (contentType(item) != ContentType::Text) {
        outStart = -1;
        return {};
    }
    outStart = -1;
    const auto &resolved = cachedText(item, item.delivery.outgoing).resolved;
    if (resolved.links.isEmpty()) {
        return {};
    }

    const int cursor = cursorAt(item, context, pos, false);
    if (cursor < 0) {
        return {};
    }

    for (const auto &link : resolved.links) {
        if (cursor >= link.start && cursor < link.start + link.length) {
            outStart = link.start;
            return link.url;
        }
    }
    return {};
}

QString codeBlockCopyAt(
    const TimelineItem &item,
    const MessagePaintContext &context,
    QPoint pos)
{
    if (contentType(item) != ContentType::Text) {
        return {};
    }
    const auto &resolved = cachedText(item, item.delivery.outgoing).resolved;
    if (resolved.blocks.isEmpty()) {
        return {};
    }

    const auto isOut = item.delivery.outgoing;
    const auto showSender = showSenderName(item, context);

    auto bubbleLeft = 0;
    auto bubbleWidth = 0;
    auto textHeight = 0;
    if (!computeTextBubbleMetrics(item, context, bubbleLeft, bubbleWidth, textHeight)) {
        return {};
    }

    // Use the same (potentially widened) availWidth as the paint function
    // so that blockInfos topY values match the painted positions.
    auto availWidth = textAvailableWidth(context.width, isOut);
    {
        const auto &cmNarrow = cachedMetrics(item, isOut, availWidth);
        const auto preNatural = cmNarrow.preMaxNaturalWidth;
        if (preNatural > 0) {
            const auto preBubble = preNatural + 2 * kBubblePaddingH;
            if (preBubble > kMaxBubbleWidth) {
                const auto codeReserved = kMarginLeft + kPhotoSkip + 8;
                const auto viewportMax = qMax(
                    kMinBubbleWidth, context.width - codeReserved);
                const auto effectiveMax = qMin(viewportMax, preBubble);
                const auto widerAvail = effectiveMax - 2 * kBubblePaddingH;
                if (widerAvail > availWidth) {
                    availWidth = widerAvail;
                }
            }
        }
    }
    const auto &cm = cachedMetrics(item, isOut, availWidth);

    const auto textLeft = bubbleLeft + kBubblePaddingH;
    auto textTop = kBubblePaddingV;
    if (showSender) {
        textTop += kSenderNameHeight;
    }
    textTop += forwardedHeaderHeight(item, bubbleWidth - 2 * kBubblePaddingH);
    if (hasReply(item)) {
        textTop += replyHeightFor(item, context) + kReplyPreviewBottomSkip;
    }
    const auto decorWidth = bubbleWidth - 2 * kBubblePaddingH;

    // Check each Pre block's header rect.
    for (int i = 0; i < cm.blockInfos.size(); ++i) {
        const auto &bi = cm.blockInfos[i];
        if (bi.type != BlockType::Pre) continue;

        const QRect headerRect(
            textLeft,
            textTop + bi.topY,
            decorWidth,
            kPreStyle.header);

        if (headerRect.contains(pos)) {
            // Find the corresponding BlockRange to extract text.
            int preIdx = 0;
            for (int j = 0; j < resolved.blocks.size(); ++j) {
                if (resolved.blocks[j].type == BlockType::Pre) {
                    if (preIdx == i) {
                        const auto &br = resolved.blocks[j];
                        auto text = resolved.plain.mid(br.start, br.end - br.start);
                        text.replace(QChar::LineSeparator, u'\n');
                        return text;
                    }
                    // Only count Pre blocks (blockInfos may skip Blockquotes
                    // but both types are in blockInfos, so match by index).
                }
            }
            // Fallback: match by blockInfos index directly against blocks list.
            // blockInfos are generated 1:1 with blocks, so use index i.
            if (i < resolved.blocks.size()) {
                const auto &br = resolved.blocks[i];
                auto text = resolved.plain.mid(br.start, br.end - br.start);
                text.replace(QChar::LineSeparator, u'\n');
                return text;
            }
        }
    }
    return {};
}

QString linkPreviewUrlAt(
    const TimelineItem &item,
    const MessagePaintContext &context,
    QPoint pos)
{
    if (!isTextMessage(item)
        || !hasRenderableLinkPreview(item)) {
        return {};
    }

    const auto preview = urlPreviewInfo(item);
    if (!preview) {
        return {};
    }
    const auto isOut = item.delivery.outgoing;
    const auto showSender = showSenderName(item, context);

    auto bubbleLeft = 0;
    auto bubbleWidth = 0;
    auto textHeight = 0;
    if (!computeTextBubbleMetrics(item, context, bubbleLeft, bubbleWidth, textHeight)) {
        return {};
    }

    auto contentTop = kBubblePaddingV;
    if (showSender) {
        contentTop += kSenderNameHeight;
    }
    contentTop += forwardedHeaderHeight(item, bubbleWidth - 2 * kBubblePaddingH);
    if (hasReply(item)) {
        contentTop += replyHeightFor(item, context) + kReplyPreviewBottomSkip;
    }
    contentTop += textHeight; // past the message text
    contentTop += st::mediaInBubbleSkip;

    const auto lpAvailWidth = bubbleWidth - 2 * kBubblePaddingH;
    const auto lpH = linkPreviewBlockHeight(item, lpAvailWidth);
    const QRect lpRect(
        bubbleLeft + kBubblePaddingH,
        contentTop,
        lpAvailWidth,
        lpH);

    if (lpRect.contains(pos)) {
        return preview->url;
    }
    return {};
}

QString reactionPillAt(
    const TimelineItem &item,
    const MessagePaintContext &context,
    QPoint pos)
{
    if (item.reactions.isEmpty() || contentType(item) == ContentType::Service) {
        return {};
    }

    const auto metrics = computeReactionBarMetrics(item, context);
    if (!metrics.valid) {
        return {};
    }

    const auto &layouts = cachedReactionPills(
        item,
        metrics.bubbleLeft,
        metrics.bubbleWidth,
        metrics.top);
    for (const auto &layout : layouts) {
        if (layout.rect.contains(pos)) {
            return layout.key;
        }
    }
    return {};
}

bool hasFastReplyAction(
    const TimelineItem &item,
    const MessagePaintContext &context)
{
    return hasFastReplyAction(item, context.isGroup)
        && !context.selectionMode;
}

void paintFastReplyButton(
    QPainter &p,
    const TimelineItem &item,
    const MessagePaintContext &context)
{
    const auto geometry = computeBubbleGeometry(item, context);
    if (!geometry.valid) return;
    paintFastReplyAction(
        p,
        item.delivery.outgoing,
        context.hoveredFastReply,
        geometry.left,
        geometry.width,
        0);
}

QRect fastReplyRect(
    const TimelineItem &item,
    const MessagePaintContext &context)
{
    const auto geometry = computeBubbleGeometry(item, context);
    if (!geometry.valid) return {};
    return fastReplyActionRect(geometry.left, geometry.width, 0);
}

bool fastReplyAt(
    const TimelineItem &item,
    const MessagePaintContext &context,
    QPoint pos)
{
    if (!hasFastReplyAction(item, context)) {
        return false;
    }

    const auto geometry = computeBubbleGeometry(item, context);
    if (!geometry.valid || geometry.width <= (2 * kBubblePaddingH)) {
        return false;
    }

    const auto rect = fastReplyActionRect(
        geometry.left,
        geometry.width,
        0);
    return rect.contains(pos);
}

namespace {

// Default reaction shown on the resting button (first quick reaction, 👍).
const auto kReactionButtonEmoji = QString::fromUtf8("\xF0\x9F\x91\x8D");

// Vertical pitch between stacked cells in the expanded column.
// reactionCornerSize.height() + reactionCornerSkip = 32 + (-4) = 28.
[[nodiscard]] int reactionCellPitch() {
    return st::reactionCornerSize.height() + st::reactionCornerSkip;
}

struct ReactionButtonGeom {
    bool valid = false;
    int centerX = 0;  // center x of the inner pill (local coords)
    int innerTop = 0; // top of the inner pill (local coords)
};

[[nodiscard]] ReactionButtonGeom reactionButtonGeom(
        const TimelineItem &item,
        const MessagePaintContext &context) {
    ReactionButtonGeom g;
    const auto bubble = bubbleRect(item, context);
    if (bubble.isNull()) {
        return g;
    }
    const auto innerW = st::reactionCornerSize.width();
    const auto innerH = st::reactionCornerSize.height();
    // Bottom-right corner for all messages (this fork left-aligns every bubble,
    // so the right edge always has room), raised by reactionCornerCenter.y().
    const auto cx = bubble.left() + bubble.width() + st::reactionCornerCenter.x();
    const auto cy = bubble.top() + bubble.height() + st::reactionCornerCenter.y();
    const auto minSkip = (innerW
        + st::reactionCornerShadow.left()
        + st::reactionCornerShadow.right()) / 2;
    g.centerX = qBound(minSkip, cx, qMax(minSkip, context.width - minSkip));
    g.innerTop = cy - innerH / 2;
    g.valid = true;
    return g;
}

[[nodiscard]] QRect reactionButtonInner(const ReactionButtonGeom &g) {
    const auto innerW = st::reactionCornerSize.width();
    const auto innerH = st::reactionCornerSize.height();
    return QRect(g.centerX - innerW / 2, g.innerTop, innerW, innerH);
}

// Screen-top of cell `i` for the given direction/scroll. Cell 0 is the anchor
// (at the resting button position); others stack away from it.
[[nodiscard]] int reactionCellTop(
        int i,
        const QRect &columnInner,
        int scroll,
        bool expandUp) {
    const auto cellH = st::reactionCornerSize.height();
    const auto pitch = reactionCellPitch();
    return expandUp
        ? (columnInner.top() + columnInner.height() - cellH - i * pitch + scroll)
        : (columnInner.top() + i * pitch - scroll);
}

// Shadow + flat background + hairline border (matches the reply pill style).
void paintReactionPillBg(QPainter &p, const QRectF &inner, qreal radius) {
    {
        auto shadow = inner.adjusted(-1, -1, 1, 1);
        p.setPen(Qt::NoPen);
        p.setBrush(st::historyFastReplyShadowBg);
        p.drawRoundedRect(shadow.adjusted(0, 1, 0, 1), radius + 1, radius + 1);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(st::windowBg);
    p.drawRoundedRect(inner, radius, radius);
    p.setPen(QPen(st::historyFastReplyBorderFg, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(inner, radius, radius);
}

// Visual emoji size ≈ reactionCornerImage; the system emoji font renders ~1.5×
// its pixel size, so the font is ~2/3 of the target box (same calibration as the
// inline-chip renderEmojiImage). `scale` shrinks it for the resting button.
[[nodiscard]] int reactionEmojiFontPx(qreal scale) {
    return qMax(1, qRound(st::reactionCornerImage * (2.0 / 3.0) * scale));
}

void paintReactionCellEmoji(
        QPainter &p,
        const QRectF &cell,
        const QString &emoji,
        qreal scale) {
    QFont f;
    f.setPixelSize(reactionEmojiFontPx(scale));
    p.setFont(f);
    p.setPen(st::windowFg);
    p.drawText(cell, Qt::AlignCenter, emoji);
}

} // namespace

bool hasReactionButton(
        const TimelineItem &item,
        const MessagePaintContext &context) {
    return contentType(item) != ContentType::Service
        && !item.eventId.isEmpty()
        && !item.delivery.deleted
        && !context.selectionMode;
}

QRect reactionButtonRect(
        const TimelineItem &item,
        const MessagePaintContext &context) {
    const auto g = reactionButtonGeom(item, context);
    if (!g.valid) {
        return {};
    }
    return reactionButtonInner(g).marginsAdded(st::reactionCornerShadow);
}

void paintReactionButton(
        QPainter &p,
        const TimelineItem &item,
        const MessagePaintContext &context) {
    const auto g = reactionButtonGeom(item, context);
    if (!g.valid) {
        return;
    }
    PainterHighQualityEnabler hq(p);
    // Resting size = the "shown" state (2/3 of full). It grows to full
    // size when the cursor moves onto it and the column expands (focus = bigger).
    constexpr qreal kRestingScale = 2.0 / 3.0;
    const auto innerW = st::reactionCornerSize.width() * kRestingScale;
    const auto innerH = st::reactionCornerSize.height() * kRestingScale;
    const auto cx = g.centerX;
    const auto cy = g.innerTop + st::reactionCornerSize.height() / 2.0;
    const QRectF inner(cx - innerW / 2.0, cy - innerH / 2.0, innerW, innerH);
    paintReactionPillBg(p, inner, innerH / 2.0);
    paintReactionCellEmoji(p, inner, kReactionButtonEmoji, kRestingScale);
}

int reactionColumnContentInnerHeight(int count) {
    if (count <= 1) {
        return st::reactionCornerSize.height();
    }
    return st::reactionCornerSize.height() + (count - 1) * reactionCellPitch();
}

int reactionColumnVisibleInnerHeight(int count) {
    const auto content = reactionColumnContentInnerHeight(count);
    const auto cap = st::reactionCornerSize.height()
        + st::reactionCornerAddedHeightMax;
    return qMin(content, cap);
}

int reactionColumnScrollMax(int count) {
    return qMax(0,
        reactionColumnContentInnerHeight(count)
            - reactionColumnVisibleInnerHeight(count));
}

QRect reactionColumnRect(
        const TimelineItem &item,
        const MessagePaintContext &context,
        int count,
        bool expandUp) {
    const auto g = reactionButtonGeom(item, context);
    if (!g.valid || count <= 0) {
        return {};
    }
    const auto innerW = st::reactionCornerSize.width();
    const auto innerH = st::reactionCornerSize.height();
    const auto visible = reactionColumnVisibleInnerHeight(count);
    const auto top = expandUp ? (g.innerTop + innerH - visible) : g.innerTop;
    const auto columnInner = QRect(g.centerX - innerW / 2, top, innerW, visible);
    return columnInner.marginsAdded(st::reactionCornerShadow);
}

void paintReactionColumn(
        QPainter &p,
        const TimelineItem &item,
        const MessagePaintContext &context,
        const QVector<QString> &emojis,
        int scroll,
        int hoveredIndex,
        bool expandUp) {
    const auto count = int(emojis.size());
    if (count <= 0) {
        return;
    }
    const auto g = reactionButtonGeom(item, context);
    if (!g.valid) {
        return;
    }
    const auto innerW = st::reactionCornerSize.width();
    const auto innerH = st::reactionCornerSize.height();
    const auto visible = reactionColumnVisibleInnerHeight(count);
    const auto top = expandUp ? (g.innerTop + innerH - visible) : g.innerTop;
    const auto columnInner = QRect(g.centerX - innerW / 2, top, innerW, visible);

    PainterHighQualityEnabler hq(p);
    const auto inner = QRectF(columnInner);
    const auto radius = innerH / 2.0;
    paintReactionPillBg(p, inner, radius);

    auto minCellTop = columnInner.top();
    auto maxCellBottom = columnInner.bottom();

    p.save();
    {
        QPainterPath clip;
        clip.addRoundedRect(inner, radius, radius);
        p.setClipPath(clip);
    }
    for (int i = 0; i < count; ++i) {
        const auto cellTop = reactionCellTop(i, columnInner, scroll, expandUp);
        minCellTop = qMin(minCellTop, cellTop);
        maxCellBottom = qMax(maxCellBottom, cellTop + innerH);
        const auto cell = QRectF(columnInner.left(), cellTop, innerW, innerH);
        if (cell.bottom() < columnInner.top() || cell.top() > columnInner.bottom()) {
            continue;
        }
        if (i == hoveredIndex) {
            p.setPen(Qt::NoPen);
            p.setBrush(st::emojiPanHover);
            p.drawRoundedRect(cell.adjusted(2, 2, -2, -2), 8, 8);
        }
        paintReactionCellEmoji(p, cell, emojis[i], 1.0);
    }

    // Static fade gradients for hidden content above/below. Drawn while the
    // rounded clip is still active so they don't square off the corners.
    const auto gradH = TeleMatrix::Style::ConvertScale(st::reactionGradientSize);
    auto bg0 = st::windowBg;
    bg0.setAlpha(0);
    if (minCellTop < columnInner.top()) {
        QLinearGradient gr(inner.left(), inner.top(), inner.left(), inner.top() + gradH);
        gr.setColorAt(0.0, st::windowBg);
        gr.setColorAt(1.0, bg0);
        p.fillRect(QRectF(inner.left(), inner.top(), inner.width(), gradH), gr);
    }
    // +1 tolerance: cell 0 is the anchor sitting flush at the bottom, and its exclusive
    // bottom lands 1px past QRect::bottom() (inclusive). Without this the anchor (the
    // first emoji) is always covered by the fade even when nothing is scrolled off.
    if (maxCellBottom > columnInner.bottom() + 1) {
        QLinearGradient gr(inner.left(), inner.bottom() - gradH, inner.left(), inner.bottom());
        gr.setColorAt(0.0, bg0);
        gr.setColorAt(1.0, st::windowBg);
        p.fillRect(QRectF(inner.left(), inner.bottom() - gradH, inner.width(), gradH), gr);
    }
    p.restore();
}

int reactionColumnCellAt(
        const TimelineItem &item,
        const MessagePaintContext &context,
        int count,
        int scroll,
        bool expandUp,
        QPoint pos) {
    if (count <= 0) {
        return -1;
    }
    const auto g = reactionButtonGeom(item, context);
    if (!g.valid) {
        return -1;
    }
    const auto innerW = st::reactionCornerSize.width();
    const auto innerH = st::reactionCornerSize.height();
    const auto visible = reactionColumnVisibleInnerHeight(count);
    const auto top = expandUp ? (g.innerTop + innerH - visible) : g.innerTop;
    const auto columnInner = QRect(g.centerX - innerW / 2, top, innerW, visible);
    // Iterate top-most cells last so the on-top cell wins overlaps.
    for (int i = count - 1; i >= 0; --i) {
        const auto cellTop = reactionCellTop(i, columnInner, scroll, expandUp);
        const auto cell = QRect(columnInner.left(), cellTop, innerW, innerH);
        const auto clipped = cell.intersected(columnInner);
        if (clipped.contains(pos)) {
            return i;
        }
    }
    return -1;
}

QString replyTargetAt(
    const TimelineItem &item,
    const MessagePaintContext &context,
    QPoint pos)
{
    if (!hasReply(item)) {
        return {};
    }

    // Deleted reply is not clickable.
    const auto replyData = resolveReplyData(item, context);
    if (replyData.deleted) {
        return {};
    }

    const auto geometry = computeBubbleGeometry(item, context);
    if (!geometry.valid || geometry.width <= (2 * kBubblePaddingH)) {
        return {};
    }

    if (contentType(item) == ContentType::Service) {
        return {};
    }

    const auto showSender = showSenderName(item, context);
    // For Image/Video, contentTop starts at 0 when no sender; for others at kBubblePaddingV.
    const bool isMedia = (contentType(item) == ContentType::Image
        || contentType(item) == ContentType::Video
        || contentType(item) == ContentType::File
        || contentType(item) == ContentType::Audio);
    const auto contentTop = (isMedia
            ? (showSender ? (kBubblePaddingV + kSenderNameHeight) : 0)
            : (kBubblePaddingV + (showSender ? kSenderNameHeight : 0)))
        + forwardedHeaderHeight(item, geometry.width - 2 * kBubblePaddingH);
    const auto replyHeight = replyPreviewHeight(replyData.hasThumb);

    if (contentTop + replyHeight > geometry.height) {
        return {};
    }

    const QRect replyRect(
        geometry.left + kBubblePaddingH,
        contentTop,
        geometry.width - 2 * kBubblePaddingH,
        replyHeight);
    return replyRect.contains(pos) ? replyEventId(item) : QString();
}

QRect bubbleRect(
    const TimelineItem &item,
    const MessagePaintContext &context) {
    const auto geometry = computeBubbleGeometry(item, context);
    if (!geometry.valid) {
        return {};
    }
    return geometry.rect();
}

QPainterPath bubbleShapePath(
    const TimelineItem &item,
    const MessagePaintContext &context) {
    const auto geometry = computeBubbleGeometry(item, context);
    if (!geometry.valid) {
        return {};
    }
    return bubblePath(
        QRectF(
            geometry.left,
            geometry.top,
            geometry.width,
            geometry.height),
        geometry.corners,
        geometry.tail);
}

bool isInsideBubble(
    const TimelineItem &item,
    const MessagePaintContext &context,
    QPoint pos) {
    // Try the full geometry path first (handles most content types).
    const auto geometry = computeBubbleGeometry(item, context);
    if (geometry.valid) {
        return geometry.rect().contains(pos);
    }
    // Fallback for UTD/deleted and other types without geometry:
    // use bubbleHeight + a reasonable width range.
    const auto isOut = item.delivery.outgoing;
    const auto h = bubbleHeight(item, context.width, context);
    const auto w = qBound(kMinBubbleWidth, context.width / 2, kMaxBubbleWidth);
    const auto left = bubbleLeftFor(context, isOut, w);
    return QRect(left, 0, w, h).contains(pos);
}

bool isInsideUtdVerifyLink(
    const TimelineItem &item,
    const MessagePaintContext &context,
    QPoint pos) {
    if (contentType(item) != ContentType::UnableToDecrypt
        || context.itemGlowActive) {
        return false;
    }
    const auto L = computeUtdCardLayout(item);
    if (!L.showLink) {
        return false;
    }
    const auto isOut = item.delivery.outgoing;
    const auto bubbleLeft = bubbleLeftFor(context, isOut, L.bubbleWidth);
    const auto textLeft = bubbleLeft + kBubblePaddingH;
    auto linkY = kBubblePaddingV + L.titleHeight;
    if (L.showBody) {
        linkY += kUtdTitleGap + L.bodyHeight;
    }
    linkY += kUtdLinkGap;
    const auto linkW = st::fontMetrics(st::msgFont).horizontalAdvance(L.linkText);
    return QRect(textLeft, linkY, linkW, L.linkHeight).contains(pos);
}

bool timestampAt(
    const TimelineItem &item,
    const MessagePaintContext &context,
    QPoint pos)
{
    if (contentType(item) != ContentType::Text) {
        return false;
    }
    const auto geometry = computeBubbleGeometry(item, context);
    if (!geometry.valid) {
        return false;
    }
    const auto &timeFm = st::fontMetrics(st::msgDateFont);
    const auto bottomInfoWidth = infoWidth(item);

    const auto infoRight = geometry.left + geometry.width
        - kBubblePaddingH + kDateDeltaX;
    const auto infoBottom = geometry.height
        - kBubblePaddingV + kDateDeltaY;
    const auto dateX = infoRight - bottomInfoWidth;
    const auto dateTop = infoBottom - timeFm.height();

    const QRect timestampRect(dateX, dateTop, bottomInfoWidth, timeFm.height());
    return timestampRect.contains(pos);
}

bool isAudioBubble(const TimelineItem &item) {
    return contentType(item) == ContentType::Audio
        || (contentType(item) == ContentType::File && isAudioFile(item));
}

QRect audioPlayButtonRect(
    const TimelineItem &item,
    const MessagePaintContext &context)
{
    if (contentType(item) == ContentType::Audio) {
        return HistoryViewAudio::playButtonRect(item, context);
    }

    const auto isOut = item.delivery.outgoing;
    const auto showSender = showSenderName(item, context);
    const auto bubbleWidth = fileBubbleWidth(item, context.width, isOut);
    const auto bubbleLeft = bubbleLeftFor(context, isOut, bubbleWidth);
    const auto fwdHeight = forwardedHeaderHeight(
        item, bubbleWidth - 2 * kBubblePaddingH);
    const auto replyData = hasReply(item)
        ? resolveReplyData(item, context) : ReplyPreviewData{};
    const auto replyH = !hasReply(item) ? 0
        : (replyData.deleted ? replyDeletedHeight() : replyPreviewHeight(replyData.hasThumb));

    auto contentTop = 0;
    if (showSender) contentTop += kSenderNameHeight;
    contentTop += fwdHeight;
    if (replyH > 0) contentTop += replyH + kReplyPreviewBottomSkip;

    return QRect(
        bubbleLeft + st::docPaddingLeft,
        contentTop + st::docPaddingTop,
        st::docThumbSize,
        st::docThumbSize);
}

QRect senderAvatarRect(
    const TimelineItem &item,
    const MessagePaintContext &context) {
    // Avatar is shown for group-chat messages and for outgoing messages
    // in private chats when enabled by the history list.
    const bool showAvatar = (context.isGroup
            || (context.showOutgoingPrivateAvatar && item.delivery.outgoing))
        && !context.sameSenderBelow
        && contentType(item) != ContentType::Service;
    if (!showAvatar) {
        return QRect();
    }
    const auto avatarLeft = st::historyPhotoLeft;
    const auto bh = bubbleHeight(item, context.width, context);
    const auto avatarTop = bh - kPhotoSize;
    return QRect(avatarLeft, avatarTop, kPhotoSize, kPhotoSize);
}

bool senderAvatarAt(
    const TimelineItem &item,
    const MessagePaintContext &context,
    QPoint pos) {
    const auto rect = senderAvatarRect(item, context);
    if (rect.isNull()) {
        return false;
    }
    // Use circular hit-test (avatar is a circle).
    const auto center = rect.center();
    const auto dx = pos.x() - center.x();
    const auto dy = pos.y() - center.y();
    const auto r = rect.width() / 2;
    return (dx * dx + dy * dy) <= (r * r);
}

QRect reactionTriggerRect(
    const TimelineItem &item,
    const MessagePaintContext &context) {
    const auto bubble = bubbleRect(item, context);
    if (bubble.isNull()) {
        return {};
    }
    const auto triggerSize = TeleMatrix::Style::ConvertScale(28);
    const auto triggerSkip = TeleMatrix::Style::ConvertScale(4);
    // Position to the right of the bubble, vertically centered.
    const auto x = bubble.right() + triggerSkip;
    const auto y = bubble.top() + (bubble.height() - triggerSize) / 2;
    return QRect(x, y, triggerSize, triggerSize);
}

QRect uploadCancelRect(
    const TimelineItem &item,
    const MessagePaintContext &context) {
    if (!showUploadOverlay(item)) {
        return {};
    }
    const auto isOut = item.delivery.outgoing;

    switch (contentType(item)) {
    case ContentType::Image:
    case ContentType::Video: {
        // Overlay is centered on the media area (same as paint code).
        const auto bubble = bubbleRect(item, context);
        if (bubble.isNull()) return {};
        return uploadOverlayButtonRect(bubble);
    }
    case ContentType::File:
    case ContentType::Audio: {
        // Overlay is on the icon circle (left side of the bubble).
        const auto showSender = showSenderName(item, context);
        const auto bubbleWidth = (contentType(item) == ContentType::File)
            ? fileBubbleWidth(item, context.width, isOut)
            : HistoryViewAudio::audioBubbleWidth(item, context.width, isOut);
        const auto bubbleLeft = bubbleLeftFor(context, isOut, bubbleWidth);
        auto contentTop = showSender ? kSenderNameHeight : 0;
        contentTop += forwardedHeaderHeight(item, bubbleWidth - 2 * kBubblePaddingH);
        if (hasReply(item)) {
            contentTop += kReplyPreviewHeight + kReplyPreviewBottomSkip;
        }
        const auto iconRect = QRect(
            bubbleLeft + st::docPaddingLeft,
            contentTop + st::docPaddingTop,
            st::docThumbSize,
            st::docThumbSize);
        return iconRect;
    }
    default:
        return {};
    }
}

QRect videoMediaRect(
        const TimelineItem &item,
        const MessagePaintContext &context) {
    if (contentType(item) != ContentType::Video) {
        return {};
    }

    const auto showSender = showSenderName(item, context);
    const auto media = photoBubbleMetrics(item, kMaxBubbleWidth, showSender);
    const auto bubbleWidth = media.bubbleWidth;
    const auto bubbleLeft = bubbleLeftFor(context, item.delivery.outgoing, bubbleWidth);
    const auto fwdHeight = forwardedHeaderHeight(
        item, bubbleWidth - 2 * kBubblePaddingH);
    const auto replyData = hasReply(item)
        ? resolveReplyData(item, context) : ReplyPreviewData{};
    const auto replyH = !hasReply(item) ? 0
        : (replyData.deleted ? replyDeletedHeight() : replyPreviewHeight(replyData.hasThumb));

    auto contentTop = showSender ? (kBubblePaddingV + kSenderNameHeight) : 0;
    if (!showSender && (fwdHeight > 0 || replyH > 0)) {
        contentTop = kBubblePaddingV;
    }
    contentTop += fwdHeight;
    if (replyH > 0) {
        contentTop += replyH + kReplyPreviewBottomSkip;
    }
    if (contentTop > 0) {
        contentTop += st::mediaInBubbleSkip;
    }

    return QRect(
        bubbleLeft,
        contentTop,
        bubbleWidth,
        media.mediaHeight);
}

QRect videoSeekBarRect(const QRect &mediaRect) {
    if (mediaRect.isNull()) {
        return {};
    }
    const auto margin = TeleMatrix::Style::ConvertScale(8);
    const auto stripH = TeleMatrix::Style::ConvertScale(16);
    const auto bottomSkip = TeleMatrix::Style::ConvertScale(8);
    return QRect(
        mediaRect.left() + margin,
        mediaRect.bottom() - bottomSkip - stripH,
        mediaRect.width() - 2 * margin,
        stripH);
}

QRect videoFullscreenButtonRect(const QRect &mediaRect) {
    if (mediaRect.isNull()) {
        return {};
    }
    // Top-right chip, same height as the top-left time pill (font.height + 2*padY).
    const auto &fm = st::fontMetrics(st::msgDateFont);
    const auto inset = TeleMatrix::Style::ConvertScale(8);
    const auto size = fm.height() + 2 * st::msgDateImgPadding.y();
    return QRect(
        mediaRect.right() - inset - size,
        mediaRect.top() + inset,
        size,
        size);
}

QRect videoMuteButtonRect(const QRect &mediaRect, qint64 durationMs) {
    // The mute glyph sits at the RIGHT of the top-left time pill.
    // Shares videoTimePillGeometry with the paint so paint and hit-test agree; the
    // duration fixes the reserved text column so this rect doesn't move as the
    // countdown ticks.
    return videoTimePillGeometry(mediaRect, durationMs, true).mute;
}

QRect downloadCancelRect(
    const TimelineItem &item,
    const MessagePaintContext &context) {
    if (mediaUrl(item).isEmpty()
        || !mediaUrl(item).startsWith(QStringLiteral("mxc://"))
        || MediaCache::isResolved(mediaUrl(item))
        || !MediaCache::isRequested(mediaUrl(item))) {
        return {};
    }
    if (isAudioBubble(item)) {
        return audioPlayButtonRect(item, context);
    }
    if (contentType(item) == ContentType::Video) {
        const auto mediaRect = videoMediaRect(item, context);
        return mediaRect.isNull()
            ? QRect()
            : uploadOverlayButtonRect(mediaRect);
    }
    if (contentType(item) != ContentType::File) {
        return {};
    }

    const auto isOut = item.delivery.outgoing;
    const auto showSender = showSenderName(item, context);
    const auto bubbleWidth = fileBubbleWidth(item, context.width, isOut);
    const auto bubbleLeft = bubbleLeftFor(context, isOut, bubbleWidth);
    const auto fwdHeight = forwardedHeaderHeight(
        item, bubbleWidth - 2 * kBubblePaddingH);
    const auto replyData = hasReply(item)
        ? resolveReplyData(item, context) : ReplyPreviewData{};
    const auto replyH = !hasReply(item) ? 0
        : (replyData.deleted ? replyDeletedHeight() : replyPreviewHeight(replyData.hasThumb));

    auto contentTop = 0;
    if (showSender) {
        contentTop += kSenderNameHeight;
    }
    contentTop += fwdHeight;
    if (replyH > 0) {
        contentTop += replyH + kReplyPreviewBottomSkip;
    }

    return QRect(
        bubbleLeft + st::docPaddingLeft,
        contentTop + st::docPaddingTop,
        st::docThumbSize,
        st::docThumbSize);
}

QPoint reactionBarAnchor(
    const TimelineItem &item,
    const MessagePaintContext &context) {
    const auto bubble = bubbleRect(item, context);
    if (bubble.isNull()) {
        return {};
    }
    // Anchor above the bubble, right-aligned.
    return QPoint(bubble.right(), bubble.top() - TeleMatrix::Style::ConvertScale(4));
}

void clearPaintCache() {
    s_textCache.clear();
    s_metricsCache.clear();
    s_drawLayoutCache.clear();
    s_fwdLayoutCache.clear();
    s_reactionCache.clear();
    s_linkPreviewCache.clear();
}

// Separate from clearPaintCache (which runs on every room switch): emoji sprites
// recur across rooms, so this is cleared only on logout alongside the media
// caches.
void clearEmojiImageCache() {
    s_emojiImageCache.clear();
}

} // namespace HistoryMessage
} // namespace TeleMatrix

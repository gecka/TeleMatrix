// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_view_audio.h"

#include "ui/text/emoji_text.h"

#include "../../protocol/media_cache.h"
#include "ui/format_bytes.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QFontMetrics>
#include <QImage>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <cmath>

#include "styles/style_chat.h"
#include "styles/style_font_metrics.h"
#include "ui/style/icon_provider.h"

namespace TeleMatrix {
namespace {

constexpr int kWaveformSamplesCount = 100;
constexpr double kWaveformHoverOpacity = 0.30;
constexpr auto kFullArcLength = 360 * 16;
constexpr auto kQuarterArcLength = kFullArcLength / 4;

[[nodiscard]] QString formatDurationAudio(quint64 durationMs) {
    const auto totalSec = static_cast<quint64>(durationMs / 1000);
    if (totalSec >= 3600) {
        const auto hours = totalSec / 3600;
        const auto mins = (totalSec % 3600) / 60;
        const auto secs = totalSec % 60;
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(mins, 2, 10, QChar('0'))
            .arg(secs, 2, 10, QChar('0'));
    }
    const auto mins = totalSec / 60;
    const auto secs = totalSec % 60;
    return QStringLiteral("%1:%2")
        .arg(mins)
        .arg(secs, 2, 10, QChar('0'));
}

[[nodiscard]] qreal downloadAnimationPhase() {
    const auto period = qMax(1, st::radialPeriod);
    const auto now = QDateTime::currentMSecsSinceEpoch();
    return qreal(now % period) / qreal(period);
}

void scheduleContextRepaint(const MessagePaintContext &ctx) {
    if (!ctx.paintTarget) {
        return;
    }
    const auto rect = ctx.repaintTargetRect;
    QTimer::singleShot(16, ctx.paintTarget, [
            w = QPointer<QWidget>(ctx.paintTarget),
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

struct DownloadVisualState {
    bool active = false;
    bool determinate = false;
    bool decrypting = false;
    quint64 receivedBytes = 0;
    quint64 totalBytes = 0;
    double progress = 0.0;
};

[[nodiscard]] DownloadVisualState downloadVisualState(const TimelineItem &item) {
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

[[nodiscard]] QString downloadStatusText(const TimelineItem &item, const DownloadVisualState &state) {
    if (state.decrypting) {
        return QCoreApplication::translate("HistoryViewAudio", "Decrypting...");
    }
    if (state.determinate && state.totalBytes > 0) {
        return formatBytes(state.receivedBytes)
            + QStringLiteral(" / ")
            + formatBytes(state.totalBytes);
    }
    if (state.receivedBytes > 0) {
        return formatBytes(state.receivedBytes)
            + QCoreApplication::translate("HistoryViewAudio", " · Downloading");
    }
    const auto size = mediaSize(item);
    if (size > 0) {
        return formatBytes(size)
            + QCoreApplication::translate("HistoryViewAudio", " · Downloading");
    }
    return QCoreApplication::translate("HistoryViewAudio", "Downloading...");
}

void paintProgressArc(
    QPainter &p,
    const QRect &rect,
    double progress,
    const QColor &color) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const auto arcRect = QRectF(rect).adjusted(
        st::uploadRadialLine,
        st::uploadRadialLine,
        -st::uploadRadialLine,
        -st::uploadRadialLine);
    QPen pen(color);
    pen.setWidthF(st::uploadRadialLine);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawArc(arcRect, kQuarterArcLength, -qRound(qBound(0.0, progress, 1.0) * kFullArcLength));
    p.restore();
}

void paintLoadingArc(
    QPainter &p,
    const QRect &rect,
    const QColor &color) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const auto arcRect = QRectF(rect).adjusted(
        st::uploadRadialLine,
        st::uploadRadialLine,
        -st::uploadRadialLine,
        -st::uploadRadialLine);
    QPen pen(color);
    pen.setWidthF(st::uploadRadialLine);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawArc(
        arcRect,
        kQuarterArcLength - qRound(downloadAnimationPhase() * kFullArcLength),
        -(kFullArcLength / 4));
    p.restore();
}

[[nodiscard]] int bubbleMaxWidth(int maxWidth, [[maybe_unused]] bool isOut) {
    const auto reserved = HistoryMessage::kMarginLeft
        + HistoryMessage::kMarginRight
        + HistoryMessage::kPhotoSkip;
    return qMax(
        HistoryMessage::kMinBubbleWidth,
        qMin(HistoryMessage::kMaxBubbleWidth, maxWidth - reserved));
}

[[nodiscard]] int forwardedHeaderHeight(
    const TimelineItem &item,
    int innerWidth) {
    if (!item.forwardedFrom || item.forwardedFrom->senderName.isEmpty()) {
        return 0;
    }
    const auto &fm = st::fontMetrics(st::msgServiceFont);
    const auto text = QCoreApplication::translate("HistoryViewAudio", "Forwarded from %1")
        .arg(item.forwardedFrom->senderName);
    const auto lines = (fm.horizontalAdvance(text) > innerWidth) ? 2 : 1;
    return lines * st::msgServiceFont->height;
}

[[nodiscard]] int contentTopFor(
    const TimelineItem &item,
    const MessagePaintContext &ctx,
    int bubbleWidth) {
    const auto showSender = HistoryMessage::showSenderName(item, ctx);

    auto top = showSender ? HistoryMessage::kSenderNameHeight : 0;
    top += forwardedHeaderHeight(item, bubbleWidth - 2 * HistoryMessage::kBubblePaddingH);
    if (item.reply && !item.reply->eventId.isEmpty()) {
        top += HistoryMessage::kReplyPreviewHeight
            + HistoryMessage::kReplyPreviewBottomSkip;
    }
    return top;
}

[[nodiscard]] QString voiceStatusText(
    const TimelineItem &item,
    const MessagePaintContext &ctx,
    bool downloading,
    const DownloadVisualState &downloadState) {
    if (downloading) {
        return downloadStatusText(item, downloadState);
    }
    const auto isPlaying = ctx.audioState
        && ctx.audioState->playingEventId == item.eventId;
    const auto durationMs = (ctx.audioState && ctx.audioState->durationMs > 0)
        ? ctx.audioState->durationMs
        : static_cast<qint64>(mediaDurationMs(item));
    if ((isPlaying || ctx.voiceSeekHoverProgress >= 0.0) && durationMs > 0) {
        const auto positionMs = (ctx.voiceSeekHoverProgress >= 0.0)
            ? static_cast<qint64>(ctx.voiceSeekHoverProgress * durationMs)
            : ctx.audioState->positionMs;
        return formatDurationAudio(qMax<qint64>(0, positionMs))
            + QStringLiteral(" / ")
            + formatDurationAudio(durationMs);
    }
    const auto duration = mediaDurationMs(item);
    const auto size = mediaSize(item);
    const auto url = mediaUrl(item);
    if (duration > 0 && size > 0
        && url.startsWith(QStringLiteral("mxc://"))
        && !MediaCache::isResolved(url)) {
        return formatDurationAudio(duration)
            + QStringLiteral(" · ")
            + formatBytes(size);
    } else if (duration > 0) {
        return formatDurationAudio(duration);
    } else if (size > 0) {
        return formatBytes(size);
    }
    return QStringLiteral("0:00");
}

[[nodiscard]] QRect waveformRect(
    const TimelineItem &item,
    const MessagePaintContext &ctx,
    int bubbleLeft,
    int bubbleWidth) {
    const auto waveformLeft = bubbleLeft + st::docNameLeft;
    const auto waveformTop = contentTopFor(item, ctx, bubbleWidth) + st::docPaddingTop;
    const auto waveformWidth = qMax(
        0,
        bubbleWidth - st::docNameLeft - st::docPaddingRight + st::msgWaveformSkip);
    return QRect(
        waveformLeft,
        waveformTop,
        waveformWidth,
        st::msgWaveformMax);
}

void paintPlayButton(
    QPainter &p,
    const QRect &rect,
    bool isOut,
    const QString &iconName,
    qreal /*dpr*/) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(isOut ? st::msgFileOutBg : st::msgFileInBg);
    p.drawEllipse(rect);

    const auto iconColor = isOut
        ? st::historyFileOutIconFg
        : st::historyFileInIconFg;
    const auto icon = TeleMatrix::Style::IconProvider::tintedIcon(
        QStringLiteral(":/telematrix/icons/chat/"), iconName, iconColor);
    if (!icon.isNull()) {
        const auto iconW = icon.width() / icon.devicePixelRatio();
        const auto iconH = icon.height() / icon.devicePixelRatio();
        const auto x = rect.x() + ((rect.width() - iconW) / 2);
        const auto y = rect.y() + ((rect.height() - iconH) / 2);
        p.drawImage(QPointF(x, y), icon);
    }
    p.restore();
}

[[nodiscard]] int waveformSampleValue(const QByteArray &waveform, int index) {
    return qBound(
        0,
        static_cast<int>(static_cast<unsigned char>(waveform.at(index))),
        31);
}

[[nodiscard]] int waveformMaxSample(const QByteArray &waveform) {
    auto result = 0;
    for (auto i = 0, count = static_cast<int>(waveform.size()); i != count; ++i) {
        result = std::max(result, waveformSampleValue(waveform, i));
    }
    return result;
}

[[nodiscard]] int waveformBucketValue(const QByteArray &waveform, int bucket) {
    if (waveform.isEmpty()) {
        return 0;
    }
    const auto sourceSize = static_cast<int>(waveform.size());
    if (sourceSize == kWaveformSamplesCount) {
        return waveformSampleValue(waveform, bucket);
    }

    const auto start = (bucket * sourceSize) / kWaveformSamplesCount;
    auto end = ((bucket + 1) * sourceSize) / kWaveformSamplesCount;
    if (end <= start) {
        end = start + 1;
    }
    end = std::min(end, sourceSize);

    auto result = 0;
    for (auto i = start; i != end; ++i) {
        result = std::max(result, waveformSampleValue(waveform, i));
    }
    return result;
}

void paintWaveform(
    QPainter &p,
    const TimelineItem &item,
    const MessagePaintContext &ctx,
    bool isOut,
    const QRect &rect) {
    if (rect.width() <= 0) {
        return;
    }

    const auto active = isOut ? st::msgWaveformOutActive : st::msgWaveformInActive;
    const auto inactive = isOut ? st::msgWaveformOutInactive : st::msgWaveformInInactive;
    const auto audio = audioContent(item);
    const auto waveform = audio ? audio->waveform : QByteArray();
    const auto hasWaveform = !waveform.isEmpty();
    const auto barNormValue = hasWaveform
        ? (waveformMaxSample(waveform) + 1)
        : 1;
    const auto activeDurationMs = (ctx.audioState && ctx.audioState->durationMs > 0)
        ? ctx.audioState->durationMs
        : static_cast<qint64>(mediaDurationMs(item));
    const auto activeRatio = (ctx.audioState
            && ctx.audioState->playingEventId == item.eventId
            && activeDurationMs > 0)
        ? qBound(
            0.0,
            static_cast<double>(ctx.audioState->positionMs)
                / static_cast<double>(activeDurationMs),
            1.0)
        : 0.0;
    const auto activeWidth = qRound(rect.width() * activeRatio);
    const auto hoverWidth = (ctx.voiceSeekHoverProgress >= 0.0)
        ? qRound(rect.width() * qBound(0.0, ctx.voiceSeekHoverProgress, 1.0))
        : -1;
    const auto barStep = st::msgWaveformBar + st::msgWaveformSkip;
    const auto wfSize = kWaveformSamplesCount;
    const auto barCount = std::min(rect.width() / barStep, wfSize);
    if (barCount <= 0) {
        return;
    }
    const auto maxDelta = st::msgWaveformMax - st::msgWaveformMin;

    p.save();
    p.setPen(Qt::NoPen);
    p.setRenderHint(QPainter::Antialiasing, true);

    auto sum = 0;
    auto peak = 0;
    for (auto i = 0, barLeft = rect.x(); i < wfSize && (i == 0 || barLeft < rect.right()); ++i) {
        const auto value = hasWaveform
            ? waveformBucketValue(waveform, i)
            : 0;
        if (sum + barCount < wfSize) {
            peak = std::max(peak, value);
            sum += barCount;
            continue;
        }

        sum = sum + barCount - wfSize;
        if (sum < ((barCount + 1) / 2)) {
            peak = std::max(peak, value);
        }

        const auto barValue = ((peak * maxDelta) + (barNormValue / 2)) / barNormValue;
        const auto barHeight = st::msgWaveformMin + barValue;
        const auto barTop = rect.y() + ((st::msgWaveformMax - barValue) / 2.0);
        const auto barRight = barLeft + st::msgWaveformBar;

        if ((barLeft < rect.x() + activeWidth) && (barRight > rect.x() + activeWidth)) {
            const auto leftWidth = rect.x() + activeWidth - barLeft;
            const auto rightWidth = barRight - (rect.x() + activeWidth);
            if (leftWidth > 0) {
                p.fillRect(QRectF(barLeft, barTop, leftWidth, barHeight), active);
            }
            if (rightWidth > 0) {
                p.fillRect(
                    QRectF(rect.x() + activeWidth, barTop, rightWidth, barHeight),
                    inactive);
            }
        } else {
            p.fillRect(
                QRectF(barLeft, barTop, st::msgWaveformBar, barHeight),
                (barLeft >= rect.x() + activeWidth) ? inactive : active);
        }
        if (hoverWidth >= 0) {
            const auto hoverFrom = rect.x() + std::min(activeWidth, hoverWidth);
            const auto hoverTo = rect.x() + std::max(activeWidth, hoverWidth);
            if (barLeft < hoverTo && barRight > hoverFrom) {
                const auto left = std::max<double>(barLeft, hoverFrom);
                const auto right = std::min<double>(barRight, hoverTo);
                auto hover = active;
                hover.setAlphaF(qBound(0.0, kWaveformHoverOpacity, 1.0));
                p.fillRect(QRectF(left, barTop, right - left, barHeight), hover);
            }
        }

        peak = 0;
        barLeft += barStep;
    }

    p.restore();
}

} // namespace

namespace HistoryViewAudio {

int audioBubbleWidth(
    const TimelineItem &item,
    int maxWidth,
    bool isOut) {
    const auto maxBubble = bubbleMaxWidth(maxWidth, isOut);
    const auto minBubble = qMin(st::docMinWidth, maxBubble);
    if (isVoiceMessage(item)) {
        const auto waveformWidth = kWaveformSamplesCount
            * (st::msgWaveformBar + st::msgWaveformSkip);
        const auto neededWidth = st::docPaddingLeft
            + st::docThumbSize
            + st::docThumbSkip
            + waveformWidth
            + st::docPaddingRight;
        return qBound(minBubble, neededWidth, maxBubble);
    }

    const auto filename = mediaFilename(item);
    const auto fileName = !filename.isEmpty()
        ? filename
        : bodyText(item);
    const auto nameWidth = st::fontMetrics(st::semiboldFont).horizontalAdvance(fileName);
    const auto neededWidth = st::docNameLeft + nameWidth + st::docPaddingRight;
    return qBound(minBubble, neededWidth, maxBubble);
}

int bubbleHeight(
    const TimelineItem &item,
    [[maybe_unused]] int maxWidth,
    const MessagePaintContext &ctx) {
    const auto showSender = HistoryMessage::showSenderName(item, ctx);
    return st::docPaddingTop
        + st::docThumbSize
        + st::docPaddingBottom
        + (showSender ? HistoryMessage::kSenderNameHeight : 0);
}

void paint(
    QPainter &p,
    const TimelineItem &item,
    const MessagePaintContext &ctx,
    bool isOut,
    [[maybe_unused]] bool showSender,
    int bubbleLeft,
    int bubbleWidth,
    qreal dpr,
    const std::function<int(int, int, int, bool)> &drawReactions) {
    const auto contentTop = contentTopFor(item, ctx, bubbleWidth);
    const auto buttonRect = QRect(
        bubbleLeft + st::docPaddingLeft,
        contentTop + st::docPaddingTop,
        st::docThumbSize,
        st::docThumbSize);
    const auto isPlaying = ctx.audioState
        && ctx.audioState->playingEventId == item.eventId;
    const auto isPaused = isPlaying && ctx.audioState->isPaused;
    const auto downloadState = downloadVisualState(item);
    const auto downloading = downloadState.active;
    const auto showPlayIcon = !isPlaying || isPaused;
    const auto iconName = downloading
        ? QStringLiteral("history_file_cancel")
        : (showPlayIcon
            ? QStringLiteral("history_file_play")
            : QStringLiteral("history_file_pause"));
    paintPlayButton(p, buttonRect, isOut, iconName, dpr);
    if (downloading && downloadState.determinate) {
        const auto iconColor = isOut ? st::historyFileOutIconFg : st::historyFileInIconFg;
        paintProgressArc(p, buttonRect, downloadState.progress, iconColor);
    } else if (downloading) {
        const auto iconColor = isOut ? st::historyFileOutIconFg : st::historyFileInIconFg;
        paintLoadingArc(p, buttonRect, iconColor);
        scheduleContextRepaint(ctx);
    }

    if (isVoiceMessage(item)) {
        const auto waveform = waveformRect(item, ctx, bubbleLeft, bubbleWidth);
        paintWaveform(p, item, ctx, isOut, waveform);

        const auto statusText = voiceStatusText(item, ctx, downloading, downloadState);
        const auto textLeft = bubbleLeft + st::docNameLeft;
        const auto textWidth = qMax(1, bubbleWidth - st::docNameLeft - st::docPaddingRight);
        p.setFont(st::normalFont);
        p.setPen(isOut ? st::mediaOutFg : st::mediaInFg);
        p.drawText(
            textLeft,
            contentTop + st::docStatusTop + st::normalFont->ascent,
            st::fontMetrics(st::normalFont).elidedText(
                statusText,
                Qt::ElideRight,
                textWidth));
    } else {
        const auto textLeft = bubbleLeft + st::docNameLeft;
        const auto textWidth = qMax(1, bubbleWidth - st::docNameLeft - st::docPaddingRight);
        const auto filename = mediaFilename(item);
        const auto fileName = !filename.isEmpty() ? filename : bodyText(item);

        p.setFont(st::semiboldFont);
        p.setPen(isOut ? st::historyTextOutFg : st::historyTextInFg);
        const auto &nameEmoji = TeleMatrix::EmojiText::CachedMetricsFor(
            st::semiboldFont, st::emojiInlineSlot, st::emojiInlineGlyph);
        TeleMatrix::EmojiText::DrawLine(
            p,
            textLeft,
            contentTop + st::docNameTop + st::semiboldFont->ascent,
            TeleMatrix::EmojiText::Elide(
                fileName,
                st::semiboldFont,
                nameEmoji,
                textWidth,
                Qt::ElideMiddle),
            nameEmoji);

        QString statusText;
        const auto activeDurationMs = (ctx.audioState && ctx.audioState->durationMs > 0)
            ? ctx.audioState->durationMs
            : static_cast<qint64>(mediaDurationMs(item));
        const auto duration = mediaDurationMs(item);
        const auto size = mediaSize(item);
        if (downloading) {
            statusText = downloadStatusText(item, downloadState);
        } else if (isPlaying && activeDurationMs > 0) {
            statusText = formatDurationAudio(ctx.audioState->positionMs)
                + QStringLiteral(" / ")
                + formatDurationAudio(activeDurationMs);
        } else if (duration > 0 && size > 0) {
            statusText = formatDurationAudio(duration)
                + QStringLiteral(" · ")
                + formatBytes(size);
        } else if (duration > 0) {
            statusText = formatDurationAudio(duration);
        } else if (size > 0) {
            statusText = formatBytes(size);
        } else {
            const auto ext = QFileInfo(filename).suffix().toUpper();
            statusText = ext.isEmpty() ? QCoreApplication::translate("HistoryViewAudio", "Audio") : ext;
        }

        p.setFont(st::msgFont);
        p.setPen(isOut ? st::mediaOutFg : st::mediaInFg);
        const auto elidedStatus = st::fontMetrics(st::msgFont).elidedText(
            statusText,
            Qt::ElideRight,
            textWidth);
        p.drawText(
            textLeft,
            contentTop + st::docStatusTop + st::msgFont->ascent,
            elidedStatus);
    }

    drawReactions(
        bubbleLeft,
        bubbleWidth,
        contentTop + st::docPaddingTop + st::docThumbSize + st::docPaddingBottom,
        false);
}

QRect playButtonRect(
    const TimelineItem &item,
    const MessagePaintContext &ctx) {
    const auto bubbleWidth = audioBubbleWidth(item, ctx.width, item.delivery.outgoing);
    const auto bubbleLeft = HistoryMessage::kMarginLeft + HistoryMessage::kPhotoSkip;
    return QRect(
        bubbleLeft + st::docPaddingLeft,
        contentTopFor(item, ctx, bubbleWidth) + st::docPaddingTop,
        st::docThumbSize,
        st::docThumbSize);
}

[[maybe_unused]] double waveformSeekAt(
    const TimelineItem &item,
    const MessagePaintContext &ctx,
    QPoint pos,
    [[maybe_unused]] int x,
    [[maybe_unused]] int y,
    [[maybe_unused]] int width) {

    if (!isVoiceMessage(item)) {
        return -1.0;
    }

    const auto bubbleWidth = audioBubbleWidth(item, ctx.width, item.delivery.outgoing);
    const auto bubbleLeft = HistoryMessage::kMarginLeft + HistoryMessage::kPhotoSkip;
    const auto rect = waveformRect(item, ctx, bubbleLeft, bubbleWidth);
    if (!rect.contains(pos) || rect.width() <= 0) {
        return -1.0;
    }
    return qBound(
        0.0,
        static_cast<double>(pos.x() - rect.x()) / static_cast<double>(rect.width()),
        1.0);
}

} // namespace HistoryViewAudio
} // namespace TeleMatrix

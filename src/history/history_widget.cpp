// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_widget.h"

#include "ui/text/emoji_text.h"
#include "trust_shield.h"
#include "app/app_controller.h"
#include "app/unread_state_store.h"
#include "history_inline_video.h"
#include "history_list.h"
#include "unread_bar_placement.h"
#include "jump_routing.h"
#include "history_input.h"
#include "history_message.h"
#include "history_emoji_picker.h"
#include "history_down_button.h"
#include "history_pinned_bar.h"
#include "history_confirm_dialog.h"
#include "ui/safe_url.h"
#include "history_forward_dialog.h"
#include "dialogs/saved_messages.h"
#include "history_attach_popup.h"
#include "history_prepared_upload.h"
#include "image_recompress.h"
#include "history_send_files_dialog.h"
#include "../dialogs/dialogs_invite_users_box.h"
#include "../dialogs/dialogs_room_info_box.h"
#include "../settings/dialogs/verify_session_popup.h"
#include "../settings/dialogs/verify_user_dialog.h"
#include "../ui/empty_userpic.h"
#include "../ui/widgets/connecting_radial.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDateTime>
#include <QTimeZone>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QDebug>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImageReader>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QMimeDatabase>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QToolTip>
#include <QSet>
#include <QFont>
#include <QPainter>
#include <QPalette>
#include <QRegularExpression>
#include <QHBoxLayout>
#include <QLabel>
#include <QUrl>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTextDocument>
#include <QTemporaryFile>
#include <QtConcurrent>
#include <QVariantAnimation>
#include <QVideoSink>
#include <QVideoFrame>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <functional>
#include <type_traits>
#include <utility>

#include "ui/style/icon_provider.h"
#include "../protocol/protocol_bridge.h"
#include "../protocol/media_cache.h"

#include "history_popup_menu_style.h"
#ifdef Q_OS_MAC
#include "ui/platform/ui_utility_mac.h"
#endif
#include "ui/painter.h"
#include "ui/toast_widget.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/scroll_area.h"
#include "styles/style_constants.h"
#include "theme/chat_background_style.h"
#include "theme/theme_manager.h"

using namespace Qt::Literals::StringLiterals;

namespace Ui {
class TextButton;
} // namespace Ui

namespace TeleMatrix {

namespace {

constexpr int kBottomSnapTolerance = 8;
constexpr int kPendingJumpTimeoutMs = 15000;
// Minimum time the empty "Loading…" cover stays up once a jump needs a fetch, so
// a fast (local-cache) fetch shows a calm 1s state instead of a sub-second flash.
constexpr int kJumpLoadingMinMs = 1000;
constexpr quint64 kMaxMemoryAudioBytes = 50ull * 1024ull * 1024ull;
// Max initial-entry scroll attempts before force-settling the unread delimiter
// (so its freeze latch always resolves even if the boundary can't be paged in).
constexpr int kMaxUnreadEntryAttempts = 6;
// The Join bar sits where the composer would; same height as the invite button bar.
constexpr int kJoinBarButtonHeight = 46;
// How many events to pull per older page of an unjoined room's preview. Larger than a normal window
// because bridge join/leave churn is dropped, so a page often maps to few or zero displayable
// messages — a bigger page means fewer round-trips to reach the real ones. (initialSync, the first
// page, ignores this and uses the server default.)
constexpr int kPreviewMessageCount = 100;
// Give up auto-paginating after this many CONSECUTIVE pages that added no displayable message (an
// all-bot room would otherwise walk its entire history). Reset whenever a page yields a message.
constexpr int kPreviewMaxEmptyPages = 20;

// Rounded surface painted with live st:: colors (so it tracks theme changes)
// instead of a frozen stylesheet background. Used by the invite info card.
class RoundedPanel : public QWidget {
public:
    explicit RoundedPanel(QWidget *parent) : QWidget(parent) {}

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(st::boxBg);
        p.drawRoundedRect(rect(), st::boxRadius, st::boxRadius);
    }
};

// Top bar values (runtime-scaled).
inline int kTopBarHeight = 54;
inline int kTopBarSearchWidth = 40;
inline int kTopBarPinnedWidth = 40;
inline int kTopBarMenuWidth = 44;
inline int kTopBarSkip = -5;
inline int kTopBarNameLeft = 17;       // topBarArrowPadding.right
inline int kTopBarNameTop = 8;         // topBarArrowPadding.top
inline int kTopBarNameRightPadding = 3; // topBarNameRightPadding
inline int kTopBarStatusBottomPadding = 8; // topBarArrowPadding.bottom
inline int kTopBarRippleTop = 7;       // rippleAreaPosition.y
inline int kTopBarRippleSize = 40;     // rippleAreaSize
inline QPoint kTopBarSearchIconPos(4, 11); // topBarSearch.iconPosition
inline QPoint kTopBarMenuIconPos(16, 17); // topBarMenuToggle.iconPosition
inline int kTopBarStatusIconSize = 14;
inline int kTopBarStatusIconSkip = 4;
// Selection toolbar buttons.
inline int kTopBarActionSkip = 10;        // topBarActionSkip
inline int kActiveButtonHeight = 34;      // defaultActiveButton.height
inline int kActiveButtonPadding = 34;     // -defaultActiveButton.width
inline int kClearButtonPadding = 18;      // -topBarClearButton.width
inline int kButtonTextTop = 8;            // defaultActiveButton.textTop
inline int kButtonRadius = 4;             // buttonRadius
inline int kNumbersSkip = 7;              // defaultActiveButton.numbersSkip

void applyTopBarScale() {
    using TeleMatrix::Style::ConvertScale;
    kTopBarHeight = ConvertScale(54);
    kTopBarSearchWidth = ConvertScale(40);
    kTopBarPinnedWidth = ConvertScale(40);
    kTopBarMenuWidth = ConvertScale(44);
    kTopBarSkip = ConvertScale(-5);
    kTopBarNameLeft = ConvertScale(17);
    kTopBarNameTop = ConvertScale(8);
    kTopBarNameRightPadding = ConvertScale(3);
    kTopBarStatusBottomPadding = ConvertScale(8);
    kTopBarRippleTop = ConvertScale(7);
    kTopBarRippleSize = ConvertScale(40);
    kTopBarSearchIconPos = QPoint(ConvertScale(4), ConvertScale(11));
    kTopBarMenuIconPos = QPoint(ConvertScale(16), ConvertScale(17));
    kTopBarStatusIconSize = ConvertScale(14);
    kTopBarStatusIconSkip = ConvertScale(4);
    kTopBarActionSkip = ConvertScale(10);
    kActiveButtonHeight = ConvertScale(34);
    kActiveButtonPadding = ConvertScale(34);
    kClearButtonPadding = ConvertScale(18);
    kButtonTextTop = ConvertScale(8);
    kButtonRadius = ConvertScale(4);
    kNumbersSkip = ConvertScale(7);
}

[[nodiscard]] bool isScrollNearBottom(const Ui::ScrollArea *scroll) {
    return scroll
        && scroll->scrollTop() >= scroll->scrollTopMax() - kBottomSnapTolerance;
}

[[nodiscard]] bool isToolbarMuted(RoomNotificationMode mode, bool isMuted) {
    return isMuted || mode != RoomNotificationMode::AllMessages;
}

void applySenderAvatarFallback(
        TimelineItem &message,
        const QHash<QString, QString> &memberAvatarCache,
        const AppController *controller) {
    if (message.sender.avatarUrl.isEmpty()) {
        const auto it = memberAvatarCache.constFind(message.sender.id);
        if (it != memberAvatarCache.cend()) {
            message.sender.avatarUrl = it.value();
        } else if (message.delivery.outgoing
            && controller
            && !controller->avatarUrl().isEmpty()) {
            message.sender.avatarUrl = controller->avatarUrl();
        }
    }
}


bool isTransientOutgoingEmptyTextMessage(const TimelineItem &item) {
    if (!item.delivery.outgoing || !isTextMessage(item)) {
        return false;
    }
    if (!bodyText(item).trimmed().isEmpty()) {
        return false;
    }
    const auto formatted = formattedText(item);
    if (formatted.isEmpty()) {
        return true;
    }

    // Matrix/SDK may emit short-lived local-echo placeholders where both
    // plain and rendered text are effectively empty.
    QTextDocument doc;
    doc.setHtml(formatted);
    return doc.toPlainText().trimmed().isEmpty();
}

[[nodiscard]] bool isLocalOnlyFailedTextEcho(const TimelineItem &item) {
    return item.delivery.outgoing
        && isTextMessage(item)
        && item.delivery.sendState == SendState::Failed
        && item.transactionId.isEmpty()
        && item.eventId.startsWith(QStringLiteral("local-"));
}

void dropTransientOutgoingEmptyTextMessages(QVector<TimelineItem> &messages) {
    messages.erase(
        std::remove_if(
            messages.begin(),
            messages.end(),
            [](const TimelineItem &item) {
                return isTransientOutgoingEmptyTextMessage(item);
            }),
        messages.end());
}

[[nodiscard]] bool isLocalSendQueueMxc(const QString &url) {
    return url.startsWith(QStringLiteral("mxc://send-queue.localhost/"));
}

[[nodiscard]] bool isLocalMediaUploadPlaceholder(const TimelineItem &item) {
    if (!item.delivery.outgoing || item.delivery.sendState != SendState::Sending) {
        return false;
    }
    switch (contentType(item)) {
    case ContentType::Image:
    case ContentType::Video:
    case ContentType::File:
    case ContentType::Audio:
        return isLocalSendQueueMxc(mediaUrl(item))
            || isLocalSendQueueMxc(mediaThumbUrl(item));
    case ContentType::Text:
    case ContentType::Service:
    case ContentType::Poll:
    case ContentType::UnableToDecrypt:
        return false;
    }
    return false;
}

[[nodiscard]] bool isRemoteMediaTimelineItem(const TimelineItem &item) {
    if (!item.delivery.outgoing || item.delivery.deleted || isTextMessage(item)
        || isServiceMessage(item) || pollContent(item)
        || isUnableToDecryptMessage(item)) {
        return false;
    }
    return (!mediaUrl(item).isEmpty() && !isLocalSendQueueMxc(mediaUrl(item)))
        || (!mediaThumbUrl(item).isEmpty() && !isLocalSendQueueMxc(mediaThumbUrl(item)));
}

// A message whose caption can be edited in place: any media (image/video/file/audio) message.
// Editing such a message must change only its caption (empty clears it) instead of replacing the
// file with a text message — and an emptied caption must not delete the whole message.
[[nodiscard]] bool isMediaCaptionMessage(const TimelineItem &item) {
    return std::holds_alternative<TimelineImageContent>(item.content)
        || std::holds_alternative<TimelineVideoContent>(item.content)
        || std::holds_alternative<TimelineFileContent>(item.content)
        || std::holds_alternative<TimelineAudioContent>(item.content);
}

[[nodiscard]] bool mediaItemsLikelyMatch(
        const TimelineItem &local,
        const TimelineItem &remote) {
    if (!remote.transactionId.isEmpty() && remote.transactionId == local.eventId) {
        return true;
    }
    if (contentType(local) != contentType(remote)
        || local.sender.id != remote.sender.id
        || !remote.delivery.outgoing) {
        return false;
    }

    if (!mediaFilename(local).isEmpty()
        && !mediaFilename(remote).isEmpty()
        && mediaFilename(local) != mediaFilename(remote)) {
        return false;
    }
    if (mediaSize(local) > 0 && mediaSize(remote) > 0 && mediaSize(local) != mediaSize(remote)) {
        return false;
    }
    if (mediaWidth(local) > 0 && mediaWidth(remote) > 0 && mediaWidth(local) != mediaWidth(remote)) {
        return false;
    }
    if (mediaHeight(local) > 0 && mediaHeight(remote) > 0 && mediaHeight(local) != mediaHeight(remote)) {
        return false;
    }
    if (mediaDurationMs(local) > 0
        && mediaDurationMs(remote) > 0
        && mediaDurationMs(local) != mediaDurationMs(remote)) {
        return false;
    }
    if (!captionText(local).isEmpty()
        && !captionText(remote).isEmpty()
        && captionText(local) != captionText(remote)) {
        return false;
    }
    if (!bodyText(local).isEmpty() && !bodyText(remote).isEmpty() && bodyText(local) != bodyText(remote)) {
        return false;
    }

    // Tight timestamp window for fuzzy fallback — only match items
    // within 5 seconds to avoid cross-matching sequential uploads.
    return std::llabs(local.timestamp - remote.timestamp) <= 5;
}

void dropAcknowledgedOutgoingMediaPlaceholders(QVector<TimelineItem> &messages) {
    if (messages.size() < 2) {
        return;
    }

    QVector<bool> drop(messages.size(), false);
    QVector<bool> remoteUsed(messages.size(), false);

    for (int i = 0; i != messages.size(); ++i) {
        const auto &local = messages[i];
        if (!isLocalMediaUploadPlaceholder(local)) {
            continue;
        }

        auto bestIndex = -1;
        auto bestDistance = std::numeric_limits<qint64>::max();
        for (int j = 0; j != messages.size(); ++j) {
            if (i == j || remoteUsed[j]) {
                continue;
            }
            const auto &remote = messages[j];
            if (!isRemoteMediaTimelineItem(remote) || !mediaItemsLikelyMatch(local, remote)) {
                continue;
            }
            const auto distance = std::llabs(local.timestamp - remote.timestamp);
            if (distance < bestDistance) {
                bestDistance = distance;
                bestIndex = j;
            }
        }

        if (bestIndex >= 0) {
            drop[i] = true;
            remoteUsed[bestIndex] = true;
        }
    }

    if (std::none_of(drop.cbegin(), drop.cend(), [](bool value) { return value; })) {
        return;
    }

    QVector<TimelineItem> filtered;
    filtered.reserve(messages.size());
    for (int i = 0; i != messages.size(); ++i) {
        if (!drop[i]) {
            filtered.push_back(messages[i]);
        }
    }
    messages = std::move(filtered);
}

[[nodiscard]] bool reactionsEqual(
        const QVector<ReactionInfo> &a,
        const QVector<ReactionInfo> &b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (auto i = 0; i < a.size(); ++i) {
        if (a[i].key != b[i].key
            || a[i].count != b[i].count
            || a[i].isSelf != b[i].isSelf) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool mediaEqualIgnoringDuration(
        TimelineMediaContent a,
        TimelineMediaContent b) {
    a.durationMs = 0;
    b.durationMs = 0;
    return a == b;
}

[[nodiscard]] bool contentEqualIgnoringMediaDuration(
        const TimelineContent &a,
        const TimelineContent &b) {
    if (a.index() != b.index()) {
        return false;
    }
    return std::visit([](const auto &left, const auto &right) -> bool {
        using Left = std::decay_t<decltype(left)>;
        using Right = std::decay_t<decltype(right)>;
        if constexpr (!std::is_same_v<Left, Right>) {
            return false;
        } else if constexpr (std::is_same_v<Left, TimelineImageContent>
            || std::is_same_v<Left, TimelineVideoContent>
            || std::is_same_v<Left, TimelineFileContent>) {
            return mediaEqualIgnoringDuration(left.media, right.media);
        } else if constexpr (std::is_same_v<Left, TimelineAudioContent>) {
            return mediaEqualIgnoringDuration(left.media, right.media)
                && left.isVoice == right.isVoice
                && left.waveform == right.waveform;
        } else {
            return left == right;
        }
    }, a, b);
}

[[nodiscard]] bool timelineItemEqualIgnoringReactions(
        const TimelineItem &a,
        const TimelineItem &b) {
    return a.eventId == b.eventId
        && a.transactionId == b.transactionId
        && a.sender == b.sender
        && contentType(a) == contentType(b)
        // Skip mediaDurationMs — it can be locally probed and differ
        // from server data, causing infinite reload loops.
        && contentEqualIgnoringMediaDuration(a.content, b.content)
        && a.reply == b.reply
        && a.forwardedFrom == b.forwardedFrom
        && a.isEdited == b.isEdited
        && a.isPinned == b.isPinned
        && a.delivery == b.delivery
        && a.timestamp == b.timestamp
        && a.urlPreview == b.urlPreview
        && a.encryption == b.encryption;
}

[[nodiscard]] bool timelineEqual(
        const QVector<TimelineItem> &a,
        const QVector<TimelineItem> &b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (auto i = 0; i < a.size(); ++i) {
        if (!timelineItemEqualIgnoringReactions(a[i], b[i])
            || !reactionsEqual(a[i].reactions, b[i].reactions)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool reactionOnlyDelta(
        const QVector<TimelineItem> &current,
        const QVector<TimelineItem> &incoming) {
    if (current.size() != incoming.size()) {
        return false;
    }

    auto changed = false;
    for (auto i = 0; i < current.size(); ++i) {
        if (!timelineItemEqualIgnoringReactions(current[i], incoming[i])) {
            return false;
        }
        if (!reactionsEqual(current[i].reactions, incoming[i].reactions)) {
            changed = true;
        }
    }
    return changed;
}

struct PreparedImageUpload {
    QString path;
    QString mime;
    QString filename;
    quint64 fileSize = 0;
    int width = 0;
    int height = 0;
};

PreparedImageUpload prepareImageUpload(
        const QString &sourcePath,
        const QFileInfo &sourceInfo,
        const QString &detectedFormat) {
    PreparedImageUpload result;
    result.path = sourcePath;
    result.mime = QStringLiteral("image/%1").arg(detectedFormat);
    result.filename = sourceInfo.fileName();
    result.fileSize = static_cast<quint64>(qMax<qint64>(0, sourceInfo.size()));

    // Header-only sizing: never full-decode a multi-megapixel image just for
    // its dimensions. We deliberately leave autoTransform OFF so size() is the
    // raw stored size, then apply the EXIF orientation ourselves (90/270 swap
    // w/h) — robust regardless of how Qt's size() interacts with autoTransform.
    QImageReader reader(sourcePath);
    reader.setDecideFormatFromContent(true);
    const auto oriented = applyOrientationToSize(
        reader.size(), reader.transformation());
    result.width = qMax(0, oriented.width());
    result.height = qMax(0, oriented.height());
    return result;
}

[[nodiscard]] QString generateUploadTransactionId() {
    auto uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    uuid.remove(QLatin1Char('-'));
    return QStringLiteral("tmcpp") + uuid;
}

[[nodiscard]] QString uploadVideoThumbTempDir() {
    auto root = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (root.isEmpty()) {
        root = QDir::tempPath();
    }
    return QDir(root).filePath(QStringLiteral("telematrix-video-thumbs"));
}

[[nodiscard]] QString createUploadVideoThumbPath() {
    const auto dir = uploadVideoThumbTempDir();
    QDir().mkpath(dir);
    QTemporaryFile file(QDir(dir).filePath(QStringLiteral("thumb_XXXXXX.jpg")));
    file.setAutoRemove(false);
    if (!file.open()) {
        return QString();
    }
    const auto path = file.fileName();
    file.close();
    return path;
}

[[nodiscard]] bool isUploadVideoThumbPath(const QString &path) {
    if (path.isEmpty()) {
        return false;
    }
    const auto expectedDir = QDir(uploadVideoThumbTempDir()).absolutePath();
    return QFileInfo(path).absoluteDir().absolutePath() == expectedDir;
}

void removeUploadVideoThumb(const QString &path) {
    if (isUploadVideoThumbPath(path)) {
        QFile::remove(path);
    }
}

void clearUploadVideoThumbTempDir() {
    QDir(uploadVideoThumbTempDir()).removeRecursively();
}

// Recompressed-image temp files (Phase 3 "Compress images"). Same lifecycle as
// video thumbs: created off the UI thread, dropped when the upload is
// acknowledged or fails, and the whole dir wiped on startup.
constexpr int kCompressMaxEdge = 2560;     // longest edge, like Telegram
constexpr int kCompressJpegQuality = 85;

[[nodiscard]] QString uploadCompressedImageTempDir() {
    auto root = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (root.isEmpty()) {
        root = QDir::tempPath();
    }
    return QDir(root).filePath(QStringLiteral("telematrix-compressed-images"));
}

[[nodiscard]] QString createCompressedImagePath() {
    const auto dir = uploadCompressedImageTempDir();
    QDir().mkpath(dir);
    QTemporaryFile file(QDir(dir).filePath(QStringLiteral("img_XXXXXX.jpg")));
    file.setAutoRemove(false);
    if (!file.open()) {
        return QString();
    }
    const auto path = file.fileName();
    file.close();
    return path;
}

[[nodiscard]] bool isUploadCompressedImagePath(const QString &path) {
    if (path.isEmpty()) {
        return false;
    }
    const auto expectedDir = QDir(uploadCompressedImageTempDir()).absolutePath();
    return QFileInfo(path).absoluteDir().absolutePath() == expectedDir;
}

void removeUploadCompressedImage(const QString &path) {
    if (isUploadCompressedImagePath(path)) {
        QFile::remove(path);
    }
}

void clearUploadCompressedImageTempDir() {
    QDir(uploadCompressedImageTempDir()).removeRecursively();
}


struct ReplyComposePreview {
    QString text;
    QString path;
};

[[nodiscard]] ReplyComposePreview composeReplyPreviewFor(const TimelineItem &item) {
    ReplyComposePreview result;
    const auto caption = captionText(item).simplified();
    const auto body = bodyText(item).simplified();
    switch (contentType(item)) {
    case ContentType::Image:
        result.text = !caption.isEmpty()
            ? caption
            : (!body.isEmpty() ? body : QCoreApplication::translate("HistoryWidget", "Photo"));
        result.path = mediaUrl(item);
        return result;
    case ContentType::Video:
        result.text = !caption.isEmpty()
            ? caption
            : (!body.isEmpty() ? body : QCoreApplication::translate("HistoryWidget", "Video"));
        result.path = !mediaThumbUrl(item).isEmpty()
            ? mediaThumbUrl(item)
            : mediaUrl(item);
        return result;
    case ContentType::File:
        result.text = !mediaFilename(item).simplified().isEmpty()
            ? mediaFilename(item).simplified()
            : (!body.isEmpty() ? body : QCoreApplication::translate("HistoryWidget", "File"));
        return result;
    case ContentType::Audio:
        result.text = !mediaFilename(item).simplified().isEmpty()
            ? mediaFilename(item).simplified()
            : (!body.isEmpty() ? body : QCoreApplication::translate("HistoryWidget", "Audio"));
        return result;
    case ContentType::Service:
        result.text = !body.isEmpty() ? body : QCoreApplication::translate("HistoryWidget", "Service message");
        return result;
    case ContentType::Poll:
        result.text = (pollContent(item) && !pollContent(item)->question.simplified().isEmpty())
            ? pollContent(item)->question.simplified()
            : (!body.isEmpty() ? body : QCoreApplication::translate("HistoryWidget", "Poll"));
        return result;
    case ContentType::Text:
    default:
        result.text = !body.isEmpty() ? body : QCoreApplication::translate("HistoryWidget", "Message");
        return result;
    }
}

[[nodiscard]] ReplyComposePreview resolveComposeReplyPreview(
        const QVector<TimelineItem> &messages,
        const QString &eventId,
        const QString &fallbackText,
        const QString &fallbackPath = QString()) {
    ReplyComposePreview result;
    result.text = fallbackText.simplified();
    result.path = fallbackPath;
    if (!eventId.isEmpty()) {
        const auto it = std::find_if(
            messages.cbegin(),
            messages.cend(),
            [&](const TimelineItem &item) {
                return item.eventId == eventId;
            });
        if (it != messages.cend()) {
            const auto resolved = composeReplyPreviewFor(*it);
            if (!resolved.text.isEmpty()) {
                result.text = resolved.text;
            }
            if (!resolved.path.isEmpty()) {
                result.path = resolved.path;
            }
        }
    }
    if (result.text.isEmpty()) {
        result.text = QCoreApplication::translate("HistoryWidget", "Message");
    }
    return result;
}

void requestResolvedMxcUrl(
        ProtocolBridge *bridge,
        const QString &url,
        bool previewImage = false,
        bool preferBytes = true) {
    const auto cacheKey = previewImage ? MediaCache::previewImageKey(url) : url;
    if (!bridge
        || !url.startsWith(QStringLiteral("mxc://"))
        || !MediaCache::needsResolution(cacheKey)) {
        return;
    }

    MediaCache::markRequested(cacheKey);
    if (previewImage) {
        if (preferBytes) {
            bridge->resolveMediaPreviewImageBytes(url);
        } else {
            bridge->resolveMediaPreviewImage(url);
        }
    } else if (preferBytes) {
        bridge->resolveMediaBytes(url);
    } else {
        bridge->resolveMedia(url);
    }
}

void requestVideoServerThumbnail(
        ProtocolBridge *bridge,
        const TimelineItem &msg) {
    const auto url = mediaUrl(msg);
    if (!bridge
        || !isVideoMessage(msg)
        || !mediaThumbUrl(msg).isEmpty()
        || url.isEmpty()
        || !url.startsWith(QStringLiteral("mxc://"))
        || isLocalSendQueueMxc(url)) {
        return;
    }
    const auto thumbKey = QStringLiteral("srvthumb:") + url;
    if (MediaCache::needsResolution(thumbKey)) {
        MediaCache::markRequested(thumbKey);
        bridge->resolveMediaThumbnailBytes(url);
    }
}

void requestVideoLocalThumbnail(
        ProtocolBridge *bridge,
        const QString &eventId,
        const QString &mxcUrl) {
    if (!bridge || eventId.isEmpty() || mxcUrl.isEmpty()) {
        return;
    }
    const auto thumbKey = QStringLiteral("vidthumb:") + eventId;
    if (MediaCache::needsResolution(thumbKey)) {
        MediaCache::markRequested(thumbKey);
        qDebug() << "[vidthumb] requesting event" << eventId << "mxc" << mxcUrl;
        bridge->getVideoThumbnail(eventId, mxcUrl);
    }
}

void requestMessageMediaForRendering(
        ProtocolBridge *bridge,
        const TimelineItem &msg) {
    // Message bodies stay lazy except for images. Video bodies are resolved
    // only from the explicit play/download click path; their previews use
    // mediaThumbUrl or the independent Matrix thumbnail request below.
    if (isImageMessage(msg)) {
        requestResolvedMxcUrl(bridge, mediaUrl(msg));
    }
    requestResolvedMxcUrl(bridge, mediaThumbUrl(msg));
    if (const auto preview = urlPreviewInfo(msg)) {
        requestResolvedMxcUrl(bridge, preview->imageUrl, true);
    }
    // Sender avatar: a server thumbnail (small), not the full image the shared
    // requestResolvedMxcUrl helper would fetch. Same needsResolution/markRequested
    // guard, keyed by the plain mxc so the avatar paint finds it.
    if (bridge && msg.sender.avatarUrl.startsWith(QStringLiteral("mxc://"))
            && MediaCache::needsResolution(msg.sender.avatarUrl)) {
        MediaCache::markRequested(msg.sender.avatarUrl);
        bridge->resolveAvatar(msg.sender.avatarUrl);
    }
    requestVideoServerThumbnail(bridge, msg);
}

} // namespace

// ─── TopBarIconButton ────────────────────────────────────

/// Icon button for the top bar (search, menu).
class TopBarIconButton : public QWidget {
public:
    enum Icon { Search, Menu, Pinned };

    TopBarIconButton(QWidget *parent, Icon icon)
        : QWidget(parent), _icon(icon)
    {
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_OpaquePaintEvent, false);
    }

    void setClickCallback(std::function<void()> callback) {
        _callback = std::move(callback);
    }
    /// Global position of the bottom-left corner, captured from the last click.
    [[nodiscard]] QPoint lastClickBottomGlobal() const {
        return _lastClickBottomGlobal;
    }

    void setActive(bool active) {
        if (_active != active) {
            _active = active;
            setCursor(Qt::PointingHandCursor);
            update();
        }
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);

        // Highlight only for active state (search), never on hover.
        const auto highlighted = _active;
        if (highlighted) {
            p.setPen(Qt::NoPen);
            p.setBrush(st::windowBgOver);
            p.drawEllipse(0, kTopBarRippleTop, kTopBarRippleSize, kTopBarRippleSize);
        }

        const auto &fg = highlighted
            ? st::menuIconFgOver
            : st::menuIconFg;
        const auto name = (_icon == Search)
            ? QStringLiteral("top_bar_search")
            : (_icon == Pinned)
            ? QStringLiteral("top_bar_pinned")
            : QStringLiteral("menu_dots");
        // Pinned reuses the search icon position (same 32px icon in a 40px button).
        const auto pos = (_icon == Menu)
            ? kTopBarMenuIconPos
            : kTopBarSearchIconPos;
        const auto icon = TeleMatrix::Style::IconProvider::tintedIcon(
            QStringLiteral(":/telematrix/icons/topbar/"), name, fg);
        if (!icon.isNull()) {
            p.drawImage(pos, icon);
        }
    }

    void enterEvent(QEnterEvent *) override {
        _over = true;
        setCursor(Qt::PointingHandCursor);
#ifdef Q_OS_MAC
        Platform::ForcePointingHandCursor();
#endif
        update();
    }
    void leaveEvent(QEvent *) override { _over = false; update(); }
    void mouseReleaseEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton && rect().contains(e->pos()) && _callback) {
            // Capture button bottom-left from the mouse event's global position.
            // QMouseEvent::globalPosition() returns correct screen coordinates
            // on macOS, unlike QWidget::mapToGlobal which is broken.
            const auto globalClick = e->globalPosition().toPoint();
            const auto localY = e->pos().y();
            _lastClickBottomGlobal = QPoint(
                globalClick.x() - e->pos().x(), // left edge of button
                globalClick.y() + (height() - localY) + 10); // bottom edge + gap
            _callback();
            e->accept();
            return;
        }
        QWidget::mouseReleaseEvent(e);
    }

    Icon _icon;
    bool _over = false;
    bool _active = false;
    std::function<void()> _callback;
    QPoint _lastClickBottomGlobal;
};

// ─── HistoryTopBar ───────────────────────────────────────

/// Top bar layout:
/// Room name (bold) + subtitle, search + menu buttons, separator line.
// Top-of-timeline warning bar shown when a previously-verified user in the open
// room has a cross-signing verification violation (identity changed). Offers an
// inline "Re-verify" (outgoing SAS) and a dismiss "x". Reuses paintTrustShield's
// red glyph as the warning icon for consistency with the shields elsewhere.
class HistoryTrustWarningBar : public QWidget {
public:
    explicit HistoryTrustWarningBar(QWidget *parent) : QWidget(parent) {
        setMouseTracking(true);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
    }

    void setUser(const QString &userId, const QString &displayName) {
        _userId = userId;
        _displayName = displayName.trimmed().isEmpty() ? userId : displayName.trimmed();
        update();
    }
    [[nodiscard]] const QString &userId() const { return _userId; }
    [[nodiscard]] const QString &displayName() const { return _displayName; }

    std::function<void()> onReverify;
    std::function<void()> onDismiss;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), st::historyPinnedBg);
        p.fillRect(0, height() - 1, width(), 1, st::shadowFg);

        const int pad = 12;
        const int iconSize = 18;
        TeleMatrix::paintTrustShield(
            p,
            QRect(pad, (height() - iconSize) / 2, iconSize, iconSize),
            TeleMatrix::UserTrustState::Violation);

        const auto dRect = dismissRect();
        p.setPen(_dismissHovered ? st::windowActiveTextFg : st::windowSubTextFg);
        p.setFont(st::normalFont);
        p.drawText(dRect, Qt::AlignCenter, QStringLiteral("✕"));

        const auto rRect = reverifyRect();
        p.setPen(Qt::NoPen);
        p.setBrush(_reverifyHovered ? st::activeButtonBgOver : st::activeButtonBg);
        p.drawRoundedRect(rRect, rRect.height() / 2, rRect.height() / 2);
        p.setPen(st::activeButtonFg);
        p.setFont(st::semiboldFont);
        p.drawText(rRect, Qt::AlignCenter, reverifyLabel());

        const int textLeft = pad + iconSize + 8;
        const int textRight = rRect.left() - 8;
        p.setPen(st::boxTextFgError);
        p.setFont(st::normalFont);
        const auto text = QCoreApplication::translate(
            "HistoryTrustWarningBar", "%1's identity has changed").arg(_displayName);
        const auto elided = QFontMetrics(st::normalFont).elidedText(
            text, Qt::ElideRight, qMax(0, textRight - textLeft));
        p.drawText(
            QRect(textLeft, 0, qMax(0, textRight - textLeft), height()),
            Qt::AlignVCenter | Qt::AlignLeft,
            elided);
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        const bool r = reverifyRect().contains(e->pos());
        const bool d = dismissRect().contains(e->pos());
        if (r != _reverifyHovered || d != _dismissHovered) {
            _reverifyHovered = r;
            _dismissHovered = d;
            update();
        }
    }
    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton) {
            return;
        }
        if (reverifyRect().contains(e->pos())) {
            if (onReverify) {
                onReverify();
            }
        } else if (dismissRect().contains(e->pos())) {
            if (onDismiss) {
                onDismiss();
            }
        }
    }
    void leaveEvent(QEvent *) override {
        if (_reverifyHovered || _dismissHovered) {
            _reverifyHovered = _dismissHovered = false;
            update();
        }
    }

private:
    [[nodiscard]] static QString reverifyLabel() {
        return QCoreApplication::translate("HistoryTrustWarningBar", "Re-verify");
    }
    [[nodiscard]] QRect dismissRect() const {
        const int s = height();
        return QRect(width() - s, 0, s, s);
    }
    [[nodiscard]] QRect reverifyRect() const {
        const int btnH = qMin(height() - 12, 26);
        const int btnW =
            QFontMetrics(st::semiboldFont).horizontalAdvance(reverifyLabel()) + 28;
        const int x = dismissRect().left() - btnW - 6;
        return QRect(x, (height() - btnH) / 2, btnW, btnH);
    }

    QString _userId;
    QString _displayName;
    bool _reverifyHovered = false;
    bool _dismissHovered = false;
};

class HistoryTopBar : public QWidget {
    Q_OBJECT

    enum class StatusIcon {
        Lock,
        Mute,
    };

public:
    explicit HistoryTopBar(QWidget *parent)
        : QWidget(parent)
    {
        setMouseTracking(true);
        setAutoFillBackground(true);
        QPalette pal = palette();
        pal.setColor(QPalette::Window, st::topBarBg);
        setPalette(pal);

        _pinnedBtn = new TopBarIconButton(this, TopBarIconButton::Pinned);
        _pinnedBtn->hide(); // shown only when the room has pinned messages
        _searchBtn = new TopBarIconButton(this, TopBarIconButton::Search);
        _menuBtn = new TopBarIconButton(this, TopBarIconButton::Menu);

        _connectingTimer = new QTimer(this);
        _connectingTimer->setInterval(16); // ~60fps spinner while connecting
        connect(_connectingTimer, &QTimer::timeout, this, [this] { update(); });

        _memberSyncTimer = new QTimer(this);
        _memberSyncTimer->setInterval(33); // ~30fps pulse for the syncing badge
        // Only the status line pulses, but this runs for the whole member load —
        // seconds on a big federated room — so repainting the whole bar 30×/s
        // would re-elide and re-draw the name, subtitle and every icon for a
        // fading one-line label.
        connect(_memberSyncTimer, &QTimer::timeout, this, [this] {
            update(statusLineRect());
        });
    }

    void setRoomInfo(const QString &name, const QString &subtitle) {
        _name = name;
        _subtitle = subtitle;
        update();
    }

    void setEncrypted(bool encrypted) {
        if (_isEncrypted != encrypted) {
            _isEncrypted = encrypted;
            update();
        }
    }

    // Trust of the DM peer (UserTrustState discriminant; 0 = Unverified = no shield).
    void setPeerTrust(int trust) {
        if (_peerTrust != trust) {
            _peerTrust = trust;
            update();
        }
    }

    // Whether the open chat is a 1:1 DM (vs a group room); selects the trust
    // shield tooltip wording ("this user" vs "some users").
    void setDirect(bool direct) { _isDirect = direct; }

    void setMuted(bool muted) {
        if (_isMuted != muted) {
            _isMuted = muted;
            update();
        }
    }

    void setSubtitle(const QString &subtitle) {
        if (_subtitle != subtitle) {
            _subtitle = subtitle;
            update();
        }
    }

    void setMemberSyncing(bool syncing) {
        if (_memberSyncing == syncing) {
            return;
        }
        _memberSyncing = syncing;
        if (syncing) {
            _memberSyncAnimStart = QDateTime::currentMSecsSinceEpoch();
            _memberSyncTimer->start();
        } else {
            _memberSyncTimer->stop();
        }
        update();
    }

    /// The status line under the room name (subtitle / typing / "Syncing members
    /// data"). Spans to the right edge rather than tracking `nameWidth`'s icon
    /// arithmetic: over-repainting a one-line band is harmless, missing part of
    /// it is not.
    [[nodiscard]] QRect statusLineRect() const {
        const auto top = kTopBarHeight
            - kTopBarStatusBottomPadding
            - st::dialogsTextFont->height;
        return QRect(
            kTopBarNameLeft,
            top - 1,
            qMax(0, width() - kTopBarNameLeft),
            st::dialogsTextFont->height + 2);
    }

    void setTypingText(const QString &text, qint64 animStart) {
        _typingText = text;
        _typingAnimStart = animStart;
        update();
    }

    void clearTyping() {
        _typingText.clear();
        _typingAnimStart = 0;
        update();
    }

    void setConnecting(bool connecting) {
        if (_connecting == connecting) {
            return;
        }
        _connecting = connecting;
        if (connecting) {
            _connectingTimer->start();
        } else {
            _connectingTimer->stop();
        }
        update();
    }

    void setDrawSeparator(bool draw) {
        if (_drawSeparator != draw) {
            _drawSeparator = draw;
            update();
        }
    }

    void setSearchCallback(std::function<void()> callback) {
        _onSearch = std::move(callback);
        if (_searchBtn) {
            _searchBtn->setClickCallback(_onSearch);
        }
    }

    void setPinnedCallback(std::function<void()> callback) {
        _onPinned = std::move(callback);
        if (_pinnedBtn) {
            _pinnedBtn->setClickCallback(_onPinned);
        }
    }

    // Show the pinned button only when the room has pinned messages; re-flow the
    // right-side buttons so the name area reclaims the space when it's hidden.
    void setHasPinned(bool hasPinned) {
        if (_hasPinned == hasPinned) {
            return;
        }
        _hasPinned = hasPinned;
        if (!_pinnedSectionMode && !_selectionMode) {
            layoutNormalModeButtons();
        } else {
            _pinnedBtn->hide();
        }
        update();
    }

    // Hide (or restore) the search / menu / pinned buttons — used for the unjoined-room preview,
    // where they'd act on a room the SDK doesn't consider ours.
    void setChromeButtonsVisible(bool visible) {
        if (_showChromeButtons == visible) {
            return;
        }
        _showChromeButtons = visible;
        if (!_pinnedSectionMode && !_selectionMode) {
            layoutNormalModeButtons();
        }
        update();
    }

    // Width reserved by the right-side buttons (used for name width + the
    // clickable-name hit zone). The pinned button only counts when shown.
    [[nodiscard]] int rightButtonsWidth() const {
        if (!_showChromeButtons) {
            return 0;
        }
        return kTopBarMenuWidth + kTopBarSkip + kTopBarSearchWidth
            + (_hasPinned ? (kTopBarSkip + kTopBarPinnedWidth) : 0);
    }

    // Position + show the right-side buttons for the normal (non-pinned-section,
    // non-selection) bar. Order right-to-left: "..." menu, pinned (only when
    // _hasPinned), then search — i.e. the pinned button sits between search and "...".
    void layoutNormalModeButtons() {
        if (!_showChromeButtons) {
            _menuBtn->hide();
            _pinnedBtn->hide();
            _searchBtn->hide();
            return;
        }
        const auto h = height();
        auto x = width();
        x -= kTopBarMenuWidth;
        _menuBtn->setGeometry(x, 0, kTopBarMenuWidth, h);
        _menuBtn->show();
        if (_hasPinned) {
            x -= (kTopBarPinnedWidth + kTopBarSkip);
            _pinnedBtn->setGeometry(x, 0, kTopBarPinnedWidth, h);
            _pinnedBtn->show();
        } else {
            _pinnedBtn->hide();
        }
        x -= (kTopBarSearchWidth + kTopBarSkip);
        _searchBtn->setGeometry(x, 0, kTopBarSearchWidth, h);
        _searchBtn->show();
    }

    void setSearchActive(bool active) {
        if (_searchBtn) {
            _searchBtn->setActive(active);
        }
    }

    void setNameClickCallback(std::function<void()> callback) {
        _onNameClick = std::move(callback);
    }

    /// Saved Messages header: title only, vertically centred like the pinned
    /// bar — no subtitle, no status glyphs, and the name area is inert.
    void setSavedMessagesMode(bool saved) {
        if (_savedMessagesMode == saved) {
            return;
        }
        _savedMessagesMode = saved;
        update();
    }

    void setMenuCallback(std::function<void()> callback) {
        _onMenu = std::move(callback);
        if (_menuBtn) {
            _menuBtn->setClickCallback(_onMenu);
        }
    }

    /// Global position of menu button bottom edge, from last click event.
    [[nodiscard]] QPoint menuButtonBottomGlobal() const {
        if (_menuBtn) {
            return _menuBtn->lastClickBottomGlobal();
        }
        return {};
    }

    /// Highlight the menu button (e.g., while the popup menu is open).
    void setMenuActive(bool active) {
        if (_menuBtn) {
            _menuBtn->setActive(active);
        }
    }

    void setPinnedSectionMode(bool active, std::function<void()> onBack = {}) {
        if (_pinnedSectionMode == active) {
            return;
        }
        _pinnedSectionMode = active;
        _onBack = std::move(onBack);
        _backOver = false;
        if (active) {
            _searchBtn->hide();
            _menuBtn->hide();
            _pinnedBtn->hide();
        } else if (!_selectionMode) {
            layoutNormalModeButtons();
        }
        unsetCursor();
        update();
    }

    void setSelectionMode(bool active) {
        if (_selectionMode == active) {
            return;
        }
        _selectionMode = active;
        _pressedAction = SelectionAction::None;
        _cancelOver = false;
        _forwardOver = false;
        if (active) {
            _searchBtn->hide();
            _menuBtn->hide();
            _pinnedBtn->hide();
        } else {
            layoutNormalModeButtons();
        }
        unsetCursor();
        update();
    }

    void setSelectedCount(int count) {
        count = qMax(0, count);
        if (_selectedCount == count) {
            return;
        }
        _selectedCount = count;
        if (_selectionMode) {
            updateSelectionButtonRects();
            update();
        }
    }

    void setSelectionCallbacks(
            std::function<void()> onCancel,
            std::function<void()> onForward) {
        _onCancel = std::move(onCancel);
        _onForward = std::move(onForward);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);

        if (_pinnedSectionMode) {
            // Back arrow on the left.
            {
                PainterHighQualityEnabler hq(p);
                const auto arrowColor = _backOver
                    ? st::menuIconFgOver
                    : st::menuIconFg;
                p.setPen(QPen(arrowColor, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p.setBrush(Qt::NoBrush);
                const auto cx = _backRect.center().x();
                const auto cy = _backRect.center().y();
                // Left-pointing chevron.
                p.drawLine(QPointF(cx + 3, cy - 7), QPointF(cx - 4, cy));
                p.drawLine(QPointF(cx - 4, cy), QPointF(cx + 3, cy + 7));
            }

            // "Pinned Messages" title.
            const auto titleLeft = _backRect.right() + 2;
            p.setPen(st::dialogsNameFg);
            p.setFont(st::semiboldFont);
            p.drawText(
                titleLeft,
                (height() - st::semiboldFont->height) / 2 + st::semiboldFont->ascent,
                tr("Pinned Messages"));

            p.setPen(QPen(st::shadowFg, 1));
            p.drawLine(0, height() - 1, width(), height() - 1);
            return;
        }

        if (_selectionMode) {
            const auto countStr = QString::number(qMax(1, _selectedCount));
            p.setFont(st::semiboldFont);
            const auto fm = p.fontMetrics();

            // Helper to paint a round button.
            const auto paintButton = [&](
                    const QRect &rect,
                    const QString &label,
                    const QString &numbers,
                    bool over,
                    bool isActive) {
                PainterHighQualityEnabler hq(p);
                const auto bg = isActive
                    ? (over ? st::activeButtonBgOver : st::activeButtonBg)
                    : (over ? st::lightButtonBgOver : st::lightButtonBg);
                p.setPen(Qt::NoPen);
                p.setBrush(bg);
                p.drawRoundedRect(rect, kButtonRadius, kButtonRadius);

                const auto textFg = isActive
                    ? st::activeButtonFg
                    : st::lightButtonFg;
                const auto labelW = fm.horizontalAdvance(label);
                const auto numbersW = numbers.isEmpty()
                    ? 0
                    : fm.horizontalAdvance(numbers);
                const auto totalW = labelW
                    + (numbersW ? kNumbersSkip + numbersW : 0);
                const auto textLeft = rect.x()
                    + (rect.width() - totalW) / 2;
                const auto textY = rect.y() + kButtonTextTop
                    + fm.ascent();

                p.setPen(textFg);
                p.drawText(textLeft, textY, label);
                if (numbersW) {
                    p.setPen(st::activeButtonSecondaryFg);
                    p.drawText(
                        textLeft + labelW + kNumbersSkip,
                        textY,
                        numbers);
                }
            };

            paintButton(
                _forwardRect, tr("Forward"),
                countStr, _forwardOver, true);
            paintButton(
                _cancelRect, tr("Cancel"),
                QString(), _cancelOver, false);

            p.setPen(QPen(st::shadowFg, 1));
            p.drawLine(0, height() - 1, width(), height() - 1);
            return;
        }

        // Right buttons layout: menu + topBarSkip + search.
        const auto rightTaken = rightButtonsWidth();
        const auto nameLeft = kTopBarNameLeft;
        const auto nameWidth = width()
            - rightTaken
            - nameLeft
            - kTopBarNameRightPadding;

        const auto statusIconCount = (_isEncrypted ? 1 : 0)
            + (_isMuted ? 1 : 0)
            + (_peerTrust != 0 ? 1 : 0);
        const auto statusIconsSpace = statusIconCount
            ? (statusIconCount * kTopBarStatusIconSize)
                + (statusIconCount * kTopBarStatusIconSkip)
            : 0;
        const auto titleColor = st::dialogsNameFg;
        p.setPen(titleColor);
        p.setFont(st::semiboldFont);
        const auto &nameEmoji = TeleMatrix::EmojiText::CachedMetricsFor(
            st::semiboldFont, st::emojiInlineSlot, st::emojiInlineGlyph);
        const auto nameElided = TeleMatrix::EmojiText::Elide(
            _name,
            st::semiboldFont,
            nameEmoji,
            qMax(0, nameWidth - statusIconsSpace));
        const auto nameTop = _savedMessagesMode
            ? (kTopBarHeight - st::semiboldFont->height) / 2
            : kTopBarNameTop;
        auto statusIconLeft = nameLeft + TeleMatrix::EmojiText::DrawLine(
            p,
            nameLeft,
            nameTop + st::semiboldFont->ascent,
            nameElided,
            nameEmoji);

        const auto drawStatusIcon = [&](StatusIcon icon) {
            statusIconLeft += kTopBarStatusIconSkip;
            const QRect target(
                statusIconLeft,
                nameTop + (st::semiboldFont->height - kTopBarStatusIconSize) / 2,
                kTopBarStatusIconSize,
                kTopBarStatusIconSize);
            const auto iconName = (icon == StatusIcon::Lock)
                ? QStringLiteral("room_status_lock")
                : QStringLiteral("room_status_mute");
            const auto image = TeleMatrix::Style::IconProvider::tintedIcon(
                QStringLiteral(":/telematrix/icons/topbar/"),
                iconName,
                titleColor);
            if (!image.isNull()) {
                p.drawImage(target, image, image.rect());
            }
            statusIconLeft += kTopBarStatusIconSize;
        };

        if (_isEncrypted) {
            drawStatusIcon(StatusIcon::Lock);
        }
        if (_isMuted) {
            drawStatusIcon(StatusIcon::Mute);
        }
        _shieldTooltips.clear();
        if (_peerTrust != 0) {
            statusIconLeft += kTopBarStatusIconSkip;
            const QRect slot(
                statusIconLeft,
                nameTop + (st::semiboldFont->height - kTopBarStatusIconSize) / 2,
                kTopBarStatusIconSize,
                kTopBarStatusIconSize);
            // Inset a couple px so the filled shield matches the lock/mute glyph
            // weight; keep the semantic colour (green / amber / red).
            const auto state = static_cast<TeleMatrix::UserTrustState>(_peerTrust);
            TeleMatrix::paintTrustShield(p, slot.adjusted(2, 2, -2, -2), state);
            _shieldTooltips.push_back({ slot, state });
            statusIconLeft += kTopBarStatusIconSize;
        }

        // Connecting status (replaces subtitle + typing): "connecting..." with a
        // small radial spinner, mirroring the chat-header connecting line.
        if (_connecting) {
            const auto statusTop = kTopBarHeight
                - kTopBarStatusBottomPadding
                - st::dialogsTextFont->height;
            Ui::DrawConnectingRadial(p,
                QRectF(nameLeft + 2, statusTop + 5, 8, 8),
                1.0,
                st::windowSubTextFg,
                QDateTime::currentMSecsSinceEpoch());
            p.setPen(st::windowSubTextFg);
            p.setFont(st::dialogsTextFont);
            p.drawText(
                nameLeft + 16,
                statusTop + st::dialogsTextFont->ascent,
                tr("connecting..."));
        } else if (!_typingText.isEmpty() && _typingAnimStart > 0) {
            const auto statusTop = kTopBarHeight
                - kTopBarStatusBottomPadding
                - st::dialogsTextFont->height;

            p.setPen(st::windowActiveTextFg);
            p.setFont(st::dialogsTextFont);
            p.drawText(
                nameLeft,
                statusTop + st::dialogsTextFont->ascent,
                _typingText);

            // Bouncing dots.
            const auto textW = QFontMetrics(st::dialogsTextFont)
                .horizontalAdvance(_typingText);
            const auto dotBaseX = nameLeft + textW + 4;
            const auto dotBaseY = statusTop + st::dialogsTextFont->ascent - 4;

            const auto now = QDateTime::currentMSecsSinceEpoch();
            const auto elapsed = now - _typingAnimStart;

            p.setPen(Qt::NoPen);
            p.setBrush(st::windowActiveTextFg);
            p.setRenderHint(QPainter::Antialiasing, true);

            for (int i = 0; i < 3; ++i) {
                auto phase = static_cast<int>((elapsed - i * 150) % 800);
                if (phase < 0) phase += 800;
                qreal r = 16.0 / 12.0; // 1.33px small
                if (phase < 640) {
                    const auto t = qreal(phase % 320) / 320.0;
                    const auto eased = std::sqrt(1.0 - (1.0 - t) * (1.0 - t));
                    if (phase < 320) {
                        r = (16.0 + (28.0 - 16.0) * eased) / 12.0;
                    } else {
                        r = (28.0 - (28.0 - 16.0) * eased) / 12.0;
                    }
                }
                p.drawEllipse(QPointF(dotBaseX + i * 6.0, dotBaseY), r, r);
            }
        } else if (!_savedMessagesMode
                && (_memberSyncing || !_subtitle.isEmpty())) {
            const auto statusTop = kTopBarHeight
                - kTopBarStatusBottomPadding
                - st::dialogsTextFont->height;
            p.setFont(st::dialogsTextFont);
            const auto &fm = QFontMetrics(st::dialogsTextFont);
            if (_memberSyncing) {
                // Member state is still downloading, so the count is unknown/stale — show a
                // pulsing "syncing" status in its place (group rooms only; see the
                // setMemberSyncing wiring). Pulse: opacity ~0.45↔1.0 over 1.2s (triangle wave).
                const auto text = QCoreApplication::translate(
                    "HistoryWidget", "Syncing members data");
                const auto elided = fm.elidedText(text, Qt::ElideRight, nameWidth);
                const auto elapsed = QDateTime::currentMSecsSinceEpoch()
                    - _memberSyncAnimStart;
                const auto phase = qreal(elapsed % 1200) / 1200.0;
                const auto tri = (phase < 0.5) ? (phase * 2.0) : ((1.0 - phase) * 2.0);
                const auto pulse = 0.45 + 0.55 * tri;
                p.setOpacity(pulse);
                p.setPen(st::windowActiveTextFg);
                p.drawText(
                    nameLeft,
                    statusTop + st::dialogsTextFont->ascent,
                    elided);
                p.setOpacity(1.0);
            } else {
                p.setPen(st::dialogsTextFg);
                TeleMatrix::EmojiText::DrawElided(
                    p,
                    nameLeft,
                    statusTop + st::dialogsTextFont->ascent,
                    nameWidth,
                    _subtitle,
                    TeleMatrix::EmojiText::CachedMetricsFor(
                        st::dialogsTextFont,
                        st::emojiInlineSlot,
                        st::emojiInlineGlyph));
            }
        }

        // Bottom separator line (only when pinned bar is below).
        if (_drawSeparator) {
            p.setPen(QPen(st::shadowFg, 1));
            p.drawLine(0, height() - 1, width(), height() - 1);
        }
    }

    void resizeEvent(QResizeEvent *e) override {
        QWidget::resizeEvent(e);
        const auto h = height();
        _backRect = QRect(0, 0, 54, h);

        if (_pinnedSectionMode) {
            _searchBtn->hide();
            _menuBtn->hide();
            _pinnedBtn->hide();
            return;
        }

        if (_selectionMode) {
            updateSelectionButtonRects();
            _searchBtn->hide();
            _menuBtn->hide();
            _pinnedBtn->hide();
            return;
        }

        layoutNormalModeButtons();
    }

    bool event(QEvent *e) override {
        if (e->type() == QEvent::ToolTip
                && !_pinnedSectionMode && !_selectionMode) {
            const auto *help = static_cast<QHelpEvent *>(e);
            for (const auto &[rect, state] : _shieldTooltips) {
                if (rect.contains(help->pos())) {
                    QToolTip::showText(
                        help->globalPos(),
                        TeleMatrix::trustDescription(state, !_isDirect),
                        this,
                        rect);
                    return true;
                }
            }
            QToolTip::hideText();
            return true;
        }
        return QWidget::event(e);
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        if (_pinnedSectionMode) {
            const auto over = _backRect.contains(e->pos());
            if (over != _backOver) {
                _backOver = over;
                setCursor(over ? Qt::PointingHandCursor : Qt::ArrowCursor);
                update();
            }
            e->accept();
            return;
        }
        if (!_selectionMode) {
            // Show hand cursor over the clickable room name area.
            // Don't override cursor in the button zone — child widgets handle it.
            const auto rightTaken = rightButtonsWidth();
            const auto inButtonZone = e->pos().x() >= width() - rightTaken;
            if (!inButtonZone) {
                setCursor(Qt::PointingHandCursor);
            }
            e->accept();
            return;
        }
        updateHoverState(e->pos());
        e->accept();
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (!_pinnedSectionMode && !_selectionMode && e->button() == Qt::LeftButton) {
            const auto rightTaken = rightButtonsWidth();
            if (e->pos().x() < width() - rightTaken) {
                _namePressed = true;
                e->accept();
                return;
            }
        }
        if (_pinnedSectionMode && e->button() == Qt::LeftButton) {
            _backPressed = _backRect.contains(e->pos());
            e->accept();
            return;
        }
        if (!_selectionMode || e->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(e);
            return;
        }
        _pressedAction = hitAction(e->pos());
        e->accept();
    }

    void mouseReleaseEvent(QMouseEvent *e) override {
        if (_namePressed && e->button() == Qt::LeftButton) {
            _namePressed = false;
            const auto rightTaken = rightButtonsWidth();
            if (e->pos().x() < width() - rightTaken && _onNameClick) {
                _onNameClick();
            }
            e->accept();
            return;
        }
        if (_pinnedSectionMode && e->button() == Qt::LeftButton) {
            const auto wasPressed = _backPressed;
            _backPressed = false;
            if (wasPressed && _backRect.contains(e->pos()) && _onBack) {
                _onBack();
            }
            e->accept();
            return;
        }
        if (!_selectionMode || e->button() != Qt::LeftButton) {
            QWidget::mouseReleaseEvent(e);
            return;
        }
        const auto released = hitAction(e->pos());
        const auto pressed = _pressedAction;
        _pressedAction = SelectionAction::None;
        if (pressed == released) {
            switch (released) {
            case SelectionAction::Cancel:
                if (_onCancel) {
                    _onCancel();
                }
                break;
            case SelectionAction::Forward:
                if (_onForward) {
                    _onForward();
                }
                break;
            case SelectionAction::None:
                break;
            }
        }
        e->accept();
    }

    void leaveEvent(QEvent *e) override {
        unsetCursor();
        if (_pinnedSectionMode) {
            _backOver = false;
            update();
        }
        if (_selectionMode) {
            _cancelOver = false;
            _forwardOver = false;
            _pressedAction = SelectionAction::None;
            update();
        }
        QWidget::leaveEvent(e);
    }

private:
    enum class SelectionAction {
        None,
        Cancel,
        Forward,
    };

    SelectionAction hitAction(const QPoint &pos) const {
        if (!_selectionMode) {
            return SelectionAction::None;
        }
        if (_cancelRect.contains(pos)) {
            return SelectionAction::Cancel;
        }
        if (_forwardRect.contains(pos)) {
            return SelectionAction::Forward;
        }
        return SelectionAction::None;
    }

    void updateSelectionButtonRects() {
        const auto fm = QFontMetrics(st::semiboldFont);
        const auto countStr = QString::number(qMax(1, _selectedCount));
        const auto fwdTextW = fm.horizontalAdvance(
            tr("Forward"));
        const auto numW = fm.horizontalAdvance(countStr);
        const auto cancelTextW = fm.horizontalAdvance(
            tr("Cancel"));

        const auto fwdW = fwdTextW + kNumbersSkip + numW
            + kActiveButtonPadding;
        const auto cancelW = cancelTextW + kClearButtonPadding;
        const auto btnTop = (height() - kActiveButtonHeight) / 2;

        _forwardRect = QRect(
            kTopBarActionSkip, btnTop, fwdW, kActiveButtonHeight);
        _cancelRect = QRect(
            width() - kTopBarActionSkip - cancelW,
            btnTop,
            cancelW,
            kActiveButtonHeight);
    }

    void updateHoverState(const QPoint &pos) {
        const auto action = hitAction(pos);
        const auto cancelOver = (action == SelectionAction::Cancel);
        const auto forwardOver = (action == SelectionAction::Forward);
        if (_cancelOver != cancelOver
            || _forwardOver != forwardOver) {
            _cancelOver = cancelOver;
            _forwardOver = forwardOver;
            update();
        }
        setCursor(action == SelectionAction::None
            ? Qt::ArrowCursor
            : Qt::PointingHandCursor);
    }

    TopBarIconButton *_pinnedBtn = nullptr;
    TopBarIconButton *_searchBtn = nullptr;
    TopBarIconButton *_menuBtn = nullptr;
    QString _name;
    QString _subtitle;
    bool _pinnedSectionMode = false;
    bool _hasPinned = false; // room has pinned messages -> show the pinned button
    // Preview of an unjoined room: search / menu / pinned all act on a room we are a member of, so
    // they are hidden entirely (not just disabled). The name stays clickable (room-info popup).
    bool _showChromeButtons = true;
    QRect _backRect;
    bool _backOver = false;
    bool _backPressed = false;
    std::function<void()> _onBack;
    bool _selectionMode = false;
    int _selectedCount = 0;
    QRect _cancelRect;
    QRect _forwardRect;
    bool _cancelOver = false;
    bool _forwardOver = false;
    SelectionAction _pressedAction = SelectionAction::None;
    std::function<void()> _onCancel;
    std::function<void()> _onForward;
    std::function<void()> _onSearch;
    std::function<void()> _onPinned;
    std::function<void()> _onMenu;
    std::function<void()> _onNameClick;
    bool _savedMessagesMode = false;
    bool _namePressed = false;
    bool _drawSeparator = false;
    bool _isEncrypted = false;
    int _peerTrust = 0; // UserTrustState of the DM peer; 0 = Unverified = no shield
    bool _isDirect = false; // 1:1 DM vs group room, for shield tooltip wording
    // Trust shield hit-targets (slot rect + state) recorded each paint, for the
    // hover tooltip in event().
    std::vector<std::pair<QRect, TeleMatrix::UserTrustState>> _shieldTooltips;
    bool _isMuted = false;
    QString _typingText;
    qint64 _typingAnimStart = 0;
    bool _connecting = false;
    QTimer *_connectingTimer = nullptr;
    bool _memberSyncing = false;
    qint64 _memberSyncAnimStart = 0;
    QTimer *_memberSyncTimer = nullptr;
};

// ─── NoChatPlaceholder ───────────────────────────────────

/// Centered pill-shaped "Select a chat to start messaging" label.
/// Uses msgService* styles with 4-color gradient background.
class NoChatPlaceholder : public QWidget {
public:
    explicit NoChatPlaceholder(QWidget *parent) : QWidget(parent) {
        setAttribute(Qt::WA_OpaquePaintEvent, true);

        // Re-tile the wallpaper once a resize settles; paintEvent stretches the
        // stale composite in the meantime.
        _bgRebuildTimer.setSingleShot(true);
        _bgRebuildTimer.setInterval(150);
        connect(&_bgRebuildTimer, &QTimer::timeout, this, [this] {
            _bgCache.rebuild(size(), devicePixelRatioF());
            update();
        });
    }

    void setSyncing(bool syncing) {
        if (_syncing != syncing) {
            _syncing = syncing;
            update();
        }
    }

    void invalidateBackground() {
        _bgCache.setSource(QPixmap(), Theme::DefaultCornerColors());
        update();
    }

    void setBackgroundDoodlesEnabled(bool enabled) {
        _bgCache.setDoodlesEnabled(enabled);
        update();
    }

protected:
    void paintEvent(QPaintEvent *e) override {
        QPainter p(this);

        // Chat background: gradient with the soft-light doodle tiled over it.
        {
            const auto area = size();
            const auto dpr = devicePixelRatioF();
            if (_bgCache.isNull()) {
                _bgCache.rebuild(area, dpr);
            } else if (!_bgCache.matches(area, dpr)) {
                _bgRebuildTimer.start();
            }
            if (_bgCache.isNull()) {
                p.fillRect(e->rect(), st::historyBg);
            } else if (_bgCache.matches(area, dpr)) {
                // deviceIndependentSize() == area, so this lands 1:1; the paint
                // event's clip takes care of the dirty region.
                p.drawPixmap(0, 0, _bgCache.pixmap());
            } else {
                // Mid-resize: stretch the stale composite until the timer fires.
                p.drawPixmap(QRect(QPoint(), area), _bgCache.pixmap());
            }
        }

        PainterHighQualityEnabler hq(p);

        // Painted with msgServiceFont, msgServiceBg pill, msgServiceFg text.
        const auto text = _syncing
            ? QCoreApplication::translate("TeleMatrix::NoChatPlaceholder", "Waiting for network...")
            : QCoreApplication::translate("TeleMatrix::NoChatPlaceholder", "Select a chat to start messaging");
        const auto f = static_cast<const QFont &>(st::msgServiceFont);
        const QFontMetrics fm(f);

        // Pill dimensions:
        //   w = font.width(text) + msgPadding.left() + msgPadding.right()
        //   h = font.height + msgServicePadding.top() + msgServicePadding.bottom()
        const auto pillW = fm.horizontalAdvance(text)
            + st::msgPadding.left()
            + st::msgPadding.right();
        const auto pillH = fm.height()
            + st::msgServicePadding.top()
            + st::msgServicePadding.bottom();
        const auto pillR = pillH / 2.0;
        const auto pillX = (width() - pillW) / 2;
        const auto pillY = (height() - pillH) / 2;

        // Service message pill.
        p.setPen(Qt::NoPen);
        p.setBrush(st::msgServiceBg);
        p.drawRoundedRect(pillX, pillY, pillW, pillH, pillR, pillR);

        // Text (msgServiceFg = windowFgActive = white).
        p.setFont(f);
        p.setPen(st::msgServiceFg);
        p.drawText(
            pillX + st::msgPadding.left(),
            pillY + st::msgServicePadding.top() + fm.ascent(),
            text);
    }

private:
    bool _syncing = false;
    Theme::ChatBackgroundCache _bgCache;
    QTimer _bgRebuildTimer;         // 150ms debounce on resize
};

class TopBarShadowWidget : public Ui::RpWidget {
public:
    explicit TopBarShadowWidget(QWidget *parent)
        : Ui::RpWidget(parent) {
        resize(1, 1);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
    }

protected:
    void paintEvent(QPaintEvent *e) override {
        QPainter p(this);
        // Erase first — see DialogsTopBarShadow: a bare semi-transparent fill
        // on a widget Qt does not auto-clear darkens on every repaint.
        p.fillRect(e->rect(), st::windowBg);
        p.fillRect(e->rect(), st::toolbarSeparatorFg);
    }
};

// ─── HistoryWidget ───────────────────────────────────────

namespace {
// Scroll-to-bottom button position.
constexpr int kDownButtonRight = 12;
constexpr int kDownButtonBottom = 10;
constexpr int kDownButtonDuration = 150; // show/hide animation duration
// Show the scroll-to-bottom button when scrolled this far from bottom.
constexpr int kScrollThreshold = 480;
constexpr int kInputSlideDuration = 200;
constexpr int kDraftDebounceMs = 1000;
constexpr int kDraftDebounceMaxMs = 5000;
} // namespace

void HistoryWidget::initTopBarPxValues() {
    applyTopBarScale();
}

HistoryWidget::HistoryWidget(
    AppController *controller,
    QWidget *parent,
    ProtocolBridge *bridge)
    : Ui::RpWidget(parent)
    , _controller(controller)
    , _bridge(bridge)
    , _unreadStateStore(controller ? controller->unreadStateStore() : nullptr)
{
    clearUploadVideoThumbTempDir();
    clearUploadCompressedImageTempDir();

    // Ensure white background even in macOS dark mode.
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, st::historyBg);
    setPalette(pal);
    setAcceptDrops(true);

    setupTopBar();
    setupMessageList();
    setupPinnedBar();
    setupInput();
    setupCornerButtons();
    setupPlaceholder();

    // Invite panel (shown when an invited room is selected).
    _invitePanel = new QWidget(this);
    _invitePanel->hide();

    // Listen for large emoji setting changes and propagate to the list.
    connect(_controller, &AppController::largeEmojiChanged,
            this, [this](bool enabled) {
        if (_list) {
            _list->setLargeEmojiEnabled(enabled);
        }
    });

    // "Display background doodles": every widget that composites the wallpaper.
    connect(_controller, &AppController::backgroundDoodlesChanged,
            this, [this](bool enabled) {
        if (_list) {
            _list->setBackgroundDoodlesEnabled(enabled);
        }
        if (_pinnedList) {
            _pinnedList->setBackgroundDoodlesEnabled(enabled);
        }
        if (_noChat) {
            _noChat->setBackgroundDoodlesEnabled(enabled);
        }
    });

    // Listen for hover reply / reaction button setting changes.
    connect(_controller, &AppController::replyButtonOnMessagesChanged,
            this, [this](bool enabled) {
        if (_list) {
            _list->setReplyButtonEnabled(enabled);
        }
    });
    connect(_controller, &AppController::reactionButtonOnMessagesChanged,
            this, [this](bool enabled) {
        if (_list) {
            _list->setReactionButtonEnabled(enabled);
        }
    });
    _hideSystemMessages = _controller->settings().hideSystemMessagesInPublicRooms();
    connect(_controller, &AppController::hideSystemMessagesInPublicRoomsChanged,
            this, [this](bool enabled) {
        _hideSystemMessages = enabled;
        // Only public rooms are affected; rebuild to add or remove their system messages.
        if (_currentRoomIsPublic) {
            refilterCurrentTimeline();
        }
    });
    if (_bridge) {
        // The saved room's id can be learned while that room is already open
        // (it was opened before the async ensure finished). The open path
        // then saw a plain room, so re-apply the whole saved presentation —
        // top bar, list affordances — and refilter the timeline.
        connect(_bridge, &ProtocolBridge::savedMessagesRoomChanged,
                this, [this](const QString &roomId) {
            if (roomId.isEmpty() || roomId != _currentRoomId) {
                return;
            }
            if (_topBar) {
                _topBar->setSavedMessagesMode(true);
                _cachedSubtitle.clear();
                _topBar->setRoomInfo(SavedMessages::displayName(), QString());
                _topBar->setMuted(false);
            }
            if (_list) {
                _list->setSavedMessagesMode(true);
            }
            refilterCurrentTimeline();
        });
    }
    if (_unreadStateStore) {
        connect(_unreadStateStore, &UnreadStateStore::activeRoomUnreadStateChanged,
            this, [this](const QString &roomId, int unreadCount, const QString &firstUnreadEventId) {
                if (_currentRoomId.isEmpty() || roomId != _currentRoomId) {
                    return;
                }
                auto state = _unreadStateStore->roomState(roomId);
                state.roomId = roomId;
                state.effectiveUnreadCount = unreadCount;
                state.effectiveFirstUnreadEventId = firstUnreadEventId;
                applyStoreUnreadState(state);
            });
    }

    _draftChangedTimer = new QTimer(this);
    _draftChangedTimer->setSingleShot(true);
    _draftChangedTimer->setInterval(kDraftDebounceMs);
    connect(_draftChangedTimer, &QTimer::timeout, this, [this] {
        flushDraftChanged();
    });

    _lastSeenTimer = new QTimer(this);
    _lastSeenTimer->setInterval(30000); // 30 seconds
    connect(_lastSeenTimer, &QTimer::timeout, this, [this] {
        refreshLastSeenSubtitle();
    });

    _mediaRecheckTimer = new QTimer(this);
    _mediaRecheckTimer->setSingleShot(true);
    _mediaRecheckTimer->setInterval(500);
    connect(_mediaRecheckTimer, &QTimer::timeout,
            this, &HistoryWidget::recheckUnresolvedMedia);

    // Coalesces a burst of media resolutions into one relayout: each resolve
    // defers its invalidation and (re)arms this; the flush does a single batched
    // invalidateLayoutForMedia + one scroll-anchor correction.
    _mediaInvalidationTimer = new QTimer(this);
    _mediaInvalidationTimer->setSingleShot(true);
    _mediaInvalidationTimer->setInterval(50);
    connect(_mediaInvalidationTimer, &QTimer::timeout,
            this, &HistoryWidget::applyDeferredMediaUpdates);

    // When a paint-time media load dead-ends (evicted, no retained bytes, no
    // disk path), the cache asks us to re-resolve. Arm the throttled recheck —
    // recheckUnresolvedMedia re-requests only what still needsResolution, so
    // this is idempotent and cannot loop. Cleared in the destructor.
    MediaCache::setMediaRecheckHook(this, [this] {
        if (_mediaRecheckTimer && !_mediaRecheckTimer->isActive()) {
            _mediaRecheckTimer->start();
        }
    });

    _typingSendTimer = new QTimer(this);
    _typingSendTimer->setSingleShot(true);
    _typingSendTimer->setInterval(5000);
    connect(_typingSendTimer, &QTimer::timeout, this, [this] {
        _typingState.setOutgoingSent(false);
    });

    _typingCancelTimer = new QTimer(this);
    _typingCancelTimer->setSingleShot(true);
    _typingCancelTimer->setInterval(5000);
    connect(_typingCancelTimer, &QTimer::timeout, this, [this] {
        if (!_currentRoomId.isEmpty()) {
            _bridge->sendTypingNotice(_currentRoomId, false);
            _typingState.setOutgoingSent(false);
        }
    });

    _typingDotTimer = new QTimer(this);
    _typingDotTimer->setInterval(16);
    connect(_typingDotTimer, &QTimer::timeout, this, [this] {
        if (_topBar && _typingState.hasIncomingUsers()) {
            _topBar->update();
        }
    });

    // Real-time presence updates for direct chats.
    connect(_bridge, &ProtocolBridge::presenceChanged,
        this, [this](const QString &userId, int state, qint64 lastActiveTs) {
            if (!_currentRoomIsDirect || userId != _directChatUserId) {
                return;
            }
            if (state == 1) { // online
                _cachedSubtitle = tr("online");
                _topBar->setSubtitle(_cachedSubtitle);
                _lastSeenTimer->stop();
            } else {
                if (lastActiveTs > 0) {
                    _lastSeenTimestamp = lastActiveTs;
                }
                refreshLastSeenSubtitle();
                if (_lastSeenTimestamp > 0) {
                    _lastSeenTimer->start();
                }
            }
        });

    // DM peer cross-signing trust -> shield in the top bar.
    const auto applyPeerTrust = [this](const QString &userId, int state) {
        if (!_currentRoomIsDirect || userId != _directChatUserId) {
            return;
        }
        if (_topBar) {
            _topBar->setPeerTrust(state);
        }
    };
    connect(_bridge, &ProtocolBridge::userTrustStateResult, this, applyPeerTrust);
    connect(_bridge, &ProtocolBridge::userTrustChanged, this, applyPeerTrust);

    // Active identity-changed warning bar for the open room: shown when the DM
    // peer OR any current timeline sender enters a verification violation, and
    // hidden when the shown user's violation resolves.
    const auto handleTrustViolation = [this](const QString &userId, int state) {
        constexpr int kViolation = 2;
        if (state == kViolation) {
            const bool relevant =
                (_currentRoomIsDirect && userId == _directChatUserId)
                || (_list && _list->hasSender(userId));
            if (relevant) {
                showTrustWarning(userId);
            }
        } else if (_trustWarningActive && userId == _trustWarningUserId) {
            hideTrustWarning();
        }
    };
    connect(_bridge, &ProtocolBridge::userTrustChanged, this, handleTrustViolation);
    connect(_bridge, &ProtocolBridge::userTrustStateResult, this, handleTrustViolation);

    // Incoming typing indicator for current room.
    connect(_bridge, &ProtocolBridge::typingChanged,
        this, [this](const QString &roomId, const QStringList &userIds) {
            if (roomId != _currentRoomId) return;
            if (_typingState.setIncomingUsers(
                    userIds,
                    QDateTime::currentMSecsSinceEpoch())) {
                _typingDotTimer->start();
                refreshTypingSubtitle();
            } else {
                _typingDotTimer->stop();
                if (_topBar) {
                    _topBar->clearTyping();
                }
                // Restore normal subtitle.
                if (_currentRoomIsDirect) {
                    if (_cachedSubtitle.isEmpty()) {
                        refreshLastSeenSubtitle();
                    } else {
                        _topBar->setSubtitle(_cachedSubtitle);
                    }
                } else {
                    _topBar->setSubtitle(_cachedSubtitle);
                }
            }
        });

    // Member-fetch progress for the current room → pulsing top-bar badge.
    // Only group rooms carry a member counter; a DM's brief member fetch would
    // put an odd "syncing members" next to its "last seen" subtitle.
    connect(_bridge, &ProtocolBridge::memberSyncStateChanged,
        this, [this](const QString &roomId, bool inProgress) {
            if (roomId != _currentRoomId || _previewMode) return;
            if (_topBar) {
                _topBar->setMemberSyncing(
                    inProgress && !_currentRoomIsDirect && !isSavedMessagesRoom());
            }
        });

    // Start with no chat selected.
    showChatControls(false);

    // Debounce rapid timeline changes (batch reactions, typing, etc.)
    // to avoid redundant getTimelineSlice + setSlice cycles.
    _timelineDebounce = new QTimer(this);
    _timelineDebounce->setSingleShot(true);
    _timelineDebounce->setInterval(50);

    // Safety timeout for pagination flags — prevents permanent lock if
    // the Rust bridge crashes or network hangs and no slice arrives.
    _paginationTimeoutTimer = new QTimer(this);
    _paginationTimeoutTimer->setSingleShot(true);
    _paginationTimeoutTimer->setInterval(10000); // 10 seconds
    connect(_paginationTimeoutTimer, &QTimer::timeout, this, [this] {
        if (!_isPaginatingBack && !_isPaginatingForward) {
            return;
        }
        const auto wasBack = _isPaginatingBack;
        const auto wasFwd = _isPaginatingForward;

        if (_paginationRetryCount < kMaxPaginationRetries
                && !_currentRoomId.isEmpty()) {
            // Retry with exponential backoff: 10s, 20s, 40s.
            ++_paginationRetryCount;
            const auto nextTimeout = 10000 * (1 << _paginationRetryCount);
            qWarning() << "[PAGINATE] timeout — retry" << _paginationRetryCount
                << "of" << kMaxPaginationRetries
                << "(next timeout:" << nextTimeout << "ms)"
                << "back=" << wasBack << "fwd=" << wasFwd;

            if (wasBack) {
                _bridge->paginateBack(_currentRoomId);
            }
            if (wasFwd) {
                _bridge->paginateForward(_currentRoomId);
            }
            _paginationTimeoutTimer->setInterval(nextTimeout);
            _paginationTimeoutTimer->start();
        } else {
            // Max retries exhausted — give up and reset.
            qWarning() << "[PAGINATE] timeout — giving up after"
                << _paginationRetryCount << "retries"
                << "back=" << wasBack << "fwd=" << wasFwd;
            _isPaginatingBack = false;
            _isPaginatingForward = false;
            _paginationRetryCount = 0;
            _paginationTimeoutTimer->setInterval(10000);
            if (_list) {
                _list->setLoadingTimeline(false);
            }
            if (!_currentRoomId.isEmpty()) {
                onTimelineChanged(_currentRoomId);
            }
        }
    });
    connect(_timelineDebounce, &QTimer::timeout, this, [this] {
        onTimelineChanged(_currentRoomId);
    });

    // Subscribe to timeline changes.
    QObject::connect(_bridge, &ProtocolBridge::timelineChanged,
        this, [this](const QString &roomId) {
        if (roomId != _currentRoomId) {
            return;
        }
        // Bypass debounce when immediate delivery is needed:
        // - jump pending, initial scroll, or pagination in flight;
        // - a media upload is in flight (so the upload bubble + progress show
        //   without the 50ms delay).
        if ((_pendingJump && _pendingJump->roomId == roomId) || _initialScrollPending
            || _isPaginatingBack || _isPaginatingForward
            || _pendingLocalMedia.hasPendingForRoom(roomId)) {
            onTimelineChanged(roomId);
        } else {
            _timelineDebounce->start(); // restart 50ms timer
        }
    });
    QObject::connect(_bridge, &ProtocolBridge::timelineSliceReady,
        this, [this](
            const QString &roomId,
            quint64 requestId,
            bool success,
            const TimelineSlice &slice) {
        if (requestId != _latestTimelineSliceRequestId
            || !success
            || roomId != _currentRoomId) {
            return;
        }
        applyTimelineSlice(roomId, slice);
    });

    // focusOnEvent completion — clear the loading state on failure; on success
    // the subsequent timelineChanged signal delivers the focused slice and
    // onTimelineChanged completes the pending jump.
    connect(_bridge, &ProtocolBridge::focusOnEventResult,
        this, [this](const QString &roomId, quint64 requestId, bool success) {
            if (roomId != _currentRoomId) {
                return;
            }
            if (requestId == 0) {
                if (!success) {
                    _list->setLoadingTimeline(false);
                    qWarning() << "HistoryWidget: focusOnEvent failed for room" << roomId;
                }
                return;
            }
            if (!_pendingJump || _pendingJump->roomId != roomId
                    || _pendingJump->requestId != requestId) {
                return;
            }
            if (!success) {
                const auto failedEventId = _pendingJump->eventId;
                _pendingJump.reset();
                _pendingJumpPreferLive = false;
                if (_jumpLoad.active()
                    && _jumpLoad.onFetchFailed()
                        == JumpLoadController::Action::Fallback) {
                    finishJumpFallback();
                } else {
                    _list->setLoadingTimeline(false);
                    cancelPinnedJump(failedEventId);
                }
                qWarning() << "HistoryWidget: focusOnEvent failed for room"
                    << roomId << "request" << requestId;
            } else {
                schedulePendingJumpVisibilityTimeout(
                    roomId,
                    _pendingJump->eventId,
                    requestId);
            }
            // Success case: timelineChanged fires next; onTimelineChanged
            // completes the jump once the message is in the slice.
        });

    // Room members loaded asynchronously — populate avatar cache and
    // @mention autocomplete, then re-enrich visible messages.
    connect(_bridge, &ProtocolBridge::roomMembersReady,
        this, [this](const QString &roomId, const QVector<UserProfile> &members) {
            if (roomId != _currentRoomId) return;
            _memberAvatarCache.clear();
            if (_input) _input->setRoomMembers(members);
            // Cache each member's avatar URL (a cheap string map used to enrich
            // messages and the @mention list) but do NOT prefetch the images.
            // A big federated room has thousands of members; fetching every
            // avatar here floods the download lane with thumbnail GETs — most
            // 404 for expired federated media — pegging the CPU. Avatars load
            // lazily where they are actually shown: visible timeline rows (via
            // the viewport-gated requestMessageMediaForRendering), the
            // virtualized member list, and the mention popup.
            for (const auto &m : members) {
                if (!m.userId.isEmpty() && !m.avatarUrl.isEmpty()) {
                    _memberAvatarCache.insert(m.userId, m.avatarUrl);
                }
            }
            // Re-enrich visible messages with freshly loaded avatars (keeps
            // both message stores in sync; repaints internally).
            if (_list) {
                _list->enrichMessages([this](TimelineItem &msg) {
                    applySenderAvatarFallback(
                        msg, _memberAvatarCache, _controller);
                });
            }
        });
    connect(_bridge, &ProtocolBridge::roomSettingsReady,
        this, [this](bool success, const RoomSettingsSnapshot &snapshot) {
            if (snapshot.roomId != _currentRoomId) {
                return;
            }
            if (!success) {
                resetCurrentRoomPermissions();
                return;
            }
            _currentRoomPermissionsLoaded = true;
            _currentRoomCanInvite = snapshot.canInvite;
            _currentRoomCanKick = snapshot.canKick;
            _currentRoomCanBan = snapshot.canBan;
            _currentNotificationMode = snapshot.notificationMode;
            // Public-ness is normally seeded from the rooms list at open time (loadRoomData), so this
            // authoritative snapshot usually just confirms it — no rebuild, no flicker. Only when it
            // CORRECTS a stale cached value do we rebuild so the filter matches. Ignore an Unknown
            // access (join rule not yet in local state): it isn't authoritative and would otherwise
            // un-seed a correct value and blink the messages back in.
            if (snapshot.access != RoomAccess::Unknown) {
                const bool wasPublic = _currentRoomIsPublic;
                _currentRoomIsPublic = (snapshot.access == RoomAccess::Public);
                if (wasPublic != _currentRoomIsPublic && !_currentRoomId.isEmpty()) {
                    refilterCurrentTimeline();
                }
            }
            _currentRoomIsEncrypted = snapshot.isEncrypted;
            if (_topBar) {
                // Saved Messages shows the lock (E2EE matters there) but
                // never the mute glyph — it is muted by design.
                _topBar->setEncrypted(snapshot.isEncrypted);
                _topBar->setMuted(!isSavedMessagesRoom() && isToolbarMuted(
                    snapshot.notificationMode,
                    snapshot.isMuted));
            }
        });

    // When mxc:// media resolves, update the cache and repaint.
    // After cache clear, re-request all mxc:// URLs for visible messages.
    QObject::connect(_bridge, &ProtocolBridge::cacheClearResult,
        this, [this](bool /*success*/, quint64 /*freedBytes*/) {
            if (!_list || _currentRoomId.isEmpty()) return;
            // Clear C++ caches and re-request resolution for on-screen media;
            // off-screen rows reload lazily via the paint-armed recheck.
            MediaCache::clearAll();
            _list->clearProbedState();
            for (const auto &msg : _list->messages()) {
                if (isMessageNearViewport(msg.eventId)) {
                    requestMessageMediaForRendering(_bridge, msg);
                }
            }
            if (_list) {
                _list->update();
            }
        });

    QObject::connect(_bridge, &ProtocolBridge::mediaDownloadProgress,
        this, [this](const QString &mxcUrl, quint64 receivedBytes, quint64 totalBytes, uint phase) {
            if (_mediaRequests.isCancelled(mxcUrl)
                || !MediaCache::isRequested(mxcUrl)) {
                _mediaRequests.removeDeferredProgress(mxcUrl);
                return;
            }
            if (!_windowActive) {
                _mediaRequests.deferProgress(mxcUrl, receivedBytes, totalBytes, phase);
                return;
            }
            MediaCache::DownloadState state;
            state.phase = (phase == 1)
                ? MediaCache::DownloadPhase::Decrypting
                : MediaCache::DownloadPhase::Downloading;
            state.receivedBytes = receivedBytes;
            state.totalBytes = totalBytes;
            MediaCache::setDownloadState(mxcUrl, state);
            updateMessageLists();
        });

    QObject::connect(_bridge, &ProtocolBridge::mediaResolved,
        this, [this](bool success, const QString &mxcUrl, const QString &localPath,
                     bool terminal) {
            _mediaRequests.removeDeferredProgress(mxcUrl);
            if (_mediaRequests.consumeCancelled(mxcUrl)) {
                _mediaRequests.clearPendingOpenRequests(mxcUrl);
                MediaCache::clearRequested(mxcUrl);
                MediaCache::clearDownloadState(mxcUrl);
                if (_windowActive) {
                    updateMessageLists();
                } else {
                    _mediaRequests.deferUpdate();
                }
                return;
            }
            if (success && !localPath.isEmpty()
                    && QFileInfo::exists(localPath)) {
                MediaCache::insertPath(mxcUrl, localPath);
                // Check for pending file open (user clicked before download finished).
                if (_mediaRequests.takePendingFileOpen(mxcUrl)) {
                    if (_list) {
                        for (const auto &msg : _list->messages()) {
                            if (mediaUrl(msg) == mxcUrl) {
                                openResolvedFile(mxcUrl, mediaFilename(msg), mediaMime(msg));
                                break;
                            }
                        }
                    }
                }
                // Check for pending audio play.
                if (_mediaRequests.hasPendingAudioPlay(mxcUrl)) {
                    const auto audioEventId = _mediaRequests.takePendingAudioPlay(mxcUrl);
                    const auto resolved = MediaCache::localPath(mxcUrl);
                    auto *target = listForEvent(audioEventId);
                    if (target && !resolved.isEmpty()) {
                        target->playAudio(audioEventId, resolved);
                    }
                }
                if (_mediaRequests.hasPendingVideoOpen(mxcUrl)) {
                    const auto videoEventId = _mediaRequests.takePendingVideoOpen(mxcUrl);
                    openMediaViewAtEvent(videoEventId);
                }
                // Coalesce a media-resolution burst into a single relayout:
                // defer the invalidation and (when active) flush it ~50ms later
                // via the same batched applyDeferredMediaUpdates path the
                // inactive-window case uses.
                _mediaRequests.deferInvalidation(mxcUrl);
                if (_windowActive
                    && _mediaInvalidationTimer
                    && !_mediaInvalidationTimer->isActive()) {
                    _mediaInvalidationTimer->start();
                }
                return;
            }
            // Terminal failure of a file-variant resolution (also the end of the
            // bytes->file fallback). Record it so needsResolution() backs this URL
            // off — otherwise clearRequested + the recheck timer below would
            // re-request it on a ~500ms loop forever for permanently-dead media.
            // A `terminal` (HTTP 4xx) failure never recovers, so suppress retries
            // for good instead of backing off; anything else keeps the backoff.
            if (terminal) {
                MediaCache::noteResolvePermanentlyFailed(mxcUrl);
            } else {
                MediaCache::noteResolveFailed(mxcUrl);
            }
            MediaCache::clearRequested(mxcUrl);
            MediaCache::clearDownloadState(mxcUrl);
            _mediaRequests.clearPendingVideoOpen(mxcUrl);
            if (!_windowActive) {
                _mediaRequests.deferUpdate();
                return;
            }
            updateMessageLists();
            // Schedule a re-check so stale-path cleanup in loadImage can trigger
            // re-resolution without waiting for next sync. Idempotent now: the
            // backoff above means the recheck re-requests only URLs whose window
            // has elapsed, not the one that just failed.
            if (_mediaRecheckTimer && !_mediaRecheckTimer->isActive()) {
                _mediaRecheckTimer->start();
            }
        });

    QObject::connect(_bridge, &ProtocolBridge::mediaBytesResolved,
        this, [this](bool success,
                     const QString &mxcUrl,
                     const QByteArray &bytes,
                     const QString &mime,
                     const QString &filename,
                     bool terminal) {
            _mediaRequests.removeDeferredProgress(mxcUrl);
            if (_mediaRequests.consumeCancelled(mxcUrl)) {
                _mediaRequests.clearPendingPlaybackRequests(mxcUrl);
                MediaCache::clearRequested(mxcUrl);
                MediaCache::clearDownloadState(mxcUrl);
                if (_windowActive) {
                    updateMessageLists();
                } else {
                    _mediaRequests.deferUpdate();
                }
                return;
            }

            // Locally-extracted video thumbnail (FFmpeg via Rust FFI). The
            // payload is a JPEG keyed by "vidthumb:<eventId>"; decode + cache it
            // and repaint the timeline / pinned section so the bubble fills in.
            if (mxcUrl.startsWith(QStringLiteral("vidthumb:"))) {
                MediaCache::clearRequested(mxcUrl);
                MediaCache::clearDownloadState(mxcUrl);
                if (success && !bytes.isEmpty()) {
                    const auto image = QImage::fromData(bytes, "JPEG");
                    if (!image.isNull()) {
                        MediaCache::insertImage(mxcUrl, image);
                        qDebug() << "[vidthumb] received" << mxcUrl << "decoded"
                                 << image.width() << "x" << image.height()
                                 << "(" << bytes.size() << "bytes)";
                    } else {
                        // Frame extraction yielded undecodable JPEG — back off so
                        // the probe queue stops re-requesting it every recheck.
                        MediaCache::noteResolveFailed(mxcUrl);
                        qWarning() << "[vidthumb] JPEG decode failed for" << mxcUrl
                                   << "(" << bytes.size() << "bytes)";
                    }
                } else if (terminal) {
                    // The underlying video is gone (404), so no frame will ever be
                    // extractable — suppress for good instead of letting the probe
                    // queue re-request it on the backoff forever.
                    MediaCache::noteResolvePermanentlyFailed(mxcUrl);
                    qWarning() << "[vidthumb] retrieval permanently failed for" << mxcUrl;
                } else {
                    MediaCache::noteResolveFailed(mxcUrl);
                    qWarning() << "[vidthumb] retrieval failed for" << mxcUrl;
                }
                if (_windowActive) {
                    if (_list) {
                        _list->update();
                    }
                    if (_pinnedSectionVisible && _pinnedList) {
                        _pinnedList->update();
                    }
                } else {
                    _mediaRequests.deferUpdate();
                }
                return;
            }

            if (success && !bytes.isEmpty()) {
                auto resolvedMime = mime;
                auto resolvedFilename = filename;
                if (_list && (resolvedMime.isEmpty() || resolvedFilename.isEmpty())) {
                    for (const auto &msg : _list->messages()) {
                        const auto preview = urlPreviewInfo(msg);
                        if (mediaUrl(msg) == mxcUrl
                            || mediaThumbUrl(msg) == mxcUrl
                            || msg.sender.avatarUrl == mxcUrl
                            || (preview && preview->imageUrl == mxcUrl)) {
                            if (resolvedMime.isEmpty()) {
                                resolvedMime = mediaMime(msg);
                            }
                            if (resolvedFilename.isEmpty()) {
                                resolvedFilename = mediaFilename(msg);
                            }
                            break;
                        }
                    }
                }

                const auto pendingAudio = _mediaRequests.hasPendingAudioPlay(mxcUrl);
                const auto imageDecoded = MediaCache::insertImageBytes(
                    mxcUrl,
                    bytes,
                    resolvedMime);
                if (!imageDecoded && pendingAudio) {
                    MediaCache::insertBytes(
                        mxcUrl,
                        bytes,
                        resolvedMime,
                        resolvedFilename);
                } else if (!imageDecoded) {
                    // Bytes arrived but weren't a decodable image. Count the decode
                    // failure so paint stops glowing after the budget.
                    MediaCache::noteDecodeFailed(mxcUrl);
                    MediaCache::clearRequested(mxcUrl);
                    MediaCache::clearDownloadState(mxcUrl);
                    // Only a plain mxc:// key has a meaningful file-variant
                    // fallback. A prefixed thumbnail key (previewthumb:/srvthumb:)
                    // resolves the SAME bytes as a file, so re-requesting it just
                    // loops — and the old code passed the prefixed key to
                    // resolveMedia(), which Rust rejects ("Not an mxc URL"),
                    // guaranteeing failure.
                    if (mxcUrl.startsWith(QStringLiteral("mxc://"))
                            && MediaCache::needsResolution(mxcUrl)) {
                        MediaCache::markRequested(mxcUrl);
                        _bridge->resolveMedia(mxcUrl);
                    } else if (_windowActive && _list) {
                        _list->update();
                    }
                    return;
                }

                if (pendingAudio) {
                    const auto eventId = _mediaRequests.takePendingAudioPlay(mxcUrl);
                    auto *target = listForEvent(eventId);
                    if (target && MediaCache::hasMemoryBlob(mxcUrl)) {
                        target->playAudioBytes(eventId, mxcUrl);
                    }
                }

                // Coalesce a media-resolution burst into a single relayout (see
                // the audio path above): defer + batched 50ms flush.
                _mediaRequests.deferInvalidation(mxcUrl);
                if (_windowActive
                    && _mediaInvalidationTimer
                    && !_mediaInvalidationTimer->isActive()) {
                    _mediaInvalidationTimer->start();
                }
                return;
            }

            MediaCache::clearRequested(mxcUrl);
            MediaCache::clearDownloadState(mxcUrl);
            // A permanent (HTTP 4xx) failure fails the same way through the
            // file-variant fallback, so suppress retries for good and stop here
            // rather than re-requesting the srvthumb:/previewthumb:/mxc:// variant.
            if (terminal) {
                MediaCache::noteResolvePermanentlyFailed(mxcUrl);
                // The bytes are never arriving, so no pending open/play intent can
                // ever be consumed. Drop them like the cancel path above: left set,
                // one would fire against a much later resolve of the same URL and
                // auto-play audio the user asked for minutes ago.
                _mediaRequests.clearPendingOpenRequests(mxcUrl);
                if (!_windowActive) {
                    _mediaRequests.deferUpdate();
                    return;
                }
                if (_list) {
                    _list->update();
                }
                return;
            }
            if (mxcUrl.startsWith(QStringLiteral("srvthumb:"))) {
                const auto mediaUrl = mxcUrl.mid(QStringLiteral("srvthumb:").size());
                if (!mediaUrl.isEmpty() && MediaCache::needsResolution(mxcUrl)) {
                    MediaCache::markRequested(mxcUrl);
                    _bridge->resolveMediaThumbnail(mediaUrl);
                    return;
                }
            }
            if (mxcUrl.startsWith(QStringLiteral("previewthumb:"))) {
                const auto mediaUrl = mxcUrl.mid(QStringLiteral("previewthumb:").size());
                if (!mediaUrl.isEmpty() && MediaCache::needsResolution(mxcUrl)) {
                    MediaCache::markRequested(mxcUrl);
                    _bridge->resolveMediaPreviewImage(mediaUrl);
                    return;
                }
            }
            if (mxcUrl.startsWith(QStringLiteral("mxc://"))
                    && MediaCache::needsResolution(mxcUrl)) {
                MediaCache::markRequested(mxcUrl);
                _bridge->resolveMedia(mxcUrl);
                return;
            }
            if (!_windowActive) {
                _mediaRequests.deferUpdate();
                return;
            }
            if (_list) {
                _list->update();
            }
        });

    // Subscribe to send confirmations.
    // Success here means "queued locally", not "server acknowledged".
    // Timeline updates (from SDK) drive Sending->Sent->Read transitions.
    // We only handle immediate enqueue failures here.
    QObject::connect(_bridge, &ProtocolBridge::messageSent,
        this, [this](quint64 requestId, bool success, const QString &eventId) {
            const auto localEventId = _pendingLocalEchoIdsByRequestId.take(requestId);
            if (localEventId.isEmpty()) {
                return;
            }
            if (success) {
                // Keep formatted body bookkeeping for event ID mapping,
                // but do not set SendState::Sent — let timeline updates
                // drive state transitions from real backend state.
                if (!eventId.isEmpty()) {
                    const auto it = _formattedBodies.constFind(localEventId);
                    if (it != _formattedBodies.cend()) {
                        _formattedBodies.insert(eventId, it.value());
                        _formattedBodies.remove(localEventId);
                    }
                }
            } else {
                _list->updateMessageSendState(localEventId, SendState::Failed);
                if (_toast) {
                    _toast->showToast(tr("Failed to send message."));
                }
            }
        });
}

HistoryWidget::~HistoryWidget() {
    MediaCache::clearMediaRecheckHook(this); // captures this — must not dangle
    clearPendingLocalMediaUploads();
}

void HistoryWidget::setupTopBar() {
    _topBar = new HistoryTopBar(this);
    _topBarShadow = new TopBarShadowWidget(this);

    _trustWarningBar = new HistoryTrustWarningBar(this);
    _trustWarningBar->hide();
    {
        auto *bar = static_cast<HistoryTrustWarningBar *>(_trustWarningBar);
        bar->onReverify = [this] {
            const auto userId = _trustWarningUserId;
            const auto name = _trustWarningBar
                ? static_cast<HistoryTrustWarningBar *>(_trustWarningBar)->displayName()
                : userId;
            hideTrustWarning();
            if (_bridge && !userId.isEmpty()) {
                auto *dialog = new VerifyUserDialog(
                    _bridge,
                    window(),
                    userId,
                    name);
                dialog->exec();
                dialog->deleteLater();
            }
        };
        bar->onDismiss = [this] {
            if (!_trustWarningUserId.isEmpty()) {
                _trustWarningDismissed.insert(_trustWarningUserId);
            }
            hideTrustWarning();
        };
    }

    // Every top-bar action below operates on a room we are a member of. In preview mode we are not,
    // so searching it, opening its pinned messages, its menu or its settings would all act on a
    // room the SDK does not know.
    _topBar->setSearchCallback([this] {
        requestSearchInCurrentRoom();
    });
    _topBar->setPinnedCallback([this] {
        if (_previewMode) {
            return;
        }
        openPinnedMessages();
    });
    _topBar->setMenuCallback([this] {
        if (_previewMode) {
            return;
        }
        showRoomQuickMenu();
    });
    _topBar->setNameClickCallback([this] {
        // In preview mode the room isn't ours to configure — show its read-only info card
        // instead of the settings panel.
        if (_previewMode) {
            if (!_previewInfo.roomId.isEmpty()) {
                auto *box = new DialogsRoomInfoBox(_previewInfo, _bridge, window());
                box->showAnimated();
            }
            return;
        }
        if (_currentRoomId.isEmpty()) {
            return;
        }
        if (isSavedMessagesRoom()) {
            showSavedMessagesInfo();
            return;
        }
        emit openRoomSettingsRequested(_currentRoomId);
    });
    _topBar->setSelectionCallbacks(
        [this] {
            if (_list) {
                _list->exitSelectionMode();
            }
        },
        [this] {
            if (_list) {
                _list->requestForwardSelected();
            }
        });
}

void HistoryWidget::resetCurrentRoomPermissions() {
    _currentRoomPermissionsLoaded = false;
    _currentRoomCanInvite = false;
    _currentRoomCanKick = false;
    _currentRoomCanBan = false;
    _currentRoomIsPublic = false;
    _currentRoomIsEncrypted = false;
}

void HistoryWidget::showSavedMessagesInfo() {
    if (_currentRoomId.isEmpty()) {
        return;
    }
    auto *box = DialogsRoomInfoBox::forSavedMessages(
        _currentRoomId, _currentRoomIsEncrypted, window());
    box->showAnimated();
}

void HistoryWidget::offerBrowserFallback(
        const QString &roomIdOrAlias, const QStringList &via) {
    if (roomIdOrAlias.isEmpty()) {
        return;
    }
    // Rebuild the permalink (id + via hints) rather than keeping the original
    // URL around — the browser's matrix.to page can route through clients on
    // other servers, which is all that is left when our homeserver cannot
    // reach the room.
    auto url = QStringLiteral("https://matrix.to/#/")
        + QString::fromLatin1(QUrl::toPercentEncoding(roomIdOrAlias));
    if (!via.isEmpty()) {
        QStringList parts;
        for (const auto &server : via) {
            parts.push_back(QStringLiteral("via=")
                + QString::fromLatin1(QUrl::toPercentEncoding(server)));
        }
        url += QLatin1Char('?') + parts.join(QLatin1Char('&'));
    }
    HistoryConfirmDialog dialog(
        this,
        QString(),
        tr("This room could not be reached through your homeserver — "
           "federation may be disabled, or the room is unknown to it. "
           "Open the link in your browser instead?"),
        tr("Open"),
        QString(),
        HistoryConfirmDialog::Normal);
    if (dialog.exec() == HistoryConfirmDialog::Accepted) {
        OpenSafeExternalUrl(url);
    }
}

bool HistoryWidget::isJoinedRoom(const QString &roomId) const {
    if (!_bridge || roomId.isEmpty()) {
        return false;
    }
    const auto rooms = _bridge->cachedRooms();
    return std::any_of(rooms.cbegin(), rooms.cend(),
        [&](const RoomSummary &room) {
            return room.roomId == roomId
                && room.membership == MembershipState::Join;
        });
}

bool HistoryWidget::isSavedMessagesRoom() const {
    return _bridge
        && !_currentRoomId.isEmpty()
        && _currentRoomId == _bridge->savedMessagesRoomId();
}

bool HistoryWidget::shouldHideSystemMessages() const {
    if (_previewMode || _currentRoomId.isEmpty()) {
        // Preview rooms already carry only messages (their raw fetch drops
        // state events).
        return false;
    }
    // Saved Messages is the user's notepad: service messages (room created,
    // encryption enabled, ...) are always noise there, toggle or not.
    if (_bridge && _currentRoomId == _bridge->savedMessagesRoomId()) {
        return true;
    }
    // Otherwise only joined public rooms, behind the Appearance toggle.
    return _hideSystemMessages && _currentRoomIsPublic;
}

void HistoryWidget::refilterCurrentTimeline() {
    if (_currentRoomId.isEmpty() || _previewMode || !_bridge || !_list
        || _list->messages().isEmpty()) {
        return;
    }
    // A fresh full slice re-runs applyTimelineSlice, which applies the current filter state and
    // rebuilds the list. (Full, not a delta, so the event-ID diff keeps the scroll position.)
    _latestTimelineSliceRequestId = _bridge->nextRequestId();
    _bridge->getTimelineSliceAsync(_currentRoomId, _latestTimelineSliceRequestId);
}

bool HistoryWidget::currentRoomCanManageMembers() const {
    return _currentRoomPermissionsLoaded
        && (_currentRoomCanInvite || _currentRoomCanKick || _currentRoomCanBan);
}

void HistoryWidget::showRoomQuickMenu() {
    if (_currentRoomId.isEmpty() || !_bridge) {
        return;
    }

    // Toggle: if menu is already open or was JUST closed (by the same
    // click's press event hiding it before the release fires), skip.
    if (_topBarMenu) {
        _topBarMenu->close();
        return;
    }
    if (_topBarMenuClosedAt.isValid()
        && _topBarMenuClosedAt.msecsTo(QDateTime::currentDateTime()) < 200) {
        return;
    }

    const auto mode = _currentNotificationMode;
    if (!_currentRoomPermissionsLoaded) {
        _bridge->getRoomSettings(_currentRoomId);
    }
    // Saved Messages keeps only Copy Room Link + Export History: it cannot be
    // muted, left, or have members managed.
    const auto savedRoom = isSavedMessagesRoom();
    const auto canInvite = !savedRoom
        && _currentRoomPermissionsLoaded && _currentRoomCanInvite;
    const auto canManageMembers = !savedRoom && currentRoomCanManageMembers();

    auto *menu = HistoryPopupMenuStyle::createStyledMenu(
        _topBar,
        HistoryPopupMenuStyle::Variant::WithIcons);

    const auto addAction = [menu](
            const QString &label,
            const QString &icon,
            const std::function<void()> &slot) {
        auto *action = menu->addAction(label);
        HistoryPopupMenuStyle::setActionIconName(action, icon);
        if (slot) {
            QObject::connect(action, &QAction::triggered, menu, slot);
        }
        return action;
    };

    const auto roomId = _currentRoomId;

    const auto setMode = [this, roomId](RoomNotificationMode newMode) {
        _currentNotificationMode = newMode;
        if (_topBar) {
            _topBar->setMuted(!isSavedMessagesRoom() && isToolbarMuted(newMode, false));
        }
        _bridge->setRoomNotificationMode(roomId, newMode);
    };

    if (savedRoom) {
        // No mute entry at all.
    } else if (mode != RoomNotificationMode::AllMessages) {
        addAction(
            tr("Unmute"),
            QStringLiteral("unmute"),
            [setMode] { setMode(RoomNotificationMode::AllMessages); });
    } else {
        // Not muted: "Mute" opens a nested submenu to pick the mute mode.
        // Both modes are "muting" — the muted branch above
        // collapses back to a single "Unmute".
        auto *muteMenu = HistoryPopupMenuStyle::createStyledMenu(
            menu,
            HistoryPopupMenuStyle::Variant::WithIcons);
        auto *mentionsAction = muteMenu->addAction(
            tr("Except mentions and keywords"), [setMode] {
                setMode(RoomNotificationMode::MentionsOnly);
            });
        HistoryPopupMenuStyle::setActionIconName(
            mentionsAction, QStringLiteral("mute_mentions"));
        auto *allMessagesAction = muteMenu->addAction(tr("All messages"), [setMode] {
            setMode(RoomNotificationMode::Mute);
        });
        HistoryPopupMenuStyle::setActionIconName(
            allMessagesAction, QStringLiteral("mute"));
        allMessagesAction->setProperty("_tm_attention", true);
        auto *muteAction = addAction(
            tr("Mute"),
            QStringLiteral("mute"),
            std::function<void()>());
        menu->setSubmenu(muteAction, muteMenu);
    }

    if (canInvite) {
        // Add Members. The invite box loads the room snapshot asynchronously
        // first so existing members are not shown as invite candidates.
        addAction(
            tr("Add Members"),
            QStringLiteral("add_member"),
            [this, roomId] {
                InviteUsersBox dialog(roomId, _bridge, this, true);
                dialog.exec();
            });
    }

    if (canManageMembers) {
        // Manage Members — opens the room settings Members block.
        addAction(
            tr("Manage Members"),
            QStringLiteral("groups"),
            [this, roomId] {
                emit openRoomMembersSettingsRequested(roomId);
            });
    }

    // Copy Room Link.
    addAction(
        tr("Copy Room Link"),
        QStringLiteral("link"),
        [this, roomId] {
            QString matrixId = roomId;
            const auto rooms = _bridge->cachedRooms();
            for (const auto &room : rooms) {
                if (room.roomId == roomId) {
                    if (!room.canonicalAlias.isEmpty()) {
                        matrixId = room.canonicalAlias;
                    }
                    break;
                }
            }
            const auto link = QStringLiteral("https://matrix.to/#/")
                + QString::fromLatin1(QUrl::toPercentEncoding(matrixId));
            QGuiApplication::clipboard()->setText(link);
            if (_toast) {
                _toast->showToast(tr("Room link copied to clipboard"));
            }
        });

    // Export History.
    addAction(
        tr("Export History"),
        QStringLiteral("download"),
        [this, roomId] {
            exportRoomHistory(roomId);
        });

    // Pinned Messages — only when the room actually has pinned messages.
    if (!savedRoom && !_pinnedState.isEmpty()) {
        addAction(
            tr("Pinned Messages"),
            QStringLiteral("pin"),
            [this] {
                openPinnedMessages();
            });
    }

    if (!savedRoom) {
        menu->addSeparator();

        // Leave Room (attention/destructive).
        auto *leaveAction = addAction(
            tr("Leave Room"),
            QStringLiteral("leave"),
            [this, roomId] {
                HistoryConfirmDialog dialog(
                    this,
                    QString(),
                    tr("Are you sure you want to leave this room?"),
                    tr("Leave"),
                    QString(),
                    HistoryConfirmDialog::Attention);
                if (dialog.exec() == HistoryConfirmDialog::Accepted) {
                    _bridge->leaveRoom(roomId);
                }
            });
        leaveAction->setProperty("_tm_attention", true);
    } else {
        menu->addSeparator();

        // "Delete" (attention/destructive): permanently deletes the room. A
        // later forward or open creates a fresh, empty one.
        auto *deleteAction = addAction(
            tr("Delete chat"),
            QStringLiteral("delete"),
            [this] {
                HistoryConfirmDialog dialog(
                    this,
                    tr("Delete Saved Messages"),
                    tr("This permanently deletes Saved Messages and everything "
                       "in it. This can't be undone."),
                    tr("Delete"),
                    QString(),
                    HistoryConfirmDialog::Attention);
                if (dialog.exec() == HistoryConfirmDialog::Accepted) {
                    _bridge->deleteSavedMessages();
                }
            });
        deleteAction->setProperty("_tm_attention", true);
    }

    // Track the menu for toggle-on-click and button highlight.
    _topBarMenu = menu;
    _topBar->setMenuActive(true);
    QObject::connect(menu, &HistoryPopupMenuStyle::PopupMenu::aboutToHide,
                     this, [this, menu] {
        _topBarMenuClosedAt = QDateTime::currentDateTime();
        _topBar->setMenuActive(false);
        _topBarMenu = nullptr;
        menu->deleteLater();
    });

    // Position menu just below the three-dots button.
    menu->popup(_topBar->menuButtonBottomGlobal());

    // Re-activate the main window so the top bar button receives
    // mouse events (cursor tracking) while the popup is visible.
    // The Qt::Tool popup stays visible even when not the active window.
    window()->activateWindow();
}

void HistoryWidget::exportRoomHistory(const QString &roomId) {
    if (roomId.isEmpty() || !_bridge) {
        return;
    }

    const auto timeline = (_list && roomId == _currentRoomId)
        ? _list->messages()
        : QVector<TimelineItem>();
    if (timeline.isEmpty()) {
        if (_toast) {
            _toast->showToast(tr("No messages to export."));
        }
        return;
    }

    // Look up room name for the filename suggestion.
    QString roomName = roomId;
    const auto rooms = _bridge->cachedRooms();
    for (const auto &room : rooms) {
        if (room.roomId == roomId) {
            roomName = room.displayName;
            break;
        }
    }

    // Sanitize room name for filesystem.
    QString safeName = roomName;
    safeName.replace(QRegularExpression(QStringLiteral("[^a-zA-Z0-9_ -]")), QStringLiteral("_"));
    if (safeName.isEmpty()) {
        safeName = QStringLiteral("chat_export");
    }

    const auto defaultPath = QDir::homePath()
        + QStringLiteral("/")
        + safeName
        + QStringLiteral(".json");

    const auto filePath = QFileDialog::getSaveFileName(
        this,
        tr("Export History"),
        defaultPath,
        tr("JSON Files (*.json);;Text Files (*.txt);;All Files (*)"));

    if (filePath.isEmpty()) {
        return;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (_toast) {
            _toast->showToast(tr("Failed to write export file."));
        }
        return;
    }

    // Write JSON export.
    QJsonArray messagesArray;
    for (const auto &item : timeline) {
        QJsonObject msg;
        msg[QStringLiteral("event_id")] = item.eventId;
        msg[QStringLiteral("sender_id")] = item.sender.id;
        msg[QStringLiteral("sender_name")] = item.sender.name;
        msg[QStringLiteral("timestamp")] = item.timestamp;
        msg[QStringLiteral("body")] = bodyText(item);
        if (!formattedText(item).isEmpty()) {
            msg[QStringLiteral("formatted_body")] = formattedText(item);
        }
        switch (contentType(item)) {
        case ContentType::Text: msg[QStringLiteral("type")] = QStringLiteral("text"); break;
        case ContentType::Image: msg[QStringLiteral("type")] = QStringLiteral("image"); break;
        case ContentType::File: msg[QStringLiteral("type")] = QStringLiteral("file"); break;
        case ContentType::Video: msg[QStringLiteral("type")] = QStringLiteral("video"); break;
        case ContentType::Audio: msg[QStringLiteral("type")] = QStringLiteral("audio"); break;
        case ContentType::Service: msg[QStringLiteral("type")] = QStringLiteral("service"); break;
        case ContentType::Poll: msg[QStringLiteral("type")] = QStringLiteral("poll"); break;
        case ContentType::UnableToDecrypt: msg[QStringLiteral("type")] = QStringLiteral("utd"); break;
        }
        if (!mediaUrl(item).isEmpty()) {
            msg[QStringLiteral("media_url")] = mediaUrl(item);
        }
        if (!mediaFilename(item).isEmpty()) {
            msg[QStringLiteral("media_filename")] = mediaFilename(item);
        }
        messagesArray.append(msg);
    }

    QJsonObject root;
    root[QStringLiteral("room_id")] = roomId;
    root[QStringLiteral("room_name")] = roomName;
    root[QStringLiteral("exported_at")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    root[QStringLiteral("message_count")] = messagesArray.size();
    root[QStringLiteral("messages")] = messagesArray;

    QJsonDocument doc(root);
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();

    if (_toast) {
        _toast->showToast(tr("History exported successfully."));
    }
}

void HistoryWidget::mentionUser(const QString &userId, const QString &displayName) {
    if (!_input || userId.isEmpty()) {
        return;
    }
    _input->insertMentionAtCursor(
        userId,
        displayName.isEmpty() ? userId : displayName);
}

void HistoryWidget::wireInlineVideoPlayer(HistoryInlineVideoPlayer *player) {
    if (!player) {
        return;
    }
    player->setStreamUrlCallback([this](const QString &mxc) -> QString {
        return _bridge ? _bridge->videoStreamUrl(mxc) : QString();
    });
    player->setStreamProgressCallback([this](const QString &mxc) -> float {
        return _bridge ? _bridge->videoStreamProgress(mxc) : 1.0f;
    });
    player->setStreamProgressBytesCallback(
        [this](const QString &mxc, quint64 &d, quint64 &t) {
            return _bridge && _bridge->videoStreamProgressBytes(mxc, d, t);
        });
    player->setStreamErroredCallback([this](const QString &mxc) {
        return _bridge && _bridge->videoStreamErrored(mxc);
    });
    player->setStreamContainerCallback([this](const QString &mxc) {
        return _bridge ? _bridge->videoStreamContainer(mxc)
                       : VideoContainer::Unknown;
    });
    player->setResolveRequester([this](const QString &mxc) {
        if (_bridge) {
            _bridge->resolveMedia(mxc);
        }
    });
}

void HistoryWidget::setupMessageList() {
    _scroll = new Ui::ScrollArea(this);
    _list = new HistoryList(_scroll);
    _scroll->setOwnedWidget(object_ptr<HistoryList>::fromRaw(_list));

    // Wire the inline video player (owned by the list) to the streaming proxy,
    // mirroring the fullscreen overlay's callbacks.
    wireInlineVideoPlayer(_list->inlineVideoPlayer());
    QObject::connect(
        _scroll->verticalScrollBar(),
        &QScrollBar::valueChanged,
        this,
        [this](int value) {
            if (_list) {
                _list->updateVisibleTop(value);
            }
            // Re-evaluate at a settled scroll position: parking at / leaving the
            // live bottom is exactly what arms / disarms the read-consuming clamp.
            // For an at-bottom arrival the only valueChanged in the turn is the
            // final scrollToY(max), which re-affirms the armed state (a no-op).
            updateReadConsumingGate();
        });
    QObject::connect(
        _scroll->verticalScrollBar(),
        &QScrollBar::rangeChanged,
        this,
        [this](int, int) {
            if (_list && _scroll) {
                _list->updateVisibleTop(_scroll->scrollTop());
            }
        });
    _list->updateVisibleTop(_scroll->scrollTop());

    // actionTriggered fires only for user-driven slider actions (drag, page,
    // arrows) — never for a programmatic value clamp on rangeChanged — so it is
    // the clean signal that the user is scrolling to read: re-arm detection.
    QObject::connect(
        _scroll->verticalScrollBar(),
        &QScrollBar::actionTriggered,
        this,
        [this](int) {
            if (_list) {
                _list->resetReadDetectionHold();
            }
        });

    // Pagination threshold check on scroll.
    connect(_scroll->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
        if (_forceBottomEntryUntilLiveSlice && !isScrollNearBottom(_scroll)) {
            _forceBottomEntryUntilLiveSlice = false;
        }
        checkPaginationThresholds();
        maybeFinalizeUnreadBarAfterScroll();
        // After a pinned jump completes, if the user scrolls manually,
        // reset the pinned bar back to the first pinned message.
        if ((!_scrollToAnimation
                || _scrollToAnimation->state() != QAbstractAnimation::Running)
            && _pinnedState.consumeManualScrollReset()) {
            refreshPinnedBar();
        }
    });

    // Connect HistoryList's scroll anchor restoration.
    connect(_list, &HistoryList::scrollToRequested, this, [this](int position) {
        _scroll->scrollToY(position);
    });

    // Anchor the wallpaper to this widget (the chat column), so the top bar and
    // the composer change which slice shows, never the doodle's scale.
    _list->setBackgroundAnchor(this);

    // Build the wallpaper from the current theme's corner colours + doodle.
    if (auto *tm = _controller->themeManager()) {
        _list->invalidateBackground();
        connect(tm, &Theme::ThemeManager::themeChanged,
                this, [this](bool /*isNight*/, Theme::ThemeMode /*mode*/) {
            // Flush cached text layouts so link/mono colors
            // are re-resolved from the current st:: tokens.
            HistoryMessage::clearPaintCache();
            if (_list) {
                _list->invalidateBackground();
            }
            // Repaint the scroll-down button with updated theme colors.
            if (_downButton) {
                _downButton->update();
            }
        });
    }

    // Feed large emoji setting to message list.
    _list->setLargeEmojiEnabled(_controller->settings().largeEmoji());
    _list->setBackgroundDoodlesEnabled(_controller->settings().backgroundDoodles());

    // Feed hover reply / reaction button settings to the message list.
    _list->setReplyButtonEnabled(_controller->settings().replyButtonOnMessages());
    _list->setReactionButtonEnabled(_controller->settings().reactionButtonOnMessages());

    // Feed recent reaction emoji to the hover reaction column (quick reactions
    // are prepended by the list). Recents are global; refreshed on each setup.
    {
        QVector<QString> recents;
        for (const auto &re : _controller->accountSettings().recentEmoji()) {
            if (!re.emoji.isEmpty()) {
                recents.push_back(re.emoji);
            }
        }
        _list->setReactionRecentEmojis(recents);
    }

    _toast = new Ui::ToastWidget(this);
    _toast->hide();
    connect(_list, &HistoryList::codeCopied, this, [this] {
        _toast->showToast(tr("Code copied to clipboard"));
    });
    connect(_list, &HistoryList::contextActionFeedback, this, [this](const QString &text) {
        _toast->showToast(text);
    });
    connect(_list, &HistoryList::openMediaViewRequested, this, [this](
            const QVector<TimelineItem> &items,
            int index) {
        emit openMediaViewRequested(items, index);
    });
    connect(_list, &HistoryList::replyToMessageRequested, this,
        [this](const QString &eventId, const QString &originEventId) {
        if (eventId.isEmpty()) {
            return;
        }
        // Use jumpToMessage so that if the quoted message is not in the
        // current slice we fetch a focused slice via focusOnEvent.
        pushReplyReturn(originEventId);
        jumpToMessage(eventId);
    });
    connect(_list, &HistoryList::matrixLinkActivated, this,
        [this](const QString &roomId, const QString &eventId,
               const QStringList &via) {
        // Unjoined target: route straight to a preview, via hints intact —
        // they are what lets the homeserver find a room-id on another server.
        if (roomId != _currentRoomId && !isJoinedRoom(roomId)) {
            emit roomSwitchRequested(roomId, via);
            return;
        }
        if (eventId.isEmpty()) {
            // Room-only link — switch to that room.
            if (roomId != _currentRoomId) {
                loadRoom(roomId);
                emit roomSwitchRequested(roomId);
            }
        } else {
            // Room + event link — jump to message.
            pushReturnPosition();
            showMessage(roomId, eventId);
        }
    });
    connect(_list, &HistoryList::matrixUserLinkActivated, this,
        [this](const QString &userId) {
        // A bare user link carries no timeline name, so let the popup fetch it.
        emit openUserProfileRequested(_currentRoomId, userId, QString());
    });
    connect(_list, &HistoryList::cancelUploadRequested, this,
        [this](const QString &eventId) {
        cancelUploadForEvent(eventId);
    });
    connect(_list, &HistoryList::selectionModeChanged, this, [this](bool active) {
        _topBar->setSelectionMode(active);
        if (!active) {
            _topBar->setSelectedCount(0);
        }
        updateControlsGeometry();
    });
    connect(_list, &HistoryList::selectedCountChanged, this, [this](int count) {
        _topBar->setSelectedCount(count);
    });
    connect(_list, &HistoryList::editMessageRequested, this, [this](
            const QString &eventId,
            const QString &senderName,
            const QString &body,
            const QString &formattedBody) {
        if (_currentRoomId.isEmpty() || eventId.isEmpty()) {
            return;
        }
        _input->enterEditMode(eventId, senderName, body, formattedBody);
    });
    connect(_list, &HistoryList::replyRequested, this, [this](
            const QString &eventId,
            const QString &senderName,
            const QString &body,
            const QString &quotedText) {
        if (_currentRoomId.isEmpty() || eventId.isEmpty()) {
            return;
        }
        const auto preview = resolveComposeReplyPreview(_list->messages(), eventId, body);
        _input->enterReplyMode(
            eventId,
            senderName,
            preview.text,
            quotedText,
            preview.path);
    });
    connect(_list, &HistoryList::deleteMessageRequested, this, [this](const QString &eventId) {
        if (_currentRoomId.isEmpty() || eventId.isEmpty()) {
            return;
        }
        // Skip confirmation for failed messages — they haven't been sent.
        bool isFailed = false;
        bool isLocalOnly = false;
        if (_list) {
            for (const auto &msg : _list->messages()) {
                if (msg.eventId == eventId && msg.delivery.sendState == SendState::Failed) {
                    isFailed = true;
                    // A failed direct-upload echo is local-only too (it has no
                    // server event to redact) — discard it locally.
                    isLocalOnly = isLocalOnlyFailedTextEcho(msg)
                        || _pendingLocalMedia.contains(eventId);
                    break;
                }
            }
        }
        if (isFailed) {
            if (isLocalOnly) {
                _list->removeMessage(eventId);
                _formattedBodies.remove(eventId);
                // No-op for text; for media drops the optimistic echo, pending
                // params and temp files.
                removePendingLocalMediaUpload(eventId);
                return;
            }
            if (_list) {
                _list->markDeleting(eventId);
            }
            _pendingDeleteEventIds.insert(eventId);
            _bridge->deleteMessage(_currentRoomId, eventId);
            return;
        }
        const auto uploading = isUploadInFlight(eventId);
        HistoryConfirmDialog dialog(
            this,
            QString(),
            uploading
                ? tr("Do you want to cancel this upload?")
                : tr("Do you want to delete this message?"),
            uploading ? tr("Cancel upload") : tr("Delete"),
            QString(),
            HistoryConfirmDialog::Attention);
        if (dialog.exec() != HistoryConfirmDialog::Accepted) {
            return;
        }
        if (uploading) {
            // Still transferring: there is no server event to redact yet, so
            // abort the upload instead (the backend redacts if it landed while
            // the dialog was open). The bubble goes at once — no dim, since it
            // isn't waiting on a redaction.
            cancelUploadForEvent(eventId);
            return;
        }
        if (_list) {
            _list->markDeleting(eventId);
        }
        _pendingDeleteEventIds.insert(eventId);
        _bridge->deleteMessage(_currentRoomId, eventId);
    });
    connect(_list, &HistoryList::resendRequested, this, [this](const QString &eventId) {
        if (_currentRoomId.isEmpty() || eventId.isEmpty() || !_list) {
            return;
        }
        // Find the failed message and re-send its content.
        for (const auto &msg : _list->messages()) {
            if (msg.eventId == eventId && msg.delivery.sendState == SendState::Failed) {
                if (!isTextMessage(msg)) {
                    // Re-run the direct upload with the SAME transaction id, from
                    // the retained send params (they survive room switches). On
                    // success the remote reconciles with this same echo.
                    const auto pending = _pendingLocalMedia.upload(eventId);
                    if (!pending.has_value() || !_bridge) {
                        if (_toast) {
                            _toast->showToast(tr("Can't resend this upload."));
                        }
                        break;
                    }
                    if (auto it = _optimisticMediaEchoes.find(eventId);
                            it != _optimisticMediaEchoes.end()) {
                        it->delivery.sendState = SendState::Sending;
                        it->delivery.uploadProgress = -1.0;
                    }
                    _list->updateMessageSendState(eventId, SendState::Sending);
                    _list->updateMessageUploadProgress(eventId, -1.0);
                    _pendingLocalMedia.setUploadPath(eventId, pending->mediaPath);
                    _bridge->sendMedia(
                        _currentRoomId, pending->type, pending->mediaPath,
                        pending->mime, pending->filename, pending->caption,
                        pending->thumbPath, pending->size, pending->width,
                        pending->height, pending->durationMs, eventId);
                    break;
                }
                const auto body = bodyText(msg);
                const auto formattedBody = formattedText(msg);
                const auto replyToEventId = replyEventId(msg);
                if (isLocalOnlyFailedTextEcho(msg)) {
                    _list->removeMessage(eventId);
                    _formattedBodies.remove(eventId);
                    onSendMessage(body, formattedBody, replyToEventId);
                } else {
                    _bridge->deleteMessage(_currentRoomId, eventId);
                    _bridge->sendMessage(_currentRoomId, body, formattedBody, replyToEventId);
                }
                break;
            }
        }
    });
    connect(_bridge, &ProtocolBridge::messageDeleted, this, [this](bool success) {
        if (success) {
            // The redacted item returns via the reload as delivery.deleted, which
            // takes over styling; the dim markers are dropped on setMessages.
            _pendingDeleteEventIds.clear();
            loadRoom(_currentRoomId);
            return;
        }
        // Failure: undim what we marked. messageDeleted carries no event id, so
        // clear all in-flight deletes (concurrent deletes are rare).
        for (const auto &id : _pendingDeleteEventIds) {
            if (_list) {
                _list->clearDeleting(id);
            }
            if (_pinnedList) {
                _pinnedList->clearDeleting(id);
            }
        }
        _pendingDeleteEventIds.clear();
        if (_toast) {
            _toast->showToast(tr("Failed to delete message."));
        }
    });
    connect(_bridge, &ProtocolBridge::urlPreviewFetchingChanged, this,
        [this](const QString &roomId, const QString &eventId, bool fetching) {
            if (_list && roomId == _currentRoomId) {
                _list->setPreviewFetching(eventId, fetching);
            }
        });
    connect(_bridge, &ProtocolBridge::roomNotificationModeSet, this, [this](bool success) {
        if (!success && _toast) {
            _toast->showToast(tr("Failed to change notification mode."));
        }
    });
    connect(_bridge, &ProtocolBridge::roomNotificationModeChangeRequested,
        this, [this](const QString &roomId, RoomNotificationMode mode) {
            if (roomId != _currentRoomId) {
                return;
            }
            _currentNotificationMode = mode;
            if (_topBar) {
                _topBar->setMuted(!isSavedMessagesRoom() && isToolbarMuted(mode, false));
            }
        });
    connect(_bridge, &ProtocolBridge::roomNotificationModeSetForRoom,
        this, [this](const QString &roomId, RoomNotificationMode mode, bool success) {
            if (roomId != _currentRoomId) {
                return;
            }
            if (!success) {
                if (_bridge) {
                    _bridge->getRoomSettings(roomId);
                }
                return;
            }
            _currentNotificationMode = mode;
            if (_topBar) {
                _topBar->setMuted(!isSavedMessagesRoom() && isToolbarMuted(mode, false));
            }
        });
    connect(_bridge, &ProtocolBridge::inviteAccepted, this, [this](bool success, const QString &roomId) {
        if (success && roomId == _currentRoomId) {
            hideInvitePanel();
            showChatControls(true);
            loadRoom(roomId);
        }
    });
    connect(_bridge, &ProtocolBridge::roomLeft, this, [this](bool /*success*/) {
        // If we were viewing an invited room, close it on decline.
        if (_isInvitedRoom) {
            hideInvitePanel();
            closeRoom();
        }
    });
    connect(_bridge, &ProtocolBridge::roomPreviewReady,
        this, &HistoryWidget::onPreviewReady);
    connect(_bridge, &ProtocolBridge::roomPreviewMessagesReady,
        this, &HistoryWidget::onPreviewMessagesReady);
    connect(_bridge, &ProtocolBridge::roomJoined,
        this, &HistoryWidget::onJoinResult);
    connect(_bridge, &ProtocolBridge::roomKnocked,
        this, &HistoryWidget::onKnockResult);
    connect(_list, &HistoryList::userAvatarClicked, this, [this](const QString &userId, const QString &eventId) {
        if (_currentRoomId.isEmpty() || userId.isEmpty()) {
            return;
        }
        // Pass the sender name shown in the timeline so the popup matches it — the fetched profile
        // is the global one in unjoined/preview rooms and can differ or be missing.
        QString displayName;
        if (_list && !eventId.isEmpty()) {
            for (const auto &msg : _list->messages()) {
                if (msg.eventId == eventId) {
                    displayName = msg.sender.name;
                    break;
                }
            }
        }
        emit openUserProfileRequested(_currentRoomId, userId, displayName);
    });
    connect(_list, &HistoryList::verifySessionRequested, this, [this] {
        const auto verified = ShowVerifySessionPopup(_bridge, window());
        if (verified && !_currentRoomId.isEmpty()) {
            _list->setSessionVerified(true);
            onTimelineChanged(_currentRoomId);
        }
    });
    connect(_bridge, &ProtocolBridge::verificationStateChanged, this, [this](int state, const QString &flowId) {
        Q_UNUSED(flowId); // Done means a device got verified, regardless of flow.
        constexpr int kDone = 8;
        if (state != kDone || !_list) {
            return;
        }
        // Verification can finish with no room open (intro flow); record it regardless.
        _list->setSessionVerified(true);
        if (!_currentRoomId.isEmpty()) {
            onTimelineChanged(_currentRoomId);
        }
    });

    // Authoritative verified status pushed from Rust's verification-state
    // observable; seed from the bridge's cache (set before this widget exists).
    _list->setSessionVerified(_bridge->isDeviceVerified());
    connect(_bridge, &ProtocolBridge::deviceVerifiedChanged,
        this, [this](bool verified) {
        if (!_list) {
            return;
        }
        const bool wasVerified = _list->isSessionVerified();
        _list->setSessionVerified(verified);
        if (verified && !wasVerified && !_currentRoomId.isEmpty()) {
            onTimelineChanged(_currentRoomId);
        }
    });
    connect(_list, &HistoryList::pinMessageRequested, this, [this](const QString &eventId, bool pinned) {
        if (_currentRoomId.isEmpty() || eventId.isEmpty()) {
            return;
        }
        _pendingPinEventId = eventId;
        _pendingPinState = pinned;
        applyPinStateLocally(eventId, pinned);
        _bridge->pinMessage(_currentRoomId, eventId, pinned);
    });
    connect(_list, &HistoryList::forwardRequested, this, [this](const QString &eventId) {
        if (_currentRoomId.isEmpty() || eventId.isEmpty()) {
            return;
        }

        const auto targets = SavedMessages::arrangeForwardTargets(
            _bridge->cachedRooms(),
            _bridge->savedMessagesRoomId(),
            _currentRoomId);

        if (targets.isEmpty()) {
            if (_toast) {
                _toast->showToast(tr("No destination rooms available."));
            }
            return;
        }

        HistoryForwardDialog dialog(targets, _bridge, this);
        if (dialog.exec() != HistoryForwardDialog::Accepted) {
            return;
        }
        const auto dstRoomId = dialog.selectedRoomId();
        if (dstRoomId.isEmpty()) {
            return;
        }
        resolveForwardDestination(dstRoomId,
            [this, eventId, src = _currentRoomId](const QString &roomId) {
                _bridge->forwardMessage(src, eventId, roomId);
            });
    });
    connect(_list, &HistoryList::forwardSelectedRequested, this, [this](const QStringList &eventIds) {
        if (_currentRoomId.isEmpty() || eventIds.isEmpty()) {
            return;
        }

        const auto targets = SavedMessages::arrangeForwardTargets(
            _bridge->cachedRooms(),
            _bridge->savedMessagesRoomId(),
            _currentRoomId);

        if (targets.isEmpty()) {
            if (_toast) {
                _toast->showToast(tr("No destination rooms available."));
            }
            return;
        }

        HistoryForwardDialog dialog(targets, _bridge, this);
        if (dialog.exec() != HistoryForwardDialog::Accepted) {
            return;
        }
        const auto dstRoomId = dialog.selectedRoomId();
        if (dstRoomId.isEmpty()) {
            return;
        }

        _list->exitSelectionMode();
        resolveForwardDestination(dstRoomId,
            [this, eventIds, src = _currentRoomId](const QString &roomId) {
                for (const auto &eventId : eventIds) {
                    if (!eventId.isEmpty()) {
                        _bridge->forwardMessage(src, eventId, roomId);
                    }
                }
            });
    });
    connect(_list, &HistoryList::reactionRequested, this, [this](
            const QString &eventId,
            const QString &key,
            bool active) {
        if (_currentRoomId.isEmpty() || eventId.isEmpty() || key.isEmpty()) {
            return;
        }
        const auto keepTop = _scroll ? _scroll->scrollTop() : 0;
        const auto wasAtBottom = isScrollNearBottom(_scroll);
        const auto result = _list->applyReactionLocally(eventId, key, active);
        if (result.changed && _scroll) {
            if (wasAtBottom) {
                // The reacted message is the bottom-most one: its growth extends
                // the content's bottom, so snap to the new bottom to keep it fully
                // above the input instead of letting it spill under the input.
                _scroll->scrollToY(_scroll->scrollTopMax());
            } else {
                // If the reacted message is above the viewport, its height change
                // shifts visible content down; compensate so it stays in place.
                auto adjustedTop = keepTop;
                if (result.messageY < keepTop) {
                    adjustedTop += result.heightDelta;
                }
                _scroll->scrollToY(qBound(0, adjustedTop, _scroll->scrollTopMax()));
            }
        }
        _bridge->setReaction(_currentRoomId, eventId, key, active);
    });
    connect(_list, &HistoryList::pollVoteRequested, this, [this](
            const QString &roomId,
            const QString &pollEventId,
            const QStringList &optionIds) {
        if (roomId.isEmpty() || pollEventId.isEmpty() || optionIds.isEmpty()) {
            return;
        }
        _bridge->sendPollVote(roomId, pollEventId, optionIds);
    });
    connect(_bridge, &ProtocolBridge::messagePinned, this, [this](bool success) {
        if (success) {
            _pendingPinEventId.clear();
        } else {
            // Revert optimistic update.
            if (!_pendingPinEventId.isEmpty()) {
                applyPinStateLocally(_pendingPinEventId, !_pendingPinState);
                _pendingPinEventId.clear();
            }
            if (_toast) {
                _toast->showToast(tr("Unable to pin message. You may not have permission."));
            }
        }
    });
    connect(_bridge, &ProtocolBridge::messageForwarded, this, [this](
            bool success,
            const QString & /*eventId*/) {
        if (success) {
            if (_toast) {
                _toast->showToast(tr("Message forwarded."));
            }
        } else if (_toast) {
            _toast->showToast(tr("Failed to forward message."));
        }
    });
    connect(_bridge, &ProtocolBridge::reactionSet, this, [this](bool success) {
        if (!success) {
            if (!_currentRoomId.isEmpty()) {
                loadRoom(_currentRoomId);
            }
            if (_toast) {
                _toast->showToast(tr("Failed to set reaction."));
            }
        }
    });
    connect(_bridge, &ProtocolBridge::pollVoteSent, this, [this](bool success) {
        if (!success && !_currentRoomId.isEmpty()) {
            loadRoom(_currentRoomId);
        }
    });
    connect(_bridge, &ProtocolBridge::userBanned, this, [this](bool success) {
        if (_toast) {
            _toast->showToast(success
                ? tr("User banned.")
                : tr("Failed to ban user."));
        }
    });
    connect(_bridge, &ProtocolBridge::userUnbanned, this, [this](bool success) {
        if (_toast) {
            _toast->showToast(success
                ? tr("User unbanned.")
                : tr("Failed to unban user."));
        }
    });

    // Reaction bar "+" button opens emoji picker in reaction mode.
    connect(_list, &HistoryList::reactionPanelRequested,
            this, [this](const QString &eventId, const QPoint &menuPos) {
        if (_currentRoomId.isEmpty() || eventId.isEmpty()) {
            return;
        }
        // Use the input's emoji picker mechanism — create a standalone one
        // in reaction mode positioned near the anchor.
        if (!_reactionEmojiPicker) {
            _reactionEmojiPicker = new HistoryEmojiPicker(_controller, window());
            connect(_reactionEmojiPicker, &HistoryEmojiPicker::reactionSelected,
                    this, [this](const QString &evtId, const QString &emoji) {
                if (_currentRoomId.isEmpty() || evtId.isEmpty() || emoji.isEmpty()) {
                    return;
                }
                // Toggle reaction (check if already self-reacted).
                bool active = true;
                if (_list) {
                    for (const auto &msg : _list->messages()) {
                        if (msg.eventId == evtId) {
                            for (const auto &r : msg.reactions) {
                                if (r.key == emoji && r.isSelf) {
                                    active = false;
                                    break;
                                }
                            }
                            break;
                        }
                    }
                }
                const auto keepTop = _scroll ? _scroll->scrollTop() : 0;
                const auto wasAtBottom = isScrollNearBottom(_scroll);
                const auto result = _list->applyReactionLocally(evtId, emoji, active);
                if (result.changed && _scroll) {
                    if (wasAtBottom) {
                        _scroll->scrollToY(_scroll->scrollTopMax());
                    } else {
                        auto adjustedTop = keepTop;
                        if (result.messageY < keepTop) {
                            adjustedTop += result.heightDelta;
                        }
                        _scroll->scrollToY(qBound(0, adjustedTop, _scroll->scrollTopMax()));
                    }
                }
                _bridge->setReaction(_currentRoomId, evtId, emoji, active);
            });
        }
        _reactionEmojiPicker->setMode(HistoryEmojiPicker::PickerMode::Reaction);
        _reactionEmojiPicker->setReactionTarget(eventId);

        // Defer showing until the next event loop iteration so the popup
        // menu's event filter is fully removed before the picker appears.
        const auto savedMenuPos = menuPos;
        QTimer::singleShot(0, this, [this, savedMenuPos]() {
            if (!_reactionEmojiPicker) return;
            HistoryEmojiPicker::initEmojiPanelPxValues();

            const auto shadow = HistoryEmojiPicker::shadowExtend();
            const auto panelWidth = HistoryEmojiPicker::panelWidth() + 2 * shadow;
            const auto bodyHeight = HistoryEmojiPicker::minBodyHeight() + 2 * shadow;
            const auto hint = QSize(panelWidth, bodyHeight);

            // Align picker's left edge to menu's left edge, below the menu position.
            auto pos = QPoint(savedMenuPos.x(), savedMenuPos.y());

            // Clamp to screen.
            if (auto *screen = window()->screen()) {
                const auto area = screen->availableGeometry();
                if (pos.x() + hint.width() > area.right() + 1)
                    pos.setX(area.right() + 1 - hint.width());
                if (pos.x() < area.left())
                    pos.setX(area.left());
                if (pos.y() < area.top())
                    pos.setY(savedMenuPos.y());
                if (pos.y() + hint.height() > area.bottom() + 1)
                    pos.setY(area.bottom() + 1 - hint.height());
            }

            _reactionEmojiPicker->resize(hint);
            _reactionEmojiPicker->move(pos);
            _reactionEmojiPicker->show();
            _reactionEmojiPicker->raise();
        });
    });

    connect(_list, &HistoryList::fileDownloadRequested,
            this, [this](const QString &mxcUrl) {
        _mediaRequests.resumeDownload(mxcUrl);
        _mediaRequests.requestFileOpen(mxcUrl);
        if (MediaCache::needsResolution(mxcUrl)) {
            MediaCache::markRequested(mxcUrl);
            _bridge->resolveMedia(mxcUrl);
        }
        _list->update();
    });

    connect(_list, &HistoryList::audioDurationLearned,
            this, [this](const QString &mxcUrl, quint64 durationMs) {
        if (_bridge) {
            _bridge->setAudioDuration(mxcUrl, durationMs);
        }
    });
    connect(_list, &HistoryList::audioDownloadRequested,
            this, [this](const QString &mxcUrl, const QString &eventId) {
        _mediaRequests.resumeDownload(mxcUrl);
        _mediaRequests.requestAudioPlay(mxcUrl, eventId);
        quint64 mediaSize = 0;
        if (_list) {
            for (const auto &msg : _list->messages()) {
                if (msg.eventId == eventId) {
                    mediaSize = TeleMatrix::mediaSize(msg);
                    break;
                }
            }
        }
        const auto forceFileFallback = mediaSize > kMaxMemoryAudioBytes;
        if (MediaCache::hasMemoryBlob(mxcUrl) && MediaCache::localPath(mxcUrl).isEmpty()) {
            // Memory playback failed; force the compatibility file fallback.
            if (!MediaCache::isRequested(mxcUrl)) {
                MediaCache::markRequested(mxcUrl);
                _bridge->resolveMedia(mxcUrl);
            }
        } else if (forceFileFallback) {
            if (!MediaCache::isRequested(mxcUrl)) {
                MediaCache::markRequested(mxcUrl);
                _bridge->resolveMedia(mxcUrl);
            }
        } else if (MediaCache::needsResolution(mxcUrl)) {
            MediaCache::markRequested(mxcUrl);
            _bridge->resolveMediaBytes(mxcUrl);
        }
        _list->update();
    });

    connect(_list, &HistoryList::videoDownloadRequested,
            this, [this](const QString &mxcUrl, const QString &eventId) {
        _mediaRequests.resumeDownload(mxcUrl);
        _mediaRequests.requestVideoOpen(mxcUrl, eventId);
        requestResolvedMxcUrl(_bridge, mxcUrl, false, false);
        if (_list) {
            _list->update();
        }
    });

    connect(_list, &HistoryList::mediaExportRequested,
            this, [this](const QString &mxcUrl, const QString &targetPath) {
        if (!_bridge || mxcUrl.isEmpty() || targetPath.isEmpty()) {
            return;
        }
        _bridge->exportMediaToPath(mxcUrl, targetPath);
    });

    connect(_list, &HistoryList::videoLocalThumbnailRequested,
            this, [this](const QString &eventId, const QString &mxcUrl) {
        requestVideoLocalThumbnail(_bridge, eventId, mxcUrl);
    });

    connect(_bridge, &ProtocolBridge::mediaExported,
            this, [this](bool success, const QString & /*mxcUrl*/, const QString & /*targetPath*/) {
        if (_toast) {
            _toast->showToast(success ? tr("File saved.") : tr("Failed to save file."));
        }
    });

    connect(_list, &HistoryList::mediaDownloadCancelRequested,
            this, [this](const QString &mxcUrl) {
        _mediaRequests.cancelDownload(mxcUrl);
        _mediaRequests.clearPendingOpenRequests(mxcUrl);
        _mediaRequests.removeDeferredProgress(mxcUrl);
        MediaCache::clearRequested(mxcUrl);
        MediaCache::clearDownloadState(mxcUrl);
        if (_bridge) {
            _bridge->cancelMediaDownload(mxcUrl);
        }
        if (_list) {
            _list->update();
        }
    });

    connect(_list, &HistoryList::fileOpenRequested,
            this, [this](const QString &mxcUrl, const QString &filename, const QString &mime) {
        openResolvedFile(mxcUrl, filename, mime);
    });

    QObject::connect(_list, &HistoryList::messagesReadTill,
        this, [this](const QString &eventId) {
            if (_currentRoomId.isEmpty()) return;

            applyOptimisticReadProgress(eventId);

            // Send read receipt (debounced via timer).
            // Fix #9: Don't update the confirmed receipt here — the timer
            // handler uses the in-flight receipt to avoid duplicate sends
            // and rolls back on failure.  Just restart the timer.
            if (_readReceipts.canRequest(eventId)) {
                _readReceipts.setPending(eventId);
                if (_readReceiptTimer) {
                    _readReceiptTimer->start();
                }
            }
        });
}

void HistoryWidget::setupPinnedBar() {
    // The always-on pinned-messages bar was removed; pinned messages are now
    // reached via the top-bar pinned button and the room menu (both open the
    // pinned section). `_pinnedBar` stays null — every use of it is guarded — so
    // the bar simply never appears. We still listen for fetched pinned messages so
    // the pinned section view can show fully-resolved bodies.
    connect(_bridge, &ProtocolBridge::pinnedMessagesFetched, this,
        [this](const QVector<TimelineItem> &fetched) {
        // Discard stale responses from a previous room.
        if (!_pinnedState.acceptsFetchForRoom(_currentRoomId)) return;
        if (fetched.isEmpty()) {
            if (_pinnedSectionVisible && _pinnedList) {
                _pinnedList->setLoadingTimeline(false);
            }
            return;
        }
        _pinnedState.setFetchedMessages(fetched);
        // Resolve only media needed for rendering. Video bodies stay lazy.
        for (const auto &msg : _pinnedState.messages()) {
            requestMessageMediaForRendering(_bridge, msg);
        }
        if (_pinnedSectionVisible && _pinnedList) {
            _pinnedList->setLoadingTimeline(false);
            _pinnedList->setMessages(_pinnedState.messages());
            applyPinnedScroll();
        }
    });
}

void HistoryWidget::setupInput() {
    _input = new HistoryInput(_controller, this);
    _input->setSendCallback([this](
            const QString &text,
            const QString &html,
            const QString &replyToEventId) {
        onSendMessage(text, html, replyToEventId);
    });
    connect(_input, &HistoryInput::contentChanged, this, [this](
            const QString &text) {
        if (_currentRoomId.isEmpty() || !_input) {
            return;
        }

        _drafts.updateTextOnly(_currentRoomId, *_input, text);
        scheduleDraftChanged(_currentRoomId);

        // Send typing notification (throttled to once per 5s).
        if (!text.isEmpty() && !_typingState.outgoingSent()) {
            _bridge->sendTypingNotice(_currentRoomId, true);
            _typingState.setOutgoingSent(true);
            _typingSendTimer->start();
        }
        if (!text.isEmpty()) {
            _typingCancelTimer->start();
        } else {
            _typingCancelTimer->stop();
            if (_typingState.outgoingSent()) {
                _bridge->sendTypingNotice(_currentRoomId, false);
                _typingState.setOutgoingSent(false);
            }
        }
    });
    connect(_input, &HistoryInput::editLastMessageRequested, this, [this] {
        const auto &messages = _list->messages();
        for (auto i = messages.size() - 1; i >= 0; --i) {
            const auto &msg = messages[i];
            if (!msg.delivery.outgoing || contentType(msg) != ContentType::Text) {
                continue;
            }
            if (msg.delivery.deleted) {
                continue;
            }
            // A still-sending message is editable too: its eventId is the local
            // transaction id, which the edit path resolves to the local echo.
            // Don't skip it — otherwise up-arrow edits the previous message.
            if (msg.eventId.isEmpty()) {
                continue;
            }
            _input->enterEditMode(msg.eventId, msg.sender.name, bodyText(msg), formattedText(msg));
            return;
        }
    });
    connect(_input, &HistoryInput::editSubmitted, this, [this](
            const QString &eventId,
            const QString &text,
            const QString &html) {
        if (_currentRoomId.isEmpty() || eventId.isEmpty()) {
            return;
        }
        // A media message (image/video/file/audio) keeps its file when edited — clearing the
        // caption text just removes the caption, it must not delete the whole message.
        bool isMedia = false;
        if (_list) {
            for (const auto &msg : _list->messages()) {
                if (msg.eventId == eventId) {
                    isMedia = isMediaCaptionMessage(msg);
                    break;
                }
            }
        }
        if (!isMedia && text.trimmed().isEmpty()) {
            // Empty edit of a text message = delete it (with confirmation).
            HistoryConfirmDialog dialog(
                this,
                QString(),
                tr("Do you want to delete this message?"),
                tr("Delete"),
                QString(),
                HistoryConfirmDialog::Attention);
            if (dialog.exec() == HistoryConfirmDialog::Accepted) {
                if (_list) {
                    _list->markDeleting(eventId);
                }
                _pendingDeleteEventIds.insert(eventId);
                _bridge->deleteMessage(_currentRoomId, eventId);
                _input->cancelEditMode();
            }
            return;
        }
        _bridge->editMessage(
            _currentRoomId, eventId, text, html, /*asMediaCaption=*/isMedia);
    });
    connect(_input, &HistoryInput::editCancelConfirmRequested, this, [this] {
        HistoryConfirmDialog dialog(
            this,
            QString(),
            tr("Cancel editing?"),
            tr("Yes"),
            tr("No"));
        if (dialog.exec() == HistoryConfirmDialog::Accepted) {
            _input->cancelEditMode();
        }
    });

    // Attach popup flow: button click -> popup -> file dialog -> preview -> send.
    connect(_input, &HistoryInput::attachPopupRequested, this, [this] {
        if (_currentRoomId.isEmpty()) {
            return;
        }
        const auto paths = QFileDialog::getOpenFileNames(
            window(),
            tr("Send File"),
            QString(),
            tr("All Files (*)"));
        if (paths.isEmpty()) {
            return;
        }
        const auto files = prepareFiles(paths);
        if (files.isEmpty()) {
            return;
        }
        auto *dialog = new HistorySendFilesDialog(
            window(), files, _controller->settings().sendSubmitWay(),
        _controller->settings().compressImages());
        if (dialog->exec() == QDialog::Accepted) {
            sendDialogFiles(
                dialog->files(), dialog->caption(), dialog->compressImages());
        }
        dialog->deleteLater();
    });

    // Media send now goes straight through sendPreparedFile -> sendPreparedMedia
    // (using the dialog's already-computed metadata), so the bubble appears
    // immediately with no second probe. The old attachmentSelected handler and
    // probeVideoPreviewAsync were removed.
    connect(_input, &HistoryInput::voiceRecorded,
        this, [this](const QString &path, quint64 durationMs, const QByteArray &waveform) {
            if (_currentRoomId.isEmpty() || path.isEmpty()) {
                QFile::remove(path);
                return;
            }

            const QFileInfo info(path);
            const auto transactionId = generateUploadTransactionId();
            _pendingLocalMedia.insert(transactionId, PendingLocalMediaUpload{
                .type = ContentType::Audio,
                .mediaPath = path,
                .uploadPath = path,
            });
            _bridge->sendMedia(
                _currentRoomId,
                ContentType::Audio,
                path,
                QStringLiteral("audio/wav"),
                QStringLiteral("voice-message.wav"),
                QString(),
                QString(),
                static_cast<quint64>(qMax<qint64>(0, info.size())),
                0,
                0,
                durationMs,
                transactionId,
                true,
                waveform);
        });
    connect(_bridge, &ProtocolBridge::messageEdited, this, [this](
            bool success,
            const QString & /*eventId*/) {
        if (success) {
            _input->cancelEditMode();
            loadRoom(_currentRoomId);
        } else if (_toast) {
            _toast->showToast(tr("Failed to edit message."));
        }
    });
    connect(_bridge, &ProtocolBridge::mediaSent, this, [this](
            bool success,
            const QString &eventId) {
        if (success) {
            // mediaSent fires on enqueue. Timeline updates will reconcile the
            // local echo without forcing the viewport to jump.
        } else {
            // Keep the bubble as Failed instead of dropping the message: a direct
            // upload has no send-queue retry, so the user resends manually
            // (right-click -> Resend), which re-runs the upload from the retained
            // params. Echo + pending info are kept until resend/cancel/teardown.
            if (auto it = _optimisticMediaEchoes.find(eventId);
                    it != _optimisticMediaEchoes.end()) {
                it->delivery.sendState = SendState::Failed;
                it->delivery.uploadProgress = -1.0;
            }
            if (_list) {
                _list->updateMessageSendState(eventId, SendState::Failed);
            }
            if (_toast) {
                _toast->showToast(tr("Failed to send media."));
            }
        }
    });

    // Direct uploads report byte progress here (they bypass the send queue, so
    // it doesn't arrive on a timeline item). Apply it to the optimistic echo —
    // both the live row and the stored copy that re-injection re-emits — so the
    // progress arc fills and survives the Full slices that arrive mid-upload.
    connect(_bridge, &ProtocolBridge::uploadProgress, this,
            [this](const QString &transactionId, quint64 current, quint64 total) {
        const double progress = (total > 0)
            ? double(current) / double(total)
            : 0.0;
        if (auto it = _optimisticMediaEchoes.find(transactionId);
                it != _optimisticMediaEchoes.end()) {
            it->delivery.uploadProgress = progress;
        }
        if (_list) {
            _list->updateMessageUploadProgress(transactionId, progress);
        }
    });

    // Re-layout when input field height changes (auto-grow).
    QObject::connect(_input, &HistoryInput::heightChanged,
        this, &HistoryWidget::updateControlsGeometry);

    // Escape with nothing to cancel in input → run widget-level escape chain.
    QObject::connect(_input, &HistoryInput::escapePressed,
        this, &HistoryWidget::escape);
}

void HistoryWidget::setupCornerButtons() {
    // The corner button is a child of _scroll (not this widget), so it stays
    // pinned to the scroll viewport.
    _downButton = new HistoryDownButton(_scroll);
    _downButton->raise();

    _downButtonAnimation = new QVariantAnimation(this);
    _downButtonAnimation->setDuration(kDownButtonDuration);
    _downButtonAnimation->setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(
        _downButtonAnimation,
        &QVariantAnimation::valueChanged,
        this,
        [this](const QVariant &value) {
            _downButtonShownProgress = value.toReal();
            updateCornerButtonPositions();
        });
    QObject::connect(
        _downButtonAnimation,
        &QVariantAnimation::finished,
        this,
        [this] {
            _downButtonShownProgress = _downButtonShown ? 1.0 : 0.0;
            updateCornerButtonPositions();
        });

    _readReceiptTimer = new QTimer(this);
    _readReceiptTimer->setSingleShot(true);
    _readReceiptTimer->setInterval(300);
    QObject::connect(_readReceiptTimer, &QTimer::timeout, this, [this] {
        if (_currentRoomId.isEmpty() || !_list) return;

        const auto eventId = _readReceipts.pendingEventId();

        // Fix #9: Skip if already confirmed OR if the same event is in-flight.
        // This allows a retry when the previous send failed and the in-flight
        // receipt was cleared.
        if (!_readReceipts.canRequest(eventId)) {
            return;
        }

        // Mark in-flight BEFORE sending — do NOT update the confirmed
        // receipt yet. Only promote it on readReceiptSent(true).
        sendTrackedReadReceipt(eventId);

        // Clear the unread bar position. Don't zero the badge count here —
        // the optimistic unread state was already pushed by messagesReadTill.
        _firstUnreadIndex = -1;
    });

    // Fix #9: Consume readReceiptSent to confirm or roll back the in-flight
    // receipt. On success, promote the in-flight receipt to confirmed.
    // On failure, clear the in-flight receipt so
    // the next scroll event retries for the same frontier.
    QObject::connect(_bridge, &ProtocolBridge::readReceiptSent, this,
        [this](quint64 requestId, const QString &roomId, const QString &eventId, bool success) {
            if (_unreadStateStore && !roomId.isEmpty()) {
                _unreadStateStore->ackReadReceipt(roomId, eventId, requestId, success);
            }
            if (!_readReceipts.isRequested(eventId)) {
                return;
            }
            if (success) {
                _readReceipts.confirmRequested(eventId);
                return;
            }
            // On failure or success: clear in-flight marker so the timer can
            // re-evaluate on next tick.
            _readReceipts.clearRequested();
        });

    QObject::connect(_downButton, &HistoryDownButton::clicked,
        this, [this] {
            // Preview has no live tail to return to (no Rust window) and no return stack — just
            // jump to the newest loaded messages.
            if (_previewMode) {
                if (_scroll) {
                    _scroll->scrollToY(_scroll->scrollTopMax());
                }
                return;
            }

            checkReturnStack();

            // Priority 1: Reply-return stack — pop and go back.
            if (_returnStack.hasReply()) {
                popReplyReturn();
                return;
            }

            // Priority 2: Generic viewport return stack.
            if (_returnStack.hasPosition()) {
                popReturnPosition();
                return;
            }

            // Priority 3 (non-live): Return to live tail.
            if (_list && !_list->isLive()) {
                _scrollToBottomPending = true;
                returnToLive();
                return;
            }

            // Priority 4: Scroll to bottom + clear unreads.
            // The unread bar is deliberately NOT a stop on the way: the button
            // points at the newest message, and taking two clicks to reach it
            // when the bar happens to be above is not what it promises. The bar
            // is still there to scroll back to by hand.
            _scroll->scrollToY(_scroll->scrollTopMax());
            destroyUnreadBar();
            _preserveUnreadBarOnEntry = false;
            _unreadBarDismissed = false;
            QString lastEventId;
            if (!_currentRoomId.isEmpty() && _list
                && !_list->messages().isEmpty()) {
                lastEventId = _list->messages().last().eventId;
            }
            _firstUnreadIndex = -1;
            _readReceipts.clearPending();
            _optimisticUnreadFrontierEventId.clear();
            _optimisticReadTillEventId = lastEventId;
            updateUnreadCount(0);
            syncReadMarkingMode();
            if (_readReceipts.canRequest(lastEventId)) {
                // Fix #9: Don't advance the confirmed receipt optimistically.
                _readReceipts.clearPending();
                sendTrackedReadReceipt(lastEventId);
            }
            updateCornerButtonPositions();
        });

    // Scroll handling re-evaluates whether the down button should show, then
    // repositions the corner buttons.
    QObject::connect(
        _scroll->verticalScrollBar(), &QScrollBar::valueChanged,
        this, [this] {
            updateCornerButtonPositions();
            if (_readReceiptTimer && !_currentRoomId.isEmpty()) {
                _readReceiptTimer->start();
            }
        });
    QObject::connect(
        _scroll->verticalScrollBar(), &QScrollBar::rangeChanged,
        this, [this] {
            updateCornerButtonPositions();
        });
}

bool HistoryWidget::cornerButtonsDownShown() const {
    // Always show when return stack is non-empty.
    if (_returnStack.hasPosition()) {
        return true;
    }

    // Always show the button when in non-live (Event/focused) mode so the
    // user can return to the live tail of the timeline. A preview is ALWAYS non-live (a static
    // snapshot with no live tail), so exclude it — it follows plain scroll position below instead,
    // appearing only when scrolled up.
    if (_list && !_list->isLive() && !_previewMode) {
        return true;
    }

    const auto top = _scroll->scrollTop();
    const auto max = _scroll->scrollTopMax();
    if (max <= 0) {
        return false;
    }
    // A fixed 480px threshold assumes very long message lists. Use the minimum
    // of that threshold and a fraction of the available range so the button
    // works with shorter lists.
    const auto threshold = qMin(kScrollThreshold, max / 3);
    if (top + threshold < max) {
        return true;
    }
    // Also show when there are unread messages and user is not at bottom.
    if (_roomUnreadCount > 0 && top < max - 1) {
        return true;
    }
    return false;
}

void HistoryWidget::updateCornerButtonPositions() {
    checkReturnStack();
    const auto shown = cornerButtonsDownShown();

    if (shown != _downButtonShown) {
        _downButtonShown = shown;
        if (_downButtonAnimation) {
            if (shown && _downButton->isHidden()) {
                _downButton->show();
            }
            _downButtonAnimation->stop();
            _downButtonAnimation->setStartValue(_downButtonShownProgress);
            _downButtonAnimation->setEndValue(shown ? 1.0 : 0.0);
            _downButtonAnimation->start();
        } else {
            _downButtonShownProgress = shown ? 1.0 : 0.0;
        }
    }

    const auto fullTop = _downButton->height() + kDownButtonBottom;
    const auto top = qRound(fullTop * _downButtonShownProgress);
    const auto x = _scroll->width()
        - kDownButtonRight
        - _downButton->width();
    const auto y = _scroll->height() - top;
    _downButton->move(x, y);
    _downButton->raise();

    // Hide only when fully animated out.
    const auto animating = _downButtonAnimation
        && _downButtonAnimation->state() == QAbstractAnimation::Running;
    const auto shouldBeHidden = !_downButtonShown && !animating && _downButtonShownProgress <= 0.0;
    if (shouldBeHidden != _downButton->isHidden()) {
        _downButton->setVisible(!shouldBeHidden);
    }

    // Update arrow direction: up-arrow when in non-live (jump-to-latest) mode,
    // down-arrow when in live scroll-to-bottom mode.
    const auto jumpToLatest = _list && !_list->isLive();
    _downButton->setJumpToLatestMode(jumpToLatest);
}

void HistoryWidget::setupPlaceholder() {
    _noChat = new NoChatPlaceholder(this);
    _noChat->setBackgroundDoodlesEnabled(_controller->settings().backgroundDoodles());

    // Feed theme background to placeholder.
    if (auto *tm = _controller->themeManager()) {
        _noChat->invalidateBackground();
        connect(tm, &Theme::ThemeManager::themeChanged,
                _noChat, [this](bool /*isNight*/, Theme::ThemeMode /*mode*/) {
            if (_noChat) {
                _noChat->invalidateBackground();
            }
        });
    }

    // Apply current sync state (signal may have fired before we connected).
    const auto applySyncState = [this](int state) {
        _synced = (state == 2);
        _noChat->setSyncing(!_synced);
        if (_list) {
            _list->setSyncing(!_synced);
        }
    };
    applySyncState(_bridge->syncState());

    // Track future sync state changes.
    QObject::connect(_bridge, &ProtocolBridge::syncStateChanged,
        this, applySyncState);
}

void HistoryWidget::escape() {
    // Escape handling follows a priority chain.
    if (_list && _list->inSelectionMode()) {
        _list->exitSelectionMode();
    } else if (_input && _input->isInEditMode()) {
        _input->cancelEditMode();
    } else if (_input && _input->isInReplyMode()) {
        _input->cancelReplyMode();
    } else if (_pinnedSectionVisible) {
        closePinnedSection();
    } else {
        emit cancelRequests();
    }
}

void HistoryWidget::showChatControls(bool show) {
    _topBar->setVisible(show);
    if (_topBarShadow) {
        _topBarShadow->setVisible(show);
    }
    _scroll->setVisible(show);

    if (show) {
        animatePinnedBarVisibility(_pinnedBar && _pinnedBar->hasPinnedMessage());
        animateInputVisibility(true);
    } else {
        if (_inputVisibilityAnimation) {
            _inputVisibilityAnimation->stop();
        }
        _inputShownProgress = 0.0;
        _input->hide();
        if (_pinnedBarAnimation) {
            _pinnedBarAnimation->stop();
        }
        _pinnedBarShownProgress = 0.0;
        if (_pinnedBar) {
            _pinnedBar->hide();
        }
    }
    _noChat->setVisible(!show);
}

void HistoryWidget::showInvitePanel(const RoomSummary &room) {
    _isInvitedRoom = true;

    // Clear old layout.
    if (auto *old = _invitePanel->layout()) {
        QLayoutItem *item;
        while ((item = old->takeAt(0)) != nullptr) {
            delete item->widget();
            delete item;
        }
        delete old;
    }

    // ─── Show top bar + invite info in timeline area
    //     + full-width Accept/Decline buttons at bottom (replaces compose) ───

    // Set up top bar with room/inviter info.
    _currentRoomId = room.roomId;
    if (_topBar) {
        const auto subtitle = room.inviterDisplayName.isEmpty()
            ? tr("Invitation")
            : tr("Invited by %1").arg(room.inviterDisplayName);
        _topBar->setRoomInfo(room.displayName, subtitle);
        _topBar->setEncrypted(false);
        _topBar->setMuted(!isSavedMessagesRoom()
            && isToolbarMuted(room.notificationMode, room.isMuted));
    }

    // Show controls: top bar visible, scroll area visible (empty), input hidden.
    if (_topBar) _topBar->show();
    if (_topBarShadow) _topBarShadow->show();
    _scroll->show();
    _input->hide();
    if (_pinnedBar) _pinnedBar->hide();
    _noChat->hide();

    // Clear timeline and show invite info centered in the scroll area.
    _list->setRoomId(room.roomId);
    _list->setMessages({});

    // Build the invite panel as a child of this widget, positioned
    // below the top bar and above the button bar.
    _invitePanel->setAutoFillBackground(false);

    auto *layout = new QVBoxLayout(_invitePanel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Scroll area so a long room topic scrolls instead of being clipped by the fixed panel region.
    // Kept transparent so the chat background shows through; the card sits centred inside and only
    // scrolls once it is taller than the available space.
    auto *scroll = new QScrollArea(_invitePanel);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setWidgetResizable(true);
    scroll->setAutoFillBackground(false);
    if (auto *vp = scroll->viewport()) {
        vp->setAutoFillBackground(false);
        vp->setAttribute(Qt::WA_TranslucentBackground);
    }

    auto *scrollInner = new QWidget();
    scrollInner->setAutoFillBackground(false);
    scrollInner->setAttribute(Qt::WA_TranslucentBackground);
    auto *innerLayout = new QVBoxLayout(scrollInner);
    innerLayout->setContentsMargins(0, 0, 0, 0);
    innerLayout->setSpacing(0);
    innerLayout->addStretch(1);

    // ─── White card for readability over gradient background ───
    auto *card = new RoundedPanel(scrollInner);
    card->setFixedWidth(qMin(360, width() - 80));
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(24, 20, 24, 20);
    cardLayout->setSpacing(0);

    // ─── Avatar (64px, centered) ───
    constexpr int kAvatarSize = 64;
    {
        const auto &avatarUrl = (room.isDirect && !room.inviterAvatarUrl.isEmpty())
            ? room.inviterAvatarUrl : room.avatarUrl;
        const auto &entityId = (room.isDirect && !room.inviterUserId.isEmpty())
            ? room.inviterUserId : room.avatarEntityId;
        const auto &name = (room.isDirect && !room.inviterDisplayName.isEmpty())
            ? room.inviterDisplayName : room.displayName;

        const auto dpr = devicePixelRatioF();
        QPixmap avatarPix(QSize(kAvatarSize, kAvatarSize) * dpr);
        avatarPix.setDevicePixelRatio(dpr);
        avatarPix.fill(Qt::transparent);
        {
            QPainter ap(&avatarPix);
            ap.setRenderHint(QPainter::Antialiasing);
            if (!avatarUrl.isEmpty()) {
                const auto loaded = MediaCache::loadAvatarPixmap(avatarUrl, kAvatarSize, dpr);
                if (!loaded.isNull()) ap.drawPixmap(0, 0, loaded);
                else Ui::EmptyUserpic::paint(ap, entityId, name, 0, 0, kAvatarSize);
            } else {
                Ui::EmptyUserpic::paint(ap, entityId, name, 0, 0, kAvatarSize);
            }
        }
        auto *avatarLabel = new QLabel(card);
        avatarLabel->setPixmap(avatarPix);
        avatarLabel->setFixedSize(kAvatarSize, kAvatarSize);
        avatarLabel->setAlignment(Qt::AlignCenter);
        auto *avatarRow = new QHBoxLayout();
        avatarRow->addStretch(1);
        avatarRow->addWidget(avatarLabel);
        avatarRow->addStretch(1);
        cardLayout->addLayout(avatarRow);
    }

    cardLayout->addSpacing(14);

    // ─── "<name> wants to chat" ───
    const auto displayName = room.inviterDisplayName.isEmpty()
        ? room.displayName : room.inviterDisplayName;
    auto *titleLabel = new QLabel(
        tr("%1 wants to chat").arg(displayName), card);
    titleLabel->setFont(st::baseFont(15, true));
    {
        QPalette pal = titleLabel->palette();
        pal.setColor(QPalette::WindowText, st::boxTitleFg);
        titleLabel->setPalette(pal);
    }
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setWordWrap(true);
    cardLayout->addWidget(titleLabel);

    cardLayout->addSpacing(4);

    // ─── "@user:server" ───
    const auto userId = room.inviterUserId.isEmpty()
        ? room.roomId : room.inviterUserId;
    auto *userIdLabel = new QLabel(userId, card);
    userIdLabel->setFont(st::baseFont(13));
    {
        QPalette pal = userIdLabel->palette();
        pal.setColor(QPalette::WindowText, st::windowSubTextFg);
        userIdLabel->setPalette(pal);
    }
    userIdLabel->setAlignment(Qt::AlignCenter);
    userIdLabel->setWordWrap(true);
    cardLayout->addWidget(userIdLabel);

    if (!room.roomTopic.isEmpty()) {
        cardLayout->addSpacing(8);
        auto *topicLabel = new QLabel(room.roomTopic, card);
        topicLabel->setFont(st::baseFont(13));
        {
            QPalette pal = topicLabel->palette();
            pal.setColor(QPalette::WindowText, st::windowSubTextFg);
            topicLabel->setPalette(pal);
        }
        topicLabel->setAlignment(Qt::AlignCenter);
        topicLabel->setWordWrap(true);
        cardLayout->addWidget(topicLabel);
    }

    innerLayout->addWidget(card, 0, Qt::AlignHCenter);
    innerLayout->addStretch(1);
    scroll->setWidget(scrollInner);
    layout->addWidget(scroll, 1);

    // ─── Full-width Accept/Decline buttons at the bottom ───
    // (compose-button style, replaces compose area)
    auto *btnBar = new QWidget(_invitePanel);
    btnBar->setFixedHeight(46); // px, compose-button bar height
    btnBar->setAutoFillBackground(true);
    {
        QPalette pal = btnBar->palette();
        pal.setColor(QPalette::Window, st::windowBg);
        btnBar->setPalette(pal);
    }
    auto *btnLayout = new QHBoxLayout(btnBar);
    btnLayout->setContentsMargins(0, 0, 0, 0);
    btnLayout->setSpacing(0);

    auto inviteButtonFont = st::baseFont(14);
    inviteButtonFont.setWeight(QFont::DemiBold);

    ::Ui::TextButton::Style declineStyle;
    declineStyle.bgOver = &st::windowBgOver;  // transparent until hovered
    declineStyle.fg = &st::attentionButtonFg;
    auto *declineBtn = new ::Ui::TextButton(tr("DECLINE"), declineStyle, btnBar);
    declineBtn->setFont(inviteButtonFont);
    declineBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    ::Ui::TextButton::Style acceptStyle;
    acceptStyle.bgOver = &st::windowBgOver;  // transparent until hovered
    acceptStyle.fg = &st::windowActiveTextFg;
    auto *acceptBtn = new ::Ui::TextButton(tr("ACCEPT"), acceptStyle, btnBar);
    acceptBtn->setFont(inviteButtonFont);
    acceptBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    btnLayout->addWidget(declineBtn);
    btnLayout->addWidget(acceptBtn);
    layout->addWidget(btnBar);

    // ─── Wire up buttons ───
    const auto roomId = room.roomId;
    connect(acceptBtn, &QAbstractButton::clicked, this, [this, roomId, acceptBtn, declineBtn] {
        acceptBtn->setEnabled(false);
        declineBtn->setEnabled(false);
        _bridge->acceptInvite(roomId);
    });
    connect(declineBtn, &QAbstractButton::clicked, this, [this, roomId, acceptBtn, declineBtn] {
        acceptBtn->setEnabled(false);
        declineBtn->setEnabled(false);
        _bridge->leaveRoom(roomId);
    });

    // Position the panel over the scroll area (between top bar and bottom).
    updateControlsGeometry();
    const auto topBarH = _topBar ? _topBar->height() : 0;
    const auto shadowH = _topBarShadow ? _topBarShadow->height() : 0;
    const auto panelTop = topBarH + shadowH;
    _invitePanel->setGeometry(0, panelTop, width(), height() - panelTop);
    _invitePanel->show();
    _invitePanel->raise();
}

void HistoryWidget::hideInvitePanel() {
    _isInvitedRoom = false;
    if (_invitePanel) {
        _invitePanel->hide();
    }
}

void HistoryWidget::setupJoinBar() {
    if (_joinBar) {
        return;
    }

    _joinBar = new QWidget(this);
    _joinBar->setAutoFillBackground(true);
    {
        QPalette pal = _joinBar->palette();
        pal.setColor(QPalette::Window, st::windowBg);
        _joinBar->setPalette(pal);
    }
    _joinBar->hide();

    auto *layout = new QVBoxLayout(_joinBar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    _joinError = new QLabel(_joinBar);
    _joinError->setAlignment(Qt::AlignCenter);
    _joinError->setWordWrap(true);
    _joinError->setFont(st::normalFont);
    _joinError->setContentsMargins(16, 4, 16, 0);
    {
        QPalette pal = _joinError->palette();
        pal.setColor(QPalette::WindowText, st::attentionButtonFg);
        _joinError->setPalette(pal);
    }
    _joinError->hide();
    layout->addWidget(_joinError);

    auto joinFont = st::baseFont(14);
    joinFont.setWeight(QFont::DemiBold);

    ::Ui::TextButton::Style joinStyle;
    joinStyle.bgOver = &st::windowBgOver;
    joinStyle.fg = &st::windowActiveTextFg;
    _joinButton = new ::Ui::TextButton(tr("JOIN"), joinStyle, _joinBar);
    _joinButton->setFont(joinFont);
    _joinButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    _joinButton->setFixedHeight(kJoinBarButtonHeight);
    layout->addWidget(_joinButton);

    connect(_joinButton, &QAbstractButton::clicked, this, [this] {
        if (!_bridge || _previewRoomIdOrAlias.isEmpty()) {
            return;
        }
        _joinError->hide();
        _joinButton->setEnabled(false);
        // Knock-only rooms reject a plain join, so "Ask to join" must actually knock.
        const auto knockOnly =
            _previewInfo.joinRule == RoomDirectoryJoinRule::Knock
            || _previewInfo.joinRule == RoomDirectoryJoinRule::KnockRestricted;
        if (knockOnly) {
            _joinButton->setText(tr("ASKING..."));
            _bridge->knockRoom(_previewRoomIdOrAlias, _previewVia);
        } else {
            _joinButton->setText(tr("JOINING..."));
            _bridge->joinRoom(_previewRoomIdOrAlias, _previewVia);
        }
    });
}

void HistoryWidget::loadRoomPreview(
    const QString &roomIdOrAlias,
    const QStringList &via) {
    if (!_bridge || roomIdOrAlias.isEmpty()) {
        return;
    }

    setupJoinBar();

    // Remember where the user came from: a preview that fails to load returns
    // there instead of stranding them on a dead screen. Chained previews keep
    // the original joined room.
    if (!_previewMode) {
        _previewReturnRoomId = _currentRoomId;
    }
    _previewMode = true;
    _previewRoomIdOrAlias = roomIdOrAlias;
    _previewResolvedRoomId.clear();
    _previewNextToken.clear();
    _previewReachedStart = false;
    _previewLoadingMore = false;
    _previewEmptyPageStreak = 0;
    _previewVia = via;
    _currentRoomId = roomIdOrAlias;

    hideInvitePanel();
    _noChat->hide();
    if (_topBar) _topBar->show();
    if (_topBarShadow) _topBarShadow->show();
    _scroll->show();
    if (_pinnedBar) _pinnedBar->hide();

    // The composer must not merely be covered — it is laid out, and a hidden-but-present input
    // would still reserve height and take focus.
    animateInputVisibility(false);

    // Deliberately NOT watchTimeline/getTimelineSliceAsync: the room is unknown to the SDK, so no
    // slice would ever arrive and the list would spin on "Loading…" forever. The id itself is
    // still set — link routing and same-room jump detection need it.
    _list->setRoomId(roomIdOrAlias);
    _list->setMessages({});
    _list->setSyncing(false);
    // Show only a centred "Loading…" pill until we know the outcome: readable → timeline, otherwise
    // → name+description placeholder. updatePreviewDisplay() drives the transitions. Keeping the
    // preview name empty here makes the pill (not the placeholder) show first.
    _list->setLoadingTimeline(true);
    // The whole preview is read-only; if world-readable history arrives it renders read-only too.
    _list->setReadOnly(true);
    _list->setPreviewInfo(QString(), QString());
    _previewSummaryReady = false;
    _previewMessagesDone = false;

    if (_topBar) {
        _topBar->setRoomInfo(roomIdOrAlias, tr("Loading..."));
        _topBar->setEncrypted(false);
        _topBar->setMuted(false);
        _topBar->setHasPinned(false);
        // Not our room — hide the search / menu buttons entirely.
        _topBar->setChromeButtonsVisible(false);
    }

    _joinButton->setEnabled(true);
    _joinButton->setText(tr("JOIN"));
    _joinError->hide();
    _joinBar->show();
    _joinBar->raise();

    updateControlsGeometry();

    // Fetch the room summary (name/topic/join-rule) and the history in PARALLEL. The summary can be
    // slow (three federated fallbacks), so gating history on it — as before — was a big part of the
    // "stuck loading" reports. The explore box always hands us a real room id, so the peek can start
    // immediately; onPreviewMessagesReady matches on `_previewResolvedRoomId`, set here.
    _previewResolvedRoomId = roomIdOrAlias;
    _bridge->getRoomPreview(roomIdOrAlias, via);
    _bridge->previewMessages(roomIdOrAlias, QString(), kPreviewMessageCount);

    // The composer is hidden in preview, so there is no natural input to focus. Put focus on the
    // read-only timeline (keyboard scrolling) so it doesn't fall back to the chat search field.
    if (_scroll) {
        _scroll->setFocus(Qt::OtherFocusReason);
    }
}

void HistoryWidget::exitPreviewMode() {
    _previewMode = false;
    _previewRoomIdOrAlias.clear();
    _previewResolvedRoomId.clear();
    _previewNextToken.clear();
    _previewReachedStart = false;
    _previewLoadingMore = false;
    _previewEmptyPageStreak = 0;
    _previewSummaryReady = false;
    _previewMessagesDone = false;
    _previewVia.clear();
    _previewInfo = RoomPreviewInfo();
    if (_joinBar) {
        _joinBar->hide();
    }
    if (_topBar) {
        // Restore the search / menu buttons for the joined room we're switching to.
        _topBar->setChromeButtonsVisible(true);
    }
    if (_list) {
        _list->setReadOnly(false);
        _list->setPreviewInfo(QString(), QString());
        _list->setLoadingTimeline(false);
    }
}

void HistoryWidget::onPreviewReady(
    const QString &roomIdOrAlias,
    bool success,
    const RoomPreviewInfo &preview,
    const QString &error) {
    if (!_previewMode || roomIdOrAlias != _previewRoomIdOrAlias) {
        return;
    }

    // The room summary resolved (or failed). We now have the name+description for the placeholder,
    // but it is only shown once the peek confirms there is no readable history (see
    // updatePreviewDisplay) — until then the centred "Loading…" pill stays.
    _previewSummaryReady = true;

    if (!success) {
        // Don't strand the user in a dead preview screen: restore where they
        // were (or the empty state) and offer the browser from there.
        const auto failedTarget = _previewRoomIdOrAlias;
        const auto failedVia = _previewVia;
        const auto returnRoom = _previewReturnRoomId;
        exitPreviewMode();
        if (!returnRoom.isEmpty() && isJoinedRoom(returnRoom)) {
            showChatControls(true);
            loadRoom(returnRoom);
            emit roomSwitchRequested(returnRoom);
        } else {
            _currentRoomId.clear();
            showChatControls(false);
        }
        offerBrowserFallback(failedTarget, failedVia);
        return;
    }

    if (_topBar) {
        _topBar->setRoomInfo(
            preview.name,
            tr("%n member(s)", nullptr, preview.memberCount));
    }

    // A knock-only room cannot be joined outright — asking is the only way in.
    const auto knockOnly =
        preview.joinRule == RoomDirectoryJoinRule::Knock
        || preview.joinRule == RoomDirectoryJoinRule::KnockRestricted;
    _joinButton->setText(knockOnly ? tr("ASK TO JOIN") : tr("JOIN"));

    _previewInfo = preview;
    // History was already requested in parallel from loadRoomPreview.
    updatePreviewDisplay();
}

void HistoryWidget::loadMorePreviewMessages() {
    if (!_previewMode || _previewLoadingMore || _previewReachedStart
        || _previewNextToken.isEmpty() || _previewResolvedRoomId.isEmpty() || !_bridge) {
        return;
    }
    _previewLoadingMore = true;
    _bridge->previewMessages(
        _previewResolvedRoomId, _previewNextToken, kPreviewMessageCount);
}

void HistoryWidget::onPreviewMessagesReady(
    const QString &roomId,
    bool success,
    const QVector<TimelineItem> &items,
    const QString &nextToken,
    const QString &error) {
    Q_UNUSED(error);
    // Ignore a stale result from a preview the user already left.
    if (!_previewMode || roomId != _previewResolvedRoomId) {
        return;
    }

    const bool wasLoadingMore = _previewLoadingMore;
    _previewLoadingMore = false;

    // A failure means the room isn't readable without joining (403) or the peek broke. Stop, and let
    // updatePreviewDisplay swap the pill for the name+description placeholder (once the summary is in).
    if (!success) {
        _previewReachedStart = true;
        _previewMessagesDone = true;
        updatePreviewDisplay();
        return;
    }

    // Reached-start is ONLY the server's empty-chunk signal (Rust returns an empty token there). A
    // page that maps to zero displayable items but carries a token is NOT the end — big rooms return
    // whole pages of joins/leaves/topic changes that all filter out, with real messages behind them.
    _previewNextToken = nextToken;
    _previewReachedStart = nextToken.isEmpty();

    if (!items.isEmpty()) {
        // Keep the list's room id empty (as loadRoomPreview set it): we are not a member, so no poll
        // voting / read receipts / real pagination should activate. Media still resolves by mxc.
        _list->setReadOnly(true);

        // Route through setSlice so the first page sets a non-live baseline and each older page
        // prepends with automatic scroll-position preservation (same primitive the joined timeline
        // uses). A Prepend when the list is already populated keeps the viewport anchored.
        TimelineSlice slice;
        slice.items = items;
        slice.isLive = false;
        slice.canPaginateBack = !_previewReachedStart;
        slice.canPaginateForward = false;
        slice.hitTimelineStart = _previewReachedStart;
        slice.unreadStateKnown = false;
        if (wasLoadingMore && !_list->messages().isEmpty()) {
            slice.updateKind = TimelineUpdateKind::Prepend;
            slice.updateIndex = 0;
        } else {
            slice.updateKind = TimelineUpdateKind::Full;
        }
        _list->setSlice(slice);
        // The placeholder hides itself once the message list is non-empty (see HistoryList paint).

        // Kick off image / thumbnail / avatar resolution for the previewed history. Media resolves
        // by mxc, independent of room membership.
        if (_bridge) {
            for (const auto &item : items) {
                requestMessageMediaForRendering(_bridge, item);
            }
        }
    }

    const bool haveMessages = _list && !_list->messages().isEmpty();

    // A page that added a displayable message means progress — reset the empty-page streak. A page
    // that mapped to nothing (pure bridge join/leave churn) advances it toward the cap.
    if (!items.isEmpty()) {
        _previewEmptyPageStreak = 0;
    } else if (wasLoadingMore) {
        ++_previewEmptyPageStreak;
    }

    // Keep pulling older pages (through all-churn pages too) until the viewport fills or the streak
    // caps out. Deferred so the just-applied slice has laid out before we measure the scroll range.
    const bool exhausted =
        _previewReachedStart || _previewEmptyPageStreak >= kPreviewMaxEmptyPages;
    if (!exhausted) {
        QTimer::singleShot(0, this, [this] { maybeContinuePreviewPagination(); });
    } else if (!haveMessages) {
        // Stopped with nothing on screen (a readable room with no messages, or an all-churn room we
        // gave up on) — treat like "not readable" and show the placeholder.
        _previewMessagesDone = true;
    }

    updatePreviewDisplay();
}

void HistoryWidget::updatePreviewDisplay() {
    if (!_previewMode || !_list) {
        return;
    }
    const bool hasMessages = !_list->messages().isEmpty();
    if (hasMessages) {
        // Readable → timeline. No pill, no placeholder (the top-of-timeline scrollback pill is
        // separate and driven by canPaginateBack).
        _list->setLoadingTimeline(false);
        _list->setPreviewInfo(QString(), QString());
    } else if (_previewMessagesDone && _previewSummaryReady) {
        // Confirmed not readable / no history → name+description placeholder, no loading pill.
        _list->setLoadingTimeline(false);
        _list->setPreviewInfo(_previewInfo.name, _previewInfo.topic);
    } else {
        // Still determining (peek in flight, or summary not yet in) → just the centred "Loading…"
        // pill. Keeping the preview name empty makes the pill show instead of the placeholder.
        _list->setPreviewInfo(QString(), QString());
        _list->setLoadingTimeline(true);
    }
}

void HistoryWidget::maybeContinuePreviewPagination() {
    if (!_previewMode || _previewLoadingMore || _previewReachedStart
        || _previewEmptyPageStreak >= kPreviewMaxEmptyPages) {
        return;
    }
    // Nothing on screen yet — the pages so far were all churn; keep going regardless of scroll.
    if (!_list || _list->messages().isEmpty()) {
        loadMorePreviewMessages();
        return;
    }
    // Otherwise only keep auto-loading while the timeline is too short to scroll (viewport not yet
    // filled) or the user is already near the top. Beyond that, a manual scroll drives further
    // pagination via checkPaginationThresholds.
    const int scrollTop = _scroll->scrollTop();
    const int viewportHeight = _scroll->viewport()->height();
    const int scrollMax = _scroll->verticalScrollBar()->maximum();
    if (scrollMax <= 0 || scrollTop < 2 * viewportHeight) {
        loadMorePreviewMessages();
    }
}

void HistoryWidget::onJoinResult(
    const QString &roomIdOrAlias,
    bool success,
    const QString &roomId,
    const QString &error) {
    if (!_previewMode || roomIdOrAlias != _previewRoomIdOrAlias) {
        return;
    }

    if (!success) {
        // The directory's join rule can be stale, so a refusal here is normal and the server's own
        // words are the most useful thing we can show.
        _joinButton->setEnabled(true);
        _joinButton->setText(tr("JOIN"));
        _joinError->setText(error.isEmpty() ? tr("Couldn't join this room.") : error);
        _joinError->show();
        updateControlsGeometry();
        return;
    }

    // The rooms list picks the new room up from roomListChanged, exactly as it does after an invite
    // is accepted — no manual refresh needed.
    exitPreviewMode();
    showChatControls(true);
    loadRoom(roomId);
}

void HistoryWidget::onKnockResult(
    const QString &roomIdOrAlias,
    bool success,
    const QString &roomId,
    const QString &error) {
    Q_UNUSED(roomId);
    if (!_previewMode || roomIdOrAlias != _previewRoomIdOrAlias) {
        return;
    }

    if (!success) {
        _joinButton->setEnabled(true);
        _joinButton->setText(tr("ASK TO JOIN"));
        _joinError->setText(error.isEmpty() ? tr("Couldn't send the request.") : error);
        _joinError->show();
        updateControlsGeometry();
        return;
    }

    // Knock sent — membership is pending, so we are NOT joined and stay in the preview (any
    // world-readable history remains readable). Reflect the pending state on the bar.
    _joinButton->setEnabled(false);
    _joinButton->setText(tr("REQUEST SENT"));
    _joinError->hide();
    updateControlsGeometry();
}

bool HistoryWidget::inputHasFocus() const {
    if (!_input) {
        return false;
    }
    const auto focused = QApplication::focusWidget();
    return focused && (focused == _input || _input->isAncestorOf(focused));
}

void HistoryWidget::animateInputVisibility(bool visible) {
    if (!_input) {
        return;
    }
    const auto target = visible ? 1.0 : 0.0;
    if (!_inputVisibilityAnimation) {
        _inputVisibilityAnimation = new QVariantAnimation(this);
        _inputVisibilityAnimation->setDuration(kInputSlideDuration);
        _inputVisibilityAnimation->setEasingCurve(QEasingCurve::OutCubic);
        connect(_inputVisibilityAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            _inputShownProgress = value.toReal();
            updateControlsGeometry();
        });
        connect(_inputVisibilityAnimation, &QVariantAnimation::finished, this, [this] {
            if (_inputShownProgress <= 0.0 && _input) {
                _input->hide();
            }
        });
    }
    if (visible && _input->isHidden()) {
        _input->show();
    }
    _inputVisibilityAnimation->stop();
    _inputVisibilityAnimation->setStartValue(_inputShownProgress);
    _inputVisibilityAnimation->setEndValue(target);
    _inputVisibilityAnimation->start();
}

void HistoryWidget::animatePinnedBarVisibility(bool visible) {
    if (!_pinnedBar) {
        return;
    }
    if (_pinnedBarAnimation) {
        _pinnedBarAnimation->stop();
    }

    const auto target = (visible && _pinnedBar->hasPinnedMessage()) ? 1.0 : 0.0;
    _pinnedBarShownProgress = target;

    if (target > 0.0) {
        if (_pinnedBar->isHidden()) {
            _pinnedBar->show();
        }
    } else {
        _pinnedBar->hide();
    }

    updateControlsGeometry();
}

void HistoryWidget::scheduleDraftChanged(const QString &roomId) {
    if (roomId.isEmpty()) {
        return;
    }

    _pendingDraftRoomId = roomId;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    if (_draftChangeBurstStarted <= 0) {
        _draftChangeBurstStarted = now;
    } else if ((now - _draftChangeBurstStarted) >= kDraftDebounceMaxMs) {
        flushDraftChanged();
        _draftChangeBurstStarted = now;
    }
    if (_draftChangedTimer) {
        _draftChangedTimer->start();
    }
}

void HistoryWidget::flushDraftChanged() {
    if (_pendingDraftRoomId.isEmpty()) {
        return;
    }
    if (_draftChangedTimer && _draftChangedTimer->isActive()) {
        _draftChangedTimer->stop();
    }
    const auto preview = HistoryDraftStore::preview(
        _drafts.value(_pendingDraftRoomId));
    emit draftChanged(_pendingDraftRoomId, preview);
    _pendingDraftRoomId.clear();
    _draftChangeBurstStarted = 0;
}

void HistoryWidget::openPinnedMessages() {
    // Entry point for the top-bar pinned button and the room menu option.
    if (_pinnedState.isEmpty()) {
        if (_toast) {
            _toast->showToast(tr("No pinned messages"));
        }
        return;
    }
    showPinnedSection();
}

void HistoryWidget::showPinnedSection() {
    if (_pinnedState.isEmpty() || _pinnedSectionVisible) {
        return;
    }
    _pinnedSectionVisible = true;

    // Create the pinned section scroll area + list on first use. The pinned
    // section REUSES the real timeline widget (HistoryList) in "pinned mode" so
    // bubbles, styles and interactions are identical to the main timeline.
    if (!_pinnedScroll) {
        _pinnedScroll = new Ui::ScrollArea(this);
        _pinnedList = new HistoryList(_pinnedScroll);
        _pinnedList->setPinnedMode(true);
        _pinnedScroll->setOwnedWidget(
            object_ptr<HistoryList>::fromRaw(_pinnedList));
        // Local video-thumbnail extraction request (bridge/FFmpeg), same hook
        // the main list uses via the videoLocalThumbnailRequested signal.
        connect(_pinnedList, &HistoryList::videoLocalThumbnailRequested, this,
                [this](const QString &eventId, const QString &mxcUrl) {
            requestVideoLocalThumbnail(_bridge, eventId, mxcUrl);
        });
        _pinnedList->setFocusPolicy(Qt::StrongFocus);

        // Wire the pinned list's own inline video player to the streaming
        // proxy, exactly like the timeline list.
        wireInlineVideoPlayer(_pinnedList->inlineVideoPlayer());

        // Feed the pinned scroll position to the list so the scroll-date
        // affordance behaves like the timeline (no pagination is wired here).
        connect(_pinnedScroll->verticalScrollBar(), &QScrollBar::valueChanged,
                this, [this](int value) {
            if (_pinnedList) {
                _pinnedList->updateVisibleTop(value);
            }
        });

        // The go-to-message jump button is the only jump affordance here. It
        // funnels through the same unified jumpTo() as search / links / replies;
        // the Pinned source closes the pinned panel and reconciles bookkeeping.
        connect(_pinnedList, &HistoryList::jumpToMessageRequested, this,
                [this](const QString &eventId) {
            jumpTo(_currentRoomId, eventId, JumpSource::Pinned);
        });

        _pinnedList->setBackgroundAnchor(this);

        // Feed theme background to pinned messages list.
        if (auto *tm = _controller->themeManager()) {
            _pinnedList->invalidateBackground();
            connect(tm, &Theme::ThemeManager::themeChanged,
                    _pinnedList, [this](bool, Theme::ThemeMode) {
                if (_pinnedList) {
                    _pinnedList->invalidateBackground();
                }
            });
        }

        connect(_pinnedList, &HistoryList::contextActionFeedback, this, [this](const QString &text) {
            if (_toast) {
                _toast->showToast(text);
            }
        });
        connect(_pinnedList, &HistoryList::replyRequested, this, [this](
                const QString &eventId,
                const QString &senderName,
                const QString &body,
                const QString &quotedText) {
            if (_currentRoomId.isEmpty() || eventId.isEmpty()) {
                return;
            }
            closePinnedSection();
            const auto preview = resolveComposeReplyPreview(_list->messages(), eventId, body);
            _input->enterReplyMode(
                eventId,
                senderName,
                preview.text,
                quotedText,
                preview.path);
        });
        connect(_pinnedList, &HistoryList::editMessageRequested, this, [this](
                const QString &eventId,
                const QString &senderName,
                const QString &body,
                const QString &formattedBody) {
            if (_currentRoomId.isEmpty() || eventId.isEmpty()) {
                return;
            }
            closePinnedSection();
            _input->enterEditMode(eventId, senderName, body, formattedBody);
        });
        connect(_pinnedList, &HistoryList::pinMessageRequested, this, [this](const QString &eventId, bool pinned) {
            if (_currentRoomId.isEmpty() || eventId.isEmpty()) {
                return;
            }
            _pendingPinEventId = eventId;
            _pendingPinState = pinned;
            applyPinStateLocally(eventId, pinned);
            _bridge->pinMessage(_currentRoomId, eventId, pinned);
        });
        connect(_pinnedList, &HistoryList::forwardRequested, this, [this](const QString &eventId) {
            if (_currentRoomId.isEmpty() || eventId.isEmpty()) {
                return;
            }
            // Forwarding is a modal pick-a-room dialog; stay in the pinned
            // section so the user returns to it afterwards (don't close it).
            const auto targets = SavedMessages::arrangeForwardTargets(
                _bridge->cachedRooms(),
                _bridge->savedMessagesRoomId(),
                _currentRoomId);
            if (targets.isEmpty()) {
                if (_toast) {
                    _toast->showToast(tr("No destination rooms available."));
                }
                return;
            }

            HistoryForwardDialog dialog(targets, _bridge, this);
            if (dialog.exec() != HistoryForwardDialog::Accepted) {
                return;
            }
            const auto dstRoomId = dialog.selectedRoomId();
            if (dstRoomId.isEmpty()) {
                return;
            }
            resolveForwardDestination(dstRoomId,
                [this, eventId, src = _currentRoomId](const QString &roomId) {
                    _bridge->forwardMessage(src, eventId, roomId);
                });
        });
        connect(_pinnedList, &HistoryList::deleteMessageRequested, this, [this](const QString &eventId) {
            if (_currentRoomId.isEmpty() || eventId.isEmpty()) {
                return;
            }
            HistoryConfirmDialog dialog(
                this,
                QString(),
                tr("Do you want to delete this message?"),
                tr("Delete"),
                QString(),
                HistoryConfirmDialog::Attention);
            if (dialog.exec() == HistoryConfirmDialog::Accepted) {
                // Dim/quiesce it in BOTH lists: the same event is on screen in the
                // pinned section and behind it in the timeline.
                _pinnedList->markDeleting(eventId);
                if (_list) {
                    _list->markDeleting(eventId);
                }
                _pendingDeleteEventIds.insert(eventId);
                _bridge->deleteMessage(_currentRoomId, eventId);
            }
        });

        // Phase 1: timeline-style interaction signals, mirroring `_list`.
        connect(_pinnedList, &HistoryList::openMediaViewRequested, this,
                [this](const QVector<TimelineItem> &items, int index) {
            emit openMediaViewRequested(items, index);
        });
        connect(_pinnedList, &HistoryList::replyToMessageRequested, this,
                [this](const QString &eventId, const QString &originEventId) {
            if (eventId.isEmpty()) {
                return;
            }
            closePinnedSection();
            pushReplyReturn(originEventId);
            jumpToMessage(eventId);
        });
        connect(_pinnedList, &HistoryList::reactionRequested, this,
                [this](const QString &eventId, const QString &key, bool active) {
            if (_currentRoomId.isEmpty() || eventId.isEmpty() || key.isEmpty()) {
                return;
            }
            _bridge->setReaction(_currentRoomId, eventId, key, active);
        });
        connect(_pinnedList, &HistoryList::fileDownloadRequested, this,
                [this](const QString &mxcUrl) {
            _mediaRequests.resumeDownload(mxcUrl);
            _mediaRequests.requestFileOpen(mxcUrl);
            if (MediaCache::needsResolution(mxcUrl)) {
                MediaCache::markRequested(mxcUrl);
                _bridge->resolveMedia(mxcUrl);
            }
            if (_pinnedList) {
                _pinnedList->update();
            }
        });
        connect(_pinnedList, &HistoryList::fileOpenRequested, this,
                [this](const QString &mxcUrl, const QString &filename, const QString &mime) {
            openResolvedFile(mxcUrl, filename, mime);
        });
        connect(_pinnedList, &HistoryList::videoDownloadRequested, this,
                [this](const QString &mxcUrl, const QString &eventId) {
            _mediaRequests.resumeDownload(mxcUrl);
            _mediaRequests.requestVideoOpen(mxcUrl, eventId);
            requestResolvedMxcUrl(_bridge, mxcUrl, false, false);
            if (_pinnedList) {
                _pinnedList->update();
            }
        });
        connect(_pinnedList, &HistoryList::audioDurationLearned, this,
                [this](const QString &mxcUrl, quint64 durationMs) {
            if (_bridge) {
                _bridge->setAudioDuration(mxcUrl, durationMs);
            }
        });
        connect(_pinnedList, &HistoryList::audioDownloadRequested, this,
                [this](const QString &mxcUrl, const QString &eventId) {
            _mediaRequests.resumeDownload(mxcUrl);
            _mediaRequests.requestAudioPlay(mxcUrl, eventId);
            quint64 mediaSize = 0;
            if (_pinnedList) {
                for (const auto &msg : _pinnedList->messages()) {
                    if (msg.eventId == eventId) {
                        mediaSize = TeleMatrix::mediaSize(msg);
                        break;
                    }
                }
            }
            const auto forceFileFallback = mediaSize > kMaxMemoryAudioBytes;
            if (MediaCache::hasMemoryBlob(mxcUrl) && MediaCache::localPath(mxcUrl).isEmpty()) {
                // Memory playback failed; force the compatibility file fallback.
                if (!MediaCache::isRequested(mxcUrl)) {
                    MediaCache::markRequested(mxcUrl);
                    _bridge->resolveMedia(mxcUrl);
                }
            } else if (forceFileFallback) {
                if (!MediaCache::isRequested(mxcUrl)) {
                    MediaCache::markRequested(mxcUrl);
                    _bridge->resolveMedia(mxcUrl);
                }
            } else if (MediaCache::needsResolution(mxcUrl)) {
                MediaCache::markRequested(mxcUrl);
                _bridge->resolveMediaBytes(mxcUrl);
            }
            if (_pinnedList) {
                _pinnedList->update();
            }
        });
        connect(_pinnedList, &HistoryList::mediaExportRequested, this,
                [this](const QString &mxcUrl, const QString &targetPath) {
            if (!_bridge || mxcUrl.isEmpty() || targetPath.isEmpty()) {
                return;
            }
            _bridge->exportMediaToPath(mxcUrl, targetPath);
        });
        connect(_pinnedList, &HistoryList::mediaDownloadCancelRequested, this,
                [this](const QString &mxcUrl) {
            _mediaRequests.cancelDownload(mxcUrl);
            _mediaRequests.clearPendingOpenRequests(mxcUrl);
            _mediaRequests.removeDeferredProgress(mxcUrl);
            MediaCache::clearRequested(mxcUrl);
            MediaCache::clearDownloadState(mxcUrl);
            if (_bridge) {
                _bridge->cancelMediaDownload(mxcUrl);
            }
            if (_pinnedList) {
                _pinnedList->update();
            }
        });
        connect(_pinnedList, &HistoryList::pollVoteRequested, this,
                [this](const QString &roomId,
                       const QString &pollEventId,
                       const QStringList &optionIds) {
            if (roomId.isEmpty() || pollEventId.isEmpty() || optionIds.isEmpty()) {
                return;
            }
            _bridge->sendPollVote(roomId, pollEventId, optionIds);
        });
    }

    // Mirror the timeline's render-affecting config so pinned bubbles look
    // identical (pin permission, large-emoji setting, verified-session state).
    if (_list) {
        _pinnedList->setCanPinMessages(_list->canPinMessages());
        _pinnedList->setSessionVerified(_list->isSessionVerified());
    }
    _pinnedList->setLargeEmojiEnabled(_controller->settings().largeEmoji());
    _pinnedList->setBackgroundDoodlesEnabled(_controller->settings().backgroundDoodles());

    // Only pass fully resolved pinned messages to the list.
    if (_pinnedState.hasUnresolvedMessages()) {
        _pinnedList->setLoadingTimeline(true);
        _pinnedState.setFetchRoomId(_currentRoomId);
        _bridge->getPinnedMessagesAsync(_currentRoomId);
    } else {
        _pinnedList->setMessages(_pinnedState.resolvedMessages());
    }
    _pinnedList->setRoomId(_currentRoomId);

    // Hide normal timeline controls. Stop any inline video in the main list so it
    // doesn't keep playing (with audio) behind the pinned section; stash its
    // position so it resumes when reopened (closePinnedSection stops the pinned
    // list's own player symmetrically).
    if (_list) {
        if (auto *player = _list->inlineVideoPlayer();
            player && !player->activeEventId().isEmpty()) {
            MediaCache::setPlaybackPosition(
                player->currentMxc(), player->positionMs());
            player->stop();
        }
    }
    _scroll->hide();
    _input->hide();
    if (_pinnedBar) {
        _pinnedBar->hide();
    }
    if (_downButton) {
        _downButton->hide();
    }
    // Show pinned section.
    _pinnedScroll->show();

    // Switch top bar to pinned section mode.
    _topBar->setPinnedSectionMode(true, [this] {
        closePinnedSection();
    });

    updateControlsGeometry();

    if (_pinnedList) {
        _pinnedList->setFocus();
    }

    applyPinnedScroll();
}

void HistoryWidget::applyPinnedScroll() {
    if (!_pinnedScroll || !_pinnedSectionVisible) {
        return;
    }
    // Deferred so the scroll area has recomputed its range after the list laid out.
    QTimer::singleShot(0, this, [this] {
        if (!_pinnedScroll || !_pinnedSectionVisible) {
            return;
        }
        // Recompute the list layout at the now-final geometry. setMessages() ran
        // recalculateLayout() before the section was shown + sized, so on reopen
        // (widget size unchanged → no resizeEvent) bubble heights stay computed at
        // a stale width, leaving layout slots taller than the painted bubbles and a
        // large empty gap below. Relaying out here corrects the content extent.
        if (_pinnedList) {
            _pinnedList->relayout();
        }
        // Scroll once the scroll area picks up the new range from the relayout above.
        QTimer::singleShot(0, this, [this] {
            if (!_pinnedScroll || !_pinnedSectionVisible) {
                return;
            }
            if (_pinnedScrollTop < 0) {
                // Initial open: start at the bottom (newest message), like the timeline.
                _pinnedScroll->scrollToY(_pinnedScroll->scrollTopMax());
            } else {
                _pinnedScroll->scrollToY(_pinnedScrollTop);
            }
        });
    });
}

void HistoryWidget::closePinnedSection() {
    if (!_pinnedSectionVisible) {
        return;
    }
    // Remember the scroll position so the next open lands where we left off.
    if (_pinnedScroll) {
        _pinnedScrollTop = _pinnedScroll->scrollTop();
    }
    _pinnedSectionVisible = false;

    // Drop any pinned-list jump preloader and stop inline video playback.
    if (_pinnedList) {
        _pinnedList->setLoadingTimeline(false);
        if (auto *player = _pinnedList->inlineVideoPlayer()) {
            player->stop();
        }
    }

    // Hide pinned section.
    if (_pinnedScroll) {
        _pinnedScroll->hide();
    }

    // Restore top bar to normal mode.
    _topBar->setPinnedSectionMode(false);

    // Restore normal timeline controls.
    _scroll->show();
    _input->show();
    if (_pinnedBar && _pinnedBar->hasPinnedMessage()) {
        _pinnedBar->show();
    }
    updateControlsGeometry();
    _input->focusInput();
}

void HistoryWidget::scrollToMessageAndHighlight(const QString &eventId) {
    if (!_list || !_scroll || eventId.isEmpty()) {
        return;
    }
    const auto y = _list->yForEventId(eventId);
    if (y < 0) {
        return;
    }
    const auto rowHeight = _list->rowHeightForEventId(eventId);
    const auto targetY = qBound(
        0,
        y + (qMax(0, rowHeight) / 2) - (_scroll->height() / 2),
        _scroll->scrollTopMax());
    if (!_scrollToAnimation) {
        _scrollToAnimation = new QVariantAnimation(this);
        _scrollToAnimation->setDuration(180);
        _scrollToAnimation->setEasingCurve(QEasingCurve::OutCubic);
        connect(_scrollToAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            if (_scroll) {
                _scroll->scrollToY(value.toInt());
            }
        });
    }
    _scrollToAnimation->stop();
    _scrollToAnimation->setStartValue(_scroll->scrollTop());
    _scrollToAnimation->setEndValue(targetY);
    _scrollToAnimation->start();
    _list->highlightMessage(eventId);
}

void HistoryWidget::jumpToMessage(const QString &eventId) {
    jumpTo(_currentRoomId, eventId, JumpSource::Normal);
}

void HistoryWidget::beginFocusFetch(const QString &eventId) {
    // Start a focused-slice fetch for eventId in the current room. The jump
    // loading-cover (enterJumpLoad) owns the visual state.
    _pendingJump = PendingJump{_nextJumpRequestId++, _currentRoomId, eventId};
    _pendingJumpPreferLive = false;
    _focusJumpEventId.clear();
    _bridge->focusOnEvent(_currentRoomId, eventId, _pendingJump->requestId);
}

void HistoryWidget::enterJumpLoad(const QString &eventId, JumpSource source) {
    // Superseding a still-loading pinned jump: balance its pinned bookkeeping so
    // _pinnedState doesn't leak an in-flight jump.
    if (_jumpLoad.active() && _jumpLoadSource == JumpSource::Pinned) {
        cancelPinnedJump(_jumpLoadEventId);
    }
    _jumpLoad.begin();
    _jumpLoadEventId = eventId;
    _jumpLoadSource = source;
    if (_list) {
        _list->setJumpLoadingCover(true);
    }
    startJumpFloorTimer();
}

void HistoryWidget::startJumpFloorTimer() {
    if (!_jumpFloorTimer) {
        _jumpFloorTimer = new QTimer(this);
        _jumpFloorTimer->setSingleShot(true);
        _jumpFloorTimer->setInterval(kJumpLoadingMinMs);
        connect(_jumpFloorTimer, &QTimer::timeout, this, [this] {
            if (_jumpLoad.onFloorElapsed() == JumpLoadController::Action::Reveal) {
                finishJumpReveal();
            }
        });
    }
    _jumpFloorTimer->start();
}

void HistoryWidget::finishJumpReveal() {
    if (_list) {
        _list->setJumpLoadingCover(false);
        _list->setLoadingTimeline(false); // clear any generic switch preloader too
    }
    if (_jumpLoadSource == JumpSource::Pinned) {
        completePinnedJump();
    }
    if (!_jumpLoadEventId.isEmpty() && _list) {
        _list->highlightMessage(_jumpLoadEventId); // flash plays after the reveal
    }
    _jumpLoadEventId.clear();
    _jumpLoad.reset();
    if (_jumpFloorTimer) {
        _jumpFloorTimer->stop();
    }
}

void HistoryWidget::finishJumpFallback() {
    if (_list) {
        _list->setJumpLoadingCover(false);
        _list->setLoadingTimeline(false);
    }
    if (_jumpLoadSource == JumpSource::Pinned) {
        cancelPinnedJump(_jumpLoadEventId);
    }
    _jumpLoadEventId.clear();
    _jumpLoad.reset();
    if (_jumpFloorTimer) {
        _jumpFloorTimer->stop();
    }
    if (_toast) {
        _toast->showToast(tr("Couldn't load that message."));
    }
    returnToLive();
}

void HistoryWidget::returnToLive() {
    if (_currentRoomId.isEmpty()) return;
    _returnStack.clear();
    _scrollStates.clearPendingRestore();
    _focusJumpEventId.clear();
    // Ask the Rust side to reset the timeline window to the live tail.
    // This fires a timelineChanged signal which calls onTimelineChanged →
    // setSlice with isLive=true.  The down-button mode will be corrected
    // when that slice arrives.
    _bridge->returnToLive(_currentRoomId);
}

void HistoryWidget::checkReturnStack() {
    if (!_list || !_scroll) {
        return;
    }

    while (_returnStack.hasPosition()) {
        const auto &pos = _returnStack.lastPosition();
        if (pos.anchorEventId.isEmpty()) {
            _returnStack.dropLastPosition();
            continue;
        }

        const auto y = _list->yForEventId(pos.anchorEventId);
        if (y < 0) {
            break;
        }

        const auto targetTop = qBound(
            0,
            y - pos.pixelOffset,
            _scroll->scrollTopMax());
        if (_scroll->scrollTop() >= targetTop) {
            _returnStack.dropLastPosition();
            continue;
        }

        break;
    }
}

void HistoryWidget::scrollToDate(const QDate &date) {
    if (!_list || !_scroll) {
        return;
    }
    // Convert date to start-of-day timestamp (offset_date = date - 1).
    const auto timestamp = QDateTime(date, QTime(0, 0), QTimeZone::UTC).toSecsSinceEpoch();
    const auto eventId = _list->eventIdNearDate(timestamp);
    if (eventId.isEmpty()) {
        return;
    }
    scrollToMessageAndHighlight(eventId);
}

void HistoryWidget::saveScrollState() {
    if (_currentRoomId.isEmpty() || !_list || !_scroll) return;

    RoomScrollState state;
    const auto isAtBottom = isScrollNearBottom(_scroll);
    // Save the first-visible anchor unless we're at the live tail (live AND at
    // bottom), where an empty anchor lets reopen use the normal bottom/unread
    // logic. A focused (jumped-to) view is never at the live tail, so always
    // anchor it: the Rust timeline window keeps the focused timeline alive
    // across the switch (we no longer return it to live on leave), so on reopen
    // the SAME items are present and this anchor+offset restores the position
    // exactly — instant, no rebuild, no drift, like a manual-scroll position.
    if (!isAtBottom || !_list->isLive()) {
        for (int i = 0; i < _list->layoutCount(); ++i) {
            const auto& layout = _list->layoutAt(i);
            if (layout.y + layout.height > _scroll->scrollTop()) {
                if (i < _list->messageCount()) {
                    state.anchorEventId = _list->messageAt(i).eventId;
                    const auto messageY = _list->yForEventId(state.anchorEventId);
                    state.anchorPixelOffset = messageY - _scroll->scrollTop();
                }
                break;
            }
        }
    }
    // wasLive stays true so reopen restores the anchor statically via the normal
    // path; loadRoomData's focusOnEvent re-fetch (gated on !wasLive) stays
    // dormant — we reuse the persisted focused window instead of rebuilding it.
    state.wasLive = true;
    state.focusEventId = QString();
    _scrollStates.save(_currentRoomId, state);
}

void HistoryWidget::closeRoom() {
    if (_currentRoomId.isEmpty()) {
        return;
    }

    // A previewed room has no saved scroll state, drafts or timeline to release — it was never
    // joined, so the cleanup below has nothing to work with.
    if (_previewMode) {
        exitPreviewMode();
        _currentRoomId.clear();
        showChatControls(false);
        return;
    }

    // Save current room state (same cleanup as switching rooms in loadRoom).
    saveScrollState();
    // In-flight uploads are intentionally NOT cleared on room switch: a direct
    // upload has no SDK echo, so its optimistic bubble must outlive the switch
    // and re-show in its own room. They're cleaned on completion/failure/cancel,
    // and fully on teardown (~HistoryWidget).
    _returnStack.clear();

    const auto draftPreview = _drafts.updateFromInput(_currentRoomId, *_input);
    flushDraftChanged();
    emit draftChanged(_currentRoomId, draftPreview);

    if (_pinnedSectionVisible) {
        closePinnedSection();
    }

    // Fully closing the room (Escape → placeholder) intentionally resets it to
    // live, unlike a room *switch* (loadRoom) which now keeps a focused view so
    // it can be restored on reopen. Closing is a deliberate "done here" signal.
    _bridge->returnToLive(_currentRoomId);
    // Closing the room for real (not a switch): drop its resident Rust timeline
    // state so memory doesn't accumulate. Reopening rebuilds it.
    _bridge->releaseRoomTimeline(_currentRoomId);
    _currentRoomId.clear();
    _currentRoomIsDirect = false;
    _currentNotificationMode = RoomNotificationMode::AllMessages;
    if (_topBar) {
        _topBar->setEncrypted(false);
        _topBar->setMuted(false);
    }
    _lastSeenTimestamp = 0;
    _directChatUserId.clear();
    resetCurrentRoomPermissions();
    _readReceipts.clearPending();
    _optimisticUnreadFrontierEventId.clear();
    _optimisticReadTillEventId.clear();
    pushReadFrontier(); // closeRoom skips updateUnreadCount — reset detector here
    _sessionUnreadBarEventId.clear();
    _unreadBarResolved = false;
    _readMarkerLoaded = false;
    _unreadStateKnown = false;
    _entryScrollSettled = false;
    _unreadEntryAttempts = 0;
    _forceBottomEntryUntilLiveSlice = false;
    // Drop the read-consuming latch; the store clears its own clamp via
    // setActiveRoomId(""). A newly opened room re-arms at its first settle.
    _readConsumingGate = false;
    _roomUnreadCount = 0;
    _firstUnreadIndex = -1;
    _preserveUnreadBarOnEntry = false;
    _unreadBarDismissed = false;
    // Cancel any in-flight jump loading-cover episode.
    _pendingJump.reset();
    _pendingJumpPreferLive = false;
    _jumpLoad.reset();
    _jumpLoadEventId.clear();
    if (_jumpFloorTimer) {
        _jumpFloorTimer->stop();
    }
    if (_list) {
        _list->setJumpLoadingCover(false);
    }
    _lastSeenTimer->stop();
    if (_list) {
        _list->setMarkingMessagesRead(false);
        _list->setShowOutgoingPrivateAvatars(false);
        _list->setMessages({});
    }
    showChatControls(false);
}

bool HistoryWidget::countsTowardsUnread(const TimelineItem &item) {
    return !item.delivery.outgoing
        && contentType(item) != ContentType::Service;
}

int HistoryWidget::messageIndexById(const QString &eventId) const {
    if (!_list || eventId.isEmpty()) {
        return -1;
    }
    for (int i = 0; i < _list->messageCount(); ++i) {
        if (_list->messageAt(i).eventId == eventId) {
            return i;
        }
    }
    return -1;
}

int HistoryWidget::unreadBarScrollTop() const {
    if (!_list) {
        return -1;
    }
    const auto eventId = _list->firstUnreadEventId();
    if (eventId.isEmpty() || !_list->hasMessage(eventId)) {
        return -1;
    }
    const auto y = _list->yForEventId(eventId);
    if (y < 0) {
        return -1;
    }
    // Land the first unread message itself at the top of the viewport; the
    // "Unread messages" bar sits in the ~40px just above it, off-screen.
    return qMax(0, y);
}

int HistoryWidget::unreadBarPassedThreshold() const {
    if (!_list) {
        return -1;
    }
    const auto eventId = _list->firstUnreadEventId();
    if (eventId.isEmpty() || !_list->hasMessage(eventId)) {
        return -1;
    }
    const auto y = _list->yForEventId(eventId);
    if (y < 0) {
        return -1;
    }
    // Matches unreadBarScrollTop(): the entry position is the message top, so
    // keep the preserved unread bar stable until the user scrolls below it.
    return qMax(0, y);
}

bool HistoryWidget::canPlaceUnreadBarAt(const QString &eventId) const {
	if (!_list || eventId.isEmpty() || !_list->hasMessage(eventId)) {
		return false;
	}

	// Delegate the anti-"jumping-bar" heuristic to the pure, unit-tested helper.
	// `_readMarkerLoaded` (the read marker IS loaded in this window) lets it
	// place immediately on a confirmed boundary even when the read region was
	// service messages the public-room filter erased — the case where the old
	// "a regular message must precede the anchor" check wrongly withheld the bar.
	QVector<UnreadBar::PlacementRow> rows;
	rows.reserve(_list->messageCount());
	for (int i = 0; i < _list->messageCount(); ++i) {
		const auto &message = _list->messageAt(i);
		rows.push_back(UnreadBar::PlacementRow{
			message.eventId,
			contentType(message) == ContentType::Service });
	}
	return UnreadBar::canPlaceUnreadBarAt(rows, eventId, _readMarkerLoaded);
}

bool HistoryWidget::scrollToUnreadBar() {
    if (!_list || !_scroll) {
        return false;
    }
    if (_list->firstUnreadEventId().isEmpty()) {
        // No bar drawn yet: un-dismiss and (re)place it at the frozen anchor.
        _unreadBarDismissed = false;
        placeUnreadBar(_roomUnreadCount);
    }
    const auto top = unreadBarScrollTop();
    if (top < 0) {
        return false;
    }
    _scrollToBottomPending = false;
    _scroll->scrollToY(qBound(0, top, _scroll->scrollTopMax()));
    // The viewport is now genuinely parked at the bar — entry has settled and
    // read detection may arm (callers run syncReadMarkingMode).
    _entryScrollSettled = true;
    return true;
}

bool HistoryWidget::requestInitialUnreadBackfill() {
	if (!_bridge
		|| !_list
		|| _currentRoomId.isEmpty()
		|| !_list->canPaginateBack()
		|| _isPaginatingBack) {
		return false;
	}
	_isPaginatingBack = true;
	_scrollToBottomPending = false;
	_bridge->paginateBack(_currentRoomId);
	_paginationTimeoutTimer->start();
	return true;
}

bool HistoryWidget::tryApplyInitialUnreadScroll() {
	if (!_list || !_scroll || _roomUnreadCount <= 0) {
		return false;
	}
	// Settle-cap: the normal path below returns keepUnreadEntryPending() (leaving
	// _initialUnreadScrollNeeded true) whenever the unread boundary isn't loaded
	// yet. If it can NEVER load (boundary beyond pageable history), that repeats
	// forever and the delimiter latch never sets — the bar stays mutable for the
	// whole session. After a bounded number of attempts, force-settle at the best
	// loaded candidate. Force-placing is best-effort: with an empty or still
	// unloaded anchor it draws nothing, and shouldResolveOnLiveSlice then keeps
	// the latch open so a later slice can still place the bar.
	if (++_unreadEntryAttempts > kMaxUnreadEntryAttempts) {
		_unreadBarDismissed = false;
		placeUnreadBar(_roomUnreadCount, /*force=*/true); // best-effort placement
		scrollToUnreadBar();                              // ok if it can't scroll
		_preserveUnreadBarOnEntry = true;
		_initialUnreadScrollNeeded = false;
		_entryScrollSettled = true; // force-settle even when the scroll failed
		syncReadMarkingMode();
		return true;
	}
	const auto keepUnreadEntryPending = [this] {
		_preserveUnreadBarOnEntry = true;
		_initialUnreadScrollNeeded = true;
		syncReadMarkingMode();
		return true;
	};
	if (scrollToUnreadBar()) {
		_preserveUnreadBarOnEntry = true;
		_initialUnreadScrollNeeded = false;
		syncReadMarkingMode();
		return true;
	}

	if (_isPaginatingBack) {
		return keepUnreadEntryPending();
	}

	const auto unreadEventId = !_list->firstUnreadEventId().isEmpty()
		? _list->firstUnreadEventId()
		: _optimisticUnreadFrontierEventId;

	if (!unreadEventId.isEmpty() && _list->hasMessage(unreadEventId)) {
		// The unread candidate is loaded, but the delimiter may be withheld
		// because the window starts directly with unread messages. Keep paging
		// back until we can place the boundary after a read item.
		if (requestInitialUnreadBackfill()) {
			return keepUnreadEntryPending();
		}

		// We reached the start (or cannot page back). Show the delimiter at
		// the first loaded unread candidate instead of falling through to bottom.
		_unreadBarDismissed = false;
		placeUnreadBar(_roomUnreadCount, /*force=*/true);
		if (scrollToUnreadBar()) {
			_preserveUnreadBarOnEntry = true;
			_initialUnreadScrollNeeded = false;
			syncReadMarkingMode();
			return true;
		}
	}

	if (requestInitialUnreadBackfill()) {
		return keepUnreadEntryPending();
	}

	return keepUnreadEntryPending();
}

int HistoryWidget::unreadCountInRange(
        const QString &firstEventId,
        const QString &lastEventId) const {
    if (!_list || firstEventId.isEmpty() || lastEventId.isEmpty()) {
        return 0;
    }

    bool started = false;
    int count = 0;
    for (int i = 0; i < _list->messageCount(); ++i) {
        const auto &message = _list->messageAt(i);
        if (!started) {
            if (message.eventId != firstEventId) {
                continue;
            }
            started = true;
        }
        if (countsTowardsUnread(message)) {
            ++count;
        }
        if (message.eventId == lastEventId) {
            return count;
        }
    }

    return 0;
}

QString HistoryWidget::nextUnreadEventIdAfter(const QString &eventId) const {
    if (!_list) {
        return {};
    }

    bool afterTarget = eventId.isEmpty();
    for (int i = 0; i < _list->messageCount(); ++i) {
        const auto &message = _list->messageAt(i);
        if (!afterTarget) {
            if (message.eventId == eventId) {
                afterTarget = true;
            }
            continue;
        }
        if (countsTowardsUnread(message)) {
            return message.eventId;
        }
    }

    return {};
}

void HistoryWidget::setUnreadBarPreservingScroll(
		const QString &eventId,
		int unreadCount) {
	if (!_list || eventId.isEmpty()) {
		return;
	}
	const auto wasAtBottom = isScrollNearBottom(_scroll);
	if (wasAtBottom && _scroll) {
		_list->setUnreadBar(eventId, unreadCount);
		const auto freshMax = qMax(
			0,
			_list->height() - _scroll->viewport()->height());
		_scroll->scrollToY(freshMax);
	} else {
		_list->saveScrollAnchor();
		_list->setUnreadBar(eventId, unreadCount);
		_list->restoreScrollAnchor();
	}
}

void HistoryWidget::clearUnreadBarPreservingScroll() {
	// Undraw the visible bar only — the frozen session anchor is intentionally
	// preserved so the delimiter re-materialises at the same event when it
	// reloads. The anchor is cleared only at genuine session boundaries
	// (leave/switch room, send, scroll-to-bottom / fully read).
	if (!_list || _list->firstUnreadEventId().isEmpty()) {
		return;
	}
	const auto wasAtBottom = isScrollNearBottom(_scroll);
	if (wasAtBottom && _scroll) {
		_list->clearUnreadBar();
		const auto freshMax = qMax(
			0,
			_list->height() - _scroll->viewport()->height());
		_scroll->scrollToY(freshMax);
	} else {
		_list->saveScrollAnchor();
		_list->clearUnreadBar();
		_list->restoreScrollAnchor();
	}
}

void HistoryWidget::placeUnreadBar(int unreadCount, bool force) {
	if (!_list || unreadCount <= 0 || _unreadBarDismissed) {
		return;
	}
	// Once the bar has been placed this session, always re-place it at the same
	// captured event id; only before the first placement do we read the live
	// (still-converging) frontier. This is what keeps a newly-arrived message
	// from sliding the delimiter (the first-unread anchor stays frozen).
	const auto anchor = UnreadBar::resolveAnchor(
		_sessionUnreadBarEventId,
		_optimisticUnreadFrontierEventId);
	if (anchor.isEmpty()) {
		return;
	}
	// force: show at the very start of history (no read item precedes it) when we
	// cannot page back any further. Otherwise require a read item before it and,
	// either way, require the anchor to be loaded so it actually draws.
	const auto placeable = force
		? _list->hasMessage(anchor)
		: canPlaceUnreadBarAt(anchor);
	if (!placeable) {
		return;
	}
	_sessionUnreadBarEventId = anchor;
	setUnreadBarPreservingScroll(anchor, unreadCount);
}

void HistoryWidget::destroyUnreadBar() {
	_sessionUnreadBarEventId.clear();
	if (_list) {
		_list->clearUnreadBar();
	}
}

void HistoryWidget::applyOptimisticReadProgress(const QString &eventId) {
    if (!_list
        || _roomUnreadCount <= 0
        || eventId.isEmpty()) {
        return;
    }

    auto frontierEventId = _optimisticUnreadFrontierEventId;
    if (frontierEventId.isEmpty()) {
        frontierEventId = _list->firstUnreadEventId();
    }
    if (frontierEventId.isEmpty()) {
        return;
    }

    const auto locallyRead = qMin(_roomUnreadCount, unreadCountInRange(frontierEventId, eventId));
    if (locallyRead <= 0) {
        return;
    }

    _optimisticReadTillEventId = eventId;
    _optimisticUnreadFrontierEventId = nextUnreadEventIdAfter(eventId);
    _firstUnreadIndex = messageIndexById(_optimisticUnreadFrontierEventId);
    updateUnreadCount(qMax(0, _roomUnreadCount - locallyRead));
}

void HistoryWidget::applyLiveUnreadState(const TimelineSlice &slice) {
    if (!slice.isLive) {
        return;
    }
    if (!slice.unreadStateKnown) {
        return;
    }

    if (slice.unreadCount <= 0) {
        const auto hasPendingLocalRead = !_optimisticReadTillEventId.isEmpty()
            || _readReceipts.hasPending();
        if (_roomUnreadCount > 0 && !hasPendingLocalRead) {
            return;
        }
        _preserveUnreadBarOnEntry = false;
        _unreadBarDismissed = false;
        _readReceipts.clearPending();
        _optimisticUnreadFrontierEventId.clear();
        _optimisticReadTillEventId.clear();
        _firstUnreadIndex = -1;
        if (_roomUnreadCount != 0) {
            updateUnreadCount(0);
        }
        syncReadMarkingMode();
        return;
    }

    auto effectiveUnreadCount = slice.unreadCount;
    auto effectiveFirstUnreadEventId = slice.firstUnreadEventId;
    if (_optimisticReadTillEventId.isEmpty()) {
        effectiveUnreadCount = qMax(effectiveUnreadCount, _roomUnreadCount);
        if (effectiveFirstUnreadEventId.isEmpty()) {
            effectiveFirstUnreadEventId = _optimisticUnreadFrontierEventId;
        }
    }
    bool hasOptimisticAdjustment = false;

    if (effectiveFirstUnreadEventId.isEmpty()) {
        _optimisticUnreadFrontierEventId.clear();
        _firstUnreadIndex = -1;
        if (effectiveUnreadCount != _roomUnreadCount) {
            updateUnreadCount(effectiveUnreadCount);
        }
        return;
    }

    if (!_optimisticReadTillEventId.isEmpty()) {
        const auto locallyRead = unreadCountInRange(
            slice.firstUnreadEventId,
            _optimisticReadTillEventId);
        if (locallyRead > 0) {
            hasOptimisticAdjustment = true;
            effectiveUnreadCount = qMax(0, effectiveUnreadCount - locallyRead);
            auto optimisticFrontier = _optimisticUnreadFrontierEventId;
            if (optimisticFrontier.isEmpty()) {
                optimisticFrontier = nextUnreadEventIdAfter(_optimisticReadTillEventId);
            }
            if (!optimisticFrontier.isEmpty()) {
                effectiveFirstUnreadEventId = optimisticFrontier;
            } else if (effectiveUnreadCount == 0) {
                effectiveFirstUnreadEventId.clear();
            }
        } else {
            _optimisticReadTillEventId.clear();
            _optimisticUnreadFrontierEventId.clear();
        }
    }

    if (effectiveUnreadCount == 0) {
        effectiveFirstUnreadEventId.clear();
    }

    _optimisticUnreadFrontierEventId = effectiveFirstUnreadEventId;
    if (!hasOptimisticAdjustment) {
        _optimisticReadTillEventId.clear();
    }
    _firstUnreadIndex = messageIndexById(effectiveFirstUnreadEventId);

    if (effectiveUnreadCount != _roomUnreadCount) {
        updateUnreadCount(effectiveUnreadCount);
    }
}

void HistoryWidget::applyStoreUnreadState(const UnreadRoomState &state) {
    if (_currentRoomId.isEmpty() || state.roomId != _currentRoomId) {
        return;
    }

    _optimisticReadTillEventId = state.pendingReadTillEventId;
    _optimisticUnreadFrontierEventId = state.effectiveFirstUnreadEventId;
    _firstUnreadIndex = messageIndexById(_optimisticUnreadFrontierEventId);

    if (state.effectiveUnreadCount <= 0) {
        _preserveUnreadBarOnEntry = false;
        _unreadBarDismissed = false;
        _readReceipts.clearPending();
    }

    updateUnreadCount(state.effectiveUnreadCount, false);

    if (_list && !_unreadBarResolved) {
        const auto currentUnreadBarEventId = _list->firstUnreadEventId();
        const auto drawnLoaded = !currentUnreadBarEventId.isEmpty()
            && _list->hasMessage(currentUnreadBarEventId);
        switch (UnreadBar::decide(
                currentUnreadBarEventId,
                drawnLoaded,
                state.effectiveUnreadCount)) {
        case UnreadBar::Action::KeepDrawn:
            // Bar already shown (or crossed after the count cleared): keep it
            // exactly where it is, only refresh the count. The freeze means it
            // is never re-pointed at the drifting frontier.
            setUnreadBarPreservingScroll(
                currentUnreadBarEventId,
                qMax(0, state.effectiveUnreadCount));
            break;
        case UnreadBar::Action::Place:
            // Not currently drawn: (re)place at the frozen session anchor. If
            // the anchor is scrolled out of the window placeUnreadBar is a
            // no-op and the bar reappears when it reloads — never at the
            // drifting frontier.
            placeUnreadBar(state.effectiveUnreadCount);
            break;
        case UnreadBar::Action::ClearDrawnKeepAnchor:
            // Count cleared and the anchor's message is unloaded: drop the
            // visible bar but KEEP the frozen anchor so it returns to the same
            // event on reload. This is the fix for the "read some, then a new
            // message slides the delimiter down" bug.
            clearUnreadBarPreservingScroll();
            break;
        case UnreadBar::Action::NoOp:
            break;
        }
    }
    syncReadMarkingMode();
    updateCornerButtonPositions();
}

void HistoryWidget::syncReadMarkingMode() {
    if (_list) {
        _list->setMarkingMessagesRead(UnreadBar::canMarkMessagesRead(
            _windowActive,
            !_currentRoomId.isEmpty(),
            _entryScrollSettled));
    }
}

void HistoryWidget::updateReadConsumingGate() {
    // "Consuming reads instantly" = this live room is focused and the user is
    // parked at the bottom, so the at-bottom auto-read zeroes any arrival in the
    // same turn. _preserveUnreadBarOnEntry excludes initial unread entry (parked
    // at the delimiter, not the tail); readDetectionHeld() excludes a programmatic
    // teleport to the bottom (a gappy-sync reset), which is not the user reading.
    const bool consuming = _windowActive
        && !_currentRoomId.isEmpty()
        && _entryScrollSettled
        && _list && _list->isLive()
        && !_preserveUnreadBarOnEntry
        && !_list->readDetectionHeld()
        && _scroll && isScrollNearBottom(_scroll);
    if (consuming == _readConsumingGate) {
        return;
    }
    _readConsumingGate = consuming;
    if (_unreadStateStore) {
        _unreadStateStore->setReadConsumingRoom(
            consuming ? _currentRoomId : QString());
    }
}

void HistoryWidget::pushReadFrontier() {
    if (_list) {
        _list->setReadFrontier(_optimisticUnreadFrontierEventId);
    }
}

void HistoryWidget::maybeFinalizeUnreadBarAfterScroll() {
    if (!_scroll || !_list) {
        return;
    }
    if (_roomUnreadCount <= 0) {
        _preserveUnreadBarOnEntry = false;
        syncReadMarkingMode();
        return;
    }
    if (_list->firstUnreadEventId().isEmpty()) {
        return;
    }
    const auto threshold = unreadBarPassedThreshold();
    if (threshold < 0 || _scroll->scrollTop() <= threshold) {
        return;
    }
    _preserveUnreadBarOnEntry = false;
    syncReadMarkingMode();
}

void HistoryWidget::updateUnreadCount(int count, bool syncToStore) {
    const auto changed = (_roomUnreadCount != count);
    _roomUnreadCount = count;
    if (_downButton) {
        _downButton->setUnreadCount(count);
    }
    if (syncToStore && _unreadStateStore && !_currentRoomId.isEmpty()) {
        _unreadStateStore->optimisticReadProgress(
            _currentRoomId,
            _optimisticReadTillEventId,
            _optimisticUnreadFrontierEventId,
            count,
            _list ? _list->eventTimestamp(_optimisticReadTillEventId) : 0);
    }
    if (changed && !_currentRoomId.isEmpty()) {
        emit unreadCountChanged(_currentRoomId, count);
    }
    // Choke point: every optimistic/live/store frontier settle routes through
    // here right after _optimisticUnreadFrontierEventId is set, so this single
    // push keeps the list's read detector in lockstep. Boundary clears that do
    // not call updateUnreadCount (closeRoom) and the live-slice exits that skip
    // it (applyLiveUnreadState's conditional updates) push explicitly.
    pushReadFrontier();
}

void HistoryWidget::sendTrackedReadReceipt(const QString &eventId) {
    if (!_bridge || _currentRoomId.isEmpty() || eventId.isEmpty()) {
        return;
    }
    _readReceipts.markRequested(eventId);
    const auto requestId = _bridge->sendReadReceipt(_currentRoomId, eventId);
    if (_unreadStateStore) {
        const auto state = _unreadStateStore->roomState(_currentRoomId);
        _unreadStateStore->bindReadReceiptRequest(
            _currentRoomId,
            eventId,
            state.pendingReadRevision,
            requestId);
    }
}

void HistoryWidget::completePinnedJump() {
    if (!_pinnedState.completeJump()) return;
    if (_pinnedList) {
        _pinnedList->setLoadingTimeline(false);
    }
    // Reveal the timeline, now scrolled/highlighted to the jumped-to message.
    closePinnedSection();
}

void HistoryWidget::schedulePendingJumpVisibilityTimeout(
        const QString &roomId,
        const QString &eventId,
        quint64 requestId) {
    QTimer::singleShot(kPendingJumpTimeoutMs, this, [this, roomId, eventId, requestId] {
        if (!_pendingJump
            || _pendingJump->roomId != roomId
            || _pendingJump->eventId != eventId
            || _pendingJump->requestId != requestId) {
            return;
        }
        _pendingJump.reset();
        _pendingJumpPreferLive = false;
        if (_jumpLoad.active()
            && _jumpLoad.onFetchFailed() == JumpLoadController::Action::Fallback) {
            finishJumpFallback();
        } else {
            if (_list) {
                _list->setLoadingTimeline(false);
            }
            cancelPinnedJump(eventId);
        }
        qWarning() << "HistoryWidget: focused jump did not expose target message"
            << eventId << "in room" << roomId << "request" << requestId;
    });
}

void HistoryWidget::cancelPinnedJump(const QString &eventId) {
    if (!_pinnedState.cancelJump(eventId)) return;
    // Fetch failed/timed out: drop the pinned-list preloader, keep the section open.
    if (_pinnedList) {
        _pinnedList->setLoadingTimeline(false);
    }
}

void HistoryWidget::refreshPinnedBar() {
    if (!_pinnedBar) return;

    const auto pinnedTextFor = [](const TimelineItem &msg) -> QString {
        // Audio: always show "Voice message" regardless of body/filename.
        if (isAudioMessage(msg)) return tr("Voice message");
        if (!captionText(msg).simplified().isEmpty()) return captionText(msg).simplified();
        if (!bodyText(msg).simplified().isEmpty()) return bodyText(msg).simplified();
        switch (contentType(msg)) {
        case ContentType::Image: return tr("Photo");
        case ContentType::Video: return tr("Video");
        case ContentType::File: return mediaFilename(msg).isEmpty() ? tr("File") : mediaFilename(msg);
        case ContentType::Poll:
            return (pollContent(msg) && !pollContent(msg)->question.isEmpty())
                ? pollContent(msg)->question
                : tr("Poll");
        default: return tr("Message");
        }
    };
    const auto pinnedPreviewFor = [](const TimelineItem &msg) -> QString {
        if (isImageMessage(msg)) return mediaUrl(msg);
        if (isVideoMessage(msg))
            return !mediaThumbUrl(msg).isEmpty() ? mediaThumbUrl(msg) : mediaUrl(msg);
        return {};
    };

    // Only show fully resolved pinned messages in the compact pinned bar.
    QVector<const TimelineItem*> resolved;
    for (const auto &msg : _pinnedState.messages()) {
        if (msg.timestamp != 0
            || !formattedText(msg).isEmpty()
            || !isTextMessage(msg)) {
            resolved.push_back(&msg);
        }
    }

    if (!resolved.isEmpty()) {
        const auto idx = _pinnedState.clampedIndexForCount(int(resolved.size()));
        const auto &shown = *resolved[idx];
        const auto title = (resolved.size() > 1)
            ? tr("%1 of %2 Pinned Messages").arg(idx + 1).arg(resolved.size())
            : tr("Pinned Message");
        _pinnedBar->setPinnedMessage(shown.eventId, title, pinnedTextFor(shown), pinnedPreviewFor(shown));
        _pinnedBar->setPinnedCount(int(resolved.size()));
        animatePinnedBarVisibility(true);
    } else if (!_pinnedState.isEmpty()) {
        // All pinned messages are still unresolved; keep the loading bar visible.
        _pinnedBar->setPinnedMessage({}, tr("Pinned Message"),
            tr("Loading..."), {});
        _pinnedBar->setPinnedCount(_pinnedState.size());
        animatePinnedBarVisibility(true);
    } else {
        _pinnedBar->clearPinnedMessage();
        animatePinnedBarVisibility(false);
    }
    updateControlsGeometry();
}

void HistoryWidget::updatePinnedMessagesFromSlice(
        const QStringList &pinnedEventIds,
        const QVector<TimelineItem> &messages) {
    _pinnedState.updateFromSlice(pinnedEventIds, messages);

    // Fetch full pinned-message bodies from the server ONLY while the pinned
    // section is open (to keep it fresh). The top-bar button needs only pin
    // existence, which comes from the pinned event IDs in local synced room state
    // (`Room::pinned_event_ids()`, no network) — so a closed-section room open
    // does NOT fetch. showPinnedSection() lazily fetches bodies when the user
    // actually opens the section.
    if (_pinnedSectionVisible
        && _pinnedState.hasUnresolvedPlaceholders()
        && !_currentRoomId.isEmpty()) {
        _pinnedState.setFetchRoomId(_currentRoomId);
        _bridge->getPinnedMessagesAsync(_currentRoomId);
    }

    refreshPinnedBar();
    if (_topBar) {
        _topBar->setHasPinned(!_pinnedState.isEmpty());
    }
}

void HistoryWidget::applyPinStateLocally(const QString &eventId, bool pinned) {
    if (!_list) {
        return;
    }
    _list->setMessagePinState(eventId, pinned);
    _pinnedState.applyLocalPinState(eventId, pinned, _list->messages());

    if (_topBar) {
        _topBar->setHasPinned(!_pinnedState.isEmpty());
    }

    // Refresh pinned bar.
    if (_pinnedBar) {
        refreshPinnedBar();
    }

    // Refresh pinned section if visible.
    if (_pinnedSectionVisible && _pinnedList) {
        _pinnedList->setMessages(_pinnedState.messages());
    }
}

void HistoryWidget::clearSearchActive() {
    if (_topBar) {
        _topBar->setSearchActive(false);
    }
}

void HistoryWidget::resolveForwardDestination(
        const QString &dstRoomId, std::function<void(const QString &)> forward) {
    // The saved room is created lazily: forwarding to the pending sentinel is
    // one of the two moments it may be created. An already-existing saved room
    // forwards straight through.
    const auto needsSavedEnsure = dstRoomId == SavedMessages::kPendingRoomId;
    if (!needsSavedEnsure) {
        forward(dstRoomId);
        return;
    }
    if (!_bridge) {
        return;
    }
    auto *waiter = new QObject(this);
    connect(_bridge, &ProtocolBridge::savedMessagesRoomReady, waiter,
        [this, waiter, forward = std::move(forward)](
            bool success, const QString &roomId) {
            waiter->deleteLater();
            if (success && !roomId.isEmpty()) {
                forward(roomId);
            } else if (_toast) {
                _toast->showToast(tr("Could not create Saved Messages."));
            }
        });
    _bridge->ensureSavedMessagesRoom(/*create=*/true);
}

bool HistoryWidget::requestSearchInCurrentRoom() {
    // In preview mode we are not a member, so searching would act on a room
    // the SDK does not know.
    if (_currentRoomId.isEmpty() || _previewMode) {
        return false;
    }
    // Look up the room display name for the search context.
    QString roomName = _currentRoomId;
    if (_bridge) {
        const auto rooms = _bridge->cachedRooms();
        for (const auto &room : rooms) {
            if (room.roomId == _currentRoomId) {
                roomName = room.displayName;
                break;
            }
        }
    }
    _topBar->setSearchActive(true);
    emit searchInChatRequested(_currentRoomId, roomName, _currentRoomIsDirect);
    return true;
}

void HistoryWidget::setConnecting(bool connecting) {
    if (_topBar) {
        _topBar->setConnecting(connecting);
    }
}

void HistoryWidget::setWindowActive(bool active) {
    if (_windowActive == active) {
        return;
    }
    _windowActive = active;
    syncReadMarkingMode();
    // Focus lost ⇒ the room no longer consumes reads (arrivals must raise the
    // badge); focus regained at the bottom ⇒ re-arm the clamp.
    updateReadConsumingGate();
    if (_windowActive) {
        applyDeferredMediaUpdates();
    }
}

void HistoryWidget::applyDeferredMediaUpdates() {
    if (!_mediaRequests.hasDeferredUpdates()) {
        return;
    }

    const auto deferred = _mediaRequests.takeDeferredUpdates();
    if (deferred.isEmpty()) {
        return;
    }

    for (auto it = deferred.progress.constBegin(); it != deferred.progress.constEnd(); ++it) {
        if (_mediaRequests.isCancelled(it.key())
            || !MediaCache::isRequested(it.key())) {
            continue;
        }
        MediaCache::DownloadState state;
        state.phase = (it.value().phase == 1)
            ? MediaCache::DownloadPhase::Decrypting
            : MediaCache::DownloadPhase::Downloading;
        state.receivedBytes = it.value().receivedBytes;
        state.totalBytes = it.value().totalBytes;
        MediaCache::setDownloadState(it.key(), state);
    }

    if (!_list) {
        return;
    }

    const auto hadInvalidations = !deferred.invalidations.isEmpty();
    const auto wasAtBottom = hadInvalidations && isScrollNearBottom(_scroll);
    if (hadInvalidations && !wasAtBottom) {
        _list->saveScrollAnchor();
    }
    if (hadInvalidations) {
        _list->invalidateLayoutForMedia(deferred.invalidations);
    }
    if (hadInvalidations && _pinnedList) {
        _pinnedList->invalidateLayoutForMedia(deferred.invalidations);
        _pinnedList->update();
    }
    if (!hadInvalidations || deferred.updateOnly || !deferred.progress.isEmpty()) {
        _list->update();
    }
    if (hadInvalidations) {
        if (wasAtBottom) {
            _scroll->scrollToY(_scroll->scrollTopMax());
        } else {
            _list->restoreScrollAnchor();
        }
    }
    if (hadInvalidations && _input) {
        _input->refreshMentionPopup();
    }
    if (hadInvalidations && _pinnedBar) {
        _pinnedBar->update();
    }
    if (deferred.updateOnly && _mediaRecheckTimer && !_mediaRecheckTimer->isActive()) {
        _mediaRecheckTimer->start();
    }
}

void HistoryWidget::scrollToUnreadOrBottom() {
    if (!_list || _currentRoomId.isEmpty()) return;

    if (_roomUnreadCount > 0 && scrollToUnreadBar()) {
        _preserveUnreadBarOnEntry = true;
        syncReadMarkingMode();
        return;
    }
    _preserveUnreadBarOnEntry = false;
    _unreadBarDismissed = false;
    destroyUnreadBar();
    _scroll->scrollToY(_scroll->scrollTopMax());
    // Explicit user navigation to the tail settles entry too.
    _entryScrollSettled = true;
    syncReadMarkingMode();
}

void HistoryWidget::showMessage(const QString &roomId, const QString &eventId) {
    jumpTo(roomId, eventId, JumpSource::Normal);
}

void HistoryWidget::showMessageLive(
        const QString &roomId,
        const QString &eventId) {
    if (roomId.isEmpty() || eventId.isEmpty() || !_list) {
        return;
    }
    // A toast can outlive our membership, or name a room we never joined; there
    // is no timeline to open, so hand it over for a preview like jumpTo() does.
    if (roomId != _currentRoomId && !isJoinedRoom(roomId)) {
        emit roomSwitchRequested(roomId);
        return;
    }
    switch (JumpRouting::routeNotificationJump(
            roomId == _currentRoomId,
            _list->isLive(),
            _list->hasMessage(eventId))) {
    case JumpRouting::Route::InstantScroll:
        scrollToMessageAndHighlight(eventId);
        return;
    case JumpRouting::Route::ReturnToLiveThenHighlight:
        _pendingJump = PendingJump{_nextJumpRequestId++, roomId, eventId};
        _pendingJumpPreferLive = true;
        returnToLive();
        return;
    case JumpRouting::Route::LiveOpenThenHighlight:
        // Deliberately no enterJumpLoad(): this is a room switch, so it gets the
        // normal switch preloader rather than the opaque jump cover.
        _pendingJump = PendingJump{_nextJumpRequestId++, roomId, eventId};
        _pendingJumpPreferLive = true;
        loadRoom(roomId);
        emit roomSwitchRequested(roomId);
        return;
    case JumpRouting::Route::FocusFetch:
        enterJumpLoad(eventId, JumpSource::Normal);
        beginFocusFetch(eventId);
        return;
    }
}

void HistoryWidget::jumpTo(
        const QString &roomId,
        const QString &eventId,
        JumpSource source) {
    if (roomId.isEmpty() || eventId.isEmpty() || !_list) {
        return;
    }
    if (source == JumpSource::Pinned) {
        // Dedup, then reveal the timeline (its cover) by closing the pinned panel.
        if (!_pinnedState.startJump(eventId)) {
            return;
        }
        closePinnedSection();
    }
    if (roomId != _currentRoomId) {
        // A link can target a room we haven't joined; there is no timeline to
        // jump-load there — hand it to the main widget, which opens a preview.
        if (!isJoinedRoom(roomId)) {
            emit roomSwitchRequested(roomId);
            return;
        }
        // Cross-room: the target is never in the current window → always fetch.
        enterJumpLoad(eventId, source);
        _pendingJump = PendingJump{_nextJumpRequestId++, roomId, eventId};
        loadRoom(roomId);
        emit roomSwitchRequested(roomId);
        return;
    }
    if (_list->hasMessage(eventId)) {
        scrollToMessageAndHighlight(eventId); // instant path
        if (source == JumpSource::Pinned) {
            completePinnedJump();
        }
        return;
    }
    if (_previewMode) {
        // A preview timeline is one raw local fetch; the jump-load machinery
        // has no SDK timeline to pull the target from.
        if (_toast) {
            _toast->showToast(tr("Join the room to view this message."));
        }
        return;
    }
    enterJumpLoad(eventId, source);
    beginFocusFetch(eventId);
}

QString HistoryWidget::formatLastSeen(
        const QVector<TimelineItem> &messages) const {
    // Find the most recent non-outgoing message timestamp.
    qint64 lastSeenTs = 0;
    for (int i = messages.size() - 1; i >= 0; --i) {
        if (!messages[i].delivery.outgoing && messages[i].timestamp > 0) {
            lastSeenTs = messages[i].timestamp;
            break;
        }
    }
    return formatLastSeenTimestamp(lastSeenTs);
}

QString HistoryWidget::formatLastSeenTimestamp(qint64 ts) {
    if (ts <= 0) {
        return tr("last seen recently");
    }

    const auto lastSeenDt = QDateTime::fromSecsSinceEpoch(ts);
    const auto nowDt = QDateTime::currentDateTime();
    const auto diffSecs = lastSeenDt.secsTo(nowDt);

    if (diffSecs < 60) {
        return tr("last seen just now");
    } else if (diffSecs < 3600) {
        const auto minutes = diffSecs / 60;
        return tr("last seen %n minute(s) ago", "", int(minutes));
    } else if (diffSecs < 43200) { // < 12 hours
        const auto hours = diffSecs / 3600;
        return tr("last seen %n hour(s) ago", "", int(hours));
    }

    const auto locale = QLocale();
    if (lastSeenDt.date() == nowDt.date()) {
        const auto time = locale.toString(
            lastSeenDt.time(), QLocale::ShortFormat);
        return tr("last seen today at %1").arg(time);
    } else if (lastSeenDt.date().addDays(1) == nowDt.date()) {
        const auto time = locale.toString(
            lastSeenDt.time(), QLocale::ShortFormat);
        return tr("last seen yesterday at %1").arg(time);
    }

    const auto date = locale.toString(
        lastSeenDt.date(), QLocale::ShortFormat);
    return tr("last seen %1").arg(date);
}

void HistoryWidget::refreshLastSeenSubtitle() {
    if (_currentRoomIsDirect && _topBar) {
        _cachedSubtitle = formatLastSeenTimestamp(_lastSeenTimestamp);
        _topBar->setSubtitle(_cachedSubtitle);
    }
}

void HistoryWidget::refreshTypingSubtitle() {
    if (!_typingState.hasIncomingUsers() || !_topBar) return;
    _topBar->setTypingText(
        _typingState.subtitleText(_currentRoomIsDirect),
        _typingState.animationStart());
}

void HistoryWidget::checkPaginationThresholds() {
    if (_currentRoomId.isEmpty()) return;
    const int scrollTop = _scroll->scrollTop();
    const int viewportHeight = _scroll->viewport()->height();
    const int scrollMax = _scroll->verticalScrollBar()->maximum();

    // Preview mode paginates its own way (raw /messages tokens, no joined-room window). It only
    // grows backward, so there is no forward branch.
    if (_previewMode) {
        const bool atBottom = (scrollMax <= 0)
            || (scrollTop >= scrollMax - 2 * viewportHeight);
        const bool nearTop = !atBottom && scrollTop < 3 * viewportHeight;
        if (nearTop && _list && _list->canPaginateBack()) {
            // A deliberate scroll-up is fresh intent to see older history, so clear the empty-page
            // cap — the user is willing to page through more churn.
            _previewEmptyPageStreak = 0;
            maybeContinuePreviewPagination();
        }
        return;
    }

    // Preload when within 3 viewport heights of the edge (aggressive preloading).
    constexpr int kPreloadHeights = 3;
    // Don't paginate backward if near the bottom — only when the user
    // has actually scrolled up toward older messages. Use a generous
    // threshold to prevent cascade on room open with short message lists.
    const bool atBottom = (scrollMax <= 0)
        || (scrollTop >= scrollMax - 2 * viewportHeight);
    // Back-pagination triggers:
    // 1) User scrolled up near the top (original condition), OR
    // 2) Content insufficient — total scrollable range is less than the
    //    preload threshold.  Covers startup with a small initial batch
    //    (scrollMax==0) AND post-reconnect timeline resets where the SDK
    //    delivers a fresh slice with only ~10 items (scrollMax small but >0).
    const bool nearTop = !atBottom
        && scrollTop < kPreloadHeights * viewportHeight;
    // An EMPTY list counts as insufficient too. A public room whose entire loaded
    // window is service events renders zero items once the "hide system messages"
    // filter runs, and with scrollMax==0 atBottom is true so nearTop can never fire —
    // without this the room sits on the "Loading..." pill forever with nothing driving
    // it toward real messages or the timeline start. canPaginateBack() is the real
    // gate: an exhausted timeline (hit start) or a not-yet-built window reports false.
    const bool contentInsufficient =
        _list && (scrollMax < kPreloadHeights * viewportHeight);
    if ((nearTop || contentInsufficient)
        && _list->canPaginateBack() && !_isPaginatingBack) {
        _isPaginatingBack = true;
        if (nearTop) {
            // User scrolled up — cancel any pending scroll-to-bottom so the
            // upcoming pagination delivery doesn't snap the view to the bottom.
            _scrollToBottomPending = false;
        }
		_bridge->paginateBack(_currentRoomId);
		_paginationTimeoutTimer->start();
	}
	if ((scrollMax - scrollTop) < kPreloadHeights * viewportHeight
		&& _list->canPaginateForward() && !_isPaginatingForward) {
		_isPaginatingForward = true;
		_bridge->paginateForward(_currentRoomId);
		_paginationTimeoutTimer->start();
	}
}

void HistoryWidget::onTimelineChanged(const QString &roomId) {
    if (_currentRoomId.isEmpty()) {
        return;
    }

    // If the changed room is not the currently displayed room,
    // just let the room list refresh pick up the new unread count.
    if (roomId != _currentRoomId) {
        return;
	}

	_latestTimelineSliceRequestId = _bridge->nextRequestId();
	// With a media upload in flight, the incremental path is discarded by the
	// pending-echo guard in applyTimelineSlice anyway — fetch the full slice
	// directly so the upload bubble/progress lands in a single round-trip.
	//
	// When hiding system messages, always take the full-slice path: the incremental delta's
	// indices are computed over the UNFILTERED timeline and would desync against the filtered
	// list. A full slice is diffed by event id (prepend/append/scroll all preserved).
	if (!_timelineBaselineReady || !_list || _list->messages().isEmpty()
		|| _pendingLocalMedia.hasPendingForRoom(roomId)
		|| shouldHideSystemMessages()) {
		_bridge->getTimelineSliceAsync(roomId, _latestTimelineSliceRequestId);
	} else {
		_bridge->getTimelineUpdateAsync(roomId, _latestTimelineSliceRequestId);
	}
}

void HistoryWidget::applyTimelineSlice(const QString &roomId, TimelineSlice slice) {
	if (_currentRoomId.isEmpty() || roomId != _currentRoomId) {
		return;
	}

	// Messages are already on screen (e.g. from the cached slice); don't keep the
	// centered preloader over them while we re-fetch a clean slice. A pending jump
	// to this room keeps its own preloader until its target is shown (cleared below
	// / on jump completion), so don't clear in that case.
	if (_list && !_list->messages().isEmpty()
		&& !(_pendingJump && _pendingJump->roomId == roomId)) {
		_list->setLoadingTimeline(false);
	}

	if (slice.updateKind != TimelineUpdateKind::Full
		&& (!_timelineBaselineReady || !_list || _list->messages().isEmpty())) {
		_latestTimelineSliceRequestId = _bridge->nextRequestId();
		_bridge->getTimelineSliceAsync(roomId, _latestTimelineSliceRequestId);
		return;
	}
	if (_list && slice.updateKind == TimelineUpdateKind::Append
		&& slice.updateIndex != _list->messages().size()) {
		_latestTimelineSliceRequestId = _bridge->nextRequestId();
		_bridge->getTimelineSliceAsync(roomId, _latestTimelineSliceRequestId);
		return;
	}
	if (slice.updateKind == TimelineUpdateKind::Prepend && slice.updateIndex != 0) {
		_latestTimelineSliceRequestId = _bridge->nextRequestId();
		_bridge->getTimelineSliceAsync(roomId, _latestTimelineSliceRequestId);
		return;
	}

	const auto hasPendingLocalEcho = !_pendingLocalEchoIdsByRequestId.isEmpty()
		|| _pendingLocalMedia.hasPendingForRoom(roomId);
	if (slice.updateKind != TimelineUpdateKind::Full && hasPendingLocalEcho) {
		_latestTimelineSliceRequestId = _bridge->nextRequestId();
		_bridge->getTimelineSliceAsync(roomId, _latestTimelineSliceRequestId);
		return;
	}

	// Hide system/service messages in public rooms. Filtering here, before any bookkeeping, keeps
	// the whole method consistent: unread counting, pinned tracking, structural-change detection and
	// the list all see the same filtered items. onTimelineChanged forces a full slice whenever this
	// is active, so the filtered slice stays a full window that setSlice diffs by event id (prepend,
	// append and scroll position all preserved). System messages are never unread or pinned, so
	// dropping them here changes nothing else.
	if (shouldHideSystemMessages()) {
		// Saved Messages also drops deleted tombstones entirely — a notepad
		// keeps notes, not "message deleted" markers.
		const auto savedRoom = isSavedMessagesRoom();
		slice.items.erase(
			std::remove_if(slice.items.begin(), slice.items.end(),
				[savedRoom](const TimelineItem &m) {
					return isServiceMessage(m)
						|| (savedRoom && m.delivery.deleted);
				}),
			slice.items.end());
		if (savedRoom) {
			// Forwards present as authored by the original sender (tdesktop
			// Saved Messages): substitute the sender identity and drop the
			// outgoing styling so the row renders like an incoming group
			// message — name over the bubble, their avatar, no checkmark.
			// Done here, before bookkeeping, so sender-grouping, the sliding
			// avatar and avatar-click all follow with no paint changes.
			// Forwards older than the identity metadata (senderId empty)
			// keep the plain "Forwarded from" header instead.
			for (auto &item : slice.items) {
				if (!item.forwardedFrom
					|| item.forwardedFrom->senderId.isEmpty()) {
					continue;
				}
				item.sender.id = item.forwardedFrom->senderId;
				item.sender.name = item.forwardedFrom->senderName;
				item.sender.avatarUrl = item.forwardedFrom->avatarUrl;
				item.delivery.outgoing = false;
				item.forwardedFrom.reset();
				// The original author is not a member of this room, so the
				// member-driven avatar path never fetches them — resolve here.
				if (item.sender.avatarUrl.startsWith(QStringLiteral("mxc://"))
					&& MediaCache::needsResolution(item.sender.avatarUrl)) {
					MediaCache::markRequested(item.sender.avatarUrl);
					_bridge->resolveAvatar(item.sender.avatarUrl);
				}
			}
		}
	}

	const auto wasExactlyAtBottom = isScrollNearBottom(_scroll);
    const auto hadMessages = _list && !_list->messages().isEmpty();
    const auto oldMessageCount = hadMessages ? _list->messages().size() : 0;
    const auto oldFirstEventId = hadMessages ? _list->messages().first().eventId : QString();
    const auto oldLastEventId = hadMessages ? _list->messages().last().eventId : QString();
    bool appendedIncomingMessage = false;

    if (_list && slice.isLive && !_list->messages().isEmpty() && !slice.items.isEmpty()) {
        const auto oldLastEventId = _list->messages().last().eventId;
        int matchIndex = -1;
        for (int i = slice.items.size() - 1; i >= 0; --i) {
            if (slice.items[i].eventId == oldLastEventId) {
                matchIndex = i;
                break;
            }
        }
        if (matchIndex >= 0 && matchIndex < slice.items.size() - 1) {
            for (int i = matchIndex + 1; i < slice.items.size(); ++i) {
                if (!slice.items[i].delivery.outgoing) {
                    appendedIncomingMessage = true;
                    break;
                }
            }
        }
    }

    // Enrich with avatar URLs using the cached member map from loadRoomData.
    // Previously this called getRoomMembers() (blocking FFI, up to 5s timeout)
    // on EVERY timeline change, which blocked the Qt main thread and caused
    // the app to hang.  The member cache is populated once in loadRoomData
    // and incrementally updated here when new senders appear.
    for (auto &msg : slice.items) {
        applySenderAvatarFallback(msg, _memberAvatarCache, _controller);
        if (!msg.sender.id.isEmpty() && !msg.sender.avatarUrl.isEmpty()) {
            // Incrementally update the cache with new sender avatar data
            // that the Rust side already enriched in the timeline items.
            _memberAvatarCache.insert(msg.sender.id, msg.sender.avatarUrl);
        }
        // Query the sender's cross-signing trust so an identity-changed
        // (violation) sender surfaces the top-of-room warning bar. No avatar
        // shield is drawn — this only drives violation detection.
        if (_bridge && !msg.sender.id.isEmpty()) {
            _bridge->ensureUserTrust(msg.sender.id);
        }
    }
    // Reapply cached formattedBody for outgoing messages.
    if (!_formattedBodies.isEmpty()) {
        for (auto &msg : slice.items) {
            if (msg.delivery.outgoing && formattedText(msg).isEmpty()) {
                const auto it = _formattedBodies.constFind(msg.eventId);
                if (it != _formattedBodies.constEnd()) {
                    setFormattedText(msg, it.value());
                }
            }
        }
    }
    // Keep optimistic upload echoes visible until the SDK's own echo (same
    // transaction id) appears in a live slice. Big files take a while to ingest,
    // so unrelated slices would otherwise drop the just-sent bubble.
    if (!_optimisticMediaEchoes.isEmpty() && slice.isLive) {
        for (auto it = _optimisticMediaEchoes.begin();
                it != _optimisticMediaEchoes.end();) {
            const auto txid = it.key();
            const auto pending = _pendingLocalMedia.upload(txid);
            if (!pending.has_value()) {
                it = _optimisticMediaEchoes.erase(it); // upload finished/removed
                continue;
            }
            if (pending->roomId != _currentRoomId) {
                ++it; // belongs to another room — it re-injects there
                continue;
            }
            const auto present = std::any_of(slice.items.cbegin(), slice.items.cend(),
                [&txid](const TimelineItem &m) {
                    return m.eventId == txid || m.transactionId == txid;
                });
            if (present || _cancelledUploadIds.contains(txid)) {
                it = _optimisticMediaEchoes.erase(it); // SDK took over (or cancelled)
            } else {
                slice.items.push_back(it.value()); // preserve our bubble
                ++it;
            }
        }
    }

    // Drop upload bubbles the user just cancelled, so they don't flash back
    // between the optimistic removal and the backend cancel (abort/redact).
    // If the SDK's echo for a cancelled upload appears (the optimistic cancel
    // may have fired before the SDK finished reading the file and created its
    // echo), re-issue the real cancel now — once — so it actually aborts.
    if (!_cancelledUploadIds.isEmpty()) {
        slice.items.erase(
            std::remove_if(slice.items.begin(), slice.items.end(),
                [this](const TimelineItem &m) {
                    const auto cancelledId =
                        _cancelledUploadIds.contains(m.eventId) ? m.eventId
                        : (!m.transactionId.isEmpty()
                            && _cancelledUploadIds.contains(m.transactionId))
                            ? m.transactionId
                            : QString();
                    if (cancelledId.isEmpty()) {
                        return false;
                    }
                    if (_bridge && !_cancelRedispatchedIds.contains(cancelledId)) {
                        _cancelRedispatchedIds.insert(cancelledId);
                        _bridge->cancelUpload(_currentRoomId, cancelledId);
                    }
                    return true;
                }),
            slice.items.end());
    }
    dropTransientOutgoingEmptyTextMessages(slice.items);
    dropAcknowledgedOutgoingMediaPlaceholders(slice.items);
    retireAcknowledgedPendingLocalMedia(slice.items);
    hydratePendingLocalMedia(slice.items);
    normalizeDormantLocalMediaPlaceholders(slice.items);

	// Clear the jump/sync preloader now that messages have arrived — UNLESS a jump
	// to this room is still pending and its target isn't in THIS slice yet. A
	// cross-room jump also fires getTimelineSliceAsync, whose live/cached slice
	// lands BEFORE the focused slice; clearing here would hide the centered
	// "Loading…" early (same-room jumps don't fetch that slice, which is why they
	// showed it). The jump-completion below, plus the focus-failure and
	// visibility-timeout handlers, clear it once the jump resolves or fails.
	const auto pendingJumpTargetHere = (_pendingJump && _pendingJump->roomId == roomId)
		? _pendingJump->eventId
		: QString();
	const bool awaitingJumpTarget = !pendingJumpTargetHere.isEmpty()
		&& std::none_of(slice.items.cbegin(), slice.items.cend(),
			[&](const TimelineItem &item) {
				return item.eventId == pendingJumpTargetHere;
			});
	// An empty initial slice with history still to load must NOT drop the loading
	// pill: on a cold start the timeline delivers an empty Full slice before
	// pagination/sync fills it — the room isn't empty, its events just haven't
	// arrived yet. Keep the pill until real content shows (cleared at the top once
	// messages exist) or the timeline is confirmed exhausted (reached its start /
	// nothing more to page back). Otherwise the room reads as blank with no
	// indicator during that window.
	// Not `canPaginateBack && !hitTimelineStart`: a cold, non-resident room's first
	// slice has NO window yet, so both flags default to false — using canPaginateBack
	// would drop the pill during the (multi-second) background window build. The
	// timeline having not reached its start is the reliable "more is coming" signal.
	const auto emptyStillLoading = slice.items.isEmpty()
		&& _list->messages().isEmpty()
		&& !slice.hitTimelineStart;
	if (!awaitingJumpTarget && !emptyStillLoading) {
		_list->setLoadingTimeline(false);
	}

    // When a jump target is active, anchor to IT instead of the first
    // visible message.  This prevents media loading above the target
    // from shifting it in the viewport.
    const auto anchorToJumpTarget = !_focusJumpEventId.isEmpty()
        && !slice.isLive
        && _list->hasMessage(_focusJumpEventId);
    int preJumpY = -1;
    int preScrollTop = _scroll->scrollTop();
    if (anchorToJumpTarget) {
        preJumpY = _list->yForEventId(_focusJumpEventId);
    }

    // Use setSlice for diffing + scroll anchoring (handles prepend, append,
    // and full replacement with automatic scroll position preservation).
	const auto structuralLiveUpdate = slice.isLive
		&& !slice.items.isEmpty()
		&& ((!hadMessages)
			|| slice.items.size() != oldMessageCount
			|| slice.items.first().eventId != oldFirstEventId
			|| slice.items.last().eventId != oldLastEventId);
	const auto pendingInitialScrollForRoom = slice.updateKind == TimelineUpdateKind::Full
		&& _pendingInitialRoomScroll
		&& _pendingInitialRoomScrollRoomId == roomId;
	// Glow is decided per item at paint time from its utdState; the list arms
	// its own shimmer timer when a glowing UTD is present.
	_list->setSlice(slice);
	if (slice.updateKind == TimelineUpdateKind::Full) {
		if (!pendingInitialScrollForRoom || !slice.items.isEmpty()) {
			_timelineBaselineReady = true;
		}
		if (pendingInitialScrollForRoom && !slice.items.isEmpty()) {
			const auto switchingRoom = _pendingInitialRoomScrollSwitchingRoom;
			const auto previousTop = _pendingInitialRoomScrollPreviousTop;
			_pendingInitialRoomScroll = false;
			_pendingInitialRoomScrollRoomId.clear();
			_pendingInitialRoomScrollSwitchingRoom = false;
			_pendingInitialRoomScrollPreviousTop = 0;
			scheduleInitialRoomScroll(
				roomId,
				switchingRoom,
				previousTop,
				slice.isLive);
		} else if (pendingInitialScrollForRoom) {
			_initialScrollPending = false;
		}
	}
	// Override setSlice's scroll anchoring: keep the jump target at the
    // same viewport offset it had before the update.
    if (anchorToJumpTarget && preJumpY >= 0
        && _list->hasMessage(_focusJumpEventId)) {
        const auto postJumpY = _list->yForEventId(_focusJumpEventId);
        if (postJumpY >= 0) {
            const auto viewportOffset = preJumpY - preScrollTop;
            const auto newScrollTop = qBound(
                0, postJumpY - viewportOffset, _scroll->scrollTopMax());
            _scroll->scrollToY(newScrollTop);
        }
    }

    // Request mxc:// resolution only for the newly-arrived messages that are on
    // or near the screen. Runs after setSlice so the rows are laid out and the
    // viewport test is meaningful; off-screen rows are fetched lazily by the
    // paint-armed recheck when scrolled into view (see isMessageNearViewport).
    for (const auto &msg : slice.items) {
        if (isMessageNearViewport(msg.eventId)) {
            requestMessageMediaForRendering(_bridge, msg);
        }
    }

    syncReadMarkingMode();

    // Refresh before any placement below: whether the read boundary is confirmed
    // loaded in this window decides whether canPlaceUnreadBarAt can place the
    // delimiter immediately (see its use in placeUnreadBar). Only slices that
    // actually carry unread state may refresh it — a cold room's live placeholder
    // slice reports readMarkerLoaded=false for lack of data, and letting that
    // clobber a confirmed boundary re-arms the "a read message must precede the
    // anchor" heuristic that the hide-system-messages filter defeats.
    if (slice.unreadStateKnown) {
        _unreadStateKnown = true;
        _readMarkerLoaded = slice.readMarkerLoaded;
    }

    // Feeding the store re-enters applyStoreUnreadState synchronously, which
    // places the delimiter against _list's rows and _readMarkerLoaded — so it must
    // run after setSlice above and after the refresh here, never against the
    // previous slice's state.
    if (_unreadStateStore) {
        _unreadStateStore->applyTimelineSnapshot(roomId, slice);
    }

    applyLiveUnreadState(slice);
    // applyLiveUnreadState's updateUnreadCount calls are conditional (count
    // unchanged → skipped), so push here to cover every live-slice exit.
    pushReadFrontier();
	const auto pendingInitialUnreadEntry = pendingInitialScrollForRoom
		&& _roomUnreadCount > 0;

    // (Re)place the delimiter during initial room entry only. Once the
    // session's delimiter is resolved (_unreadBarResolved, set at the end of
    // entry below) it is frozen for as long as the room stays open: live updates
    // never move it and never make it (re)appear. Leaving the room resets it.
    if (!_unreadBarResolved
        && _optimisticReadTillEventId.isEmpty()
        && _roomUnreadCount > 0
        && _list->firstUnreadEventId().isEmpty()) {
        placeUnreadBar(_roomUnreadCount);
    }

	// Reset pagination debounce flags once the slice has been delivered. Do
	// this before initial unread handling so that startup can chain backward
	// pagination until the real unread boundary is available.
	const bool wasPaginatingBack = _isPaginatingBack;
	_isPaginatingBack = false;
	_isPaginatingForward = false;
	_paginationRetryCount = 0;
	_paginationTimeoutTimer->setInterval(10000);
	_paginationTimeoutTimer->stop();

    // Scroll to first unread on initial room entry.
    // This runs here (not in the deferred lambda) because the unread bar
    // is only set above, after the slice is delivered.
	if (pendingInitialUnreadEntry) {
		_preserveUnreadBarOnEntry = true;
		_initialUnreadScrollNeeded = true;
	}
    if (_initialUnreadScrollNeeded && _list) {
		tryApplyInitialUnreadScroll();
    }

    // Update pinned bar if pinned event IDs changed.
    // Update from both live and focused slices — focused mode preserves
    // the pinned event IDs even though the message window is narrower.
    if (!_pinnedSectionVisible) {
        updatePinnedMessagesFromSlice(slice.pinnedEventIds, slice.items);
	}

    const auto explicitScrollToBottom = _scrollToBottomPending;
    const auto shouldForceBottomOnEntry = _forceBottomEntryUntilLiveSlice
        && slice.isLive;
    const auto shouldPreserveBottomOnLiveUpdate = structuralLiveUpdate
        && wasExactlyAtBottom
        && _windowActive
        && !_preserveUnreadBarOnEntry;
    const auto shouldAutoScrollIncoming = !wasPaginatingBack
        && appendedIncomingMessage
        && _windowActive
        && shouldPreserveBottomOnLiveUpdate;
    if (explicitScrollToBottom
        || shouldForceBottomOnEntry
        || shouldAutoScrollIncoming
        || shouldPreserveBottomOnLiveUpdate) {
        _scrollToBottomPending = false;
        if (shouldForceBottomOnEntry) {
            _forceBottomEntryUntilLiveSlice = false;
        }
        // Scroll to bottom synchronously — a deferred (QueuedConnection) scroll
        // would fire AFTER a prepend's scroll correction, overriding it.
        _scroll->scrollToY(_scroll->scrollTopMax());
        // Arriving at the live tail is explicit "caught up" intent — re-arm read
        // detection if a prior reset had held it. It also settles entry (the
        // disk-first bottom entry lands here, not in scheduleInitialRoomScroll).
        _entryScrollSettled = true;
        syncReadMarkingMode();
        if (_list) {
            _list->resetReadDetectionHold();
        }
        // Mark last message as read and clear unread state optimistically.
        const auto canConsumeReadState = !shouldForceBottomOnEntry
            || _windowActive;
        const auto allowUnsyncedReadConsumption = _synced || _windowActive;
        if (canConsumeReadState
            && slice.unreadStateKnown
            && allowUnsyncedReadConsumption
            && !_currentRoomId.isEmpty()
            && _list
            && !_list->messages().isEmpty()) {
            const auto &lastMsg = _list->messages().last();
            if (_readReceipts.canRequest(lastMsg.eventId)) {
                // Fix #9: set in-flight marker, not the confirmed receipt.
                _readReceipts.clearPending();
                _firstUnreadIndex = -1;
                _optimisticUnreadFrontierEventId.clear();
                _optimisticReadTillEventId = lastMsg.eventId;
                updateUnreadCount(0);
                _unreadBarDismissed = false;
                // Once frozen for the session, an auto-read at the bottom must
                // not remove the delimiter placed at entry — only explicit
                // navigation (down button / send / room switch) clears it.
                if (!_unreadBarResolved) {
                    destroyUnreadBar();
                }
                syncReadMarkingMode();
                sendTrackedReadReceipt(lastMsg.eventId);
            }
        }
    }

    // Freeze the delimiter for the rest of this room session once initial entry
    // (including any unread backfill) has settled: from here live updates never
    // place, move, or clear the drawn delimiter. Reset on room switch.
    if (slice.isLive
        && UnreadBar::shouldResolveOnLiveSlice(
            _unreadStateKnown,
            _initialUnreadScrollNeeded,
            _roomUnreadCount,
            _list->firstUnreadEventId())) {
        _unreadBarResolved = true;
    }

    // Complete a pending scroll restore (non-live room re-entry via focusOnEvent).
    if (_scrollStates.hasPendingRestore()) {
        auto state = _scrollStates.pendingRestore();
        if (!state.anchorEventId.isEmpty() && _list->hasMessage(state.anchorEventId)) {
            for (int i = 0; i < _list->messageCount(); ++i) {
                if (_list->messageAt(i).eventId == state.anchorEventId && i < _list->layoutCount()) {
                    const auto messageY = _list->yForEventId(state.anchorEventId);
                    _scroll->scrollToY(qMax(0, messageY - state.anchorPixelOffset));
                    break;
                }
            }
            _scrollStates.clearPendingRestore();
            _entryScrollSettled = true;
            syncReadMarkingMode();
        }
    }

    // Complete a pending jump once the target event is in the slice. Position it
    // (no animation — the 180ms animation races with later SDK slice snaps) under
    // the cover; the JumpLoadController decides when to reveal (both the 1s floor
    // AND the target must be ready).
    const auto pendingJumpHere = _pendingJump && _pendingJump->roomId == roomId;
    const auto pendingJumpLoaded = pendingJumpHere
        && _list->hasMessage(_pendingJump->eventId);
    if (pendingJumpHere && pendingJumpLoaded) {
        const auto jumpId = _pendingJump->eventId;
        const auto wasPreferLive = _pendingJumpPreferLive;
        _pendingJump.reset();
        _pendingJumpPreferLive = false;
        _focusJumpEventId = jumpId;
        if (wasPreferLive && _list) {
            // Opening a toast is explicit "I am reading this" intent, but the
            // replace that delivered the live slice armed the read-detection
            // hold — without re-arming, the receipt would wait for a manual
            // scroll. Cleared before the scroll so the move it causes detects.
            _list->resetReadDetectionHold();
        }
        const auto y = _list->yForEventId(jumpId);
        if (y >= 0) {
            const auto rh = _list->rowHeightForEventId(jumpId);
            const auto targetY = qBound(
                0,
                y + (qMax(0, rh) / 2) - (_scroll->height() / 2),
                _scroll->scrollTopMax());
            _scroll->scrollToY(targetY);
        }
        if (wasPreferLive && _list) {
            // Re-run detection at the settled position: when the scroll above was
            // a no-op (already there) nothing else would trigger it.
            _list->updateVisibleTop(_scroll->scrollTop());
        }
        // Prevent the queued loadRoomData scroll lambda from overriding this.
        _jumpScrollApplied = true;
        // The jump landed the viewport at its target — entry has settled.
        _entryScrollSettled = true;
        syncReadMarkingMode();
        if (_jumpLoad.active()) {
            if (_jumpLoad.onTargetArrived() == JumpLoadController::Action::Reveal) {
                finishJumpReveal();
            }
            // else: cover stays up; the floor timer will finish the reveal.
        } else {
            // Defensive: a fetched jump with no active cover episode — reveal now.
            _list->setLoadingTimeline(false);
            _list->highlightMessage(jumpId);
        }
    } else if (pendingJumpHere
            && JumpRouting::shouldEscalateToFocusFetch(
                _pendingJumpPreferLive,
                slice.isLive,
                pendingJumpLoaded)) {
        // A notification target that was not at the live tail after all (an old
        // toast, or a burst since it fired): serve it as a real jump. Re-arming
        // via beginFocusFetch spends the preferLive flag, so this fires once.
        const auto jumpId = _pendingJump->eventId;
        enterJumpLoad(jumpId, JumpSource::Normal);
        beginFocusFetch(jumpId);
    }

    // After slice delivery and scroll adjustments, check if the viewport
    // still isn't full and needs more history.  Deferred so layout settles
    // and scrollTopMax() reflects the new content height.
    QTimer::singleShot(0, this, &HistoryWidget::checkPaginationThresholds);

    // Refresh button visibility and arrow direction after slice update.
    // This is essential when live-mode changes (e.g. after returnToLive or
    // after jumpToMessage enters Event mode): the button must appear/disappear
    // and flip its arrow independently of scroll position changes.
    updateCornerButtonPositions();
}

void HistoryWidget::hydratePendingLocalMedia(QVector<TimelineItem> &messages) {
    if (_pendingLocalMedia.isEmpty()) {
        return;
    }

    for (auto &item : messages) {
        if (!item.delivery.outgoing || item.delivery.sendState != SendState::Sending) {
            continue;
        }

        const auto pending = _pendingLocalMedia.upload(item.eventId);
        if (!pending.has_value()) {
            continue;
        }
        if (pending->type != contentType(item)) {
            continue;
        }

        if (pending->type == ContentType::Image) {
            const auto url = mediaUrl(item);
            if (isLocalSendQueueMxc(url)
                && MediaCache::localPath(url).isEmpty()
                && !pending->mediaPath.isEmpty()) {
                MediaCache::insertPath(url, pending->mediaPath);
            }
        } else if (pending->type == ContentType::Video) {
            if (!pending->thumbPath.isEmpty() && !item.eventId.isEmpty()) {
                const auto previewKey = QStringLiteral("vidthumb:") + item.eventId;
                if (MediaCache::loadImage(previewKey).isNull()) {
                    const auto image = MediaCache::loadImage(pending->thumbPath);
                    if (!image.isNull()) {
                        MediaCache::insertImage(previewKey, image);
                    }
                }
                const auto thumb = mediaThumbUrl(item);
                if (isLocalSendQueueMxc(thumb)
                    && MediaCache::localPath(thumb).isEmpty()) {
                    MediaCache::insertPath(thumb, pending->thumbPath);
                }
            }
            const auto url = mediaUrl(item);
            if (isLocalSendQueueMxc(url)
                && MediaCache::localPath(url).isEmpty()
                && !pending->mediaPath.isEmpty()) {
                MediaCache::insertPath(url, pending->mediaPath);
            }
        } else if (pending->type == ContentType::Audio || pending->type == ContentType::File) {
            const auto url = mediaUrl(item);
            if (isLocalSendQueueMxc(url)
                && MediaCache::localPath(url).isEmpty()
                && !pending->mediaPath.isEmpty()) {
                MediaCache::insertPath(url, pending->mediaPath);
            }
        }
    }
}

bool HistoryWidget::isUploadInFlight(const QString &eventId) const {
    // `_pendingLocalMedia` holds our media uploads; it also keeps FAILED ones
    // around for a resend, so the send state decides whether one is still moving.
    if (!_list || eventId.isEmpty() || !_pendingLocalMedia.contains(eventId)) {
        return false;
    }
    for (const auto &msg : _list->messages()) {
        if (msg.eventId == eventId) {
            return msg.delivery.outgoing
                && msg.delivery.sendState == SendState::Sending;
        }
    }
    return false;
}

void HistoryWidget::cancelUploadForEvent(const QString &eventId) {
    if (_currentRoomId.isEmpty() || !_bridge || eventId.isEmpty()) {
        return;
    }
    // Remove the bubble immediately (optimistic) and suppress it from
    // incoming slices until the backend cancel (abort or redact) lands —
    // so cancel feels instant in any upload state and never flashes back.
    _cancelledUploadIds.insert(eventId);
    _optimisticMediaEchoes.remove(eventId);
    if (_list) {
        _list->removeMessage(eventId);
    }
    _bridge->cancelUpload(_currentRoomId, eventId);
    removePendingLocalMediaUpload(eventId);
}

void HistoryWidget::removePendingLocalMediaUpload(const QString &eventId) {
    _optimisticMediaEchoes.remove(eventId);
    const auto pending = _pendingLocalMedia.take(eventId);
    if (pending.has_value()) {
        removeUploadVideoThumb(pending->thumbPath);
        // No-op unless mediaPath is one of our recompressed temp files (never
        // touches the user's original, which lives outside the temp dir).
        removeUploadCompressedImage(pending->mediaPath);
    }
}

void HistoryWidget::clearPendingLocalMediaUploads() {
    for (const auto &pending : _pendingLocalMedia.takeAll()) {
        removeUploadVideoThumb(pending.thumbPath);
        removeUploadCompressedImage(pending.mediaPath);
    }
    _cancelledUploadIds.clear();
    _cancelRedispatchedIds.clear();
    _optimisticMediaEchoes.clear();
}

void HistoryWidget::onNetworkOnlineChanged(bool online) {
    // Drives the "Waiting for network..." upload label (see uploadStatusText).
    HistoryMessage::setUploadsNetworkOnline(online);
    if (_list) {
        _list->update(); // repaint in-flight upload bubbles with the new status
    }
    if (online) {
        resendFailedUploads();
        // Re-attempt a failed/stuck inline video: the network drop may have latched
        // streaming off or truncated an in-flight proxy download. Fresh download now
        // instead of waiting for a user re-click.
        if (_list && _list->inlineVideoPlayer()) {
            _list->inlineVideoPlayer()->onNetworkOnline();
        }
        if (_pinnedList && _pinnedList->inlineVideoPlayer()) {
            _pinnedList->inlineVideoPlayer()->onNetworkOnline();
        }
    }
}

void HistoryWidget::resendFailedUploads() {
    if (!_bridge || _optimisticMediaEchoes.isEmpty()) {
        return;
    }
    // Self-heal on reconnect: re-run every failed upload (any room) with the same
    // transaction id, mirroring the send queue's retry that direct uploads lack.
    for (auto it = _optimisticMediaEchoes.begin();
            it != _optimisticMediaEchoes.end(); ++it) {
        if (it->delivery.sendState != SendState::Failed) {
            continue;
        }
        const auto txid = it.key();
        const auto pending = _pendingLocalMedia.upload(txid);
        if (!pending.has_value()) {
            continue;
        }
        it->delivery.sendState = SendState::Sending;
        it->delivery.uploadProgress = -1.0;
        if (_list) {
            _list->updateMessageSendState(txid, SendState::Sending);
            _list->updateMessageUploadProgress(txid, -1.0);
        }
        _pendingLocalMedia.setUploadPath(txid, pending->mediaPath);
        _bridge->sendMedia(
            pending->roomId, pending->type, pending->mediaPath, pending->mime,
            pending->filename, pending->caption, pending->thumbPath,
            pending->size, pending->width, pending->height,
            pending->durationMs, txid);
    }
}

void HistoryWidget::sendDialogFiles(
        const QVector<PreparedFile> &files,
        const QString &caption,
        bool compress) {
    // Reject files larger than the homeserver's upload limit up front, so the
    // user gets a clear message instead of a silent server-side 413 that would
    // leave the bubble stuck in the Failed state.
    const auto maxUpload = _bridge ? _bridge->maxUploadSize() : 0;
    QVector<PreparedFile> sendable;
    sendable.reserve(files.size());
    for (const auto &file : files) {
        if (maxUpload > 0 && file.size > maxUpload) {
            if (_toast) {
                _toast->showToast(
                    tr("\"%1\" is too large to upload (server limit %2 MB).")
                        .arg(file.filename)
                        .arg((maxUpload + 512 * 1024) / (1024 * 1024)));
            }
            continue;
        }
        sendable.push_back(file);
    }
    if (sendable.isEmpty()) {
        return;
    }

    // Persist the compress choice as the new default — but only when images were
    // actually present, since compress is forced false otherwise and would
    // clobber the stored preference.
    const bool hadImages = std::any_of(sendable.cbegin(), sendable.cend(),
        [](const PreparedFile &f) { return f.kind == PreparedFileKind::Image; });
    if (hadImages && _controller
            && compress != _controller->settings().compressImages()) {
        _controller->settings().setCompressImages(compress);
        _controller->saveSettingsDelayed();
    }

    // Telegram parity: when several files carry one caption, the caption goes
    // out first as its own text message; otherwise it rides the lone file.
    const bool multiWithCaption = (sendable.size() > 1 && !caption.isEmpty());
    if (multiWithCaption) {
        _bridge->sendMessage(_currentRoomId, caption, QString());
    }
    for (const auto &file : sendable) {
        sendPreparedFile(file, multiWithCaption ? QString() : caption, compress);
    }
}

void HistoryWidget::sendPreparedFile(
        const PreparedFile &file,
        const QString &caption,
        bool compress) {
    // Show the bubble IMMEDIATELY — before recompression and before the SDK
    // ingests the file (both scale with file size). The actual upload is
    // dispatched afterwards with the same transaction id and reconciles with
    // this optimistic echo.
    const auto transactionId = generateUploadTransactionId();
    appendOptimisticMediaEcho(file, caption, transactionId);

    // Recompress images off the UI thread: downscaling + JPEG-encoding several
    // large photos would otherwise freeze the UI and serialize sends (Phase
    // 1.2). Non-images, and images with the toggle off, dispatch immediately.
    if (compress && file.kind == PreparedFileKind::Image) {
        const auto roomId = _currentRoomId;
        const auto sourcePath = file.path;
        const auto originalSize = file.size;
        QPointer<HistoryWidget> guard(this);
        static_cast<void>(QtConcurrent::run(
            [guard, roomId, file, sourcePath, caption, originalSize, transactionId]() {
                const auto outPath = createCompressedImagePath();
                RecompressResult result;
                if (!outPath.isEmpty()) {
                    result = recompressImageForUpload(
                        sourcePath, outPath,
                        kCompressMaxEdge, kCompressJpegQuality, originalSize);
                }
                // Fall back to the original when recompression didn't help.
                const auto sendPath = result.ok ? outPath : sourcePath;
                QMetaObject::invokeMethod(qApp,
                    [guard, roomId, file, sendPath, caption, transactionId]() {
                        if (!guard || guard->_currentRoomId != roomId) {
                            // Room switched/closed before we finished: drop our
                            // temp file (no-op for an original) and skip.
                            removeUploadCompressedImage(sendPath);
                            return;
                        }
                        guard->sendPreparedMedia(file, sendPath, caption, transactionId);
                    });
            }));
        return;
    }
    sendPreparedMedia(file, file.path, caption, transactionId);
}

void HistoryWidget::appendOptimisticMediaEcho(
        const PreparedFile &file,
        const QString &caption,
        const QString &transactionId) {
    if (_currentRoomId.isEmpty()) {
        return;
    }
    // Local placeholder mxc (send-queue style so it's treated as a local, not
    // downloadable, URL). The bubble renders its preview from the MediaCache
    // entries registered below; once the SDK creates its own echo (same
    // transaction id) the two reconcile by event id. Built from the ORIGINAL
    // PreparedFile so it's instant even when a recompress is pending.
    const auto localMxc = QStringLiteral("mxc://send-queue.localhost/optimistic-")
        + transactionId;

    TimelineMediaContent mc;
    mc.url = localMxc;
    mc.filename = file.filename;
    mc.caption = caption;
    mc.size = file.size;
    mc.mime = file.mime;
    mc.width = qMax(0, file.width);
    mc.height = qMax(0, file.height);

    auto type = ContentType::File;
    QString thumbPath;
    TimelineContent echoContent;

    if (file.kind == PreparedFileKind::Image) {
        type = ContentType::Image;
        if (!file.preview.isNull()) {
            MediaCache::insertImage(localMxc, file.preview);
        }
        echoContent = TimelineImageContent{ .media = mc };
    } else if (file.kind == PreparedFileKind::Video) {
        type = ContentType::Video;
        mc.durationMs = static_cast<quint64>(qMax(0, file.durationMs));
        if (!file.preview.isNull()) {
            // The dialog already extracted the first frame — reuse it as the
            // upload thumbnail (no second probe).
            thumbPath = createUploadVideoThumbPath();
            if (thumbPath.isEmpty() || !file.preview.save(thumbPath, "JPG", 88)) {
                removeUploadVideoThumb(thumbPath);
                thumbPath.clear();
            } else {
                mc.thumbUrl = localMxc + QStringLiteral("-thumb");
                MediaCache::insertImage(mc.thumbUrl, file.preview);
            }
        }
        echoContent = TimelineVideoContent{ .media = mc };
    } else {
        if (mc.mime.isEmpty()) {
            mc.mime = QStringLiteral("application/octet-stream");
        }
        echoContent = TimelineFileContent{ .media = mc };
    }

    TimelineItem echo;
    echo.eventId = transactionId;
    echo.transactionId = transactionId;
    if (_controller) {
        echo.sender.id = _controller->userId();
        echo.sender.name = _controller->displayName().isEmpty()
            ? tr("Me") : _controller->displayName();
        echo.sender.avatarUrl = _controller->avatarUrl();
    } else {
        echo.sender.name = tr("Me");
    }
    echo.timestamp = QDateTime::currentSecsSinceEpoch();
    echo.delivery.outgoing = true;
    echo.delivery.sendState = SendState::Sending;
    echo.delivery.uploadProgress = -1.0;
    echo.content = echoContent;

    _optimisticMediaEchoes.insert(transactionId, echo);
    _pendingLocalMedia.insert(transactionId, PendingLocalMediaUpload{
        .type = type,
        .mediaPath = file.path,
        // Overwritten by sendPreparedMedia once a recompression settles.
        .uploadPath = file.path,
        .thumbPath = thumbPath,
        .roomId = _currentRoomId,
        .mime = mc.mime,
        .filename = file.filename,
        .caption = caption,
        .size = mc.size,
        .width = mc.width,
        .height = mc.height,
        .durationMs = mc.durationMs,
    });
    if (_list) {
        _list->appendMessage(echo);
        if (_scroll) {
            _scroll->scrollToY(_scroll->scrollTopMax());
        }
    }
}

void HistoryWidget::sendPreparedMedia(
        const PreparedFile &file,
        const QString &sendPath,
        const QString &caption,
        const QString &transactionId) {
    if (_currentRoomId.isEmpty() || sendPath.isEmpty() || !_bridge) {
        return;
    }
    const QFileInfo info(sendPath);
    const auto size = static_cast<quint64>(qMax<qint64>(0, info.size()));

    if (file.kind == PreparedFileKind::Image) {
        // Header-only metadata from the actual send path (cheap; correct for a
        // recompressed image, whose dimensions/mime differ from the original).
        const auto format = QString::fromLatin1(
            QImageReader::imageFormat(sendPath)).toLower();
        const auto prepared = prepareImageUpload(sendPath, QFileInfo(sendPath), format);
        _pendingLocalMedia.setUploadPath(transactionId, sendPath);
        _bridge->sendMedia(
            _currentRoomId, ContentType::Image, sendPath, prepared.mime,
            file.filename, caption, QString(), prepared.fileSize,
            prepared.width, prepared.height, 0, transactionId);
        return;
    }

    if (file.kind == PreparedFileKind::Video) {
        QString thumbPath;
        if (const auto pending = _pendingLocalMedia.upload(transactionId)) {
            thumbPath = pending->thumbPath;
        }
        _bridge->sendMedia(
            _currentRoomId, ContentType::Video, file.path, file.mime, file.filename,
            caption, thumbPath, size, qMax(0, file.width), qMax(0, file.height),
            static_cast<quint64>(qMax(0, file.durationMs)), transactionId);
        return;
    }

    _bridge->sendMedia(
        _currentRoomId, ContentType::File, sendPath,
        file.mime.isEmpty() ? QStringLiteral("application/octet-stream") : file.mime,
        file.filename, caption, QString(), size, 0, 0, 0, transactionId);
}

void HistoryWidget::retireAcknowledgedPendingLocalMedia(
        const QVector<TimelineItem> &messages) {
    if (_pendingLocalMedia.isEmpty()) {
        return;
    }

    QSet<QString> acknowledgedTransactionIds;
    for (const auto &item : messages) {
        if (item.transactionId.isEmpty() || !isRemoteMediaTimelineItem(item)) {
            continue;
        }
        acknowledgedTransactionIds.insert(item.transactionId);
        adoptUploadedMediaAsResolved(item);
    }

    for (const auto &transactionId : acknowledgedTransactionIds) {
        removePendingLocalMediaUpload(transactionId);
    }

    // Pending entries are cleared in the mediaSent success handler.
    // normalizeDormant handles any remaining local echo placeholders
    // that are no longer actively tracked.
}

void HistoryWidget::adoptUploadedMediaAsResolved(const TimelineItem &item) {
    // Our own image, just acknowledged: point its mxc at the very bytes we
    // uploaded, so requestMessageMediaForRendering sees it already resolved and
    // never asks the backend to fetch back the file sitting on this disk.
    // (Rust seeds its own media cache in parallel, for later sessions.)
    // Only images: no other body is fetched on render.
    if (!isImageMessage(item)) {
        return;
    }
    const auto pending = _pendingLocalMedia.upload(item.transactionId);
    if (!pending.has_value() || pending->uploadPath.isEmpty()) {
        return;
    }
    const auto url = mediaUrl(item);
    if (url.isEmpty()
        || isLocalSendQueueMxc(url)
        || MediaCache::isResolved(url)
        || !QFileInfo::exists(pending->uploadPath)) {
        return;
    }
    MediaCache::insertPath(url, pending->uploadPath);
}

void HistoryWidget::normalizeDormantLocalMediaPlaceholders(
        QVector<TimelineItem> &messages) const {
    for (auto &item : messages) {
        if (!isLocalMediaUploadPlaceholder(item)) {
            continue;
        }
        if (_pendingLocalMedia.contains(item.eventId)) {
            continue;
        }
        item.delivery.sendState = SendState::Failed;
        item.delivery.uploadProgress = -1.0;
    }
}

void HistoryWidget::loadRoom(const QString &roomId) {
    // Opening any real room ends a preview, whether it is the one we just joined or another the
    // user clicked in the chat list. Clearing the id makes the leaving-room cleanup below skip it:
    // a room we never joined has no draft, typing notice or scroll state to save.
    if (_previewMode) {
        exitPreviewMode();
        showChatControls(true);
        _currentRoomId.clear();
    }

    // Clear search highlight when switching rooms.
    if (_currentRoomId != roomId && _topBar) {
        _topBar->setSearchActive(false);
    }
    // Detect whether we're switching rooms or just refreshing the same one.
    const bool switchingRoom = (_currentRoomId != roomId);
    const auto previousTop = _scroll->scrollTop();

    // Check if this is an invited room.
    if (switchingRoom) {
        const auto rooms = _bridge->cachedRooms();
        for (const auto &room : rooms) {
            if (room.roomId == roomId
                && room.membership == MembershipState::Invite) {
                _currentRoomId = roomId;
                resetCurrentRoomPermissions();
                showInvitePanel(room);
                return;
            }
        }
    }
    // Normal room — hide invite panel if visible.
    hideInvitePanel();

    if (switchingRoom) {
        if (_input) {
            _input->cancelVoiceRecording();
        }
        // Cancel any in-flight typing notice before leaving the room.
        if (_typingState.outgoingSent() && !_currentRoomId.isEmpty()) {
            _bridge->sendTypingNotice(_currentRoomId, false);
            _typingState.setOutgoingSent(false);
        }
        _typingSendTimer->stop();
        _typingCancelTimer->stop();
        _typingState.clearIncomingUsers();
        _typingDotTimer->stop();
        if (_topBar) {
            _topBar->clearTyping();
        }

        // Do NOT return the leaving room to live: keep its timeline window as
        // it is. If it's in a focused (jumped-to) view, the Rust window stays
        // focused so reopening re-shows that exact position instantly — no
        // rebuild, no drift — the way a manual-scroll position survives a
        // switch. The user returns to live via "Jump to Latest" or by sending.
        // Save scroll position of the room we're leaving.
        saveScrollState();
        // Keep in-flight uploads across the switch (see loadRoom) — they re-show
        // in their own room and are cleaned on completion/failure/cancel.
        _isPaginatingBack = false;
        _isPaginatingForward = false;
        _paginationRetryCount = 0;
        _paginationTimeoutTimer->stop();
        _paginationTimeoutTimer->setInterval(10000);
        // Room switches choose between saved position, unread delimiter,
        // or bottom later in loadRoomData(). Avoid forcing a bottom-first path.
        _scrollToBottomPending = false;
        _readReceipts.resetForRoom(); // Fix #9: clear in-flight marker on room switch
        _optimisticUnreadFrontierEventId.clear();
        _optimisticReadTillEventId.clear();
        _forceBottomEntryUntilLiveSlice = false;
        _preserveUnreadBarOnEntry = false;
        _unreadBarDismissed = false;
        // Fresh room session: re-arm the read-consuming gate from scratch at the
        // new room's first settle (its scroll position decides it).
        _readConsumingGate = false;
        // Fresh room session: the delimiter must be re-decided by initial entry.
        _unreadBarResolved = false;
        _readMarkerLoaded = false;
        _unreadStateKnown = false;
        _unreadEntryAttempts = 0;
        _initialScrollPending = true;
        // Read detection stays disarmed until the new room's entry scroll is
        // applied — the first slices render at a transient viewport position.
        _entryScrollSettled = false;
        if (_readReceiptTimer) {
            _readReceiptTimer->stop();
        }
        if (!_currentRoomId.isEmpty()) {
            const auto draftPreview = _drafts.updateFromInput(_currentRoomId, *_input);
            flushDraftChanged();
            emit draftChanged(_currentRoomId, draftPreview);
        }
    }

    _currentRoomId = roomId;
    if (switchingRoom) {
        resetCurrentRoomPermissions();
        // Public-ness is seeded in loadRoomData(), after its own
        // resetCurrentRoomPermissions() call — seeding here would be wiped by it.
    }
    _focusJumpEventId.clear();
    // Only discard a pending jump if it targets a different room.
    // A cross-room jump via showMessage() sets _pendingJump before calling
    // loadRoom(), so we must preserve it here for loadRoomData() to consume.
    if (_pendingJump && _pendingJump->roomId != roomId) {
        _pendingJump.reset();
        _pendingJumpPreferLive = false;
    }
    _jumpScrollApplied = false;

    // Clear stale pinned bar before showing controls so that
    // showChatControls does not briefly animate a leftover pinned bar
    // from the previous room.  Snap progress to zero immediately —
    // the 200ms slide-out animation is only appropriate when the user
    // dismisses a pin inside the same room.
    // Close pinned section when switching rooms.
    if (switchingRoom && _pinnedSectionVisible) {
        closePinnedSection();
    }

    if (switchingRoom && _pinnedBar) {
        _pinnedBar->clearPinnedMessage();
        if (_pinnedBarAnimation) {
            _pinnedBarAnimation->stop();
        }
        _pinnedBarShownProgress = 0.0;
        _pinnedBar->hide();
    }

    // Hide the pinned button immediately on room switch; updatePinnedMessages-
    // FromSlice re-shows it if the new room actually has pinned messages.
    if (switchingRoom && _topBar) {
        _topBar->setHasPinned(false);
        // Clear the member-sync badge from the previous room; the new room's
        // fetch (if any) re-raises it via memberSyncStateChanged.
        _topBar->setMemberSyncing(false);
    }
    // A different room's pinned list starts at the bottom (newest), like the timeline.
    if (switchingRoom) {
        _pinnedScrollTop = -1;
    }

    if (switchingRoom) {
        // A cross-room JUMP (matrix.to link / search result) keeps its own jump
        // cover until the destination's focused slice arrives. A plain switch has
        // no cover: it clears the timeline at once and shows the "Loading…" pill.
        // A notification open is a plain switch — it never raised a cover, and
        // must not inherit one an interrupted jump left up.
        const bool crossRoomJump = _pendingJump
            && _pendingJump->roomId == roomId
            && !_pendingJumpPreferLive;

        // Clear old room's messages BEFORE making the scroll area visible.
        // HistoryList::resizeEvent calls recalculateLayout() which is O(N)
        // in message count — if we show controls first, the visibility
        // change can trigger a resize on _list with thousands of stale
		// messages, causing a visible freeze.
		_list->setRoomId(roomId);
		_timelineBaselineReady = false;
		// Pin permission: default to false; updated async when room data arrives.
        _list->setCanPinMessages(false);
        _list->setMessages({});
        _list->setLoadingTimeline(true);
        if (!crossRoomJump) {
            // A plain switch (or a switch away from a pending jump) cancels any
            // in-flight jump cover so the normal switch preloader shows instead.
            _jumpLoad.reset();
            _jumpLoadEventId.clear();
            if (_jumpFloorTimer) {
                _jumpFloorTimer->stop();
            }
            _list->setJumpLoadingCover(false);
        }
        _list->setMarkingMessagesRead(false);
        _pinnedState.clear();
        _returnStack.clear();
        _initialUnreadScrollNeeded = false;
        _unreadBarDismissed = false;
        _scrollStates.clearPendingRestore();
        _pinnedState.clearFetchRoomId(); // Invalidate in-flight async fetches.
        // Stop debounce timer to prevent stale onTimelineChanged firing
        // for the old room after the switch.
        if (_timelineDebounce) {
            _timelineDebounce->stop();
        }
    }

    // Show chat controls (hides placeholder).
    showChatControls(true);

    if (switchingRoom) {
        // After showChatControls the scroll area has proper geometry.
        // Resize the list to fill the viewport so the "Syncing..." pill
        // (painted in the vertical center) is actually visible.
        const auto vpw = _scroll->viewport()->width();
        const auto vph = _scroll->viewport()->height();
        if (_list->width() != vpw || _list->height() < vph) {
            _list->resize(vpw, qMax(_list->height(), vph));
        }
        updateControlsGeometry();

        // Restore draft + focus input immediately (uses fallback preview text).
        {
            const auto draft = _drafts.value(roomId);
            _input->cancelEditMode();
            _input->cancelReplyMode();
            if (draft.editMode && !draft.editEventId.isEmpty()) {
                _input->enterEditMode(
                    draft.editEventId,
                    draft.editSenderName,
                    draft.editPreviewText,
                    draft.html);
            } else if (draft.replyMode && !draft.replyEventId.isEmpty()) {
                // Messages not loaded yet — use saved preview as fallback.
                _input->enterReplyMode(
                    draft.replyEventId,
                    draft.replySenderName,
                    draft.replyPreviewText,
                    QString(),
                    draft.replyPreviewPath);
            }
            if (!draft.html.isEmpty()) {
                _input->setFieldHtml(draft.html);
            } else {
                _input->setFieldText(draft.text);
            }
            flushDraftChanged();
            emit draftChanged(roomId, HistoryDraftStore::preview(draft));
        }
        _input->focusInput();

        // Paint the cleared timeline (and its "Loading…" pill) first, then defer
        // the synchronous cache reads so the room switch stays responsive.
        _scroll->repaint();

        QTimer::singleShot(1, this, [this, roomId] {
            if (_currentRoomId != roomId) return;
            loadRoomData(roomId, /*switchingRoom=*/true,
                         /*previousTop=*/0);
        });
        return;
    }

    // Same-room reload: data is cached, run synchronously.
    loadRoomData(roomId, false, previousTop);
}

void HistoryWidget::scheduleInitialRoomScroll(
        const QString &roomId,
        bool switchingRoom,
        int previousTop,
        bool initialSliceIsLive) {
    QMetaObject::invokeMethod(this, [this, switchingRoom, roomId, previousTop, initialSliceIsLive] {
        if (switchingRoom && _currentRoomId != roomId) {
            return;
        }
        // Compute scroll maximum directly from widget/viewport heights
        // to avoid any staleness in QScrollBar::maximum().
        const auto listH = _list->height();
        const auto vpH = _scroll->viewport()->height();
        const auto freshMax = qMax(0, listH - vpH);
        const auto unsyncedEntry = switchingRoom && !_synced;

        if (switchingRoom) {
            _initialScrollPending = false;
            if ((_pendingJump && _pendingJump->roomId == roomId) || _jumpScrollApplied) {
                _jumpScrollApplied = false;
                return;
            }

            // Restore a saved scroll position (first-visible anchor + pixel
            // offset recorded on leave by saveScrollState). Takes priority over
            // the first-unread / last-message fallback below.
            if (_scrollStates.has(roomId)) {
                const auto saved = _scrollStates.value(roomId);
                if (!saved.anchorEventId.isEmpty()) {
                    _forceBottomEntryUntilLiveSlice = false;
                    _initialUnreadScrollNeeded = false;
                    if (_list->hasMessage(saved.anchorEventId)) {
                        // Anchor is in the (persisted) loaded window — restore
                        // instantly, staying live.
                        const auto y = _list->yForEventId(saved.anchorEventId);
                        if (y >= 0) {
                            _scrollStates.clearPendingRestore();
                            _scroll->scrollToY(
                                qBound(0, y - saved.anchorPixelOffset, freshMax));
                            _entryScrollSettled = true;
                            syncReadMarkingMode();
                            return;
                        }
                    }
                    // Anchor isn't loaded — fetch it focused; the pendingRestore
                    // completion in applyTimelineSlice places it (anchor + offset)
                    // when the focused slice arrives.
                    _scrollStates.setPendingRestore(saved);
                    if (_list) {
                        _list->setLoadingTimeline(true);
                    }
                    _bridge->focusOnEvent(roomId, saved.anchorEventId);
                    return;
                }
            }

            // No saved position: jump to the first unread message, otherwise to
            // the last message (bottom).
            if (_roomUnreadCount > 0) {
                _forceBottomEntryUntilLiveSlice = false;
                if (!tryApplyInitialUnreadScroll()) {
                    _preserveUnreadBarOnEntry = true;
                    _initialUnreadScrollNeeded = true;
                    syncReadMarkingMode();
                }
            } else {
                _forceBottomEntryUntilLiveSlice = !initialSliceIsLive;
                _initialUnreadScrollNeeded = false;
                _scroll->scrollToY(freshMax);
                _entryScrollSettled = true;
                syncReadMarkingMode();
            }
        } else {
            // Same-room refresh: preserve the current position.
            _scroll->scrollToY(qBound(0, previousTop, freshMax));
        }

        // After scroll position is applied, check if at bottom and mark read.
        // Live mode only: "at bottom" is a pixel test, and a reopened focused
        // (jumped-to) window can sit at ITS bottom without being at the live
        // tail — marking its last event read there would falsely clear unreads.
        const auto top = _scroll->scrollTop();
        const auto max = freshMax;
        const auto canConsumeUnsyncedEntryRead = unsyncedEntry
            && initialSliceIsLive
            && _windowActive;
        // _unreadStateKnown: on a cold open the room's unread state has not
        // arrived yet, and a public room whose loaded window is all system
        // messages renders zero rows once the filter runs — so max==0 makes the
        // "at bottom" test trivially true. Consuming the read state there zeroes
        // the badge and receipts the newest loaded event while the real unread
        // span is still unknown.
        if ((!unsyncedEntry || canConsumeUnsyncedEntryRead)
            && _unreadStateKnown
            && _list && _list->isLive()
            && !_preserveUnreadBarOnEntry
            && (max <= 0 || top >= max - 1)) {
            if (!_currentRoomId.isEmpty() && _list && !_list->messages().isEmpty()) {
                const auto &lastMsg = _list->messages().last();
                if (_readReceipts.canRequest(lastMsg.eventId)) {
                    // Fix #9: set in-flight marker, not the confirmed receipt.
                    _readReceipts.clearPending();
                    _firstUnreadIndex = -1;
                    _optimisticUnreadFrontierEventId.clear();
                    _optimisticReadTillEventId = lastMsg.eventId;
                    updateUnreadCount(0);
                    sendTrackedReadReceipt(lastMsg.eventId);
                }
            }
        }
    }, Qt::QueuedConnection);
}

void HistoryWidget::loadRoomData(
        const QString &roomId,
        bool switchingRoom,
        int previousTop) {
    // Register per-room timeline change callback (idempotent).
    _bridge->watchTimeline(roomId);

    // Fetch room members asynchronously — results arrive via
    // roomMembersReady signal (connected in the constructor).
    _memberAvatarCache.clear();
    if (_input) {
        _input->setRoomMembers({});
    }
    _bridge->getRoomMembersAsync(roomId);
    if (switchingRoom) {
        resetCurrentRoomPermissions();
        // Seed public-ness from the rooms list synchronously — before any timeline slice arrives
        // (those are async) — so the first slice is filtered correctly instead of showing system
        // messages and then blinking them out when the async room-settings snapshot lands. This must
        // run after resetCurrentRoomPermissions() above, which would otherwise wipe it back to false.
        // roomSettingsReady still corrects this if the cached value is stale.
        for (const auto &room : _bridge->cachedRooms()) {
            if (room.roomId == roomId) {
                _currentRoomIsPublic = room.isPublic;
                break;
            }
        }
    }
    _bridge->getRoomSettings(roomId);

    // Ensure list widget has correct width before layout computation.
    if (_list->width() != _scroll->viewport()->width()) {
        _list->resize(_scroll->viewport()->width(), _list->height());
    }

    _list->setRoomId(roomId);
    syncReadMarkingMode();

    const auto rooms = _bridge->cachedRooms();
    const auto roomIt = std::find_if(
        rooms.cbegin(),
        rooms.cend(),
        [&](const RoomSummary &room) {
            return room.roomId == roomId;
        });
    const auto roomIsDirect = (roomIt != rooms.cend()) && roomIt->isDirect;
    _list->setShowOutgoingPrivateAvatars(roomIsDirect);

    // If switching rooms with a pending jump target, trigger focusOnEvent.
    const bool hasPendingJump = _pendingJump && _pendingJump->roomId == roomId;
    if (switchingRoom && hasPendingJump) {
        if (_pendingJumpPreferLive) {
            // The target is expected at the live tail. The Rust window outlives a
            // room switch, so a room an earlier jump left focused would come back
            // focused — reset it to live instead of focusing it on the target.
            _bridge->returnToLive(roomId);
        } else {
            _bridge->focusOnEvent(roomId, _pendingJump->eventId, _pendingJump->requestId);
        }
        // Show the (light) preloader on the destination timeline while the
        // slice loads; applyTimelineSlice clears it once it arrives.
        if (_list) {
            _list->setLoadingTimeline(true);
        }
    }

    // If switching rooms and a saved scroll state exists, kick off focusOnEvent.
    if (switchingRoom && !hasPendingJump && _scrollStates.has(roomId)) {
        const auto state = _scrollStates.value(roomId);
        if (!state.wasLive && !state.focusEventId.isEmpty()) {
            _bridge->focusOnEvent(roomId, state.focusEventId);
            _scrollStates.setPendingRestore(state);
        }
    }

    // Clear pinned bar BEFORE loading timeline (so updatePinnedMessagesFromSlice works).
    _pinnedState.clear();
    if (_pinnedBar) {
        _pinnedBar->clearPinnedMessage();
        animatePinnedBarVisibility(false);
    }

    updateControlsGeometry();

    if (switchingRoom) {
        // Find room display name from the rooms list.
        QString roomName = roomId;
        for (const auto &room : rooms) {
            if (room.roomId == roomId) {
                roomName = room.displayName;
                break;
            }
        }

        // For direct chats show last-seen status; for groups show member count.
        // Note: messages aren't loaded yet — direct-chat user ID will be
        // updated when onTimelineChanged fires. Encryption status arrives
        // with the room settings snapshot requested above.
        QString subtitle;
        bool foundRoom = false;
        bool roomMuted = false;
        _currentRoomIsDirect = false;
        _currentNotificationMode = RoomNotificationMode::AllMessages;
        _lastSeenTimestamp = 0;
        _directChatUserId.clear();
        if (_topBar) {
            _topBar->setPeerTrust(0);
        }
        _trustWarningDismissed.clear();
        hideTrustWarning();
        for (const auto &room : rooms) {
            if (room.roomId == roomId) {
                foundRoom = true;
                _currentNotificationMode = room.notificationMode;
                roomMuted = isToolbarMuted(room.notificationMode, room.isMuted);
                _list->setCanPinMessages(room.canPinMessages);
                if (room.isDirect) {
                    _currentRoomIsDirect = true;
                    _directChatUserId = room.avatarEntityId;
                    if (_bridge && !_directChatUserId.isEmpty()) {
                        _bridge->userTrustState(_directChatUserId);
                    }
                    subtitle = (room.peerPresence == 1)
                        ? tr("online")
                        : formatLastSeenTimestamp(_lastSeenTimestamp);
                } else {
                    const auto memberCount = qMax(quint64(1), room.memberCount);
                    subtitle = tr("%n member(s)", "", int(memberCount));
                }
                break;
            }
        }
        if (_topBar) {
            _topBar->setDirect(_currentRoomIsDirect);
        }
        if (!foundRoom) {
            subtitle = tr("last seen recently");
        }

        // Saved Messages: bare pinned-style header — title only, no member
        // count, no lock/mute glyphs, no click-through to room info.
        const auto savedRoom = _bridge
            && roomId == _bridge->savedMessagesRoomId();
        if (savedRoom) {
            subtitle.clear();
        }
        _topBar->setSavedMessagesMode(savedRoom);
        _list->setSavedMessagesMode(savedRoom);

        _lastSeenTimer->stop();

        _cachedSubtitle = subtitle;
        _topBar->setRoomInfo(roomName, subtitle);

        // Encryption status will be set when room settings arrive.
        _topBar->setEncrypted(false);
        _topBar->setMuted(roomMuted && !savedRoom);

        // Seed unread count from cached room summary. The first-unread
        // frontier arrives with the async timeline slice below.
        _roomUnreadCount = 0;
        _firstUnreadIndex = -1;
        _optimisticUnreadFrontierEventId.clear();
        _optimisticReadTillEventId.clear();
        // Leaving the previous room: drop its frozen delimiter anchor so the
        // new room captures its own (see history_widget.h). Kept out of the
        // transient count-0 / unload paths, which must preserve the anchor.
        _sessionUnreadBarEventId.clear();
        _unreadBarDismissed = false;
        int roomUnread = 0;
        for (const auto &room : rooms) {
            if (room.roomId == roomId) {
                roomUnread = room.unreadCount;
                break;
            }
        }
        _list->clearUnreadBar();  // clear old room's bar first (before setting new count)
        updateUnreadCount(roomUnread);
	}

	_latestTimelineSliceRequestId = _bridge->nextRequestId();
	_bridge->getTimelineSliceAsync(roomId, _latestTimelineSliceRequestId);

    if (!switchingRoom && _initialScrollPending) {
        // Safety: clear stale flag if it wasn't cleared by the deferred
        // scroll handler (e.g., room switch was interrupted).
        _initialScrollPending = false;
        return;
    }
    if (switchingRoom && !_timelineBaselineReady) {
        _pendingInitialRoomScroll = true;
        _pendingInitialRoomScrollRoomId = roomId;
        _pendingInitialRoomScrollSwitchingRoom = switchingRoom;
        _pendingInitialRoomScrollPreviousTop = previousTop;
    } else {
        scheduleInitialRoomScroll(roomId, switchingRoom, previousTop, true);
    }

    // Focus input field (same-room reload path; switching path handles it above).
    if (!switchingRoom) {
        _input->focusInput();
    }
}

void HistoryWidget::onSendMessage(
    const QString &text,
    const QString &formattedBody,
    const QString &replyToEventId) {
    if (_currentRoomId.isEmpty() || text.isEmpty()) {
        return;
    }

    // If viewing a focused (non-live) slice, return to the live tail before
    // sending.  returnToLive() is non-blocking — the Rust side will fire a
    // timelineChanged signal which will deliver the live slice; the local echo
    // we append below will be reconciled once that slice arrives.
    if (_list && !_list->isLive()) {
        _scrollToBottomPending = true;
        returnToLive();
    }

    // Sending a message implies user is at the bottom — clear unread state.
    _preserveUnreadBarOnEntry = false;
    _unreadBarDismissed = false;
    _list->clearUnreadBar();
    // Sending consumes the unread boundary — clear the frozen anchor too.
    _sessionUnreadBarEventId.clear();
    _firstUnreadIndex = -1;
    _readReceipts.clearPending();
    _optimisticUnreadFrontierEventId.clear();
    _optimisticReadTillEventId.clear();
    updateUnreadCount(0);
    syncReadMarkingMode();

    _drafts.remove(_currentRoomId);

    // Local echo: immediately show the message.
    const auto requestId = _nextSendRequestId++;
    TimelineItem echo;
    echo.eventId = u"local-"_s + QString::number(requestId);
    echo.sender.id = (_controller && !_controller->userId().isEmpty())
        ? _controller->userId()
        : u"@me:local"_s;
    echo.sender.name = (_controller && !_controller->displayName().isEmpty())
        ? _controller->displayName()
        : tr("Me");
    echo.sender.avatarUrl = _controller ? _controller->avatarUrl() : QString();
    echo.content = TimelineTextContent{
        .body = text,
        .formattedBody = formattedBody,
    };
    if (!replyToEventId.isEmpty()) {
        echo.reply = TimelineReplyInfo{ .eventId = replyToEventId };
    }
    echo.timestamp = QDateTime::currentSecsSinceEpoch();
    echo.delivery.sendState = SendState::Sending;
    echo.delivery.outgoing = true;
    if (!formattedBody.isEmpty()) {
        _formattedBodies.insert(echo.eventId, formattedBody);
    }
    _pendingLocalEchoIdsByRequestId.insert(requestId, echo.eventId);
    _list->appendMessage(echo);
    if (_scroll) {
        _scroll->scrollToY(_scroll->scrollTopMax());
    }

    // Send via bridge (async), including formatted HTML if present.
    _bridge->sendMessage(_currentRoomId, text, formattedBody, replyToEventId, requestId);

    // Cancel typing notice immediately after sending.
    _typingCancelTimer->stop();
    if (_typingState.outgoingSent()) {
        _bridge->sendTypingNotice(_currentRoomId, false);
        _typingState.setOutgoingSent(false);
    }

}

void HistoryWidget::resizeEvent(QResizeEvent *e) {
    Ui::RpWidget::resizeEvent(e);
    updateControlsGeometry();
    if (_invitePanel && _invitePanel->isVisible()) {
        const auto topBarH = _topBar ? _topBar->height() : 0;
        const auto shadowH = _topBarShadow ? _topBarShadow->height() : 0;
        const auto panelTop = topBarH + shadowH;
        _invitePanel->setGeometry(0, panelTop, width(), height() - panelTop);
    }
}

void HistoryWidget::dragEnterEvent(QDragEnterEvent *e) {
    if (!e->mimeData()->hasUrls()
        || _currentRoomId.isEmpty()
        || _isInvitedRoom) {
        e->ignore();
        return;
    }
    for (const auto &url : e->mimeData()->urls()) {
        if (url.isLocalFile()) {
            e->acceptProposedAction();
            return;
        }
    }
    e->ignore();
}

void HistoryWidget::dragLeaveEvent(QDragLeaveEvent *e) {
    e->accept();
}

void HistoryWidget::dropEvent(QDropEvent *e) {
    if (!e->mimeData()->hasUrls()
        || _currentRoomId.isEmpty()
        || _isInvitedRoom) {
        e->ignore();
        return;
    }

    QStringList paths;
    for (const auto &url : e->mimeData()->urls()) {
        if (url.isLocalFile()) {
            paths.append(url.toLocalFile());
        }
    }
    if (paths.isEmpty()) {
        e->ignore();
        return;
    }
    e->acceptProposedAction();

    const auto files = prepareFiles(paths);
    if (files.isEmpty()) {
        return;
    }
    auto *dialog = new HistorySendFilesDialog(
        window(), files, _controller->settings().sendSubmitWay(),
        _controller->settings().compressImages());
    if (dialog->exec() == QDialog::Accepted) {
        sendDialogFiles(
            dialog->files(), dialog->caption(), dialog->compressImages());
    }
    dialog->deleteLater();
}

void HistoryWidget::updateControlsGeometry() {
    if (_inUpdateControls) {
        return;
    }
    _inUpdateControls = true;
    const auto preserveBottomAfterResize = _scroll
        && _scroll->isVisible()
        && !_preserveUnreadBarOnEntry
        && (_forceBottomEntryUntilLiveSlice
            || _scrollToBottomPending
            || isScrollNearBottom(_scroll));
    const auto w = width();
    const auto h = height();

    // Placeholder covers the full area.
    _noChat->setGeometry(0, 0, w, h);

    // Top bar at the top.
    _topBar->setGeometry(0, 0, w, kTopBarHeight);
    if (_topBarShadow) {
        _topBarShadow->setGeometry(0, kTopBarHeight, w, 1);
    }

    // Pinned section mode: top bar + pinned scroll area (full height below it).
    if (_pinnedSectionVisible && _pinnedScroll) {
        const auto scrollTop = kTopBarHeight;
        const auto scrollHeight = qMax(0, h - scrollTop);
        _pinnedScroll->setGeometry(0, scrollTop, w, scrollHeight);
        if (_pinnedList) {
            _pinnedList->setMinimumWidth(w);
        }
        _topBar->raise();
        if (_topBarShadow) {
            _topBarShadow->raise();
        }
        _inUpdateControls = false;
        return;
    }

    auto scrollTop = kTopBarHeight;
    if (_pinnedBar) {
        const auto barHeight = st::historyReplyHeight;
        const auto shown = qRound(_pinnedBarShownProgress * barHeight);
        const auto animating = _pinnedBarAnimation
            && _pinnedBarAnimation->state() == QAbstractAnimation::Running;
        _topBar->setDrawSeparator(false);
        if ((shown > 0 || animating) && _pinnedBar->hasPinnedMessage()) {
            if (_pinnedBar->isHidden()) {
                _pinnedBar->show();
            }
            _pinnedBar->setGeometry(
                0,
                scrollTop - (barHeight - shown),
                w,
                barHeight);
        } else {
            _pinnedBar->setGeometry(0, scrollTop - barHeight, w, barHeight);
            _pinnedBar->hide();
        }
        scrollTop += shown;
    }

    // Trust-violation warning bar pushes the timeline down, like the pinned bar.
    if (_trustWarningBar) {
        if (_trustWarningActive) {
            const auto barHeight = st::historyReplyHeight;
            if (_trustWarningBar->isHidden()) {
                _trustWarningBar->show();
            }
            _trustWarningBar->setGeometry(0, scrollTop, w, barHeight);
            _trustWarningBar->raise();
            scrollTop += barHeight;
        } else if (!_trustWarningBar->isHidden()) {
            _trustWarningBar->hide();
        }
    }

    // Bottom strip: normally the composer, but the Join bar in preview mode. It is a real laid-out
    // bar, not an overlay — it reserves height so the timeline above it shrinks to fit.
    int bottomHeight = 0;
    if (_previewMode && _joinBar) {
        bottomHeight = _joinBar->sizeHint().height();
        _joinBar->setGeometry(0, h - bottomHeight, w, bottomHeight);
        _input->setGeometry(0, h, w, _input->height());
        _input->hide();
    } else {
        // Input at the bottom (height is dynamic based on text content).
        const auto fullInputHeight = _input->height();
        const auto inputHeight = qRound(_inputShownProgress * fullInputHeight);
        const auto inputAnimating = _inputVisibilityAnimation
            && _inputVisibilityAnimation->state() == QAbstractAnimation::Running;
        if (inputHeight > 0 || inputAnimating) {
            if (_input->isHidden()) {
                _input->show();
            }
            _input->setGeometry(0, h - inputHeight, w, fullInputHeight);
        } else {
            _input->setGeometry(0, h, w, fullInputHeight);
            _input->hide();
        }
        bottomHeight = inputHeight;
    }

    // Scroll area fills the middle.
    const auto scrollHeight = qMax(0, h - scrollTop - bottomHeight);
    _scroll->setGeometry(0, scrollTop, w, scrollHeight);
    // ScrollArea::resizeEvent already syncs _list width to viewport.

    _topBar->raise();
    if (_topBarShadow) {
        _topBarShadow->raise();
    }

    if (preserveBottomAfterResize) {
        _scroll->scrollToY(_scroll->scrollTopMax());
    }

    // Reposition corner buttons within scroll area.
    updateCornerButtonPositions();
    _inUpdateControls = false;
}

void HistoryWidget::showTrustWarning(const QString &userId) {
    if (userId.isEmpty()
        || _trustWarningDismissed.contains(userId)
        || !_trustWarningBar) {
        return;
    }
    const auto name = _list ? _list->senderName(userId) : QString();
    static_cast<HistoryTrustWarningBar *>(_trustWarningBar)->setUser(userId, name);
    _trustWarningUserId = userId;
    _trustWarningActive = true;
    updateControlsGeometry();
}

void HistoryWidget::hideTrustWarning() {
    if (!_trustWarningActive) {
        return;
    }
    _trustWarningActive = false;
    _trustWarningUserId.clear();
    updateControlsGeometry();
}

// A message's media (sender avatar, inline image/video thumbnail, url-preview
// image) is worth fetching only when the row is on or near the screen. Without
// this gate, back-scrolling a big room requests an avatar for EVERY sender in
// the whole loaded history at once — hundreds of thumbnail GETs, a large share
// 404 for federated media the homeserver has expired, which pegs the CPU, floods
// the log, and hangs shutdown draining the in-flight tasks. One screenful of
// margin each way keeps just-off-screen rows warm; a message not laid out yet
// (index < 0) is skipped here and picked up by the paint-armed recheck once it
// scrolls into view.
bool HistoryWidget::isMessageNearViewport(const QString &eventId) const {
    if (!_list || !_scroll) {
        return false;
    }
    const auto y = _list->yForEventId(eventId);
    if (y < 0) {
        return false;
    }
    const auto h = qMax(0, _list->rowHeightForEventId(eventId));
    const auto scrollTop = _scroll->scrollTop();
    const auto viewH = _scroll->height();
    const auto margin = viewH; // one screenful of look-ahead each direction
    return (y + h) >= (scrollTop - margin) && y <= (scrollTop + viewH + margin);
}

void HistoryWidget::recheckUnresolvedMedia() {
    if (!_list || _currentRoomId.isEmpty()) {
        return;
    }
    for (const auto &msg : _list->messages()) {
        if (isMessageNearViewport(msg.eventId)) {
            requestMessageMediaForRendering(_bridge, msg);
        }
    }
}

void HistoryWidget::openResolvedFile(
        const QString &mxcUrl,
        const QString &filename,
        const QString &mime) {
    auto filePath = MediaCache::localPath(mxcUrl);
    if (filePath.isEmpty()) return;

    const auto downloadsDir = QStandardPaths::writableLocation(
        QStandardPaths::DownloadLocation);
    auto targetName = filename;
    if (targetName.isEmpty()) {
        const auto resolved = MediaCache::resolvedPathForPlayback(
            filePath, targetName, mime);
        targetName = QFileInfo(resolved).fileName();
    }
    const auto targetPath = downloadsDir + QStringLiteral("/") + targetName;

    // Reuse existing file if it matches (same size).
    if (QFile::exists(targetPath)) {
        const auto cachedSize = QFileInfo(filePath).size();
        const auto targetSize = QFileInfo(targetPath).size();
        if (cachedSize == targetSize) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(targetPath));
            return;
        }
    }
    // Copy with original filename for OS file association.
    QFile::remove(targetPath);
    QFile::copy(filePath, targetPath);
    QDesktopServices::openUrl(QUrl::fromLocalFile(targetPath));
}

void HistoryWidget::openMediaViewAtEvent(const QString &eventId) {
    if (!_list || eventId.isEmpty()) {
        return;
    }

    QVector<TimelineItem> mediaItems;
    mediaItems.reserve(_list->messageCount());
    auto clickedIndex = -1;
    for (auto i = 0; i < _list->messageCount(); ++i) {
        const auto &message = _list->messageAt(i);
        if ((!isImageMessage(message) && !isVideoMessage(message))
            || mediaUrl(message).isEmpty()) {
            continue;
        }
        if (message.eventId == eventId) {
            clickedIndex = mediaItems.size();
        }
        mediaItems.push_back(message);
    }
    if (clickedIndex >= 0) {
        // Stop any inline video first so it doesn't play behind the overlay.
        if (auto *player = _list->inlineVideoPlayer();
            player && !player->activeEventId().isEmpty()) {
            MediaCache::setPlaybackPosition(
                player->currentMxc(), player->positionMs());
            player->stop();
        }
        emit openMediaViewRequested(mediaItems, clickedIndex);
    }
}

void HistoryWidget::onFullscreenVideoClosed(
        const QString &mxcUrl, qint64 positionMs) {
    // Only the fullscreen hand-off leaves an inline player alive (paused) for this
    // mxc; other open paths stop their player, so this is a no-op for them. Re-sync
    // the paused player to the position the fullscreen viewer reached.
    for (auto *list : { _list, _pinnedList }) {
        if (!list) {
            continue;
        }
        if (auto *player = list->inlineVideoPlayer();
            player && !player->activeEventId().isEmpty()
                && player->currentMxc() == mxcUrl) {
            player->seekPausedTo(positionMs);
        }
    }
}

void HistoryWidget::updateMessageLists() {
    if (_list) {
        _list->update();
    }
    if (_pinnedList) {
        _pinnedList->update();
    }
}

HistoryList *HistoryWidget::listForEvent(const QString &eventId) const {
    if (_pinnedList && _pinnedScroll && _pinnedScroll->isVisible()
        && _pinnedList->hasMessage(eventId)) {
        return _pinnedList;
    }
    return _list;
}

void HistoryWidget::pushReplyReturn(const QString &eventId) {
    _returnStack.pushReply(eventId);
}

void HistoryWidget::popReplyReturn() {
    while (_returnStack.hasReply()) {
        const auto eventId = _returnStack.takeReply();
        if (eventId.isEmpty()) {
            continue;
        }
        jumpToMessage(eventId);
        return;
    }
}

void HistoryWidget::pushReturnPosition() {
    if (!_list || !_scroll) return;
    HistoryReturnPosition pos;
    const auto scrollTop = _scroll->scrollTop();
    for (int i = 0; i < _list->layoutCount(); ++i) {
        const auto &layout = _list->layoutAt(i);
        if (layout.y + layout.height > scrollTop) {
            if (i < _list->messageCount()) {
                pos.anchorEventId = _list->messageAt(i).eventId;
                const auto messageY = _list->yForEventId(pos.anchorEventId);
                pos.pixelOffset = messageY - scrollTop;
            }
            break;
        }
    }
    _returnStack.pushPosition(pos);
}

void HistoryWidget::popReturnPosition() {
    if (!_returnStack.hasPosition()) return;
    const auto pos = _returnStack.takePosition();
    if (_list && _list->hasMessage(pos.anchorEventId)) {
        const auto y = _list->yForEventId(pos.anchorEventId);
        if (y >= 0) {
            _scroll->scrollToY(qBound(0, y - pos.pixelOffset, _scroll->scrollTopMax()));
            updateCornerButtonPositions();
            return;
        }
    }
    // Anchor not in current slice — focus on it.
    _scrollStates.setPendingRestore(RoomScrollState{
        pos.anchorEventId,
        pos.pixelOffset,
        true,
        QString()
    });
    _bridge->focusOnEvent(_currentRoomId, pos.anchorEventId);
}

} // namespace TeleMatrix

#include "history_widget.moc"

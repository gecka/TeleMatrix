// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>
#include <QVector>

#include <optional>
#include <variant>

namespace TeleMatrix {

enum class ContentType : int {
    Text = 0,
    Image = 1,
    File = 2,
    Video = 3,
    Service = 4,
    Poll = 5,
    Audio = 7,
    UnableToDecrypt = 8,
};

enum class PollKind : int {
    Disclosed = 0,
    Undisclosed = 1,
};

enum class SendState : int {
    Sending = 0,
    Sent = 1,
    Read = 2,
    Failed = 3,
};

/// Preview type for link cards — subset of common web-page card kinds.
enum class PreviewType : int {
    None = 0,
    Article = 1,
    Photo = 2,
    Video = 3,
    Document = 4,
    Profile = 5,
    Group = 6,
    Channel = 7,
};

/// A single poll answer/option for rendering.
struct PollOptionInfo {
    QString id;
    QString text;
    int voteCount = 0;
    int votePercent = 0;   // 0-100, rounded so all options sum to 100
    double filling = 0.0;  // 0.0-1.0, relative to max-voted option
    bool isChosen = false;
    bool isCorrect = false;

    bool operator==(const PollOptionInfo &other) const {
        return id == other.id
            && text == other.text
            && voteCount == other.voteCount
            && votePercent == other.votePercent
            && qFuzzyCompare(1.0 + filling, 1.0 + other.filling)
            && isChosen == other.isChosen
            && isCorrect == other.isCorrect;
    }
};

struct TimelineTextContent {
    QString body;
    QString formattedBody;

    bool operator==(const TimelineTextContent &other) const = default;
};

struct TimelineMediaContent {
    QString body;
    QString url;
    QString mime;
    QString filename;
    QString caption;
    QString thumbUrl;
    QString blurhash;
    quint64 size = 0;
    int width = 0;
    int height = 0;
    quint64 durationMs = 0;

    bool operator==(const TimelineMediaContent &other) const = default;
};

struct TimelineImageContent {
    TimelineMediaContent media;

    bool operator==(const TimelineImageContent &other) const = default;
};

struct TimelineVideoContent {
    TimelineMediaContent media;

    bool operator==(const TimelineVideoContent &other) const = default;
};

struct TimelineFileContent {
    TimelineMediaContent media;

    bool operator==(const TimelineFileContent &other) const = default;
};

struct TimelineAudioContent {
    TimelineMediaContent media;
    bool isVoice = false;
    QByteArray waveform;

    bool operator==(const TimelineAudioContent &other) const = default;
};

struct TimelinePollContent {
    QString question;
    QString subtitle;
    QVector<PollOptionInfo> options;
    int totalVoters = 0;
    int maxSelections = 1;
    bool isClosed = false;
    bool isMultiChoice = false;
    bool isQuiz = false;
    bool hasVoted = false;
    PollKind kind = PollKind::Disclosed;

    bool operator==(const TimelinePollContent &other) const = default;
};

struct TimelineServiceContent {
    QString body;

    bool operator==(const TimelineServiceContent &other) const = default;
};

struct TimelineUnableToDecryptContent {
    QString body;
    int cause = 0;
    int utdState = 0; // 0 = decrypting/glow (cause Unknown); 1 = terminal

    bool operator==(const TimelineUnableToDecryptContent &other) const = default;
};

using TimelineContent = std::variant<
    TimelineTextContent,
    TimelineImageContent,
    TimelineFileContent,
    TimelineVideoContent,
    TimelineAudioContent,
    TimelineServiceContent,
    TimelinePollContent,
    TimelineUnableToDecryptContent>;

struct TimelineSenderInfo {
    QString id;
    QString name;
    QString avatarUrl;

    bool operator==(const TimelineSenderInfo &other) const = default;
};

struct TimelineReplyInfo {
    QString eventId;
    QString senderName;
    QString text;
    QString thumbUrl;
    bool hasThumb = false;
    bool isTextColorized = false;
    bool isDeleted = false;
    bool isUnavailable = false;

    bool operator==(const TimelineReplyInfo &other) const = default;
};

struct TimelineForwardInfo {
    QString senderName;
    /// Original author identity; empty on forwards recorded before it existed.
    QString senderId;
    QString avatarUrl;

    bool operator==(const TimelineForwardInfo &other) const = default;
};

struct TimelineUrlPreviewInfo {
    QString url;
    QString siteName;
    QString title;
    QString description;
    QString imageUrl;
    int imageWidth = 0;
    int imageHeight = 0;
    PreviewType type = PreviewType::None;
    int duration = 0;
    QString author;
    bool hasLargeMedia = false;
    QString siteNameCanonical;

    bool operator==(const TimelineUrlPreviewInfo &other) const = default;
};

struct TimelineEncryptionInfo {
    bool encrypted = false;
    QString decryptionError;

    bool operator==(const TimelineEncryptionInfo &other) const = default;
};

struct TimelineDeliveryInfo {
    SendState sendState = SendState::Sent;
    double uploadProgress = -1.0;
    bool outgoing = false;
    bool deleted = false;

    bool operator==(const TimelineDeliveryInfo &other) const = default;
};

struct TimelineItem;

[[nodiscard]] ContentType contentType(const TimelineItem &item);
[[nodiscard]] const TimelineTextContent *textContent(const TimelineItem &item);
[[nodiscard]] const TimelineMediaContent *mediaContent(const TimelineItem &item);
[[nodiscard]] const TimelineImageContent *imageContent(const TimelineItem &item);
[[nodiscard]] const TimelineVideoContent *videoContent(const TimelineItem &item);
[[nodiscard]] const TimelineFileContent *fileContent(const TimelineItem &item);
[[nodiscard]] const TimelineAudioContent *audioContent(const TimelineItem &item);
[[nodiscard]] const TimelinePollContent *pollContent(const TimelineItem &item);
[[nodiscard]] const TimelineServiceContent *serviceContent(const TimelineItem &item);
[[nodiscard]] const TimelineUnableToDecryptContent *unableToDecryptContent(const TimelineItem &item);

[[nodiscard]] TimelineTextContent *mutableTextContent(TimelineItem &item);
[[nodiscard]] TimelineMediaContent *mutableMediaContent(TimelineItem &item);
[[nodiscard]] TimelineImageContent *mutableImageContent(TimelineItem &item);
[[nodiscard]] TimelineVideoContent *mutableVideoContent(TimelineItem &item);
[[nodiscard]] TimelineFileContent *mutableFileContent(TimelineItem &item);
[[nodiscard]] TimelineAudioContent *mutableAudioContent(TimelineItem &item);
[[nodiscard]] TimelinePollContent *mutablePollContent(TimelineItem &item);

[[nodiscard]] const TimelineReplyInfo *replyInfo(const TimelineItem &item);
[[nodiscard]] TimelineReplyInfo *mutableReplyInfo(TimelineItem &item);
[[nodiscard]] const TimelineForwardInfo *forwardInfo(const TimelineItem &item);
[[nodiscard]] TimelineForwardInfo *mutableForwardInfo(TimelineItem &item);
[[nodiscard]] const TimelineUrlPreviewInfo *urlPreviewInfo(const TimelineItem &item);
[[nodiscard]] TimelineUrlPreviewInfo *mutableUrlPreviewInfo(TimelineItem &item);

[[nodiscard]] QString replyEventId(const TimelineItem &item);
[[nodiscard]] bool hasReply(const TimelineItem &item);
[[nodiscard]] QString forwardedSenderName(const TimelineItem &item);

[[nodiscard]] QString bodyText(const TimelineItem &item);
[[nodiscard]] QString formattedText(const TimelineItem &item);
[[nodiscard]] QString captionText(const TimelineItem &item);
[[nodiscard]] QString mediaUrl(const TimelineItem &item);
[[nodiscard]] QString mediaThumbUrl(const TimelineItem &item);
[[nodiscard]] QString mediaFilename(const TimelineItem &item);
[[nodiscard]] QString mediaMime(const TimelineItem &item);
[[nodiscard]] QString mediaBlurhash(const TimelineItem &item);
[[nodiscard]] quint64 mediaSize(const TimelineItem &item);
[[nodiscard]] int mediaWidth(const TimelineItem &item);
[[nodiscard]] int mediaHeight(const TimelineItem &item);
[[nodiscard]] quint64 mediaDurationMs(const TimelineItem &item);

void setFormattedText(TimelineItem &item, const QString &formattedBody);
void setMediaDurationMs(TimelineItem &item, quint64 durationMs);

[[nodiscard]] bool isTextMessage(const TimelineItem &item);
[[nodiscard]] bool isMediaMessage(const TimelineItem &item);
[[nodiscard]] bool isImageMessage(const TimelineItem &item);
[[nodiscard]] bool isVideoMessage(const TimelineItem &item);
[[nodiscard]] bool isFileMessage(const TimelineItem &item);
[[nodiscard]] bool isAudioMessage(const TimelineItem &item);
[[nodiscard]] bool isVoiceMessage(const TimelineItem &item);
[[nodiscard]] bool isServiceMessage(const TimelineItem &item);
[[nodiscard]] bool isUnableToDecryptMessage(const TimelineItem &item);

/// True iff the item is UTD and still in the decrypting/glow state (utdState==0).
/// Note this is necessary but not sufficient for the shimmer: an unverified
/// session never glows, since nothing can arrive for it (see itemGlowActive).
[[nodiscard]] bool isUtdGlowing(const TimelineItem &item);

} // namespace TeleMatrix

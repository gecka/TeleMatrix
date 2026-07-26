// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "timeline_item_content.h"

#include "protocol_types.h"

namespace TeleMatrix {
namespace {

template <typename T>
[[nodiscard]] const T *variantContent(const TimelineItem &item) {
    return std::get_if<T>(&item.content);
}

template <typename T>
[[nodiscard]] T *mutableVariantContent(TimelineItem &item) {
    return std::get_if<T>(&item.content);
}

[[nodiscard]] const TimelineMediaContent *typedMediaContent(const TimelineItem &item) {
    if (const auto image = imageContent(item)) {
        return &image->media;
    }
    if (const auto file = fileContent(item)) {
        return &file->media;
    }
    if (const auto video = videoContent(item)) {
        return &video->media;
    }
    if (const auto audio = audioContent(item)) {
        return &audio->media;
    }
    return nullptr;
}

[[nodiscard]] TimelineMediaContent *mutableTypedMediaContent(TimelineItem &item) {
    if (auto image = mutableImageContent(item)) {
        return &image->media;
    }
    if (auto file = mutableFileContent(item)) {
        return &file->media;
    }
    if (auto video = mutableVideoContent(item)) {
        return &video->media;
    }
    if (auto audio = mutableAudioContent(item)) {
        return &audio->media;
    }
    return nullptr;
}

} // namespace

ContentType contentType(const TimelineItem &item) {
    if (std::holds_alternative<TimelineImageContent>(item.content)) {
        return ContentType::Image;
    } else if (std::holds_alternative<TimelineFileContent>(item.content)) {
        return ContentType::File;
    } else if (std::holds_alternative<TimelineVideoContent>(item.content)) {
        return ContentType::Video;
    } else if (std::holds_alternative<TimelineAudioContent>(item.content)) {
        return ContentType::Audio;
    } else if (std::holds_alternative<TimelineServiceContent>(item.content)) {
        return ContentType::Service;
    } else if (std::holds_alternative<TimelinePollContent>(item.content)) {
        return ContentType::Poll;
    } else if (std::holds_alternative<TimelineUnableToDecryptContent>(item.content)) {
        return ContentType::UnableToDecrypt;
    }
    return ContentType::Text;
}

const TimelineTextContent *textContent(const TimelineItem &item) {
    return variantContent<TimelineTextContent>(item);
}

const TimelineMediaContent *mediaContent(const TimelineItem &item) {
    return typedMediaContent(item);
}

const TimelineImageContent *imageContent(const TimelineItem &item) {
    return variantContent<TimelineImageContent>(item);
}

const TimelineVideoContent *videoContent(const TimelineItem &item) {
    return variantContent<TimelineVideoContent>(item);
}

const TimelineFileContent *fileContent(const TimelineItem &item) {
    return variantContent<TimelineFileContent>(item);
}

const TimelineAudioContent *audioContent(const TimelineItem &item) {
    return variantContent<TimelineAudioContent>(item);
}

const TimelinePollContent *pollContent(const TimelineItem &item) {
    return variantContent<TimelinePollContent>(item);
}

const TimelineServiceContent *serviceContent(const TimelineItem &item) {
    return variantContent<TimelineServiceContent>(item);
}

const TimelineUnableToDecryptContent *unableToDecryptContent(const TimelineItem &item) {
    return variantContent<TimelineUnableToDecryptContent>(item);
}

TimelineTextContent *mutableTextContent(TimelineItem &item) {
    return mutableVariantContent<TimelineTextContent>(item);
}

TimelineMediaContent *mutableMediaContent(TimelineItem &item) {
    return mutableTypedMediaContent(item);
}

TimelineImageContent *mutableImageContent(TimelineItem &item) {
    return mutableVariantContent<TimelineImageContent>(item);
}

TimelineVideoContent *mutableVideoContent(TimelineItem &item) {
    return mutableVariantContent<TimelineVideoContent>(item);
}

TimelineFileContent *mutableFileContent(TimelineItem &item) {
    return mutableVariantContent<TimelineFileContent>(item);
}

TimelineAudioContent *mutableAudioContent(TimelineItem &item) {
    return mutableVariantContent<TimelineAudioContent>(item);
}

TimelinePollContent *mutablePollContent(TimelineItem &item) {
    return mutableVariantContent<TimelinePollContent>(item);
}

const TimelineReplyInfo *replyInfo(const TimelineItem &item) {
    if (item.reply) {
        return &*item.reply;
    }
    return nullptr;
}

TimelineReplyInfo *mutableReplyInfo(TimelineItem &item) {
    if (item.reply) {
        return &*item.reply;
    }
    return nullptr;
}

const TimelineForwardInfo *forwardInfo(const TimelineItem &item) {
    if (item.forwardedFrom) {
        return &*item.forwardedFrom;
    }
    return nullptr;
}

TimelineForwardInfo *mutableForwardInfo(TimelineItem &item) {
    if (item.forwardedFrom) {
        return &*item.forwardedFrom;
    }
    return nullptr;
}

const TimelineUrlPreviewInfo *urlPreviewInfo(const TimelineItem &item) {
    if (item.urlPreview) {
        return &*item.urlPreview;
    }
    return nullptr;
}

TimelineUrlPreviewInfo *mutableUrlPreviewInfo(TimelineItem &item) {
    if (item.urlPreview) {
        return &*item.urlPreview;
    }
    return nullptr;
}

QString replyEventId(const TimelineItem &item) {
    if (const auto reply = replyInfo(item)) {
        return reply->eventId;
    }
    return QString();
}

bool hasReply(const TimelineItem &item) {
    return !replyEventId(item).isEmpty();
}

QString forwardedSenderName(const TimelineItem &item) {
    if (const auto forwarded = forwardInfo(item)) {
        return forwarded->senderName;
    }
    return QString();
}

QString bodyText(const TimelineItem &item) {
    if (const auto text = textContent(item)) {
        return text->body;
    } else if (const auto service = serviceContent(item)) {
        return service->body;
    } else if (const auto unable = unableToDecryptContent(item)) {
        return unable->body;
    } else if (const auto poll = pollContent(item)) {
        return poll->question;
    } else if (const auto media = typedMediaContent(item)) {
        return media->body;
    }
    return QString();
}

QString formattedText(const TimelineItem &item) {
    if (const auto text = textContent(item)) {
        return text->formattedBody;
    }
    return QString();
}

QString captionText(const TimelineItem &item) {
    if (const auto media = typedMediaContent(item)) {
        return media->caption;
    }
    return QString();
}

QString mediaUrl(const TimelineItem &item) {
    if (const auto media = typedMediaContent(item)) {
        return media->url;
    }
    return QString();
}

QString mediaThumbUrl(const TimelineItem &item) {
    if (const auto media = typedMediaContent(item)) {
        return media->thumbUrl;
    }
    return QString();
}

QString mediaFilename(const TimelineItem &item) {
    if (const auto media = typedMediaContent(item)) {
        return media->filename;
    }
    return QString();
}

QString mediaMime(const TimelineItem &item) {
    if (const auto media = typedMediaContent(item)) {
        return media->mime;
    }
    return QString();
}

QString mediaBlurhash(const TimelineItem &item) {
    if (const auto media = typedMediaContent(item)) {
        return media->blurhash;
    }
    return QString();
}

quint64 mediaSize(const TimelineItem &item) {
    if (const auto media = typedMediaContent(item)) {
        return media->size;
    }
    return 0;
}

int mediaWidth(const TimelineItem &item) {
    if (const auto media = typedMediaContent(item)) {
        return media->width;
    }
    return 0;
}

int mediaHeight(const TimelineItem &item) {
    if (const auto media = typedMediaContent(item)) {
        return media->height;
    }
    return 0;
}

quint64 mediaDurationMs(const TimelineItem &item) {
    if (const auto media = typedMediaContent(item)) {
        return media->durationMs;
    }
    return 0;
}

void setFormattedText(TimelineItem &item, const QString &formattedBody) {
    if (auto text = mutableTextContent(item)) {
        text->formattedBody = formattedBody;
    }
}

void setMediaDurationMs(TimelineItem &item, quint64 durationMs) {
    if (auto media = mutableTypedMediaContent(item)) {
        media->durationMs = durationMs;
    }
}

bool isTextMessage(const TimelineItem &item) {
    return contentType(item) == ContentType::Text;
}

bool isMediaMessage(const TimelineItem &item) {
    switch (contentType(item)) {
    case ContentType::Image:
    case ContentType::File:
    case ContentType::Video:
    case ContentType::Audio:
        return true;
    case ContentType::Text:
    case ContentType::Service:
    case ContentType::Poll:
    case ContentType::UnableToDecrypt:
        return false;
    }
    return false;
}

bool isImageMessage(const TimelineItem &item) {
    return contentType(item) == ContentType::Image;
}

bool isVideoMessage(const TimelineItem &item) {
    return contentType(item) == ContentType::Video;
}

bool isFileMessage(const TimelineItem &item) {
    return contentType(item) == ContentType::File;
}

bool isAudioMessage(const TimelineItem &item) {
    return contentType(item) == ContentType::Audio;
}

bool isVoiceMessage(const TimelineItem &item) {
    if (const auto audio = audioContent(item)) {
        return audio->isVoice;
    }
    return false;
}

bool isServiceMessage(const TimelineItem &item) {
    return contentType(item) == ContentType::Service;
}

bool isUnableToDecryptMessage(const TimelineItem &item) {
    return contentType(item) == ContentType::UnableToDecrypt;
}

bool isUtdGlowing(const TimelineItem &item) {
    const auto utd = unableToDecryptContent(item);
    return utd && utd->utdState == 0;
}

} // namespace TeleMatrix

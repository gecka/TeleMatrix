// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_prepared_upload.h"

#include "heic_decode.h"

#include <QEventLoop>
#include <QFileInfo>
#include <QImageReader>
#include <QSize>
#include <QMediaPlayer>
#include <QMimeDatabase>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoSink>

namespace TeleMatrix {

namespace {

// High enough that the send-files dialog preview (~340pt wide) stays crisp on
// retina (2x), and the optimistic upload bubble looks sharp too. The dialog
// scales this down to device pixels (see ImagePreviewWidget).
constexpr int kPreviewMaxDimension = 960;

/// Files that Qt can decode as images but should be treated as documents.
/// Matches the Rust-side logic in convert_message_type (pdf as file).
[[nodiscard]] bool isDocumentMime(const QString &mime, const QString &filename) {
    static const QStringList kDocumentMimes = {
        QStringLiteral("application/pdf"),
        QStringLiteral("application/vnd.ms-powerpoint"),
        QStringLiteral("application/vnd.openxmlformats-officedocument.presentationml.presentation"),
        QStringLiteral("application/msword"),
        QStringLiteral("application/vnd.openxmlformats-officedocument.wordprocessingml.document"),
        QStringLiteral("application/vnd.ms-excel"),
        QStringLiteral("application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"),
    };
    for (const auto &dm : kDocumentMimes) {
        if (mime == dm) return true;
    }
    static const QStringList kDocumentExts = {
        QStringLiteral(".pdf"),
        QStringLiteral(".ppt"), QStringLiteral(".pptx"),
        QStringLiteral(".doc"), QStringLiteral(".docx"),
        QStringLiteral(".xls"), QStringLiteral(".xlsx"),
    };
    const auto lower = filename.toLower();
    for (const auto &ext : kDocumentExts) {
        if (lower.endsWith(ext)) return true;
    }
    return false;
}

} // namespace

PreparedFile prepareFile(const QString &path) {
    PreparedFile result;
    result.path = path;

    const QFileInfo info(path);
    result.filename = info.fileName();
    result.size = static_cast<quint64>(qMax<qint64>(0, info.size()));

    QMimeDatabase mimeDb;
    result.mime = mimeDb.mimeTypeForFile(path).name();

    // Check if it is an image (but not a document Qt happens to decode).
    // Decide whether this is an image. HEIC/HEIF must be keyed off the file
    // EXTENSION: Qt's content sniff misses them — and can even mis-detect them
    // as another format — while the deployed qmacheif plugin decodes them fine
    // when handed the format explicitly. Other types use content detection and
    // only fall back to the extension when sniffing returns nothing.
    const auto sniffedFormat = QString::fromLatin1(
        QImageReader::imageFormat(path)).toLower();
    const auto ext = info.suffix().toLower();
    const bool isHeic = (ext == QStringLiteral("heic")
        || ext == QStringLiteral("heif"));
    QString format;
    bool byExtension = false;
    if (isHeic) {
        format = ext;
        byExtension = true;
    } else if (!sniffedFormat.isEmpty()) {
        format = sniffedFormat;
    } else {
        static const QStringList kImageExtensions = {
            QStringLiteral("jpg"),  QStringLiteral("jpeg"),
            QStringLiteral("png"),  QStringLiteral("gif"),
            QStringLiteral("webp"), QStringLiteral("bmp"),
            QStringLiteral("tif"),  QStringLiteral("tiff"),
            QStringLiteral("jp2") };
        if (kImageExtensions.contains(ext)) {
            format = (ext == QStringLiteral("jpg"))
                ? QStringLiteral("jpeg")
                : ext;
            byExtension = true;
        }
    }
    if (!format.isEmpty() && !isDocumentMime(result.mime, result.filename)) {
        result.kind = PreparedFileKind::Image;
        result.mime = QStringLiteral("image/%1").arg(format);

        // HEIC/HEIF: decode natively (macOS ImageIO). The bundled Qt HEIF plugin
        // doesn't decode reliably at runtime, so QImageReader would return null
        // and the file would fall through as a generic document.
        if (isHeic) {
            QSize original;
            result.preview = DecodeHeicNative(
                path, kPreviewMaxDimension, &original);
            if (original.isValid()) {
                result.width = original.width();
                result.height = original.height();
            }
        }

        if (result.preview.isNull()) {
            // Construct in place: QImageReader is non-copyable/non-movable, so a
            // ternary-initialised temporary is ill-formed on MSVC. An empty format
            // means auto-detect (identical to the single-argument constructor).
            QImageReader reader(
                path, byExtension ? format.toLatin1() : QByteArray());
            reader.setAutoTransform(true);
            if (!byExtension) {
                reader.setDecideFormatFromContent(true);
            }

            const auto imageSize = reader.size();
            if (imageSize.isValid()) {
                result.width = imageSize.width();
                result.height = imageSize.height();
            }

            // Load a scaled preview.
            if (imageSize.width() > kPreviewMaxDimension
                    || imageSize.height() > kPreviewMaxDimension) {
                reader.setScaledSize(imageSize.scaled(
                    kPreviewMaxDimension,
                    kPreviewMaxDimension,
                    Qt::KeepAspectRatio));
            }
            result.preview = reader.read();

            if (result.preview.isNull()) {
                // Fallback: read full and scale. Construct in place — QImageReader is
                // non-copyable, so ternary-initialised copies fail on MSVC.
                QImageReader r2(
                    path, byExtension ? format.toLatin1() : QByteArray());
                r2.setAutoTransform(true);
                auto full = r2.read();
                if (!full.isNull()) {
                    result.width = full.width();
                    result.height = full.height();
                    result.preview = full.scaled(
                        kPreviewMaxDimension,
                        kPreviewMaxDimension,
                        Qt::KeepAspectRatio,
                        Qt::SmoothTransformation);
                }
            }
        }
        return result;
    }

    // Check if it is a video.
    if (result.mime.startsWith(QStringLiteral("video/"))) {
        result.kind = PreparedFileKind::Video;

        // Extract the first frame (thumbnail) + dimensions + duration in one
        // QMediaPlayer pass, so the send path can reuse them without a second,
        // slower probe. Quit once both the frame and the duration are known, or
        // after the timeout.
        QMediaPlayer player;
        QVideoSink sink;
        player.setVideoOutput(&sink);

        QImage frame;
        QEventLoop loop;
        const auto maybeFinish = [&] {
            if (!frame.isNull() && result.durationMs > 0) {
                loop.quit();
            }
        };
        QObject::connect(&sink, &QVideoSink::videoFrameChanged,
            &loop, [&](const QVideoFrame &f) {
                const auto img = f.toImage();
                if (!img.isNull() && frame.isNull()) {
                    frame = img;
                    maybeFinish();
                }
            });
        QObject::connect(&player, &QMediaPlayer::durationChanged,
            &loop, [&](qint64 duration) {
                if (duration > 0) {
                    result.durationMs = static_cast<int>(duration);
                    maybeFinish();
                }
            });
        QObject::connect(&player, &QMediaPlayer::errorOccurred,
            &loop, [&] { loop.quit(); });
        // Timeout after 2 seconds.
        QTimer::singleShot(2000, &loop, [&] { loop.quit(); });

        player.setSource(QUrl::fromLocalFile(path));
        player.play();
        loop.exec();
        player.stop();

        if (result.durationMs <= 0 && player.duration() > 0) {
            result.durationMs = static_cast<int>(player.duration());
        }
        if (!frame.isNull()) {
            result.width = frame.width();
            result.height = frame.height();
            if (frame.width() > kPreviewMaxDimension
                    || frame.height() > kPreviewMaxDimension) {
                result.preview = frame.scaled(
                    kPreviewMaxDimension,
                    kPreviewMaxDimension,
                    Qt::KeepAspectRatio,
                    Qt::SmoothTransformation);
            } else {
                result.preview = frame;
            }
        }
        result.durationMs = static_cast<int>(player.duration());
        return result;
    }

    // Otherwise it is a generic file.
    result.kind = PreparedFileKind::File;
    return result;
}

QVector<PreparedFile> prepareFiles(const QStringList &paths) {
    QVector<PreparedFile> result;
    result.reserve(paths.size());
    for (const auto &path : paths) {
        result.append(prepareFile(path));
    }
    return result;
}

} // namespace TeleMatrix

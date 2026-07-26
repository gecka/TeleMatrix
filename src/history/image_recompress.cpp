// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "image_recompress.h"

#include "heic_decode.h"

#include <QColorSpace>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>

namespace TeleMatrix {

QSize applyOrientationToSize(
        QSize rawSize,
        QImageIOHandler::Transformations transformation) {
    // The Rotate90 bit is set for every quarter-turn orientation
    // (Rotate90, Rotate270, and the mirrored-and-rotated variants); those swap
    // the stored width and height. Mirror/Flip/Rotate180 keep them.
    if (transformation & QImageIOHandler::TransformationRotate90) {
        return rawSize.transposed();
    }
    return rawSize;
}

RecompressResult recompressImageForUpload(
        const QString &sourcePath,
        const QString &outputPath,
        int maxEdge,
        int quality,
        quint64 originalSize) {
    RecompressResult result;

    // HEIC/HEIF decode natively via macOS ImageIO (orientation-corrected and
    // scaled to maxEdge): the bundled Qt HEIF plugin doesn't decode reliably at
    // runtime, so relying on it would make compress fail and fall back to
    // sending the raw, often-unrenderable HEIC. Other formats use Qt's reader.
    const auto ext = QFileInfo(sourcePath).suffix().toLower();
    const bool isHeic = (ext == QStringLiteral("heic")
        || ext == QStringLiteral("heif"));

    QImage image;
    if (isHeic) {
        image = DecodeHeicNative(sourcePath, maxEdge > 0 ? maxEdge : 0);
        if (image.isNull()) {
            return result; // can't decode -> caller sends the original
        }
    } else {
        QImageReader reader(sourcePath);
        reader.setAutoTransform(true); // bake EXIF orientation into the pixels
        reader.setDecideFormatFromContent(true);

        // Never flatten a multi-frame source (animated GIF/WebP/APNG, multi-page
        // TIFF) — re-encoding to a single JPEG would silently drop the animation
        // / extra pages. imageCount() is 1 for still images and -1 when unknown.
        if (reader.imageCount() > 1) {
            return result;
        }

        image = reader.read();
        if (image.isNull()) {
            return result;
        }
    }

    // Downscale so the longest edge is at most maxEdge; never upscale.
    const int longest = qMax(image.width(), image.height());
    if (maxEdge > 0 && longest > maxEdge) {
        image = image.scaled(
            maxEdge, maxEdge,
            Qt::KeepAspectRatio,
            Qt::SmoothTransformation);
    }

    // JPEG has no alpha; flatten onto white so transparency doesn't turn black.
    if (image.hasAlphaChannel()) {
        QImage flattened(image.size(), QImage::Format_RGB32);
        flattened.fill(Qt::white);
        QPainter p(&flattened);
        p.drawImage(0, 0, image);
        p.end();
        image = flattened;
    }

    // Normalize to sRGB so other clients render the same colors (the JPEG
    // writer does not embed arbitrary ICC profiles).
    if (image.colorSpace().isValid()
            && image.colorSpace() != QColorSpace(QColorSpace::SRgb)) {
        image.convertToColorSpace(QColorSpace::SRgb);
    }

    QImageWriter writer(outputPath, "jpeg");
    writer.setQuality(quality);
    if (!writer.write(image)) {
        QFile::remove(outputPath);
        return result;
    }

    const auto outSize = static_cast<quint64>(
        qMax<qint64>(0, QFileInfo(outputPath).size()));
    if (originalSize > 0 && outSize >= originalSize) {
        // Recompression didn't help — discard it; caller sends the original.
        QFile::remove(outputPath);
        return result;
    }

    result.ok = true;
    result.size = outSize;
    result.width = image.width();
    result.height = image.height();
    return result;
}

} // namespace TeleMatrix

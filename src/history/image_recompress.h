// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QImageIOHandler>
#include <QSize>
#include <QString>

namespace TeleMatrix {

/// Apply an EXIF orientation to a header-read image size. Quarter-turn
/// rotations (90/270, and their mirrored variants) swap width and height;
/// all other transforms leave the size unchanged.
[[nodiscard]] QSize applyOrientationToSize(
    QSize rawSize,
    QImageIOHandler::Transformations transformation);

/// Result of recompressImageForUpload.
struct RecompressResult {
    bool ok = false;   // true only when a smaller JPEG was written to outputPath
    quint64 size = 0;  // output file size in bytes
    int width = 0;     // oriented output width
    int height = 0;    // oriented output height
};

/// Downscale `sourcePath` so its longest edge is at most `maxEdge` (never
/// upscales), bake in EXIF orientation, flatten any alpha onto white, and
/// re-encode as JPEG at `quality` into `outputPath`. Returns ok=false and
/// leaves no file at `outputPath` when the source can't be decoded, the
/// encode fails, or (when `originalSize` > 0) the result is not smaller than
/// the original — in which case the caller should send the original file.
[[nodiscard]] RecompressResult recompressImageForUpload(
    const QString &sourcePath,
    const QString &outputPath,
    int maxEdge,
    int quality,
    quint64 originalSize);

} // namespace TeleMatrix

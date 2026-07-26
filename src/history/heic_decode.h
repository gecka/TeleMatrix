// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>

namespace TeleMatrix {

/// Decode a HEIC/HEIF image natively via macOS ImageIO, bypassing Qt's HEIF
/// plugin (which is bundled but doesn't decode reliably at runtime, so HEIC
/// otherwise falls through as a generic file / blank bubble). Returns an
/// orientation-corrected QImage whose longest edge is <= maxEdge (full size when
/// maxEdge <= 0); if `originalSize` is non-null it receives the source's
/// orientation-adjusted dimensions. Returns a null QImage off macOS or on
/// failure (caller falls back to QImageReader).
///
/// Two overloads: from a file on disk (upload/preview), and from in-memory bytes
/// (downloaded timeline media — see MediaCache::decodeImageBytes).
[[nodiscard]] QImage DecodeHeicNative(
    const QString &path,
    int maxEdge,
    QSize *originalSize = nullptr);

[[nodiscard]] QImage DecodeHeicNative(
    const QByteArray &bytes,
    int maxEdge,
    QSize *originalSize = nullptr);

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "heic_decode.h"

#include <QFile>

#ifdef Q_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#endif

namespace TeleMatrix {

#ifdef Q_OS_MACOS
namespace {

// EXIF orientation values 5..8 are the 90/270-degree rotations that swap W/H.
[[nodiscard]] bool orientationSwapsAxes(int orientation) {
    return orientation >= 5 && orientation <= 8;
}

// Core decode: build a CGImageSource from in-memory bytes and produce an
// orientation-corrected QImage scaled to maxEdge (full size when maxEdge <= 0).
QImage decodeFromData(const QByteArray &bytes, int maxEdge, QSize *originalSize) {
    if (bytes.isEmpty()) {
        return {};
    }

    CFDataRef data = CFDataCreate(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8 *>(bytes.constData()),
        bytes.size());
    if (!data) {
        return {};
    }
    CGImageSourceRef source = CGImageSourceCreateWithData(data, nullptr);
    CFRelease(data);
    if (!source) {
        return {};
    }

    if (originalSize) {
        *originalSize = QSize();
        if (CFDictionaryRef props =
                CGImageSourceCopyPropertiesAtIndex(source, 0, nullptr)) {
            int w = 0;
            int h = 0;
            int orientation = 1;
            if (auto n = static_cast<CFNumberRef>(
                    CFDictionaryGetValue(props, kCGImagePropertyPixelWidth))) {
                CFNumberGetValue(n, kCFNumberIntType, &w);
            }
            if (auto n = static_cast<CFNumberRef>(
                    CFDictionaryGetValue(props, kCGImagePropertyPixelHeight))) {
                CFNumberGetValue(n, kCFNumberIntType, &h);
            }
            if (auto n = static_cast<CFNumberRef>(
                    CFDictionaryGetValue(props, kCGImagePropertyOrientation))) {
                CFNumberGetValue(n, kCFNumberIntType, &orientation);
            }
            *originalSize = orientationSwapsAxes(orientation)
                ? QSize(h, w)
                : QSize(w, h);
            CFRelease(props);
        }
    }

    // Thumbnail API gives us a downscaled, orientation-baked image in one step.
    CFMutableDictionaryRef options = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(options,
        kCGImageSourceCreateThumbnailFromImageAlways, kCFBooleanTrue);
    CFDictionarySetValue(options,
        kCGImageSourceCreateThumbnailWithTransform, kCFBooleanTrue);
    if (maxEdge > 0) {
        CFNumberRef num = CFNumberCreate(
            kCFAllocatorDefault, kCFNumberIntType, &maxEdge);
        CFDictionarySetValue(options,
            kCGImageSourceThumbnailMaxPixelSize, num);
        CFRelease(num);
    }
    CGImageRef cgImage = CGImageSourceCreateThumbnailAtIndex(source, 0, options);
    CFRelease(options);
    CFRelease(source);
    if (!cgImage) {
        return {};
    }

    const size_t width = CGImageGetWidth(cgImage);
    const size_t height = CGImageGetHeight(cgImage);
    if (width == 0 || height == 0) {
        CGImageRelease(cgImage);
        return {};
    }

    QImage result(int(width), int(height), QImage::Format_ARGB32_Premultiplied);
    result.fill(0);
    CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    // ARGB32_Premultiplied + premultiplied-first + host byte order is the
    // canonical CoreGraphics->QImage pixel layout on macOS.
    CGContextRef ctx = CGBitmapContextCreate(
        result.bits(), width, height, 8, result.bytesPerLine(), colorSpace,
        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host);
    CGColorSpaceRelease(colorSpace);
    if (!ctx) {
        CGImageRelease(cgImage);
        return {};
    }
    CGContextDrawImage(
        ctx, CGRectMake(0, 0, CGFloat(width), CGFloat(height)), cgImage);
    CGContextRelease(ctx);
    CGImageRelease(cgImage);

    if (originalSize && !originalSize->isValid()) {
        *originalSize = result.size();
    }
    return result;
}

} // namespace

QImage DecodeHeicNative(const QByteArray &bytes, int maxEdge, QSize *originalSize) {
    return decodeFromData(bytes, maxEdge, originalSize);
}

QImage DecodeHeicNative(const QString &path, int maxEdge, QSize *originalSize) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray bytes = file.readAll();
    file.close();
    return decodeFromData(bytes, maxEdge, originalSize);
}

#else // !Q_OS_MACOS

QImage DecodeHeicNative(const QByteArray &, int, QSize *) {
    return {};
}

QImage DecodeHeicNative(const QString &, int, QSize *) {
    return {};
}

#endif

} // namespace TeleMatrix

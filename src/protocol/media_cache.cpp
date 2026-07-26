// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "media_cache.h"

#include "history/heic_decode.h"
#include "protocol/media_retry_policy.h"

#include <QApplication>
#include <QBuffer>
#include <QColorSpace>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QQueue>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent>
#include <functional>
#include <list>
#include <utility>

namespace TeleMatrix {

namespace {

// O(1) LRU order for the caches: move-to-back (touch) and dedup are both
// constant-time, unlike a QQueue whose removeOne is a linear scan paid on every
// cache hit during paint. front() == least-recently-used (the trim victim).
struct LruOrder {
    std::list<QString> seq;
    QHash<QString, std::list<QString>::iterator> pos;

    void touch(const QString &k) {
        auto h = pos.find(k);
        if (h != pos.end()) {
            seq.erase(h.value());
        }
        seq.push_back(k);
        pos[k] = std::prev(seq.end());
    }
    bool isEmpty() const { return seq.empty(); }
    QString dequeue() {
        const auto k = seq.front();
        seq.pop_front();
        pos.remove(k);
        return k;
    }
    void clear() {
        seq.clear();
        pos.clear();
    }
};

static constexpr int kMaxPixmapCacheSize = 500;
static constexpr qsizetype kMaxPixmapCacheBytes = 64 * 1024 * 1024;

// Avatars get their own pool so a media-scroll storm can't evict every rooms-list
// avatar (which would trigger a re-decode wave on the next list paint).
static constexpr int kMaxAvatarPixmapCacheSize = 2000;
static constexpr qsizetype kMaxAvatarPixmapCacheBytes = 32 * 1024 * 1024;

static LruOrder &avatarPixmapInsertionOrder() {
    static LruOrder order;
    return order;
}

static LruOrder &pixmapInsertionOrder() {
    static LruOrder order;
    return order;
}

// 400 entries of real photos/thumbs. Entries are now capped by capForTimeline,
// so the byte budget can be far smaller than when it held full-res decodes.
// Blurhash placeholders live in their own placeholderImageCache (below).
static constexpr int kMaxImageCacheSize = 400;
static constexpr qsizetype kMaxImageCacheBytes = 64 * 1024 * 1024;

static LruOrder &imageInsertionOrder() {
    static LruOrder order;
    return order;
}

// Tiny blur/synthblur placeholders get their own pool so a fast-scroll flood of
// 12 KB blurhashes can't evict real photos by entry count, and vice-versa.
static constexpr int kMaxPlaceholderImageCacheSize = 2000;
static constexpr qsizetype kMaxPlaceholderImageCacheBytes = 8 * 1024 * 1024;

static LruOrder &placeholderImageInsertionOrder() {
    static LruOrder order;
    return order;
}

// Blur/synthblur placeholder keys route to the placeholder pool.
static bool isPlaceholderKey(const QString &key) {
    return key.startsWith(QLatin1String("blur:"))
        || key.startsWith(QLatin1String("synthblur:"));
}

QHash<QString, QPixmap> &avatarPixmapCache() {
    static QHash<QString, QPixmap> cache;
    return cache;
}

QHash<QString, QImage> &placeholderImageCache() {
    static QHash<QString, QImage> cache;
    return cache;
}

// Decode-free size probe: records a decoded image's original dimensions so
// layout can size a media bubble without a sync decode, and survives imageCache
// eviction (tiny QSize entries). Filled by the decode paths.
QHash<QString, QSize> &probeSizeCache() {
    static QHash<QString, QSize> cache;
    return cache;
}

void recordProbeSize(const QString &url, QSize size) {
    if (url.isEmpty() || size.isEmpty()) {
        return;
    }
    auto &cache = probeSizeCache();
    if (cache.size() >= 8192 && !cache.contains(url)) {
        cache.clear(); // bound; entries are ~tiny and repopulate on decode
    }
    cache.insert(url, size);
}

static QSet<QString> &asyncLoadingSet() {
    static QSet<QString> set;
    return set;
}

static quint64 &cacheGeneration() {
    static quint64 value = 0;
    return value;
}

static constexpr int kMaxScaledImageCacheSize = 500;
static constexpr qsizetype kMaxScaledImageCacheBytes = 96 * 1024 * 1024;

static LruOrder &scaledImageInsertionOrder() {
    static LruOrder order;
    return order;
}

static constexpr int kMaxMemoryBlobCacheSize = 64;
static constexpr qsizetype kMaxMemoryBlobCacheBytes = 48 * 1024 * 1024;

static LruOrder &memoryBlobInsertionOrder() {
    static LruOrder order;
    return order;
}

qsizetype imageBytes(const QImage &image) {
    return image.isNull() ? 0 : image.sizeInBytes();
}

qsizetype blobBytes(const MediaCache::MemoryBlob &blob) {
    return blob.bytes.size();
}

qsizetype pixmapBytes(const QPixmap &pixmap) {
    if (pixmap.isNull()) {
        return 0;
    }
    const auto bytesPerPixel = (qMax(1, pixmap.depth()) + 7) / 8;
    return qsizetype(pixmap.width()) * pixmap.height() * bytesPerPixel;
}

// Cap a decoded image at a display ceiling before it enters the timeline
// imageCache. Full resolution stays available via sourceBytesCache / disk (the
// viewer decodes that path via loadFullImage), so this trades no visible quality
// at timeline sizes for a large drop in resident decoded-pixel memory. A 4K
// photo (~33 MB ARGB) collapses to <=1600px (~10 MB worst case, 1-4 MB typical).
static constexpr int kTimelineDecodeCeilingPx = 1600; // device px, long edge

QImage capForTimeline(QImage img) {
    if (img.isNull()) {
        return img;
    }
    const auto longEdge = qMax(img.width(), img.height());
    if (longEdge <= kTimelineDecodeCeilingPx) {
        return img;
    }
    return img.scaled(
        kTimelineDecodeCeilingPx,
        kTimelineDecodeCeilingPx,
        Qt::KeepAspectRatio,
        Qt::SmoothTransformation);
}

template <typename Cache, typename SizeFn>
void trimCache(Cache &cache, LruOrder &order, int maxEntries, qsizetype maxBytes, SizeFn sizeFn) {
    qsizetype bytes = 0;
    for (auto it = cache.cbegin(); it != cache.cend(); ++it) {
        bytes += sizeFn(it.value());
    }
    while ((cache.size() > maxEntries || bytes > maxBytes) && !order.isEmpty()) {
        const auto oldest = order.dequeue();
        auto it = cache.find(oldest);
        if (it == cache.end()) {
            continue;
        }
        bytes -= sizeFn(it.value());
        cache.erase(it);
    }
}

QString deterministicRemotePath([[maybe_unused]] const QString &url) {
    // Persistent Matrix media cache files are encrypted by the Rust backend.
    // They must be resolved through ProtocolBridge so Rust can decrypt into a
    // transient plaintext file for Qt image/video APIs.
    return {};
}

// Move-to-back for the LRU insertion order. Called on every cache hit so the
// trim (which evicts from the front) drops the least-recently-USED entry, not
// merely the oldest-inserted — otherwise a still-visible image is evicted on
// the same schedule as an idle one. O(1) via LruOrder; also de-dupes.
void touchOrder(LruOrder &order, const QString &key) {
    order.touch(key);
}

// Compact retention tier for image-class media: the original encoded bytes,
// keyed identically to imageCache. Encoded JPEG/PNG is ~10-50x smaller than the
// decoded ARGB32 frame, so even this modest budget holds a far larger working
// set than imageCache and lets an evicted decoded image rebuild locally at full
// resolution. Kept deliberately small: it is purely additive memory on top of
// the decoded caches, so it trades a bounded ~64MB for no reload/blink on
// reopen of recently-seen rooms (hundreds of thumbnails / dozens of photos).
static constexpr int kMaxSourceBytesCacheSize = 512;
static constexpr qsizetype kMaxSourceBytesCacheBytes = 64 * 1024 * 1024;

static LruOrder &sourceBytesInsertionOrder() {
    static LruOrder order;
    return order;
}

// Dedicated, bounded pool for off-thread image decode+scale. Caps concurrency
// so a fast-scroll storm of cache misses cannot saturate the global pool (which
// also serves unrelated QtConcurrent work). Half the cores, clamped to [2, 4].
QThreadPool &decodePool() {
    static QThreadPool pool;
    static bool configured = false;
    if (!configured) {
        const auto ideal = QThread::idealThreadCount();
        pool.setMaxThreadCount(qBound(2, ideal / 2, 4));
        configured = true;
    }
    return pool;
}

// Decode encoded image bytes to an sRGB QImage (window backing store is sRGB).
// Shared by insertImageBytes and the byte-sourced async decoders.
QImage decodeImageBytes(const QByteArray &bytes, const QString &mime) {
    if (bytes.isEmpty()) {
        return {};
    }
    // HEIC/HEIF: Qt's bundled HEIF plugin doesn't decode reliably at runtime, so
    // go straight to the macOS-native decoder when the MIME says so.
    const auto lowerMime = mime.toLower();
    if (lowerMime.contains(QLatin1String("heic"))
            || lowerMime.contains(QLatin1String("heif"))) {
        QImage native = TeleMatrix::DecodeHeicNative(bytes, 0);
        if (!native.isNull()) {
            return native;
        }
    }
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return QImage::fromData(bytes);
    }
    QImageReader reader(&buffer);
    if (!mime.isEmpty()) {
        const auto subtype = mime.section(QLatin1Char('/'), 1, 1).toUtf8();
        if (!subtype.isEmpty()) {
            reader.setFormat(subtype);
        }
    }
    reader.setAutoTransform(true);
    auto img = reader.read();
    if (img.isNull()) {
        img = QImage::fromData(bytes);
    }
    // Last resort: the native decoder (returns null for non-HEIC, so harmless) —
    // catches HEIC that arrived without a usable MIME hint.
    if (img.isNull()) {
        img = TeleMatrix::DecodeHeicNative(bytes, 0);
    }
    if (!img.isNull() && img.colorSpace().isValid()
            && img.colorSpace() != QColorSpace(QColorSpace::SRgb)) {
        img.convertToColorSpace(QColorSpace::SRgb);
    }
    return img;
}

// Single process-global recheck slot, tagged with its owner so a stale
// teardown cannot clear a newer owner's hook (see setMediaRecheckHook).
struct RecheckHook {
    const void *owner = nullptr;
    std::function<void()> fn;
};

RecheckHook &mediaRecheckHook() {
    static RecheckHook hook;
    return hook;
}

// Ask the host to re-resolve unresolved visible media (throttled on its side).
// Called when a paint-time load dead-ends with nothing cached, no retained
// bytes, and no disk path — the only way an evicted-with-no-bytes URL would
// otherwise recover is a room reopen.
void requestMediaRecheck() {
    if (const auto &fn = mediaRecheckHook().fn) {
        fn();
    }
}

// A sized cache key is "<base>:<WxH>@<dpr>". Match on the ':' boundary so one
// media id can't be matched by another that merely extends its string
// (e.g. ".../abc" vs ".../abcd"). Cheaper than building `base + ':'` per probe.
bool keyHasBase(const QString &key, const QString &base) {
    return key.size() > base.size()
        && key.at(base.size()) == QLatin1Char(':')
        && key.startsWith(base);
}

} // namespace

QHash<QString, QString> &MediaCache::pathCache() {
    static QHash<QString, QString> cache;
    return cache;
}

QHash<QString, QImage> &MediaCache::imageCache() {
    static QHash<QString, QImage> cache;
    return cache;
}

QHash<QString, QPixmap> &MediaCache::pixmapCache() {
    static QHash<QString, QPixmap> cache;
    return cache;
}

QHash<QString, QImage> &MediaCache::scaledImageCache() {
    static QHash<QString, QImage> cache;
    return cache;
}

QHash<QString, MediaCache::MemoryBlob> &MediaCache::memoryBlobCache() {
    static QHash<QString, MemoryBlob> cache;
    return cache;
}

QHash<QString, MediaCache::MemoryBlob> &MediaCache::sourceBytesCache() {
    static QHash<QString, MemoryBlob> cache;
    return cache;
}

void MediaCache::setMediaRecheckHook(const void *owner, std::function<void()> hook) {
    mediaRecheckHook() = RecheckHook{owner, std::move(hook)};
}

void MediaCache::clearMediaRecheckHook(const void *owner) {
    if (mediaRecheckHook().owner == owner) {
        mediaRecheckHook() = RecheckHook{};
    }
}

QHash<QString, bool> &MediaCache::requestedSet() {
    static QHash<QString, bool> set;
    return set;
}

QHash<QString, MediaCache::DownloadState> &MediaCache::downloadStates() {
    static QHash<QString, DownloadState> states;
    return states;
}

QHash<QString, qint64> &MediaCache::playbackPositions() {
    static QHash<QString, qint64> positions;
    return positions;
}

QHash<QString, MediaCache::ResolveFailure> &MediaCache::resolveFailures() {
    static QHash<QString, ResolveFailure> failures;
    return failures;
}

QHash<QString, int> &MediaCache::decodeFailureCounts() {
    static QHash<QString, int> counts;
    return counts;
}

namespace {
// Overridable clock for backoff timing (tests inject a controllable one).
std::function<qint64()> &mediaClockHook() {
    static std::function<qint64()> hook;
    return hook;
}
} // namespace

qint64 MediaCache::nowMs() {
    const auto &hook = mediaClockHook();
    return hook ? hook() : QDateTime::currentMSecsSinceEpoch();
}

void MediaCache::setClockForTesting(std::function<qint64()> clock) {
    mediaClockHook() = std::move(clock);
}

void MediaCache::clearFailureState(const QString &key) {
    resolveFailures().remove(key);
    decodeFailureCounts().remove(key);
}

void MediaCache::noteResolveFailed(const QString &key) {
    if (key.isEmpty()) {
        return;
    }
    auto &failure = resolveFailures()[key];
    failure.attempts += 1;
    failure.lastFailureAtMs = nowMs();
}

void MediaCache::noteResolvePermanentlyFailed(const QString &key) {
    if (key.isEmpty()) {
        return;
    }
    auto &failure = resolveFailures()[key];
    failure.attempts += 1;
    failure.lastFailureAtMs = nowMs();
    failure.permanent = true;
}

bool MediaCache::retrySuppressed(const QString &key) {
    const auto it = resolveFailures().constFind(key);
    if (it == resolveFailures().cend()) {
        return false;
    }
    return !MediaRetryPolicy::retryAllowed(
        it->permanent, it->attempts, it->lastFailureAtMs, nowMs());
}

void MediaCache::noteDecodeFailed(const QString &key) {
    if (key.isEmpty()) {
        return;
    }
    decodeFailureCounts()[key] += 1;
}

bool MediaCache::decodeSuppressed(const QString &key) {
    const auto it = decodeFailureCounts().constFind(key);
    if (it == decodeFailureCounts().cend()) {
        return false;
    }
    return !MediaRetryPolicy::decodeAllowed(it.value());
}

bool MediaCache::mediaUnavailable(const QString &key) {
    // "Unavailable" means the fetch is truly finished with no image to show, so the
    // caller paints a static skeleton and stops the loading glow. That is ONLY a
    // permanent (terminal 4xx) resolve failure or an exhausted decode budget.
    //
    // A transient failure that is merely backing off is NOT unavailable: it will
    // retry, so it stays "loading" and the caller keeps the skeleton pulsing until
    // it resolves or turns permanent. (Previously a 2nd transient failure flipped
    // this to a static placeholder mid-backoff, freezing the glow while a retry was
    // still pending.)
    if (decodeSuppressed(key)) {
        return true;
    }
    const auto it = resolveFailures().constFind(key);
    if (it == resolveFailures().cend()) {
        return false;
    }
    return it->permanent;
}

bool MediaCache::shouldGlowWhileLoading(const QString &key) {
    // Gone or undecodable: nothing is coming, so never animate.
    if (mediaUnavailable(key)) {
        return false;
    }
    const auto it = resolveFailures().constFind(key);
    if (it == resolveFailures().cend()) {
        return true; // never failed — genuinely loading
    }
    // Still retrying, but a pulsing placeholder repaints its row at ~60fps and a
    // transient failure never becomes permanent, so a host that just keeps timing out
    // would pulse forever. Go static after a few failures; retries continue quietly.
    return MediaRetryPolicy::glowAllowed(it->permanent, it->attempts);
}

QString MediaCache::previewImageKey(const QString &url) {
    return url.startsWith(QStringLiteral("mxc://"))
        ? (QStringLiteral("previewthumb:") + url)
        : url;
}

QString MediaCache::localPath(const QString &mxcUrl) {
    const auto cached = pathCache().value(mxcUrl);
    if (!cached.isEmpty()) {
        if (QFileInfo::exists(cached)) {
            return cached;
        }
        pathCache().remove(mxcUrl);
    }
    if (isRemoteCacheKey(mxcUrl)) {
        return deterministicRemotePath(mxcUrl);
    }
    return {};
}

void MediaCache::insertPath(const QString &mxcUrl, const QString &path) {
    pathCache().insert(mxcUrl, path);
    requestedSet().remove(mxcUrl);
    downloadStates().remove(mxcUrl);
    clearFailureState(mxcUrl); // resolved successfully — forget past failures
    // Invalidate cached images so they reload from the new path.
    imageCache().remove(mxcUrl);
    memoryBlobCache().remove(mxcUrl);
    // Remove all pixmap cache entries for this URL (any size variant).
    auto &pxCache = pixmapCache();
    auto it = pxCache.begin();
    while (it != pxCache.end()) {
        if (keyHasBase(it.key(), mxcUrl)) {
            it = pxCache.erase(it);
        } else {
            ++it;
        }
    }
    auto &siCache = scaledImageCache();
    auto sit = siCache.begin();
    while (sit != siCache.end()) {
        if (keyHasBase(sit.key(), mxcUrl)) {
            sit = siCache.erase(sit);
        } else {
            ++sit;
        }
    }
}

bool MediaCache::isResolved(const QString &mxcUrl) {
    return !localPath(mxcUrl).isEmpty()
        || imageCache().contains(mxcUrl)
        || sourceBytesCache().contains(mxcUrl)
        || memoryBlobCache().contains(mxcUrl);
}

bool MediaCache::needsResolution(const QString &mxcUrl) {
    return !isResolved(mxcUrl)
        && !requestedSet().contains(mxcUrl)
        && !retrySuppressed(mxcUrl);
}

void MediaCache::markRequested(const QString &mxcUrl) {
    requestedSet().insert(mxcUrl, true);
}

bool MediaCache::isRequested(const QString &mxcUrl) {
    return requestedSet().contains(mxcUrl);
}

void MediaCache::clearRequested(const QString &mxcUrl) {
    requestedSet().remove(mxcUrl);
    downloadStates().remove(mxcUrl);
}

void MediaCache::setDownloadState(const QString &mxcUrl, const DownloadState &state) {
    if (mxcUrl.isEmpty()) {
        return;
    }
    downloadStates().insert(mxcUrl, state);
}

bool MediaCache::hasDownloadState(const QString &mxcUrl) {
    return downloadStates().contains(mxcUrl);
}

MediaCache::DownloadState MediaCache::downloadState(const QString &mxcUrl) {
    return downloadStates().value(mxcUrl);
}

void MediaCache::clearDownloadState(const QString &mxcUrl) {
    downloadStates().remove(mxcUrl);
}

void MediaCache::setPlaybackPosition(const QString &mxcUrl, qint64 positionMs) {
    if (mxcUrl.isEmpty()) {
        return;
    }
    playbackPositions().insert(mxcUrl, positionMs);
}

qint64 MediaCache::takePlaybackPosition(const QString &mxcUrl) {
    const auto it = playbackPositions().constFind(mxcUrl);
    if (it == playbackPositions().cend()) {
        return -1;
    }
    const auto ms = it.value();
    playbackPositions().erase(it);
    return ms;
}

bool MediaCache::isRemoteCacheKey(const QString &url) {
    return url.startsWith(QStringLiteral("mxc://"))
        || url.startsWith(QStringLiteral("srvthumb:"))
        || url.startsWith(QStringLiteral("previewthumb:"));
}

QString MediaCache::filePathForRead(
        const QString &url,
        bool *waitingForResolution) {
    if (waitingForResolution) {
        *waitingForResolution = false;
    }
    auto cached = pathCache().value(url);
    if (!cached.isEmpty()) {
        if (QFileInfo::exists(cached)) {
            return cached;
        }
        pathCache().remove(url);
        cached.clear();
    }
    if (isRemoteCacheKey(url)) {
        const auto deterministic = deterministicRemotePath(url);
        if (!deterministic.isEmpty()) {
            return deterministic;
        }
        if (waitingForResolution) {
            *waitingForResolution = true;
        }
        return {};
    }
    return url;
}

QImage MediaCache::loadImage(const QString &url) {
    if (url.isEmpty()) {
        return {};
    }

    // Blur/synthblur placeholders live only in their in-memory pool (never on
    // disk or in sourceBytesCache); a miss means the caller must re-decode.
    if (isPlaceholderKey(url)) {
        auto &cache = placeholderImageCache();
        auto it = cache.find(url);
        if (it != cache.end()) {
            touchOrder(placeholderImageInsertionOrder(), url);
            return *it;
        }
        return {};
    }

    auto &cache = imageCache();
    auto it = cache.find(url);
    if (it != cache.end()) {
        touchOrder(imageInsertionOrder(), url);
        return *it;
    }

    // Decode budget exhausted: don't keep re-reading + re-decoding a source that
    // has already failed to decode (a corrupt/truncated file, an unsupported
    // format). Cleared by any successful insert / invalidate.
    if (decodeSuppressed(url)) {
        return {};
    }

    // Rebuild from retained encoded bytes (full resolution, local) before
    // touching disk. Covers both the timeline and the full-screen viewer
    // (which reads loadImage) after the decoded frame was evicted.
    {
        auto &bytesCache = sourceBytesCache();
        auto bit = bytesCache.find(url);
        if (bit != bytesCache.end()) {
            const auto blob = *bit;
            touchOrder(sourceBytesInsertionOrder(), url);
            auto img = decodeImageBytes(blob.bytes, blob.mime);
            if (!img.isNull()) {
                recordProbeSize(url, img.size());
                img = capForTimeline(img);
                cache.insert(url, img);
                auto &order = imageInsertionOrder();
                touchOrder(order, url);
                trimCache(cache, order, kMaxImageCacheSize, kMaxImageCacheBytes, imageBytes);
                return img;
            }
        }
    }

    bool waitingForResolution = false;
    const auto filePath = filePathForRead(url, &waitingForResolution);
    if (waitingForResolution) {
        return {}; // Not yet resolved.
    }

    // Self-heal: if the cached path no longer exists on disk, remove the
    // stale mapping so the URL becomes eligible for re-resolution.
    if (!QFileInfo::exists(filePath)) {
        pathCache().remove(url);
        requestedSet().remove(url);
        return {};
    }

    // Load from disk with EXIF auto-transform.
    QImageReader reader(filePath);
    reader.setAutoTransform(true);
    auto img = reader.read();
    if (img.isNull()) {
        img = QImage(filePath);
    }
    // The cached file is extensionless, so there's no MIME hint here. If Qt
    // couldn't decode it, try the macOS-native decoder (returns null for
    // non-HEIC), which handles HEIC the Qt plugin can't.
    if (img.isNull()) {
        img = TeleMatrix::DecodeHeicNative(filePath, 0);
    }

    // Convert non-sRGB images to sRGB. The window's backing store is forced
    // to sRGB via NSWindow setColorSpace, so pixel values must be in sRGB.
    if (!img.isNull() && img.colorSpace().isValid()
            && img.colorSpace() != QColorSpace(QColorSpace::SRgb)) {
        img.convertToColorSpace(QColorSpace::SRgb);
    }

    if (!img.isNull()) {
        recordProbeSize(url, img.size());
        img = capForTimeline(img);
        cache.insert(url, img);
        auto &order = imageInsertionOrder();
        touchOrder(order, url);
        trimCache(cache, order, kMaxImageCacheSize, kMaxImageCacheBytes, imageBytes);
    } else {
        // The file exists but decoded to nothing (corrupt/truncated/unsupported):
        // count it so the budget stops the paint-rate re-decode.
        noteDecodeFailed(url);
    }
    // Note: do NOT delete files that fail to load as images — they may be
    // valid video/audio/document files. Only images go through loadImage.
    return img;
}

QImage MediaCache::loadFullImage(const QString &url) {
    if (url.isEmpty()) {
        return {};
    }
    // Full resolution for the fullscreen viewer: bypass the timeline imageCache
    // entirely (its entries are downscaled to a display ceiling — see
    // capForTimeline) and decode fresh from the retained encoded bytes, else the
    // resolved on-disk file. Returns null when neither is available yet.
    {
        auto &bytesCache = sourceBytesCache();
        auto bit = bytesCache.find(url);
        if (bit != bytesCache.end()) {
            const auto blob = *bit;
            touchOrder(sourceBytesInsertionOrder(), url);
            auto img = decodeImageBytes(blob.bytes, blob.mime);
            if (!img.isNull()) {
                return img;
            }
        }
    }

    bool waitingForResolution = false;
    const auto filePath = filePathForRead(url, &waitingForResolution);
    if (waitingForResolution) {
        return {};
    }
    if (!QFileInfo::exists(filePath)) {
        pathCache().remove(url);
        requestedSet().remove(url);
        return {};
    }

    QImageReader reader(filePath);
    reader.setAutoTransform(true);
    auto img = reader.read();
    if (img.isNull()) {
        img = QImage(filePath);
    }
    if (img.isNull()) {
        img = TeleMatrix::DecodeHeicNative(filePath, 0);
    }
    if (!img.isNull() && img.colorSpace().isValid()
            && img.colorSpace() != QColorSpace(QColorSpace::SRgb)) {
        img.convertToColorSpace(QColorSpace::SRgb);
    }
    return img;
}

QSize MediaCache::peekImageSize(const QString &url) {
    if (url.isEmpty()) {
        return {};
    }
    if (auto it = imageCache().find(url);
            it != imageCache().end() && !it->isNull()) {
        return it->size();
    }
    if (auto it = probeSizeCache().find(url); it != probeSizeCache().end()) {
        return it.value();
    }
    return {};
}

bool MediaCache::insertImageBytes(
        const QString &key,
        const QByteArray &bytes,
        const QString &mime) {
    if (key.isEmpty() || bytes.isEmpty()) {
        return false;
    }

    auto img = decodeImageBytes(bytes, mime);
    if (img.isNull()) {
        return false;
    }

    recordProbeSize(key, img.size());
    clearFailureState(key); // fresh bytes decoded — reset resolve+decode memory
    auto &cache = imageCache();
    cache.insert(key, capForTimeline(img));
    auto &order = imageInsertionOrder();
    touchOrder(order, key);
    trimCache(cache, order, kMaxImageCacheSize, kMaxImageCacheBytes, imageBytes);

    // Retain the compact encoded bytes so the decoded image can be rebuilt
    // locally (full resolution) after eviction — no backend round-trip, no
    // preloader flash on room reopen.
    auto &bytesCache = sourceBytesCache();
    bytesCache.insert(key, MemoryBlob{bytes, mime, QString()});
    auto &bytesOrder = sourceBytesInsertionOrder();
    touchOrder(bytesOrder, key);
    trimCache(bytesCache, bytesOrder, kMaxSourceBytesCacheSize,
        kMaxSourceBytesCacheBytes, blobBytes);

    requestedSet().remove(key);
    downloadStates().remove(key);

    auto &pxCache = pixmapCache();
    auto pit = pxCache.begin();
    while (pit != pxCache.end()) {
        if (keyHasBase(pit.key(), key)) {
            pit = pxCache.erase(pit);
        } else {
            ++pit;
        }
    }
    auto &siCache = scaledImageCache();
    auto sit = siCache.begin();
    while (sit != siCache.end()) {
        if (keyHasBase(sit.key(), key)) {
            sit = siCache.erase(sit);
        } else {
            ++sit;
        }
    }
    return true;
}

void MediaCache::insertBytes(
        const QString &key,
        const QByteArray &bytes,
        const QString &mime,
        const QString &filename) {
    if (key.isEmpty() || bytes.isEmpty()) {
        return;
    }
    auto &cache = memoryBlobCache();
    cache.insert(key, MemoryBlob{bytes, mime, filename});
    auto &order = memoryBlobInsertionOrder();
    touchOrder(order, key);
    trimCache(cache, order, kMaxMemoryBlobCacheSize, kMaxMemoryBlobCacheBytes, blobBytes);
    requestedSet().remove(key);
    downloadStates().remove(key);
    clearFailureState(key); // bytes are in hand — forget past failures
}

MediaCache::MemoryBlob MediaCache::memoryBlob(const QString &key) {
    return memoryBlobCache().value(key);
}

bool MediaCache::hasMemoryBlob(const QString &key) {
    return memoryBlobCache().contains(key);
}

QPixmap MediaCache::loadPixmap(const QString &url, const QSize &targetSize, qreal dpr) {
    if (url.isEmpty() || targetSize.isEmpty()) {
        return {};
    }

    // Cache key includes URL + target dimensions + DPR.
    const auto key = url
        + QChar(':') + QString::number(targetSize.width())
        + QChar('x') + QString::number(targetSize.height())
        + QChar('@') + QString::number(dpr);

    auto &cache = pixmapCache();
    auto it = cache.find(key);
    if (it != cache.end()) {
        touchOrder(pixmapInsertionOrder(), key);
        return *it;
    }

    // Load the source QImage (itself cached in imageCache).
    const auto source = loadImage(url);
    if (source.isNull()) {
        return {};
    }

    const auto tw = targetSize.width();
    const auto th = targetSize.height();
    const auto pixelW = qRound(tw * dpr);
    const auto pixelH = qRound(th * dpr);

    // Always cover-crop: scale to fill target, center-crop overflow.
    const auto sw = source.width();
    const auto sh = source.height();
    QRect sourceRect(0, 0, sw, sh);
    const auto targetAspect = double(tw) / double(th);
    const auto sourceAspect = double(sw) / double(sh);
    if (sourceAspect > targetAspect) {
        const auto cropW = qBound(1, int(std::round(sh * targetAspect)), sw);
        sourceRect = QRect((sw - cropW) / 2, 0, cropW, sh);
    } else if (sourceAspect < targetAspect) {
        const auto cropH = qBound(1, int(std::round(sw / targetAspect)), sh);
        sourceRect = QRect(0, (sh - cropH) / 2, sw, cropH);
    }
    auto cropped = source.copy(sourceRect);
    auto result = cropped.scaled(pixelW, pixelH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    result.setDevicePixelRatio(dpr);
    auto pixmap = QPixmap::fromImage(std::move(result));
    if (!pixmap.isNull()) {
        cache.insert(key, pixmap);
        auto &order = pixmapInsertionOrder();
        touchOrder(order, key);
        trimCache(cache, order, kMaxPixmapCacheSize, kMaxPixmapCacheBytes, pixmapBytes);
    }
    return pixmap;
}

QImage MediaCache::loadScaledImage(const QString &url, const QSize &targetSize, qreal dpr) {
    if (url.isEmpty() || targetSize.isEmpty()) {
        return {};
    }

    const auto key = url
        + QLatin1Char(':')
        + QString::number(targetSize.width())
        + QLatin1Char('x')
        + QString::number(targetSize.height())
        + QLatin1Char('@')
        + QString::number(dpr);

    {
        auto &cache = scaledImageCache();
        auto it = cache.find(key);
        if (it != cache.end()) {
            touchOrder(scaledImageInsertionOrder(), key);
            return *it;
        }
    }

    const auto source = loadImage(url);
    if (source.isNull()) {
        return {};
    }
    const auto tw = targetSize.width();
    const auto th = targetSize.height();
    const auto pixelW = qRound(tw * dpr);
    const auto pixelH = qRound(th * dpr);
    const auto sw = source.width();
    const auto sh = source.height();
    QRect sourceRect(0, 0, sw, sh);
    const auto targetAspect = double(tw) / double(th);
    const auto sourceAspect = double(sw) / double(sh);
    if (sourceAspect > targetAspect) {
        const auto cropW = qBound(1, int(std::round(sh * targetAspect)), sw);
        sourceRect = QRect((sw - cropW) / 2, 0, cropW, sh);
    } else if (sourceAspect < targetAspect) {
        const auto cropH = qBound(1, int(std::round(sw / targetAspect)), sh);
        sourceRect = QRect(0, (sh - cropH) / 2, sw, cropH);
    }
    auto cropped = source.copy(sourceRect);
    auto result = cropped.scaled(pixelW, pixelH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    result.setDevicePixelRatio(dpr);

    {
        auto &cache = scaledImageCache();
        cache.insert(key, result);
        auto &order = scaledImageInsertionOrder();
        touchOrder(order, key);
        trimCache(cache, order, kMaxScaledImageCacheSize, kMaxScaledImageCacheBytes, imageBytes);
    }

    return result;
}

QImage MediaCache::loadScaledImageAsync(
        const QString &url,
        const QSize &targetSize,
        qreal dpr,
        QWidget *repaintTarget,
        const QRect &repaintRect) {
    if (url.isEmpty() || targetSize.isEmpty()) {
        return {};
    }

    const auto key = url
        + QLatin1Char(':')
        + QString::number(targetSize.width())
        + QLatin1Char('x')
        + QString::number(targetSize.height())
        + QLatin1Char('@')
        + QString::number(dpr);

    {
        auto &cache = scaledImageCache();
        auto it = cache.find(key);
        if (it != cache.end()) {
            touchOrder(scaledImageInsertionOrder(), key);
            return *it;
        }
    }

    // Decode budget exhausted for this source: stop re-dispatching a doomed
    // decode (and don't re-arm the recheck). Paint shows a static placeholder.
    if (decodeSuppressed(url)) {
        return {};
    }

    QImage cachedSource;
    {
        auto &cache = imageCache();
        auto it = cache.find(url);
        if (it != cache.end()) {
            cachedSource = *it;
            touchOrder(imageInsertionOrder(), url);
        }
    }

    // Retained encoded bytes let an evicted decoded image rebuild locally
    // (full resolution) without a backend round-trip or a preloader flash.
    QByteArray sourceBytes;
    QString sourceMime;
    if (cachedSource.isNull()) {
        auto &bytesCache = sourceBytesCache();
        auto bit = bytesCache.find(url);
        if (bit != bytesCache.end()) {
            sourceBytes = bit->bytes;
            sourceMime = bit->mime;
            touchOrder(sourceBytesInsertionOrder(), url);
        }
    }

    QString filePath;
    if (cachedSource.isNull() && sourceBytes.isEmpty()) {
        bool waitingForResolution = false;
        filePath = filePathForRead(url, &waitingForResolution);
        if (waitingForResolution) {
            requestMediaRecheck(); // Evicted with no bytes — re-resolve.
            return {};
        }
    }

    if (asyncLoadingSet().contains(key)) {
        return {}; // Already in progress.
    }

    // Self-heal stale paths before dispatching work.
    if (cachedSource.isNull() && sourceBytes.isEmpty()
            && !QFileInfo::exists(filePath)) {
        pathCache().remove(url);
        requestedSet().remove(url);
        // Only remote keys can be re-resolved by the host; a non-remote key
        // (e.g. "vidthumb:") would make recheck a no-op timeline scan.
        if (isRemoteCacheKey(url)) {
            requestMediaRecheck();
        }
        return {};
    }

    asyncLoadingSet().insert(key);

    QPointer<QWidget> target(repaintTarget);
    const auto targetRect = repaintRect;
    const auto capturedUrl = url;
    const auto generation = cacheGeneration();
    static_cast<void>(QtConcurrent::run(&decodePool(), [capturedUrl, filePath, sourceBytes, sourceMime, key, targetSize, dpr, target, targetRect, cachedSource, generation]() mutable {
        auto img = cachedSource;
        if (img.isNull()) {
            if (!sourceBytes.isEmpty()) {
                img = decodeImageBytes(sourceBytes, sourceMime);
            } else {
                QImageReader reader(filePath);
                reader.setAutoTransform(true);
                img = reader.read();
                if (img.isNull()) {
                    img = QImage(filePath);
                }
                // Convert non-sRGB images to sRGB (backing store is sRGB).
                if (!img.isNull() && img.colorSpace().isValid()
                        && img.colorSpace() != QColorSpace(QColorSpace::SRgb)) {
                    img.convertToColorSpace(QColorSpace::SRgb);
                }
            }
        }
        if (img.isNull()) {
            QMetaObject::invokeMethod(qApp, [capturedUrl, key, generation]() {
                if (generation != cacheGeneration()) {
                    return;
                }
                asyncLoadingSet().remove(key);
                noteDecodeFailed(capturedUrl); // undecodable — cap the retries
            });
            return;
        }

        const auto tw = targetSize.width();
        const auto th = targetSize.height();
        const auto pixelW = qRound(tw * dpr);
        const auto pixelH = qRound(th * dpr);
        const auto sw = img.width();
        const auto sh = img.height();
        QRect sourceRect(0, 0, sw, sh);
        if (sw > 0 && sh > 0 && pixelW > 0 && pixelH > 0) {
            const auto targetAspect = static_cast<double>(tw) / th;
            const auto sourceAspect = static_cast<double>(sw) / sh;
            if (sourceAspect > targetAspect) {
                const auto cropW = qBound(1, int(std::round(sh * targetAspect)), sw);
                sourceRect = QRect((sw - cropW) / 2, 0, cropW, sh);
            } else if (sourceAspect < targetAspect) {
                const auto cropH = qBound(1, int(std::round(sw / targetAspect)), sh);
                sourceRect = QRect(0, (sh - cropH) / 2, sw, cropH);
            }
        }
        auto cropped = img.copy(sourceRect);
        auto scaled = cropped.scaled(
            pixelW,
            pixelH,
            Qt::IgnoreAspectRatio,
            Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(dpr);

        // Insert raw+scaled images on the main thread, then trigger repaint.
        QMetaObject::invokeMethod(qApp, [capturedUrl, key, img = std::move(img), scaled = std::move(scaled), target, targetRect, generation]() mutable {
            if (generation != cacheGeneration()) {
                return;
            }
            asyncLoadingSet().remove(key);
            if (!img.isNull()) {
                recordProbeSize(capturedUrl, img.size());
                auto &cache = imageCache();
                if (!cache.contains(capturedUrl)) {
                    // `scaled` above was already derived from the full-res img,
                    // so capping here costs the thumbnail no quality.
                    cache.insert(capturedUrl, capForTimeline(img));
                    auto &order = imageInsertionOrder();
                    touchOrder(order, capturedUrl);
                    trimCache(cache, order, kMaxImageCacheSize, kMaxImageCacheBytes, imageBytes);
                }
            }
            if (!scaled.isNull()) {
                auto &cache = scaledImageCache();
                cache.insert(key, scaled);
                auto &order = scaledImageInsertionOrder();
                touchOrder(order, key);
                trimCache(cache, order, kMaxScaledImageCacheSize, kMaxScaledImageCacheBytes, imageBytes);
            }
            if (target) {
                if (targetRect.isValid()) {
                    target->update(targetRect);
                } else {
                    target->update();
                }
            }
        });
    }));

    return {}; // Return empty; repaint triggers when done.
}

QPixmap MediaCache::loadPixmapAsync(
        const QString &url,
        const QSize &targetSize,
        qreal dpr,
        QWidget *repaintTarget,
        const QRect &repaintRect) {
    if (url.isEmpty() || targetSize.isEmpty()) {
        return {};
    }

    const auto key = url
        + QLatin1Char(':')
        + QString::number(targetSize.width())
        + QLatin1Char('x')
        + QString::number(targetSize.height())
        + QLatin1Char('@')
        + QString::number(dpr);

    // Fast path: already cached.
    auto &cache = pixmapCache();
    auto it = cache.find(key);
    if (it != cache.end()) {
        touchOrder(pixmapInsertionOrder(), key);
        return *it;
    }

    // Decode budget exhausted for this source: stop re-dispatching.
    if (decodeSuppressed(url)) {
        return {};
    }

    // Reuse the raw image if it is already decoded, but still scale off the
    // GUI thread. Large phone photos can take a noticeable frame to resample.
    QImage cachedSource;
    {
        auto &imgCache = imageCache();
        auto imgIt = imgCache.find(url);
        if (imgIt != imgCache.end()) {
            cachedSource = *imgIt;
            touchOrder(imageInsertionOrder(), url);
        }
    }

    // Retained encoded bytes let an evicted decoded image rebuild locally
    // (full resolution) without a backend round-trip or a preloader flash.
    QByteArray sourceBytes;
    QString sourceMime;
    if (cachedSource.isNull()) {
        auto &bytesCache = sourceBytesCache();
        auto bit = bytesCache.find(url);
        if (bit != bytesCache.end()) {
            sourceBytes = bit->bytes;
            sourceMime = bit->mime;
            touchOrder(sourceBytesInsertionOrder(), url);
        }
    }

    QString filePath;
    if (cachedSource.isNull() && sourceBytes.isEmpty()) {
        bool waitingForResolution = false;
        filePath = filePathForRead(url, &waitingForResolution);
        if (waitingForResolution) {
            requestMediaRecheck(); // Evicted with no bytes — re-resolve.
            return {};
        }
    }

    if (asyncLoadingSet().contains(key)) {
        return {}; // Already in progress.
    }
    if (cachedSource.isNull() && sourceBytes.isEmpty()
            && !QFileInfo::exists(filePath)) {
        pathCache().remove(url);
        requestedSet().remove(url);
        // Only remote keys can be re-resolved by the host; a non-remote key
        // (e.g. "vidthumb:") would make recheck a no-op timeline scan.
        if (isRemoteCacheKey(url)) {
            requestMediaRecheck();
        }
        return {};
    }
    asyncLoadingSet().insert(key);

    QPointer<QWidget> target(repaintTarget);
    const auto targetRect = repaintRect;
    const auto capturedUrl = url;
    const auto generation = cacheGeneration();
    static_cast<void>(QtConcurrent::run(&decodePool(), [capturedUrl, filePath, sourceBytes, sourceMime, key, targetSize, dpr, target, targetRect, cachedSource, generation]() mutable {
        auto img = cachedSource;
        if (img.isNull()) {
            if (!sourceBytes.isEmpty()) {
                img = decodeImageBytes(sourceBytes, sourceMime);
            } else {
                QImageReader reader(filePath);
                reader.setAutoTransform(true);
                img = reader.read();
                if (img.isNull()) {
                    img = QImage(filePath);
                }
                // Convert non-sRGB images to sRGB (backing store is sRGB).
                if (!img.isNull() && img.colorSpace().isValid()
                        && img.colorSpace() != QColorSpace(QColorSpace::SRgb)) {
                    img.convertToColorSpace(QColorSpace::SRgb);
                }
            }
        }
        if (img.isNull()) {
            QMetaObject::invokeMethod(qApp, [capturedUrl, key, generation]() {
                if (generation != cacheGeneration()) {
                    return;
                }
                asyncLoadingSet().remove(key);
                noteDecodeFailed(capturedUrl); // undecodable — cap the retries
            });
            return;
        }

        // Cover-crop + scale (same logic as loadPixmap).
        const auto tw = targetSize.width();
        const auto th = targetSize.height();
        const auto pixelW = qRound(tw * dpr);
        const auto pixelH = qRound(th * dpr);
        const auto sw = img.width();
        const auto sh = img.height();
        QRect sourceRect(0, 0, sw, sh);
        if (sw > 0 && sh > 0 && pixelW > 0 && pixelH > 0) {
            const auto targetRatio = static_cast<double>(pixelW) / pixelH;
            const auto sourceRatio = static_cast<double>(sw) / sh;
            if (sourceRatio > targetRatio) {
                const auto cropW = static_cast<int>(sh * targetRatio);
                sourceRect = QRect((sw - cropW) / 2, 0, cropW, sh);
            } else {
                const auto cropH = static_cast<int>(sw / targetRatio);
                sourceRect = QRect(0, (sh - cropH) / 2, sw, cropH);
            }
        }
        auto cropped = img.copy(sourceRect);
        auto result = cropped.scaled(pixelW, pixelH,
            Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        result.setDevicePixelRatio(dpr);

        // Insert on main thread.
        QMetaObject::invokeMethod(qApp, [capturedUrl, key, img = std::move(img), result = std::move(result), target, targetRect, generation]() mutable {
            if (generation != cacheGeneration()) {
                return;
            }
            asyncLoadingSet().remove(key);
            if (!result.isNull()) {
                auto pixmap = QPixmap::fromImage(result);
                auto &cache = pixmapCache();
                if (!pixmap.isNull()) {
                    cache.insert(key, pixmap);
                    auto &order = pixmapInsertionOrder();
                    touchOrder(order, key);
                    trimCache(cache, order, kMaxPixmapCacheSize, kMaxPixmapCacheBytes, pixmapBytes);
                }
            }
            auto &imgCache = imageCache();
            if (!img.isNull() && !imgCache.contains(capturedUrl)) {
                imgCache.insert(capturedUrl, capForTimeline(img));
                auto &order = imageInsertionOrder();
                touchOrder(order, capturedUrl);
                trimCache(imgCache, order, kMaxImageCacheSize, kMaxImageCacheBytes, imageBytes);
            }
            if (target) {
                if (targetRect.isValid()) {
                    target->update(targetRect);
                } else {
                    target->update();
                }
            }
        });
    }));

    return {}; // Return empty; repaint triggers when done.
}

QPixmap MediaCache::loadAvatarPixmap(const QString &url, int size, qreal dpr) {
    if (url.isEmpty() || size <= 0) {
        return {};
    }

    const auto key = QStringLiteral("avatar:") + url
        + QChar(':') + QString::number(size)
        + QChar('@') + QString::number(dpr);

    auto &cache = avatarPixmapCache();
    auto it = cache.find(key);
    if (it != cache.end()) {
        touchOrder(avatarPixmapInsertionOrder(), key);
        return *it;
    }

    const auto source = loadImage(url);
    if (source.isNull()) {
        return {};
    }

    // Create a circular avatar pixmap.
    const auto pixelSize = qRound(size * dpr);
    QImage result(pixelSize, pixelSize, QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);
    result.setDevicePixelRatio(dpr);
    {
        QPainter painter(&result);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

        QPainterPath clipPath;
        clipPath.addEllipse(0, 0, size, size);
        painter.setClipPath(clipPath);

        // Cover-crop into the circle.
        const auto sw = source.width();
        const auto sh = source.height();
        QRect sourceRect(0, 0, sw, sh);
        if (sw > sh) {
            sourceRect = QRect((sw - sh) / 2, 0, sh, sh);
        } else if (sh > sw) {
            sourceRect = QRect(0, (sh - sw) / 2, sw, sw);
        }
        painter.drawImage(QRect(0, 0, size, size), source, sourceRect);
    }

    auto pixmap = QPixmap::fromImage(std::move(result));
    if (!pixmap.isNull()) {
        cache.insert(key, pixmap);
        auto &order = avatarPixmapInsertionOrder();
        touchOrder(order, key);
        trimCache(cache, order, kMaxAvatarPixmapCacheSize,
            kMaxAvatarPixmapCacheBytes, pixmapBytes);
    }
    return pixmap;
}

QPixmap MediaCache::loadAvatarPixmapAsync(
        const QString &url,
        int size,
        qreal dpr,
        QWidget *repaintTarget,
        const QRect &repaintRect) {
    if (url.isEmpty() || size <= 0) {
        return {};
    }

    const auto key = QStringLiteral("avatar:") + url
        + QChar(':') + QString::number(size)
        + QChar('@') + QString::number(dpr);

    auto &cache = avatarPixmapCache();
    auto it = cache.find(key);
    if (it != cache.end()) {
        touchOrder(avatarPixmapInsertionOrder(), key);
        return *it;
    }

    // If the source is already decoded, crop the circle synchronously — cheap.
    if (imageCache().contains(url)) {
        return loadAvatarPixmap(url, size, dpr);
    }

    // Decode budget exhausted for this source: stop re-dispatching.
    if (decodeSuppressed(url)) {
        return {};
    }

    // Otherwise decode off the GUI thread: from retained encoded bytes if we
    // have them (no round-trip), else from the resolved disk path. Decoding a
    // full-resolution avatar synchronously here would stall the paint frame.
    QByteArray sourceBytes;
    QString sourceMime;
    {
        auto &bytesCache = sourceBytesCache();
        auto bit = bytesCache.find(url);
        if (bit != bytesCache.end()) {
            sourceBytes = bit->bytes;
            sourceMime = bit->mime;
            touchOrder(sourceBytesInsertionOrder(), url);
        }
    }

    QString filePath;
    if (sourceBytes.isEmpty()) {
        bool waitingForResolution = false;
        filePath = filePathForRead(url, &waitingForResolution);
        if (waitingForResolution) {
            requestMediaRecheck();
            return {};
        }
        if (!QFileInfo::exists(filePath)) {
            pathCache().remove(url);
            requestedSet().remove(url);
            if (isRemoteCacheKey(url)) {
                requestMediaRecheck();
            }
            return {};
        }
    }
    if (asyncLoadingSet().contains(key)) {
        return {};
    }

    asyncLoadingSet().insert(key);
    QPointer<QWidget> target(repaintTarget);
    const auto targetRect = repaintRect;
    const auto capturedUrl = url;
    const auto generation = cacheGeneration();
    static_cast<void>(QtConcurrent::run(&decodePool(), [capturedUrl, filePath, sourceBytes, sourceMime, key, size, dpr, target, targetRect, generation]() {
        QImage img;
        if (!sourceBytes.isEmpty()) {
            img = decodeImageBytes(sourceBytes, sourceMime);
        } else {
            QImageReader reader(filePath);
            reader.setAutoTransform(true);
            img = reader.read();
            if (img.isNull()) {
                img = QImage(filePath);
            }
            if (!img.isNull() && img.colorSpace().isValid()
                    && img.colorSpace() != QColorSpace(QColorSpace::SRgb)) {
                img.convertToColorSpace(QColorSpace::SRgb);
            }
        }
        if (img.isNull()) {
            QMetaObject::invokeMethod(qApp, [capturedUrl, key, generation]() {
                if (generation != cacheGeneration()) {
                    return;
                }
                asyncLoadingSet().remove(key);
                noteDecodeFailed(capturedUrl); // undecodable — cap the retries
            });
            return;
        }

        const auto pixelSize = qRound(size * dpr);
        QImage result(pixelSize, pixelSize, QImage::Format_ARGB32_Premultiplied);
        result.fill(Qt::transparent);
        result.setDevicePixelRatio(dpr);
        {
            QPainter painter(&result);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

            QPainterPath clipPath;
            clipPath.addEllipse(0, 0, size, size);
            painter.setClipPath(clipPath);

            const auto sw = img.width();
            const auto sh = img.height();
            QRect sourceRect(0, 0, sw, sh);
            if (sw > 0 && sh > 0) {
                if (sw > sh) {
                    sourceRect = QRect((sw - sh) / 2, 0, sh, sh);
                } else if (sh > sw) {
                    sourceRect = QRect(0, (sh - sw) / 2, sw, sw);
                }
            }
            painter.drawImage(QRect(0, 0, size, size), img, sourceRect);
        }

        QMetaObject::invokeMethod(qApp, [capturedUrl, key, img = std::move(img), result = std::move(result), target, targetRect, generation]() mutable {
            if (generation != cacheGeneration()) {
                return;
            }
            asyncLoadingSet().remove(key);
            if (!img.isNull()) {
                recordProbeSize(capturedUrl, img.size());
                auto &imgCache = imageCache();
                if (!imgCache.contains(capturedUrl)) {
                    imgCache.insert(capturedUrl, capForTimeline(img));
                    auto &imgOrder = imageInsertionOrder();
                    touchOrder(imgOrder, capturedUrl);
                    trimCache(imgCache, imgOrder, kMaxImageCacheSize, kMaxImageCacheBytes, imageBytes);
                }
            }
            if (!result.isNull()) {
                auto pixmap = QPixmap::fromImage(result);
                if (!pixmap.isNull()) {
                    auto &pxCache = avatarPixmapCache();
                    pxCache.insert(key, pixmap);
                    auto &pxOrder = avatarPixmapInsertionOrder();
                    touchOrder(pxOrder, key);
                    trimCache(pxCache, pxOrder, kMaxAvatarPixmapCacheSize,
                        kMaxAvatarPixmapCacheBytes, pixmapBytes);
                }
            }
            if (target) {
                if (targetRect.isValid()) {
                    target->update(targetRect);
                } else {
                    target->update();
                }
            }
        });
    }));

    return {};
}

void MediaCache::insertImage(const QString &key, const QImage &image) {
    if (key.isEmpty() || image.isNull()) {
        return;
    }
    if (isPlaceholderKey(key)) {
        auto &cache = placeholderImageCache();
        cache.insert(key, image); // tiny (64x48) — no display-ceiling cap needed
        auto &order = placeholderImageInsertionOrder();
        touchOrder(order, key);
        trimCache(cache, order, kMaxPlaceholderImageCacheSize,
            kMaxPlaceholderImageCacheBytes, imageBytes);
        return;
    }
    clearFailureState(key); // a real decoded image landed — clear failure memory
    auto &imgCache = imageCache();
    imgCache.insert(key, capForTimeline(image));
    auto &order = imageInsertionOrder();
    touchOrder(order, key);
    trimCache(imgCache, order, kMaxImageCacheSize, kMaxImageCacheBytes, imageBytes);
    // Invalidate any existing pixmap variants for this key.
    auto &pxCache = pixmapCache();
    auto it = pxCache.begin();
    while (it != pxCache.end()) {
        if (keyHasBase(it.key(), key)) {
            it = pxCache.erase(it);
        } else {
            ++it;
        }
    }
}

QString MediaCache::resolvedPathForPlayback(
    const QString &filePath,
    const QString &filename,
    const QString &mime) {
    if (filePath.isEmpty()) {
        return {};
    }
    if (!QFileInfo(filePath).suffix().isEmpty()) {
        return filePath;
    }
    auto ext = QFileInfo(filename).suffix().toLower();
    if (ext.isEmpty()) {
        // Audio types.
        if (mime.contains(QStringLiteral("mpeg"))) ext = QStringLiteral("mp3");
        else if (mime.contains(QStringLiteral("ogg"))) ext = QStringLiteral("ogg");
        else if (mime.contains(QStringLiteral("flac"))) ext = QStringLiteral("flac");
        else if (mime.contains(QStringLiteral("wav"))) ext = QStringLiteral("wav");
        else if (mime.contains(QStringLiteral("aac"))) ext = QStringLiteral("aac");
        // Video types.
        else if (mime.contains(QStringLiteral("mp4"))) ext = QStringLiteral("mp4");
        else if (mime.contains(QStringLiteral("webm"))) ext = QStringLiteral("webm");
        else if (mime.contains(QStringLiteral("quicktime"))) ext = QStringLiteral("mov");
        else if (mime.contains(QStringLiteral("matroska"))) ext = QStringLiteral("mkv");
        else if (mime.contains(QStringLiteral("avi"))) ext = QStringLiteral("avi");
        // Image types.
        else if (mime.contains(QStringLiteral("png"))) ext = QStringLiteral("png");
        else if (mime.contains(QStringLiteral("jpeg"))) ext = QStringLiteral("jpg");
        else if (mime.contains(QStringLiteral("gif"))) ext = QStringLiteral("gif");
        else if (mime.contains(QStringLiteral("webp"))) ext = QStringLiteral("webp");
        else if (mime.contains(QStringLiteral("svg"))) ext = QStringLiteral("svg");
        // Document/archive types.
        else if (mime.contains(QStringLiteral("zip"))) ext = QStringLiteral("zip");
        else if (mime.contains(QStringLiteral("gzip"))) ext = QStringLiteral("gz");
        else if (mime.contains(QStringLiteral("x-tar"))) ext = QStringLiteral("tar");
        else if (mime.contains(QStringLiteral("x-7z"))) ext = QStringLiteral("7z");
        else if (mime.contains(QStringLiteral("x-rar"))) ext = QStringLiteral("rar");
        else if (mime.contains(QStringLiteral("pdf"))) ext = QStringLiteral("pdf");
        else if (mime.contains(QStringLiteral("json"))) ext = QStringLiteral("json");
        else if (mime.contains(QStringLiteral("xml"))) ext = QStringLiteral("xml");
        else if (mime.contains(QStringLiteral("plain"))) ext = QStringLiteral("txt");
        else if (mime.contains(QStringLiteral("html"))) ext = QStringLiteral("html");
        else if (mime.contains(QStringLiteral("csv"))) ext = QStringLiteral("csv");
        // Fallback by category.
        else if (mime.startsWith(QStringLiteral("video/"))) ext = QStringLiteral("mp4");
        else if (mime.startsWith(QStringLiteral("audio/"))) ext = QStringLiteral("mp3");
        else if (mime.startsWith(QStringLiteral("image/"))) ext = QStringLiteral("png");
        else ext = QStringLiteral("bin"); // generic binary fallback
    }
    const auto linkPath = filePath + QStringLiteral(".") + ext;
    if (!QFile::exists(linkPath)) {
        QFile::link(filePath, linkPath);
    }
    return linkPath;
}

void MediaCache::invalidateImage(const QString &url) {
    imageCache().remove(url);
    sourceBytesCache().remove(url); // else loadImage would rebuild the old image
    clearFailureState(url); // force a fresh decode/resolve attempt next time
    // Also invalidate all pixmap variants for this URL. Match on the ':' boundary
    // (was contains(url), which could match an unrelated key that merely embeds
    // this URL as a substring).
    auto &pxCache = pixmapCache();
    auto it = pxCache.begin();
    while (it != pxCache.end()) {
        if (keyHasBase(it.key(), url)) {
            it = pxCache.erase(it);
        } else {
            ++it;
        }
    }
    auto &siCache = scaledImageCache();
    auto sit = siCache.begin();
    while (sit != siCache.end()) {
        if (keyHasBase(sit.key(), url)) {
            sit = siCache.erase(sit);
        } else {
            ++sit;
        }
    }
}

void MediaCache::clearAll() {
    ++cacheGeneration();
    pathCache().clear();
    imageCache().clear();
    imageInsertionOrder().clear();
    placeholderImageCache().clear();
    placeholderImageInsertionOrder().clear();
    probeSizeCache().clear();
    pixmapCache().clear();
    pixmapInsertionOrder().clear();
    avatarPixmapCache().clear();
    avatarPixmapInsertionOrder().clear();
    scaledImageCache().clear();
    scaledImageInsertionOrder().clear();
    sourceBytesCache().clear();
    sourceBytesInsertionOrder().clear();
    memoryBlobCache().clear();
    memoryBlobInsertionOrder().clear();
    requestedSet().clear();
    downloadStates().clear();
    asyncLoadingSet().clear();
    resolveFailures().clear();
    decodeFailureCounts().clear();
}

void MediaCache::clearForDataDir(const QString &dataDirPrefix) {
    if (dataDirPrefix.isEmpty()) {
        return;
    }
    // Bump the generation so in-flight async loads for the departing account
    // can't write their results back into the caches after this.
    ++cacheGeneration();

    // Match on the dir plus a trailing separator, so ".../accounts/1" can't also match
    // ".../accounts/10/..." — a latent prefix collision if the dir-naming scheme ever
    // changes. Cached values are files under the dir, so they carry the separator. R9-2.
    const QString prefix = dataDirPrefix.endsWith(QDir::separator())
        ? dataDirPrefix
        : dataDirPrefix + QDir::separator();

    QStringList staleUrls;
    for (auto it = pathCache().cbegin(); it != pathCache().cend(); ++it) {
        if (it.value().startsWith(prefix)) {
            staleUrls.append(it.key());
        }
    }
    for (const auto &url : staleUrls) {
        pathCache().remove(url);
        // Everything below is keyed by the same URL and derived from the file
        // that is about to be deleted.
        invalidateImage(url);
        requestedSet().remove(url);
        downloadStates().remove(url);
        asyncLoadingSet().remove(url);
        resolveFailures().remove(url);
        decodeFailureCounts().remove(url);
    }
}

void MediaCache::resetPendingRequests() {
    requestedSet().clear();
    asyncLoadingSet().clear();
    downloadStates().clear();
}

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QPixmap>
#include <QPointer>
#include <QRect>
#include <QSize>
#include <QString>
#include <QWidget>

#include <functional>

namespace TeleMatrix {

/// Shared cache for resolving mxc:// URLs to local file paths and loaded images.
/// Thread safety: must only be accessed from the Qt main thread.
class MediaCache {
public:
    enum class DownloadPhase {
        Downloading = 0,
        Decrypting = 1,
    };

    struct DownloadState {
        DownloadPhase phase = DownloadPhase::Downloading;
        quint64 receivedBytes = 0;
        quint64 totalBytes = 0;
    };

    struct MemoryBlob {
        QByteArray bytes;
        QString mime;
        QString filename;
    };

    /// Returns the cache key used for OG-card preview thumbnails.
    /// Non-MXC URLs are returned unchanged.
    static QString previewImageKey(const QString &url);

    /// Returns the local file path for an mxc:// URL, or empty if not yet resolved.
    static QString localPath(const QString &mxcUrl);

    /// Store a resolved local path for an mxc:// URL.
    static void insertPath(const QString &mxcUrl, const QString &path);

    /// Returns true if the mxc:// URL has a resolved local path on disk.
    static bool isResolved(const QString &mxcUrl);

    /// Returns true if this mxc:// URL has not been requested for resolution yet.
    static bool needsResolution(const QString &mxcUrl);

    /// Mark that resolution has been requested (avoids duplicate requests).
    static void markRequested(const QString &mxcUrl);

    /// Returns true if the URL currently has an in-flight resolution request.
    static bool isRequested(const QString &mxcUrl);

    /// Clear the "requested" marker, allowing a retry.
    static void clearRequested(const QString &mxcUrl);

    // --- Failure memory (H1/H2) -------------------------------------------
    // Two independent classes of failure, both keyed by the cache key (mxc://,
    // previewthumb:, srvthumb:, vidthumb:). Resolve failures back off (consulted
    // by needsResolution); decode failures get a small budget then declare the
    // key unavailable. All are cleared by any successful insert / invalidate /
    // clearAll so a genuinely-recovered URL is retried.

    /// Record a failed backend resolution for `key` (bumps its attempt count and
    /// stamps the failure time for the backoff).
    static void noteResolveFailed(const QString &key);

    /// Record a permanent (terminal) resolution failure for `key` — an HTTP 4xx
    /// other than 429. Unlike noteResolveFailed this suppresses retries for good
    /// (the resource is gone / the request is malformed), until the source changes.
    static void noteResolvePermanentlyFailed(const QString &key);

    /// True while `key` is inside its resolve-failure backoff window, or has failed
    /// permanently.
    static bool retrySuppressed(const QString &key);

    /// Record a failed decode of already-fetched bytes for `key`.
    static void noteDecodeFailed(const QString &key);

    /// True once `key` has exhausted its decode-attempt budget.
    static bool decodeSuppressed(const QString &key);

    /// True when paint should treat `key` as unavailable (static placeholder, no
    /// glow, no re-dispatch): decode budget spent, or resolution has failed
    /// repeatedly and is currently backing off.
    static bool mediaUnavailable(const QString &key);

    /// True while `key`'s still-loading placeholder should ANIMATE. False once it is
    /// unavailable, or it has failed enough times that pulsing it is no longer worth
    /// a ~60fps repaint of its row — the backoff retry continues either way.
    static bool shouldGlowWhileLoading(const QString &key);

    /// Override the millisecond clock used for backoff timing (tests only). Pass
    /// an empty function to restore the real monotonic wall clock.
    static void setClockForTesting(std::function<qint64()> clock);

    /// Store in-flight download progress for an mxc:// URL.
    static void setDownloadState(const QString &mxcUrl, const DownloadState &state);

    /// Returns true if the URL currently has tracked download progress.
    static bool hasDownloadState(const QString &mxcUrl);

    /// Returns the current download progress state for an mxc:// URL.
    static DownloadState downloadState(const QString &mxcUrl);

    /// Clear any in-flight download progress for an mxc:// URL.
    static void clearDownloadState(const QString &mxcUrl);

    // Per-mxc playback position, to hand the current position between the inline
    // player and the fullscreen overlay so switching between them doesn't restart.
    static void setPlaybackPosition(const QString &mxcUrl, qint64 positionMs);
    static qint64 takePlaybackPosition(const QString &mxcUrl); // ms, or -1; clears it

    /// Load an image, using the media cache for mxc:// URLs.
    /// Returns a cached QImage, loading from disk if necessary.
    /// For mxc:// URLs, returns null image if not yet resolved.
    static QImage loadImage(const QString &url);

    /// Full-resolution decode for the fullscreen viewer. Never served from (or
    /// inserted into) the downscaled timeline imageCache: decodes fresh from the
    /// retained encoded bytes, else from the resolved on-disk file. Returns null
    /// if neither is available yet (caller falls back to loadImage / async
    /// resolution).
    static QImage loadFullImage(const QString &url);

    /// Decode-free size probe for layout: returns a decoded image's dimensions
    /// from the in-memory caches or the probe cache, or an invalid QSize if
    /// unknown. Never touches disk or decodes — layout must not sync-decode.
    static QSize peekImageSize(const QString &url);

    /// Decode image bytes and insert the decoded image into the cache.
    /// The original encoded bytes are also retained (compact) so the decoded
    /// image can be rebuilt locally — at full resolution — after the decoded
    /// QImage is evicted under memory pressure, without a backend round-trip.
    static bool insertImageBytes(
        const QString &key,
        const QByteArray &bytes,
        const QString &mime = QString());

    /// Register a callback invoked when a paint-time load dead-ends on media
    /// that is no longer resolved and has no retained bytes (e.g. evicted with
    /// no disk path). The host arms a throttled recheck so the URL re-resolves
    /// without requiring the room to be reopened. `owner` identifies the caller
    /// so a stale teardown cannot clear a newer owner's hook.
    static void setMediaRecheckHook(const void *owner, std::function<void()> hook);

    /// Clear the recheck hook, but only if `owner` still owns it (no-op if a
    /// newer owner has since registered). Call from the owner's teardown.
    static void clearMediaRecheckHook(const void *owner);

    /// Store original decrypted bytes for short-lived memory playback.
    static void insertBytes(
        const QString &key,
        const QByteArray &bytes,
        const QString &mime = QString(),
        const QString &filename = QString());

    /// Return cached decrypted bytes metadata, or an empty blob if absent.
    static MemoryBlob memoryBlob(const QString &key);

    /// Returns true when original decrypted bytes are cached in memory.
    static bool hasMemoryBlob(const QString &key);

    /// Returns a QPixmap pre-scaled to targetSize, cached for fast paint.
    /// Loads from disk on first call, returns cached pixmap on subsequent calls.
    /// Uses cover-crop scaling (fills target, crops overflow).
    static QPixmap loadPixmap(const QString &url, const QSize &targetSize, qreal dpr);

    /// Returns a QImage pre-scaled to targetSize with color space preserved.
    /// Use with QPainter::drawImage() for color-accurate rendering on macOS
    /// (QPixmap loses color space metadata, causing color cast).
    static QImage loadScaledImage(const QString &url, const QSize &targetSize, qreal dpr);

    /// Return a scaled QImage if already cached, or null QImage and start
    /// a background load. When load completes, inserts into cache and updates
    /// repaintRect on repaintTarget, or the whole target if repaintRect is invalid.
    /// The caller should paint a placeholder when the returned image is null.
    static QImage loadScaledImageAsync(
        const QString &url,
        const QSize &targetSize,
        qreal dpr,
        QWidget *repaintTarget = nullptr,
        const QRect &repaintRect = QRect());

    /// Return cached pixmap if available, or null pixmap and start background
    /// load. When load completes, inserts into cache and updates repaintRect
    /// on repaintTarget, or the whole target if repaintRect is invalid.
    static QPixmap loadPixmapAsync(
        const QString &url,
        const QSize &targetSize,
        qreal dpr,
        QWidget *repaintTarget = nullptr,
        const QRect &repaintRect = QRect());

    /// Returns a circular QPixmap for avatar display, cached.
    static QPixmap loadAvatarPixmap(const QString &url, int size, qreal dpr);

    /// Return cached circular avatar pixmap, or start background load and
    /// repaint target/rect when ready.
    static QPixmap loadAvatarPixmapAsync(
        const QString &url,
        int size,
        qreal dpr,
        QWidget *repaintTarget = nullptr,
        const QRect &repaintRect = QRect());

    /// Insert a QImage directly into the image cache under a synthetic key.
    /// Used for locally-extracted video thumbnails ("vidthumb:<eventId>").
    static void insertImage(const QString &key, const QImage &image);

    /// Clear the image cache entry for a URL (forces reload on next paint).
    static void invalidateImage(const QString &url);

    /// Clear all caches (paths, images, pixmaps, requested markers).
    /// Call after clearing media files from disk so URLs get re-resolved.
    static void clearAll();

    /// Drop only what one account's storage backs: every entry whose resolved
    /// local path lives under `dataDirPrefix`, plus anything derived from it.
    ///
    /// The caches are shared by every account because an mxc URI is content-
    /// addressed and names its origin server, so two accounts can never disagree
    /// about what one refers to — sharing a resolved file is a feature. What is
    /// NOT shared is the file on disk: signing one account out trashes its data
    /// dir, so entries pointing into that dir must go with it or they become
    /// dangling paths.
    static void clearForDataDir(const QString &dataDirPrefix);

    /// Forget which URLs have a fetch in flight, keeping everything resolved.
    ///
    /// Requests belong to the bridge that issued them. When the UI is rebuilt
    /// against a different account's bridge, the old bridge's in-flight replies
    /// will never reach it, so the markers have to be dropped or those URLs would
    /// look permanently "already requested" and never load.
    static void resetPendingRequests();

    /// Ensure a cached media file has an extension so AVFoundation can detect
    /// the codec. Creates a symlink next to the file if needed.
    /// @param filePath  resolved local path (may be extensionless hash)
    /// @param filename  original filename hint (e.g. "clip.mp4")
    /// @param mime      MIME type hint (e.g. "video/mp4")
    static QString resolvedPathForPlayback(
        const QString &filePath,
        const QString &filename,
        const QString &mime);

private:
    static QHash<QString, QString> &pathCache();
    static QHash<QString, QImage> &imageCache();
    static QHash<QString, QPixmap> &pixmapCache();
    static QHash<QString, QImage> &scaledImageCache();
    static QHash<QString, MemoryBlob> &memoryBlobCache();
    /// Original encoded bytes for image-class media (keyed identically to
    /// imageCache: mxc://, previewthumb:, srvthumb:). Compact retention tier:
    /// survives imageCache eviction so the decoded image rebuilds locally.
    static QHash<QString, MemoryBlob> &sourceBytesCache();
    static QHash<QString, bool> &requestedSet();
    static QHash<QString, DownloadState> &downloadStates();
    static QHash<QString, qint64> &playbackPositions();
    static bool isRemoteCacheKey(const QString &url);
    static QString filePathForRead(const QString &url, bool *waitingForResolution = nullptr);

    // Failure memory backing state + helpers (see the public API above).
    struct ResolveFailure {
        int attempts = 0;
        qint64 lastFailureAtMs = 0;
        bool permanent = false; // terminal (4xx) — never retry
    };
    static QHash<QString, ResolveFailure> &resolveFailures();
    static QHash<QString, int> &decodeFailureCounts();
    static void clearFailureState(const QString &key);
    static qint64 nowMs();
};

} // namespace TeleMatrix

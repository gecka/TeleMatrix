// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::collections::HashMap;
use std::future::Future;
use std::io::{Cursor, Read};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use anyhow::{anyhow, Result};
use matrix_sdk::ruma::events::room::MediaSource;
use matrix_sdk::Client;
use matrix_sdk_crypto::AttachmentDecryptor;
use tokio::io::AsyncWriteExt;

use crate::media_cache_service::MediaCacheService;

pub const MEDIA_DOWNLOAD_PHASE_DOWNLOADING: u32 = 0;
pub const MEDIA_DOWNLOAD_PHASE_DECRYPTING: u32 = 1;

const MEDIA_PROGRESS_MIN_STEP_BYTES: u64 = 64 * 1024;
const MEDIA_PROGRESS_INTERVAL: Duration = Duration::from_millis(120);
const MAX_MEMORY_MEDIA_BYTES: u64 = 50 * 1024 * 1024;

/// Video-thumbnail frame source is a bounded RANGE download, never the whole file.
/// First try the leading 2 MB — enough for a faststart mp4 (moov + first frames).
const VIDTHUMB_FASTSTART_BYTES: u64 = 2 * 1024 * 1024;
/// If that can't be decoded (a moov-at-end mp4, whose sample table sits at the tail),
/// try a larger but still bounded prefix: it decodes a SMALL such clip (the prefix
/// spans the whole file, tail moov included) while a large one just gets no preview
/// instead of a multi-minute, permit-holding full download.
const VIDTHUMB_MAX_PARTIAL_BYTES: u64 = 16 * 1024 * 1024;

/// Cap concurrent outbound media downloads/thumbnails PER ACCOUNT. Opening a
/// photo-heavy room requests media for every visible item at once; without a bound
/// that is dozens of parallel full-resolution fetches competing with sync traffic.
/// 6: a background-resolution ceiling that still leaves headroom for an explicit
/// click. Held only for the network transfer (acquired after the cache-hit fast
/// paths). Per-service, NOT a process-global static: with up to 6 accounts a shared
/// pool handed each ~1 permit on a busy cold start, serializing cross-account media.
/// See code-review-2026-07-19 PERF-3.
const MEDIA_DOWNLOAD_CONCURRENCY: usize = 6;

/// Marker attached (via `anyhow::Error::context`) to a media-resolve error that is
/// permanent for this mxc: HTTP 404, i.e. the media is gone. Retrying can't recover
/// it, so the UI suppresses retries for good instead of backing off forever. Every
/// other failure (timeout, 5xx, 429, 401/403, network) is transient and never carries
/// it. Callers test with [`is_permanent_media_error`].
#[derive(Debug)]
pub(crate) struct PermanentMediaError;

impl std::fmt::Display for PermanentMediaError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("permanent media error")
    }
}

impl std::error::Error for PermanentMediaError {}

/// True for an HTTP status that permanently dooms a media fetch: only 404 (the media
/// is gone).
///
/// Deliberately NOT "any 4xx except 429". The raw download paths attach the access
/// token by hand, so a rotated/expired token, a captive portal or a corporate proxy
/// can answer 401/403 for a moment — treating those as permanent would blank every
/// image touched during that window for the rest of the session, with no recovery
/// once the network heals. 400 is likewise more likely a gateway artifact than a
/// malformed mxc (those fail before the request is ever sent). Everything except 404
/// therefore backs off and retries.
pub(crate) fn is_permanent_http_status(status: u16) -> bool {
    status == 404
}

/// mxc urls this process uploaded, with when. A homeserver answers 404 for a
/// thumbnail it has not generated yet, so for a short window after upload a 404
/// is "not ready", not "gone" — see the thumbnail branch in `resolve_avatar`.
fn recent_uploads() -> &'static std::sync::RwLock<HashMap<String, Instant>> {
    static UPLOADS: std::sync::OnceLock<std::sync::RwLock<HashMap<String, Instant>>> =
        std::sync::OnceLock::new();
    UPLOADS.get_or_init(|| std::sync::RwLock::new(HashMap::new()))
}

/// Generous enough to cover thumbnailing on a loaded server, short enough that a
/// genuinely dead url returns to the one-request-per-dead-avatar path.
const RECENT_UPLOAD_GRACE: Duration = Duration::from_secs(600);

/// Record an mxc we just uploaded, so its 404s are treated as "not ready yet".
pub(crate) fn note_recent_upload(mxc_url: &str) {
    if let Ok(mut uploads) = recent_uploads().write() {
        uploads.retain(|_, at| at.elapsed() < RECENT_UPLOAD_GRACE);
        uploads.insert(mxc_url.to_string(), Instant::now());
    }
}

fn is_recent_upload(mxc_url: &str) -> bool {
    recent_uploads()
        .read()
        .ok()
        .and_then(|uploads| {
            uploads
                .get(mxc_url)
                .map(|at| at.elapsed() < RECENT_UPLOAD_GRACE)
        })
        .unwrap_or(false)
}

/// Classify a typed matrix-sdk error (e.g. from `get_media_content`) as permanent.
fn matrix_error_is_permanent(e: &matrix_sdk::Error) -> bool {
    e.as_client_api_error()
        .map(|api| is_permanent_http_status(api.status_code.as_u16()))
        .unwrap_or(false)
}

/// True if `err` was tagged with [`PermanentMediaError`] via `anyhow::Context`.
///
/// Must use `downcast_ref`, NOT `err.chain().any(|c| c.is::<PermanentMediaError>())`:
/// `anyhow`'s `.context(v)` stores `v` inside an internal `ContextError` wrapper, so
/// no chain element's concrete type is `PermanentMediaError` — the chain scan always
/// returns false. `downcast_ref` special-cases context values and finds it.
pub(crate) fn is_permanent_media_error(err: &anyhow::Error) -> bool {
    err.downcast_ref::<PermanentMediaError>().is_some()
}

#[derive(Clone)]
pub(crate) struct MediaTransferService {
    cache: MediaCacheService,
    media_sources: Arc<std::sync::RwLock<HashMap<String, MediaSource>>>,
    // Per-account media-download concurrency cap (see MEDIA_DOWNLOAD_CONCURRENCY).
    // Clones of this service share the same Arc, so one account's downloads all draw
    // from its own pool — not a process-global one shared across accounts.
    download_semaphore: Arc<tokio::sync::Semaphore>,
}

impl MediaTransferService {
    pub(crate) fn new(
        cache: MediaCacheService,
        media_sources: Arc<std::sync::RwLock<HashMap<String, MediaSource>>>,
    ) -> Self {
        Self {
            cache,
            media_sources,
            download_semaphore: Arc::new(tokio::sync::Semaphore::new(MEDIA_DOWNLOAD_CONCURRENCY)),
        }
    }

    pub(crate) fn cleanup_plaintext(&self) {
        self.cache.cleanup_plaintext();
    }

    /// Remember an upload's source file so its echo seeds the cache instead of
    /// downloading the bytes back. See `upload_seed_store`.
    pub(crate) fn register_upload_seed(&self, txn_id: &str, path: &Path) {
        crate::upload_seed_store::register(txn_id, path, &self.cache);
    }

    pub(crate) async fn delete_cache_dir(&self) {
        self.cache.delete_cache_dir().await;
    }

    pub(crate) fn require_cache_key(&self) -> Result<()> {
        self.cache.require_key()
    }

    pub(crate) async fn clear_media_files(
        &self,
        max_age_days: u32,
        size_limit_bytes: u64,
    ) -> Result<u64> {
        let freed = self
            .cache
            .clear_media_files(max_age_days, size_limit_bytes)
            .await?;
        if let Ok(mut sources) = self.media_sources.write() {
            sources.clear();
        }
        Ok(freed)
    }

    /// Resolve an mxc:// URL to a local file path.
    pub(crate) async fn resolve_media(&self, client: &Client, mxc_url: &str) -> Result<PathBuf> {
        self.resolve_media_with_progress(client, mxc_url, |_received, _total, _phase| {})
            .await
    }

    /// Resolve an avatar (or other small display image): fetch a server-generated
    /// `size`×`size` thumbnail and store it under the PLAIN mxc cache key, so the
    /// same paint path finds it exactly as a full resolve would — avatars are only
    /// ever shown small, and avatar mxc URLs are disjoint from message-media URLs, so
    /// this never shadows a full image. Falls back to the full download when the
    /// server can't thumbnail the media, so it never resolves worse than resolve_media.
    pub(crate) async fn resolve_avatar(
        &self,
        client: &Client,
        mxc_url: &str,
        size: u32,
    ) -> Result<PathBuf> {
        self.cache.ensure_cache_dir().await?;
        if let Some(path) = self.cache.cached_plaintext_path(mxc_url, None).await? {
            return Ok(path);
        }

        // Scope the permit to the thumbnail attempt so the full-download fallback
        // (which acquires its own) can't deadlock against a permit we still hold.
        let thumb = {
            let _permit = self
                .download_semaphore
                .acquire()
                .await
                .map_err(|e| anyhow!("media download semaphore closed: {e}"))?;
            let source = self
                .lookup_source(mxc_url)
                .unwrap_or_else(|| MediaSource::Plain(mxc_url.into()));
            let dim = matrix_sdk::ruma::UInt::new(u64::from(size.max(1))).unwrap_or_default();
            let request = matrix_sdk::media::MediaRequestParameters {
                source,
                format: matrix_sdk::media::MediaFormat::Thumbnail(
                    matrix_sdk::media::MediaThumbnailSettings::new(dim, dim),
                ),
            };
            match tokio::time::timeout(
                Duration::from_secs(10),
                client.media().get_media_content(&request, true),
            )
            .await
            {
                Ok(Ok(data)) if !data.is_empty() => Ok(Some(data)),
                // A thumbnail 404 usually means the media is gone, and reporting
                // it permanent here saves a second doomed request — scrolling a
                // big federated room whose avatars have expired would otherwise
                // flood the lane with 404 pairs.
                //
                // But it does NOT mean gone for media uploaded moments ago: the
                // server has not generated the thumbnail yet and answers 404
                // while the ORIGINAL is perfectly fetchable. Treating that as
                // permanent suppressed all retries, so a just-changed room
                // avatar showed the letter placeholder forever. Freshly uploaded
                // urls therefore fall through to the full download, which needs
                // no server-side generation; everything else keeps the
                // one-request-per-dead-avatar behaviour.
                Ok(Err(e)) if matrix_error_is_permanent(&e) && !is_recent_upload(mxc_url) => Err(e),
                // Empty, transient error or timeout: the full media may still be
                // fetchable, so fall through to the download path (which backs off).
                _ => Ok(None),
            }
        };
        match thumb {
            Ok(Some(data)) => self.cache.store_bytes(mxc_url, &data, None).await,
            Ok(None) => self.resolve_media(client, mxc_url).await,
            Err(e) => Err(anyhow::Error::new(e).context(PermanentMediaError)),
        }
    }

    /// Resolve an mxc:// URL to a local file path with local progress updates.
    pub(crate) async fn resolve_media_with_progress<F>(
        &self,
        client: &Client,
        mxc_url: &str,
        progress: F,
    ) -> Result<PathBuf>
    where
        F: Fn(u64, u64, u32) + Send + Sync,
    {
        let mxc: matrix_sdk::ruma::OwnedMxcUri = mxc_url.into();

        self.cache.ensure_cache_dir().await?;

        let cache_key = mxc_url;
        if let Some(path) = self.cache.cached_plaintext_path(cache_key, None).await? {
            return Ok(path);
        }

        // Bound concurrent downloads (held for the whole transfer).
        // Probe: split the permit wait from the transfer so a slow media resolve
        // stays attributable — a large wait_ms means permit starvation, a large
        // dl_ms means the transfer itself is slow.
        let wait_t0 = std::time::Instant::now();
        let _permit = self
            .download_semaphore
            .acquire()
            .await
            .map_err(|e| anyhow!("media download semaphore closed: {e}"))?;
        let wait_ms = wait_t0.elapsed().as_millis();

        let source = self
            .lookup_source(mxc_url)
            .unwrap_or(MediaSource::Plain(mxc));
        let source_url = Self::extract_mxc_url(&source);
        let download_path = self.cache.work_path(cache_key, "download");
        let decrypt_path = self.cache.work_path(cache_key, "decrypt");
        if let Some(parent) = download_path.parent() {
            tokio::fs::create_dir_all(parent).await?;
        }
        let _ = tokio::fs::remove_file(&download_path).await;
        let _ = tokio::fs::remove_file(&decrypt_path).await;

        let dl_t0 = std::time::Instant::now();
        let (received_bytes, total_bytes) =
            Self::download_media_to_path(client, &source_url, &download_path, &progress).await?;
        let dl_ms = dl_t0.elapsed().as_millis();
        if wait_ms > 200 {
            // Verification probe for the interactive/background lane split: this now
            // fires only on real permit starvation (a long wait for a lane permit),
            // which the split should have eliminated for interactive media. Plain
            // module-path target so warn,telematrix_protocol=debug passes it.
            tracing::info!(
                mxc = %source_url,
                wait_ms,
                dl_ms,
                bytes = received_bytes,
                "media resolve waited for a lane permit (starvation check)"
            );
        }

        if let MediaSource::Encrypted(encrypted) = source {
            let decrypt_total = if total_bytes > 0 {
                total_bytes
            } else {
                received_bytes
            };
            progress(
                decrypt_total,
                decrypt_total,
                MEDIA_DOWNLOAD_PHASE_DECRYPTING,
            );

            let encrypted = encrypted.as_ref().clone();
            let download_path_for_blocking = download_path.clone();
            let decrypt_path_for_blocking = decrypt_path.clone();
            let decrypt_result = tokio::task::spawn_blocking(move || -> Result<()> {
                let mut input =
                    std::io::BufReader::new(std::fs::File::open(&download_path_for_blocking)?);
                let mut decryptor = AttachmentDecryptor::new(&mut input, encrypted.into())
                    .map_err(|e| anyhow!("attachment decrypt init failed: {e}"))?;
                let mut output =
                    std::io::BufWriter::new(std::fs::File::create(&decrypt_path_for_blocking)?);
                std::io::copy(&mut decryptor, &mut output)?;
                std::io::Write::flush(&mut output)?;
                Ok(())
            })
            .await;

            match decrypt_result {
                Ok(Ok(())) => {
                    let result = self
                        .cache
                        .store_plaintext(cache_key, &decrypt_path, None)
                        .await;
                    if let Err(err) = result {
                        let _ = tokio::fs::remove_file(&download_path).await;
                        let _ = tokio::fs::remove_file(&decrypt_path).await;
                        return Err(err);
                    }
                }
                Ok(Err(err)) => {
                    let _ = tokio::fs::remove_file(&download_path).await;
                    let _ = tokio::fs::remove_file(&decrypt_path).await;
                    return Err(err);
                }
                Err(err) => {
                    let _ = tokio::fs::remove_file(&download_path).await;
                    let _ = tokio::fs::remove_file(&decrypt_path).await;
                    return Err(anyhow!("attachment decrypt task failed: {err}"));
                }
            }
            let _ = tokio::fs::remove_file(&download_path).await;
        } else if let Err(err) = self
            .cache
            .store_plaintext(cache_key, &download_path, None)
            .await
        {
            let _ = tokio::fs::remove_file(&download_path).await;
            return Err(err);
        }

        self.cache
            .cached_plaintext_path(cache_key, None)
            .await?
            .ok_or_else(|| anyhow!("media cache write did not produce a readable plaintext file"))
    }

    /// Resolve an mxc:// URL into decrypted bytes without writing plaintext to
    /// the temporary media directory.
    pub(crate) async fn resolve_media_bytes_with_progress<F>(
        &self,
        client: &Client,
        mxc_url: &str,
        progress: F,
    ) -> Result<Vec<u8>>
    where
        F: Fn(u64, u64, u32) + Send + Sync,
    {
        let mxc: matrix_sdk::ruma::OwnedMxcUri = mxc_url.into();

        self.cache.ensure_cache_dir().await?;

        if let Some(bytes) = self
            .cache
            .cached_bytes(mxc_url, MAX_MEMORY_MEDIA_BYTES)
            .await?
        {
            return Ok(bytes);
        }

        // Bound concurrent downloads (held for the whole transfer).
        let _permit = self
            .download_semaphore
            .acquire()
            .await
            .map_err(|e| anyhow!("media download semaphore closed: {e}"))?;

        let source = self
            .lookup_source(mxc_url)
            .unwrap_or(MediaSource::Plain(mxc));
        let source_url = Self::extract_mxc_url(&source);
        let downloaded =
            Self::download_media_to_bytes(client, &source_url, MAX_MEMORY_MEDIA_BYTES, &progress)
                .await?;

        let bytes = if let MediaSource::Encrypted(encrypted) = source {
            progress(
                downloaded.len() as u64,
                downloaded.len() as u64,
                MEDIA_DOWNLOAD_PHASE_DECRYPTING,
            );
            let encrypted = encrypted.as_ref().clone();
            tokio::task::spawn_blocking(move || -> Result<Vec<u8>> {
                let mut input = Cursor::new(downloaded);
                let mut decryptor = AttachmentDecryptor::new(&mut input, encrypted.into())
                    .map_err(|e| anyhow!("attachment decrypt init failed: {e}"))?;
                let mut output = Vec::new();
                let mut limited = decryptor.by_ref().take(MAX_MEMORY_MEDIA_BYTES + 1);
                std::io::copy(&mut limited, &mut output)?;
                if output.len() as u64 > MAX_MEMORY_MEDIA_BYTES {
                    return Err(anyhow!("media is too large for memory resolve"));
                }
                Ok(output)
            })
            .await
            .map_err(|err| anyhow!("attachment decrypt task failed: {err}"))??
        } else {
            downloaded
        };

        self.cache.store_encrypted_bytes(mxc_url, &bytes).await?;
        Ok(bytes)
    }

    /// Resolve a server-generated thumbnail for a media URL.
    ///
    /// Tries the Matrix thumbnail API first. When `allow_partial_video` is set
    /// (video server-thumbnail resolution), it then falls back to a partial
    /// download (first 2MB) so QMediaPlayer can extract a frame. That fallback
    /// must NOT run for OG-card / image thumbnails: it would cache a truncated
    /// prefix of the original image under an `.mp4`-labelled key, which then
    /// fails to decode. Image callers pass `false` and fail cleanly instead.
    pub(crate) async fn resolve_media_thumbnail(
        &self,
        client: &Client,
        mxc_url: &str,
        width: u32,
        height: u32,
        allow_partial_video: bool,
    ) -> Result<PathBuf> {
        let mxc: matrix_sdk::ruma::OwnedMxcUri = mxc_url.into();

        self.cache.ensure_cache_dir().await?;

        let cache_key = format!("{mxc_url}_thumb");
        if let Some(path) = self.cache.cached_plaintext_path(&cache_key, None).await? {
            return Ok(path);
        }
        let partial_video_cache_key = format!("{mxc_url}_thumb_partial_mp4");
        if let Some(path) = self
            .cache
            .cached_plaintext_path(&partial_video_cache_key, Some("mp4"))
            .await?
        {
            return Ok(path);
        }

        // Bound concurrent downloads (held across the thumbnail API request and any
        // partial-download fallback).
        let _permit = self
            .download_semaphore
            .acquire()
            .await
            .map_err(|e| anyhow!("media download semaphore closed: {e}"))?;

        let source = self
            .lookup_source(mxc_url)
            .unwrap_or_else(|| MediaSource::Plain(mxc.clone()));

        let is_encrypted = matches!(&source, MediaSource::Encrypted(_));

        // Try 1: Server thumbnail API (works for images, some servers support video).
        let settings = matrix_sdk::media::MediaThumbnailSettings::new(
            matrix_sdk::ruma::UInt::new(u64::from(width.max(1))).unwrap_or_default(),
            matrix_sdk::ruma::UInt::new(u64::from(height.max(1))).unwrap_or_default(),
        );
        let thumb_request = matrix_sdk::media::MediaRequestParameters {
            source: source.clone(),
            format: matrix_sdk::media::MediaFormat::Thumbnail(settings),
        };
        // Retain any terminal (4xx) classification from the thumbnail API for the
        // final error below, in case the partial-video fallback doesn't apply.
        let mut permanent = false;
        match tokio::time::timeout(
            Duration::from_secs(10),
            client.media().get_media_content(&thumb_request, true),
        )
        .await
        {
            Ok(Ok(data)) if !data.is_empty() => {
                return self.cache.store_bytes(&cache_key, &data, None).await;
            }
            Ok(Err(ref e)) => permanent = matrix_error_is_permanent(e),
            _ => {}
        }

        // Try 2: Partial download (first 2MB) for unencrypted media.
        // QMediaPlayer can extract a frame from a partial video file. Only for
        // video callers — see the doc comment (an image prefix would decode to
        // garbage under the `.mp4` key).
        if allow_partial_video && !is_encrypted {
            let mxc_str = mxc.as_str();
            if let Some(rest) = mxc_str.strip_prefix("mxc://") {
                if let Some((server, media_id)) = rest.split_once('/') {
                    let homeserver = client.homeserver().to_string();
                    let hs = homeserver.trim_end_matches('/');

                    // Try authenticated v1 API first, fall back to v3.
                    let urls = [
                        format!("{hs}/_matrix/client/v1/media/download/{server}/{media_id}"),
                        format!("{hs}/_matrix/media/v3/download/{server}/{media_id}"),
                    ];
                    let session = client.matrix_auth().session();
                    let http_client = crate::media_stream::upstream::http_client();
                    let mut resp = None;
                    for url in &urls {
                        let mut req = http_client.get(url).header("Range", "bytes=0-2097151");
                        if let Some(ref s) = session {
                            req = req.header(
                                "Authorization",
                                format!("Bearer {}", s.tokens.access_token),
                            );
                        }
                        match tokio::time::timeout(Duration::from_secs(15), req.send()).await {
                            Ok(Ok(r)) if r.status().is_success() || r.status().as_u16() == 206 => {
                                resp = Some(r);
                                break;
                            }
                            // A definitive 404 leaves any Try-1 permanent classification
                            // standing; anything else (timeout, network, 5xx, 401/403)
                            // is recoverable and must keep the whole resolve retriable.
                            Ok(Ok(r)) if is_permanent_http_status(r.status().as_u16()) => {}
                            _ => permanent = false,
                        }
                    }
                    // This early return used to drop the marker, so a purged video
                    // (thumbnail 404 + both download endpoints 404) was reported
                    // transient and retried forever.
                    let Some(resp) = resp else {
                        let err = anyhow!("partial download: all endpoints failed");
                        return Err(if permanent {
                            err.context(PermanentMediaError)
                        } else {
                            err
                        });
                    };
                    // The media downloaded, so it is NOT gone — only the thumbnail
                    // couldn't be produced. Never let Try 1's 404 mark that permanent.
                    permanent = false;
                    let data = resp
                        .bytes()
                        .await
                        .map_err(|e| anyhow!("partial download read failed: {e}"))?;
                    if data.len() > 1000 {
                        return self
                            .cache
                            .store_bytes(&partial_video_cache_key, &data, Some("mp4"))
                            .await;
                    }
                }
            }
        }

        let err = anyhow!("Could not generate thumbnail for {mxc_url}");
        Err(if permanent {
            err.context(PermanentMediaError)
        } else {
            err
        })
    }

    /// Resolve a server-generated thumbnail into decrypted bytes.
    pub(crate) async fn resolve_media_thumbnail_bytes(
        &self,
        client: &Client,
        mxc_url: &str,
        width: u32,
        height: u32,
    ) -> Result<Vec<u8>> {
        let mxc: matrix_sdk::ruma::OwnedMxcUri = mxc_url.into();

        self.cache.ensure_cache_dir().await?;

        let cache_key = format!("{mxc_url}_thumb");
        if let Some(bytes) = self
            .cache
            .cached_bytes(&cache_key, MAX_MEMORY_MEDIA_BYTES)
            .await?
        {
            return Ok(bytes);
        }

        // Bound concurrent downloads (held for the thumbnail request).
        let _permit = self
            .download_semaphore
            .acquire()
            .await
            .map_err(|e| anyhow!("media download semaphore closed: {e}"))?;

        let source = self
            .lookup_source(mxc_url)
            .unwrap_or(MediaSource::Plain(mxc));

        let settings = matrix_sdk::media::MediaThumbnailSettings::new(
            matrix_sdk::ruma::UInt::new(u64::from(width.max(1))).unwrap_or_default(),
            matrix_sdk::ruma::UInt::new(u64::from(height.max(1))).unwrap_or_default(),
        );
        let thumb_request = matrix_sdk::media::MediaRequestParameters {
            source,
            format: matrix_sdk::media::MediaFormat::Thumbnail(settings),
        };
        let bytes = match tokio::time::timeout(
            Duration::from_secs(10),
            client.media().get_media_content(&thumb_request, true),
        )
        .await
        {
            Err(_) => return Err(anyhow!("thumbnail request timed out for {mxc_url}")),
            Ok(Err(e)) => {
                // Preserve terminal (4xx) classification before stringifying the error.
                let permanent = matrix_error_is_permanent(&e);
                let err = anyhow!("thumbnail request failed for {mxc_url}: {e}");
                return Err(if permanent {
                    err.context(PermanentMediaError)
                } else {
                    err
                });
            }
            Ok(Ok(bytes)) => bytes,
        };
        if bytes.is_empty() {
            return Err(anyhow!(
                "thumbnail request returned empty data for {mxc_url}"
            ));
        }
        if bytes.len() as u64 > MAX_MEMORY_MEDIA_BYTES {
            return Err(anyhow!("thumbnail is too large for memory resolve"));
        }

        self.cache.store_encrypted_bytes(&cache_key, &bytes).await?;
        Ok(bytes)
    }

    /// Download just the first `max_bytes` of an UNENCRYPTED video via a Range
    /// request, cached per `max_bytes` (a larger retry must not read back the
    /// smaller cached prefix), so a frame can be extracted without fetching the
    /// whole file. Returns `None` for encrypted media or when the partial can't be
    /// fetched. Mirrors the partial path in `resolve_media_thumbnail`.
    async fn fetch_partial_video(
        &self,
        client: &Client,
        mxc_url: &str,
        max_bytes: u64,
    ) -> Result<Option<std::path::PathBuf>> {
        let partial_key = format!("{mxc_url}_thumb_partial_mp4_{max_bytes}");
        if let Some(path) = self
            .cache
            .cached_plaintext_path(&partial_key, Some("mp4"))
            .await?
        {
            return Ok(Some(path));
        }
        // Fetch + decrypt the first frames via the streaming proxy module: it
        // reuses the proxy's upstream fetch, CTR-decrypt, and persistent cache (so
        // an already-streamed video's frame comes straight off disk). Unlike the
        // old path this also handles ENCRYPTED media — CTR is seekable, so the
        // prefix decrypts standalone (full-file SHA integrity isn't verified, which
        // is fine for a preview). The fetch caps the transfer at `max_bytes` even
        // when the homeserver ignores Range and replies 200 with the whole file.
        let source = self
            .lookup_source(mxc_url)
            .unwrap_or_else(|| MediaSource::Plain(mxc_url.into()));
        let cache_dir = self.cache.stream_cache_dir();
        let data = match crate::media_stream::cache::fetch_decrypted_prefix(
            client, mxc_url, &source, &cache_dir, max_bytes,
        )
        .await
        {
            Ok(d) => d,
            // Partial (streaming-prefix) fetch failed — fall back silently; the
            // caller resolves the full video for thumbnailing.
            Err(_) => return Ok(None),
        };
        // These decrypted bytes already answer "can this video stream?" — learn it
        // here, while merely rendering the poster, so a later click shows real
        // download progress immediately. Persisted, because the media cache is
        // LRU-capped and evicts the video long before this small thumbnail.
        if let crate::media_stream::container::Verdict::Decided(verdict) =
            crate::media_stream::container::classify(&data)
        {
            crate::container_store::store(client, mxc_url.to_string(), verdict).await;
        }
        if data.len() <= 1000 {
            return Ok(None);
        }
        self.cache
            .store_bytes(&partial_key, &data, Some("mp4"))
            .await
            .map(Some)
    }

    /// Produce a JPEG thumbnail for a video, decoding one frame locally.
    ///
    /// Fast path: a previously-derived thumbnail (keyed by `event_id`) is
    /// returned straight from the encrypted `thumbnails/` cache — no ffmpeg, no
    /// network. Otherwise a frame source is chosen cheapest-first (already-cached
    /// full video → a partial range download → full download only as a fallback),
    /// one frame is extracted and scaled to `width`×`height` via libav on a
    /// blocking task, and the resulting JPEG is cached encrypted before return.
    pub(crate) async fn get_video_thumbnail(
        &self,
        client: &Client,
        event_id: &str,
        mxc_url: &str,
        width: u32,
        height: u32,
    ) -> Result<Vec<u8>> {
        let started = std::time::Instant::now();
        tracing::info!("[VIDTHUMB] request event={event_id} mxc={mxc_url} {width}x{height}");
        self.cache.ensure_cache_dir().await?;

        let thumb_key = format!("vidthumb:{event_id}:{width}x{height}");

        // (a) Fast path — already derived and cached.
        if let Some(bytes) = self.cache.cached_thumbnail_bytes(&thumb_key).await? {
            tracing::info!(
                "[VIDTHUMB] cache HIT event={event_id} ({} bytes) in {:?}",
                bytes.len(),
                started.elapsed()
            );
            return Ok(bytes);
        }
        tracing::info!("[VIDTHUMB] cache miss event={event_id}; resolving video");

        // (b) Pick the cheapest frame source. NEVER download the whole video just
        // for a preview: prefer the already-downloaded full file (no network), else
        // a bounded PARTIAL range download (2MB, then a larger capped retry). A
        // moov-at-end video too large for the cap simply gets no preview rather than
        // a full download. ffmpeg is blocking, so each extraction runs on a blocking
        // thread.
        let (w, h) = (width, height);
        let mut bytes: Option<Vec<u8>> = None;
        let mut via = "none";

        // 1. Already-downloaded full video → extract directly, no network.
        if let Some(full) = self.cache.cached_plaintext_path(mxc_url, None).await? {
            match tokio::task::spawn_blocking(move || {
                crate::video_thumbnail_service::extract_thumbnail_jpeg(&full, w, h)
            })
            .await
            {
                Ok(Ok(b)) => {
                    via = "full(cached)";
                    bytes = Some(b);
                }
                Ok(Err(e)) => tracing::info!(
                    "[VIDTHUMB] cached-full extract failed event={event_id} ({e}); trying partial"
                ),
                Err(e) => tracing::warn!(
                    "[VIDTHUMB] cached-full extract task failed event={event_id}: {e}"
                ),
            }
        }

        // 2. Else a partial range download (leading 2MB — faststart mp4).
        if bytes.is_none() {
            if let Ok(Some(partial)) = self
                .fetch_partial_video(client, mxc_url, VIDTHUMB_FASTSTART_BYTES)
                .await
            {
                tracing::info!(
                    "[VIDTHUMB] partial fetched event={event_id} in {:?}; extracting",
                    started.elapsed()
                );
                match tokio::task::spawn_blocking(move || {
                    crate::video_thumbnail_service::extract_thumbnail_jpeg(&partial, w, h)
                })
                .await
                {
                    Ok(Ok(b)) => {
                        via = "partial";
                        bytes = Some(b);
                    }
                    Ok(Err(e)) => tracing::info!(
                        "[VIDTHUMB] partial extract failed event={event_id} ({e}); \
                         trying a larger bounded partial"
                    ),
                    Err(e) => tracing::warn!(
                        "[VIDTHUMB] partial extract task failed event={event_id}: {e}"
                    ),
                }
            }
        }

        // 3. Larger BOUNDED partial (never the whole video). Decodes a small
        // moov-at-end clip whose prefix spans the entire file; a large one falls
        // through to the placeholder instead of a full download.
        if bytes.is_none() {
            if let Ok(Some(partial)) = self
                .fetch_partial_video(client, mxc_url, VIDTHUMB_MAX_PARTIAL_BYTES)
                .await
            {
                if let Ok(Ok(b)) = tokio::task::spawn_blocking(move || {
                    crate::video_thumbnail_service::extract_thumbnail_jpeg(&partial, w, h)
                })
                .await
                {
                    via = "partial(large)";
                    bytes = Some(b);
                }
            }
        }

        let Some(bytes) = bytes else {
            // No frame within the bounded partials (e.g. a large moov-at-end mp4).
            // The UI shows the video placeholder rather than paying a full download.
            tracing::info!(
                "[VIDTHUMB] no decodable frame within {VIDTHUMB_MAX_PARTIAL_BYTES} bytes \
                 event={event_id} in {:?}",
                started.elapsed()
            );
            return Err(anyhow!(
                "no decodable frame within the bounded partial for {event_id}"
            ));
        };
        tracing::info!(
            "[VIDTHUMB] extracted event={event_id} ({} bytes) via {via} in {:?}",
            bytes.len(),
            started.elapsed()
        );

        // Cache the derived thumbnail (best-effort; failure to cache must not
        // fail the request).
        if let Err(err) = self.cache.store_thumbnail_bytes(&thumb_key, &bytes).await {
            tracing::warn!("[VIDTHUMB] cache store failed for {event_id}: {err}");
        } else {
            tracing::info!(
                "[VIDTHUMB] cached event={event_id}; total {:?}",
                started.elapsed()
            );
        }
        Ok(bytes)
    }

    /// Export media directly to a user-selected path. This avoids keeping a
    /// decrypted temp file solely to support "Save As".
    pub(crate) async fn export_media_to_path<F, Fut>(
        &self,
        require_client: F,
        mxc_url: &str,
        target_path: &str,
    ) -> Result<()>
    where
        F: FnOnce() -> Fut,
        Fut: Future<Output = Result<Client>>,
    {
        if target_path.is_empty() {
            return Err(anyhow!("empty media export target path"));
        }
        let target = std::path::PathBuf::from(target_path);
        if let Some(parent) = target.parent() {
            tokio::fs::create_dir_all(parent).await?;
        }
        let target_name = target
            .file_name()
            .map(|name| name.to_string_lossy().into_owned())
            .unwrap_or_else(|| String::from("media"));
        let temp_target =
            target.with_file_name(format!(".{target_name}.{}.tm-exporting", rand_id()));
        let _ = tokio::fs::remove_file(&temp_target).await;

        self.cache.require_key()?;
        self.cache.ensure_cache_dir().await?;
        let download_path = self.cache.work_path(mxc_url, "export-download");

        let export_result = async {
            if self
                .cache
                .decrypt_cache_to_file(mxc_url, &temp_target)
                .await?
            {
                return Ok(());
            }

            let client = require_client().await?;
            let mxc: matrix_sdk::ruma::OwnedMxcUri = mxc_url.into();
            let source = self
                .lookup_source(mxc_url)
                .unwrap_or(MediaSource::Plain(mxc));
            let source_url = Self::extract_mxc_url(&source);

            if let Some(parent) = download_path.parent() {
                tokio::fs::create_dir_all(parent).await?;
            }

            if let MediaSource::Encrypted(encrypted) = source {
                let _ = tokio::fs::remove_file(&download_path).await;
                Self::download_media_to_path(
                    &client,
                    &source_url,
                    &download_path,
                    &|_received, _total, _phase| {},
                )
                .await?;

                let encrypted = encrypted.as_ref().clone();
                let download_path_for_blocking = download_path.clone();
                let temp_target_for_blocking = temp_target.clone();
                tokio::task::spawn_blocking(move || -> Result<()> {
                    let mut input =
                        std::io::BufReader::new(std::fs::File::open(&download_path_for_blocking)?);
                    let mut decryptor = AttachmentDecryptor::new(&mut input, encrypted.into())
                        .map_err(|e| anyhow!("attachment decrypt init failed: {e}"))?;
                    let mut output =
                        std::io::BufWriter::new(std::fs::File::create(&temp_target_for_blocking)?);
                    std::io::copy(&mut decryptor, &mut output)?;
                    std::io::Write::flush(&mut output)?;
                    Ok(())
                })
                .await
                .map_err(|err| anyhow!("attachment decrypt task failed: {err}"))??;
            } else {
                Self::download_media_to_path(
                    &client,
                    &source_url,
                    &temp_target,
                    &|_received, _total, _phase| {},
                )
                .await?;
            }

            self.cache
                .encrypt_file_to_cache(mxc_url, &temp_target)
                .await?;
            Ok(())
        }
        .await;

        let _ = tokio::fs::remove_file(&download_path).await;
        if let Err(err) = export_result {
            let _ = tokio::fs::remove_file(&temp_target).await;
            return Err(err);
        }

        if let Err(err) = tokio::fs::rename(&temp_target, &target).await {
            let _ = tokio::fs::remove_file(&temp_target).await;
            return Err(err.into());
        }
        Ok(())
    }

    fn lookup_source(&self, mxc_url: &str) -> Option<MediaSource> {
        self.media_sources
            .read()
            .ok()
            .and_then(|cache| cache.get(mxc_url).cloned())
    }

    fn extract_mxc_url(source: &MediaSource) -> String {
        match source {
            MediaSource::Plain(mxc_uri) => mxc_uri.to_string(),
            MediaSource::Encrypted(encrypted) => encrypted.url.to_string(),
        }
    }

    fn should_report_media_progress(
        received: u64,
        total: u64,
        last_reported: u64,
        last_report_at: Instant,
    ) -> bool {
        if received <= last_reported {
            return false;
        }
        let byte_step = if total > 0 {
            (total / 100).max(MEDIA_PROGRESS_MIN_STEP_BYTES)
        } else {
            MEDIA_PROGRESS_MIN_STEP_BYTES
        };
        received - last_reported >= byte_step || last_report_at.elapsed() >= MEDIA_PROGRESS_INTERVAL
    }

    async fn download_media_to_path<F>(
        client: &Client,
        source_url: &str,
        download_path: &Path,
        progress: &F,
    ) -> Result<(u64, u64)>
    where
        F: Fn(u64, u64, u32) + Send + Sync,
    {
        let rest = source_url
            .strip_prefix("mxc://")
            .ok_or_else(|| anyhow!("Not an mxc URL: {source_url}"))?;
        let (server, media_id) = rest
            .split_once('/')
            .ok_or_else(|| anyhow!("Malformed mxc URL: {source_url}"))?;

        let homeserver = client.homeserver().to_string();
        let hs = homeserver.trim_end_matches('/');
        let urls = [
            format!("{hs}/_matrix/client/v1/media/download/{server}/{media_id}"),
            format!("{hs}/_matrix/media/v3/download/{server}/{media_id}"),
        ];

        let access_token = client
            .matrix_auth()
            .session()
            .map(|session| session.tokens.access_token.clone());

        let http_client = crate::media_stream::upstream::http_client();
        let mut response = None;
        let mut last_error = None;
        // Stays true only while every failed attempt was a permanent client error
        // (4xx except 429); one transient failure (timeout, network, 5xx, rate-limit)
        // clears it so the caller keeps retrying with backoff.
        let mut permanent = true;

        for url in &urls {
            let mut request = http_client.get(url);
            if let Some(token) = access_token.as_ref() {
                request = request.header("Authorization", format!("Bearer {token}"));
            }

            match tokio::time::timeout(Duration::from_secs(30), request.send()).await {
                Ok(Ok(resp)) if resp.status().is_success() => {
                    response = Some(resp);
                    break;
                }
                Ok(Ok(resp)) => {
                    let status = resp.status();
                    if !is_permanent_http_status(status.as_u16()) {
                        permanent = false;
                    }
                    last_error = Some(anyhow!(
                        "download endpoint {} failed with HTTP {}",
                        url,
                        status
                    ));
                }
                Ok(Err(err)) => {
                    permanent = false;
                    last_error = Some(anyhow!("download request failed for {}: {err}", url));
                }
                Err(_) => {
                    permanent = false;
                    last_error = Some(anyhow!("download request timed out for {}", url));
                }
            }
        }

        let mut response = match response {
            Some(resp) => resp,
            None => {
                let err =
                    last_error.unwrap_or_else(|| anyhow!("all media download endpoints failed"));
                return Err(if permanent {
                    err.context(PermanentMediaError)
                } else {
                    err
                });
            }
        };

        let total_bytes = response.content_length().unwrap_or(0);
        progress(0, total_bytes, MEDIA_DOWNLOAD_PHASE_DOWNLOADING);

        let _ = tokio::fs::remove_file(download_path).await;
        let mut output = tokio::fs::File::create(download_path).await?;
        let mut received_bytes = 0u64;
        let mut last_reported = 0u64;
        let mut last_report_at = Instant::now();

        while let Some(chunk) = response.chunk().await? {
            if chunk.is_empty() {
                continue;
            }
            output.write_all(&chunk).await?;
            received_bytes += chunk.len() as u64;
            if Self::should_report_media_progress(
                received_bytes,
                total_bytes,
                last_reported,
                last_report_at,
            ) {
                progress(
                    received_bytes,
                    total_bytes,
                    MEDIA_DOWNLOAD_PHASE_DOWNLOADING,
                );
                last_reported = received_bytes;
                last_report_at = Instant::now();
            }
        }

        output.flush().await?;
        if received_bytes != last_reported || total_bytes == 0 {
            progress(
                received_bytes,
                total_bytes,
                MEDIA_DOWNLOAD_PHASE_DOWNLOADING,
            );
        }

        Ok((received_bytes, total_bytes))
    }

    async fn download_media_to_bytes<F>(
        client: &Client,
        source_url: &str,
        max_bytes: u64,
        progress: &F,
    ) -> Result<Vec<u8>>
    where
        F: Fn(u64, u64, u32) + Send + Sync,
    {
        let rest = source_url
            .strip_prefix("mxc://")
            .ok_or_else(|| anyhow!("Not an mxc URL: {source_url}"))?;
        let (server, media_id) = rest
            .split_once('/')
            .ok_or_else(|| anyhow!("Malformed mxc URL: {source_url}"))?;

        let homeserver = client.homeserver().to_string();
        let hs = homeserver.trim_end_matches('/');
        let urls = [
            format!("{hs}/_matrix/client/v1/media/download/{server}/{media_id}"),
            format!("{hs}/_matrix/media/v3/download/{server}/{media_id}"),
        ];

        let access_token = client
            .matrix_auth()
            .session()
            .map(|session| session.tokens.access_token.clone());

        let http_client = crate::media_stream::upstream::http_client();
        let mut response = None;
        let mut last_error = None;
        // Stays true only while every failed attempt was a permanent client error
        // (4xx except 429); one transient failure (timeout, network, 5xx, rate-limit)
        // clears it so the caller keeps retrying with backoff.
        let mut permanent = true;

        for url in &urls {
            let mut request = http_client.get(url);
            if let Some(token) = access_token.as_ref() {
                request = request.header("Authorization", format!("Bearer {token}"));
            }

            match tokio::time::timeout(Duration::from_secs(30), request.send()).await {
                Ok(Ok(resp)) if resp.status().is_success() => {
                    response = Some(resp);
                    break;
                }
                Ok(Ok(resp)) => {
                    let status = resp.status();
                    if !is_permanent_http_status(status.as_u16()) {
                        permanent = false;
                    }
                    last_error = Some(anyhow!(
                        "download endpoint {} failed with HTTP {}",
                        url,
                        status
                    ));
                }
                Ok(Err(err)) => {
                    permanent = false;
                    last_error = Some(anyhow!("download request failed for {}: {err}", url));
                }
                Err(_) => {
                    permanent = false;
                    last_error = Some(anyhow!("download request timed out for {}", url));
                }
            }
        }

        let mut response = match response {
            Some(resp) => resp,
            None => {
                let err =
                    last_error.unwrap_or_else(|| anyhow!("all media download endpoints failed"));
                return Err(if permanent {
                    err.context(PermanentMediaError)
                } else {
                    err
                });
            }
        };

        let total_bytes = response.content_length().unwrap_or(0);
        if total_bytes > max_bytes {
            return Err(anyhow!("media is too large for memory resolve"));
        }
        progress(0, total_bytes, MEDIA_DOWNLOAD_PHASE_DOWNLOADING);

        let mut bytes = Vec::with_capacity(total_bytes.min(max_bytes).try_into().unwrap_or(0));
        let mut received_bytes = 0u64;
        let mut last_reported = 0u64;
        let mut last_report_at = Instant::now();

        while let Some(chunk) = response.chunk().await? {
            if chunk.is_empty() {
                continue;
            }
            bytes.extend_from_slice(&chunk);
            received_bytes += chunk.len() as u64;
            if received_bytes > max_bytes {
                return Err(anyhow!("media is too large for memory resolve"));
            }
            if Self::should_report_media_progress(
                received_bytes,
                total_bytes,
                last_reported,
                last_report_at,
            ) {
                progress(
                    received_bytes,
                    total_bytes,
                    MEDIA_DOWNLOAD_PHASE_DOWNLOADING,
                );
                last_reported = received_bytes;
                last_report_at = Instant::now();
            }
        }

        if received_bytes != last_reported || total_bytes == 0 {
            progress(
                received_bytes,
                total_bytes,
                MEDIA_DOWNLOAD_PHASE_DOWNLOADING,
            );
        }

        Ok(bytes)
    }
}

fn rand_id() -> u32 {
    let t = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos() as u32;
    let mut x = t.wrapping_add(0x9E37_79B9);
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    x
}

#[cfg(test)]
mod tests {
    use super::*;
    use anyhow::anyhow;

    #[test]
    fn only_404_is_permanent() {
        assert!(is_permanent_http_status(404)); // media is gone
                                                // Everything else must stay retriable. 401/403 in particular: the raw
                                                // download paths attach the token by hand, so a rotated token or a captive
                                                // portal must not blank media for the rest of the session.
        assert!(!is_permanent_http_status(401));
        assert!(!is_permanent_http_status(403));
        assert!(!is_permanent_http_status(400));
        assert!(!is_permanent_http_status(429)); // rate-limited
        assert!(!is_permanent_http_status(500)); // server error
        assert!(!is_permanent_http_status(502));
        assert!(!is_permanent_http_status(200));
    }

    #[test]
    fn detects_permanent_marker_through_anyhow_context() {
        // The exact shape the download paths produce: a stringified error with the
        // PermanentMediaError attached via `.context()`. This must be detected —
        // a chain scan (`.is::<PermanentMediaError>()`) fails here because anyhow
        // hides the value inside a ContextError wrapper.
        let err = anyhow!("thumbnail request failed: HTTP 404").context(PermanentMediaError);
        assert!(is_permanent_media_error(&err));
    }

    #[test]
    fn detects_permanent_marker_even_when_further_wrapped() {
        // A later `?` / `.context()` layer must not hide the marker.
        let err = anyhow!("boom")
            .context(PermanentMediaError)
            .context("while resolving media");
        assert!(is_permanent_media_error(&err));
    }

    #[test]
    fn plain_errors_are_not_permanent() {
        assert!(!is_permanent_media_error(&anyhow!(
            "download request timed out"
        )));
    }
}

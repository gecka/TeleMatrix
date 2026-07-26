// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::collections::HashMap;
use std::convert::Infallible;
use std::sync::{Arc, Mutex};

use http_body_util::combinators::BoxBody;
use http_body_util::{BodyExt, Full, StreamBody};
use hyper::body::{Bytes, Frame};
use hyper::service::service_fn;
use hyper::{Request, Response, StatusCode};
use hyper_util::rt::TokioIo;
use matrix_sdk::ruma::events::room::MediaSource;
use matrix_sdk::Client;
use rand::{rngs::SysRng, TryRng};
use tokio::net::TcpListener;
use tokio::sync::oneshot;
use tokio::sync::RwLock;

use super::cache::{EntryReader, MediaCache};
use super::decrypt::{decrypt_ctr_range, key_iv_from_encrypted};
use super::range::parse_range;

/// Bytes served per body chunk. Reads near the download frontier still return
/// smaller partial reads (bounded by what's written), so latency is unaffected;
/// only cold replays of already-downloaded regions batch into 1 MiB reads.
const STREAM_CHUNK: u64 = 1024 * 1024;

/// Streaming response body: either a progressive stream off the cache, or a small
/// empty body for error statuses — both boxed to one type. The stream's error is
/// `std::io::Error` (cache file reads).
type StreamBoxBody = BoxBody<Bytes, std::io::Error>;

/// Dependencies the loopback server needs to resolve, fetch, and cache media.
pub struct StreamDeps {
    pub client: Arc<RwLock<Option<Client>>>,
    pub media_sources: Arc<std::sync::RwLock<HashMap<String, MediaSource>>>,
    pub runtime: tokio::runtime::Handle,
    pub cache: Arc<MediaCache>,
}

/// A `127.0.0.1` HTTP server that streams Matrix media with Range support,
/// decrypting encrypted attachments per-range on the fly, backed by an on-disk
/// progressive cache (homeservers don't honor `Range`, so the proxy owns it).
pub struct MediaStreamServer {
    port: u16,
    secret: String,
    shutdown: Mutex<Option<oneshot::Sender<()>>>,
    cache: Arc<MediaCache>,
}

impl MediaStreamServer {
    /// Bind a loopback listener on an ephemeral port and spawn the accept loop on
    /// `deps.runtime`. Returns once bound (so `stream_url` is immediately usable).
    pub async fn start(deps: StreamDeps) -> anyhow::Result<MediaStreamServer> {
        let listener = TcpListener::bind(("127.0.0.1", 0)).await?;
        let port = listener.local_addr()?.port();

        let mut secret_bytes = [0u8; 16];
        SysRng.try_fill_bytes(&mut secret_bytes)?;
        let mut secret = String::with_capacity(32);
        for b in secret_bytes {
            secret.push_str(&format!("{b:02x}"));
        }

        let (shutdown_tx, mut shutdown_rx) = oneshot::channel::<()>();

        let cache = deps.cache.clone();
        let deps = Arc::new(deps);
        let secret_for_loop = secret.clone();
        let runtime = deps.runtime.clone();

        // Reclaim partial/orphan leftovers + enforce the disk budget off the caller:
        // start() is reached via block_on on the UI thread, and this scans/stats/
        // unlinks the whole cache dir. The accept loop is already usable; a first
        // request only touches its own mxc's file, so a slightly-late sweep is fine.
        let cache_for_sweep = cache.clone();
        runtime.spawn_blocking(move || cache_for_sweep.reclaim_and_enforce());

        runtime.spawn(async move {
            loop {
                tokio::select! {
                    accepted = listener.accept() => {
                        let stream = match accepted {
                            Ok((stream, _)) => stream,
                            Err(_) => continue,
                        };
                        let deps = deps.clone();
                        let secret = secret_for_loop.clone();
                        tokio::spawn(async move {
                            let service = service_fn(move |req| {
                                let deps = deps.clone();
                                let secret = secret.clone();
                                async move { Ok::<_, Infallible>(handle(req, deps, secret).await) }
                            });
                            let io = TokioIo::new(stream);
                            let _ = hyper::server::conn::http1::Builder::new()
                                .serve_connection(io, service)
                                .await;
                        });
                    }
                    _ = &mut shutdown_rx => break,
                }
            }
        });

        Ok(MediaStreamServer {
            port,
            secret,
            shutdown: Mutex::new(Some(shutdown_tx)),
            cache,
        })
    }

    /// Loopback URL the media player can open for `mxc`.
    ///
    /// The `mxc://` scheme prefix is stripped so the path contains no `://`
    /// double-slash, which Qt/QMediaPlayer may normalize away. The handler
    /// reconstructs `mxc://{remainder}` from the path segment after `/{secret}/`.
    pub fn stream_url(&self, mxc: &str) -> String {
        let rest = mxc.strip_prefix("mxc://").unwrap_or(mxc);
        format!("http://127.0.0.1:{}/{}/{}", self.port, self.secret, rest)
    }

    /// Drop the in-memory cache entries (used by "clear local cache" before the
    /// on-disk files are wiped). The accept loop keeps running.
    pub async fn clear_cache_entries(&self) {
        self.cache.clear_entries().await;
    }

    /// Enforce the stream cache's (setting-derived) disk budget now (e.g. when the
    /// media size setting changes mid-session).
    pub async fn enforce_stream_budget(&self) {
        self.cache.enforce_budget().await;
    }

    /// Download progress (written, total) for `mxc`, if known.
    pub async fn progress(&self, mxc: &str) -> Option<(u64, u64)> {
        self.cache.progress(mxc).await
    }

    /// Whether `mxc`'s current download has failed (see `MediaCache::errored`).
    pub async fn errored(&self, mxc: &str) -> bool {
        self.cache.errored(mxc).await
    }

    /// Signal the accept loop to stop, cancel any in-flight downloads (so none keeps
    /// pulling a whole file into an about-to-be-deleted inode), and drop the disk
    /// cache. Idempotent.
    pub async fn stop(&self) {
        if let Ok(mut guard) = self.shutdown.lock() {
            if let Some(tx) = guard.take() {
                let _ = tx.send(());
            }
        }
        self.cache.cancel_all().await;
        self.cache.cleanup();
    }
}

/// Panic-free request entry point: every outcome maps to an HTTP status. `panic=abort`
/// is fatal, so the real work lives in `try_handle` and errors become a `502`.
async fn handle(
    req: Request<hyper::body::Incoming>,
    deps: Arc<StreamDeps>,
    secret: String,
) -> Response<StreamBoxBody> {
    match try_handle(req, &deps, &secret).await {
        Ok(resp) => resp,
        Err(HandlerError::NotFound) => status_response(StatusCode::NOT_FOUND),
        Err(HandlerError::Unavailable) => status_response(StatusCode::SERVICE_UNAVAILABLE),
        Err(HandlerError::Bad(e)) => {
            tracing::warn!("media stream request failed: {e:#}");
            status_response(StatusCode::BAD_GATEWAY)
        }
    }
}

enum HandlerError {
    NotFound,
    Unavailable,
    Bad(anyhow::Error),
}

impl From<anyhow::Error> for HandlerError {
    fn from(e: anyhow::Error) -> Self {
        HandlerError::Bad(e)
    }
}

async fn try_handle(
    req: Request<hyper::body::Incoming>,
    deps: &StreamDeps,
    secret: &str,
) -> std::result::Result<Response<StreamBoxBody>, HandlerError> {
    let prefix = format!("/{secret}/");
    let path = req.uri().path();
    let remainder = path.strip_prefix(&prefix).ok_or(HandlerError::NotFound)?;
    if remainder.is_empty() {
        return Err(HandlerError::NotFound);
    }
    // Reconstruct the full mxc URI (stream_url strips the "mxc://" prefix to
    // avoid Qt normalizing the double-slash away in the URL path).
    let mxc = format!("mxc://{remainder}");

    let range_header = req
        .headers()
        .get(hyper::header::RANGE)
        .and_then(|v| v.to_str().ok())
        .map(|s| s.to_string());

    // Snapshot the client (we hand a clone to the cache's download task).
    let client = {
        let guard = deps.client.read().await;
        guard.clone().ok_or(HandlerError::Unavailable)?
    };

    let source = deps
        .media_sources
        .read()
        .ok()
        .and_then(|m| m.get(&mxc).cloned())
        .unwrap_or_else(|| MediaSource::Plain(mxc.as_str().into()));

    // Get (or start) the single progressive download for this mxc, then wait only
    // for its size (headers), not the whole body, before answering.
    let entry = deps
        .cache
        .get_or_start(&mxc, source, client, deps.runtime.clone())
        .await;
    let total = entry.wait_total().await.map_err(HandlerError::Bad)?;

    let requested = parse_range(range_header.as_deref(), total);
    let start = requested.map(|r| r.start).unwrap_or(0);
    // Honor the range END too (the old proxy always streamed start..EOF): a bounded
    // probe like `bytes=0-32767` now transfers exactly what was asked instead of
    // running until the client hangs up. Open-ended playback ranges (`bytes=N-`)
    // map to end_inclusive = total-1, so they still stream to EOF as before.
    let end_exclusive = requested
        .map(|r| r.end_inclusive + 1)
        .unwrap_or(total)
        .min(total);

    // Extract the decrypt key/iv once for encrypted media; the streaming body
    // decrypts each emitted chunk at its absolute offset (CTR is seekable).
    let key_iv = match &entry.source {
        MediaSource::Encrypted(file) => {
            Some(key_iv_from_encrypted(file).map_err(HandlerError::Bad)?)
        }
        _ => None,
    };

    // On the initial (offset-0) request, sniff the container of the decrypted
    // bytes so a player-side "Failed to load media" can be classified from the
    // logs: an UNRECOGNISED header means decryption produced garbage (key/iv
    // mismatch); a recognised container that still won't play means an
    // unsupported codec in the FFmpeg backend.
    if start == 0 {
        let source_kind = match &entry.source {
            MediaSource::Encrypted(_) => "encrypted",
            MediaSource::Plain(_) => "plain",
        };
        // 64 bytes: enough for `describe` to walk past `ftyp` and report whether
        // the `moov` leads or trails, not just which container this is.
        let container = match entry.read_chunk(0, 64).await {
            Ok(Some(mut head)) => {
                if let Some((key, iv)) = &key_iv {
                    decrypt_ctr_range(key, iv, 0, &mut head);
                }
                super::container::describe(&head)
            }
            _ => "<unread>",
        };
        if container.starts_with("UNRECOGNISED") {
            tracing::warn!(
                "media stream {mxc}: {source_kind}, {total} bytes, container={container} — \
                 decrypted bytes are not a known media container (decryption failure?)"
            );
        } else {
            tracing::debug!(
                "media stream {mxc}: {source_kind}, {total} bytes, container={container}"
            );
        }
    }

    // One persistent reader for this response (keeps the cache file open across
    // chunks instead of reopening per 256 KiB — see EntryReader).
    let reader = EntryReader::new(entry);
    let stream = futures_util::stream::unfold(
        (reader, start, key_iv, end_exclusive),
        move |(mut reader, offset, key_iv, end_exclusive)| async move {
            if offset >= end_exclusive {
                return None; // served the whole requested range
            }
            let want = STREAM_CHUNK.min(end_exclusive - offset) as usize;
            match reader.read_at(offset, want).await {
                Ok(Some(mut chunk)) => {
                    if let Some((key, iv)) = key_iv {
                        decrypt_ctr_range(&key, &iv, offset, &mut chunk);
                    }
                    let next = offset + chunk.len() as u64;
                    let frame: Result<Frame<Bytes>, std::io::Error> =
                        Ok(Frame::data(Bytes::from(chunk)));
                    Some((frame, (reader, next, key_iv, end_exclusive)))
                }
                // Clean EOF: served everything available; end the stream normally.
                Ok(None) => None,
                // The download died mid-body (e.g. a connection dropped after sleep).
                // Emit an ERROR frame so hyper aborts the connection instead of ending
                // it cleanly short of the advertised Content-Length. The player then
                // sees a network error (QMediaPlayer::ResourceError, which is retried)
                // rather than a truncated-but-clean body missing the trailing `moov`
                // (an unrecoverable QMediaPlayer::FormatError). Advance offset to
                // end_exclusive so the next unfold step terminates the stream.
                Err(e) => {
                    let frame: Result<Frame<Bytes>, std::io::Error> =
                        Err(std::io::Error::other(e.to_string()));
                    Some((frame, (reader, end_exclusive, key_iv, end_exclusive)))
                }
            }
        },
    );
    let body = StreamBody::new(stream).boxed();

    let serve_len = end_exclusive.saturating_sub(start);
    let status = if range_header.is_some() {
        StatusCode::PARTIAL_CONTENT
    } else {
        StatusCode::OK
    };
    let mut builder = Response::builder()
        .status(status)
        .header(hyper::header::ACCEPT_RANGES, "bytes")
        .header(hyper::header::CONTENT_TYPE, "application/octet-stream")
        .header(hyper::header::CONTENT_LENGTH, serve_len);
    if range_header.is_some() {
        builder = builder.header(
            hyper::header::CONTENT_RANGE,
            format!(
                "bytes {}-{}/{}",
                start,
                end_exclusive.saturating_sub(1),
                total
            ),
        );
    }
    match builder.body(body) {
        Ok(resp) => Ok(resp),
        Err(_) => Ok(status_response(StatusCode::INTERNAL_SERVER_ERROR)),
    }
}

/// A small empty body, boxed to the streaming body type (for error statuses).
fn empty_body() -> StreamBoxBody {
    Full::new(Bytes::new()).map_err(|e| match e {}).boxed()
}

fn status_response(status: StatusCode) -> Response<StreamBoxBody> {
    Response::builder()
        .status(status)
        .body(empty_body())
        .unwrap_or_else(|_| Response::new(empty_body()))
}

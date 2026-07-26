// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::sync::OnceLock;
use std::time::Duration;

use anyhow::{anyhow, Result};
use matrix_sdk::Client;

/// Bound how long the upstream media GET may hang, and how many times the open is
/// retried. `READ_TIMEOUT` is the max idle gap between body chunks; it also applies
/// to the later `resp.chunk()` reads in `download_to_file`, so a mid-download stall
/// surfaces as an error (the proxy then drops the entry so a replay restarts it)
/// instead of hanging forever. Keep the C++ player's stall-detection window (see
/// `history_inline_video.cpp`) longer than this so a dead download is diagnosed as
/// failed — not merely slow — before the player restarts it.
const CONNECT_TIMEOUT: Duration = Duration::from_secs(15);
const READ_TIMEOUT: Duration = Duration::from_secs(30);
const OPEN_ATTEMPTS: u32 = 3;
const OPEN_BACKOFF: Duration = Duration::from_millis(600);

/// One shared HTTP client for all media fetches: building a fresh `reqwest::Client`
/// per open (as this module used to) means a new connection pool + TLS handshake
/// for every video and every thumbnail probe. Built lazily so the timeouts apply.
/// Shared with `media_transfer_service` so plain downloads/thumbnails reuse the
/// same pool and inherit the connect+read (idle-gap) timeouts — the read timeout
/// doubles as a mid-download stall watchdog.
pub(crate) fn http_client() -> &'static reqwest::Client {
    static HTTP: OnceLock<reqwest::Client> = OnceLock::new();
    HTTP.get_or_init(|| {
        reqwest::Client::builder()
            .connect_timeout(CONNECT_TIMEOUT)
            .read_timeout(READ_TIMEOUT)
            // After sleep / network change, idle keep-alive sockets in the pool are
            // dead but still get checked out (connect_timeout does NOT apply to a
            // reused connection), so the first post-wake fetch stalls to read_timeout
            // and the body dies mid-stream. Media fetches are infrequent and large,
            // so don't reuse idle sockets, and enable TCP keepalive so half-open ones
            // are detected/dropped rather than handed to a new request.
            .pool_max_idle_per_host(0)
            .tcp_keepalive(Duration::from_secs(15))
            .build()
            .unwrap_or_else(|_| reqwest::Client::new())
    })
}

pub fn download_urls(homeserver: &str, server: &str, media_id: &str) -> [String; 2] {
    let hs = homeserver.trim_end_matches('/');
    [
        format!("{hs}/_matrix/client/v1/media/download/{server}/{media_id}"),
        format!("{hs}/_matrix/media/v3/download/{server}/{media_id}"),
    ]
}

/// Open a streaming GET of the whole file at `mxc`. Matrix homeservers ignore
/// `Range` (they return the full file with `200`), so the proxy always fetches
/// the whole file and streams it chunk-by-chunk via `Response::chunk()`; the
/// on-disk cache (see `cache.rs`) provides Range/seek support to the local player.
///
/// Returns the total length from `Content-Length` (`None` if the server sent none
/// or reported 0 — a chunked media response, which can't back a Range/seek proxy)
/// and the response for chunked reading. For encrypted attachments the bytes are
/// ciphertext. Tries authenticated v1 then legacy v3.
pub async fn fetch_open(client: &Client, mxc: &str) -> Result<(Option<u64>, reqwest::Response)> {
    let rest = mxc
        .strip_prefix("mxc://")
        .ok_or_else(|| anyhow!("not an mxc url"))?;
    let (server, media_id) = rest
        .split_once('/')
        .ok_or_else(|| anyhow!("malformed mxc"))?;
    let urls = download_urls(client.homeserver().as_str(), server, media_id);
    let token = client
        .matrix_auth()
        .session()
        .map(|s| s.tokens.access_token.clone());
    // A stalled or momentarily-unavailable homeserver shouldn't poison the whole
    // stream: the shared client bounds how long a connect / body read may hang, and
    // we retry the open a few times before giving up.
    let http = http_client();

    let mut last_err = None;
    for attempt in 0..OPEN_ATTEMPTS {
        if attempt > 0 {
            tokio::time::sleep(OPEN_BACKOFF * attempt).await;
        }
        let mut retryable = false;
        for url in &urls {
            let mut req = http.get(url);
            if let Some(t) = token.as_ref() {
                req = req.header("Authorization", format!("Bearer {t}"));
            }
            match req.send().await {
                Ok(resp) if resp.status().is_success() => {
                    // Treat a missing or zero Content-Length as "unknown" — the
                    // cache layer refuses to stream it (chunked responses can't be
                    // ranged), falling back to the full-download path.
                    let total = resp.content_length().filter(|t| *t > 0);
                    return Ok((total, resp));
                }
                Ok(resp) => {
                    let status = resp.status();
                    // 5xx / 429 may clear on a retry; a 4xx is a permanent answer.
                    if status.is_server_error() || status == reqwest::StatusCode::TOO_MANY_REQUESTS
                    {
                        retryable = true;
                    }
                    last_err = Some(anyhow!("upstream {url} -> HTTP {status}"));
                }
                Err(e) => {
                    retryable = true; // connect/read timeout or network error
                    last_err = Some(anyhow!("upstream {url} failed: {e}"));
                }
            }
        }
        if !retryable {
            break; // every endpoint gave a permanent error — don't burn retries
        }
    }
    Err(last_err.unwrap_or_else(|| anyhow!("all media endpoints failed")))
}

/// Fetch up to `max_bytes` of the (cipher)text at `mxc` for thumbnail first-frame
/// extraction, reading only that prefix even when the homeserver ignores `Range`
/// and replies `200` with the whole file (read `max_bytes`, then drop the
/// connection so only ~that much transfers). Returns ciphertext for encrypted media.
pub async fn fetch_prefix(client: &Client, mxc: &str, max_bytes: u64) -> Result<Vec<u8>> {
    let (_total, mut resp) = fetch_open(client, mxc).await?;
    let cap = max_bytes as usize;
    let mut data: Vec<u8> = Vec::new();
    while let Some(chunk) = resp.chunk().await? {
        data.extend_from_slice(&chunk);
        if data.len() >= cap {
            data.truncate(cap);
            break;
        }
    }
    Ok(data)
}

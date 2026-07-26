// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! One-click auto-update: fetch the release manifest, compare versions, and
//! download the platform asset with the signature verified before it is handed
//! to C++ to apply.
//!
//! **App-global and handle-free.** The updater is deliberately not tied to a
//! per-account `Handle`: with up to six accounts there is no canonical handle,
//! `tm_destroy` only drains its own tasks, and the updater has to work before
//! anyone has logged in. It therefore owns a small private tokio runtime.
//!
//! **Trust model.** TLS to GitHub is not enough — a manifest is just JSON and an
//! attacker who can serve one could otherwise point the app at any payload. The
//! only thing actually trusted is the ed25519/minisign signature over the asset,
//! checked against [`UPDATE_PUBLIC_KEY`] which is compiled into the binary. Two
//! properties matter and are both enforced in [`download_and_verify`]:
//!
//! 1. The signature covers the asset bytes, so the payload cannot be swapped.
//! 2. The **trusted comment** — which is itself signature-covered — carries
//!    `version=X.Y.Z`, which must equal the manifest version *and* be strictly
//!    newer than what is running. Without this an attacker could replay an old,
//!    validly-signed asset under a high manifest version and force a downgrade
//!    onto a known-vulnerable build. minisign's *untrusted* comment (`-c`) is
//!    freely rewritable and is never consulted.
//!
//! **Known limitation — freeze attacks.** The version binding proves "newer than
//! what is running", not "the newest release that exists". Someone who can edit
//! the published manifest *without* holding the signing key can still pin
//! clients to a genuinely-signed but stale release: point 1.0.0 users at the
//! real 1.2.0 assets forever and they never learn 1.5.0 shipped. Closing that
//! needs freshness metadata with its own expiry (TUF's timestamp role), which
//! this scheme deliberately does not carry. A full downgrade *is* blocked; only
//! withholding newer releases is possible, and only from an attacker who has
//! already compromised the release pipeline.

use anyhow::{anyhow, bail, Context, Result};
use minisign_verify::{PublicKey, Signature};
use semver::Version;
use serde::Deserialize;
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::OnceLock;
use std::time::Duration;
use tokio::io::AsyncWriteExt;
use tokio::runtime::Runtime;
use tracing::{info, warn};

/// Minisign public key every update asset is verified against.
///
/// **Empty until the one-time keygen** (`minisign -G`; see
/// `docs/autoupdate-plan.md` → CI). Paste the *second* line of `minisign.pub`
/// here — the base64 blob, not the `untrusted comment:` line. The matching
/// secret key belongs in the `MINISIGN_SECRET_KEY` GitHub secret and nowhere
/// else, least of all this repository.
///
/// While this is empty the updater still *checks* for new versions but refuses
/// to download anything ([`signing_configured`] is false, and C++ degrades to
/// notify-only). That is deliberate: an unconfigured build can never install an
/// unverified payload, it can only fail loudly.
const UPDATE_PUBLIC_KEY: &str = "";

/// Hosts the updater may talk to, checked on the initial URL *and* on every
/// redirect hop. Everything else is refused, which stops a swapped manifest from
/// turning the app into an arbitrary-URL downloader.
fn host_allowed(host: Option<&str>) -> bool {
    let Some(host) = host else {
        return false;
    };
    let host = host.to_ascii_lowercase();
    // Narrow to the hosts that actually serve releases. A blanket
    // `*.githubusercontent.com` would also admit raw./gist./camo., i.e. any
    // GitHub user's content, handing a swapped manifest a general fetch
    // primitive. `release-assets.` is the current redirect target and
    // `objects.` the previous one; both are kept so a rollback on GitHub's side
    // doesn't break downloads.
    host == "github.com"
        || host == "www.github.com"
        || host == "objects.githubusercontent.com"
        || host == "release-assets.githubusercontent.com"
}

/// Only https. The signature governs authenticity either way, but a plaintext
/// hop leaks which version a user runs and lets a network attacker withhold
/// updates silently.
fn url_allowed(url: &reqwest::Url) -> bool {
    url.scheme().eq_ignore_ascii_case("https") && host_allowed(url.host_str())
}

/// A manifest is a few hundred bytes; anything approaching this is hostile.
const MANIFEST_MAX_BYTES: u64 = 1024 * 1024;
/// Refuse absurd assets outright — installers are ~100-200 MB.
const DOWNLOAD_MAX_BYTES: u64 = 512 * 1024 * 1024;
/// Allowance over the manifest's declared size before we abort a download. The
/// signature only verifies once the file is complete, so without a running cap
/// a hostile manifest could fill the disk first.
const DOWNLOAD_SLACK_BYTES: u64 = 1024 * 1024;

const CONNECT_TIMEOUT: Duration = Duration::from_secs(15);
/// Idle gap between body chunks, not a whole-request deadline — a slow but live
/// download must not be killed.
const READ_TIMEOUT: Duration = Duration::from_secs(60);
const MANIFEST_TIMEOUT: Duration = Duration::from_secs(20);

/// True once a real public key has been compiled in. C++ uses this to decide
/// whether to offer "Download" or fall back to opening the release page.
pub fn signing_configured() -> bool {
    !UPDATE_PUBLIC_KEY.is_empty()
}

/// Private single-threaded runtime. The updater is idle almost always, so it
/// gets one worker rather than a share of an account runtime — and it must
/// outlive every `Handle`, so it is never shut down.
fn runtime() -> Option<&'static Runtime> {
    static RT: OnceLock<Option<Runtime>> = OnceLock::new();
    RT.get_or_init(|| {
        match tokio::runtime::Builder::new_multi_thread()
            .worker_threads(1)
            .max_blocking_threads(2)
            .thread_name("tm-updater")
            .enable_all()
            .build()
        {
            Ok(rt) => Some(rt),
            Err(e) => {
                warn!("[UPDATE] runtime init failed: {e}");
                None
            }
        }
    })
    .as_ref()
}

/// Run `fut` on the updater runtime. Returns false if the runtime is unavailable
/// (in which case the caller must still deliver a terminal callback itself).
pub fn spawn<F>(fut: F) -> bool
where
    F: std::future::Future<Output = ()> + Send + 'static,
{
    match runtime() {
        Some(rt) => {
            rt.spawn(fut);
            true
        }
        None => false,
    }
}

/// The hardened client, or an error. Deliberately NOT falling back to
/// `Client::new()`: that would silently drop the redirect allowlist, the
/// timeouts and https-only — every protection this module relies on — and the
/// updater would keep working, quietly unhardened.
fn http_client() -> Result<&'static reqwest::Client> {
    static CLIENT: OnceLock<Option<reqwest::Client>> = OnceLock::new();
    CLIENT
        .get_or_init(|| {
            let redirect = reqwest::redirect::Policy::custom(|attempt| {
                if attempt.previous().len() >= 10 {
                    attempt.error("too many redirects")
                } else if url_allowed(attempt.url()) {
                    attempt.follow()
                } else {
                    attempt.error("redirect to a host the updater does not trust")
                }
            });
            reqwest::Client::builder()
                .user_agent("TeleMatrix-Updater")
                .connect_timeout(CONNECT_TIMEOUT)
                .read_timeout(READ_TIMEOUT)
                .https_only(true)
                .redirect(redirect)
                .build()
                .map_err(|e| warn!("[UPDATE] http client init failed: {e}"))
                .ok()
        })
        .as_ref()
        .ok_or_else(|| anyhow!("the updater could not initialise a secure HTTP client"))
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Deserialize)]
pub struct PlatformAsset {
    pub url: String,
    #[serde(default)]
    pub size: u64,
    #[serde(default)]
    pub sha256: String,
    /// Full `.minisig` file contents, inlined by CI.
    #[serde(default)]
    pub minisig: String,
}

#[derive(Debug, Clone, Deserialize)]
pub struct Manifest {
    pub version: String,
    #[serde(default)]
    pub page: String,
    #[serde(default)]
    pub platforms: HashMap<String, PlatformAsset>,
}

/// What a check concluded. `Available { asset: None }` means a newer release
/// exists but this platform has no updater asset in it (or the key is unknown),
/// so the UI degrades to notify-only and links `page`.
#[derive(Debug, Clone)]
pub enum CheckOutcome {
    UpToDate,
    Available {
        version: String,
        page: String,
        asset: Option<PlatformAsset>,
    },
}

pub fn parse_manifest(body: &str) -> Result<Manifest> {
    let manifest: Manifest =
        serde_json::from_str(body).context("update manifest is not valid JSON")?;
    if manifest.version.trim().is_empty() {
        bail!("update manifest has no version");
    }
    Ok(manifest)
}

/// Strictly-newer comparison. Both sides tolerate a leading `v`. Pre-releases
/// order below their release (`1.1.0-beta.1` < `1.1.0`), which is what we want:
/// a beta must not shadow the final.
pub fn is_newer(current: &str, candidate: &str) -> Result<bool> {
    let parse = |s: &str, what: &str| -> Result<Version> {
        Version::parse(s.trim().trim_start_matches('v'))
            .with_context(|| format!("unparseable {what} version {s:?}"))
    };
    Ok(parse(candidate, "manifest")? > parse(current, "running")?)
}

pub fn evaluate(
    manifest: Manifest,
    current_version: &str,
    platform_key: &str,
) -> Result<CheckOutcome> {
    if !is_newer(current_version, &manifest.version)? {
        return Ok(CheckOutcome::UpToDate);
    }
    // A missing/unknown platform key is not an error — it degrades to
    // notify-only, which is also how deb/rpm and the publish window behave.
    let asset = manifest
        .platforms
        .get(platform_key)
        .filter(|a| !a.url.is_empty() && !a.minisig.is_empty())
        .cloned();
    Ok(CheckOutcome::Available {
        version: manifest.version,
        page: manifest.page,
        asset,
    })
}

pub async fn check(
    current_version: &str,
    manifest_url: &str,
    platform_key: &str,
) -> Result<CheckOutcome> {
    let url = reqwest::Url::parse(manifest_url).context("bad manifest URL")?;
    if !url_allowed(&url) {
        bail!("manifest URL is not an allowed https GitHub address");
    }
    let mut response = http_client()?
        .get(url)
        .timeout(MANIFEST_TIMEOUT)
        .send()
        .await
        .context("manifest request failed")?;

    // The publish window makes "latest" briefly asset-less, and a repo with no
    // release at all 404s. Neither is an error the user should see.
    if response.status() == reqwest::StatusCode::NOT_FOUND {
        info!("[UPDATE] manifest not published yet (404)");
        return Ok(CheckOutcome::UpToDate);
    }
    if !response.status().is_success() {
        bail!("manifest request returned {}", response.status());
    }
    if response.content_length().unwrap_or(0) > MANIFEST_MAX_BYTES {
        bail!("update manifest is implausibly large");
    }
    // Streamed with a running cap rather than `.text()`. A chunked response
    // carries no Content-Length, so the check above sees None and passes — and
    // `.text()` would then buffer the whole body before any length check could
    // reject it, bounded only by the request timeout. Same defence the asset
    // download already uses.
    let mut body = Vec::new();
    while let Some(chunk) = response.chunk().await.context("manifest body unreadable")? {
        if body.len() as u64 + chunk.len() as u64 > MANIFEST_MAX_BYTES {
            bail!("update manifest is implausibly large");
        }
        body.extend_from_slice(&chunk);
    }
    let body = String::from_utf8(body).context("update manifest is not valid UTF-8")?;
    evaluate(parse_manifest(&body)?, current_version, platform_key)
}

// ---------------------------------------------------------------------------
// Download + verify
// ---------------------------------------------------------------------------

/// One download at a time; a second request is rejected rather than queued.
static DOWNLOAD_ACTIVE: AtomicBool = AtomicBool::new(false);
static CANCEL_REQUESTED: AtomicBool = AtomicBool::new(false);

/// Claims the single download slot. The returned guard releases it on drop, so
/// an early `?` can't strand the updater in a permanently-busy state.
pub struct DownloadSlot;

impl DownloadSlot {
    pub fn acquire() -> Option<Self> {
        if DOWNLOAD_ACTIVE.swap(true, Ordering::AcqRel) {
            return None;
        }
        CANCEL_REQUESTED.store(false, Ordering::Release);
        Some(Self)
    }
}

impl Drop for DownloadSlot {
    fn drop(&mut self) {
        DOWNLOAD_ACTIVE.store(false, Ordering::Release);
    }
}

pub fn request_cancel() {
    CANCEL_REQUESTED.store(true, Ordering::Release);
}

fn cancelled() -> bool {
    CANCEL_REQUESTED.load(Ordering::Acquire)
}

/// Derive a local filename from the asset URL without letting the manifest pick
/// the path: only the last segment is used, and everything outside a safe
/// character set is dropped — so `..`, separators and query strings cannot
/// escape the updates directory.
fn safe_asset_name(url: &str) -> String {
    let last = url.rsplit('/').next().unwrap_or_default();
    let last = last.split(['?', '#']).next().unwrap_or_default();
    let cleaned: String = last
        .chars()
        .filter(|c| c.is_ascii_alphanumeric() || matches!(c, '.' | '-' | '_'))
        .collect();
    if cleaned.is_empty() || cleaned.chars().all(|c| c == '.') {
        "telematrix-update".to_string()
    } else {
        cleaned
    }
}

fn bytes_to_hex(bytes: &[u8]) -> String {
    let mut hex = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        use std::fmt::Write as _;
        let _ = write!(&mut hex, "{byte:02x}");
    }
    hex
}

/// Pull `version=` out of a minisign trusted comment. Tokenised rather than
/// matched whole so an added timestamp field wouldn't break the binding.
pub fn trusted_comment_version(comment: &str) -> Option<&str> {
    comment
        .split_whitespace()
        .find_map(|token| token.strip_prefix("version="))
}

/// Check the signed version binding: it must name exactly the version the
/// manifest advertised, and that version must still be newer than what is
/// running. The second half is what makes a replayed old-but-valid asset
/// useless to an attacker.
pub fn check_version_binding(
    trusted_comment: &str,
    manifest_version: &str,
    current_version: &str,
) -> Result<()> {
    let signed = trusted_comment_version(trusted_comment).ok_or_else(|| {
        anyhow!("signature carries no version= in its trusted comment (was it signed with -c instead of -t?)")
    })?;
    let normalize = |s: &str| s.trim().trim_start_matches('v').to_string();
    if normalize(signed) != normalize(manifest_version) {
        bail!("signed version {signed:?} does not match manifest version {manifest_version:?}");
    }
    if !is_newer(current_version, signed)? {
        bail!(
            "refusing downgrade: signed version {signed:?} is not newer than {current_version:?}"
        );
    }
    Ok(())
}

/// Re-verify an already-downloaded file against the same signature and version
/// binding the download enforced.
///
/// The download leaves its payload at a predictable path under the user's cache
/// directory, and under the auto-download policy it can sit there for hours
/// before the user clicks "Update & Restart". Anything running as the same user
/// can swap it in that window — and on Windows the swapped installer would then
/// be run through a UAC prompt the user has already been primed to accept. So
/// the file is verified again immediately before it is used, closing the gap
/// between "we checked it" and "we ran it".
///
/// Synchronous and blocking: it hashes up to a few hundred MB, but it runs on a
/// deliberate click at the moment the app is about to exit anyway.
pub fn verify_file(
    path: &Path,
    expected_sha256: &str,
    minisig: &str,
    manifest_version: &str,
    current_version: &str,
) -> Result<()> {
    use std::io::Read as _;

    if !signing_configured() {
        bail!("this build has no update signing key compiled in");
    }
    let public_key = PublicKey::from_base64(UPDATE_PUBLIC_KEY)
        .context("embedded update public key is invalid")?;
    let signature = Signature::decode(minisig).context("update signature is malformed")?;
    let mut verifier = public_key
        .verify_stream(&signature)
        .context("update signature is not in the expected prehashed format")?;

    let mut file = std::fs::File::open(path).context("the downloaded update is unreadable")?;
    let mut hasher = Sha256::new();
    let mut buffer = vec![0u8; 1024 * 1024];
    let mut total: u64 = 0;
    loop {
        let read = file
            .read(&mut buffer)
            .context("the downloaded update is unreadable")?;
        if read == 0 {
            break;
        }
        total = total.saturating_add(read as u64);
        if total > DOWNLOAD_MAX_BYTES {
            bail!("the downloaded update grew beyond the allowed size");
        }
        hasher.update(&buffer[..read]);
        verifier.update(&buffer[..read]);
    }

    verifier
        .finalize()
        .map_err(|e| anyhow!("the downloaded update no longer matches its signature: {e}"))?;
    check_version_binding(
        signature.trusted_comment(),
        manifest_version,
        current_version,
    )?;

    let actual = bytes_to_hex(&hasher.finalize());
    if !expected_sha256.is_empty() && !actual.eq_ignore_ascii_case(expected_sha256.trim()) {
        bail!("the downloaded update no longer matches its checksum");
    }
    Ok(())
}

/// Download `url`, verifying as the bytes arrive, and return the final path.
///
/// sha256 and the minisign hash are computed in the same streaming pass, so a
/// 200 MB installer is never held in memory and never read back from disk. The
/// file only gets its real name once every check has passed — a `.part` left by
/// a crash or a cancel is therefore never mistakable for a verified payload.
#[allow(clippy::too_many_arguments)]
pub async fn download_and_verify(
    url: &str,
    expected_size: u64,
    expected_sha256: &str,
    minisig: &str,
    manifest_version: &str,
    current_version: &str,
    cache_dir: &Path,
    progress: impl Fn(u64, u64) + Send + 'static,
) -> Result<PathBuf> {
    if !signing_configured() {
        bail!("this build has no update signing key compiled in");
    }
    // Parse the key and signature before spending any bandwidth.
    let public_key = PublicKey::from_base64(UPDATE_PUBLIC_KEY)
        .context("embedded update public key is invalid")?;
    let signature = Signature::decode(minisig).context("update signature is malformed")?;
    // Streaming verification requires a prehashed signature, so a legacy
    // (non-prehashed) one is refused outright rather than silently accepted.
    let mut verifier = public_key
        .verify_stream(&signature)
        .context("update signature is not in the expected prehashed format")?;

    let parsed = reqwest::Url::parse(url).context("bad asset URL")?;
    if !url_allowed(&parsed) {
        bail!("asset URL is not an allowed https GitHub address");
    }
    if expected_size > DOWNLOAD_MAX_BYTES {
        bail!("update asset is implausibly large");
    }

    // The directory below is deleted recursively, so refuse anything that isn't
    // an absolute path. An empty cache_dir slipping through from C++ would
    // otherwise make this `remove_dir_all("updates")` relative to the working
    // directory.
    if !cache_dir.is_absolute() {
        bail!("update cache directory is not an absolute path");
    }

    // Wipe the whole updates directory rather than hunting for stale `.part`
    // files: nothing in it survives a restart anyway (readiness is in-memory),
    // so the only correct content is the download about to start.
    let dir = cache_dir.join("updates");
    let _ = tokio::fs::remove_dir_all(&dir).await;
    tokio::fs::create_dir_all(&dir)
        .await
        .context("cannot create the updates cache directory")?;

    let name = safe_asset_name(url);
    let final_path = dir.join(&name);
    let part_path = dir.join(format!("{name}.part"));

    let cap = expected_size.saturating_add(DOWNLOAD_SLACK_BYTES);
    let mut response = http_client()?
        .get(parsed)
        .send()
        .await
        .context("update download failed to start")?;
    if !response.status().is_success() {
        bail!("update download returned {}", response.status());
    }
    let total = response.content_length().unwrap_or(expected_size);

    let result = async {
        let mut file = tokio::fs::File::create(&part_path)
            .await
            .context("cannot open the update file for writing")?;
        let mut hasher = Sha256::new();
        let mut received: u64 = 0;

        while let Some(chunk) = response
            .chunk()
            .await
            .context("update download interrupted")?
        {
            if cancelled() {
                bail!("cancelled");
            }
            received = received.saturating_add(chunk.len() as u64);
            if received > cap {
                bail!("update asset is larger than the manifest declared");
            }
            file.write_all(&chunk)
                .await
                .context("cannot write the update file")?;
            hasher.update(&chunk);
            verifier.update(&chunk);
            progress(received, total);
        }
        file.flush().await.context("cannot flush the update file")?;
        drop(file);

        if expected_size != 0 && received != expected_size {
            bail!("update asset size {received} does not match the manifest");
        }
        // Signature first: it is the only real authority here. The sha256 is a
        // cheap cross-check that also lets SHA256SUMS users verify by hand.
        verifier
            .finalize()
            .map_err(|e| anyhow!("update signature verification failed: {e}"))?;
        check_version_binding(
            signature.trusted_comment(),
            manifest_version,
            current_version,
        )?;

        let actual = bytes_to_hex(&hasher.finalize());
        if !expected_sha256.is_empty() && !actual.eq_ignore_ascii_case(expected_sha256.trim()) {
            bail!("update asset sha256 does not match the manifest");
        }
        Ok(())
    }
    .await;

    if let Err(e) = result {
        let _ = tokio::fs::remove_file(&part_path).await;
        return Err(e);
    }

    tokio::fs::rename(&part_path, &final_path)
        .await
        .context("cannot finalize the downloaded update")?;
    info!("[UPDATE] verified {} ({} bytes)", name, expected_size);
    Ok(final_path)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    // --- host allowlist ---------------------------------------------------

    #[test]
    fn allows_only_github_release_hosts() {
        assert!(host_allowed(Some("github.com")));
        assert!(host_allowed(Some("GitHub.com"))); // case-insensitive
        assert!(host_allowed(Some("objects.githubusercontent.com")));
        // GitHub renamed the asset host once already; both are accepted.
        assert!(host_allowed(Some("release-assets.githubusercontent.com")));

        assert!(!host_allowed(None));
        assert!(!host_allowed(Some("evil.com")));
        // The classic suffix-match bug: a lookalike registered domain.
        assert!(!host_allowed(Some("notgithub.com")));
        assert!(!host_allowed(Some("github.com.evil.com")));
        assert!(!host_allowed(Some("githubusercontent.com.evil.com")));
        // Other githubusercontent subdomains host arbitrary user content, so a
        // swapped manifest must not be able to point us at them.
        assert!(!host_allowed(Some("raw.githubusercontent.com")));
        assert!(!host_allowed(Some("gist.githubusercontent.com")));
        assert!(!host_allowed(Some("camo.githubusercontent.com")));
    }

    #[test]
    fn requires_https() {
        let allowed = |s: &str| url_allowed(&reqwest::Url::parse(s).unwrap());
        assert!(allowed(
            "https://github.com/x/y/releases/latest/download/latest.json"
        ));
        // Plaintext leaks the running version and lets a network attacker
        // silently withhold updates, even though the signature still governs.
        assert!(!allowed("http://github.com/x/y"));
        assert!(!allowed("ftp://github.com/x/y"));
        assert!(!allowed("file:///etc/passwd"));
        assert!(!allowed("https://evil.com/x"));
    }

    // --- version comparison ----------------------------------------------

    #[test]
    fn compares_versions_including_prereleases() {
        assert!(is_newer("1.0.0", "1.0.1").unwrap());
        assert!(is_newer("1.0.0", "v1.1.0").unwrap());
        assert!(!is_newer("1.0.0", "1.0.0").unwrap());
        assert!(!is_newer("1.1.0", "1.0.0").unwrap());
        // A pre-release must not shadow the final release it precedes.
        assert!(!is_newer("1.1.0", "1.1.0-beta.1").unwrap());
        assert!(is_newer("1.1.0-beta.1", "1.1.0").unwrap());
        assert!(is_newer("0.9.0", "1.0.0-rc.1").unwrap());
        assert!(is_newer("1.0.0", "1.0.1").unwrap());
        assert!(is_newer("1.0.0-beta", "1.0.0-beta.2").unwrap());
    }

    #[test]
    fn rejects_unparseable_versions() {
        assert!(is_newer("1.0.0", "not-a-version").is_err());
        assert!(is_newer("", "1.0.0").is_err());
    }

    // --- manifest ---------------------------------------------------------

    fn manifest_json(version: &str, key: &str) -> String {
        format!(
            r#"{{
              "version": "{version}",
              "pub_date": "2026-07-25",
              "page": "https://github.com/gecka/telematrix/releases/tag/v{version}",
              "platforms": {{
                "{key}": {{
                  "url": "https://github.com/gecka/telematrix/releases/download/v{version}/app.tar.gz",
                  "size": 1234,
                  "sha256": "abc",
                  "minisig": "untrusted comment: x\nRWQsig\ntrusted comment: version={version}\nRWQglobal\n"
                }}
              }}
            }}"#
        )
    }

    #[test]
    fn parses_a_well_formed_manifest() {
        let m = parse_manifest(&manifest_json("1.1.0", "macos-aarch64")).unwrap();
        assert_eq!(m.version, "1.1.0");
        assert!(m.platforms.contains_key("macos-aarch64"));
        assert_eq!(m.platforms["macos-aarch64"].size, 1234);
    }

    /// Byte-for-byte output of the `Build update manifest` step in
    /// `.github/workflows/release.yml`, captured from a real run of that script.
    ///
    /// This is the contract between CI and the client: the two halves are
    /// written in different languages, in different files, and only meet during
    /// a release. If someone renames a manifest field on either side, this test
    /// is what fails — instead of every user's update check.
    const CI_MANIFEST: &str = r#"{
  "version": "1.1.0",
  "pub_date": "2026-07-25T10:00:00Z",
  "page": "https://github.com/gecka/telematrix/releases/tag/v1.1.0",
  "platforms": {
    "macos-aarch64": {
      "url": "https://github.com/gecka/telematrix/releases/download/v1.1.0/TeleMatrix-1.1.0-macos-aarch64-app.tar.gz",
      "size": 53,
      "sha256": "9b5b1c9c443aa8d66c4cd4b02c4437c0fa24c8106e77ef5309bb0eddc0ae5e02",
      "minisig": "untrusted comment: sig\nRWQfakesig\ntrusted comment: version=1.1.0\nRWQfakeglobal\n"
    },
    "windows-x86_64": {
      "url": "https://github.com/gecka/telematrix/releases/download/v1.1.0/TeleMatrix-1.1.0-win64.exe",
      "size": 38,
      "sha256": "60483630d262cf5073fea7521aec895b19a90d5bd8c0e227b1e8f40c44d60d0e",
      "minisig": "untrusted comment: sig\nRWQfakesig\ntrusted comment: version=1.1.0\nRWQfakeglobal\n"
    },
    "linux-appimage-x86_64": {
      "url": "https://github.com/gecka/telematrix/releases/download/v1.1.0/TeleMatrix-1.1.0-x86_64.AppImage",
      "size": 44,
      "sha256": "57977686c96f688047e085a24a79e63776f394f6c4277c56e86a9c17f3d2e04a",
      "minisig": "untrusted comment: sig\nRWQfakesig\ntrusted comment: version=1.1.0\nRWQfakeglobal\n"
    }
  }
}"#;

    #[test]
    fn parses_the_manifest_ci_actually_produces() {
        let manifest = parse_manifest(CI_MANIFEST).expect("CI manifest must parse");
        assert_eq!(manifest.version, "1.1.0");
        assert!(manifest.page.ends_with("/releases/tag/v1.1.0"));

        // Every platform key the release workflow emits must resolve here.
        for key in ["macos-aarch64", "windows-x86_64", "linux-appimage-x86_64"] {
            let outcome = evaluate(parse_manifest(CI_MANIFEST).unwrap(), "1.0.0", key).unwrap();
            let CheckOutcome::Available { asset, .. } = outcome else {
                panic!("{key}: expected an update to be available");
            };
            let asset = asset.unwrap_or_else(|| panic!("{key} resolved no asset"));
            assert!(asset.size > 0, "{key}: size missing");
            assert_eq!(asset.sha256.len(), 64, "{key}: sha256 is not a hex digest");
            assert!(
                asset.url.starts_with("https://github.com/"),
                "{key}: url must be a GitHub release asset"
            );
            assert!(
                host_allowed(reqwest::Url::parse(&asset.url).unwrap().host_str()),
                "{key}: url host would be refused by the downloader"
            );
            // The inlined .minisig must be a whole 4-line minisign file, not
            // just the base64 signature line.
            assert_eq!(asset.minisig.lines().count(), 4, "{key}: truncated minisig");
            assert_eq!(
                trusted_comment_version(asset.minisig.lines().nth(2).unwrap()),
                Some("1.1.0"),
                "{key}: trusted comment lost its version binding"
            );
        }
    }

    #[test]
    fn rejects_junk_and_versionless_manifests() {
        assert!(parse_manifest("not json").is_err());
        assert!(parse_manifest(r#"{"page":"x"}"#).is_err());
        assert!(parse_manifest(r#"{"version":"  "}"#).is_err());
    }

    #[test]
    fn unknown_platform_key_degrades_to_notify_only() {
        let m = parse_manifest(&manifest_json("1.1.0", "macos-aarch64")).unwrap();
        let outcome = evaluate(m, "1.0.0", "linux-appimage-x86_64").unwrap();
        match outcome {
            CheckOutcome::Available { asset, version, .. } => {
                assert_eq!(version, "1.1.0");
                assert!(asset.is_none(), "unknown key must not resolve an asset");
            }
            other => panic!("expected Available, got {other:?}"),
        }
    }

    #[test]
    fn same_or_older_manifest_is_up_to_date() {
        let m = parse_manifest(&manifest_json("1.0.0", "macos-aarch64")).unwrap();
        assert!(matches!(
            evaluate(m, "1.0.0", "macos-aarch64").unwrap(),
            CheckOutcome::UpToDate
        ));
        let m = parse_manifest(&manifest_json("0.9.0", "macos-aarch64")).unwrap();
        assert!(matches!(
            evaluate(m, "1.0.0", "macos-aarch64").unwrap(),
            CheckOutcome::UpToDate
        ));
    }

    // --- asset name sanitising -------------------------------------------

    #[test]
    fn asset_name_keeps_ordinary_filenames() {
        assert_eq!(
            safe_asset_name("https://x/TeleMatrix-1.1.0.AppImage"),
            "TeleMatrix-1.1.0.AppImage"
        );
        assert_eq!(
            safe_asset_name("https://x/a/b/win64.exe?token=1"),
            "win64.exe"
        );
        assert_eq!(safe_asset_name("https://x/app.tar.gz#frag"), "app.tar.gz");
    }

    #[test]
    fn asset_name_cannot_escape_the_updates_directory() {
        // The invariant that matters: whatever a hostile manifest names its
        // asset, the result stays one non-traversing path component. Asserting
        // the property rather than exact strings keeps this honest — e.g.
        // `a\..\b.exe` legitimately becomes `a..b.exe`, which embeds dots but
        // is still a single harmless component.
        for url in [
            "https://x/..",
            "https://x/.",
            "https://x/../../etc/passwd",
            "https://x/",
            "https://x/a%2F..%2Fb",
            r"https://x/a\..\b.exe",
            "https://x/we;ird$name.exe",
            "https://x/../../../../../../etc/shadow",
            "https://x/~/.ssh/authorized_keys",
        ] {
            let name = safe_asset_name(url);
            assert!(!name.is_empty(), "{url}");
            assert!(!name.contains('/'), "{url} -> {name}");
            assert!(!name.contains('\\'), "{url} -> {name}");
            assert_ne!(name, "..", "{url}");
            assert_ne!(name, ".", "{url}");
            assert_eq!(
                Path::new(&name).components().count(),
                1,
                "{url} -> {name} must be a single component"
            );
            // The decisive check: joining it cannot leave the base directory.
            let joined = Path::new("/base/updates").join(&name);
            assert!(joined.starts_with("/base/updates"), "{url} -> {joined:?}");
        }
        // Traversal-only names fall back rather than producing something odd.
        assert_eq!(safe_asset_name("https://x/.."), "telematrix-update");
        assert_eq!(safe_asset_name("https://x/"), "telematrix-update");
    }

    // --- trusted-comment version binding ---------------------------------

    #[test]
    fn extracts_the_signed_version() {
        assert_eq!(trusted_comment_version("version=1.2.3"), Some("1.2.3"));
        assert_eq!(
            trusted_comment_version("timestamp:170 file:x.tar.gz version=1.2.3"),
            Some("1.2.3")
        );
        assert_eq!(trusted_comment_version("timestamp:170 file:x.tar.gz"), None);
    }

    #[test]
    fn version_binding_accepts_a_matching_newer_version() {
        assert!(check_version_binding("version=1.1.0", "1.1.0", "1.0.0").is_ok());
        assert!(check_version_binding("version=v1.1.0", "1.1.0", "1.0.0").is_ok());
    }

    #[test]
    fn version_binding_rejects_a_manifest_asset_mismatch() {
        // The attack this exists for: an old but validly-signed asset paired
        // with a freshly-written high manifest version.
        let err = check_version_binding("version=1.0.1", "9.9.9", "1.0.0").unwrap_err();
        assert!(err.to_string().contains("does not match manifest"), "{err}");
    }

    #[test]
    fn version_binding_rejects_a_downgrade() {
        let err = check_version_binding("version=0.9.0", "0.9.0", "1.0.0").unwrap_err();
        assert!(err.to_string().contains("refusing downgrade"), "{err}");
    }

    #[test]
    fn version_binding_rejects_a_signature_with_no_version() {
        // A signature made with `-c` (untrusted comment) instead of `-t` lands
        // here: the trusted comment keeps minisign's default and carries no
        // version=, so the binding is absent and the update is refused.
        let err = check_version_binding(
            "timestamp:1700000000 file:TeleMatrix.tar.gz",
            "1.1.0",
            "1.0.0",
        )
        .unwrap_err();
        assert!(err.to_string().contains("no version="), "{err}");
    }

    // --- real signature verification --------------------------------------
    //
    // A throwaway keypair is minted per test, so `cargo test` needs no minisign
    // CLI and no key material is checked in.

    fn sign_fixture(data: &[u8], trusted_comment: &str) -> (String, String) {
        let keypair = minisign::KeyPair::generate_unencrypted_keypair().unwrap();
        let signature = minisign::sign(
            None,
            &keypair.sk,
            Cursor::new(data),
            Some(trusted_comment),
            None,
        )
        .unwrap();
        (keypair.pk.to_base64(), signature.into_string())
    }

    fn verify_fixture(pk_b64: &str, sig_text: &str, data: &[u8]) -> Result<String> {
        let public_key = PublicKey::from_base64(pk_b64)?;
        let signature = Signature::decode(sig_text)?;
        let mut verifier = public_key.verify_stream(&signature)?;
        verifier.update(data);
        verifier
            .finalize()
            .map_err(|e| anyhow!("signature verification failed: {e}"))?;
        Ok(signature.trusted_comment().to_string())
    }

    #[test]
    fn verifies_a_genuine_signature_and_reads_its_trusted_comment() {
        let data = b"pretend this is a 200MB installer";
        let (pk, sig) = sign_fixture(data, "version=1.1.0");
        let comment = verify_fixture(&pk, &sig, data).expect("genuine signature must verify");
        assert_eq!(trusted_comment_version(&comment), Some("1.1.0"));
        assert!(check_version_binding(&comment, "1.1.0", "1.0.0").is_ok());
    }

    #[test]
    fn refuses_a_tampered_payload() {
        let (pk, sig) = sign_fixture(b"original payload", "version=1.1.0");
        let mut tampered = b"original payload".to_vec();
        tampered[0] ^= 0x01; // flip a single bit
        assert!(
            verify_fixture(&pk, &sig, &tampered).is_err(),
            "a tampered payload must not verify"
        );
    }

    #[test]
    fn refuses_a_signature_from_a_different_key() {
        let data = b"payload";
        let (_pk, sig) = sign_fixture(data, "version=1.1.0");
        let (other_pk, _other_sig) = sign_fixture(data, "version=1.1.0");
        // Right payload, right comment, wrong signer.
        assert!(verify_fixture(&other_pk, &sig, data).is_err());
    }

    #[test]
    fn a_version_in_the_untrusted_comment_alone_does_not_bind() {
        // Signing with the version only in the *untrusted* comment (`-c`) leaves
        // the trusted comment at minisign's default. The bytes still verify —
        // that is exactly the trap — but the binding check must reject it.
        let data = b"payload";
        let keypair = minisign::KeyPair::generate_unencrypted_keypair().unwrap();
        let signature = minisign::sign(
            None,
            &keypair.sk,
            Cursor::new(data),
            None,                  // trusted comment: minisign's default
            Some("version=9.9.9"), // untrusted comment: attacker-writable
        )
        .unwrap();
        let comment = verify_fixture(&keypair.pk.to_base64(), &signature.into_string(), data)
            .expect("bytes themselves are validly signed");
        assert!(
            check_version_binding(&comment, "9.9.9", "1.0.0").is_err(),
            "a version that rides only in the untrusted comment must not bind"
        );
    }

    /// The pre-apply re-verification, exercised through the same helpers
    /// `verify_file` uses. (`verify_file` itself is bound to the compiled-in
    /// key, which is empty in this build — see `unconfigured_builds_cannot_verify`
    /// — so the logic is driven here with a throwaway keypair instead.)
    fn verify_file_with_key(
        pk_b64: &str,
        sig_text: &str,
        path: &Path,
        expected_sha256: &str,
        manifest_version: &str,
        current_version: &str,
    ) -> Result<()> {
        use std::io::Read as _;
        let public_key = PublicKey::from_base64(pk_b64)?;
        let signature = Signature::decode(sig_text)?;
        let mut verifier = public_key.verify_stream(&signature)?;
        let mut file = std::fs::File::open(path)?;
        let mut hasher = Sha256::new();
        let mut buf = vec![0u8; 4096];
        loop {
            let n = file.read(&mut buf)?;
            if n == 0 {
                break;
            }
            hasher.update(&buf[..n]);
            verifier.update(&buf[..n]);
        }
        verifier
            .finalize()
            .map_err(|e| anyhow!("signature check failed: {e}"))?;
        check_version_binding(
            signature.trusted_comment(),
            manifest_version,
            current_version,
        )?;
        let actual = bytes_to_hex(&hasher.finalize());
        if !expected_sha256.is_empty() && !actual.eq_ignore_ascii_case(expected_sha256.trim()) {
            bail!("checksum mismatch");
        }
        Ok(())
    }

    #[test]
    fn pre_apply_verification_catches_a_swapped_payload() {
        let dir = std::env::temp_dir().join(format!("tm-update-verify-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("payload.bin");

        let payload = b"the genuine, signed installer";
        let (pk, sig) = sign_fixture(payload, "version=1.1.0");
        let sha = bytes_to_hex(&Sha256::digest(payload));
        std::fs::write(&path, payload).unwrap();

        // Untouched: passes.
        verify_file_with_key(&pk, &sig, &path, &sha, "1.1.0", "1.0.0")
            .expect("an untouched download must still verify");

        // Swapped between download and apply — the whole reason this exists.
        std::fs::write(&path, b"malicious replacement of the same length!").unwrap();
        let err = verify_file_with_key(&pk, &sig, &path, &sha, "1.1.0", "1.0.0")
            .expect_err("a swapped payload must be refused at apply time");
        assert!(err.to_string().contains("signature check failed"), "{err}");

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn unconfigured_builds_cannot_verify() {
        // Guards the shipped default: an empty key must read as unconfigured, so
        // the download path refuses rather than installing something unchecked.
        if UPDATE_PUBLIC_KEY.is_empty() {
            assert!(!signing_configured());
            assert!(PublicKey::from_base64(UPDATE_PUBLIC_KEY).is_err());
        } else {
            assert!(signing_configured());
            assert!(
                PublicKey::from_base64(UPDATE_PUBLIC_KEY).is_ok(),
                "UPDATE_PUBLIC_KEY is set but not a valid minisign key"
            );
        }
    }

    // --- download slot ----------------------------------------------------

    #[test]
    fn only_one_download_runs_at_a_time() {
        let first = DownloadSlot::acquire().expect("first acquire");
        assert!(DownloadSlot::acquire().is_none(), "second must be refused");
        drop(first);
        assert!(DownloadSlot::acquire().is_some(), "slot must free on drop");
    }
}

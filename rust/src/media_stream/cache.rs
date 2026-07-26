// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! On-disk progressive cache for the streaming proxy.
//!
//! Matrix homeservers don't honor `Range` (they return the whole file with `200`),
//! so the proxy can't fetch bounded chunks. Instead, the first request for an mxc
//! spawns ONE background download that appends the raw bytes (ciphertext for E2EE)
//! to a cache file, and player requests read from that file as it fills — waiting
//! when they reach the written end and the download isn't complete. All requests
//! for the same mxc share the one file, so seeks/replays never re-download.
//!
//! Completed downloads **persist across restarts**: a finished file `<hash>` gets a
//! sibling marker `<hash>.done`, so a later session reuses it without downloading.
//! A `<hash>` without a `.done` is a partial (crash/cancel leftover) and is
//! reclaimed at startup. Total cache size is capped by an LRU byte budget. The
//! whole dir is wiped on logout (`cleanup()`), along with the session data dir.

use std::collections::{HashMap, HashSet};
use std::hash::{Hash, Hasher};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::SystemTime;

use anyhow::{anyhow, Result};
use matrix_sdk::ruma::events::room::MediaSource;
use matrix_sdk::Client;
use tokio::fs::{File, OpenOptions};
use tokio::io::{AsyncReadExt, AsyncSeekExt, AsyncWriteExt};
use tokio::runtime::Handle;
use tokio::sync::{watch, Mutex};

use super::container::{self, Container};
use super::decrypt::{decrypt_ctr_range, key_iv_from_encrypted};
use super::upstream::{fetch_open, fetch_prefix};
use crate::container_store;

/// How much of the head to buffer while classifying the container. Real files
/// decide on the second top-level box (~40 bytes in); this only has to absorb a
/// leading `free` pad before `moov`/`mdat`.
const HEAD_PROBE_BYTES: usize = 8 * 1024;

// The persistent stream-cache dir is capped by an LRU byte budget derived from
// the user's `cacheSizeLimitMB` setting (cache_manager::stream_dir_budget_bytes);
// oldest complete files are evicted past it when a new download starts and at
// session start, freeing down to a 50% low watermark (hysteresis).

/// Per-file download progress, broadcast to readers via a `watch` channel (no
/// lost-wakeup race, and any number of concurrent readers).
#[derive(Clone)]
struct Progress {
    total: Option<u64>,
    written: u64,
    complete: bool,
    error: Option<String>,
}

/// One media file's on-disk progressive cache.
pub struct MediaEntry {
    path: PathBuf,
    pub source: MediaSource,
    progress: watch::Sender<Progress>,
    /// Set when a newer video opens while this one is still downloading: the
    /// download stops and the partial file is discarded (no-Range homeservers
    /// can't resume a partial anyway).
    cancel: AtomicBool,
}

impl MediaEntry {
    /// Wait until the total size is known (download headers received, or a
    /// completed file's size) or fail.
    pub async fn wait_total(&self) -> Result<u64> {
        let mut rx = self.progress.subscribe();
        loop {
            {
                let p = rx.borrow_and_update();
                if let Some(e) = &p.error {
                    return Err(anyhow!("{e}"));
                }
                if let Some(t) = p.total {
                    return Ok(t);
                }
            }
            if rx.changed().await.is_err() {
                return Err(anyhow!("media download ended before its size was known"));
            }
        }
    }

    /// Read up to `max` bytes starting at `offset`, waiting for the download to
    /// reach `offset` if needed. `Ok(None)` = clean EOF; `Err` = download failed.
    pub async fn read_chunk(&self, offset: u64, max: usize) -> Result<Option<Vec<u8>>> {
        let mut rx = self.progress.subscribe();
        loop {
            let (written, complete, error) = {
                let p = rx.borrow_and_update();
                (p.written, p.complete, p.error.clone())
            };
            if let Some(e) = error {
                return Err(anyhow!("{e}"));
            }
            if offset < written {
                let want = ((written - offset).min(max as u64)) as usize;
                let mut f = File::open(&self.path).await?;
                f.seek(std::io::SeekFrom::Start(offset)).await?;
                let mut buf = vec![0u8; want];
                f.read_exact(&mut buf).await?;
                return Ok(Some(buf));
            }
            if complete {
                return Ok(None);
            }
            if rx.changed().await.is_err() {
                return Ok(None);
            }
        }
    }
}

/// Classify the container from a (possibly ciphertext) head prefix. `None` means
/// the walk needs more bytes; `final_attempt` collapses that to `Unknown`.
fn classify_head(
    head: &[u8],
    key_iv: &Option<([u8; 32], [u8; 16])>,
    final_attempt: bool,
) -> Option<Container> {
    let mut plain = head.to_vec();
    if let Some((key, iv)) = key_iv {
        decrypt_ctr_range(key, iv, 0, &mut plain);
    }
    match container::classify(&plain) {
        container::Verdict::Decided(c) => Some(c),
        container::Verdict::NeedMore if final_attempt => Some(Container::Unknown),
        container::Verdict::NeedMore => None,
    }
}

/// Seek to `offset` and read exactly `want` bytes. Split out so `EntryReader` can
/// own the `File` across the call (take/restore) without a borrow conflict.
async fn read_exact_at(f: &mut File, offset: u64, want: usize) -> std::io::Result<Vec<u8>> {
    f.seek(std::io::SeekFrom::Start(offset)).await?;
    let mut buf = vec![0u8; want];
    f.read_exact(&mut buf).await?;
    Ok(buf)
}

/// A reader over one entry's progressive cache file that keeps the file handle
/// open across chunks. The old per-chunk `MediaEntry::read_chunk` reopened the
/// file for every 256 KiB (thousands of open/seek cycles for a long video); this
/// opens once and seeks thereafter, waiting on the progress channel when a read
/// reaches the currently-written end.
pub struct EntryReader {
    entry: Arc<MediaEntry>,
    file: Option<File>,
    rx: watch::Receiver<Progress>,
}

impl EntryReader {
    pub fn new(entry: Arc<MediaEntry>) -> Self {
        let rx = entry.progress.subscribe();
        EntryReader {
            entry,
            file: None,
            rx,
        }
    }

    /// Read up to `max` bytes at `offset`, waiting for the download to reach it.
    /// `Ok(None)` = clean EOF (download complete, nothing more at/after `offset`);
    /// `Err` = the download failed. The file handle is reused between calls; any
    /// read error drops it so a retry reopens (and an evicted/truncated file
    /// surfaces as a clean failure rather than stale bytes).
    pub async fn read_at(&mut self, offset: u64, max: usize) -> Result<Option<Vec<u8>>> {
        loop {
            let (written, complete, error) = {
                let p = self.rx.borrow_and_update();
                (p.written, p.complete, p.error.clone())
            };
            if let Some(e) = error {
                return Err(anyhow!("{e}"));
            }
            if offset < written {
                let want = ((written - offset).min(max as u64)) as usize;
                // Take the handle out (opening if first use) so the error branch can
                // drop it without a borrow conflict; restore it on success.
                let mut f = match self.file.take() {
                    Some(f) => f,
                    None => File::open(&self.entry.path).await?,
                };
                match read_exact_at(&mut f, offset, want).await {
                    Ok(buf) => {
                        self.file = Some(f);
                        return Ok(Some(buf));
                    }
                    // Drop f (don't restore): the next call reopens, so a transient
                    // error retries and an evicted/truncated file fails cleanly.
                    Err(e) => return Err(e.into()),
                }
            }
            if complete {
                return Ok(None);
            }
            if self.rx.changed().await.is_err() {
                return Ok(None);
            }
        }
    }
}

/// A directory of progressive cache files, one per mxc, with a single background
/// download per file and persistence of completed files across restarts.
pub struct MediaCache {
    dir: PathBuf,
    entries: Mutex<HashMap<String, Arc<MediaEntry>>>,
}

impl MediaCache {
    /// Open the cache directory, keeping completed files for reuse. Cheap: the
    /// startup reclaim + budget scan (which read_dir + stat + unlink the whole dir)
    /// is deferred to `reclaim_and_enforce`, because `new()` is reached on the UI
    /// thread via `block_on` in `video_stream_url` and must not do disk work there.
    pub fn new(dir: PathBuf) -> Self {
        let _ = std::fs::create_dir_all(&dir);
        MediaCache {
            dir,
            entries: Mutex::new(HashMap::new()),
        }
    }

    /// Reclaim partial/orphan leftovers and enforce the disk budget. Blocking
    /// std::fs — call from a `spawn_blocking` task (see `MediaStreamServer::start`),
    /// not on the UI thread.
    pub fn reclaim_and_enforce(&self) {
        reclaim_incomplete(&self.dir);
        evict_to_budget(&self.dir, &HashSet::new());
    }

    fn data_file(&self, mxc: &str) -> PathBuf {
        cache_data_path(&self.dir, mxc)
    }

    /// Get the cache entry for `mxc`: reuse a completed file from any session,
    /// otherwise start its download (first request only).
    pub async fn get_or_start(
        &self,
        mxc: &str,
        source: MediaSource,
        client: Client,
        runtime: Handle,
    ) -> Arc<MediaEntry> {
        let mut map = self.entries.lock().await;
        if let Some(e) = map.get(mxc) {
            // Reuse a healthy entry (downloading or complete). A previously FAILED
            // entry (upstream error/stall) must NOT be handed back — that would make
            // every replay return the same error. Fall through: the stale-incomplete
            // cleanup below drops it (and its partial file) and a fresh download starts.
            if e.progress.borrow().error.is_none() {
                return e.clone();
            }
        }

        let data = self.data_file(mxc);
        // Reuse a completed file from a previous (or this) session — no download.
        if done_path(&data).exists() {
            // Refresh mtime so a replayed video counts as recently used for LRU.
            crate::cache_manager::touch_mtime(&data);
            if let Ok(meta) = std::fs::metadata(&data) {
                let total = meta.len();
                let (tx, _rx) = watch::channel(Progress {
                    total: Some(total),
                    written: total,
                    complete: true,
                    error: None,
                });
                let entry = Arc::new(MediaEntry {
                    path: data,
                    source,
                    progress: tx,
                    cancel: AtomicBool::new(false),
                });
                map.insert(mxc.to_string(), entry.clone());
                return entry;
            }
        }

        // Starting a NEW download. Cancel any other still-downloading entry so the
        // new one gets full bandwidth, and discard its partial file (a no-Range
        // homeserver can't resume a partial).
        let stale: Vec<String> = map
            .iter()
            .filter(|(_, e)| !e.progress.borrow().complete)
            .map(|(k, _)| k.clone())
            .collect();
        for k in stale {
            if let Some(e) = map.remove(&k) {
                e.cancel.store(true, Ordering::Relaxed);
                // Wake any reader parked on this entry's watch channel. Without a
                // terminal publish the reader (which keeps the Sender alive via its
                // own Arc<MediaEntry>) would block until the player's network
                // timeout — see MediaEntry::read_chunk / wait_total.
                publish_cancelled(&e);
                let _ = std::fs::remove_file(&e.path);
            }
        }
        let (tx, _rx) = watch::channel(Progress {
            total: None,
            written: 0,
            complete: false,
            error: None,
        });
        let entry = Arc::new(MediaEntry {
            path: data,
            source,
            progress: tx,
            cancel: AtomicBool::new(false),
        });
        map.insert(mxc.to_string(), entry.clone());

        // Snapshot the active file set (the new entry included, so it's never
        // evicted) and release the entries lock before any disk work.
        let active: HashSet<PathBuf> = map.values().map(|e| e.path.clone()).collect();
        drop(map);

        let dl = entry.clone();
        let mxc_owned = mxc.to_string();
        runtime.spawn(async move {
            if let Err(e) = download_to_file(&dl, &client, &mxc_owned).await {
                dl.progress.send_modify(|p| {
                    if p.error.is_none() {
                        p.error = Some(e.to_string());
                    }
                });
            }
        });

        // Enforce the disk budget off the async worker and off the entries lock:
        // the dir scan + stats + unlinks are blocking std::fs. Fire-and-forget —
        // the new file isn't `.done` yet, so a concurrent sweep can't evict it, and
        // `active` protects every other live file.
        let dir = self.dir.clone();
        tokio::task::spawn_blocking(move || evict_to_budget(&dir, &active));
        entry
    }

    /// Drop all in-memory entries (used by "clear local cache" before the on-disk
    /// files are wiped, so live entries don't reference deleted files). The next
    /// request for any mxc re-creates its entry and re-downloads.
    pub async fn clear_entries(&self) {
        self.cancel_all().await;
    }

    /// Cancel every in-flight download and drop all in-memory entries. Readers
    /// blocked on a cancelled entry wake with an error (the terminal publish
    /// below); the background download task notices `cancel` and stops. Used by
    /// "clear local cache" and by server shutdown/logout before the files are
    /// wiped, so a download can't keep pulling a whole file into an unlinked inode.
    pub async fn cancel_all(&self) {
        let mut map = self.entries.lock().await;
        for (_, e) in map.drain() {
            e.cancel.store(true, Ordering::Relaxed);
            publish_cancelled(&e);
        }
    }

    /// Enforce the (setting-derived) disk budget now, e.g. when the size setting
    /// changes mid-session. Files backing active in-memory entries are never evicted.
    pub async fn enforce_budget(&self) {
        let active: HashSet<PathBuf> = self
            .entries
            .lock()
            .await
            .values()
            .map(|e| e.path.clone())
            .collect();
        let dir = self.dir.clone();
        // Off the async worker: the dir scan + unlinks are blocking std::fs.
        let _ = tokio::task::spawn_blocking(move || evict_to_budget(&dir, &active)).await;
    }

    /// Current download progress for `mxc` as (written, total), if it's being or
    /// has been downloaded this session and its size is known.
    pub async fn progress(&self, mxc: &str) -> Option<(u64, u64)> {
        let map = self.entries.lock().await;
        map.get(mxc).and_then(|e| {
            let p = e.progress.borrow();
            p.total.map(|t| (p.written, t))
        })
    }

    /// Whether `mxc`'s current entry has failed (upstream error, stall, or cancel).
    /// Lets the C++ retry loop fail fast instead of waiting out its stall window on
    /// a stream that is already dead. False when there's no entry for `mxc`.
    pub async fn errored(&self, mxc: &str) -> bool {
        let map = self.entries.lock().await;
        map.get(mxc)
            .map(|e| e.progress.borrow().error.is_some())
            .unwrap_or(false)
    }

    /// Remove the whole cache directory (called on server stop / logout).
    pub fn cleanup(&self) {
        let _ = std::fs::remove_dir_all(&self.dir);
    }
}

/// Path of the cache data file for `mxc` under `dir`: a 64-bit hash → case-stable
/// filename (APFS is case-insensitive by default, so the raw mxc isn't safe).
pub fn cache_data_path(dir: &Path, mxc: &str) -> PathBuf {
    let mut h = std::collections::hash_map::DefaultHasher::new();
    mxc.hash(&mut h);
    dir.join(format!("{:016x}", h.finish()))
}

/// The completion marker path for a data file (`<hash>` → `<hash>.done`).
fn done_path(data: &Path) -> PathBuf {
    let mut s = data.as_os_str().to_os_string();
    s.push(".done");
    PathBuf::from(s)
}

/// At startup, drop incomplete leftovers: a data file with no `.done` marker is a
/// partial (crash/cancel), and a `.done` with no data file is an orphan. Complete
/// pairs are kept for reuse.
fn reclaim_incomplete(dir: &Path) {
    let present: HashSet<String> = match std::fs::read_dir(dir) {
        Ok(rd) => rd
            .flatten()
            .filter_map(|e| e.file_name().into_string().ok())
            .collect(),
        Err(_) => return,
    };
    for name in &present {
        if let Some(stem) = name.strip_suffix(".done") {
            if !present.contains(stem) {
                let _ = std::fs::remove_file(dir.join(name));
            }
        } else if !present.contains(&format!("{name}.done")) {
            let _ = std::fs::remove_file(dir.join(name));
        }
    }
}

/// Evict oldest *completed*, *inactive* (not in `active`) files until the cache
/// dir is within the setting-derived budget, down to the ~50% low watermark.
/// Blocking std::fs (read_dir + stat + unlink) — call off the async worker.
fn evict_to_budget(dir: &Path, active: &HashSet<PathBuf>) {
    let budget = crate::cache_manager::stream_dir_budget_bytes();
    let target = crate::cache_manager::low_watermark_bytes(budget);
    let mut total: u64 = 0;
    let mut evictable: Vec<(PathBuf, u64, SystemTime)> = Vec::new();
    let rd = match std::fs::read_dir(dir) {
        Ok(rd) => rd,
        Err(_) => return,
    };
    for e in rd.flatten() {
        let name = match e.file_name().into_string() {
            Ok(n) => n,
            Err(_) => continue,
        };
        if name.ends_with(".done") {
            continue;
        }
        let data = dir.join(&name);
        if !done_path(&data).exists() {
            continue; // partial — not counted, reclaimed elsewhere
        }
        let meta = match e.metadata() {
            Ok(m) => m,
            Err(_) => continue,
        };
        total += meta.len();
        if !active.contains(&data) {
            let mtime = meta.modified().unwrap_or(SystemTime::UNIX_EPOCH);
            evictable.push((data, meta.len(), mtime));
        }
    }
    if total <= budget {
        return;
    }
    evictable.sort_by_key(|(_, _, mtime)| *mtime); // oldest (least-recently-used) first
                                                   // Evict down to the low watermark (~50%), not just under the limit, so a run of
                                                   // new videos doesn't re-trigger eviction on every download.
    let mut running = total;
    for (data, size, _) in evictable {
        if running <= target {
            break;
        }
        let _ = std::fs::remove_file(&data);
        let _ = std::fs::remove_file(done_path(&data));
        running = running.saturating_sub(size);
    }
}

/// Publish a terminal "cancelled" error on an entry's progress channel, unless it
/// already carries one. Idempotent, so it's safe to call from both the get_or_start
/// cancel loop and the download task's own cancel checks.
fn publish_cancelled(entry: &MediaEntry) {
    entry.progress.send_modify(|p| {
        if p.error.is_none() {
            p.error = Some("superseded by a newer stream".into());
        }
    });
}

/// Open the progressive cache file for writing (create + truncate).
///
/// On Windows, `get_or_start`'s stale-cancel deletes a partial (`remove_file`) that
/// the previous, still-open download task can leave in a "delete pending" state;
/// re-creating that same path then fails with `ACCESS_DENIED` until the old handle
/// closes. Retry the create briefly to ride out the pending delete. On POSIX an
/// unlink is immediate (the inode lives on the old fd), so a single attempt is
/// correct there and any open error is permanent — never retried.
async fn open_cache_file_for_write(path: &Path) -> std::io::Result<File> {
    #[cfg(not(windows))]
    {
        OpenOptions::new()
            .create(true)
            .write(true)
            .truncate(true)
            .open(path)
            .await
    }

    #[cfg(windows)]
    {
        use std::io::ErrorKind;
        // ~1s total (10 × 100ms): enough for the old handle to close at its next
        // chunk boundary. A pathologically-stuck handle still fails here, but the
        // player's higher-level retry re-invokes get_or_start, which tries again.
        let mut last_err = None;
        for _ in 0..10u32 {
            match OpenOptions::new()
                .create(true)
                .write(true)
                .truncate(true)
                .open(path)
                .await
            {
                Ok(file) => return Ok(file),
                // ERROR_ACCESS_DENIED (delete pending) maps to PermissionDenied.
                Err(e) if e.kind() == ErrorKind::PermissionDenied => {
                    last_err = Some(e);
                    tokio::time::sleep(std::time::Duration::from_millis(100)).await;
                }
                Err(e) => return Err(e),
            }
        }
        Err(last_err.unwrap_or_else(|| {
            std::io::Error::new(
                ErrorKind::PermissionDenied,
                "cache file create retries exhausted (delete pending?)",
            )
        }))
    }
}

/// The single background download: GET the whole file and append it to the cache
/// file chunk-by-chunk, publishing progress so readers can stream behind it. On a
/// clean finish it writes the `.done` marker so later sessions reuse the file.
async fn download_to_file(entry: &MediaEntry, client: &Client, mxc: &str) -> Result<()> {
    let (total, mut resp) = fetch_open(client, mxc).await?;
    // A homeserver that streams media with chunked transfer (no Content-Length)
    // can't back a Range/seek proxy: refuse it here so the reader errors and the
    // player falls back to the full-download path instead of getting a broken
    // zero-length response.
    let Some(total) = total else {
        return Err(anyhow!("upstream sent no Content-Length; not streamable"));
    };
    entry.progress.send_modify(|p| p.total = Some(total));

    let mut file = open_cache_file_for_write(&entry.path).await?;

    // Classify the container from the head as it streams in. A `moov` at the end
    // means the whole file must download before the first frame, and the players
    // show real download progress instead of an indeterminate spinner. In
    // practice this decides on the very first chunk (the box type alone settles
    // it), so the verdict is in the store before the UI's first 300ms poll.
    let key_iv = match &entry.source {
        MediaSource::Encrypted(f) => key_iv_from_encrypted(f).ok(),
        MediaSource::Plain(_) => None,
    };
    let mut head: Vec<u8> = Vec::new();
    let mut classified = false;

    while let Some(chunk) = resp.chunk().await? {
        if entry.cancel.load(Ordering::Relaxed) {
            // A newer video opened; stop and leave the partial file (already
            // unlinked by get_or_start) to be reclaimed. Publish a terminal error
            // (idempotent) so any reader still parked on this entry wakes promptly
            // rather than waiting out the player's network timeout.
            publish_cancelled(entry);
            return Ok(());
        }
        file.write_all(&chunk).await?;
        file.flush().await?;
        let n = chunk.len() as u64;
        entry.progress.send_modify(|p| p.written += n);

        if !classified && head.len() < HEAD_PROBE_BYTES {
            let want = HEAD_PROBE_BYTES - head.len();
            head.extend_from_slice(&chunk[..want.min(chunk.len())]);
            let full = head.len() >= HEAD_PROBE_BYTES;
            if let Some(verdict) = classify_head(&head, &key_iv, full) {
                classified = true;
                container_store::store(client, mxc.to_string(), verdict).await;
            }
        }
    }

    if entry.cancel.load(Ordering::Relaxed) {
        publish_cancelled(entry);
        return Ok(());
    }
    // A file shorter than the probe window: classify what we got.
    if !classified && !head.is_empty() {
        if let Some(verdict) = classify_head(&head, &key_iv, true) {
            container_store::store(client, mxc.to_string(), verdict).await;
        }
    }
    file.flush().await?;
    drop(file);
    // Persist the completion marker so a later session reuses this file.
    let _ = tokio::fs::write(done_path(&entry.path), b"").await;
    entry.progress.send_modify(|p| p.complete = true);
    Ok(())
}

/// Fetch and decrypt the first `max_bytes` of a video for thumbnail first-frame
/// extraction. Reuses a completed proxy cache file if present (the persisted
/// stream cache doubles as a thumbnail source), else does a capped upstream fetch
/// that never transfers more than `max_bytes` even when the homeserver ignores
/// `Range`. Encrypted media is CTR-decrypted from offset 0 — CTR is seekable, so
/// the prefix decrypts standalone (full-file SHA integrity isn't checked, which is
/// fine for a preview).
pub async fn fetch_decrypted_prefix(
    client: &Client,
    mxc: &str,
    source: &MediaSource,
    cache_dir: &Path,
    max_bytes: u64,
) -> Result<Vec<u8>> {
    let data_path = cache_data_path(cache_dir, mxc);
    let mut bytes = if done_path(&data_path).exists() {
        read_file_prefix(&data_path, max_bytes).await?
    } else {
        fetch_prefix(client, mxc, max_bytes).await?
    };
    if let MediaSource::Encrypted(file) = source {
        let (key, iv) = key_iv_from_encrypted(file)?;
        decrypt_ctr_range(&key, &iv, 0, &mut bytes);
    }
    Ok(bytes)
}

/// Read the first `max_bytes` (or the whole file if shorter) of a cache file.
async fn read_file_prefix(path: &Path, max_bytes: u64) -> Result<Vec<u8>> {
    let len = tokio::fs::metadata(path).await?.len().min(max_bytes) as usize;
    let mut f = File::open(path).await?;
    let mut buf = vec![0u8; len];
    f.read_exact(&mut buf).await?;
    Ok(buf)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    fn test_entry(progress: Progress) -> Arc<MediaEntry> {
        let (tx, _rx) = watch::channel(progress);
        Arc::new(MediaEntry {
            path: PathBuf::from("/nonexistent/telematrix-stream-cache-test"),
            source: MediaSource::Plain("mxc://test/entry".into()),
            progress: tx,
            cancel: AtomicBool::new(false),
        })
    }

    // H1 regression: a reader parked in read_chunk must wake with Err when the
    // download is cancelled (terminal error published), not hang until the
    // player's network timeout. The nonexistent path is never opened because the
    // error is checked before the on-disk read.
    #[tokio::test]
    async fn read_chunk_wakes_with_err_on_cancel_publish() {
        let entry = test_entry(Progress {
            total: Some(1024),
            written: 0, // offset 0 is not < written 0 → the reader must park
            complete: false,
            error: None,
        });
        let reader = entry.clone();
        let handle = tokio::spawn(async move { reader.read_chunk(0, 256).await });
        tokio::task::yield_now().await; // let the reader reach rx.changed()
        publish_cancelled(&entry);
        let result = tokio::time::timeout(Duration::from_secs(1), handle)
            .await
            .expect("read_chunk did not wake within 1s of the cancel publish")
            .expect("reader task panicked");
        assert!(result.is_err(), "read_chunk should return Err after cancel");
    }

    // H1 regression, size-headers variant: wait_total must likewise wake with Err.
    #[tokio::test]
    async fn wait_total_wakes_with_err_on_cancel_publish() {
        let entry = test_entry(Progress {
            total: None, // size unknown → wait_total parks until total or error
            written: 0,
            complete: false,
            error: None,
        });
        let waiter = entry.clone();
        let handle = tokio::spawn(async move { waiter.wait_total().await });
        tokio::task::yield_now().await;
        publish_cancelled(&entry);
        let result = tokio::time::timeout(Duration::from_secs(1), handle)
            .await
            .expect("wait_total did not wake within 1s of the cancel publish")
            .expect("waiter task panicked");
        assert!(result.is_err(), "wait_total should return Err after cancel");
    }

    // cancel_all drains the map and publishes a terminal error on each entry, so
    // any in-flight reader wakes and the background download task will stop.
    #[tokio::test]
    async fn cancel_all_publishes_error_and_drains() {
        let dir = std::env::temp_dir().join("telematrix-cancel-all-test");
        let _ = std::fs::create_dir_all(&dir);
        let cache = MediaCache::new(dir);
        let entry = test_entry(Progress {
            total: None,
            written: 0,
            complete: false,
            error: None,
        });
        cache
            .entries
            .lock()
            .await
            .insert("mxc://test/entry".into(), entry.clone());
        cache.cancel_all().await;
        assert!(
            entry.cancel.load(Ordering::Relaxed),
            "entry marked cancelled"
        );
        assert!(
            entry.progress.borrow().error.is_some(),
            "terminal error published"
        );
        assert!(cache.entries.lock().await.is_empty(), "entries drained");
    }

    // The write-open helper must create the file and truncate any prior content
    // (the download restarts from byte 0). Guards the L5 refactor's flag preservation
    // on the tested platform; the Windows delete-pending retry is cfg'd out here.
    #[tokio::test]
    async fn open_cache_file_for_write_creates_and_truncates() {
        let dir = std::env::temp_dir().join("telematrix-open-write-test");
        let _ = std::fs::create_dir_all(&dir);
        let path = dir.join("open-write-file");
        tokio::fs::write(&path, b"stale-partial-bytes")
            .await
            .unwrap();
        {
            let mut f = open_cache_file_for_write(&path).await.unwrap();
            f.write_all(b"x").await.unwrap();
            f.flush().await.unwrap();
        }
        let contents = tokio::fs::read(&path).await.unwrap();
        assert_eq!(contents, b"x", "create+truncate must replace old content");
        let _ = std::fs::remove_file(&path);
    }
}

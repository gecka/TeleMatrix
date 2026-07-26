// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Cache management utilities for TeleMatrix.
//!
//! Provides cache statistics calculation and eviction for media files,
//! preview databases, and SDK sqlite stores.

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::OnceLock;
use std::time::{Duration, SystemTime};

use anyhow::Result;
use tokio::sync::Notify;

// ---- Media-cache size budget (pushed from the C++ `cacheSizeLimitMB` setting) ----

/// Total media-on-disk budget in bytes. 0 = not yet pushed by the app; callers
/// fall back to `DEFAULT_MEDIA_CACHE_LIMIT_BYTES`.
static MEDIA_CACHE_LIMIT_BYTES: AtomicU64 = AtomicU64::new(0);

/// Fallback budget used before the app pushes the setting (matches the app default).
pub const DEFAULT_MEDIA_CACHE_LIMIT_BYTES: u64 = 4 * 1000 * 1024 * 1024;

/// Split of the total budget across the two file caches.
const MEDIA_DIR_SHARE_PCT: u64 = 70; // media_cache/ (downloads, photos, files, thumbs)
const STREAM_DIR_SHARE_PCT: u64 = 30; // media-stream-cache/ (videos)
/// After crossing a budget, evict LRU down to this fraction of it (hysteresis,
/// so eviction doesn't re-run on every new file).
const LOW_WATERMARK_PCT: u64 = 50;
/// Slice reserved for the SDK thumbnail store (matrix-sdk-media.sqlite3), capped
/// so it never dominates the budget (thumbnails are small).
const SDK_THUMB_RESERVE_MAX_BYTES: u64 = 128 * 1024 * 1024;

/// Set the total media budget (called from the FFI when the setting changes).
pub fn set_media_cache_limit_bytes(bytes: u64) {
    MEDIA_CACHE_LIMIT_BYTES.store(bytes, Ordering::Relaxed);
}

/// Current total media budget, or the default if the app hasn't pushed one yet.
pub fn media_cache_limit_bytes() -> u64 {
    match MEDIA_CACHE_LIMIT_BYTES.load(Ordering::Relaxed) {
        0 => DEFAULT_MEDIA_CACHE_LIMIT_BYTES,
        v => v,
    }
}

/// Live signed-in account count, so the media budget is shared across accounts
/// instead of each account's dir filling to the full cap. See PERF-4.
static LIVE_ACCOUNTS: AtomicUsize = AtomicUsize::new(0);

/// Record that an account (Rust Handle) started; call from tm_create.
pub fn note_account_started() {
    LIVE_ACCOUNTS.fetch_add(1, Ordering::Relaxed);
}

/// Record that an account (Rust Handle) was destroyed; call from tm_destroy.
pub fn note_account_stopped() {
    let _ = LIVE_ACCOUNTS.fetch_update(Ordering::Relaxed, Ordering::Relaxed, |v| {
        Some(v.saturating_sub(1))
    });
}

/// The media budget a SINGLE account's dirs may use: the global total split across
/// live accounts, so N accounts don't each fill their `media_cache/` to the full
/// configured cap (which put up to N× the total on disk). See code-review-2026-07-19 PERF-4.
fn per_account_media_budget_bytes() -> u64 {
    let live = LIVE_ACCOUNTS.load(Ordering::Relaxed).max(1) as u64;
    media_cache_limit_bytes() / live
}

/// High-watermark budget for one account's `media_cache/` dir.
pub fn media_dir_budget_bytes() -> u64 {
    per_account_media_budget_bytes() / 100 * MEDIA_DIR_SHARE_PCT
}

/// High-watermark budget for one account's `media-stream-cache/` dir.
pub fn stream_dir_budget_bytes() -> u64 {
    per_account_media_budget_bytes() / 100 * STREAM_DIR_SHARE_PCT
}

/// Low watermark (eviction target) for a given high-watermark budget.
pub fn low_watermark_bytes(budget: u64) -> u64 {
    budget / 100 * LOW_WATERMARK_PCT
}

/// Reserved budget for the SDK thumbnail store's `MediaRetentionPolicy`.
pub fn sdk_thumb_reserve_bytes() -> u64 {
    (media_cache_limit_bytes() / 8).min(SDK_THUMB_RESERVE_MAX_BYTES)
}

/// Signaled after each media write so the debounced enforcement loop wakes.
fn media_stored_notify() -> &'static Notify {
    static N: OnceLock<Notify> = OnceLock::new();
    N.get_or_init(Notify::new)
}

/// Wake the debounced media-cache enforcement loop (called after a store).
pub fn signal_media_stored() {
    // notify_waiters, not notify_one: up to 6 per-account enforcement loops park on
    // this one global Notify, and notify_one wakes an arbitrary one — often not the
    // account whose dir actually grew — leaving the busy account over budget. Waking
    // all lets each re-check its own dir. (notify_waiters stores no permit, so a store
    // landing while a loop is mid-enforce is folded into the next signal; the mtime-LRU
    // budget has hysteresis, so it self-heals.) See code-review-2026-07-19 PERF-5.
    media_stored_notify().notify_waiters();
}

/// Await the next media-store signal (used by the enforcement loop).
pub async fn media_stored_notified() {
    media_stored_notify().notified().await;
}

/// Refresh a cached file's mtime to now, so the mtime-ordered LRU treats a
/// recently-READ file as recently used (not just recently written).
pub fn touch_mtime(path: &Path) {
    let _ = filetime::set_file_mtime(path, filetime::FileTime::now());
}

/// Aggregate cache size statistics.
#[derive(Debug, Clone, Default)]
pub struct CacheStats {
    /// Total bytes of files in `media_cache/`.
    pub media_files_bytes: u64,
    /// Size of `preview_cache.db` + WAL + SHM.
    pub preview_cache_bytes: u64,
    /// Size of `app_cache.db` + WAL + SHM.
    pub app_cache_bytes: u64,
    /// Size of `search_index.db` + WAL + SHM.
    pub search_index_bytes: u64,
    /// Sum of all above.
    pub total_bytes: u64,
    /// Number of files in `media_cache/`.
    pub media_file_count: u64,
}

/// Calculate cache statistics by scanning the data directory.
pub async fn calculate_stats(data_dir: &Path) -> CacheStats {
    let mut stats = CacheStats::default();

    let media_dir = data_dir.join("media_cache");
    if let Ok(files) = collect_files_recursive(&media_dir).await {
        stats.media_files_bytes = files.iter().map(|(_, size, _)| *size).sum();
        stats.media_file_count = files.len() as u64;
    }

    // Progressive video-stream cache (persisted completed videos) counts as media.
    let stream_dir = data_dir.join("media-stream-cache");
    if let Ok(files) = collect_files_recursive(&stream_dir).await {
        stats.media_files_bytes += files.iter().map(|(_, size, _)| *size).sum::<u64>();
        stats.media_file_count += files.len() as u64;
    }

    // preview cache DB: preview_cache.db + WAL + SHM
    stats.preview_cache_bytes = file_size_with_wal(data_dir, "preview_cache.db").await;
    stats.app_cache_bytes = file_size_with_wal(data_dir, "app_cache.db").await;
    stats.search_index_bytes = file_size_with_wal(data_dir, "search_index.db").await;

    stats.total_bytes = stats.media_files_bytes
        + stats.preview_cache_bytes
        + stats.app_cache_bytes
        + stats.search_index_bytes;

    stats
}

/// LRU-evict the app media cache (`media_cache/`) to `size_limit_bytes` (age +
/// size). Thin wrapper over `evict_dir_lru` evicting down to the limit; used by
/// the manual `tm_clear_media_cache` path. Returns total bytes freed.
pub async fn clear_media_files(
    data_dir: &Path,
    max_age_days: u32,
    size_limit_bytes: u64,
) -> Result<u64> {
    evict_dir_lru(
        &data_dir.join("media_cache"),
        max_age_days,
        size_limit_bytes,
        size_limit_bytes,
    )
    .await
}

/// Enforce the configured `media_cache/` budget with hysteresis: when usage
/// exceeds the high watermark (its share of the total budget), evict the
/// least-recently-used files down to the low watermark (~50%). Returns bytes freed.
pub async fn enforce_media_cache_limit(data_dir: &Path) -> Result<u64> {
    let high = media_dir_budget_bytes();
    let target = low_watermark_bytes(high);
    evict_dir_lru(&data_dir.join("media_cache"), 0, high, target).await
}

/// Generic LRU eviction over a directory tree.
///
/// Phase 1: remove files older than `max_age_days` (0 = skip).
/// Phase 2: if the total still exceeds `high_bytes`, sort by mtime (oldest first)
///          and remove until at or below `target_bytes` (`target <= high`). With
///          `target < high` this gives hysteresis; with `target == high` it just
///          trims to the limit. mtime is refreshed on read (`touch_mtime`), so
///          "oldest" means least-recently-USED. Returns total bytes freed.
pub async fn evict_dir_lru(
    dir: &Path,
    max_age_days: u32,
    high_bytes: u64,
    target_bytes: u64,
) -> Result<u64> {
    if !dir.exists() {
        return Ok(0);
    }

    let mut files = collect_files_recursive(dir).await?;
    let mut freed: u64 = 0;
    let now = SystemTime::now();

    // Phase 1: remove files older than max_age_days
    if max_age_days > 0 {
        let cutoff = Duration::from_secs(max_age_days as u64 * 24 * 60 * 60);
        let mut remaining = Vec::new();
        for (path, size, modified) in files {
            let age = now.duration_since(modified).unwrap_or_default();
            if age > cutoff {
                if tokio::fs::remove_file(&path).await.is_ok() {
                    freed += size;
                } else {
                    remaining.push((path, size, modified));
                }
            } else {
                remaining.push((path, size, modified));
            }
        }
        files = remaining;
    }

    // Phase 2: LRU eviction down to the target if still over the high watermark
    let current_size: u64 = files.iter().map(|(_, s, _)| s).sum();
    if current_size > high_bytes {
        files.sort_by_key(|(_, _, modified)| *modified); // oldest (least-recently-used) first

        let mut running_size = current_size;
        for (path, size, _) in &files {
            if running_size <= target_bytes {
                break;
            }
            if tokio::fs::remove_file(path).await.is_ok() {
                freed += size;
                running_size -= size;
            }
        }
    }

    Ok(freed)
}

/// Remove all cached data:
/// - All files in `media_cache/`
/// - `preview_cache.db` (+ WAL + SHM)
/// - (`search_index.db` is deleted by its connection owner under lock, not here)
/// - VACUUM the two SDK sqlite databases
///
/// Returns total bytes freed.
pub async fn clear_all(data_dir: &Path) -> Result<u64> {
    let mut freed: u64 = 0;

    let media_dir = data_dir.join("media_cache");
    if media_dir.exists() {
        if let Ok(files) = collect_files_recursive(&media_dir).await {
            freed += files.iter().map(|(_, size, _)| *size).sum::<u64>();
        }
        let _ = tokio::fs::remove_dir_all(&media_dir).await;
    }

    // Remove preview_cache.db + WAL + SHM
    for suffix in &["", "-wal", "-shm"] {
        let path = data_dir.join(format!("preview_cache.db{}", suffix));
        if let Ok(meta) = tokio::fs::metadata(&path).await {
            let size = meta.len();
            if tokio::fs::remove_file(&path).await.is_ok() {
                freed += size;
            }
        }
    }

    // search_index.db is deleted by its connection owner (clear_cache_data /
    // reset_local_cache_stores_for_new_session) while holding the search-index
    // mutex, so the sync loop cannot reopen it mid-delete. Do NOT delete it here:
    // doing so would race a freshly reopened live connection -> the file is
    // unlinked under it -> SQLITE_READONLY_DBMOVED on the next insert.

    // The SDK media + event-cache SQLite stores are NOT cleared here. The media
    // (thumbnail) store is bounded separately by its MediaRetentionPolicy (set in
    // MatrixProtocol::set_media_cache_limit); the event cache is intentionally
    // retained (offline history). Neither can be safely truncated via a second
    // SQLite connection while the SDK holds them open.

    Ok(freed)
}

/// Wipe the progressive video-stream cache dir and recreate it empty. The live
/// in-memory `MediaCache` map must be cleared separately (it holds open handles /
/// entries that would otherwise point at the deleted files). Returns bytes freed.
pub async fn clear_stream_cache(data_dir: &Path) -> u64 {
    let dir = data_dir.join("media-stream-cache");
    let mut freed = 0u64;
    if let Ok(files) = collect_files_recursive(&dir).await {
        freed = files.iter().map(|(_, size, _)| *size).sum();
    }
    let _ = tokio::fs::remove_dir_all(&dir).await;
    let _ = tokio::fs::create_dir_all(&dir).await;
    freed
}

async fn collect_files_recursive(root: &Path) -> Result<Vec<(PathBuf, u64, SystemTime)>> {
    let mut result = Vec::new();
    if !root.exists() {
        return Ok(result);
    }
    let mut stack = vec![root.to_path_buf()];
    while let Some(dir) = stack.pop() {
        let mut entries = match tokio::fs::read_dir(&dir).await {
            Ok(entries) => entries,
            Err(_) => continue,
        };
        while let Ok(Some(entry)) = entries.next_entry().await {
            let meta = match entry.metadata().await {
                Ok(meta) => meta,
                Err(_) => continue,
            };
            if meta.is_dir() {
                stack.push(entry.path());
            } else if meta.is_file() {
                result.push((
                    entry.path(),
                    meta.len(),
                    meta.modified().unwrap_or(SystemTime::UNIX_EPOCH),
                ));
            }
        }
    }
    Ok(result)
}

/// Get the combined size of a database file + its WAL and SHM files (async).
async fn file_size_with_wal(data_dir: &Path, db_relative: &str) -> u64 {
    let mut total = 0u64;
    for suffix in &["", "-wal", "-shm"] {
        let path = data_dir.join(format!("{}{}", db_relative, suffix));
        if let Ok(meta) = tokio::fs::metadata(&path).await {
            total += meta.len();
        }
    }
    total
}

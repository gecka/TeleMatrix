// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Seeds the media cache from a file we just uploaded, so its server echo does
//! not download back the bytes still sitting on the user's disk.
//!
//! The cache is keyed by the `mxc://` URI, which only exists once the upload has
//! finished and the event has come back through sync — and the cache key doubles
//! as AEAD associated data, so a blob written under a provisional key could not
//! be renamed into place later. Instead `send_media` registers the source path
//! under the upload's transaction id here, and timeline conversion — the first
//! point where both the txn id and the mxc are known — claims the entry and
//! encrypts the file into the cache slot, before the item reaches C++.
//!
//! Encrypted rooms are covered for free: `MediaCacheService` stores media as
//! plaintext (re-encrypted under the local cache key), which is exactly what we
//! handed to `send_attachment`. Video *playback* is not covered — it streams
//! from `media_stream::cache`, which holds raw server bytes (ciphertext in an
//! encrypted room) that the upload path never sees.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{LazyLock, Mutex};

use anyhow::{anyhow, Result};

use crate::media_cache_service::MediaCacheService;

/// Placeholder media server the SDK gives send-queue local echoes, before the
/// real upload resolves (`matrix_sdk::media::LOCAL_MXC_SERVER_NAME`).
const LOCAL_SEND_QUEUE_PREFIX: &str = "mxc://send-queue.localhost/";

/// Ceiling on one seeded body. Seeding costs a full encrypt + a second on-disk
/// copy of a file the user already has, and only image bodies are fetched
/// automatically — for everything else it just makes the first open/save click
/// instant. Not worth spending a large slice of the media budget on.
const MAX_SEED_BYTES: u64 = 64 * 1024 * 1024;

/// Uploads awaiting an echo. Entries are claimed by conversion; the cap bounds
/// the ones that never will be (cancelled, failed, sent from another device).
const MAX_PENDING: usize = 32;

struct Seed {
    path: PathBuf,
    /// Size at registration — re-checked before seeding, so a source edited
    /// between upload and echo is skipped rather than cached as the event body.
    size: u64,
    cache: MediaCacheService,
    /// Insertion order, for evicting the oldest entry when the map is full.
    seq: u64,
}

static SEEDS: LazyLock<Mutex<HashMap<String, Seed>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
static NEXT_SEQ: AtomicU64 = AtomicU64::new(0);

/// Per-upload ceiling: also bounded by the media budget, which the user can set
/// as low as 50 MB — one seeded file must never dominate it.
fn max_seed_bytes() -> u64 {
    MAX_SEED_BYTES.min(crate::cache_manager::media_dir_budget_bytes() / 8)
}

/// Remember `path` as the source of the upload identified by `txn_id`. Must be
/// called before the upload starts, so no echo can beat the registration.
pub(crate) fn register(txn_id: &str, path: &Path, cache: &MediaCacheService) {
    if txn_id.is_empty() {
        return;
    }
    let size = match std::fs::metadata(path) {
        Ok(meta) => meta.len(),
        Err(_) => return,
    };
    if size == 0 || size > max_seed_bytes() {
        return;
    }
    let Ok(mut map) = SEEDS.lock() else {
        return;
    };
    if map.len() >= MAX_PENDING && !map.contains_key(txn_id) {
        let oldest = map
            .iter()
            .min_by_key(|(_, seed)| seed.seq)
            .map(|(txn, _)| txn.clone());
        if let Some(oldest) = oldest {
            map.remove(&oldest);
        }
    }
    map.insert(
        txn_id.to_string(),
        Seed {
            path: path.to_path_buf(),
            size,
            cache: cache.clone(),
            seq: NEXT_SEQ.fetch_add(1, Ordering::Relaxed),
        },
    );
}

/// Forget an upload that will never produce an echo (failed or cancelled).
pub(crate) fn remove(txn_id: &str) {
    if let Ok(mut map) = SEEDS.lock() {
        map.remove(txn_id);
    }
}

/// Drop every pending seed (logout: the cache dir and its key are gone).
pub(crate) fn clear() {
    if let Ok(mut map) = SEEDS.lock() {
        map.clear();
    }
}

/// Claim the upload registered for `txn_id` and cache its file under `mxc_url`.
///
/// Synchronous, so the timeline conversion path can call it; the encrypt runs on
/// a spawned task. Every failure is swallowed — the render path then downloads
/// the media as it did before.
pub(crate) fn seed_if_pending(txn_id: &str, mxc_url: &str) {
    if txn_id.is_empty()
        || !mxc_url.starts_with("mxc://")
        // A send-queue local echo still points at the placeholder source; leave
        // the entry for the remote echo, which carries the real mxc.
        || mxc_url.starts_with(LOCAL_SEND_QUEUE_PREFIX)
    {
        return;
    }
    let Ok(runtime) = tokio::runtime::Handle::try_current() else {
        return;
    };
    let Some(seed) = (match SEEDS.lock() {
        Ok(mut map) => map.remove(txn_id),
        Err(_) => None,
    }) else {
        return;
    };

    let mxc = mxc_url.to_string();
    runtime.spawn(async move {
        if let Err(err) = store(&seed, &mxc).await {
            tracing::debug!("not seeding uploaded media for {mxc}: {err}");
        } else {
            tracing::debug!(
                "seeded uploaded media for {mxc} from {}",
                seed.path.display()
            );
        }
    });
}

async fn store(seed: &Seed, mxc: &str) -> Result<()> {
    if seed.cache.encrypted_path(mxc).exists() {
        return Ok(());
    }
    let size = tokio::fs::metadata(&seed.path).await?.len();
    if size != seed.size {
        return Err(anyhow!(
            "source changed since upload ({size} bytes, uploaded {})",
            seed.size
        ));
    }
    seed.cache.seed_from_file(mxc, &seed.path).await
}

#[cfg(test)]
mod tests {
    use super::*;

    /// `SEEDS` and the media budget are process-global; serialize the tests that
    /// assert on them (cargo runs them on parallel threads).
    static TEST_LOCK: Mutex<()> = Mutex::new(());

    fn test_dir() -> PathBuf {
        std::env::temp_dir().join("tm-seed-store-test")
    }

    fn cache() -> MediaCacheService {
        MediaCacheService::new(test_dir())
    }

    fn write_temp(name: &str, bytes: &[u8]) -> PathBuf {
        std::fs::create_dir_all(test_dir()).unwrap();
        let path = test_dir().join(name);
        std::fs::write(&path, bytes).unwrap();
        path
    }

    fn is_pending(txn_id: &str) -> bool {
        SEEDS.lock().unwrap().contains_key(txn_id)
    }

    #[test]
    fn only_a_real_mxc_claims_the_entry() {
        let _guard = TEST_LOCK.lock();
        clear();
        let path = write_temp("claim.bin", b"hello");
        register("txn-claim", &path, &cache());
        assert!(is_pending("txn-claim"));

        // A send-queue local echo must leave the entry for the remote echo.
        seed_if_pending("txn-claim", "mxc://send-queue.localhost/abc");
        assert!(is_pending("txn-claim"));
        seed_if_pending("txn-claim", "https://example.org/file");
        assert!(is_pending("txn-claim"));

        remove("txn-claim");
        assert!(!is_pending("txn-claim"));
    }

    #[tokio::test]
    async fn seeding_claims_the_entry_and_keeps_the_source() {
        let _guard = TEST_LOCK.lock();
        clear();
        let path = write_temp("real.bin", b"hello");
        let cache = cache();
        let mxc = "mxc://example.org/abc";
        // Pre-create the blob so the spawned task short-circuits before it needs
        // the (unavailable in tests) cache key.
        let blob = cache.encrypted_path(mxc);
        std::fs::create_dir_all(blob.parent().unwrap()).unwrap();
        std::fs::write(&blob, b"blob").unwrap();

        register("txn-real", &path, &cache);
        seed_if_pending("txn-real", mxc);
        assert!(!is_pending("txn-real"));
        // The source is the user's own file: never moved or consumed.
        assert!(path.exists());
        let _ = std::fs::remove_file(blob);
    }

    #[test]
    fn skips_missing_empty_and_oversized_sources() {
        let _guard = TEST_LOCK.lock();
        clear();
        register("txn-missing", Path::new("/nonexistent/tm-seed"), &cache());
        let empty = write_temp("empty.bin", b"");
        register("txn-empty", &empty, &cache());

        let big = write_temp("big.bin", &[0u8; 1024]);
        let saved = crate::cache_manager::media_cache_limit_bytes();
        crate::cache_manager::set_media_cache_limit_bytes(1024); // budget/8 < 1KiB
        register("txn-big", &big, &cache());
        crate::cache_manager::set_media_cache_limit_bytes(saved);

        assert!(!is_pending("txn-missing"));
        assert!(!is_pending("txn-empty"));
        assert!(!is_pending("txn-big"));
    }

    #[test]
    fn evicts_oldest_when_full() {
        let _guard = TEST_LOCK.lock();
        clear();
        let path = write_temp("cap.bin", b"x");
        for index in 0..MAX_PENDING + 4 {
            register(&format!("txn-{index}"), &path, &cache());
        }
        assert_eq!(SEEDS.lock().unwrap().len(), MAX_PENDING);
        assert!(!is_pending("txn-0"));
        assert!(is_pending(&format!("txn-{}", MAX_PENDING + 3)));
        clear();
    }
}

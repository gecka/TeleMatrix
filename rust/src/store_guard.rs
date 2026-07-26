// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Process-wide serialization of SDK store directory lifecycle.
//!
//! Two `Handle`s can coexist in one process: the C++ side keeps the old
//! bridge alive through logout while a new bridge logs in, and both operate
//! on the same data directory. Store wipes (logout cleanup, fresh-login prep)
//! and client builds must never interleave on that directory: on a fresh
//! store each sqlite file's cipher is created with a deliberately slow KDF
//! between "read kv: no cipher" and "write kv: cipher", so two concurrent
//! first-opens each create their own store cipher, one overwrites the other,
//! and every value written by the loser becomes permanently unreadable
//! (aead::Error on otherwise intact databases).

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, OnceLock};

use tracing::warn;

type DirLocks = Mutex<HashMap<PathBuf, Arc<tokio::sync::Mutex<()>>>>;

static LOCKS: OnceLock<DirLocks> = OnceLock::new();

/// Acquire the exclusive store-lifecycle lock for `data_dir`.
///
/// Hold the returned guard across any operation that creates, opens, or
/// deletes the SDK store under this directory.
pub(crate) async fn lock_store_dir(data_dir: &Path) -> tokio::sync::OwnedMutexGuard<()> {
    let lock = {
        let map = LOCKS.get_or_init(|| Mutex::new(HashMap::new()));
        let mut guard = match map.lock() {
            Ok(guard) => guard,
            Err(poisoned) => {
                warn!("Recovering poisoned store-guard map lock");
                poisoned.into_inner()
            }
        };
        guard.entry(data_dir.to_path_buf()).or_default().clone()
    };
    lock.lock_owned().await
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU32, Ordering};

    #[tokio::test]
    async fn serializes_same_dir() {
        let dir = PathBuf::from("/tmp/telematrix-store-guard-test");
        let counter = Arc::new(AtomicU32::new(0));

        let mut handles = Vec::new();
        for _ in 0..8 {
            let dir = dir.clone();
            let counter = counter.clone();
            handles.push(tokio::spawn(async move {
                let _guard = lock_store_dir(&dir).await;
                let inside = counter.fetch_add(1, Ordering::SeqCst);
                assert_eq!(inside, 0, "two tasks held the same dir lock at once");
                tokio::time::sleep(std::time::Duration::from_millis(5)).await;
                counter.fetch_sub(1, Ordering::SeqCst);
            }));
        }
        for handle in handles {
            handle.await.unwrap();
        }
    }

    #[tokio::test]
    async fn different_dirs_do_not_block() {
        let guard_a = lock_store_dir(Path::new("/tmp/telematrix-guard-a")).await;
        // Must not deadlock: a different directory uses a different mutex.
        let guard_b = lock_store_dir(Path::new("/tmp/telematrix-guard-b")).await;
        drop(guard_a);
        drop(guard_b);
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Fast logout support: rename heavy encrypted stores aside, delete off-path.
//!
//! Every on-disk store is encrypted with a key held only in the OS keychain,
//! which logout clears synchronously. Once those keys are gone the on-disk data
//! is cryptographically inaccessible, so the slow recursive delete need not
//! block logout. We move the stores into `{data_dir}/.trash/<unique>/` with an
//! atomic rename (O(1) regardless of size) — which also frees the canonical
//! paths instantly so the next login starts clean — then reclaim the space on a
//! detached thread, with a startup [`sweep`] as the durable safety net for an
//! app killed mid-delete.
//!
//! The rename requires the store's file handles to be CLOSED. On POSIX it would
//! succeed with them still open, but Windows refuses to rename or delete a
//! directory holding open files, so the caller must drop the SDK `Client` first
//! and the wipe must stay retry-tolerant while sqlite's connections close on
//! their background blocking threads. A `Client` that leaks (see
//! `integration_tests::session_teardown`) holds those handles for the whole
//! process lifetime, which no retry can outlast.

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

use tracing::{info, warn};

const TRASH_DIR: &str = ".trash";

static SEQ: AtomicU64 = AtomicU64::new(0);

/// `{data_dir}/.trash` — the parent for renamed-aside stores awaiting deletion.
pub(crate) fn trash_root(data_dir: &Path) -> PathBuf {
    data_dir.join(TRASH_DIR)
}

/// Create a fresh, uniquely-named subdirectory under `.trash` to receive a batch
/// of renamed-aside stores. The name embeds time + pid + an atomic counter so
/// rapid repeat logouts (and a second process sharing the data dir) never
/// collide.
pub(crate) fn new_trash_subdir(data_dir: &Path) -> std::io::Result<PathBuf> {
    let root = trash_root(data_dir);
    std::fs::create_dir_all(&root)?;
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    let pid = std::process::id();
    for attempt in 0..u32::MAX {
        let seq = SEQ.fetch_add(1, Ordering::Relaxed);
        let candidate = root.join(format!("{nanos}-{pid}-{seq}-{attempt}"));
        match std::fs::create_dir(&candidate) {
            Ok(()) => return Ok(candidate),
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(e) => return Err(e),
        }
    }
    Err(std::io::Error::new(
        std::io::ErrorKind::AlreadyExists,
        "could not allocate a unique trash subdirectory",
    ))
}

/// Recursively delete a single trash entry on a detached OS thread.
///
/// Must be a plain thread, not a tokio task: logout tears the runtime down
/// (`tm_destroy`) right after, which would abort a spawned task. The thread
/// never panics (panic=abort would make that fatal) — every error is logged and
/// the entry is left for the next startup [`sweep`].
pub(crate) fn spawn_background_delete(path: PathBuf) {
    let builder = std::thread::Builder::new().name("tm-trash-delete".into());
    if let Err(e) = builder.spawn(move || remove_path(&path)) {
        warn!("trash: failed to spawn background delete thread: {e}");
    }
}

/// Reclaim any leftover trash from a previous run (e.g. the app was killed
/// mid-delete). Only a cheap directory listing happens on the caller's thread;
/// each leftover is removed on a detached thread so startup is never blocked on
/// a large leftover.
pub(crate) fn sweep(data_dir: &Path) {
    let root = trash_root(data_dir);
    let entries = match std::fs::read_dir(&root) {
        Ok(entries) => entries,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return,
        Err(e) => {
            warn!("trash: cannot read {}: {e}", root.display());
            return;
        }
    };
    let mut swept = 0u32;
    for entry in entries.flatten() {
        spawn_background_delete(entry.path());
        swept += 1;
    }
    if swept > 0 {
        info!(
            "trash: reclaiming {swept} leftover item(s) from {}",
            root.display()
        );
    }
}

/// Remove a path recursively (dir) or as a single file, swallowing a missing
/// target. Never panics.
fn remove_path(path: &Path) {
    let result = if path.is_dir() {
        std::fs::remove_dir_all(path)
    } else {
        std::fs::remove_file(path)
    };
    if let Err(e) = result {
        if e.kind() != std::io::ErrorKind::NotFound {
            warn!("trash: failed to delete {}: {e}", path.display());
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_dir(tag: &str) -> PathBuf {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0);
        std::env::temp_dir().join(format!(
            "telematrix-trash-test-{tag}-{nanos}-{}",
            std::process::id()
        ))
    }

    #[test]
    fn new_trash_subdir_is_unique_and_created() {
        let data_dir = temp_dir("unique");
        std::fs::create_dir_all(&data_dir).unwrap();
        let a = new_trash_subdir(&data_dir).unwrap();
        let b = new_trash_subdir(&data_dir).unwrap();
        assert_ne!(a, b);
        assert!(a.is_dir());
        assert!(b.is_dir());
        assert!(a.starts_with(trash_root(&data_dir)));
        let _ = std::fs::remove_dir_all(&data_dir);
    }

    #[test]
    fn sweep_absent_trash_is_noop() {
        let data_dir = temp_dir("absent");
        std::fs::create_dir_all(&data_dir).unwrap();
        // No `.trash` dir present — must not panic or error.
        sweep(&data_dir);
        let _ = std::fs::remove_dir_all(&data_dir);
    }

    #[test]
    fn remove_path_deletes_dir_then_tolerates_absence() {
        let data_dir = temp_dir("remove");
        let dir = data_dir.join("sub");
        std::fs::create_dir_all(dir.join("nested")).unwrap();
        std::fs::write(dir.join("nested/f.bin"), b"x").unwrap();
        remove_path(&dir);
        assert!(!dir.exists());
        // Removing an already-absent path is a silent no-op.
        remove_path(&dir);
        let _ = std::fs::remove_dir_all(&data_dir);
    }
}

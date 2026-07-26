// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Persisted "can this video stream?" verdicts, keyed by media `mxc://` URL.
//!
//! The verdict (see `media_stream::container`) is learned the first time anyone
//! reads a video's leading bytes — either the streaming proxy's download or the
//! thumbnail path's 2 MB prefix, whichever happens first. It must outlive the
//! video file itself: the media cache is LRU-capped, so a large video is evicted
//! while its tiny derived poster survives, and without this store the app would
//! re-learn from scratch on every click.
//!
//! `store()` updates the in-memory mirror *synchronously* before awaiting the
//! disk write, so a `cached()` read right after a classification sees it. That
//! makes this mirror both the live and the persisted signal — the FFI needs no
//! runtime and no merge with the proxy's in-flight state.

use std::collections::HashMap;
use std::sync::{LazyLock, RwLock};

use matrix_sdk::Client;

use crate::media_stream::container::Container;

/// Custom state-store key holding the `{ mxc: verdict }` JSON blob.
const STORE_KEY: &[u8] = b"telematrix:video_containers:v1";

/// The blob is rewritten whole on every insert, and a verdict costs ~40 bytes of
/// header to re-learn, so cap the map rather than let it grow with the account.
const MAX_ENTRIES: usize = 4096;

static CACHE: LazyLock<RwLock<HashMap<String, u8>>> = LazyLock::new(|| RwLock::new(HashMap::new()));

/// The verdict for a media mxc URL, `Unknown` when we've never classified it.
/// Synchronous — safe to call from the FFI without a runtime.
pub(crate) fn cached(mxc: &str) -> Container {
    if mxc.is_empty() {
        return Container::Unknown;
    }
    let raw = CACHE
        .read()
        .ok()
        .and_then(|map| map.get(mxc).copied())
        .unwrap_or(0);
    match raw {
        1 => Container::Faststart,
        2 => Container::MoovAtEnd,
        _ => Container::Unknown,
    }
}

/// Merge this session's stored verdicts into the in-memory mirror. Call once per
/// session start. Merging (rather than replacing) matters with several accounts
/// live: a second session starting must not evict what the first already learned.
pub(crate) async fn load(client: &Client) {
    let map = match client.state_store().get_custom_value(STORE_KEY).await {
        Ok(Some(bytes)) => {
            serde_json::from_slice::<HashMap<String, u8>>(&bytes).unwrap_or_default()
        }
        _ => HashMap::new(),
    };
    merge_into_mirror(map);
}

/// Fold one session's stored verdicts into the shared mirror, keeping whatever
/// other live sessions already learned.
fn merge_into_mirror(map: HashMap<String, u8>) {
    if let Ok(mut guard) = CACHE.write() {
        guard.extend(map);
    }
}

/// Record a learned verdict in the mirror and persist it to this account's store.
/// `Unknown` is never stored (we'd rather re-classify than cache "don't know"), and
/// a value the mirror already holds is a no-op — both writers (proxy download,
/// thumbnail prefix) can race on the same mxc harmlessly. That no-op is keyed on the
/// shared mirror, so a verdict another live account already learned is not written
/// again here; the mirror serves it for this run and it costs one header re-read
/// after a restart.
///
/// The blob is re-read and rewritten rather than dumped from the mirror: the mirror
/// is shared by every live session, so writing it whole would copy other accounts'
/// mxc URLs into this account's store.
pub(crate) async fn store(client: &Client, mxc: String, container: Container) {
    let value = match container {
        Container::Unknown => return,
        Container::Faststart => 1u8,
        Container::MoovAtEnd => 2u8,
    };
    if mxc.is_empty() {
        return;
    }
    {
        let mut guard = match CACHE.write() {
            Ok(guard) => guard,
            Err(_) => return,
        };
        if guard.get(&mxc) == Some(&value) {
            return;
        }
        if guard.len() >= MAX_ENTRIES && !guard.contains_key(&mxc) {
            if let Some(victim) = guard.keys().next().cloned() {
                guard.remove(&victim);
            }
        }
        guard.insert(mxc.clone(), value);
    }
    let mut snapshot = match client.state_store().get_custom_value(STORE_KEY).await {
        Ok(Some(bytes)) => {
            serde_json::from_slice::<HashMap<String, u8>>(&bytes).unwrap_or_default()
        }
        _ => HashMap::new(),
    };
    if snapshot.len() >= MAX_ENTRIES && !snapshot.contains_key(&mxc) {
        if let Some(victim) = snapshot.keys().next().cloned() {
            snapshot.remove(&victim);
        }
    }
    snapshot.insert(mxc, value);
    match serde_json::to_vec(&snapshot) {
        Ok(bytes) => {
            if let Err(e) = client
                .state_store()
                .set_custom_value(STORE_KEY, bytes)
                .await
            {
                tracing::warn!("failed to persist video container verdicts: {e}");
            }
        }
        Err(e) => tracing::warn!("failed to serialize video container verdicts: {e}"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The mirror is a process global, so tests that reset it must not overlap.
    static TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

    /// Exercises the mirror directly: `store()` needs a Client, but the
    /// synchronous half (mirror update, no-op, cap) is the part the FFI reads.
    fn reset() {
        if let Ok(mut g) = CACHE.write() {
            g.clear();
        }
    }

    #[test]
    fn cached_maps_raw_values() {
        let _guard = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        reset();
        if let Ok(mut g) = CACHE.write() {
            g.insert("mxc://a".into(), 1);
            g.insert("mxc://b".into(), 2);
            g.insert("mxc://c".into(), 9); // out of range
        }
        assert_eq!(cached("mxc://a"), Container::Faststart);
        assert_eq!(cached("mxc://b"), Container::MoovAtEnd);
        assert_eq!(cached("mxc://c"), Container::Unknown);
        assert_eq!(cached("mxc://missing"), Container::Unknown);
        assert_eq!(cached(""), Container::Unknown);
        reset();
    }

    /// Multi-account: a second session loading its own store must fold into the
    /// mirror, not replace it — replacing would blank the verdicts the account
    /// already running had learned, sending its player back to re-classifying.
    #[test]
    fn loading_a_second_session_keeps_the_first_sessions_verdicts() {
        let _guard = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        reset();
        merge_into_mirror(HashMap::from([("mxc://from-a".to_string(), 1u8)]));
        merge_into_mirror(HashMap::from([("mxc://from-b".to_string(), 2u8)]));

        assert_eq!(cached("mxc://from-a"), Container::Faststart);
        assert_eq!(cached("mxc://from-b"), Container::MoovAtEnd);
        reset();
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Persisted audio durations, keyed by media `mxc://` URL.
//!
//! Audio events frequently omit a duration in their metadata, so a bubble can
//! only learn the real length by playing/probing the file. We persist that
//! learned value in the matrix-sdk state store (one JSON blob under a custom
//! key) and keep a process-global in-memory mirror, so the *synchronous*
//! timeline conversion can fill `duration_ms` without awaiting the store.
//!
//! The mirror is shared by all sessions because a duration is a property of the
//! media itself — the same mxc is the same number of milliseconds long for every
//! account, so there is nothing account-specific to keep apart. Persistence is
//! still per-account: `store` writes back only the entries this account learned,
//! never the shared mirror, so one account's mxc URLs never land in another's
//! state store.

use std::collections::HashMap;
use std::sync::{LazyLock, RwLock};

use matrix_sdk::Client;

/// Custom state-store key holding the `{ mxc: duration_ms }` JSON blob.
const STORE_KEY: &[u8] = b"telematrix:audio_durations:v1";

static CACHE: LazyLock<RwLock<HashMap<String, u64>>> =
    LazyLock::new(|| RwLock::new(HashMap::new()));

/// Cached duration (ms) for a media mxc URL, or 0 when unknown. Synchronous —
/// safe to call from the timeline conversion path.
pub(crate) fn cached_duration_ms(mxc: &str) -> u64 {
    if mxc.is_empty() {
        return 0;
    }
    CACHE
        .read()
        .ok()
        .and_then(|map| map.get(mxc).copied())
        .unwrap_or(0)
}

/// Merge this session's stored durations into the in-memory mirror. Call once per
/// session start. Merging (rather than replacing) matters with several accounts
/// live: a second session starting must not evict what the first already learned.
pub(crate) async fn load(client: &Client) {
    let map = match client.state_store().get_custom_value(STORE_KEY).await {
        Ok(Some(bytes)) => {
            serde_json::from_slice::<HashMap<String, u64>>(&bytes).unwrap_or_default()
        }
        _ => HashMap::new(),
    };
    merge_into_mirror(map);
}

/// Fold one session's stored durations into the shared mirror, keeping whatever
/// other live sessions already learned.
fn merge_into_mirror(map: HashMap<String, u64>) {
    if let Ok(mut guard) = CACHE.write() {
        guard.extend(map);
    }
}

/// Record a learned duration in the mirror and persist it to this account's store.
/// No-op when the value is unchanged (avoids redundant store writes).
///
/// The blob is re-read and rewritten rather than dumped from the mirror: the mirror
/// is shared by every live session, so writing it whole would copy other accounts'
/// mxc URLs into this account's store.
pub(crate) async fn store(client: &Client, mxc: String, duration_ms: u64) {
    if mxc.is_empty() || duration_ms == 0 {
        return;
    }
    {
        let mut guard = match CACHE.write() {
            Ok(guard) => guard,
            Err(_) => return,
        };
        if guard.get(&mxc) == Some(&duration_ms) {
            return;
        }
        guard.insert(mxc.clone(), duration_ms);
    }
    let mut snapshot = match client.state_store().get_custom_value(STORE_KEY).await {
        Ok(Some(bytes)) => {
            serde_json::from_slice::<HashMap<String, u64>>(&bytes).unwrap_or_default()
        }
        _ => HashMap::new(),
    };
    snapshot.insert(mxc, duration_ms);
    match serde_json::to_vec(&snapshot) {
        Ok(bytes) => {
            if let Err(e) = client
                .state_store()
                .set_custom_value(STORE_KEY, bytes)
                .await
            {
                tracing::warn!("failed to persist audio durations: {e}");
            }
        }
        Err(e) => tracing::warn!("failed to serialize audio durations: {e}"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn reset() {
        if let Ok(mut guard) = CACHE.write() {
            guard.clear();
        }
    }

    /// Multi-account: a second session loading its own store must fold into the
    /// mirror, not replace it — replacing would drop durations the account already
    /// running had learned, and its audio bubbles would go back to showing 0:00.
    #[test]
    fn loading_a_second_session_keeps_the_first_sessions_durations() {
        reset();
        merge_into_mirror(HashMap::from([("mxc://from-a".to_string(), 4_000u64)]));
        merge_into_mirror(HashMap::from([("mxc://from-b".to_string(), 9_000u64)]));

        assert_eq!(cached_duration_ms("mxc://from-a"), 4_000);
        assert_eq!(cached_duration_ms("mxc://from-b"), 9_000);
        assert_eq!(cached_duration_ms("mxc://never-seen"), 0);
        reset();
    }
}

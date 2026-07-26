// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Remembered global user profiles (display name + avatar URL), keyed by user id.
//!
//! This is the memo behind the timeline's sender-name resolution, and it is
//! PERSISTED. Why it must be: a user who left a room and later deleted their account
//! answers 404 on `/profile` forever, and their name is in no member store we hold —
//! so the only way to keep showing the name they had is to remember one we resolved
//! while we still could. A process-lifetime memo re-collapses those senders to raw
//! MXIDs on every restart (and re-fetches everyone else from scratch).
//!
//! Only GLOBAL profiles belong here. A room-scoped name (`m.room.member`
//! displayname / prev_content, see `RoomMemberService::member_display_name`) can
//! differ per room and is already restored locally by the SDK's member store —
//! folding it in would show room A's nickname in room B.
//!
//! Storage is one JSON blob in the SDK state store, which is already
//! SQLCipher-encrypted and moved to the trash on logout, so this inherits both
//! encryption and the account wipe for free. The blob is rewritten whole on each new
//! user, hence [`MAX_ENTRIES`].

use std::collections::HashMap;
use std::sync::LazyLock;

use matrix_sdk::Client;
use serde::{Deserialize, Serialize};
use tokio::sync::RwLock;
use tracing::warn;

use crate::types::UserProfile;

/// Custom state-store key holding the `{ user_id: StoredProfile }` JSON blob.
const STORE_KEY: &[u8] = b"telematrix:user_profiles:v1";

/// The blob is rewritten whole whenever a new user is learned, so cap it. Eviction
/// drops the least-recently-learned entry.
const MAX_ENTRIES: usize = 8192;

/// A remembered profile. Both fields `None` encodes the negative case — the server
/// told us this user has no name and no avatar — so it isn't re-fetched every run.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub(crate) struct StoredProfile {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    display_name: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    avatar_url: Option<String>,
    /// Unix seconds when this was learned. Used to pick the eviction victim.
    #[serde(default)]
    updated: u64,
}

impl StoredProfile {
    fn is_absent(&self) -> bool {
        self.display_name.is_none() && self.avatar_url.is_none()
    }

    /// `None` when this records a real absence.
    fn to_profile(&self, user_id: &str) -> Option<UserProfile> {
        if self.is_absent() {
            return None;
        }
        Some(UserProfile {
            user_id: user_id.to_string(),
            // A user with only an avatar has no name to show, so fall back to the
            // MXID exactly as the live fetch path does.
            display_name: self
                .display_name
                .clone()
                .unwrap_or_else(|| user_id.to_string()),
            avatar_url: self.avatar_url.clone(),
        })
    }
}

/// Owner (logged-in account) user id -> that account's `{ user_id: profile }` memo.
///
/// A profile is what ONE account observed, not a content-global fact: two accounts
/// can see different names for the same user (different homeservers, federation
/// lag, a since-renamed user), so partitions are never shared. Multi-account keeps
/// several partitions live at once; each session only ever reads and persists its
/// own.
static CACHE: LazyLock<RwLock<HashMap<String, HashMap<String, StoredProfile>>>> =
    LazyLock::new(|| RwLock::new(HashMap::new()));

/// The cache partition a client owns. A client with no user id has no session, so
/// it gets the empty partition, which no live session ever writes.
pub(crate) fn owner_key(client: &Client) -> String {
    client
        .user_id()
        .map(|id| id.as_str().to_string())
        .unwrap_or_default()
}

fn now_secs() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

/// What we remember for `user_id`: `Some(Some(p))` a profile, `Some(None)` a known
/// absence, `None` never resolved. No network, no disk — this is the timeline render
/// path's lookup.
pub(crate) async fn lookup(owner: &str, user_id: &str) -> Option<Option<UserProfile>> {
    let guard = CACHE.read().await;
    guard
        .get(owner)?
        .get(user_id)
        .map(|stored| stored.to_profile(user_id))
}

/// Replace this account's memo from its own store. Call once per session start
/// (see `start_sync`) so remembered names are available before the first timeline
/// renders. Only this account's partition is touched, so a session starting while
/// another is live cannot evict the other's memo.
pub(crate) async fn load(client: &Client) {
    let map = match client.state_store().get_custom_value(STORE_KEY).await {
        Ok(Some(bytes)) => serde_json::from_slice::<HashMap<String, StoredProfile>>(&bytes)
            .unwrap_or_else(|e| {
                warn!("discarding unreadable persisted user profiles: {e}");
                HashMap::new()
            }),
        _ => HashMap::new(),
    };
    CACHE.write().await.insert(owner_key(client), map);
}

/// Drop one account's remembered profiles, in memory only. The persisted blob lives
/// in that account's state store, which logout trashes wholesale. Call on logout /
/// new session, so the next account on this context cannot serve the previous one's
/// names or inherit its cached absences.
pub(crate) async fn clear(owner: &str) {
    CACHE.write().await.remove(owner);
}

/// Remember a DEFINITIVELY resolved profile (a real answer, or a real absence) and
/// persist it. Never call this for a transient failure — see `global_profile`.
///
/// STICKY POSITIVES: once a name/avatar is remembered, a later absence must NOT erase
/// it. When a departed user deletes their account, `/profile` starts answering 404
/// forever; keeping the remembered name across that transition is the entire point of
/// this cache. Returns what the cache now holds, which is what the caller displays.
pub(crate) async fn remember(
    client: &Client,
    user_id: &str,
    display_name: Option<String>,
    avatar_url: Option<String>,
) -> Option<UserProfile> {
    let incoming_absent = display_name.is_none() && avatar_url.is_none();
    let (snapshot, result) = {
        let mut cache = CACHE.write().await;
        let guard = cache.entry(owner_key(client)).or_default();
        if let Some(existing) = guard.get(user_id) {
            if incoming_absent && !existing.is_absent() {
                // Sticky: keep the remembered name rather than record the absence.
                return existing.to_profile(user_id);
            }
        }
        if guard.len() >= MAX_ENTRIES && !guard.contains_key(user_id) {
            let victim = guard
                .iter()
                .min_by_key(|(_, stored)| stored.updated)
                .map(|(key, _)| key.clone());
            if let Some(victim) = victim {
                guard.remove(&victim);
            }
        }
        let entry = StoredProfile {
            display_name,
            avatar_url,
            updated: now_secs(),
        };
        let result = entry.to_profile(user_id);
        guard.insert(user_id.to_string(), entry);
        // Persist only this account's partition: its state store must never hold
        // users another account resolved.
        (guard.clone(), result)
    };
    match serde_json::to_vec(&snapshot) {
        Ok(bytes) => {
            if let Err(e) = client
                .state_store()
                .set_custom_value(STORE_KEY, bytes)
                .await
            {
                warn!("failed to persist user profiles: {e}");
            }
        }
        Err(e) => warn!("failed to serialize user profiles: {e}"),
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Every test names its own owner partition, so they never collide even though
    /// the cache is a process-wide static shared by the whole test binary.
    const OWNER: &str = "@me:x";

    async fn reset(owner: &str) {
        CACHE.write().await.remove(owner);
    }

    async fn insert(
        owner: &str,
        user_id: &str,
        display_name: Option<&str>,
        avatar_url: Option<&str>,
    ) {
        CACHE
            .write()
            .await
            .entry(owner.to_string())
            .or_default()
            .insert(
                user_id.to_string(),
                StoredProfile {
                    display_name: display_name.map(str::to_string),
                    avatar_url: avatar_url.map(str::to_string),
                    updated: now_secs(),
                },
            );
    }

    #[tokio::test]
    async fn lookup_distinguishes_absent_from_unknown() {
        let owner = "@absent-test:x";
        reset(owner).await;
        insert(owner, "@named:x", Some("Domy"), None).await;
        insert(owner, "@gone:x", None, None).await;

        // Known name.
        let named = lookup(owner, "@named:x")
            .await
            .expect("cached")
            .expect("profile");
        assert_eq!(named.display_name, "Domy");
        // Known absence: cached, but no profile.
        assert_eq!(lookup(owner, "@gone:x").await, Some(None));
        // Never resolved.
        assert!(lookup(owner, "@unknown:x").await.is_none());
        reset(owner).await;
    }

    #[tokio::test]
    async fn avatar_only_falls_back_to_mxid_for_the_name() {
        let owner = "@avatar-test:x";
        reset(owner).await;
        insert(owner, "@pic:x", None, Some("mxc://x/a")).await;
        let p = lookup(owner, "@pic:x")
            .await
            .expect("cached")
            .expect("profile");
        assert_eq!(p.display_name, "@pic:x");
        assert_eq!(p.avatar_url.as_deref(), Some("mxc://x/a"));
        reset(owner).await;
    }

    /// The reason this cache exists: a remembered name must survive the account
    /// being deleted (which turns /profile into a permanent 404 => an absence).
    #[tokio::test]
    async fn a_later_absence_never_erases_a_remembered_name() {
        let owner = "@sticky-test:x";
        reset(owner).await;
        insert(owner, "@admin:zupari", Some("Domy"), None).await;

        // Simulate remember()'s sticky branch without a Client: an incoming absence
        // must leave the stored positive untouched.
        let incoming_absent = true;
        let kept = {
            let cache = CACHE.read().await;
            let existing = cache
                .get(owner)
                .expect("partition")
                .get("@admin:zupari")
                .expect("stored");
            assert!(incoming_absent && !existing.is_absent());
            existing.to_profile("@admin:zupari")
        };
        assert_eq!(kept.expect("profile").display_name, "Domy");
        assert_eq!(
            lookup(owner, "@admin:zupari")
                .await
                .expect("cached")
                .expect("profile")
                .display_name,
            "Domy"
        );
        reset(owner).await;
    }

    #[tokio::test]
    async fn eviction_drops_the_least_recently_learned() {
        reset(OWNER).await;
        {
            let mut cache = CACHE.write().await;
            let guard = cache.entry(OWNER.to_string()).or_default();
            for i in 0..3 {
                guard.insert(
                    format!("@u{i}:x"),
                    StoredProfile {
                        display_name: Some(format!("n{i}")),
                        avatar_url: None,
                        updated: 100 + i as u64,
                    },
                );
            }
            let victim = guard
                .iter()
                .min_by_key(|(_, stored)| stored.updated)
                .map(|(key, _)| key.clone());
            assert_eq!(victim.as_deref(), Some("@u0:x"));
        }
        reset(OWNER).await;
    }

    /// Multi-account: two live sessions must never see each other's remembered
    /// names, and one signing out must not blank the other's memo.
    #[tokio::test]
    async fn partitions_isolate_accounts_and_survive_a_sibling_logout() {
        let (a, b) = ("@alice:x", "@bob:y");
        reset(a).await;
        reset(b).await;

        insert(a, "@shared:z", Some("Name Alice Sees"), None).await;
        insert(b, "@shared:z", Some("Name Bob Sees"), None).await;
        insert(a, "@only-alice:z", Some("Solo"), None).await;

        // Each account reads its own answer for the same user.
        let seen = |o: &'static str| async move {
            lookup(o, "@shared:z")
                .await
                .expect("cached")
                .expect("profile")
                .display_name
        };
        assert_eq!(seen(a).await, "Name Alice Sees");
        assert_eq!(seen(b).await, "Name Bob Sees");
        // A user only Alice resolved is invisible to Bob.
        assert!(lookup(b, "@only-alice:z").await.is_none());

        // Alice signs out: Bob keeps everything.
        clear(a).await;
        assert!(lookup(a, "@shared:z").await.is_none());
        assert_eq!(seen(b).await, "Name Bob Sees");

        reset(b).await;
    }
}

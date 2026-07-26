// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use anyhow::{anyhow, Result};
use matrix_sdk::Client;
use rand::{rngs::OsRng, RngCore};
use std::sync::{Arc, Mutex};
use zeroize::Zeroizing;

use crate::types::SessionInfo;

/// The local-cache secret namespace for an account: its data directory's own name
/// (e.g. `0`, `1` under `accounts/`). Derived from the path rather than the session
/// so it is known at context creation, before any login.
pub(crate) fn dir_namespace(data_dir: &std::path::Path) -> String {
    data_dir
        .file_name()
        .map(|name| name.to_string_lossy().into_owned())
        .unwrap_or_default()
}

#[derive(Clone, Default)]
pub(crate) struct SessionStorageService {
    // Zeroizing: scrub the stored passphrase from the heap when it is cleared/replaced.
    pending_auth_store_passphrase: Arc<Mutex<Option<Zeroizing<String>>>>,
}

impl SessionStorageService {
    pub(crate) fn new() -> Self {
        Self::default()
    }

    pub(crate) fn load_local_secret(dir_ns: &str, kind: &str) -> Option<String> {
        crate::keychain::load_secret(&Self::local_secret_key(dir_ns, kind))
            .ok()
            .flatten()
            .filter(|value| !value.is_empty())
    }

    pub(crate) fn load_required_local_secret(dir_ns: &str, kind: &str) -> Result<String> {
        Self::load_local_secret(dir_ns, kind)
            .ok_or_else(|| anyhow!("Missing secure local cache secret: {kind}"))
    }

    pub(crate) fn load_session_secret(
        kind: &str,
        homeserver: &str,
        user_id: &str,
        device_id: &str,
    ) -> Result<String> {
        let key = Self::session_secret_key(kind, homeserver, user_id, device_id);
        crate::keychain::load_secret(&key)?
            .filter(|value| !value.is_empty())
            .ok_or_else(|| anyhow!("Missing secure session secret: {kind}"))
    }

    pub(crate) fn search_index_key_material(client: &Client) -> Option<String> {
        let key = Self::client_session_secret_key("search_passphrase", client)?;
        crate::keychain::load_secret(&key).ok().flatten()
    }

    pub(crate) fn current_session_info(client: &Client) -> Result<SessionInfo> {
        let session = client
            .matrix_auth()
            .session()
            .ok_or_else(|| anyhow!("No active session"))?;

        Ok(SessionInfo {
            homeserver: client.homeserver().to_string(),
            user_id: session.meta.user_id.to_string(),
            device_id: session.meta.device_id.to_string(),
            access_token: session.tokens.access_token.clone(),
        })
    }

    /// The six keychain keys an account owns: three session-identity-scoped
    /// (token / SDK-store / search passphrase) plus three data-dir-scoped local-cache
    /// passphrases. Session keys are omitted when there is no active session (restore
    /// failed) — only the local-cache keys, which are known from the dir namespace, remain.
    pub(crate) fn account_secret_keys(session: Option<&SessionInfo>, dir_ns: &str) -> Vec<String> {
        let mut keys = Vec::with_capacity(6);
        if let Some(s) = session {
            for kind in [
                "session_access_token",
                "sdk_store_passphrase",
                "search_passphrase",
            ] {
                keys.push(Self::session_secret_key(
                    kind,
                    &s.homeserver,
                    &s.user_id,
                    &s.device_id,
                ));
            }
        }
        for kind in [
            "app_cache_passphrase",
            "preview_cache_passphrase",
            "media_cache_passphrase",
        ] {
            keys.push(Self::local_secret_key(dir_ns, kind));
        }
        keys
    }

    /// Delete exactly this account's secrets (see `account_secret_keys`), leaving
    /// every other signed-in account's secrets intact. The session identity must be
    /// captured before the client is torn down; the local-cache namespace comes from
    /// the data dir (known even with no session). See code-review-2026-07-19 MA-1.
    pub(crate) fn delete_account_secrets(
        session: Option<&SessionInfo>,
        data_dir: &std::path::Path,
    ) -> Result<()> {
        let keys = Self::account_secret_keys(session, &dir_namespace(data_dir));
        crate::keychain::delete_secrets(&keys)
    }

    pub(crate) fn pending_auth_store_passphrase(&self) -> Result<String> {
        let mut guard = self
            .pending_auth_store_passphrase
            .lock()
            .map_err(|_| anyhow!("Pending auth store passphrase lock poisoned"))?;
        if let Some(passphrase) = guard.as_ref() {
            return Ok(passphrase.as_str().to_owned());
        }

        let passphrase = Self::generate_local_secret();
        *guard = Some(Zeroizing::new(passphrase.clone()));
        Ok(passphrase)
    }

    pub(crate) fn clear_pending_auth_store_passphrase(&self) {
        if let Ok(mut guard) = self.pending_auth_store_passphrase.lock() {
            *guard = None;
        }
    }

    pub(crate) fn commit_session_secrets(
        &self,
        client: &Client,
        store_passphrase: &str,
        dir_ns: &str,
    ) -> Result<(String, String, String)> {
        let session = client
            .matrix_auth()
            .session()
            .ok_or_else(|| anyhow!("No active session"))?;

        let homeserver = client.homeserver().to_string();
        let user_id = session.meta.user_id.to_string();
        let device_id = session.meta.device_id.to_string();
        let token = session.tokens.access_token.clone();

        let search_passphrase = Self::generate_local_secret();
        let app_cache_passphrase = Self::generate_local_secret();
        let preview_cache_passphrase = Self::generate_local_secret();
        let media_cache_passphrase = Self::generate_local_secret();

        let token_key =
            Self::session_secret_key("session_access_token", &homeserver, &user_id, &device_id);
        let store_key =
            Self::session_secret_key("sdk_store_passphrase", &homeserver, &user_id, &device_id);
        let search_key =
            Self::session_secret_key("search_passphrase", &homeserver, &user_id, &device_id);
        let app_cache_key = Self::local_secret_key(dir_ns, "app_cache_passphrase");
        let preview_cache_key = Self::local_secret_key(dir_ns, "preview_cache_passphrase");
        let media_cache_key = Self::local_secret_key(dir_ns, "media_cache_passphrase");

        crate::keychain::store_secrets(&[
            (token_key.as_str(), token.as_str()),
            (store_key.as_str(), store_passphrase),
            (search_key.as_str(), search_passphrase.as_str()),
            (app_cache_key.as_str(), app_cache_passphrase.as_str()),
            (
                preview_cache_key.as_str(),
                preview_cache_passphrase.as_str(),
            ),
            (media_cache_key.as_str(), media_cache_passphrase.as_str()),
        ])?;

        Ok((
            app_cache_passphrase,
            preview_cache_passphrase,
            media_cache_passphrase,
        ))
    }

    fn session_secret_key(kind: &str, homeserver: &str, user_id: &str, device_id: &str) -> String {
        format!("v1|{kind}|{homeserver}|{user_id}|{device_id}")
    }

    /// Key for a local-cache passphrase (app cache / preview cache / media cache).
    ///
    /// Namespaced by the account's data-directory name rather than its session
    /// identity, because these stores are opened at context creation — before any
    /// session exists to name them. Every account therefore encrypts its own local
    /// caches with its own passphrase.
    fn local_secret_key(dir_ns: &str, kind: &str) -> String {
        format!("v1|local_cache|{dir_ns}|{kind}")
    }

    fn client_session_secret_key(kind: &str, client: &Client) -> Option<String> {
        let session = client.matrix_auth().session()?;
        let homeserver = client.homeserver().to_string();
        Some(Self::session_secret_key(
            kind,
            &homeserver,
            session.meta.user_id.as_str(),
            session.meta.device_id.as_str(),
        ))
    }

    fn generate_local_secret() -> String {
        let mut bytes = [0u8; 32];
        OsRng.fill_bytes(&mut bytes);
        bytes.iter().map(|byte| format!("{byte:02x}")).collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::Path;

    #[test]
    fn dir_namespace_is_the_account_directory_name() {
        assert_eq!(dir_namespace(Path::new("/data/TeleMatrix/accounts/0")), "0");
        assert_eq!(
            dir_namespace(Path::new("/data/TeleMatrix/accounts/12")),
            "12"
        );
        // No file name (root) degrades to the empty namespace rather than panicking.
        assert_eq!(dir_namespace(Path::new("/")), "");
    }

    /// Two accounts must never share a local-cache passphrase key, or the second
    /// would overwrite the first's and make its encrypted caches unreadable.
    #[test]
    fn local_cache_keys_are_distinct_per_account() {
        let a = SessionStorageService::local_secret_key("0", "app_cache_passphrase");
        let b = SessionStorageService::local_secret_key("1", "app_cache_passphrase");
        assert_eq!(a, "v1|local_cache|0|app_cache_passphrase");
        assert_ne!(a, b);
        // ...and the three kinds stay distinct within one account.
        assert_ne!(
            SessionStorageService::local_secret_key("0", "app_cache_passphrase"),
            SessionStorageService::local_secret_key("0", "media_cache_passphrase")
        );
    }

    /// Logout must delete exactly the departing account's six keys — no more (which
    /// would wipe a sibling account) and no fewer. Locks the MA-1 fix.
    #[test]
    fn account_secret_keys_are_exactly_the_six_scoped_keys() {
        let session = SessionInfo {
            homeserver: "https://a.example".into(),
            user_id: "@alice:a.example".into(),
            device_id: "DEV1".into(),
            access_token: "t".into(),
        };
        let keys = SessionStorageService::account_secret_keys(Some(&session), "0");
        assert_eq!(
            keys,
            vec![
                "v1|session_access_token|https://a.example|@alice:a.example|DEV1".to_string(),
                "v1|sdk_store_passphrase|https://a.example|@alice:a.example|DEV1".to_string(),
                "v1|search_passphrase|https://a.example|@alice:a.example|DEV1".to_string(),
                "v1|local_cache|0|app_cache_passphrase".to_string(),
                "v1|local_cache|0|preview_cache_passphrase".to_string(),
                "v1|local_cache|0|media_cache_passphrase".to_string(),
            ]
        );
        // No session (restore failed): only the dir-namespaced local-cache keys remain,
        // and they belong to THIS account's dir, so no sibling is touched.
        let local_only = SessionStorageService::account_secret_keys(None, "0");
        assert_eq!(local_only.len(), 3);
        assert!(local_only
            .iter()
            .all(|k| k.starts_with("v1|local_cache|0|")));
        // A different account's dir namespace yields entirely different keys.
        let sibling = SessionStorageService::account_secret_keys(None, "1");
        assert!(sibling.iter().all(|k| !local_only.contains(k)));
    }

    /// Session secrets were already account-namespaced; lock the format so a
    /// second account's token can never collide with the first's.
    #[test]
    fn session_keys_include_the_full_session_identity() {
        let a = SessionStorageService::session_secret_key(
            "session_access_token",
            "https://a.example",
            "@alice:a.example",
            "DEV1",
        );
        let b = SessionStorageService::session_secret_key(
            "session_access_token",
            "https://b.example",
            "@bob:b.example",
            "DEV2",
        );
        assert_eq!(
            a,
            "v1|session_access_token|https://a.example|@alice:a.example|DEV1"
        );
        assert_ne!(a, b);
    }
}

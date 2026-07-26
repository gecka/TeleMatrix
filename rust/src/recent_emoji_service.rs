// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Server-synced recent emojis. Source of truth is the user's global account
//! data (`io.element.recent_emoji`); a mirror is cached in the encrypted
//! `app_cache.db` (`AppCacheStore`) for instant startup + offline, wiped on
//! logout. Mirrors `FolderService`.

use std::sync::{Arc, Mutex};

use anyhow::{anyhow, Result};
use matrix_sdk::ruma::api::client::config::get_global_account_data;
use matrix_sdk::ruma::events::{AnyGlobalAccountDataEventContent, GlobalAccountDataEventType};
use matrix_sdk::ruma::serde::Raw;
use matrix_sdk::Client;
use tokio::sync::RwLock;
use tracing::warn;

use crate::app_cache_store::AppCacheStore;
use crate::recent_emoji::{RecentEmojiPayload, RECENT_EMOJI_EVENT_TYPE};

#[derive(Clone)]
pub(crate) struct RecentEmojiService {
    cache: Arc<RwLock<Vec<(String, u32)>>>,
    app_cache_store: Arc<Mutex<Option<AppCacheStore>>>,
}

impl RecentEmojiService {
    pub(crate) fn new(app_cache_store: Arc<Mutex<Option<AppCacheStore>>>) -> Self {
        Self {
            cache: Arc::new(RwLock::new(Vec::new())),
            app_cache_store,
        }
    }

    /// Local (app_cache.db) snapshot for instant startup; never hits the network.
    pub(crate) fn local(&self) -> Vec<(String, u32)> {
        if let Ok(guard) = self.app_cache_store.lock() {
            if let Some(store) = guard.as_ref() {
                return store.load_recent_emoji().unwrap_or_default();
            }
        }
        Vec::new()
    }

    async fn load_from_server(client: &Client) -> Result<Option<Vec<(String, u32)>>> {
        let event_type = GlobalAccountDataEventType::from(RECENT_EMOJI_EVENT_TYPE);
        let own_user = client
            .user_id()
            .ok_or_else(|| anyhow!("Not logged in"))?
            .to_owned();
        let request = get_global_account_data::v3::Request::new(own_user, event_type);
        match client.send(request).await {
            Ok(response) => {
                let payload: RecentEmojiPayload =
                    serde_json::from_str(response.account_data.json().get())
                        .map_err(|e| anyhow!("Failed to parse recent emoji: {e}"))?;
                Ok(Some(payload.recent_emoji))
            }
            Err(e) => {
                // 404 (M_NOT_FOUND) just means no recents stored yet.
                let s = e.to_string();
                if s.contains("M_NOT_FOUND") || s.contains("404") {
                    Ok(None)
                } else {
                    Err(anyhow!("Failed to fetch recent emoji: {e}"))
                }
            }
        }
    }

    /// Fetch from the server once (startup) and reconcile the local cache.
    /// Returns the list to push to the UI when the server has non-empty data.
    pub(crate) async fn hydrate_from_server(&self, client: &Client) -> Option<Vec<(String, u32)>> {
        match Self::load_from_server(client).await {
            Ok(Some(pairs)) if !pairs.is_empty() => {
                self.store_local(&pairs).await;
                Some(pairs)
            }
            // No server data yet (or empty): keep whatever is local/default.
            Ok(_) => None,
            Err(e) => {
                warn!("[recent-emoji] server load failed: {e}");
                None
            }
        }
    }

    /// Persist a new list (server + local cache). Called when the user picks.
    pub(crate) async fn save(&self, client: &Client, pairs: Vec<(String, u32)>) -> Result<()> {
        self.store_local(&pairs).await;
        let payload = RecentEmojiPayload {
            recent_emoji: pairs,
        };
        let raw: Raw<AnyGlobalAccountDataEventContent> = Raw::from_json(
            serde_json::value::to_raw_value(&payload)
                .map_err(|e| anyhow!("Failed to serialize recent emoji: {e}"))?,
        );
        let event_type = GlobalAccountDataEventType::from(RECENT_EMOJI_EVENT_TYPE);
        client
            .account()
            .set_account_data_raw(event_type, raw)
            .await
            .map_err(|e| anyhow!("Failed to save recent emoji: {e}"))?;
        Ok(())
    }

    /// Apply an incoming account-data event (live sync / cross-device). Returns the
    /// parsed list when non-empty so the caller can push it to the UI.
    pub(crate) async fn apply_sync_json(&self, json: &str) -> Option<Vec<(String, u32)>> {
        let wrapper: serde_json::Value = serde_json::from_str(json).ok()?;
        let content = wrapper.get("content")?;
        let payload: RecentEmojiPayload = serde_json::from_value(content.clone()).ok()?;
        if payload.recent_emoji.is_empty() {
            return None;
        }
        self.store_local(&payload.recent_emoji).await;
        Some(payload.recent_emoji)
    }

    async fn store_local(&self, pairs: &[(String, u32)]) {
        {
            let mut cache = self.cache.write().await;
            *cache = pairs.to_vec();
        }
        if let Ok(guard) = self.app_cache_store.lock() {
            if let Some(store) = guard.as_ref() {
                let _ = store.save_recent_emoji(pairs);
            }
        }
    }
}

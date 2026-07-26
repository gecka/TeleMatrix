// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::collections::HashMap;
use std::sync::{Arc, Mutex};

use tokio::sync::RwLock;
use tracing::{info, warn};

use crate::types::{RoomNotificationMode, RoomSummary};

type RoomListCallback = Box<dyn Fn() + Send>;
pub(crate) type RoomListCallbackSlot = Arc<Mutex<Option<RoomListCallback>>>;

#[derive(Clone)]
pub(crate) struct RoomListService {
    pub(crate) rooms: Arc<RwLock<Vec<RoomSummary>>>,
    pub(crate) callback: RoomListCallbackSlot,
    /// Local notification mode overrides (room_id -> mode).
    ///
    /// Set when we bypass the SDK's push rule API; cleared on next sync when
    /// the SDK's cache catches up with the server state.
    pub(crate) notification_overrides: Arc<RwLock<HashMap<String, RoomNotificationMode>>>,
}

impl RoomListService {
    pub(crate) fn new() -> Self {
        Self {
            rooms: Arc::new(RwLock::new(Vec::new())),
            callback: Arc::new(Mutex::new(None)),
            notification_overrides: Arc::new(RwLock::new(HashMap::new())),
        }
    }

    pub(crate) async fn current_rooms_or_cached(
        &self,
        app_cache_store: Arc<Mutex<Option<crate::app_cache_store::AppCacheStore>>>,
    ) -> Vec<RoomSummary> {
        let cache = self.rooms.read().await;
        if !cache.is_empty() {
            return cache.clone();
        }
        drop(cache);

        // Off the async worker: load_rooms is a synchronous SQLCipher decrypt+query,
        // and running it on a tokio worker stalls the executor (the 570ms-yield class).
        // See PERF-7 / [[sync-sqlite-on-async-workers]].
        tokio::task::spawn_blocking(move || {
            let Ok(guard) = app_cache_store.lock() else {
                warn!("get_rooms: app cache store lock poisoned");
                return Vec::new();
            };
            let Some(store) = guard.as_ref() else {
                warn!("get_rooms: no rooms in memory and app cache store not open");
                return Vec::new();
            };
            match store.load_rooms() {
                Ok(rooms) => {
                    info!(
                        "get_rooms: memory cache empty; loaded {} rooms from disk snapshot",
                        rooms.len()
                    );
                    rooms
                }
                Err(e) => {
                    warn!("get_rooms: failed to load rooms snapshot from disk: {e}");
                    Vec::new()
                }
            }
        })
        .await
        .unwrap_or_else(|e| {
            warn!("get_rooms: snapshot load task failed: {e}");
            Vec::new()
        })
    }
}

impl Default for RoomListService {
    fn default() -> Self {
        Self::new()
    }
}

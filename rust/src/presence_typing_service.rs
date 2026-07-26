// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::collections::HashMap;
use std::sync::{Arc, Mutex, MutexGuard};

use anyhow::{anyhow, Result};
use matrix_sdk::ruma::OwnedRoomId;
use matrix_sdk::{Client, Room};
use tracing::warn;

use crate::session_task_service::SessionTaskService;
use crate::types::PresenceState;

pub(crate) type PresenceCallback = Box<dyn Fn(&str, u32, u64) + Send>;
pub(crate) type TypingCallback = Box<dyn Fn(&str, Vec<String>) + Send>;

fn lock_presence_mutex<'a, T>(mutex: &'a Mutex<T>, name: &str) -> MutexGuard<'a, T> {
    match mutex.lock() {
        Ok(guard) => guard,
        Err(poisoned) => {
            warn!("Recovering poisoned presence/typing mutex: {name}");
            poisoned.into_inner()
        }
    }
}

#[derive(Clone)]
pub(crate) struct PresenceTypingService {
    cache: Arc<std::sync::RwLock<HashMap<String, (u32, u64)>>>,
    presence_callback: Arc<Mutex<Option<PresenceCallback>>>,
    typing_callback: Arc<Mutex<Option<TypingCallback>>>,
}

impl PresenceTypingService {
    pub(crate) fn new() -> Self {
        Self {
            cache: Arc::new(std::sync::RwLock::new(HashMap::new())),
            presence_callback: Arc::new(Mutex::new(None)),
            typing_callback: Arc::new(Mutex::new(None)),
        }
    }

    pub(crate) fn on_presence_changed(&self, callback: PresenceCallback) {
        let mut cb = lock_presence_mutex(&self.presence_callback, "presence_callback");
        *cb = Some(callback);
    }

    pub(crate) fn on_typing_changed(&self, callback: TypingCallback) {
        let mut cb = lock_presence_mutex(&self.typing_callback, "typing_callback");
        *cb = Some(callback);
    }

    pub(crate) fn clear_callbacks(&self) {
        {
            let mut cb = lock_presence_mutex(&self.presence_callback, "presence_callback");
            *cb = None;
        }
        {
            let mut cb = lock_presence_mutex(&self.typing_callback, "typing_callback");
            *cb = None;
        }
    }

    pub(crate) fn presence_snapshot(&self) -> HashMap<String, u32> {
        self.cache
            .read()
            .map(|cache| {
                cache
                    .iter()
                    .map(|(user_id, (state, _))| (user_id.clone(), *state))
                    .collect()
            })
            .unwrap_or_default()
    }

    pub(crate) fn member_presence(&self, user_id: &str) -> (PresenceState, u64) {
        self.cache
            .read()
            .ok()
            .and_then(|cache| cache.get(user_id).copied())
            .map(|(state, ts)| {
                let presence = match state {
                    1 => PresenceState::Online,
                    2 => PresenceState::Unavailable,
                    _ => PresenceState::Offline,
                };
                (presence, ts)
            })
            .unwrap_or((PresenceState::Offline, 0))
    }

    pub(crate) async fn send_typing(
        &self,
        client: Client,
        room_id: &str,
        typing: bool,
    ) -> Result<()> {
        let rid: OwnedRoomId = room_id
            .try_into()
            .map_err(|_| anyhow!("Invalid room ID: {room_id}"))?;
        let Some(room) = client.get_room(&rid) else {
            return Err(anyhow!("Room not found: {room_id}"));
        };
        room.typing_notice(typing).await?;
        Ok(())
    }

    pub(crate) fn subscribe_room_typing_notifications(
        &self,
        room: &Room,
        room_id: String,
        session_tasks: &SessionTaskService,
    ) {
        let typing_callback = self.typing_callback.clone();
        let (guard, mut rx) = room.subscribe_to_typing_notifications();
        session_tasks.spawn(async move {
            let _guard = guard;
            while let Ok(users) = rx.recv().await {
                let names: Vec<String> = users.into_iter().map(|user| user.to_string()).collect();
                let cb = lock_presence_mutex(&typing_callback, "typing_callback");
                if let Some(callback) = cb.as_ref() {
                    callback(&room_id, names);
                }
            }
        });
    }
}

impl Default for PresenceTypingService {
    fn default() -> Self {
        Self::new()
    }
}

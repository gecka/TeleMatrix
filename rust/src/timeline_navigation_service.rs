// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::collections::HashMap;
use std::sync::Arc;

use anyhow::{anyhow, Result};
use matrix_sdk::Room;
use tokio::sync::RwLock;

use crate::timeline_window::TimelineWindow;

#[derive(Clone)]
pub(crate) struct TimelineNavigationService {
    windows: Arc<RwLock<HashMap<String, TimelineWindow>>>,
    focus_generations: Arc<RwLock<HashMap<String, u64>>>,
}

impl TimelineNavigationService {
    pub(crate) fn new(windows: Arc<RwLock<HashMap<String, TimelineWindow>>>) -> Self {
        Self {
            windows,
            focus_generations: Arc::new(RwLock::new(HashMap::new())),
        }
    }

    pub(crate) async fn begin_focus_request(&self, room_id: &str) -> u64 {
        let mut generations = self.focus_generations.write().await;
        let entry = generations.entry(room_id.to_string()).or_insert(0);
        *entry = entry.saturating_add(1);
        *entry
    }

    pub(crate) async fn window_missing(&self, room_id: &str) -> bool {
        let windows = self.windows.read().await;
        !windows.contains_key(room_id)
    }

    pub(crate) fn paginate_back(&self, room_id: &str, count: u16) {
        let windows = self.windows.clone();
        let room_id = room_id.to_string();
        tokio::spawn(async move {
            let windows = windows.read().await;
            if let Some(window) = windows.get(&room_id) {
                window.paginate_back(count);
            }
        });
    }

    pub(crate) fn paginate_forward(&self, room_id: &str, count: u16) {
        let windows = self.windows.clone();
        let room_id = room_id.to_string();
        tokio::spawn(async move {
            let windows = windows.read().await;
            if let Some(window) = windows.get(&room_id) {
                window.paginate_forward(count);
            }
        });
    }

    pub(crate) async fn focus_on_event(
        &self,
        room_id: &str,
        event_id: &str,
        generation: u64,
        room: &Room,
    ) -> Result<()> {
        let (target_id, focused) = TimelineWindow::build_focused_timeline(room, event_id).await?;

        {
            let generations = self.focus_generations.read().await;
            if generations.get(room_id).copied() != Some(generation) {
                return Ok(());
            }
        }

        let mut windows = self.windows.write().await;
        let window = windows
            .get_mut(room_id)
            .ok_or_else(|| anyhow!("Timeline window missing for focused jump: {room_id}"))?;
        window.apply_focused_timeline(target_id, focused);
        Ok(())
    }

    pub(crate) fn return_to_live(&self, room_id: &str) {
        let windows = self.windows.clone();
        let room_id = room_id.to_string();
        tokio::spawn(async move {
            let mut windows = windows.write().await;
            if let Some(window) = windows.get_mut(&room_id) {
                window.return_to_live();
            }
        });
    }
}

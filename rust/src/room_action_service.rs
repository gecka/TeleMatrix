// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::collections::HashMap;
use std::sync::Arc;

use anyhow::{anyhow, Result};
use matrix_sdk::ruma::api::client::push::{get_pushrules_all, set_pushrule, set_pushrule_enabled};
use matrix_sdk::ruma::api::client::receipt::create_receipt::v3::ReceiptType;
use matrix_sdk::ruma::events::receipt::ReceiptThread;
use matrix_sdk::ruma::push::{
    Action, EventMatchConditionData, NewConditionalPushRule, NewPushRule, NewSimplePushRule,
    PushCondition, RuleKind,
};
use matrix_sdk::ruma::OwnedRoomId;
use matrix_sdk::{Client, Room};
use tokio::sync::RwLock;

use crate::local_cache_service::LocalCacheService;
use crate::types::{RoomNotificationMode, RoomSummary};

pub(crate) struct RoomActionService;

impl RoomActionService {
    /// Evenly spaced `m.tag` order values for `count` pinned rooms, topmost first.
    /// The spec puts `order` in [0,1] and sorts ascending, so the whole list is
    /// rewritten on every change rather than squeezing new values into the gaps —
    /// with a chat-list-sized list that costs nothing, and it can't run out of
    /// room between two neighbours after enough drags.
    pub(crate) fn pinned_tag_order(index: usize, count: usize) -> f64 {
        (index + 1) as f64 / (count + 1) as f64
    }

    pub(crate) async fn pin_room(
        room: Room,
        client: Option<Client>,
        pinned: bool,
        order: Option<f64>,
    ) -> Result<()> {
        if pinned {
            room.set_is_favourite(true, order).await?;
        } else {
            let client = client.ok_or_else(|| anyhow!("Not logged in"))?;
            let token = client
                .access_token()
                .ok_or_else(|| anyhow!("Not logged in"))?;
            let user_id = client.user_id().ok_or_else(|| anyhow!("No user ID"))?;
            let url = format!(
                "{}/_matrix/client/v3/user/{}/rooms/{}/tags/m.favourite",
                client.homeserver().as_str().trim_end_matches('/'),
                Self::url_encode(user_id.as_str()),
                Self::url_encode(room.room_id().as_str()),
            );
            let resp = reqwest::Client::new()
                .delete(&url)
                .bearer_auth(&token)
                .send()
                .await
                .map_err(|e| anyhow!("Failed to delete favourite tag: {e}"))?;
            if !resp.status().is_success() {
                return Err(anyhow!(
                    "Failed to delete favourite tag: HTTP {}",
                    resp.status()
                ));
            }
        }
        Ok(())
    }

    /// Rewrite the `order` on every pinned room's `m.favourite` tag so the list reads
    /// back in exactly this sequence — on the next login, and on every other device.
    /// `room_ids` is the pinned list top-first.
    pub(crate) async fn set_pinned_order(client: Client, room_ids: Vec<String>) -> Result<()> {
        let count = room_ids.len();
        for (index, room_id) in room_ids.iter().enumerate() {
            let parsed: OwnedRoomId = room_id
                .as_str()
                .try_into()
                .map_err(|_| anyhow!("Invalid room ID: {room_id}"))?;
            let Some(room) = client.get_room(&parsed) else {
                // Left/forgotten since the list was built: skip it rather than fail
                // the whole reorder and leave the rest of the list unwritten.
                continue;
            };
            room.set_is_favourite(true, Some(Self::pinned_tag_order(index, count)))
                .await?;
        }
        Ok(())
    }

    pub(crate) async fn set_room_notification_mode(
        client: Client,
        room: Room,
        room_id: &str,
        mode: RoomNotificationMode,
        notification_overrides: Arc<RwLock<HashMap<String, RoomNotificationMode>>>,
        rooms_cache: Arc<RwLock<Vec<RoomSummary>>>,
        local_cache: LocalCacheService,
    ) -> Result<()> {
        // Bypass the SDK's high-level notification settings API entirely.
        // The SDK bundles INSERT + DELETE push rule commands atomically, but
        // the homeserver returns 413 on DELETE push rule requests, causing all
        // operations to fail. Instead, use raw PUT requests and never DELETE.
        let rid = room.room_id().to_string();

        let resp = client
            .send(get_pushrules_all::v3::Request::new())
            .await
            .map_err(|e| anyhow!("Failed to fetch push rules: {e}"))?;

        let has_override = resp.global.override_.iter().any(|r| r.rule_id == rid);
        let has_room = resp.global.room.iter().any(|r| r.rule_id == rid);

        // A PUT to /pushrules updates a rule's conditions/actions but NOT its
        // `enabled` flag — the homeserver tracks that separately. So re-creating a
        // rule that a previous mode change left disabled keeps it disabled, and the
        // SDK ignores disabled rules when resolving a room's mode. After each PUT we
        // therefore explicitly (re-)enable the rule we just wrote. This is the fix
        // for "mute doesn't survive restart": muting re-wrote the override rule but
        // left it disabled from a prior unmute, so on restart the SDK read the room
        // as AllMessages.
        match mode {
            RoomNotificationMode::AllMessages => {
                let new_rule = NewSimplePushRule::new(
                    room.room_id().to_owned(),
                    vec![
                        Action::Notify,
                        Action::SetTweak(matrix_sdk::ruma::push::Tweak::Sound("default".into())),
                    ],
                );
                let req = set_pushrule::v3::Request::new(NewPushRule::Room(new_rule));
                client
                    .send(req)
                    .await
                    .map_err(|e| anyhow!("Failed to set room rule: {e}"))?;
                let req = set_pushrule_enabled::v3::Request::enable(RuleKind::Room, rid.clone());
                client
                    .send(req)
                    .await
                    .map_err(|e| anyhow!("Failed to enable room rule: {e}"))?;
                if has_override {
                    let req =
                        set_pushrule_enabled::v3::Request::disable(RuleKind::Override, rid.clone());
                    client
                        .send(req)
                        .await
                        .map_err(|e| anyhow!("Failed to disable override rule: {e}"))?;
                }
            }
            RoomNotificationMode::MentionsOnly => {
                let new_rule =
                    NewSimplePushRule::new(room.room_id().to_owned(), Vec::<Action>::new());
                let req = set_pushrule::v3::Request::new(NewPushRule::Room(new_rule));
                client
                    .send(req)
                    .await
                    .map_err(|e| anyhow!("Failed to set room rule: {e}"))?;
                let req = set_pushrule_enabled::v3::Request::enable(RuleKind::Room, rid.clone());
                client
                    .send(req)
                    .await
                    .map_err(|e| anyhow!("Failed to enable room rule: {e}"))?;
                if has_override {
                    let req =
                        set_pushrule_enabled::v3::Request::disable(RuleKind::Override, rid.clone());
                    client
                        .send(req)
                        .await
                        .map_err(|e| anyhow!("Failed to disable override rule: {e}"))?;
                }
            }
            RoomNotificationMode::Mute => {
                let new_rule = NewConditionalPushRule::new(
                    rid.clone(),
                    vec![PushCondition::EventMatch(EventMatchConditionData::new(
                        "room_id".to_owned(),
                        rid.clone(),
                    ))],
                    Vec::<Action>::new(),
                );
                let req = set_pushrule::v3::Request::new(NewPushRule::Override(new_rule));
                client
                    .send(req)
                    .await
                    .map_err(|e| anyhow!("Failed to set override rule: {e}"))?;
                let req =
                    set_pushrule_enabled::v3::Request::enable(RuleKind::Override, rid.clone());
                client
                    .send(req)
                    .await
                    .map_err(|e| anyhow!("Failed to enable override rule: {e}"))?;
                if has_room {
                    let req =
                        set_pushrule_enabled::v3::Request::disable(RuleKind::Room, rid.clone());
                    client
                        .send(req)
                        .await
                        .map_err(|e| anyhow!("Failed to disable room rule: {e}"))?;
                }
            }
        }

        notification_overrides
            .write()
            .await
            .insert(room_id.to_string(), mode);

        {
            let is_muted = mode == RoomNotificationMode::Mute;
            let changed = {
                let mut cache = rooms_cache.write().await;
                if let Some(room) = cache.iter_mut().find(|r| r.room_id == room_id) {
                    room.notification_mode = mode;
                    room.is_muted = is_muted;
                    true
                } else {
                    false
                }
            };
            if changed {
                local_cache.schedule_rooms_snapshot();
            }
        }

        Ok(())
    }

    pub(crate) async fn mark_room_read(
        room: Room,
        room_id: &str,
        read: bool,
        rooms_cache: Arc<RwLock<Vec<RoomSummary>>>,
        local_cache: LocalCacheService,
    ) -> Result<()> {
        if !read {
            room.set_unread_flag(true).await?;
            let changed = {
                let mut cache = rooms_cache.write().await;
                if let Some(summary) = cache.iter_mut().find(|room| room.room_id == room_id) {
                    summary.is_marked_unread = true;
                    true
                } else {
                    false
                }
            };
            if changed {
                local_cache.schedule_rooms_snapshot();
            }
            return Ok(());
        }

        let latest_event_id = room
            .latest_event()
            .event_id()
            .map(|event_id| event_id.to_string());
        if let Some(event_id) = latest_event_id.as_deref() {
            let event_id = matrix_sdk::ruma::OwnedEventId::try_from(event_id)?;
            room.send_single_receipt(
                ReceiptType::ReadPrivate,
                ReceiptThread::Unthreaded,
                event_id,
            )
            .await?;
        }

        room.set_unread_flag(false).await?;
        let changed = {
            let mut cache = rooms_cache.write().await;
            if let Some(summary) = cache.iter_mut().find(|room| room.room_id == room_id) {
                summary.unread_count = 0;
                summary.highlight_count = 0;
                summary.is_marked_unread = false;
                true
            } else {
                false
            }
        };
        if changed {
            local_cache.schedule_rooms_snapshot();
        }
        Ok(())
    }

    pub(crate) async fn send_read_receipt(room: Room, event_id: &str) -> Result<()> {
        let event_id = matrix_sdk::ruma::OwnedEventId::try_from(event_id)?;
        room.send_single_receipt(
            ReceiptType::ReadPrivate,
            ReceiptThread::Unthreaded,
            event_id,
        )
        .await?;
        Ok(())
    }

    pub(crate) async fn upload_room_avatar(
        client: Client,
        room: Room,
        room_id: &str,
        data: Vec<u8>,
        content_type: &str,
    ) -> Result<String> {
        let mime: mime::Mime = content_type
            .parse()
            .unwrap_or(mime::APPLICATION_OCTET_STREAM);
        let response = client
            .media()
            .upload(&mime, data, None)
            .await
            .map_err(|e| anyhow!("Failed to upload room avatar for {room_id}: {e}"))?;
        let mxc_url = response.content_uri.to_string();

        use matrix_sdk::ruma::events::room::avatar::RoomAvatarEventContent;
        let mut avatar_content = RoomAvatarEventContent::new();
        avatar_content.url = Some(response.content_uri);
        room.send_state_event(avatar_content)
            .await
            .map_err(|e| anyhow!("Failed to send room avatar event for {room_id}: {e}"))?;

        Ok(mxc_url)
    }

    pub(crate) async fn set_room_name(room: Room, room_id: &str, name: &str) -> Result<()> {
        room.set_name(name.to_owned())
            .await
            .map_err(|e| anyhow!("Failed to set room name for {room_id}: {e}"))?;
        Ok(())
    }

    pub(crate) async fn set_room_topic(room: Room, room_id: &str, topic: &str) -> Result<()> {
        use matrix_sdk::ruma::events::room::topic::RoomTopicEventContent;
        room.send_state_event(RoomTopicEventContent::new(topic.to_owned()))
            .await
            .map_err(|e| anyhow!("Failed to set room topic for {room_id}: {e}"))?;
        Ok(())
    }

    pub(crate) async fn delete_room_avatar(room: Room, room_id: &str) -> Result<()> {
        use matrix_sdk::ruma::events::room::avatar::RoomAvatarEventContent;
        let avatar_content = RoomAvatarEventContent::new();
        room.send_state_event(avatar_content)
            .await
            .map_err(|e| anyhow!("Failed to delete room avatar for {room_id}: {e}"))?;

        Ok(())
    }

    pub(crate) async fn leave_room(
        room: Room,
        room_id: &str,
        rooms_cache: Arc<RwLock<Vec<RoomSummary>>>,
        local_cache: LocalCacheService,
    ) -> Result<()> {
        room.leave().await?;
        // The Invited→Left (or Joined→Left) transition reaches the room-list stream as an
        // index-based `Remove` diff, which the incremental refresh deliberately discards — so
        // nothing else evicts this room. Without the explicit eviction here a declined invite
        // sticks in the list and is re-persisted to disk, surviving restarts. Trim it and save.
        let changed = {
            let mut cache = rooms_cache.write().await;
            let before = cache.len();
            cache.retain(|summary| summary.room_id != room_id);
            cache.len() != before
        };
        if changed {
            local_cache.schedule_rooms_snapshot();
        }
        Ok(())
    }

    fn url_encode(s: &str) -> String {
        let mut out = String::with_capacity(s.len() * 3);
        for b in s.bytes() {
            match b {
                b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => {
                    out.push(b as char)
                }
                _ => out.push_str(&format!("%{:02X}", b)),
            }
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::RoomActionService;

    #[test]
    fn pinned_orders_ascend_with_position() {
        // The spec sorts m.tag `order` ascending, so the topmost pinned room must get
        // the smallest value — that mapping is the whole point of the field.
        let orders: Vec<f64> = (0..4)
            .map(|index| RoomActionService::pinned_tag_order(index, 4))
            .collect();
        assert!(orders.windows(2).all(|pair| pair[0] < pair[1]));
    }

    #[test]
    fn pinned_orders_stay_inside_the_spec_range() {
        for count in 1..8usize {
            for index in 0..count {
                let order = RoomActionService::pinned_tag_order(index, count);
                assert!(order > 0.0 && order < 1.0, "{order} outside (0,1)");
            }
        }
    }

    #[test]
    fn a_room_appended_to_the_pinned_list_sorts_after_the_existing_ones() {
        // Pinning one room writes only that room's order, computed against the NEW
        // count, and leaves the others alone — cheaper, and it can't race a rewrite.
        // That is only safe if the appended value still exceeds every older one.
        for count in 1..8usize {
            let last_existing = RoomActionService::pinned_tag_order(count - 1, count);
            let appended = RoomActionService::pinned_tag_order(count, count + 1);
            assert!(
                appended > last_existing,
                "appending to {count} pinned rooms put it out of order"
            );
        }
    }
}

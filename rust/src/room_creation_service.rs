// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use anyhow::{anyhow, Result};
use matrix_sdk::ruma::api::client::room::create_room::v3::{
    CreationContent, Request as CreateRoomReq, RoomPreset,
};
use matrix_sdk::ruma::api::client::room::Visibility;
use matrix_sdk::ruma::events::room::encryption::RoomEncryptionEventContent;
use matrix_sdk::ruma::events::room::guest_access::{GuestAccess, RoomGuestAccessEventContent};
use matrix_sdk::ruma::events::room::history_visibility::{
    HistoryVisibility as RumaHistoryVisibility, RoomHistoryVisibilityEventContent,
};
use matrix_sdk::ruma::events::InitialStateEvent;
use matrix_sdk::ruma::serde::Raw;
use matrix_sdk::Client;
use tracing::warn;

use crate::types::{CreateRoomGuestAccess, CreateRoomHistoryVisibility, CreateRoomRequest};

pub(crate) struct RoomCreationService;

impl RoomCreationService {
    pub(crate) async fn create_room(client: Client, request: CreateRoomRequest) -> Result<String> {
        let name = request.name.trim().to_string();
        if name.is_empty() {
            return Err(anyhow!("Room name cannot be empty"));
        }

        let mut req = CreateRoomReq::new();
        req.name = Some(name);

        if let Some(topic) = request.topic.as_deref() {
            let trimmed = topic.trim();
            if !trimmed.is_empty() {
                req.topic = Some(trimmed.to_string());
            }
        }

        if request.is_public {
            req.visibility = Visibility::Public;
            req.preset = Some(RoomPreset::PublicChat);
        }

        if !request.federate {
            let mut creation = CreationContent::new();
            creation.federate = false;
            req.creation_content = Some(Raw::new(&creation)?);
        }

        if let Some(alias) = request.alias.as_deref() {
            let trimmed = alias.trim();
            if !trimmed.is_empty() {
                req.room_alias_name = Some(trimmed.to_string());
            }
        }

        let mut initial_state: Vec<Raw<_>> = Vec::new();

        if request.encrypted {
            let encryption_content = RoomEncryptionEventContent::with_recommended_defaults();
            let initial_event = InitialStateEvent::with_empty_state_key(encryption_content);
            let raw = Raw::new(&initial_event)?;
            initial_state.push(raw.cast());
        }

        let guest = match request.guest_access {
            CreateRoomGuestAccess::CanJoin => GuestAccess::CanJoin,
            _ => GuestAccess::Forbidden,
        };
        let guest_content = RoomGuestAccessEventContent::new(guest);
        let guest_event = InitialStateEvent::with_empty_state_key(guest_content);
        initial_state.push(Raw::new(&guest_event)?.cast());

        let history_visibility = match request.history_visibility {
            CreateRoomHistoryVisibility::Invited => RumaHistoryVisibility::Invited,
            CreateRoomHistoryVisibility::Shared => RumaHistoryVisibility::Shared,
            CreateRoomHistoryVisibility::WorldReadable => RumaHistoryVisibility::WorldReadable,
            _ => RumaHistoryVisibility::Joined,
        };
        let history_content = RoomHistoryVisibilityEventContent::new(history_visibility);
        let history_event = InitialStateEvent::with_empty_state_key(history_content);
        initial_state.push(Raw::new(&history_event)?.cast());

        req.initial_state = initial_state;

        let response = client.create_room(req).await?;
        let room_id = response.room_id().to_string();

        if let Some(avatar_path) = request.avatar_path.as_deref() {
            if !avatar_path.is_empty() {
                let path = std::path::Path::new(avatar_path);
                if path.exists() {
                    let avatar_result = async {
                        let room = client
                            .get_room(response.room_id())
                            .ok_or_else(|| anyhow!("Room not found: {room_id}"))?;
                        let data = std::fs::read(path).map_err(|e| {
                            anyhow!("Failed to read room avatar {avatar_path}: {e}")
                        })?;
                        let mime = match path.extension().and_then(|e| e.to_str()) {
                            Some("png") => mime::IMAGE_PNG,
                            Some("jpg") | Some("jpeg") => mime::IMAGE_JPEG,
                            Some("gif") => mime::IMAGE_GIF,
                            Some("webp") => "image/webp"
                                .parse()
                                .unwrap_or(mime::APPLICATION_OCTET_STREAM),
                            _ => mime::APPLICATION_OCTET_STREAM,
                        };
                        let mxc = client
                            .media()
                            .upload(&mime, data, None)
                            .await
                            .map_err(|e| {
                                anyhow!("Failed to upload room avatar for {room_id}: {e}")
                            })?;
                        use matrix_sdk::ruma::events::room::avatar::RoomAvatarEventContent;
                        let mut avatar_content = RoomAvatarEventContent::new();
                        avatar_content.url = Some(mxc.content_uri);
                        room.send_state_event(avatar_content).await.map_err(|e| {
                            anyhow!("Failed to send room avatar event for {room_id}: {e}")
                        })?;
                        Ok::<(), anyhow::Error>(())
                    }
                    .await;

                    if let Err(e) = avatar_result {
                        warn!("Room {room_id} created, but avatar setup failed: {e}");
                    }
                } else {
                    warn!("Room {room_id} created, but avatar path does not exist: {avatar_path}");
                }
            }
        }

        Ok(room_id)
    }
}

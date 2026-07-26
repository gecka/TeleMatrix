// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Finds/creates the per-account Saved Messages room and keeps its id cached.
//! Marker lives in global account data (see saved_messages.rs); creation is
//! lazy (first forward / first open). Two devices racing at first creation
//! converge on whichever marker write lands last; the orphan room is inert.

use anyhow::{anyhow, Result};
use matrix_sdk::ruma::api::client::config::get_global_account_data;
use matrix_sdk::ruma::events::{AnyGlobalAccountDataEventContent, GlobalAccountDataEventType};
use matrix_sdk::ruma::serde::Raw;
use matrix_sdk::{Client, RoomState};
use tracing::warn;

use crate::room_creation_service::RoomCreationService;
use crate::saved_messages::{
    decide_ensure_action, EnsureAction, MarkerRoomState, SavedMessagesPayload,
    SAVED_MESSAGES_EVENT_TYPE,
};
use crate::types::{CreateRoomGuestAccess, CreateRoomHistoryVisibility, CreateRoomRequest};

#[derive(Clone, Default)]
pub(crate) struct SavedMessagesService;

impl SavedMessagesService {
    pub(crate) fn new() -> Self {
        Self
    }

    async fn load_marker(client: &Client) -> Result<Option<SavedMessagesPayload>> {
        let event_type = GlobalAccountDataEventType::from(SAVED_MESSAGES_EVENT_TYPE);
        // An empty room_id (written by a permanent delete) means "no saved
        // room" — same as an absent marker.
        let non_empty = |p: SavedMessagesPayload| (!p.room_id.is_empty()).then_some(p);
        // Local-first: any previous sync has persisted the marker in the
        // state store, which makes the startup ensure effectively instant —
        // opening the room right after launch must already know its id
        // (otherwise it briefly renders as a plain room). The network
        // fallback covers a fresh login whose store has no sync yet.
        if let Ok(Some(raw)) = client.account().account_data_raw(event_type.clone()).await {
            if let Ok(payload) = serde_json::from_str::<SavedMessagesPayload>(raw.json().get()) {
                return Ok(non_empty(payload));
            }
        }
        let own_user = client
            .user_id()
            .ok_or_else(|| anyhow!("Not logged in"))?
            .to_owned();
        let request = get_global_account_data::v3::Request::new(own_user, event_type);
        match client.send(request).await {
            Ok(response) => {
                let payload: SavedMessagesPayload =
                    serde_json::from_str(response.account_data.json().get())
                        .map_err(|e| anyhow!("Bad saved-messages marker: {e}"))?;
                Ok(non_empty(payload))
            }
            // Missing account data is the normal "never created" state.
            Err(_) => Ok(None),
        }
    }

    async fn store_marker(client: &Client, room_id: &str) -> Result<()> {
        let payload = SavedMessagesPayload {
            room_id: room_id.to_owned(),
        };
        let raw: Raw<AnyGlobalAccountDataEventContent> = Raw::from_json(
            serde_json::value::to_raw_value(&payload)
                .map_err(|e| anyhow!("Serialize marker: {e}"))?,
        );
        let event_type = GlobalAccountDataEventType::from(SAVED_MESSAGES_EVENT_TYPE);
        client
            .account()
            .set_account_data_raw(event_type, raw)
            .await
            .map_err(|e| anyhow!("Store marker: {e}"))?;
        Ok(())
    }

    fn marker_room_state(client: &Client, room_id: &str) -> MarkerRoomState {
        let Some(room) = room_id
            .try_into()
            .ok()
            .and_then(|id: matrix_sdk::ruma::OwnedRoomId| client.get_room(&id))
        else {
            return MarkerRoomState::Unknown;
        };
        if room.state() == RoomState::Joined {
            MarkerRoomState::Joined
        } else {
            MarkerRoomState::Left
        }
    }

    /// Return the saved room, creating (+ marking) it only when `create` is set
    /// (an explicit forward / open). A passive caller (session start) with no
    /// live saved room gets `None` — the room is never auto-created.
    /// Mute-on-create is the caller's job (needs the client wrapper).
    /// Returns Some((room_id, created_now)), or None when there is no room.
    pub(crate) async fn ensure_room(
        &self,
        client: &Client,
        create: bool,
    ) -> Result<Option<(String, bool)>> {
        let marker_id = Self::load_marker(client)
            .await
            .unwrap_or(None)
            .map(|p| p.room_id);
        let state = marker_id
            .as_deref()
            .map(|id| Self::marker_room_state(client, id))
            .unwrap_or(MarkerRoomState::Unknown);
        match decide_ensure_action(marker_id, state, create) {
            EnsureAction::UseExisting(room_id) => {
                // Saved Messages must stay E2EE even if an older or foreign
                // client created the marker room plain. enable_encryption()
                // re-checks server state and no-ops when already encrypted.
                if let Ok(id) = matrix_sdk::ruma::OwnedRoomId::try_from(room_id.as_str()) {
                    if let Some(room) = client.get_room(&id) {
                        if !room.encryption_state().is_encrypted() {
                            if let Err(e) = room.enable_encryption().await {
                                warn!("[saved-messages] enable_encryption failed: {e:?}");
                            }
                        }
                    }
                }
                Ok(Some((room_id, false)))
            }
            EnsureAction::CreateNew => {
                let request = CreateRoomRequest {
                    name: "Saved Messages".to_owned(),
                    topic: None,
                    is_public: false,
                    encrypted: true,
                    alias: None,
                    avatar_path: None,
                    guest_access: CreateRoomGuestAccess::Forbidden,
                    history_visibility: CreateRoomHistoryVisibility::Invited,
                    federate: true,
                };
                let room_id = RoomCreationService::create_room(client.clone(), request).await?;
                if let Err(e) = Self::store_marker(client, &room_id).await {
                    // The room exists; a missing marker just means the next
                    // ensure creates a duplicate. Surface loudly.
                    warn!("[saved-messages] marker store failed: {e:?}");
                }
                Ok(Some((room_id, true)))
            }
            EnsureAction::NoneExists => Ok(None),
        }
    }

    /// Permanently forget the saved room: clear the marker (room_id → empty)
    /// and return the id it held so the caller can leave/forget the room.
    /// None when there was no saved room. The marker is cleared FIRST, so even
    /// if the leave fails the room stops being Saved Messages (and no stale
    /// marker re-adopts a forgotten room on the next launch).
    pub(crate) async fn take_marker(&self, client: &Client) -> Result<Option<String>> {
        let Some(payload) = Self::load_marker(client).await? else {
            return Ok(None);
        };
        Self::store_marker(client, "").await?;
        Ok(Some(payload.room_id))
    }
}

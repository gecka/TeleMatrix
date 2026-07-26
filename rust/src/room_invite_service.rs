// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use anyhow::{anyhow, Result};
use matrix_sdk::ruma::OwnedRoomId;
use matrix_sdk::Client;

use crate::timeline_window_service::{
    TimelineChangedFactory, TimelineRuntime, TimelineWindowService,
};

pub(crate) struct RoomInviteService;

impl RoomInviteService {
    pub(crate) async fn accept_invite(
        client: Client,
        room_id: &str,
        runtime: TimelineRuntime,
        make_on_changed: TimelineChangedFactory,
    ) -> Result<()> {
        let parsed_room_id: OwnedRoomId =
            room_id.try_into().map_err(|_| anyhow!("Invalid room ID"))?;
        let room = client
            .get_room(&parsed_room_id)
            .ok_or_else(|| anyhow!("Room not found"))?;
        room.join()
            .await
            .map_err(|e| anyhow!("Failed to accept invite: {e}"))?;

        {
            let wins = runtime.windows.read().await;
            if wins.contains_key(room_id) {
                return Ok(());
            }
        }

        let room = client
            .get_room(&parsed_room_id)
            .ok_or_else(|| anyhow!("Room not found after join"))?;

        TimelineWindowService::create_window_after_join(
            room,
            room_id.to_string(),
            runtime,
            make_on_changed,
        )
        .await;

        Ok(())
    }
}

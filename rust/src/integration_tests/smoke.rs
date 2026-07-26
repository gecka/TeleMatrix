// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Smoke test validating the harness: a client can log into the mock server and
//! observe a synced joined room. If this fails, the heavier scenarios can't run.

use matrix_sdk::ruma::room_id;

use super::common;

#[tokio::test]
async fn client_observes_a_synced_joined_room() {
    let (server, client) = common::mock_server_and_client().await;
    server.mock_room_state_encryption().plain().mount().await;

    let room = server
        .sync_joined_room(&client, room_id!("!room:localhost"))
        .await;

    assert_eq!(room.room_id(), room_id!("!room:localhost"));
    assert_eq!(client.rooms().len(), 1);
}

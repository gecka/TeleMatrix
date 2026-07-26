// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! End-to-end rooms-list preview cascade through `build_rooms_cache` over a real
//! SDK `Client`. Pins three regressions at the integration level (the pure helper
//! is unit-tested in `room_summary_service`):
//!   * a redacted latest message must not surface as "[Message deleted]";
//!   * a system/state-only room must use the UNIX epoch as its sort time, not a
//!     misleading per-event timestamp;
//!   * a joined space must not reach the chat list at all.

use std::collections::HashMap;
use std::time::UNIX_EPOCH;

use matrix_sdk::ruma::events::AnySyncStateEvent;
use matrix_sdk::ruma::room_id;
use matrix_sdk::ruma::serde::Raw;
use matrix_sdk_test::JoinedRoomBuilder;
use tokio::sync::RwLock;

use super::common;
use crate::room_summary_service::RoomSummaryService;

#[tokio::test]
async fn build_rooms_cache_skips_deleted_and_epochs_system_rooms() {
    let (server, client) = common::mock_server_and_client().await;
    server.mock_room_state_encryption().plain().mount().await;

    // Two joined rooms with no previewable server `latest_event`, so the builder
    // falls through to our in-memory timeline cache (the cascade under test).
    let with_deleted = room_id!("!withdeleted:localhost");
    let system_only = room_id!("!systemonly:localhost");
    server.sync_joined_room(&client, with_deleted).await;
    server.sync_joined_room(&client, system_only).await;

    let mut tc = HashMap::new();
    // Newest item is redacted -> the row must show the previous REAL message.
    tc.insert(
        with_deleted.to_string(),
        vec![
            common::cache_text_item("$real", "the real message", 10, false),
            common::cache_text_item("$gone", "redacted", 20, true),
        ],
    );
    // Only a system/service event -> empty preview + epoch timestamp.
    tc.insert(
        system_only.to_string(),
        vec![common::cache_service_item(
            "$svc",
            "Alice changed the avatar",
            99,
        )],
    );
    let timeline_cache = RwLock::new(tc);

    let empty_overrides = HashMap::new();
    let empty_presence = HashMap::new();
    let summaries = RoomSummaryService::build_rooms_cache(
        &client,
        &timeline_cache,
        &empty_overrides,
        &empty_presence,
    )
    .await
    .expect("build_rooms_cache should succeed against the mock server");

    let deleted_summary = summaries
        .iter()
        .find(|s| s.room_id == with_deleted.as_str())
        .expect("the room with a redacted latest message must be present");
    assert_eq!(
        deleted_summary.last_event_text, "the real message",
        "redacted latest message must not surface; previous real one wins"
    );
    assert_eq!(
        deleted_summary.last_event_timestamp,
        UNIX_EPOCH + std::time::Duration::from_secs(10)
    );

    let system_summary = summaries
        .iter()
        .find(|s| s.room_id == system_only.as_str())
        .expect("the system-only room must be present");
    assert!(
        system_summary.last_event_text.is_empty(),
        "a service/system-only room must not produce a fake preview"
    );
    assert_eq!(
        system_summary.last_event_timestamp, UNIX_EPOCH,
        "system-only room must sort by the epoch, not the service event's time"
    );
}

/// A space is a joined room as far as the SDK is concerned, so without an explicit `is_space` guard
/// it lands in the chat list as a permanently empty row.
#[tokio::test]
async fn build_rooms_cache_excludes_joined_spaces() {
    let (server, client) = common::mock_server_and_client().await;
    server.mock_room_state_encryption().plain().mount().await;

    let space = room_id!("!space:localhost");
    let normal = room_id!("!normal:localhost");

    let create_space = Raw::<AnySyncStateEvent>::from_json_string(
        serde_json::json!({
            "type": "m.room.create",
            "state_key": "",
            "sender": "@alice:localhost",
            "event_id": "$create_space",
            "origin_server_ts": 0,
            "content": {
                "creator": "@alice:localhost",
                "room_version": "9",
                "type": "m.space",
            },
        })
        .to_string(),
    )
    .expect("the create event must deserialize");

    server
        .sync_room(
            &client,
            JoinedRoomBuilder::new(space).add_state_event(create_space),
        )
        .await;
    server.sync_joined_room(&client, normal).await;

    // Without this the test would pass vacuously if the fixture failed to register as a space.
    assert!(
        client
            .get_room(space)
            .expect("the space must be a known room")
            .is_space(),
        "fixture is not actually a space"
    );

    let timeline_cache = RwLock::new(HashMap::new());
    let empty_overrides = HashMap::new();
    let empty_presence = HashMap::new();
    let summaries = RoomSummaryService::build_rooms_cache(
        &client,
        &timeline_cache,
        &empty_overrides,
        &empty_presence,
    )
    .await
    .expect("build_rooms_cache should succeed against the mock server");

    assert!(
        summaries.iter().all(|s| s.room_id != space.as_str()),
        "a joined space must never reach the chat list"
    );
    assert!(
        summaries.iter().any(|s| s.room_id == normal.as_str()),
        "the ordinary joined room must still be listed"
    );
}

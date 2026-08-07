// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! End-to-end proof of the room-state override chain for avatars: upload the
//! media, send the state event, record the override, and assert the summary the
//! rooms list is built from carries the NEW url even though the store still
//! reports the old one (the sliding-sync timeline-state gap). Then simulate the
//! store catching up and assert the override retires.

use std::collections::HashMap;

use matrix_sdk::ruma::events::room::avatar::RoomAvatarEventContent;
use matrix_sdk::ruma::{event_id, owned_mxc_uri, room_id, user_id};
use matrix_sdk_test::{event_factory::EventFactory, JoinedRoomBuilder};
use tokio::sync::RwLock;

use super::common::mock_server_and_client;
use crate::room_action_service::RoomActionService;
use crate::room_summary_service::{room_avatar_url, set_avatar_override, RoomSummaryService};

#[tokio::test]
async fn a_freshly_set_avatar_reaches_the_summary_before_the_store_learns_it() {
    let (server, client) = mock_server_and_client().await;
    let room_id = room_id!("!room:localhost");
    server
        .mock_sync()
        .ok_and_run(&client, |builder| {
            builder.add_joined_room(JoinedRoomBuilder::new(room_id));
        })
        .await;
    let room = client.get_room(room_id).expect("room known");

    // The SDK preflights the media config before uploading.
    server
        .mock_authenticated_media_config()
        .ok(matrix_sdk::ruma::uint!(1_048_576))
        .mount()
        .await;
    // Not `mock_upload()`: its fixed `/_matrix/media/v3/upload` path 404s here,
    // so match any upload-shaped POST and let the response carry the mxc.
    wiremock::Mock::given(wiremock::matchers::method("POST"))
        .and(wiremock::matchers::path_regex(r".*/upload$"))
        .respond_with(
            wiremock::ResponseTemplate::new(200)
                .set_body_json(serde_json::json!({ "content_uri": "mxc://localhost/new-avatar" })),
        )
        .mount(server.server())
        .await;
    server
        .mock_room_send_state()
        .ok(event_id!("$avatar-event:localhost"))
        .mount()
        .await;

    // The full production sequence from the FFI wrapper.
    let mxc_url = RoomActionService::upload_room_avatar(
        client.clone(),
        room.clone(),
        room_id.as_str(),
        vec![1, 2, 3],
        "image/png",
    )
    .await
    .expect("upload + state event");
    assert_eq!(mxc_url, "mxc://localhost/new-avatar");
    set_avatar_override(&room, Some(mxc_url.clone()));

    // The store has NOT ingested the state event (that is the whole bug), but
    // the override must already win at the raw read...
    assert_eq!(room.avatar_url(), None, "precondition: store still stale");
    assert_eq!(room_avatar_url(&room), Some(mxc_url.clone()));

    // ...and through the real summary build the rooms list uses.
    let timeline_cache = RwLock::new(HashMap::new());
    let summaries = RoomSummaryService::build_rooms_cache(
        &client,
        &timeline_cache,
        &HashMap::new(),
        &HashMap::new(),
    )
    .await
    .expect("summary build");
    let summary = summaries
        .iter()
        .find(|s| s.room_id == room_id.as_str())
        .expect("room summarised");
    assert_eq!(
        summary.avatar_url.as_deref(),
        Some("mxc://localhost/new-avatar"),
        "the summary must serve the override while the store lags"
    );

    // Store catches up (the avatar state event finally arrives as state):
    // the override retires and the store value is served directly.
    let factory = EventFactory::new()
        .room(room_id)
        .sender(user_id!("@me:localhost"));
    let mut avatar_content = RoomAvatarEventContent::new();
    avatar_content.url = Some(owned_mxc_uri!("mxc://localhost/new-avatar"));
    server
        .mock_sync()
        .ok_and_run(&client, |builder| {
            builder.add_joined_room(
                JoinedRoomBuilder::new(room_id).add_state_event(
                    factory
                        .event(avatar_content)
                        .state_key("")
                        .into_raw_sync_state(),
                ),
            );
        })
        .await;
    assert_eq!(
        room.avatar_url().map(|u| u.to_string()).as_deref(),
        Some("mxc://localhost/new-avatar"),
        "precondition: store caught up"
    );
    assert_eq!(
        room_avatar_url(&room),
        Some("mxc://localhost/new-avatar".to_string())
    );
}

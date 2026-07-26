// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Room discovery against a mocked homeserver: the public directory, a space's children, and
//! joining. The hierarchy tests carry the most weight — the endpoint returns the queried space
//! inside its own `rooms` array, and the `via` hints for its children live on the space's chunk
//! rather than the children's.

use matrix_sdk::ruma::directory::{PublicRoomsChunk, PublicRoomsChunkInit};
use matrix_sdk::ruma::{room_id, uint};
use serde_json::json;
use wiremock::matchers::{method, path_regex};
use wiremock::{Mock, ResponseTemplate};

use super::common;
use crate::room_directory_service::RoomDirectoryService;
use crate::types::{RoomDirectoryRequest, RoomMembershipState, SpaceHierarchyRequest};

fn directory_request(query: &str) -> RoomDirectoryRequest {
    RoomDirectoryRequest {
        request_id: 1,
        query: query.to_owned(),
        limit: 50,
        next_token: None,
    }
}

fn chunk(id: &str, name: &str, topic: &str, space: bool) -> PublicRoomsChunk {
    let mut chunk: PublicRoomsChunk = PublicRoomsChunkInit {
        num_joined_members: uint!(7),
        room_id: id.try_into().expect("valid room id"),
        world_readable: true,
        guest_can_join: false,
    }
    .into();
    chunk.name = Some(name.to_owned());
    chunk.topic = Some(topic.to_owned());
    if space {
        chunk.room_type = Some(matrix_sdk::ruma::room::RoomType::Space);
    }
    chunk
}

/// Mount a raw `/hierarchy` response — the SDK's mock server has no helper for this endpoint.
async fn mock_hierarchy(
    server: &matrix_sdk::test_utils::mocks::MatrixMockServer,
    body: serde_json::Value,
) {
    Mock::given(method("GET"))
        .and(path_regex(r"/_matrix/client/v1/rooms/.*/hierarchy"))
        .respond_with(ResponseTemplate::new(200).set_body_json(body))
        .mount(server.server())
        .await;
}

#[tokio::test]
async fn search_public_rooms_maps_chunks_and_pagination() {
    let (server, client) = common::mock_server_and_client().await;
    server
        .mock_public_rooms()
        .ok(
            vec![
                chunk("!room:localhost", "Matrix HQ", "General chat", false),
                chunk("!space:localhost", "Foundation", "The space", true),
            ],
            Some("tok2".to_owned()),
            None,
            Some(42),
        )
        .mount()
        .await;

    let page = RoomDirectoryService::search_public_rooms(client, directory_request("matrix"))
        .await
        .expect("the directory search must succeed");

    assert_eq!(page.entries.len(), 2);
    assert_eq!(page.total_approx, 42);
    assert_eq!(page.next_token.as_deref(), Some("tok2"));
    assert!(!page.done, "a next_batch means there is more to fetch");

    let room = &page.entries[0];
    assert_eq!(room.name, "Matrix HQ");
    assert_eq!(room.topic, "General chat");
    assert_eq!(room.member_count, 7);
    assert!(!room.is_space);

    assert!(page.entries[1].is_space, "an m.space chunk must be flagged");
}

#[tokio::test]
async fn search_public_rooms_empty_directory_is_done() {
    let (server, client) = common::mock_server_and_client().await;
    server
        .mock_public_rooms()
        .ok(vec![], None, None, Some(0))
        .mount()
        .await;

    let page = RoomDirectoryService::search_public_rooms(client, directory_request(""))
        .await
        .expect("an empty directory is not an error");

    assert!(page.entries.is_empty());
    assert!(page.done, "no next_batch means the listing is exhausted");
    assert_eq!(page.total_approx, 0);
}

#[tokio::test]
async fn search_public_rooms_flags_rooms_we_are_already_in() {
    let (server, client) = common::mock_server_and_client().await;
    let joined = room_id!("!joined:localhost");
    server.sync_joined_room(&client, joined).await;
    server
        .mock_public_rooms()
        .ok(
            vec![
                chunk(joined.as_str(), "Already in", "", false),
                chunk("!other:localhost", "Not in", "", false),
            ],
            None,
            None,
            None,
        )
        .mount()
        .await;

    let page = RoomDirectoryService::search_public_rooms(client, directory_request(""))
        .await
        .expect("the directory search must succeed");

    assert_eq!(page.entries[0].membership, RoomMembershipState::Joined);
    assert_eq!(page.entries[1].membership, RoomMembershipState::None);
    assert_eq!(
        page.total_approx, -1,
        "no server estimate must surface as -1, not 0"
    );
}

/// `/hierarchy` returns the queried space as a member of its own `rooms` array. Left in, every space
/// would list itself as its own first child.
#[tokio::test]
async fn space_children_excludes_the_space_itself() {
    let (server, client) = common::mock_server_and_client().await;
    mock_hierarchy(
        &server,
        json!({
            "rooms": [
                {
                    "room_id": "!space:localhost",
                    "name": "Foundation",
                    "num_joined_members": 3,
                    "world_readable": true,
                    "guest_can_join": false,
                    "join_rule": "public",
                    "room_type": "m.space",
                    "children_state": [],
                },
                {
                    "room_id": "!a:localhost",
                    "name": "Design",
                    "topic": "Design chat",
                    "num_joined_members": 10,
                    "world_readable": true,
                    "guest_can_join": false,
                    "join_rule": "public",
                    "children_state": [],
                },
                {
                    "room_id": "!b:localhost",
                    "name": "Governance",
                    "num_joined_members": 5,
                    "world_readable": true,
                    "guest_can_join": false,
                    "join_rule": "public",
                    "children_state": [],
                },
            ],
            "next_batch": "h2",
        }),
    )
    .await;

    let page = RoomDirectoryService::space_children(
        client,
        SpaceHierarchyRequest {
            request_id: 1,
            space_id: "!space:localhost".to_owned(),
            limit: 50,
            next_token: None,
        },
    )
    .await
    .expect("the hierarchy request must succeed");

    assert_eq!(page.entries.len(), 2, "the space must not be its own child");
    assert!(
        page.entries.iter().all(|e| e.room_id != "!space:localhost"),
        "the queried space leaked into its own child list"
    );
    assert_eq!(page.entries[0].name, "Design");
    assert_eq!(page.entries[0].topic, "Design chat");
    assert_eq!(page.next_token.as_deref(), Some("h2"));
    assert!(!page.done);
}

/// The `via` hints for a child live on the *space's* `m.space.child` events, not on the child's own
/// chunk. A child the space gave no hint for falls back to its room ID's server.
#[tokio::test]
async fn space_children_take_via_from_the_spaces_children_state() {
    let (server, client) = common::mock_server_and_client().await;
    mock_hierarchy(
        &server,
        json!({
            "rooms": [
                {
                    "room_id": "!space:localhost",
                    "num_joined_members": 3,
                    "world_readable": true,
                    "guest_can_join": false,
                    "join_rule": "public",
                    "room_type": "m.space",
                    "children_state": [{
                        "type": "m.space.child",
                        "state_key": "!hinted:other.org",
                        "sender": "@alice:localhost",
                        "origin_server_ts": 1,
                        "content": { "via": ["other.org", "third.org"] },
                    }],
                },
                {
                    "room_id": "!hinted:other.org",
                    "num_joined_members": 1,
                    "world_readable": true,
                    "guest_can_join": false,
                    "join_rule": "public",
                    "children_state": [],
                },
                {
                    "room_id": "!unhinted:fallback.org",
                    "num_joined_members": 1,
                    "world_readable": true,
                    "guest_can_join": false,
                    "join_rule": "public",
                    "children_state": [],
                },
            ],
        }),
    )
    .await;

    let page = RoomDirectoryService::space_children(
        client,
        SpaceHierarchyRequest {
            request_id: 1,
            space_id: "!space:localhost".to_owned(),
            limit: 50,
            next_token: None,
        },
    )
    .await
    .expect("the hierarchy request must succeed");

    let hinted = page
        .entries
        .iter()
        .find(|e| e.room_id == "!hinted:other.org")
        .expect("the hinted child must be listed");
    assert_eq!(hinted.via, vec!["other.org", "third.org"]);

    let unhinted = page
        .entries
        .iter()
        .find(|e| e.room_id == "!unhinted:fallback.org")
        .expect("the unhinted child must be listed");
    assert_eq!(
        unhinted.via,
        vec!["fallback.org"],
        "a child with no hint falls back to its own server"
    );

    assert!(page.done, "no next_batch means the space is fully listed");
}

#[tokio::test]
async fn space_children_surface_rate_limiting() {
    let (server, client) = common::mock_server_and_client().await;
    Mock::given(method("GET"))
        .and(path_regex(r"/_matrix/client/v1/rooms/.*/hierarchy"))
        .respond_with(ResponseTemplate::new(429).set_body_json(json!({
            "errcode": "M_LIMIT_EXCEEDED",
            "error": "Too many requests",
            "retry_after_ms": 2000,
        })))
        .mount(server.server())
        .await;

    let result = RoomDirectoryService::space_children(
        client,
        SpaceHierarchyRequest {
            request_id: 1,
            space_id: "!space:localhost".to_owned(),
            limit: 50,
            next_token: None,
        },
    )
    .await;

    // The UI renders this verbatim, so a 429 must not masquerade as an empty space.
    assert!(result.is_err(), "rate limiting must surface as an error");
}

#[tokio::test]
async fn preview_messages_first_page_uses_initial_sync() {
    use crate::types::MessageContent;
    use std::collections::HashMap;
    use std::sync::{Arc, RwLock};

    let (server, client) = common::mock_server_and_client().await;

    // The first page (from = None) must hit /initialSync, which sets up the peek for remote
    // world-readable rooms. Its chunk is chronological (oldest→newest); `start` is the token for
    // the next-older page.
    Mock::given(method("GET"))
        .and(path_regex(r"/_matrix/client/.*/rooms/.*/initialSync"))
        .respond_with(ResponseTemplate::new(200).set_body_json(json!({
            "room_id": "!room:localhost",
            "messages": {
                "start": "older_token",
                "end": "e1",
                "chunk": [
                    {
                        "type": "m.room.message",
                        "event_id": "$older:localhost",
                        "sender": "@alice:localhost",
                        "origin_server_ts": 1000,
                        "room_id": "!room:localhost",
                        "content": { "msgtype": "m.text", "body": "first" }
                    },
                    {
                        "type": "m.room.message",
                        "event_id": "$newer:localhost",
                        "sender": "@alice:localhost",
                        "origin_server_ts": 2000,
                        "room_id": "!room:localhost",
                        "content": { "msgtype": "m.text", "body": "second" }
                    }
                ]
            },
            "state": [
                {
                    "type": "m.room.member",
                    "event_id": "$m:localhost",
                    "sender": "@alice:localhost",
                    "state_key": "@alice:localhost",
                    "origin_server_ts": 900,
                    "room_id": "!room:localhost",
                    "content": { "membership": "join", "displayname": "Alice" }
                }
            ]
        })))
        .mount(server.server())
        .await;

    let media_sources = Arc::new(RwLock::new(HashMap::new()));
    let (items, next_token) =
        RoomDirectoryService::preview_messages(client, "!room:localhost", None, 30, &media_sources)
            .await
            .expect("a world-readable room must return its history");

    assert_eq!(items.len(), 2, "both message events must map");
    // initialSync's chunk is already chronological — no reversing.
    assert_eq!(items[0].event_id, "$older:localhost");
    assert_eq!(items[1].event_id, "$newer:localhost");
    // The sender name is taken from the state block, not the bare MXID.
    assert_eq!(items[0].sender.display_name, "Alice");
    match &items[0].content {
        MessageContent::Text { body, .. } => assert_eq!(body, "first"),
        other => panic!("expected a text message, got {other:?}"),
    }
    // `start` is the token that continues backward into /messages.
    assert_eq!(next_token.as_deref(), Some("older_token"));
}

#[tokio::test]
async fn preview_messages_older_page_uses_messages_endpoint() {
    use std::collections::HashMap;
    use std::sync::{Arc, RwLock};

    let (server, client) = common::mock_server_and_client().await;

    // Older pages (from = Some) use /messages?dir=b (newest→oldest, reversed for display); `end`
    // is the token for the next-older page.
    Mock::given(method("GET"))
        .and(path_regex(r"/_matrix/client/v3/rooms/.*/messages"))
        .respond_with(ResponseTemplate::new(200).set_body_json(json!({
            "start": "s1",
            "end": "even_older",
            "chunk": [
                {
                    "type": "m.room.message",
                    "event_id": "$b:localhost",
                    "sender": "@alice:localhost",
                    "origin_server_ts": 2000,
                    "room_id": "!room:localhost",
                    "content": { "msgtype": "m.text", "body": "b" }
                },
                {
                    "type": "m.room.message",
                    "event_id": "$a:localhost",
                    "sender": "@alice:localhost",
                    "origin_server_ts": 1000,
                    "room_id": "!room:localhost",
                    "content": { "msgtype": "m.text", "body": "a" }
                }
            ],
            "state": []
        })))
        .mount(server.server())
        .await;

    let media_sources = Arc::new(RwLock::new(HashMap::new()));
    let (items, next_token) = RoomDirectoryService::preview_messages(
        client,
        "!room:localhost",
        Some("older_token".to_owned()),
        30,
        &media_sources,
    )
    .await
    .expect("the older-page request must succeed");

    assert_eq!(items.len(), 2);
    // Reversed into reading order: oldest first.
    assert_eq!(items[0].event_id, "$a:localhost");
    assert_eq!(items[1].event_id, "$b:localhost");
    assert_eq!(next_token.as_deref(), Some("even_older"));
}

#[tokio::test]
async fn preview_messages_empty_chunk_stops_pagination() {
    use std::collections::HashMap;
    use std::sync::{Arc, RwLock};

    let (server, client) = common::mock_server_and_client().await;

    // The server returns an empty chunk once history is exhausted; its `end` must NOT be handed
    // back, or the caller would loop forever re-requesting the same empty page.
    Mock::given(method("GET"))
        .and(path_regex(r"/_matrix/client/v3/rooms/.*/messages"))
        .respond_with(ResponseTemplate::new(200).set_body_json(json!({
            "start": "s1",
            "end": "e1",
            "chunk": [],
            "state": []
        })))
        .mount(server.server())
        .await;

    let media_sources = Arc::new(RwLock::new(HashMap::new()));
    let (items, next_token) = RoomDirectoryService::preview_messages(
        client,
        "!room:localhost",
        Some("older".to_owned()),
        30,
        &media_sources,
    )
    .await
    .expect("the request must succeed");

    assert!(items.is_empty());
    assert_eq!(
        next_token, None,
        "an empty chunk means the start of history"
    );
}

#[tokio::test]
async fn preview_messages_ignores_non_message_events() {
    use std::collections::HashMap;
    use std::sync::{Arc, RwLock};

    let (server, client) = common::mock_server_and_client().await;

    // A topic change is a state event, not a message — a preview shows only messages.
    Mock::given(method("GET"))
        .and(path_regex(r"/_matrix/client/.*/rooms/.*/initialSync"))
        .respond_with(ResponseTemplate::new(200).set_body_json(json!({
            "room_id": "!room:localhost",
            "messages": {
                "start": "s1",
                "end": "e1",
                "chunk": [
                    {
                        "type": "m.room.topic",
                        "event_id": "$topic:localhost",
                        "sender": "@alice:localhost",
                        "origin_server_ts": 1000,
                        "room_id": "!room:localhost",
                        "state_key": "",
                        "content": { "topic": "new topic" }
                    },
                    {
                        "type": "m.room.message",
                        "event_id": "$msg:localhost",
                        "sender": "@bob:localhost",
                        "origin_server_ts": 1100,
                        "room_id": "!room:localhost",
                        "content": { "msgtype": "m.text", "body": "hi" }
                    }
                ]
            },
            "state": []
        })))
        .mount(server.server())
        .await;

    let media_sources = Arc::new(RwLock::new(HashMap::new()));
    let (items, _next_token) =
        RoomDirectoryService::preview_messages(client, "!room:localhost", None, 30, &media_sources)
            .await
            .expect("the request must succeed");

    assert_eq!(items.len(), 1, "only the m.room.message event is kept");
    assert_eq!(items[0].event_id, "$msg:localhost");
    // A sender absent from the state block falls back to the bare MXID.
    assert_eq!(items[0].sender.display_name, "@bob:localhost");
}

#[tokio::test]
async fn preview_messages_folds_edits_into_the_original() {
    use crate::types::MessageContent;
    use std::collections::HashMap;
    use std::sync::{Arc, RwLock};

    let (server, client) = common::mock_server_and_client().await;

    // An m.replace edit must update the original in place, NOT appear as a second `* edited` bubble.
    Mock::given(method("GET"))
        .and(path_regex(r"/_matrix/client/.*/rooms/.*/initialSync"))
        .respond_with(ResponseTemplate::new(200).set_body_json(json!({
            "room_id": "!room:localhost",
            "messages": {
                "start": "s1",
                "end": "e1",
                "chunk": [
                    {
                        "type": "m.room.message",
                        "event_id": "$orig:localhost",
                        "sender": "@a:localhost",
                        "origin_server_ts": 1000,
                        "room_id": "!room:localhost",
                        "content": { "msgtype": "m.text", "body": "original" }
                    },
                    {
                        "type": "m.room.message",
                        "event_id": "$edit:localhost",
                        "sender": "@a:localhost",
                        "origin_server_ts": 1001,
                        "room_id": "!room:localhost",
                        "content": {
                            "msgtype": "m.text",
                            "body": "* edited",
                            "m.new_content": { "msgtype": "m.text", "body": "edited" },
                            "m.relates_to": { "rel_type": "m.replace", "event_id": "$orig:localhost" }
                        }
                    }
                ]
            },
            "state": []
        })))
        .mount(server.server())
        .await;

    let media_sources = Arc::new(RwLock::new(HashMap::new()));
    let (items, _) =
        RoomDirectoryService::preview_messages(client, "!room:localhost", None, 30, &media_sources)
            .await
            .expect("the request must succeed");

    assert_eq!(items.len(), 1, "the edit must not add a second bubble");
    assert_eq!(items[0].event_id, "$orig:localhost");
    assert!(items[0].is_edited);
    match &items[0].content {
        MessageContent::Text { body, .. } => assert_eq!(body, "edited"),
        other => panic!("expected the edited text, got {other:?}"),
    }
}

#[tokio::test]
async fn preview_messages_maps_stickers_to_images() {
    use crate::types::MessageContent;
    use std::collections::HashMap;
    use std::sync::{Arc, RwLock};

    let (server, client) = common::mock_server_and_client().await;

    Mock::given(method("GET"))
        .and(path_regex(r"/_matrix/client/.*/rooms/.*/initialSync"))
        .respond_with(ResponseTemplate::new(200).set_body_json(json!({
            "room_id": "!room:localhost",
            "messages": {
                "start": "s1",
                "end": "e1",
                "chunk": [
                    {
                        "type": "m.sticker",
                        "event_id": "$st:localhost",
                        "sender": "@a:localhost",
                        "origin_server_ts": 1000,
                        "room_id": "!room:localhost",
                        "content": {
                            "body": "wave",
                            "url": "mxc://localhost/sticker",
                            "info": { "mimetype": "image/png", "w": 128, "h": 128 }
                        }
                    }
                ]
            },
            "state": []
        })))
        .mount(server.server())
        .await;

    let media_sources = Arc::new(RwLock::new(HashMap::new()));
    let (items, _) =
        RoomDirectoryService::preview_messages(client, "!room:localhost", None, 30, &media_sources)
            .await
            .expect("the request must succeed");

    assert_eq!(items.len(), 1, "the sticker must render");
    match &items[0].content {
        MessageContent::Image {
            url, width, height, ..
        } => {
            assert_eq!(url, "mxc://localhost/sticker");
            assert_eq!(*width, 128);
            assert_eq!(*height, 128);
        }
        other => panic!("expected a sticker image, got {other:?}"),
    }
}

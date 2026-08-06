// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! The post-verification gossip sweep must find stuck sessions in rooms the user
//! never opened.
//!
//! Its first implementation collected them from our own rendered-timeline cache,
//! which is only written for rooms with an open `TimelineWindow`. A device
//! verifying before opening anything — the normal fresh-login flow — therefore
//! swept an empty map and requested nothing, leaving every room to the
//! ~9-minute backup ladder. It now reads the SDK's persistent event-cache store,
//! which sliding sync fills for every joined room with no timeline involved.
//!
//! The contract these tests pin: the store's `m.room.encrypted` type filter
//! never returns events whose kind is plaintext, and finds UTDs with no
//! timeline involved. The other half of the approach — the redecryptor
//! rewriting a row's stored type when an event decrypts — is upstream-tested
//! (matrix-sdk-base's shared store suite runs the same filter against both
//! stores) and verified in the vendored source, not exercised here: that would
//! need a full e2ee round trip.

use matrix_sdk::cross_process_lock::MappedCrossProcessLockState as LockState;
use matrix_sdk::deserialized_responses::{
    TimelineEvent, UnableToDecryptInfo, UnableToDecryptReason,
};
use matrix_sdk::ruma::events::AnySyncTimelineEvent;
use matrix_sdk::ruma::serde::Raw;
use matrix_sdk::ruma::{room_id, RoomId};
use matrix_sdk::Client;

use super::common::mock_server_and_client;
use crate::matrix::stuck_sessions_from_store;

fn raw_encrypted(event_id: &str) -> Raw<AnySyncTimelineEvent> {
    Raw::from_json_string(format!(
        r#"{{"type":"m.room.encrypted","event_id":"{event_id}",
            "sender":"@alice:localhost","origin_server_ts":0,"content":{{}}}}"#
    ))
    .expect("valid json")
}

fn raw_plaintext(event_id: &str) -> Raw<AnySyncTimelineEvent> {
    Raw::from_json_string(format!(
        r#"{{"type":"m.room.message","event_id":"{event_id}",
            "sender":"@alice:localhost","origin_server_ts":0,
            "content":{{"msgtype":"m.text","body":"hi"}}}}"#
    ))
    .expect("valid json")
}

fn utd(event_id: &str, session_id: Option<&str>) -> TimelineEvent {
    TimelineEvent::from_utd(
        raw_encrypted(event_id),
        UnableToDecryptInfo {
            session_id: session_id.map(str::to_owned),
            reason: UnableToDecryptReason::MissingMegolmSession {
                withheld_code: None,
            },
        },
    )
}

async fn seed(client: &Client, room: &RoomId, events: Vec<TimelineEvent>) {
    let guard = match client.event_cache_store().lock().await.expect("store lock") {
        LockState::Clean(guard) | LockState::Dirty(guard) => guard,
    };
    for event in events {
        guard.save_event(room, event).await.expect("save event");
    }
}

#[tokio::test]
async fn sweep_finds_stuck_sessions_without_any_room_being_opened() {
    let (server, client) = mock_server_and_client().await;
    let stuck = room_id!("!stuck:localhost");
    let clean = room_id!("!clean:localhost");
    server.sync_joined_room(&client, stuck).await;
    server.sync_joined_room(&client, clean).await;

    seed(
        &client,
        stuck,
        vec![
            utd("$1", Some("sess-A")),
            utd("$2", Some("sess-A")), // same session -> one request
            utd("$3", Some("sess-B")),
            utd("$4", None), // non-megolm: no key to ask for
            // Decrypted events live in the store under their plaintext type, so
            // the query must not return them.
            TimelineEvent::from_plaintext(raw_plaintext("$5")),
        ],
    )
    .await;
    seed(
        &client,
        clean,
        vec![TimelineEvent::from_plaintext(raw_plaintext("$6"))],
    )
    .await;

    // No timeline was ever built for either room — that is the whole point.
    let by_room = stuck_sessions_from_store(&client).await;

    assert_eq!(
        by_room.len(),
        1,
        "a room with nothing stuck must be absent, not present with an empty set: {by_room:?}"
    );
    assert_eq!(
        by_room[stuck.as_str()],
        ["sess-A".to_string(), "sess-B".to_string()]
            .into_iter()
            .collect()
    );
}

#[tokio::test]
async fn sweep_reports_nothing_for_a_fully_decrypted_account() {
    let (server, client) = mock_server_and_client().await;
    let room = room_id!("!room:localhost");
    server.sync_joined_room(&client, room).await;
    seed(
        &client,
        room,
        vec![TimelineEvent::from_plaintext(raw_plaintext("$1"))],
    )
    .await;

    assert!(stuck_sessions_from_store(&client).await.is_empty());
}

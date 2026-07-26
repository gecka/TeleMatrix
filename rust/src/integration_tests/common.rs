// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Shared harness for the mock-SDK integration tests.

use std::time::{Duration, UNIX_EPOCH};

use matrix_sdk::test_utils::mocks::MatrixMockServer;
use matrix_sdk::Client;

use crate::types::{MessageContent, SendState, TimelineItem, UserProfile};

/// A mock homeserver paired with a logged-in client built against it.
/// The server keeps a stateful sync-response builder, so repeated syncs advance
/// the batch token like a real server.
pub(super) async fn mock_server_and_client() -> (MatrixMockServer, Client) {
    let server = MatrixMockServer::new().await;
    let client = server.client_builder().build().await;
    (server, client)
}

/// Build one of our `TimelineItem`s for seeding the in-memory timeline cache that
/// the rooms-list builder scans. `secs` is the offset from the epoch so ordering
/// is explicit; `deleted` exercises the redacted-skip path.
pub(super) fn cache_text_item(
    event_id: &str,
    body: &str,
    secs: u64,
    deleted: bool,
) -> TimelineItem {
    cache_item(
        event_id,
        MessageContent::Text {
            body: body.into(),
            formatted_body: None,
        },
        secs,
        deleted,
    )
}

pub(super) fn cache_service_item(event_id: &str, body: &str, secs: u64) -> TimelineItem {
    cache_item(
        event_id,
        MessageContent::Service { body: body.into() },
        secs,
        false,
    )
}

fn cache_item(event_id: &str, content: MessageContent, secs: u64, deleted: bool) -> TimelineItem {
    TimelineItem {
        event_id: event_id.into(),
        transaction_id: None,
        sender: UserProfile {
            user_id: "@alice:localhost".into(),
            display_name: "Alice".into(),
            avatar_url: None,
        },
        timestamp: UNIX_EPOCH + Duration::from_secs(secs),
        content,
        reply_to_event_id: None,
        reply_preview: None,
        forwarded_from: None,
        is_edited: false,
        is_pinned: false,
        reactions: Vec::new(),
        send_state: SendState::Sent,
        upload_progress: 0.0,
        is_outgoing: false,
        is_deleted: deleted,
        url_preview: None,
        is_encrypted: false,
        decryption_error: None,
    }
}

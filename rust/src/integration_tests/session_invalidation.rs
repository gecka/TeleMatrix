// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! A homeserver that rejects our access token must reach the watcher.
//!
//! The whole forced-signout path hangs off one signal: the SDK broadcasting
//! `SessionChange::UnknownToken` when a request comes back `M_UNKNOWN_TOKEN`.
//! These drive that end to end against a mock homeserver — a real 401 with a
//! real error body, through the SDK's own plumbing, into
//! [`wait_for_invalidation`].

use std::time::Duration;

use serde_json::json;
use wiremock::ResponseTemplate;

use super::common::mock_server_and_client;
use crate::session_invalidation::wait_for_invalidation;

/// `M_UNKNOWN_TOKEN` body as a homeserver sends it. `soft_logout` is omitted
/// when false, exactly like Synapse.
fn unknown_token_response(soft_logout: bool) -> ResponseTemplate {
    let body = if soft_logout {
        json!({
            "errcode": "M_UNKNOWN_TOKEN",
            "error": "Soft logged out",
            "soft_logout": true,
        })
    } else {
        json!({
            "errcode": "M_UNKNOWN_TOKEN",
            "error": "Access token has been deleted",
        })
    };
    ResponseTemplate::new(401).set_body_json(body)
}

#[tokio::test]
async fn deleted_session_invalidates_the_client() {
    let (server, client) = mock_server_and_client().await;
    server
        .mock_who_am_i()
        .respond_with(unknown_token_response(false))
        .mount()
        .await;

    // Subscribe before the rejected request: the broadcast has no replay.
    let mut rx = client.subscribe_to_session_changes();
    client
        .whoami()
        .await
        .expect_err("the mocked server rejects the token");

    let invalidated = tokio::time::timeout(Duration::from_secs(5), wait_for_invalidation(&mut rx))
        .await
        .expect("the watcher should resolve once the server rejects the token");
    assert_eq!(invalidated, Some(false));
}

#[tokio::test]
async fn soft_logout_flag_survives_the_round_trip() {
    let (server, client) = mock_server_and_client().await;
    server
        .mock_who_am_i()
        .respond_with(unknown_token_response(true))
        .mount()
        .await;

    let mut rx = client.subscribe_to_session_changes();
    client
        .whoami()
        .await
        .expect_err("the mocked server rejects the token");

    let invalidated = tokio::time::timeout(Duration::from_secs(5), wait_for_invalidation(&mut rx))
        .await
        .expect("the watcher should resolve once the server rejects the token");
    assert_eq!(invalidated, Some(true));
}

/// The failure mode that would cost a user their session: a plain server error
/// is NOT proof the token died, and must never resolve the watcher. This is the
/// distinction the sync loop cannot make on its own.
#[tokio::test]
async fn a_server_error_does_not_invalidate_the_client() {
    let (server, client) = mock_server_and_client().await;
    server
        .mock_who_am_i()
        .respond_with(ResponseTemplate::new(500))
        .mount()
        .await;

    let mut rx = client.subscribe_to_session_changes();
    client.whoami().await.expect_err("the mocked server fails");

    let outcome =
        tokio::time::timeout(Duration::from_millis(500), wait_for_invalidation(&mut rx)).await;
    assert!(
        outcome.is_err(),
        "a 500 must leave the watcher pending, got {outcome:?}"
    );
}

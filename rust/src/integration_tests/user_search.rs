// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Searching for a complete user id must find the user even when the server's
//! user directory refuses to admit they exist.
//!
//! Synapse only lists someone in `/user_directory/search` if they are in a
//! published room or already share one with us (`search_all_users` is off by
//! default, including on matrix.org). So pasting the id of a real account you
//! have never met returned an empty list, which reads as "no such user".
//! `/profile/{user_id}` has no such restriction, so a complete id falls back to
//! it and is offered as the first result.

use matrix_sdk::ruma::profile::ProfileFieldValue;
use matrix_sdk::ruma::user_id;
use matrix_sdk::test_utils::mocks::MatrixMockServer;
use serde_json::json;
use wiremock::matchers::{method, path};
use wiremock::{Mock, ResponseTemplate};

use super::common::mock_server_and_client;
use crate::room_member_service::RoomMemberService;

/// The id the mocked directory never returns.
const UNLISTED: &str = "@supergecka:matrix.org";

/// A directory that answers every query with one unrelated user — the shape of
/// the real problem. (The SDK's prebuilt `mock_user_directory` matches one
/// hardcoded request body, so it cannot serve arbitrary search terms.)
async fn mock_directory_ignoring_the_query(server: &MatrixMockServer) {
    Mock::given(method("POST"))
        .and(path("/_matrix/client/v3/user_directory/search"))
        .respond_with(ResponseTemplate::new(200).set_body_json(json!({
            "limited": false,
            "results": [{
                "user_id": "@test:example.me",
                "display_name": "Test",
                "avatar_url": "mxc://example.me/someid"
            }]
        })))
        .mount(server.server())
        .await;
}

#[tokio::test]
async fn a_complete_id_missing_from_the_directory_is_resolved_via_the_profile() {
    let (server, client) = mock_server_and_client().await;
    // The canned directory response contains @test:example.me and nothing else,
    // which is the whole problem: our target is a real account it never lists.
    mock_directory_ignoring_the_query(&server).await;
    server
        .mock_get_profile(user_id!("@supergecka:matrix.org"))
        .ok_with_fields(vec![ProfileFieldValue::DisplayName(
            "SuperGecka".to_owned(),
        )])
        .mount()
        .await;

    let (results, _limited) = RoomMemberService::search_user_directory(client, UNLISTED, 10)
        .await
        .expect("search succeeds");

    let first = results.first().expect("the typed id is offered");
    assert_eq!(first.user_id, UNLISTED, "exact match must come first");
    assert_eq!(
        first.display_name, "SuperGecka",
        "the real profile name, not the bare localpart"
    );
    // The directory's own hit is kept, not replaced.
    assert!(results.iter().any(|u| u.user_id == "@test:example.me"));
}

#[tokio::test]
async fn an_unresolvable_id_is_still_offered() {
    let (server, client) = mock_server_and_client().await;
    mock_directory_ignoring_the_query(&server).await;
    // No profile mock: the lookup 404s, standing in for a restricted profile or
    // a server we cannot federate with. Inviting such an id is still legal, so
    // withholding the row would leave the user no way to reach them at all.

    let (results, _limited) = RoomMemberService::search_user_directory(client, UNLISTED, 10)
        .await
        .expect("search succeeds");

    let first = results.first().expect("the typed id is offered anyway");
    assert_eq!(first.user_id, UNLISTED);
    assert_eq!(
        first.display_name, "supergecka",
        "falls back to the localpart when the profile cannot be fetched"
    );
    assert!(first.avatar_url.is_none());
}

#[tokio::test]
async fn a_name_search_is_left_to_the_directory() {
    let (server, client) = mock_server_and_client().await;
    mock_directory_ignoring_the_query(&server).await;

    // Not a complete id, so nothing is invented: whatever the directory says
    // stands on its own.
    let (results, _limited) = RoomMemberService::search_user_directory(client, "supergecka", 10)
        .await
        .expect("search succeeds");

    assert_eq!(results.len(), 1);
    assert_eq!(results[0].user_id, "@test:example.me");
}

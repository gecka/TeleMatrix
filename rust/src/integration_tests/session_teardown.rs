// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! The SDK `Client` must actually drop when a session ends.
//!
//! `Client` is an `Arc<ClientInner>`, and `ClientInner` owns BOTH the four
//! encrypted SQLite stores and the event-handler store. A handler closure that
//! captures a strong `Client` is therefore a reference cycle through the very
//! `Arc` it lives in: `ClientInner` is never dropped and the sqlite files stay
//! open for the whole process lifetime.
//!
//! On POSIX that leak is invisible — `rename`/`unlink` work on open files, so
//! logout and the next login still wipe the store. On Windows an open handle
//! makes both fail, so logout's rename-aside and the fresh-login store wipe
//! error out and every later sign-in fails (as a generic "check your
//! credentials") until the app is restarted.
//!
//! Handlers must take `client: Client` as an injected argument
//! (`EventHandlerContext`) instead of capturing one.
//!
//! A live verification flow pins the same `Arc` a second way — its watcher tasks
//! hold `Client` clones — which only `logout`'s verification reset releases; the
//! tests at the bottom drive a real `logout` to hold that call site in place.

use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use matrix_sdk::test_utils::mocks::MatrixMockServer;
use matrix_sdk::Client;
use serde_json::json;
use wiremock::{Request, Respond, ResponseTemplate};

use super::common::mock_server_and_client;
use crate::matrix::MatrixProtocol;
use crate::verification_service::VerificationService;

/// Wait (briefly) for `sentinel` to become uniquely owned.
///
/// The sentinel is handed to the client as an event-handler context, so the
/// client's internal store holds the only other reference: it drops exactly
/// when `ClientInner` drops. Polling rather than asserting immediately keeps
/// the test honest about the SDK's asynchronous teardown instead of trading a
/// leak check for a race.
async fn client_dropped_within(sentinel: &Arc<()>, attempts: u32) -> bool {
    for _ in 0..attempts {
        if Arc::strong_count(sentinel) == 1 {
            return true;
        }
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
    false
}

async fn client_dropped(sentinel: &Arc<()>) -> bool {
    client_dropped_within(sentinel, 100).await
}

/// Hand the client a sentinel it will own for exactly as long as `ClientInner`
/// lives.
fn sentinel_for(client: &Client) -> Arc<()> {
    let sentinel = Arc::new(());
    client.add_event_handler_context(sentinel.clone());
    sentinel
}

/// Control: proves the probe can observe a healthy drop. If this ever fails,
/// the leak tests below are meaningless rather than passing.
#[tokio::test]
async fn bare_client_drops_cleanly() {
    let (_server, client) = mock_server_and_client().await;
    let sentinel = sentinel_for(&client);

    drop(client);

    assert!(
        client_dropped(&sentinel).await,
        "probe is broken: a client with no handlers of ours still leaked"
    );
}

#[tokio::test]
async fn verification_handlers_do_not_leak_the_client() {
    let (_server, client) = mock_server_and_client().await;
    let sentinel = sentinel_for(&client);

    let verification = VerificationService::new();
    verification.register_incoming_request_handler(&client);
    verification.register_incoming_user_request_handler(&client);

    drop(client);

    assert!(
        client_dropped(&sentinel).await,
        "verification handlers captured a strong Client: ClientInner is kept \
         alive by its own event-handler store, so the sqlite stores stay open \
         and logout/login cannot wipe them on Windows"
    );
}

/// Answers `POST /logout` and, while doing so, bumps the auth generation —
/// standing in for a sign-in that begins while teardown is in flight. `logout`
/// then takes its documented early return and skips the destructive tail, which
/// no unit test may run: it wipes the store directory and deletes this account's
/// OS-keychain secrets. Everything these tests are about — the verification
/// reset and the release of the client — happens well before that point.
struct SignInDuringTeardown(Arc<AtomicU64>);

impl Respond for SignInDuringTeardown {
    fn respond(&self, _request: &Request) -> ResponseTemplate {
        self.0.fetch_add(1, Ordering::SeqCst);
        ResponseTemplate::new(200).set_body_json(json!({}))
    }
}

/// A protocol holding `client` as its live session, pointed at a scratch data
/// dir (`MatrixProtocol::new` opens, and logout would wipe, whatever it is
/// given). Returns the auth generation, so a test can prove the mocked logout
/// actually fired, and the dir to remove afterwards.
async fn protocol_with_session(
    server: &MatrixMockServer,
    client: Client,
    tag: &str,
) -> (MatrixProtocol, Arc<AtomicU64>, PathBuf) {
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    let data_dir = std::env::temp_dir().join(format!(
        "telematrix-teardown-{tag}-{nanos}-{}",
        std::process::id()
    ));
    std::fs::create_dir_all(&data_dir).expect("scratch data dir");

    let protocol = MatrixProtocol::new(tokio::runtime::Handle::current(), data_dir.clone());
    let generation = protocol.auth_generation_for_test();
    server
        .mock_logout()
        .respond_with(SignInDuringTeardown(generation.clone()))
        .mount()
        .await;
    protocol.set_client_for_test(client).await;
    (protocol, generation, data_dir)
}

/// Control for the test below: the harness itself releases the client, so a
/// failure there is attributable to the verification watcher and not to logout
/// (or this setup) leaking the session on its own.
#[tokio::test]
async fn logout_releases_a_client_with_no_verification_flow() {
    let (server, client) = mock_server_and_client().await;
    let sentinel = sentinel_for(&client);
    let (protocol, generation, data_dir) =
        protocol_with_session(&server, client.clone(), "no-flow").await;
    drop(client);

    protocol.logout().await.expect("logout should succeed");
    assert_eq!(
        generation.load(Ordering::SeqCst),
        1,
        "the mocked logout never fired, so logout ran its destructive tail"
    );
    assert!(
        !data_dir.join(".trash").exists(),
        "logout ran its destructive tail"
    );

    assert!(
        client_dropped(&sentinel).await,
        "logout leaks the session even with nothing pinning it"
    );
    let _ = std::fs::remove_dir_all(&data_dir);
}

// The verification ctx pins a Client through its watchers (a live flow's
// request/SAS/QR objects each hold a clone, and every watcher task holds its
// own). `logout` must reset that ctx while it still owns the session, or its
// `drop(client)` is not the last reference and the store wipe runs against open
// sqlite handles — invisible on POSIX, and on Windows a sign-in that fails with
// a generic "check your credentials" until the app restarts.
//
// `verification_service` already pins what `reset_context` *does*; this pins the
// single, eminently deletable line in `logout` that calls it.
#[tokio::test]
async fn logout_releases_a_client_pinned_by_a_verification_watcher() {
    let (server, client) = mock_server_and_client().await;
    let sentinel = sentinel_for(&client);
    let (protocol, generation, data_dir) =
        protocol_with_session(&server, client.clone(), "pinned").await;

    let pinned = client.clone();
    protocol
        .verification_for_test()
        .seed_watcher_for_test(tokio::spawn(async move {
            let _pinned = pinned;
            // Stands in for a watcher parked on a `changes()` stream: nothing
            // but the abort in `reset_context` ends it.
            std::future::pending::<()>().await;
        }))
        .await;
    drop(client);

    // The protocol still holds the session at this point, so this only shows the
    // probe is not trivially satisfied — the watcher's own pin is what the
    // assertion after logout is about.
    assert!(
        !client_dropped_within(&sentinel, 5).await,
        "probe is broken: the client dropped while the protocol still held it"
    );

    protocol.logout().await.expect("logout should succeed");
    assert_eq!(
        generation.load(Ordering::SeqCst),
        1,
        "the mocked logout never fired, so logout ran its destructive tail"
    );
    assert!(
        !data_dir.join(".trash").exists(),
        "logout ran its destructive tail"
    );

    assert!(
        client_dropped(&sentinel).await,
        "logout did not reset the verification context: a watcher still holds a \
         Client clone, so the sqlite stores stay open across the wipe and every \
         later sign-in fails on Windows"
    );
    let _ = std::fs::remove_dir_all(&data_dir);
}

// The untracked half of the same pin: a banner watcher for an incoming
// request also holds a `Client` clone, in a vec `reset_context` never touches
// (`banner_watchers` outlives flow changes on purpose). `logout` must abort it
// separately — this pins that single, equally deletable line.
#[tokio::test]
async fn logout_releases_a_client_pinned_by_a_banner_watcher() {
    let (server, client) = mock_server_and_client().await;
    let sentinel = sentinel_for(&client);
    let (protocol, generation, data_dir) =
        protocol_with_session(&server, client.clone(), "banner-pinned").await;

    let pinned = client.clone();
    protocol
        .verification_for_test()
        .seed_banner_watcher_for_test(tokio::spawn(async move {
            let _pinned = pinned;
            // Stands in for a banner watcher parked on an incoming request's
            // changes: nothing but the abort in `abort_banner_watchers` ends it.
            std::future::pending::<()>().await;
        }));
    drop(client);

    assert!(
        !client_dropped_within(&sentinel, 5).await,
        "probe is broken: the client dropped while the protocol still held it"
    );

    protocol.logout().await.expect("logout should succeed");
    assert_eq!(
        generation.load(Ordering::SeqCst),
        1,
        "the mocked logout never fired, so logout ran its destructive tail"
    );
    assert!(
        !data_dir.join(".trash").exists(),
        "logout ran its destructive tail"
    );

    assert!(
        client_dropped(&sentinel).await,
        "logout did not abort banner watchers: one still holds a Client clone, \
         so the sqlite stores stay open across the wipe and every later sign-in \
         fails on Windows"
    );
    let _ = std::fs::remove_dir_all(&data_dir);
}

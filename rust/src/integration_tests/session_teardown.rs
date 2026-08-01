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

use std::sync::atomic::AtomicU32;
use std::sync::Arc;
use std::time::Duration;

use matrix_sdk::Client;

use super::common::mock_server_and_client;
use crate::verification_service::VerificationService;

/// Wait (briefly) for `sentinel` to become uniquely owned.
///
/// The sentinel is handed to the client as an event-handler context, so the
/// client's internal store holds the only other reference: it drops exactly
/// when `ClientInner` drops. Polling rather than asserting immediately keeps
/// the test honest about the SDK's asynchronous teardown instead of trading a
/// leak check for a race.
async fn client_dropped(sentinel: &Arc<()>) -> bool {
    for _ in 0..100 {
        if Arc::strong_count(sentinel) == 1 {
            return true;
        }
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
    false
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

    let verification = VerificationService::new(Arc::new(AtomicU32::new(0)));
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

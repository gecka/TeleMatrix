// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Remote session invalidation ("you were signed out on another device").
//!
//! When the homeserver rejects our access token with `M_UNKNOWN_TOKEN` — the
//! device was deleted from another client, an admin removed it, a password
//! change revoked it, or an OAuth refresh was denied — the SDK broadcasts
//! [`SessionChange::UnknownToken`] on [`matrix_sdk::Client::subscribe_to_session_changes`].
//! Nothing used to subscribe, so the session simply died in place: the sliding
//! sync service restarted forever against a dead token and the UI sat on
//! "Waiting for network…".
//!
//! Acting on this destroys local data, so it matters *why* this signal is safe
//! to act on where a failed sync is not: `ErrorKind::UnknownToken` is parsed
//! from the server's own `errcode` JSON body. A timeout, a 5xx, a DNS failure
//! or a captive portal can never produce it. It is positive proof from the
//! server that this token is dead.
//!
//! The pure [`classify_session_change`] decision is kept separate so it is
//! unit-testable, mirroring [`crate::new_login_service::should_alert_new_login`].

use std::sync::{Arc, Mutex};

use matrix_sdk::SessionChange;
use tokio::sync::broadcast;
use tracing::warn;

/// The session-invalidated callback. Args: whether the server flagged this as a
/// [soft logout] (the device survives and re-login with the same device id would
/// keep the E2EE keys).
///
/// [soft logout]: https://spec.matrix.org/v1.18/client-server-api/#soft-logout
pub(crate) type SessionInvalidatedFn = Box<dyn Fn(bool) + Send>;
/// Storage slot for the (per-session) session-invalidated callback.
pub(crate) type SessionInvalidatedCallbackSlot = Arc<Mutex<Option<SessionInvalidatedFn>>>;

/// What the watcher should do with one item off the session-change broadcast.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum SessionChangeAction {
    /// The homeserver rejected the access token: sign this account out.
    ForceSignOut { soft_logout: bool },
    /// Nothing to do.
    Ignore,
}

/// Decide what a session change means for us.
pub(crate) fn classify_session_change(change: &SessionChange) -> SessionChangeAction {
    match change {
        SessionChange::UnknownToken(data) => SessionChangeAction::ForceSignOut {
            soft_logout: data.soft_logout,
        },
        SessionChange::TokensRefreshed => SessionChangeAction::Ignore,
    }
}

/// Wait for the homeserver to declare this access token dead.
///
/// Resolves to `Some(soft_logout)` the first time the server answers any request
/// with `M_UNKNOWN_TOKEN`, or `None` if the channel closes first (the client was
/// dropped). Token refreshes are skipped, not returned.
///
/// Takes an already-subscribed receiver rather than the `Client`: the broadcast
/// does not replay, so the subscription has to exist *before* the request that
/// gets rejected. Subscribing at the call site and polling here closes the
/// window where a 401 lands between spawning the watcher and its first poll.
pub(crate) async fn wait_for_invalidation(
    rx: &mut broadcast::Receiver<SessionChange>,
) -> Option<bool> {
    loop {
        match rx.recv().await {
            Ok(change) => match classify_session_change(&change) {
                SessionChangeAction::ForceSignOut { soft_logout } => return Some(soft_logout),
                SessionChangeAction::Ignore => continue,
            },
            // A burst of in-flight requests can all fail at once and overrun the
            // channel. Whatever was dropped, the token is no better for it —
            // keep listening; the next rejected request re-broadcasts.
            Err(broadcast::error::RecvError::Lagged(skipped)) => {
                warn!("session-change stream lagged, {skipped} change(s) dropped");
                continue;
            }
            Err(broadcast::error::RecvError::Closed) => return None,
        }
    }
}

/// Fire the session-invalidated callback. No-op if none is installed.
pub(crate) fn emit(slot: &SessionInvalidatedCallbackSlot, soft_logout: bool) {
    if let Ok(guard) = slot.lock() {
        if let Some(cb) = guard.as_ref() {
            cb(soft_logout);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use matrix_sdk::ruma::api::error::UnknownTokenErrorData;

    fn unknown_token(soft_logout: bool) -> SessionChange {
        let mut data = UnknownTokenErrorData::new();
        data.soft_logout = soft_logout;
        SessionChange::UnknownToken(data)
    }

    #[test]
    fn unknown_token_forces_sign_out() {
        assert_eq!(
            classify_session_change(&unknown_token(false)),
            SessionChangeAction::ForceSignOut { soft_logout: false }
        );
    }

    #[test]
    fn unknown_token_carries_the_soft_logout_flag() {
        assert_eq!(
            classify_session_change(&unknown_token(true)),
            SessionChangeAction::ForceSignOut { soft_logout: true }
        );
    }

    // The dangerous confusion: a routine token refresh shares the same broadcast
    // channel. Treating it as an invalidation would wipe a perfectly good
    // session every time the access token rotated.
    #[test]
    fn refreshed_tokens_never_sign_out() {
        assert_eq!(
            classify_session_change(&SessionChange::TokensRefreshed),
            SessionChangeAction::Ignore
        );
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, Instant};

use anyhow::{anyhow, Result};
use futures_util::StreamExt;
use matrix_sdk::encryption::identities::UserIdentity;
use matrix_sdk::encryption::verification::{
    QrVerification, QrVerificationState, SasState, SasVerification, VerificationRequest,
    VerificationRequestState,
};
use matrix_sdk::ruma::events::key::verification::request::ToDeviceKeyVerificationRequestEventContent;
use matrix_sdk::ruma::events::key::verification::VerificationMethod;
use matrix_sdk::ruma::events::room::message::{MessageType, OriginalSyncRoomMessageEvent};
use matrix_sdk::ruma::events::ToDeviceEvent;
use matrix_sdk::ruma::{OwnedUserId, UserId};
use matrix_sdk::{Client, Room};
use tracing::{info, warn};

use crate::types::{
    QrCodeImage, SasEmoji, UserTrustState, VerificationCapabilities, VerificationState,
};

type VerificationStateCallback = Box<dyn Fn(u32, &str) + Send>;
type IncomingVerificationRequestCallback = Box<dyn Fn(&str, &str, &str) + Send>;
type UserTrustChangedCallback = Box<dyn Fn(&str, u32) + Send>;
type IncomingUserVerificationRequestCallback = Box<dyn Fn(&str, &str, &str) + Send>;
/// Fires with a flow id when an incoming request can no longer be answered.
type VerificationRequestClosedCallback = Box<dyn Fn(&str) + Send>;
/// Fires with the flow id whenever a SAS reaches the compare-emoji stage.
type SasEmojisCallback = Box<dyn Fn(&str, &[SasEmoji]) + Send>;
/// Fires with the flow id when a QR code has been generated and rendered.
type QrDataCallback = Box<dyn Fn(&str, &QrCodeImage) + Send>;
/// Fires with (flow_id, cancel_code, cancelled_by_us) just before the
/// corresponding `Cancelled` state, so the UI can pick a severity for it.
type VerificationCancelInfoCallback = Box<dyn Fn(&str, &str, bool) + Send>;

macro_rules! verification_debug {
    ($($arg:tt)*) => {{
        let message = format!($($arg)*);
        tracing::warn!(target: "telematrix::verification", "{}", message);
    }};
}

fn lock_verification_mutex<'a, T>(mutex: &'a Mutex<T>, name: &str) -> MutexGuard<'a, T> {
    match mutex.lock() {
        Ok(guard) => guard,
        Err(poisoned) => {
            warn!("Recovering poisoned verification mutex: {name}");
            poisoned.into_inner()
        }
    }
}

/// Which method this device wants once the request reaches Ready; the request
/// watcher acts on it.
#[derive(Clone, Copy, PartialEq, Eq)]
enum DesiredMethod {
    Sas,
    Qr,
}

/// Internal state for an active sign-in verification flow.
struct VerificationContext {
    request: Option<VerificationRequest>,
    sas: Option<SasVerification>,
    qr: Option<QrVerification>,
    /// Generation counter to detect stale callbacks.
    generation: u64,
    desired: Option<DesiredMethod>,
    /// Bumped when `sas` is replaced so a superseded SAS watcher (lost
    /// start-race tie-break) can tell it must stay silent.
    sas_token: u64,
    /// Same for `qr`: a QR can be displaced inside one flow+generation (peer
    /// started SAS, or a duplicated `Ready`), which flow id alone cannot detect.
    qr_token: u64,
    /// Watcher tasks for this flow; aborted on reset.
    watchers: Vec<tokio::task::JoinHandle<()>>,
}

impl VerificationContext {
    fn new() -> Self {
        Self {
            request: None,
            sas: None,
            qr: None,
            generation: 0,
            desired: None,
            sas_token: 0,
            qr_token: 0,
            watchers: Vec::new(),
        }
    }

    fn reset(&mut self) {
        for handle in self.watchers.drain(..) {
            handle.abort();
        }
        self.request = None;
        self.sas = None;
        self.qr = None;
        self.desired = None;
        self.generation += 1;
        self.sas_token += 1;
        self.qr_token += 1;
    }
}

/// An incoming request remembered so it can be replayed to a consumer that
/// attaches after it arrived (main window built later, intro, a fresh
/// account switch) — matrix-sdk 0.18 has no pending-request enumeration API.
#[derive(Clone)]
struct PendingIncomingRequest {
    flow_id: String,
    is_user: bool,
    /// Device id (self-verification) or user id (cross-user).
    counterpart_id: String,
    display_label: String,
}

#[derive(Clone)]
pub(crate) struct VerificationService {
    ctx: Arc<tokio::sync::Mutex<VerificationContext>>,
    state_callback: Arc<Mutex<Option<VerificationStateCallback>>>,
    incoming_request_callback: Arc<Mutex<Option<IncomingVerificationRequestCallback>>>,
    /// Fires when another *user's* cross-signing identity trust changes, so the
    /// UI can keep trust shields live.
    user_trust_changed_callback: Arc<Mutex<Option<UserTrustChangedCallback>>>,
    /// Fires when ANOTHER user requests to verify with us (in-room). Distinct
    /// from `incoming_request_callback`, which is our own other devices
    /// (to-device).
    incoming_user_request_callback: Arc<Mutex<Option<IncomingUserVerificationRequestCallback>>>,
    /// Fires when an incoming request stops being answerable — another of our
    /// sessions accepted it, the requester withdrew it, or it expired. The UI
    /// uses it to take down the request banner, which otherwise sits there
    /// offering to accept a request that no longer exists.
    request_closed_callback: Arc<Mutex<Option<VerificationRequestClosedCallback>>>,
    /// Fires when a SAS reaches the compare stage. Separate from the state
    /// callback because the emojis can arrive at any time — including from a SAS
    /// the *other* device started — so no start call can carry them back.
    sas_emojis_callback: Arc<Mutex<Option<SasEmojisCallback>>>,
    /// Fires when a QR code has been generated, for the same reason.
    qr_data_callback: Arc<Mutex<Option<QrDataCallback>>>,
    /// Fires with the SDK cancel code just before the `Cancelled` state it
    /// pertains to, so the UI can distinguish a security failure (key
    /// mismatch) from an ordinary cancellation.
    cancel_info_callback: Arc<Mutex<Option<VerificationCancelInfoCallback>>>,
    /// Serializes outgoing start attempts so two near-simultaneous starts
    /// (e.g. tapping "emoji" then "QR") cannot create two requests that clobber
    /// each other's `ctx.request` and strand one flow.
    start_guard: Arc<tokio::sync::Mutex<()>>,
    /// Flow id of the currently-active verification flow, tagged onto every
    /// emitted state so the UI can attribute a state (notably `Cancelled`) to
    /// the flow it belongs to and ignore stale states from a torn-down flow.
    /// Deliberately NOT cleared on reset, so a `Cancelled` emitted right after a
    /// reset still carries the just-ended flow's id; it is overwritten when the
    /// next flow starts.
    current_flow_id: Arc<Mutex<String>>,
    /// The most recent incoming request still worth replaying to a consumer
    /// that attaches late. Cleared once the request it names stops being
    /// answerable, so a replay can never resurrect a dead or stale request.
    pending_incoming: Arc<Mutex<Option<PendingIncomingRequest>>>,
}

impl VerificationService {
    pub(crate) fn new() -> Self {
        Self {
            ctx: Arc::new(tokio::sync::Mutex::new(VerificationContext::new())),
            state_callback: Arc::new(Mutex::new(None)),
            incoming_request_callback: Arc::new(Mutex::new(None)),
            user_trust_changed_callback: Arc::new(Mutex::new(None)),
            incoming_user_request_callback: Arc::new(Mutex::new(None)),
            request_closed_callback: Arc::new(Mutex::new(None)),
            sas_emojis_callback: Arc::new(Mutex::new(None)),
            qr_data_callback: Arc::new(Mutex::new(None)),
            cancel_info_callback: Arc::new(Mutex::new(None)),
            start_guard: Arc::new(tokio::sync::Mutex::new(())),
            current_flow_id: Arc::new(Mutex::new(String::new())),
            pending_incoming: Arc::new(Mutex::new(None)),
        }
    }

    /// Register a callback that fires when the verification state changes.
    pub(crate) fn on_state_changed(&self, callback: VerificationStateCallback) {
        let mut cb = lock_verification_mutex(&self.state_callback, "verification_state_callback");
        *cb = Some(callback);
    }

    /// Register a callback that fires when another own device requests verification.
    pub(crate) fn on_incoming_request(&self, callback: IncomingVerificationRequestCallback) {
        let mut cb = lock_verification_mutex(
            &self.incoming_request_callback,
            "incoming_verification_request_callback",
        );
        *cb = Some(callback);
    }

    /// Register a callback that fires when another user's identity trust changes.
    pub(crate) fn on_user_trust_changed(&self, callback: UserTrustChangedCallback) {
        let mut cb = lock_verification_mutex(
            &self.user_trust_changed_callback,
            "user_trust_changed_callback",
        );
        *cb = Some(callback);
    }

    /// Register a callback that fires when another user requests to verify with us.
    pub(crate) fn on_incoming_user_request(
        &self,
        callback: IncomingUserVerificationRequestCallback,
    ) {
        let mut cb = lock_verification_mutex(
            &self.incoming_user_request_callback,
            "incoming_user_verification_request_callback",
        );
        *cb = Some(callback);
    }

    /// Register a callback that fires when an incoming request can no longer be
    /// answered (accepted elsewhere, withdrawn, or expired).
    pub(crate) fn on_request_closed(&self, callback: VerificationRequestClosedCallback) {
        let mut cb = lock_verification_mutex(
            &self.request_closed_callback,
            "verification_request_closed_callback",
        );
        *cb = Some(callback);
    }

    /// Register a callback that fires when a SAS flow's emojis become available.
    pub(crate) fn on_sas_emojis(&self, callback: SasEmojisCallback) {
        let mut cb = lock_verification_mutex(&self.sas_emojis_callback, "sas_emojis_callback");
        *cb = Some(callback);
    }

    /// Register a callback that fires when a QR code has been generated.
    pub(crate) fn on_qr_data(&self, callback: QrDataCallback) {
        let mut cb = lock_verification_mutex(&self.qr_data_callback, "qr_data_callback");
        *cb = Some(callback);
    }

    /// Register a callback that fires with a flow's SDK cancel code just
    /// before its `Cancelled` state.
    pub(crate) fn on_cancel_info(&self, callback: VerificationCancelInfoCallback) {
        let mut cb = lock_verification_mutex(&self.cancel_info_callback, "cancel_info_callback");
        *cb = Some(callback);
    }

    pub(crate) fn clear_callbacks(&self) {
        {
            let mut cb =
                lock_verification_mutex(&self.state_callback, "verification_state_callback");
            *cb = None;
        }
        {
            let mut cb = lock_verification_mutex(
                &self.incoming_request_callback,
                "incoming_verification_request_callback",
            );
            *cb = None;
        }
        {
            let mut cb = lock_verification_mutex(
                &self.user_trust_changed_callback,
                "user_trust_changed_callback",
            );
            *cb = None;
        }
        {
            let mut cb = lock_verification_mutex(
                &self.incoming_user_request_callback,
                "incoming_user_verification_request_callback",
            );
            *cb = None;
        }
        {
            let mut cb = lock_verification_mutex(
                &self.request_closed_callback,
                "verification_request_closed_callback",
            );
            *cb = None;
        }
        {
            let mut cb = lock_verification_mutex(&self.sas_emojis_callback, "sas_emojis_callback");
            *cb = None;
        }
        {
            let mut cb = lock_verification_mutex(&self.qr_data_callback, "qr_data_callback");
            *cb = None;
        }
        {
            let mut cb =
                lock_verification_mutex(&self.cancel_info_callback, "cancel_info_callback");
            *cb = None;
        }
        {
            // This service outlives one login: `clear_callbacks` runs on logout
            // against the SAME instance the next sign-in reuses. Without this, a
            // request remembered under the departing session could be replayed
            // into the next one once a consumer re-attaches.
            let mut pending = lock_verification_mutex(&self.pending_incoming, "pending_incoming");
            *pending = None;
        }
    }

    fn emit_sas_emojis(&self, flow_id: &str, emojis: &[SasEmoji]) {
        let cb = lock_verification_mutex(&self.sas_emojis_callback, "sas_emojis_callback");
        if let Some(ref f) = *cb {
            f(flow_id, emojis);
        }
    }

    fn emit_qr_data(&self, flow_id: &str, image: &QrCodeImage) {
        let cb = lock_verification_mutex(&self.qr_data_callback, "qr_data_callback");
        if let Some(ref f) = *cb {
            f(flow_id, image);
        }
    }

    fn emit_cancel_info(&self, flow_id: &str, cancel_code: &str, cancelled_by_us: bool) {
        let cb = lock_verification_mutex(&self.cancel_info_callback, "cancel_info_callback");
        if let Some(ref f) = *cb {
            f(flow_id, cancel_code, cancelled_by_us);
        }
    }

    /// Emit a user-trust-changed signal to the registered callback.
    pub(crate) fn emit_user_trust_changed(&self, user_id: &str, state: u32) {
        let cb = lock_verification_mutex(
            &self.user_trust_changed_callback,
            "user_trust_changed_callback",
        );
        if let Some(cb) = cb.as_ref() {
            cb(user_id, state);
        }
    }

    /// Read another user's current cross-signing trust state from the local
    /// crypto store (no network round-trip). Unknown/absent identity ->
    /// `Unverified`.
    pub(crate) async fn user_trust_state(
        &self,
        client: &Client,
        user_id: &str,
    ) -> Result<UserTrustState> {
        let uid = OwnedUserId::try_from(user_id)
            .map_err(|e| anyhow!("invalid user id '{user_id}': {e}"))?;
        Ok(self.resolve_user_trust(client, &uid).await)
    }

    /// Device-aware trust resolution shared by the on-demand query and the
    /// identity-updates watch, so both agree on what shield to show.
    ///
    /// A verified identity is downgraded to `VerifiedWithWarning` when the user
    /// has any active (non-dehydrated) session that is not cross-signed by their
    /// own identity: we verified *them*, but a message could still arrive from an
    /// untrusted device, so the shield must not read as a clean verified check.
    pub(crate) async fn resolve_user_trust(&self, client: &Client, uid: &UserId) -> UserTrustState {
        // Never surface a trust shield for ourselves. Our own identity isn't
        // something we "verify", and reporting it as Verified would badge our own
        // messages, member row and profile.
        if client.user_id() == Some(uid) {
            return UserTrustState::Unverified;
        }
        let identity = match client.encryption().get_user_identity(uid).await {
            Ok(Some(identity)) => identity,
            _ => return UserTrustState::Unverified,
        };
        let base = UserTrustState::from_flags(
            identity.is_verified(),
            identity.has_verification_violation(),
        );
        // Only a cleanly-verified identity can be downgraded to a warning;
        // Unverified / Violation stand on their own.
        if base != UserTrustState::Verified {
            return base;
        }
        let has_unverified_session = match client.encryption().get_user_devices(uid).await {
            Ok(devices) => devices
                .devices()
                .any(|device| !device.is_dehydrated() && !device.is_cross_signed_by_owner()),
            // If we can't enumerate devices, don't fabricate a warning.
            Err(_) => false,
        };
        if has_unverified_session {
            UserTrustState::VerifiedWithWarning
        } else {
            UserTrustState::Verified
        }
    }

    /// Capture incoming self-verification requests from another session.
    pub(crate) fn register_incoming_request_handler(&self, client: &Client) {
        let verification_ctx = self.ctx.clone();
        let verification_state_callback = self.state_callback.clone();
        let incoming_verification_request_callback = self.incoming_request_callback.clone();
        let request_closed_callback = self.request_closed_callback.clone();
        let pending_incoming = self.pending_incoming.clone();
        verification_debug!("registering incoming self-verification request handler");
        // `client` is an injected handler argument, never a captured one: the
        // handler store lives inside `ClientInner`, so a captured `Client` is a
        // reference cycle that keeps the sqlite stores open forever. See
        // `integration_tests::session_teardown`.
        client.add_event_handler(
            move |ev: ToDeviceEvent<ToDeviceKeyVerificationRequestEventContent>,
                  client: Client| {
                let verification_ctx = verification_ctx.clone();
                let verification_state_callback = verification_state_callback.clone();
                let incoming_verification_request_callback =
                    incoming_verification_request_callback.clone();
                let request_closed_callback = request_closed_callback.clone();
                let pending_incoming = pending_incoming.clone();
                async move {
                    let Some(own_user_id) = client.user_id() else {
                        return;
                    };
                    if ev.sender != own_user_id {
                        return;
                    }
                    let transaction_id = ev.content.transaction_id.to_string();
                    let from_device = ev.content.from_device.to_string();
                    if let Some(own_device_id) = client.device_id() {
                        if from_device == own_device_id.as_str() {
                            verification_debug!(
                                "incoming request ignored, originated from this device flow_id={} from_device={}",
                                transaction_id,
                                from_device
                            );
                            return;
                        }
                    }
                    // No method filter: the list says what the requester
                    // SUPPORTS, not what its user picked, so it cannot route
                    // anything. Filtering on it only hid requests, leaving the
                    // other device waiting on a prompt that never appeared.
                    verification_debug!(
                        "incoming request event sender={} from_device={} flow_id={} methods=[{}]",
                        ev.sender,
                        from_device,
                        transaction_id,
                        Self::verification_methods_debug(&ev.content.methods)
                    );

                    let mut request = None;
                    for attempt in 0..8 {
                        if let Some(found) = client
                            .encryption()
                            .get_verification_request(&ev.sender, &transaction_id)
                            .await
                        {
                            verification_debug!(
                                "incoming request found in crypto store flow_id={} attempt={} state={}",
                                transaction_id,
                                attempt + 1,
                                Self::verification_request_state_details(&found.state())
                            );
                            request = Some(found);
                            break;
                        }
                        verification_debug!(
                            "incoming request not in crypto store yet flow_id={} attempt={}",
                            transaction_id,
                            attempt + 1
                        );
                        tokio::time::sleep(Duration::from_millis(125)).await;
                    }

                    let Some(request) = request else {
                        verification_debug!(
                            "Incoming self-verification request was not available in crypto store, flow_id={transaction_id}"
                        );
                        return;
                    };

                    let device_label = match client.devices().await {
                        Ok(response) => response
                            .devices
                            .into_iter()
                            .find(|device| device.device_id == from_device)
                            .and_then(|device| device.display_name)
                            .filter(|name| !name.trim().is_empty())
                            .unwrap_or_else(|| from_device.clone()),
                        Err(_) => from_device.clone(),
                    };

                    let mut stored_request = false;
                    {
                        let mut ctx = verification_ctx.lock().await;
                        let replace = ctx
                            .request
                            .as_ref()
                            .map(|current| {
                                current.is_done()
                                    || current.is_cancelled()
                                    || matches!(
                                        current.state(),
                                        VerificationRequestState::Created { .. }
                                    )
                            })
                            .unwrap_or(true);
                        verification_debug!(
                            "incoming request merge flow_id={} replace_existing={} incoming_state={}",
                            transaction_id,
                            replace,
                            Self::verification_request_state_details(&request.state())
                        );
                        if replace {
                            // reset() aborts the superseded flow's watchers; a
                            // bare overwrite would leave them emitting.
                            ctx.reset();
                            ctx.request = Some(request.clone());
                            stored_request = true;
                        }
                    }

                    if stored_request {
                        {
                            let mut pending = lock_verification_mutex(
                                &pending_incoming,
                                "pending_incoming",
                            );
                            *pending = Some(PendingIncomingRequest {
                                flow_id: transaction_id.clone(),
                                is_user: false,
                                counterpart_id: from_device.clone(),
                                display_label: device_label.clone(),
                            });
                        }
                        let cb = lock_verification_mutex(
                            &incoming_verification_request_callback,
                            "incoming_verification_request_callback",
                        );
                        if let Some(ref f) = *cb {
                            verification_debug!(
                                "emit incoming verification request flow_id={} device_id={} label={}",
                                transaction_id,
                                from_device,
                                device_label
                            );
                            f(&transaction_id, &from_device, &device_label);
                        }
                        Self::spawn_incoming_request_watcher(
                            request,
                            request_closed_callback,
                            pending_incoming,
                        );
                    }

                    let cb = lock_verification_mutex(
                        &verification_state_callback,
                        "verification_state_callback",
                    );
                    if let Some(ref f) = *cb {
                        verification_debug!(
                            "emit verification state=WaitingForReady from incoming request handler"
                        );
                        f(VerificationState::WaitingForReady as u32, &transaction_id);
                    }
                }
            },
        );
    }

    /// Capture incoming IN-ROOM verification requests from OTHER users
    /// (cross-user verification). Distinct from `register_incoming_request_handler`,
    /// which only handles our own other devices over to-device. Fires the
    /// incoming-user callback with `(flow_id, user_id, display_name)`.
    pub(crate) fn register_incoming_user_request_handler(&self, client: &Client) {
        let verification_ctx = self.ctx.clone();
        let verification_state_callback = self.state_callback.clone();
        let incoming_user_request_callback = self.incoming_user_request_callback.clone();
        let request_closed_callback = self.request_closed_callback.clone();
        let pending_incoming = self.pending_incoming.clone();
        verification_debug!("registering incoming user-verification request handler");
        // Injected, not captured — see `register_incoming_request_handler`.
        client.add_event_handler(
            move |ev: OriginalSyncRoomMessageEvent, room: Room, client: Client| {
                let verification_ctx = verification_ctx.clone();
                let verification_state_callback = verification_state_callback.clone();
                let incoming_user_request_callback = incoming_user_request_callback.clone();
                let request_closed_callback = request_closed_callback.clone();
                let pending_incoming = pending_incoming.clone();
                async move {
                    let Some(own_user_id) = client.user_id() else {
                        return;
                    };
                    // Only OTHER users' requests here; our own devices arrive via
                    // to-device and are handled by the self-verification handler.
                    if ev.sender == own_user_id {
                        return;
                    }
                    let MessageType::VerificationRequest(ref content) = ev.content.msgtype else {
                        return;
                    };
                    // Spec: only respond if we are the named recipient.
                    if content.to != own_user_id {
                        return;
                    }
                    let flow_id = ev.event_id.to_string();
                    let sender = ev.sender.clone();
                    verification_debug!(
                        "incoming user-verification request sender={} flow_id={}",
                        sender,
                        flow_id
                    );

                    let mut request = None;
                    for _ in 0..8 {
                        if let Some(found) = client
                            .encryption()
                            .get_verification_request(&sender, &flow_id)
                            .await
                        {
                            request = Some(found);
                            break;
                        }
                        tokio::time::sleep(Duration::from_millis(125)).await;
                    }
                    let Some(request) = request else {
                        verification_debug!(
                            "incoming user-verification request not in crypto store flow_id={}",
                            flow_id
                        );
                        return;
                    };

                    let display_name = match room.get_member_no_sync(&sender).await {
                        Ok(Some(member)) => member
                            .display_name()
                            .map(|name| name.to_owned())
                            .filter(|name| !name.trim().is_empty())
                            .unwrap_or_else(|| sender.to_string()),
                        _ => sender.to_string(),
                    };

                    let mut stored = false;
                    {
                        let mut ctx = verification_ctx.lock().await;
                        let replace = ctx
                            .request
                            .as_ref()
                            .map(|current| {
                                current.is_done()
                                    || current.is_cancelled()
                                    || matches!(
                                        current.state(),
                                        VerificationRequestState::Created { .. }
                                    )
                            })
                            .unwrap_or(true);
                        if replace {
                            // See the self-verification handler: reset() aborts
                            // the superseded flow's watchers.
                            ctx.reset();
                            ctx.request = Some(request.clone());
                            stored = true;
                        }
                    }

                    if stored {
                        {
                            let mut pending =
                                lock_verification_mutex(&pending_incoming, "pending_incoming");
                            *pending = Some(PendingIncomingRequest {
                                flow_id: flow_id.clone(),
                                is_user: true,
                                counterpart_id: sender.to_string(),
                                display_label: display_name.clone(),
                            });
                        }
                        let cb = lock_verification_mutex(
                            &incoming_user_request_callback,
                            "incoming_user_verification_request_callback",
                        );
                        if let Some(ref f) = *cb {
                            f(&flow_id, sender.as_str(), &display_name);
                        }
                        Self::spawn_incoming_request_watcher(
                            request,
                            request_closed_callback,
                            pending_incoming,
                        );
                    }

                    let cb = lock_verification_mutex(
                        &verification_state_callback,
                        "verification_state_callback",
                    );
                    if let Some(ref f) = *cb {
                        f(VerificationState::WaitingForReady as u32, &flow_id);
                    }
                }
            },
        );
    }

    /// Re-fire the pending incoming request to a freshly-attached consumer.
    /// Consumers attach at different lifetimes (main window, intro); a request
    /// that arrived before any consumer existed would otherwise vanish.
    ///
    /// Deliberately does not also re-emit `WaitingForReady` the way the
    /// incoming handlers do: the request was already answerable when it was
    /// first stored, so nothing about its readiness state has changed here —
    /// only that a new consumer is now listening. A future consumer that
    /// gates on that state would otherwise be silently skipped on replay.
    pub(crate) async fn replay_pending_incoming_request(&self) {
        let pending = lock_verification_mutex(&self.pending_incoming, "pending_incoming").clone();
        let Some(pending) = pending else {
            return;
        };
        let answerable = {
            let ctx = self.ctx.lock().await;
            ctx.request
                .as_ref()
                .map(|r| {
                    r.flow_id() == pending.flow_id && Self::request_state_is_answerable(&r.state())
                })
                .unwrap_or(false)
        };
        if !answerable {
            *lock_verification_mutex(&self.pending_incoming, "pending_incoming") = None;
            return;
        }
        verification_debug!(
            "replaying pending incoming request flow_id={}",
            pending.flow_id
        );
        if pending.is_user {
            let cb = lock_verification_mutex(
                &self.incoming_user_request_callback,
                "incoming_user_verification_request_callback",
            );
            if let Some(ref f) = *cb {
                f(
                    &pending.flow_id,
                    &pending.counterpart_id,
                    &pending.display_label,
                );
            }
        } else {
            let cb = lock_verification_mutex(
                &self.incoming_request_callback,
                "incoming_verification_request_callback",
            );
            if let Some(ref f) = *cb {
                f(
                    &pending.flow_id,
                    &pending.counterpart_id,
                    &pending.display_label,
                );
            }
        }
    }

    /// Answer a specific request with SAS. Initiate-only: the emojis arrive
    /// later through the SAS watcher's `sas_emojis_callback`.
    pub(crate) async fn start_sas_verification_for(&self, expected_flow_id: &str) -> Result<()> {
        // Held for the same reason as the outgoing starts: an accept racing a
        // second start would answer the same request twice.
        let _start_guard = self.start_guard.lock().await;
        let request = self
            .active_verification_request_for(expected_flow_id)
            .await
            .map_err(|e| anyhow!("No matching verification request: {e}"))?;
        self.activate_flow(request, DesiredMethod::Sas).await
    }

    pub(crate) async fn cancel_verification_for(&self, expected_flow_id: &str) -> Result<()> {
        let (request, sas, qr) = {
            let ctx = self.ctx.lock().await;
            let request = ctx
                .request
                .as_ref()
                .ok_or_else(|| anyhow!("No active verification request"))?;
            if request.flow_id() != expected_flow_id {
                return Err(anyhow!(
                    "Active verification flow_id={} does not match expected flow_id={expected_flow_id}",
                    request.flow_id()
                ));
            }
            (request.clone(), ctx.sas.clone(), ctx.qr.clone())
        };

        if let Some(sas) = sas {
            let _ = sas.cancel().await;
        }
        if let Some(qr) = qr {
            let _ = qr.cancel().await;
        }

        if !request.is_done() && !request.is_cancelled() {
            let _ = request.cancel().await;
        }

        let mut ctx = self.ctx.lock().await;
        let still_current = ctx
            .request
            .as_ref()
            .map(|request| request.flow_id() == expected_flow_id)
            .unwrap_or(false);
        if !still_current {
            return Err(anyhow!(
                "Active verification request changed before cancellation completed"
            ));
        }
        ctx.reset();
        drop(ctx);
        self.emit_state(VerificationState::Cancelled);
        Ok(())
    }

    /// Start (or adopt) a self-verification and drive it to SAS. Initiate-only:
    /// the emojis arrive later through the SAS watcher.
    pub(crate) async fn start_sas_verification_checked(
        &self,
        client: Client,
        expected_flow_id: Option<&str>,
    ) -> Result<()> {
        debug_assert!(expected_flow_id.is_none());
        let _start_guard = self.start_guard.lock().await;
        let request = match self.ensure_verification_request(client).await {
            Ok(request) => request,
            Err(e) => {
                self.emit_state(VerificationState::Cancelled);
                self.reset_context_if_current(expected_flow_id).await;
                return Err(e);
            }
        };

        self.activate_flow(request, DesiredMethod::Sas).await
    }

    /// Start an interactive SAS (emoji) verification of ANOTHER user's identity.
    /// Sends an in-room verification request to the user (the SDK routes it
    /// through a shared DM, creating one if none exists) and drives it to SAS
    /// once the peer is ready. On a successful SAS the SDK signs the user's
    /// master key with our user-signing key.
    pub(crate) async fn start_user_verification(
        &self,
        client: Client,
        user_id: &str,
    ) -> Result<()> {
        let _start_guard = self.start_guard.lock().await;
        // `ensure_user_verification_request` performs its own flow-scoped context
        // cleanup on failure, so the outer handler only surfaces the error state;
        // a blanket `reset_context()` here would wipe a concurrent flow (e.g. an
        // incoming request that replaced `ctx`).
        let request = match self
            .ensure_user_verification_request(&client, user_id)
            .await
        {
            Ok(request) => request,
            Err(e) => {
                self.emit_state(VerificationState::Cancelled);
                return Err(e);
            }
        };
        self.activate_flow(request, DesiredMethod::Sas).await
    }

    /// Create + send an outgoing verification request to another user. Fetches
    /// the target's cross-signing identity (cache first, then a server key
    /// query) — a user without one cannot be verified. Storing the request and
    /// attaching its watcher is `activate_flow`'s job.
    async fn ensure_user_verification_request(
        &self,
        client: &Client,
        user_id: &str,
    ) -> Result<VerificationRequest> {
        let uid = OwnedUserId::try_from(user_id)
            .map_err(|e| anyhow!("invalid user id '{user_id}': {e}"))?;

        let identity = match client.encryption().get_user_identity(&uid).await? {
            Some(identity) => identity,
            None => client
                .encryption()
                .request_user_identity(&uid)
                .await?
                .ok_or_else(|| anyhow!("user {user_id} has no cross-signing identity"))?,
        };

        self.emit_state(VerificationState::RequestingVerification);

        let request = identity
            .request_verification_with_methods(Self::outgoing_verification_methods())
            .await?;
        let flow_id = request.flow_id().to_string();
        // Tag every subsequent emitted state (WaitingForReady/Ready, and any
        // error-path Cancelled) with this flow id so the UI can attribute it and
        // the outgoing-user dialog can latch onto its own flow.
        self.set_current_flow_id(&flow_id);
        verification_debug!(
            "created outgoing user-verification request target={} flow_id={} state={}",
            user_id,
            flow_id,
            Self::verification_request_state_details(&request.state())
        );

        Ok(request)
    }

    /// Remove our verification of another user's identity (clears the trust
    /// shield / stops the violation warning). Emits the fresh trust state so the
    /// UI updates without waiting for the identity-updates stream.
    pub(crate) async fn withdraw_user_verification(
        &self,
        client: &Client,
        user_id: &str,
    ) -> Result<()> {
        let uid = OwnedUserId::try_from(user_id)
            .map_err(|e| anyhow!("invalid user id '{user_id}': {e}"))?;
        let identity = client
            .encryption()
            .get_user_identity(&uid)
            .await?
            .ok_or_else(|| anyhow!("user {user_id} has no cross-signing identity"))?;
        identity.withdraw_verification().await?;
        let state = self
            .user_trust_state(client, user_id)
            .await
            .unwrap_or(UserTrustState::Unverified);
        self.emit_user_trust_changed(user_id, state as u32);
        Ok(())
    }

    pub(crate) async fn confirm_sas_match(&self) -> Result<()> {
        // Clone the SAS out and release the ctx lock before the network
        // round-trip in `confirm()`, so a concurrent cancel/"don't match" is
        // not blocked behind it.
        let sas = {
            let ctx = self.ctx.lock().await;
            ctx.sas
                .as_ref()
                .ok_or_else(|| anyhow!("No active SAS verification"))?
                .clone()
        };

        self.emit_state(VerificationState::SasWaitingForConfirm);

        // Send-only: Done and Cancelled reach the UI from the SAS watcher, which
        // is the sole owner of terminal states.
        sas.confirm().await?;
        Ok(())
    }

    /// Answer a specific request with QR. Initiate-only: the module grid arrives
    /// later through the request watcher's `qr_data_callback`.
    pub(crate) async fn start_qr_verification_for(&self, expected_flow_id: &str) -> Result<()> {
        let _start_guard = self.start_guard.lock().await;
        let request = self
            .active_verification_request_for(expected_flow_id)
            .await
            .map_err(|e| anyhow!("No matching verification request: {e}"))?;
        self.activate_flow(request, DesiredMethod::Qr).await
    }

    /// Start (or adopt) a self-verification and drive it to QR. Initiate-only:
    /// the module grid arrives later through `qr_data_callback`.
    pub(crate) async fn start_qr_verification_checked(
        &self,
        client: Client,
        expected_flow_id: Option<&str>,
    ) -> Result<()> {
        debug_assert!(expected_flow_id.is_none());
        let _start_guard = self.start_guard.lock().await;
        let request = match self.ensure_verification_request(client).await {
            Ok(request) => request,
            Err(e) => {
                self.emit_state(VerificationState::Cancelled);
                self.reset_context_if_current(expected_flow_id).await;
                return Err(e);
            }
        };
        self.activate_flow(request, DesiredMethod::Qr).await
    }

    pub(crate) async fn confirm_qr_scanned(&self) -> Result<()> {
        let qr = {
            let ctx = self.ctx.lock().await;
            ctx.qr
                .as_ref()
                .ok_or_else(|| anyhow!("No active QR verification"))?
                .clone()
        };

        // The high-level wrapper's confirm() runs confirm_scanning() and sends.
        // Send-only: terminal states come from the QR watcher.
        qr.confirm().await?;
        Ok(())
    }

    pub(crate) async fn verify_with_recovery_key(&self, client: Client, key: &str) -> Result<()> {
        let secret_store = client
            .encryption()
            .secret_storage()
            .open_secret_store(key)
            .await
            .map_err(|e| {
                let msg = e.to_string();
                if msg.contains("Mac") || msg.contains("mac") || msg.contains("decrypt") {
                    anyhow!("Invalid recovery key or passphrase")
                } else if msg.contains("no default key") || msg.contains("No default") {
                    anyhow!("Secret storage is not configured on this account")
                } else {
                    anyhow!("Failed to open secret storage: {e}")
                }
            })?;

        secret_store
            .import_secrets()
            .await
            .map_err(|e| anyhow!("Failed to import secrets: {e}"))?;

        info!("Recovery key verification successful — secrets imported");
        self.emit_state(VerificationState::Done);
        self.reset_context().await;

        Ok(())
    }

    /// Poll until key backup is enabled (the gossiped backup key arrived and
    /// the SDK activated it) or the timeout lapses. Recovery-key verification
    /// enables backups inline, so it returns true immediately there.
    pub(crate) async fn wait_backup_keys_ready(&self, client: Client, timeout: Duration) -> bool {
        let backups = client.encryption().backups();
        let started = Instant::now();
        loop {
            if backups.are_enabled().await {
                return true;
            }
            if started.elapsed() >= timeout {
                return false;
            }
            tokio::time::sleep(Duration::from_secs(1)).await;
        }
    }

    pub(crate) async fn cancel_verification(&self) -> Result<()> {
        let mut ctx = self.ctx.lock().await;

        if let Some(ref sas) = ctx.sas {
            let _ = sas.cancel().await;
        }

        if let Some(ref qr) = ctx.qr {
            let _ = qr.cancel().await;
        }

        if let Some(ref request) = ctx.request {
            if !request.is_done() && !request.is_cancelled() {
                let _ = request.cancel().await;
            }
        }

        ctx.reset();
        drop(ctx);
        self.emit_state(VerificationState::Cancelled);

        Ok(())
    }

    /// Reject a SAS verification because the emojis did not match. Sends the
    /// protocol-correct `MismatchedSas` cancel code so the other device shows
    /// "the emoji didn't match" instead of a generic "user cancelled".
    pub(crate) async fn mismatch_sas(&self) -> Result<()> {
        let mut ctx = self.ctx.lock().await;

        if let Some(ref sas) = ctx.sas {
            let _ = sas.mismatch().await;
        }

        if let Some(ref request) = ctx.request {
            if !request.is_done() && !request.is_cancelled() {
                let _ = request.cancel().await;
            }
        }

        ctx.reset();
        drop(ctx);
        self.emit_state(VerificationState::Cancelled);

        Ok(())
    }

    pub(crate) async fn skip_verification(&self) -> Result<()> {
        let mut ctx = self.ctx.lock().await;
        if let Some(ref request) = ctx.request {
            let _ = request.cancel().await;
        }
        ctx.reset();
        drop(ctx);
        // Emit a distinct Skipped state — NOT Done. The UI treats Done as
        // "device verified", which arms the decrypting-glow skeleton for
        // permanently-UTD messages (no keys will arrive after a skip), leaving
        // them glowing forever instead of showing the "Unable to decrypt" card.
        self.emit_state(VerificationState::Skipped);
        Ok(())
    }

    pub(crate) async fn capabilities(&self, client: Client) -> Result<VerificationCapabilities> {
        let own_user_id = client
            .user_id()
            .ok_or_else(|| anyhow!("Not logged in"))?
            .to_owned();

        let has_identity = match Self::own_identity_for_verification(&client, &own_user_id).await {
            Ok(_) => true,
            Err(e) => {
                warn!("Verification capabilities: own identity unavailable: {e}");
                false
            }
        };

        // Both probes below are load-bearing: the intro sends a user with neither another device
        // nor recovery into *creating* a recovery key, which rotates secret storage. A wrong
        // `false` would do that to someone who already has both. So they are counted from the
        // server and a failure is an error, never a silent `false` — the caller then falls back
        // to offering every verification method, which is what it did before any of this.

        // The server's /devices list, not the crypto store: the store is cold right after a login
        // and under-reports.
        let device_count = client
            .devices()
            .await
            .map_err(|e| anyhow!("Own devices unavailable: {e}"))?
            .devices
            .len();
        let can_verify_with_device = device_count > 1;

        let can_verify_with_recovery = client
            .encryption()
            .secret_storage()
            .is_enabled()
            .await
            .map_err(|e| anyhow!("Secret storage state unavailable: {e}"))?;

        info!("Verification capabilities: identity={has_identity}, devices={device_count}, recovery={can_verify_with_recovery}");

        Ok(VerificationCapabilities {
            can_verify_with_device,
            can_verify_with_recovery,
            sas_supported: has_identity,
            qr_supported: can_verify_with_device,
        })
    }

    async fn active_verification_request_for(
        &self,
        expected_flow_id: &str,
    ) -> Result<VerificationRequest> {
        let ctx = self.ctx.lock().await;
        let request = ctx
            .request
            .as_ref()
            .ok_or_else(|| anyhow!("No active verification request"))?;
        if request.flow_id() != expected_flow_id {
            return Err(anyhow!(
                "Active verification flow_id={} does not match expected flow_id={expected_flow_id}",
                request.flow_id()
            ));
        }
        if request.is_done() || request.is_cancelled() {
            return Err(anyhow!(
                "Verification request flow_id={expected_flow_id} is no longer active"
            ));
        }
        Ok(request.clone())
    }

    async fn reset_context(&self) {
        let mut ctx = self.ctx.lock().await;
        ctx.reset();
    }

    /// Returns whether this call performed the reset, so a caller that owes the
    /// UI exactly one terminal state can tell whether it was the one to tear the
    /// flow down.
    async fn reset_context_if_current(&self, expected_flow_id: Option<&str>) -> bool {
        let mut ctx = self.ctx.lock().await;
        if let Some(expected) = expected_flow_id {
            let still_current = ctx
                .request
                .as_ref()
                .map(|request| request.flow_id() == expected)
                .unwrap_or(false);
            if !still_current {
                return false;
            }
        }
        ctx.reset();
        true
    }

    /// Store the request and attach its watcher. The watcher is the single
    /// owner of request-level emissions (Element's `onChange` shape). Returns
    /// the generation the flow is installed under, so the caller can tell later
    /// whether it is still the one it started.
    async fn begin_flow(
        &self,
        request: VerificationRequest,
        desired: Option<DesiredMethod>,
    ) -> u64 {
        let mut ctx = self.ctx.lock().await;
        let is_same = ctx
            .request
            .as_ref()
            .map(|r| r.flow_id() == request.flow_id())
            .unwrap_or(false);
        if !is_same {
            for handle in ctx.watchers.drain(..) {
                handle.abort();
            }
            ctx.sas = None;
            ctx.qr = None;
            ctx.sas_token += 1;
            ctx.qr_token += 1;
            ctx.request = Some(request.clone());
        }
        // The watcher belongs to the stored request, not to this store: an
        // incoming handler stores the request itself, so answering one would
        // otherwise install a flow nothing is subscribed to and hang forever.
        if ctx.watchers.is_empty() {
            let generation = ctx.generation;
            let handle = self.spawn_request_watcher(request, generation);
            ctx.watchers.push(handle);
        }
        if desired.is_some() {
            ctx.desired = desired;
        }
        ctx.generation
    }

    fn spawn_request_watcher(
        &self,
        request: VerificationRequest,
        generation: u64,
    ) -> tokio::task::JoinHandle<()> {
        let service = self.clone();
        tokio::spawn(async move {
            let flow_id = request.flow_id().to_string();
            let mut changes = request.changes();
            // `changes()` does not replay the current state — seed with it.
            let mut next_state = Some(request.state());
            loop {
                let state = match next_state.take() {
                    Some(state) => state,
                    None => match changes.next().await {
                        Some(state) => state,
                        None => break,
                    },
                };
                verification_debug!(
                    "request watcher flow_id={} state={}",
                    flow_id,
                    Self::verification_request_state_details(&state)
                );
                match state {
                    VerificationRequestState::Ready { .. } => {
                        service.emit_state_for(VerificationState::Ready, &flow_id);
                        service.act_on_ready(&request, generation).await;
                    }
                    VerificationRequestState::Transitioned { verification } => {
                        if let Some(sas) = verification.sas() {
                            service.adopt_sas(sas, &flow_id, generation).await;
                        }
                        // QR transitions are driven by our own generate path;
                        // nothing to adopt (this client never scans).
                    }
                    VerificationRequestState::Cancelled(info) => {
                        if service.flow_is_current(&flow_id, generation).await {
                            verification_debug!(
                                "request cancelled flow_id={} code={} by_us={}",
                                flow_id,
                                info.cancel_code().as_str(),
                                info.cancelled_by_us()
                            );
                            service.emit_cancel_info(
                                &flow_id,
                                info.cancel_code().as_str(),
                                info.cancelled_by_us(),
                            );
                            service.emit_state_for(VerificationState::Cancelled, &flow_id);
                            service.reset_context_if_current(Some(&flow_id)).await;
                        }
                        break;
                    }
                    VerificationRequestState::Done => {
                        // The SAS/QR watcher normally emits Done first; this is
                        // the safety net for flows completing without one.
                        service.emit_state_for(VerificationState::Done, &flow_id);
                        service.reset_context_if_current(Some(&flow_id)).await;
                        break;
                    }
                    _ => {}
                }
            }
        })
    }

    async fn act_on_ready(&self, request: &VerificationRequest, generation: u64) {
        let desired = {
            let ctx = self.ctx.lock().await;
            if ctx.generation != generation {
                return;
            }
            ctx.desired
        };
        match desired {
            Some(DesiredMethod::Sas) => self.start_sas_on_ready(request, generation).await,
            Some(DesiredMethod::Qr) => self.generate_qr_on_ready(request, generation).await,
            None => {}
        }
    }

    async fn start_sas_on_ready(&self, request: &VerificationRequest, generation: u64) {
        let flow_id = request.flow_id().to_string();
        match request.start_sas().await {
            Ok(Some(sas)) => self.adopt_sas(sas, &flow_id, generation).await,
            Ok(None) => {
                // Peer transitioned between Ready and start_sas — the watcher
                // adopts theirs. Anything else is a real failure.
                if !matches!(
                    request.state(),
                    VerificationRequestState::Transitioned { .. }
                ) {
                    self.fail_flow(&flow_id, "Failed to start SAS verification")
                        .await;
                }
            }
            Err(e) => {
                self.fail_flow(&flow_id, &format!("Failed to start SAS: {e}"))
                    .await;
            }
        }
    }

    /// Store the SAS (ours or the peer's), accept it when the peer started it
    /// — including a replacement after a lost start-race tie-break — and spawn
    /// its watcher. matrix-sdk-ffi does exactly this on `Transitioned`.
    async fn adopt_sas(&self, sas: SasVerification, flow_id: &str, generation: u64) {
        {
            let mut ctx = self.ctx.lock().await;
            if ctx.generation != generation {
                return;
            }
            let same_flow = ctx
                .request
                .as_ref()
                .map(|r| r.flow_id() == flow_id)
                .unwrap_or(false);
            if !same_flow {
                return;
            }
            if ctx.sas.is_some() && sas.we_started() {
                // Our own start echoed back through the request stream.
                return;
            }
            // SAS displaces any QR we were showing (matrix-sdk-crypto replaces
            // the cache entry without cancelling it, so only the token stops
            // that watcher from tearing this SAS down later).
            ctx.qr = None;
            ctx.qr_token += 1;
            ctx.sas_token += 1;
            let token = ctx.sas_token;
            ctx.sas = Some(sas.clone());
            let handle =
                self.spawn_sas_watcher(sas.clone(), flow_id.to_string(), generation, token);
            ctx.watchers.push(handle);
        }
        if !sas.we_started() {
            if let Err(e) = sas.accept().await {
                verification_debug!("failed to accept peer SAS flow_id={flow_id} error={e}");
            }
        }
        self.emit_state_for(VerificationState::SasStarted, flow_id);
    }

    fn spawn_sas_watcher(
        &self,
        sas: SasVerification,
        flow_id: String,
        generation: u64,
        token: u64,
    ) -> tokio::task::JoinHandle<()> {
        let service = self.clone();
        tokio::spawn(async move {
            let mut changes = sas.changes();
            let mut next_state = Some(sas.state());
            let mut emojis_emitted = false;
            loop {
                let state = match next_state.take() {
                    Some(state) => state,
                    None => match changes.next().await {
                        Some(state) => state,
                        None => break,
                    },
                };
                // A replaced SAS must stay silent; the winner has its own watcher.
                if !service.sas_token_is_current(generation, token).await {
                    break;
                }
                match state {
                    SasState::Done { .. } => {
                        service.emit_state_for(VerificationState::Done, &flow_id);
                        service.reset_context_if_current(Some(&flow_id)).await;
                        break;
                    }
                    SasState::Cancelled(info) => {
                        verification_debug!(
                            "sas cancelled flow_id={} code={} by_us={}",
                            flow_id,
                            info.cancel_code().as_str(),
                            info.cancelled_by_us()
                        );
                        service.emit_cancel_info(
                            &flow_id,
                            info.cancel_code().as_str(),
                            info.cancelled_by_us(),
                        );
                        service.emit_state_for(VerificationState::Cancelled, &flow_id);
                        service.reset_context_if_current(Some(&flow_id)).await;
                        break;
                    }
                    _ => {
                        // Emojis appear on KeysExchanged, but `changes()` can
                        // coalesce — take them from whatever state has them.
                        if !emojis_emitted {
                            if let Some(emoji_arr) = sas.emoji() {
                                emojis_emitted = true;
                                let emojis: Vec<SasEmoji> = emoji_arr
                                    .iter()
                                    .map(|e| SasEmoji {
                                        emoji: e.symbol.to_string(),
                                        label: e.description.to_string(),
                                    })
                                    .collect();
                                service.emit_sas_emojis(&flow_id, &emojis);
                                service.emit_state_for(
                                    VerificationState::SasEmojisAvailable,
                                    &flow_id,
                                );
                            }
                        }
                    }
                }
            }
        })
    }

    async fn generate_qr_on_ready(&self, request: &VerificationRequest, generation: u64) {
        let flow_id = request.flow_id().to_string();
        {
            let ctx = self.ctx.lock().await;
            let stale = ctx.generation != generation
                || ctx
                    .request
                    .as_ref()
                    .map(|r| r.flow_id() != flow_id)
                    .unwrap_or(true);
            // A duplicated `Ready` (the seed/stream race) would otherwise mint a
            // second, *different* code — matrix-sdk-crypto transitions here
            // instead of returning None — over one the peer may have scanned.
            if stale || ctx.qr.is_some() {
                return;
            }
        }
        let qr = match request.generate_qr_code().await {
            Ok(Some(qr)) => qr,
            Ok(None) => {
                self.fail_flow(
                    &flow_id,
                    "QR verification unavailable (the other device can't scan, or cross-signing is missing)",
                )
                .await;
                return;
            }
            Err(e) => {
                self.fail_flow(&flow_id, &format!("Failed to generate QR code: {e}"))
                    .await;
                return;
            }
        };
        let image = match Self::render_qr(&qr) {
            Ok(image) => image,
            Err(e) => {
                self.fail_flow(&flow_id, &e.to_string()).await;
                return;
            }
        };
        {
            let mut ctx = self.ctx.lock().await;
            if ctx.generation != generation
                || ctx
                    .request
                    .as_ref()
                    .map(|r| r.flow_id() != flow_id)
                    .unwrap_or(true)
            {
                return;
            }
            ctx.qr_token += 1;
            let token = ctx.qr_token;
            ctx.qr = Some(qr.clone());
            let handle = self.spawn_qr_watcher(qr, flow_id.clone(), generation, token);
            ctx.watchers.push(handle);
        }
        self.emit_qr_data(&flow_id, &image);
        self.emit_state_for(VerificationState::QrCodeReady, &flow_id);
    }

    fn render_qr(qr: &QrVerification) -> Result<QrCodeImage> {
        let code = qr
            .to_qr_code()
            .map_err(|e| anyhow!("Failed to render QR code: {e}"))?;
        let size = code.width();
        let modules: Vec<u8> = code
            .to_colors()
            .into_iter()
            .map(|c| u8::from(c == qrcode::Color::Dark))
            .collect();
        Ok(QrCodeImage { size, modules })
    }

    fn spawn_qr_watcher(
        &self,
        qr: QrVerification,
        flow_id: String,
        generation: u64,
        token: u64,
    ) -> tokio::task::JoinHandle<()> {
        let service = self.clone();
        tokio::spawn(async move {
            let mut changes = qr.changes();
            let mut next_state = Some(qr.state());
            loop {
                let state = match next_state.take() {
                    Some(state) => state,
                    None => match changes.next().await {
                        Some(state) => state,
                        None => break,
                    },
                };
                // A displaced QR must stay silent, exactly like a replaced SAS:
                // flow id and generation still match after a QR→SAS adoption.
                if !service.qr_token_is_current(generation, token).await {
                    break;
                }
                match state {
                    QrVerificationState::Scanned => {
                        service.emit_state_for(VerificationState::QrCodeScanned, &flow_id);
                    }
                    QrVerificationState::Done { .. } => {
                        service.emit_state_for(VerificationState::Done, &flow_id);
                        service.reset_context_if_current(Some(&flow_id)).await;
                        break;
                    }
                    QrVerificationState::Cancelled(info) => {
                        verification_debug!(
                            "qr cancelled flow_id={} code={} by_us={}",
                            flow_id,
                            info.cancel_code().as_str(),
                            info.cancelled_by_us()
                        );
                        service.emit_cancel_info(
                            &flow_id,
                            info.cancel_code().as_str(),
                            info.cancelled_by_us(),
                        );
                        service.emit_state_for(VerificationState::Cancelled, &flow_id);
                        service.reset_context_if_current(Some(&flow_id)).await;
                        break;
                    }
                    _ => {}
                }
            }
        })
    }

    async fn flow_is_current(&self, flow_id: &str, generation: u64) -> bool {
        let ctx = self.ctx.lock().await;
        ctx.generation == generation
            && ctx
                .request
                .as_ref()
                .map(|r| r.flow_id() == flow_id)
                .unwrap_or(false)
    }

    async fn sas_token_is_current(&self, generation: u64, token: u64) -> bool {
        let ctx = self.ctx.lock().await;
        ctx.generation == generation && ctx.sas_token == token
    }

    async fn qr_token_is_current(&self, generation: u64, token: u64) -> bool {
        let ctx = self.ctx.lock().await;
        ctx.generation == generation && ctx.qr_token == token
    }

    /// Tear a failed flow down and report it — but only if this call is the one
    /// that tore it down. Two tasks can decide to fail the same flow at once (the
    /// accept send and the watcher's own start failing together), and
    /// `emit_state_for` shares no lock with ctx, so emitting first would let both
    /// through and send the UI two `Cancelled` for one flow.
    async fn fail_flow(&self, flow_id: &str, reason: &str) {
        verification_debug!("verification flow failed flow_id={flow_id} reason={reason}");
        if self.reset_context_if_current(Some(flow_id)).await {
            self.emit_state_for(VerificationState::Cancelled, flow_id);
        } else {
            // Someone else ended this flow and owes the terminal state.
            verification_debug!("flow failure not emitted, flow already ended flow_id={flow_id}");
        }
    }

    /// Fail a flow only while nothing else owns it: same generation, same
    /// request, and no SAS or QR started under it. For failures that surface
    /// after the watcher may already have driven the flow forward — cancelling a
    /// live SAS/QR from a losing path is the client-side teardown this design
    /// exists to eliminate. Decision and teardown share one lock, and the
    /// `Cancelled` is emitted only if the teardown actually happened, so a flow
    /// the watcher has already failed is not reported twice.
    async fn fail_flow_if_unstarted(&self, flow_id: &str, generation: u64, reason: &str) {
        {
            let mut ctx = self.ctx.lock().await;
            let unstarted = ctx.generation == generation
                && ctx.sas.is_none()
                && ctx.qr.is_none()
                && ctx
                    .request
                    .as_ref()
                    .map(|r| r.flow_id() == flow_id)
                    .unwrap_or(false);
            if !unstarted {
                verification_debug!(
                    "flow failure not applied, flow moved on flow_id={flow_id} reason={reason}"
                );
                return;
            }
            ctx.reset();
        }
        verification_debug!("verification flow failed flow_id={flow_id} reason={reason}");
        self.emit_state_for(VerificationState::Cancelled, flow_id);
    }

    /// User action driving a flow: accept if it's an incoming request in
    /// `Requested`, record the desired method, let the watcher do the rest.
    async fn activate_flow(
        &self,
        request: VerificationRequest,
        desired: DesiredMethod,
    ) -> Result<()> {
        let flow_id = request.flow_id().to_string();
        self.set_current_flow_id(&flow_id);
        let generation = self.begin_flow(request.clone(), Some(desired)).await;
        match request.state() {
            VerificationRequestState::Cancelled(info) => {
                return Err(anyhow!("Verification request cancelled: {}", info.reason()));
            }
            VerificationRequestState::Done => {
                return Err(anyhow!("Verification request already completed"));
            }
            VerificationRequestState::Requested { .. } => {
                // Sends m.key.verification.ready; local state flips to Ready
                // synchronously and the watcher acts on it.
                if let Err(e) = request
                    .accept_with_methods(Self::accepted_verification_methods())
                    .await
                {
                    // The flip to Ready is committed before the send that failed
                    // (matrix-sdk `accept_with_methods`), so the watcher may
                    // already own this flow — fail it only if it does not.
                    // Otherwise nothing would emit a terminal state for it.
                    let reason = format!("Failed to accept verification request: {e}");
                    self.fail_flow_if_unstarted(&flow_id, generation, &reason)
                        .await;
                    return Err(anyhow!("{reason}"));
                }
            }
            VerificationRequestState::Created { .. } => {
                self.emit_state_for(VerificationState::WaitingForReady, &flow_id);
            }
            // Ready / Transitioned: the watcher seeds from the current state.
            _ => {}
        }
        Ok(())
    }

    fn emit_state(&self, state: VerificationState) {
        let flow_id = lock_verification_mutex(&self.current_flow_id, "current_flow_id").clone();
        self.emit_state_for(state, &flow_id);
    }

    /// Emit a state tagged with an explicit flow id, for cases where the
    /// currently-active flow is not the one the state pertains to (e.g. a loser
    /// flow that lost a concurrent-start race).
    fn emit_state_for(&self, state: VerificationState, flow_id: &str) {
        verification_debug!(
            "emit verification state={} flow_id={}",
            Self::verification_state_name(state),
            flow_id
        );
        let cb = lock_verification_mutex(&self.state_callback, "verification_state_callback");
        if let Some(ref f) = *cb {
            f(state as u32, flow_id);
        }
    }

    fn set_current_flow_id(&self, flow_id: &str) {
        let mut guard = lock_verification_mutex(&self.current_flow_id, "current_flow_id");
        flow_id.clone_into(&mut guard);
    }

    fn verification_state_name(state: VerificationState) -> &'static str {
        match state {
            VerificationState::RequestingVerification => "RequestingVerification",
            VerificationState::WaitingForReady => "WaitingForReady",
            VerificationState::Ready => "Ready",
            VerificationState::SasStarted => "SasStarted",
            VerificationState::SasEmojisAvailable => "SasEmojisAvailable",
            VerificationState::SasWaitingForConfirm => "SasWaitingForConfirm",
            VerificationState::QrCodeReady => "QrCodeReady",
            VerificationState::QrCodeScanned => "QrCodeScanned",
            VerificationState::Done => "Done",
            VerificationState::Cancelled => "Cancelled",
            VerificationState::Skipped => "Skipped",
        }
    }

    async fn own_identity_for_verification(
        client: &Client,
        own_user_id: &OwnedUserId,
    ) -> Result<UserIdentity> {
        let encryption = client.encryption();
        info!("Refreshing own identity and device list before self-verification");
        let identity = match encryption.request_user_identity(own_user_id).await {
            Ok(Some(identity)) => identity,
            Ok(None) => return Err(anyhow!("Own user identity not found")),
            Err(e) => {
                warn!("Failed to refresh own identity from homeserver: {e}; falling back to local crypto store");
                encryption
                    .get_user_identity(own_user_id)
                    .await?
                    .ok_or_else(|| anyhow!("Own user identity not found"))?
            }
        };

        let devices = encryption.get_user_devices(own_user_id).await?;
        let current_device_id = client.device_id().map(|id| id.to_string());
        let device_ids: Vec<String> = devices.keys().map(|id| id.to_string()).collect();
        let other_device_count = device_ids
            .iter()
            .filter(|id| Some(id.as_str()) != current_device_id.as_deref())
            .count();
        info!(
            "Own device list refreshed for verification: total={}, other={}",
            device_ids.len(),
            other_device_count
        );
        if other_device_count == 0 {
            return Err(anyhow!(
                "No other signed-in devices are available for verification"
            ));
        }

        Ok(identity)
    }

    /// Element parity: one identity refresh (a single server key query), no
    /// polling loop and no cross-signed-peer precondition — the SDK falls back
    /// to a `*` to-device broadcast when no cross-signed device exists.
    async fn verification_identity(
        client: &Client,
        own_user_id: &OwnedUserId,
    ) -> Result<UserIdentity> {
        client
            .encryption()
            .wait_for_e2ee_initialization_tasks()
            .await;
        Self::own_identity_for_verification(client, own_user_id).await
    }

    fn outgoing_verification_methods() -> Vec<VerificationMethod> {
        vec![
            VerificationMethod::SasV1,
            VerificationMethod::QrCodeShowV1,
            VerificationMethod::ReciprocateV1,
        ]
    }

    fn accepted_verification_methods() -> Vec<VerificationMethod> {
        vec![
            VerificationMethod::SasV1,
            VerificationMethod::QrCodeShowV1,
            VerificationMethod::ReciprocateV1,
        ]
    }

    /// Whether a request in this state can still be accepted or declined. Only
    /// a request still awaiting an answer can; everything else means it was
    /// taken, withdrawn, or completed. Mirrors Element's
    /// `canAcceptVerificationRequest`.
    fn request_state_is_answerable(state: &VerificationRequestState) -> bool {
        matches!(
            state,
            VerificationRequestState::Created { .. } | VerificationRequestState::Requested { .. }
        )
    }

    fn emit_request_closed(
        closed_callback: &Arc<Mutex<Option<VerificationRequestClosedCallback>>>,
        flow_id: &str,
    ) {
        let cb = lock_verification_mutex(closed_callback, "verification_request_closed_callback");
        if let Some(ref f) = *cb {
            f(flow_id);
        }
    }

    /// Watch an incoming request we surfaced to the UI and report when it stops
    /// being answerable. Self-verification requests go to every one of our
    /// sessions, so accepting on one makes the requester cancel the rest with
    /// `m.accepted`; without this the banner on those sessions never goes away.
    fn spawn_incoming_request_watcher(
        request: VerificationRequest,
        closed_callback: Arc<Mutex<Option<VerificationRequestClosedCallback>>>,
        pending: Arc<Mutex<Option<PendingIncomingRequest>>>,
    ) {
        let flow_id = request.flow_id().to_string();
        tokio::spawn(async move {
            let mut changes = request.changes();
            // `changes()` does not replay the current state, so a request that was
            // already answered before we subscribed would never wake this task.
            let mut next_state = Some(request.state());
            loop {
                let state = match next_state.take() {
                    Some(state) => state,
                    None => match changes.next().await {
                        Some(state) => state,
                        // Stream gone: the request is unreachable either way, so
                        // take the banner down rather than strand it.
                        None => break,
                    },
                };
                // Ready lands here when WE accepted; the banner is already down
                // by then and a second dismissal is a no-op.
                if !Self::request_state_is_answerable(&state) {
                    break;
                }
            }
            verification_debug!("incoming request no longer answerable flow_id={}", flow_id);
            {
                let mut pending = lock_verification_mutex(&pending, "pending_incoming");
                if pending
                    .as_ref()
                    .map(|p| p.flow_id == flow_id)
                    .unwrap_or(false)
                {
                    *pending = None;
                }
            }
            Self::emit_request_closed(&closed_callback, &flow_id);
        });
    }

    fn verification_methods_debug(methods: &[VerificationMethod]) -> String {
        methods
            .iter()
            .map(|method| format!("{method:?}"))
            .collect::<Vec<_>>()
            .join(",")
    }

    fn verification_request_state_details(state: &VerificationRequestState) -> String {
        match state {
            VerificationRequestState::Created { our_methods } => format!(
                "Created our_methods=[{}]",
                Self::verification_methods_debug(our_methods)
            ),
            VerificationRequestState::Requested {
                their_methods,
                other_device_data,
            } => format!(
                "Requested other_device={} their_methods=[{}]",
                other_device_data.device_id(),
                Self::verification_methods_debug(their_methods)
            ),
            VerificationRequestState::Ready {
                their_methods,
                our_methods,
                other_device_data,
            } => format!(
                "Ready other_device={} our_methods=[{}] their_methods=[{}]",
                other_device_data.device_id(),
                Self::verification_methods_debug(our_methods),
                Self::verification_methods_debug(their_methods)
            ),
            VerificationRequestState::Transitioned { .. } => "Transitioned".to_string(),
            VerificationRequestState::Done => "Done".to_string(),
            VerificationRequestState::Cancelled(info) => {
                format!("Cancelled reason={}", info.reason())
            }
        }
    }

    async fn ensure_verification_request(&self, client: Client) -> Result<VerificationRequest> {
        verification_debug!("ensure verification request start");
        let own_user_id = client
            .user_id()
            .ok_or_else(|| anyhow!("Not logged in"))?
            .to_owned();
        verification_debug!(
            "ensure verification request user_id={} device_id={:?}",
            own_user_id,
            client.device_id().map(|id| id.to_string())
        );

        {
            let ctx = self.ctx.lock().await;
            if let Some(ref req) = ctx.request {
                verification_debug!(
                    "existing verification request flow_id={} state={} done={} cancelled={} ready={}",
                    req.flow_id(),
                    Self::verification_request_state_details(&req.state()),
                    req.is_done(),
                    req.is_cancelled(),
                    req.is_ready()
                );
                if !req.is_done() && !req.is_cancelled() {
                    return Ok(req.clone());
                }
            }
        }

        verification_debug!(
            "creating outgoing self-verification request methods=[{}]",
            Self::verification_methods_debug(&Self::outgoing_verification_methods())
        );
        self.emit_state(VerificationState::RequestingVerification);

        let identity = Self::verification_identity(&client, &own_user_id).await?;

        let request = identity
            .request_verification_with_methods(Self::outgoing_verification_methods())
            .await?;
        // Tag every subsequent emitted state with this flow id (the user path
        // at ensure_user_verification_request already does this).
        self.set_current_flow_id(request.flow_id());
        verification_debug!(
            "created outgoing self-verification request source=own-user flow_id={} state={}",
            request.flow_id(),
            Self::verification_request_state_details(&request.state())
        );

        Ok(request)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex as StdMutex;

    // QR verification only works if both negotiated method sets advertise the
    // show + reciprocate methods; reverting either to SAS-only silently breaks
    // QR (generate_qr_code() then always returns None).
    #[test]
    fn qr_methods_are_advertised() {
        for methods in [
            VerificationService::outgoing_verification_methods(),
            VerificationService::accepted_verification_methods(),
        ] {
            assert!(methods.contains(&VerificationMethod::SasV1));
            assert!(methods.contains(&VerificationMethod::QrCodeShowV1));
            assert!(methods.contains(&VerificationMethod::ReciprocateV1));
        }
    }

    // A request that has left `Requested` can no longer be accepted or declined,
    // so the banner offering it must come down. `Done` is the reachable terminal
    // state here; the same predicate covers Ready/Transitioned/Cancelled, which
    // is what a request accepted on another session becomes.
    #[test]
    fn answered_requests_are_not_answerable() {
        assert!(VerificationService::request_state_is_answerable(
            &VerificationRequestState::Created {
                our_methods: VerificationService::outgoing_verification_methods(),
            }
        ));
        assert!(!VerificationService::request_state_is_answerable(
            &VerificationRequestState::Done
        ));
    }

    // The closed callback must reach the UI with the flow id it belongs to (the
    // banner only dismisses on a match), and must not survive `clear_callbacks`
    // — a logout leaves the C++ guard object behind.
    #[test]
    fn request_closed_callback_is_flow_tagged_and_clearable() {
        let service = VerificationService::new();
        let captured = Arc::new(StdMutex::new(Vec::<String>::new()));
        let sink = captured.clone();
        service.on_request_closed(Box::new(move |flow_id| {
            sink.lock()
                .expect("sink not poisoned")
                .push(flow_id.to_string());
        }));

        VerificationService::emit_request_closed(
            &service.request_closed_callback,
            "$flow:example.org",
        );
        assert_eq!(
            captured.lock().expect("captured not poisoned").clone(),
            vec!["$flow:example.org".to_string()]
        );

        service.clear_callbacks();
        VerificationService::emit_request_closed(
            &service.request_closed_callback,
            "$after-logout:example.org",
        );
        assert_eq!(
            captured.lock().expect("captured not poisoned").len(),
            1,
            "a cleared callback must not fire"
        );
    }

    // Regression: skipping verification must emit a state the UI cannot mistake
    // for a verified device. If skip emits `Done`, the C++ side latches the
    // session "verified" and arms the decrypting-glow skeleton for messages that
    // are permanently UTD (no keys will ever arrive), so they glow forever
    // instead of showing the "Unable to decrypt" card.
    #[tokio::test]
    async fn skip_verification_emits_skipped_not_done() {
        let service = VerificationService::new();
        let captured = Arc::new(StdMutex::new(Vec::<u32>::new()));
        let sink = captured.clone();
        service.on_state_changed(Box::new(move |state, _flow_id| {
            sink.lock().expect("sink not poisoned").push(state);
        }));

        service
            .skip_verification()
            .await
            .expect("skip_verification should succeed with no active request");

        let states = captured.lock().expect("captured not poisoned").clone();
        assert_eq!(
            states,
            vec![VerificationState::Skipped as u32],
            "skip must emit a distinct Skipped state, never Done"
        );
        assert_ne!(
            VerificationState::Skipped as u32,
            VerificationState::Done as u32,
            "Skipped and Done must be distinct discriminants"
        );
    }

    // The mismatch path (P3-13) must still emit a terminal Cancelled state so the
    // UI tears down, even with no active SAS (it then just cancels the flow).
    #[tokio::test]
    async fn mismatch_sas_emits_cancelled() {
        let service = VerificationService::new();
        let captured = Arc::new(StdMutex::new(Vec::<u32>::new()));
        let sink = captured.clone();
        service.on_state_changed(Box::new(move |state, _flow_id| {
            sink.lock().expect("sink not poisoned").push(state);
        }));

        service
            .mismatch_sas()
            .await
            .expect("mismatch_sas should succeed with no active flow");

        let states = captured.lock().expect("captured not poisoned").clone();
        assert_eq!(states, vec![VerificationState::Cancelled as u32]);
    }

    // `reset_context_if_current` must only wipe the context when it still owns
    // the active flow. The outgoing user-verification error path relies on this:
    // an incoming request may have replaced `ctx` while we waited for readiness,
    // and a blanket reset there would strand that other flow. Its return value is
    // load-bearing too: `fail_flow` emits `Cancelled` only when it is true, which
    // is what keeps two concurrent failers from both reporting one flow.
    #[tokio::test]
    async fn reset_context_if_current_is_flow_scoped() {
        let service = VerificationService::new();
        let gen_before = service.ctx.lock().await.generation;

        // No request stored: a flow-scoped reset for some other flow is a no-op.
        assert!(
            !service
                .reset_context_if_current(Some("$other:example.org"))
                .await
        );
        assert_eq!(service.ctx.lock().await.generation, gen_before);

        // An unconditional reset always advances the generation.
        assert!(service.reset_context_if_current(None).await);
        assert_eq!(service.ctx.lock().await.generation, gen_before + 1);
    }

    // A QR can be displaced inside one flow+generation — the peer starts SAS, or
    // a duplicated `Ready` mints a second code — and its watcher would still pass
    // a flow-id/generation check. Only the token tells it to stay silent, which
    // matters because a displaced QR reaching `Cancelled` would otherwise tear
    // down the SAS that replaced it.
    #[tokio::test]
    async fn qr_token_supersedes_a_displaced_qr_watcher() {
        let service = VerificationService::new();
        let (generation, token) = {
            let mut ctx = service.ctx.lock().await;
            ctx.qr_token += 1;
            (ctx.generation, ctx.qr_token)
        };
        assert!(service.qr_token_is_current(generation, token).await);

        // What `adopt_sas` now does when SAS displaces the QR.
        {
            let mut ctx = service.ctx.lock().await;
            ctx.qr = None;
            ctx.qr_token += 1;
        }
        assert!(
            !service.qr_token_is_current(generation, token).await,
            "a displaced QR watcher must not pass its ownership check"
        );

        // And a reset invalidates it too, like sas_token.
        let before = service.ctx.lock().await.qr_token;
        service.reset_context().await;
        assert!(service.ctx.lock().await.qr_token > before);
    }

    // `fail_flow` reports a failure only when it is the call that ended the flow.
    // Both directions matter: two tasks failing one flow at once must produce one
    // `Cancelled` (the emission shares no lock with ctx, so emitting first let
    // both through), and a watcher must not announce a terminal state for a flow
    // someone else already tore down.
    #[tokio::test]
    async fn fail_flow_is_silent_when_it_did_not_end_the_flow() {
        let service = VerificationService::new();
        let captured = Arc::new(StdMutex::new(Vec::<(u32, String)>::new()));
        let sink = captured.clone();
        service.on_state_changed(Box::new(move |state, flow_id| {
            sink.lock()
                .expect("sink not poisoned")
                .push((state, flow_id.to_string()));
        }));

        // ctx does not hold this flow, so this call did not end it.
        let gen_before = service.ctx.lock().await.generation;
        service
            .fail_flow("$flow:example.org", "accept failed")
            .await;

        assert!(
            captured.lock().expect("captured not poisoned").is_empty(),
            "a flow this call did not end must not be reported cancelled"
        );
        assert_eq!(service.ctx.lock().await.generation, gen_before);
    }

    // `accept_with_methods` commits local state to Ready *before* the send that
    // can fail, so by the time an accept failure surfaces the watcher may already
    // own the flow, and this call must not tear it down. What the two cases below
    // pin is the after-the-fact half: once ctx no longer holds the flow under the
    // generation the caller began it with, nothing is emitted or reset. The
    // genuinely concurrent interleaving needs an SDK-backed request and cannot be
    // modelled here.
    #[tokio::test]
    async fn fail_flow_if_unstarted_leaves_a_flow_it_no_longer_owns_alone() {
        let service = VerificationService::new();
        let captured = Arc::new(StdMutex::new(Vec::<u32>::new()));
        let sink = captured.clone();
        service.on_state_changed(Box::new(move |state, _flow_id| {
            sink.lock().expect("sink not poisoned").push(state);
        }));

        // The state a watcher's own failure leaves behind once its reset has
        // landed: no request, and the generation moved on.
        let generation = service.ctx.lock().await.generation;
        service.reset_context().await;
        service
            .fail_flow_if_unstarted("$flow:example.org", generation, "accept send failed")
            .await;

        // Same generation, but the flow is gone from ctx.
        let generation = service.ctx.lock().await.generation;
        service
            .fail_flow_if_unstarted("$flow:example.org", generation, "accept send failed")
            .await;

        assert!(
            captured.lock().expect("captured not poisoned").is_empty(),
            "a flow this call no longer owns must not be cancelled or reported"
        );
    }

    // Cancel info must reach the UI before the Cancelled state so the page can
    // choose the message severity, and must not survive clear_callbacks.
    #[test]
    fn cancel_info_callback_fires_and_clears() {
        let service = VerificationService::new();
        let captured = Arc::new(StdMutex::new(Vec::<(String, String, bool)>::new()));
        let sink = captured.clone();
        service.on_cancel_info(Box::new(move |flow_id, code, by_us| {
            sink.lock().expect("sink not poisoned").push((
                flow_id.to_string(),
                code.to_string(),
                by_us,
            ));
        }));
        service.emit_cancel_info("$flow:example.org", "m.mismatched_sas", false);
        assert_eq!(
            captured.lock().expect("captured not poisoned").clone(),
            vec![("$flow:example.org".into(), "m.mismatched_sas".into(), false)]
        );
        service.clear_callbacks();
        service.emit_cancel_info("$flow:example.org", "m.user", true);
        assert_eq!(captured.lock().expect("captured not poisoned").len(), 1);
    }

    // The flow id tagged onto an emitted state is the current flow id, so the UI
    // can attribute states to the right flow (P0-5).
    #[test]
    fn emitted_state_carries_current_flow_id() {
        let service = VerificationService::new();
        let captured = Arc::new(StdMutex::new(Vec::<(u32, String)>::new()));
        let sink = captured.clone();
        service.on_state_changed(Box::new(move |state, flow_id| {
            sink.lock()
                .expect("sink not poisoned")
                .push((state, flow_id.to_string()));
        }));

        service.set_current_flow_id("$flow:example.org");
        service.emit_state(VerificationState::SasStarted);
        service.emit_state_for(VerificationState::Cancelled, "$other:example.org");

        let events = captured.lock().expect("captured not poisoned").clone();
        assert_eq!(
            events,
            vec![
                (
                    VerificationState::SasStarted as u32,
                    "$flow:example.org".to_string()
                ),
                (
                    VerificationState::Cancelled as u32,
                    "$other:example.org".to_string()
                ),
            ]
        );
    }

    // Replay with no live matching request must stay silent and clear the
    // stale pending entry (the request died while nobody was listening).
    #[tokio::test]
    async fn replay_without_live_request_is_silent_and_clears() {
        let service = VerificationService::new();
        let fired = Arc::new(StdMutex::new(0u32));
        let sink = fired.clone();
        service.on_incoming_request(Box::new(move |_, _, _| {
            *sink.lock().expect("sink not poisoned") += 1;
        }));
        *lock_verification_mutex(&service.pending_incoming, "pending_incoming") =
            Some(PendingIncomingRequest {
                flow_id: "$flow:example.org".into(),
                is_user: false,
                counterpart_id: "DEVICEID".into(),
                display_label: "Laptop".into(),
            });
        service.replay_pending_incoming_request().await;
        assert_eq!(*fired.lock().expect("fired not poisoned"), 0);
        assert!(lock_verification_mutex(&service.pending_incoming, "pending_incoming").is_none());
    }

    // `clear_callbacks()` (called on logout) must drop `pending_incoming` too:
    // this service outlives one login, so a request remembered under the
    // departing session must not replay into the next one it gets reused for.
    #[test]
    fn clear_callbacks_drops_pending_incoming() {
        let service = VerificationService::new();
        *lock_verification_mutex(&service.pending_incoming, "pending_incoming") =
            Some(PendingIncomingRequest {
                flow_id: "$flow:example.org".into(),
                is_user: false,
                counterpart_id: "DEVICEID".into(),
                display_label: "Laptop".into(),
            });
        service.clear_callbacks();
        assert!(lock_verification_mutex(&service.pending_incoming, "pending_incoming").is_none());
    }
}

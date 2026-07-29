// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::collections::BTreeMap;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, Instant};

use anyhow::{anyhow, Result};
use futures_util::StreamExt;
use matrix_sdk::encryption::identities::UserIdentity;
use matrix_sdk::encryption::verification::{
    QrVerification, QrVerificationState, SasState, SasVerification, VerificationRequest,
    VerificationRequestState,
};
use matrix_sdk::ruma::api::client::to_device::send_event_to_device::v3::Request as RumaToDeviceRequest;
use matrix_sdk::ruma::events::key::verification::request::ToDeviceKeyVerificationRequestEventContent;
use matrix_sdk::ruma::events::key::verification::VerificationMethod;
use matrix_sdk::ruma::events::room::message::{MessageType, OriginalSyncRoomMessageEvent};
use matrix_sdk::ruma::events::ToDeviceEvent;
use matrix_sdk::ruma::events::{AnyToDeviceEventContent, ToDeviceEventContent};
use matrix_sdk::ruma::serde::Raw;
use matrix_sdk::ruma::to_device::DeviceIdOrAllDevices;
use matrix_sdk::ruma::{MilliSecondsSinceUnixEpoch, OwnedUserId, TransactionId, UserId};
use matrix_sdk::{Client, Room};
use tracing::{info, warn};

use crate::matrix::SYNC_STATE_SYNCED;
use crate::types::{
    QrCodeImage, SasEmoji, UserTrustState, VerificationCapabilities, VerificationState,
};

const VERIFICATION_TRANSPORT_READY_TIMEOUT: Duration = Duration::from_secs(30);
const VERIFICATION_READY_TIMEOUT: Duration = Duration::from_secs(120);
const VERIFICATION_TRANSITIONED_BEFORE_SELECTED_METHOD: &str =
    "Verification request transitioned before this client could start the selected method";

type VerificationStateCallback = Box<dyn Fn(u32, &str) + Send>;
type IncomingVerificationRequestCallback = Box<dyn Fn(&str, &str, &str) + Send>;
type UserTrustChangedCallback = Box<dyn Fn(&str, u32) + Send>;
type IncomingUserVerificationRequestCallback = Box<dyn Fn(&str, &str, &str) + Send>;
/// Fires with a flow id when an incoming request can no longer be answered.
type VerificationRequestClosedCallback = Box<dyn Fn(&str) + Send>;

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

/// Internal state for an active sign-in verification flow.
struct VerificationContext {
    request: Option<VerificationRequest>,
    sas: Option<SasVerification>,
    qr: Option<QrVerification>,
    /// Generation counter to detect stale callbacks.
    generation: u64,
}

impl VerificationContext {
    fn new() -> Self {
        Self {
            request: None,
            sas: None,
            qr: None,
            generation: 0,
        }
    }

    fn reset(&mut self) {
        self.request = None;
        self.sas = None;
        self.qr = None;
        self.generation += 1;
    }
}

#[derive(Clone)]
pub(crate) struct VerificationService {
    ctx: Arc<tokio::sync::Mutex<VerificationContext>>,
    sync_state: Arc<AtomicU32>,
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
}

impl VerificationService {
    pub(crate) fn new(sync_state: Arc<AtomicU32>) -> Self {
        Self {
            ctx: Arc::new(tokio::sync::Mutex::new(VerificationContext::new())),
            sync_state,
            state_callback: Arc::new(Mutex::new(None)),
            incoming_request_callback: Arc::new(Mutex::new(None)),
            user_trust_changed_callback: Arc::new(Mutex::new(None)),
            incoming_user_request_callback: Arc::new(Mutex::new(None)),
            request_closed_callback: Arc::new(Mutex::new(None)),
            start_guard: Arc::new(tokio::sync::Mutex::new(())),
            current_flow_id: Arc::new(Mutex::new(String::new())),
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
        let client_for_verification = client.clone();
        verification_debug!("registering incoming self-verification request handler");
        client.add_event_handler(
            move |ev: ToDeviceEvent<ToDeviceKeyVerificationRequestEventContent>| {
                let verification_ctx = verification_ctx.clone();
                let verification_state_callback = verification_state_callback.clone();
                let incoming_verification_request_callback =
                    incoming_verification_request_callback.clone();
                let request_closed_callback = request_closed_callback.clone();
                let client = client_for_verification.clone();
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
                            ctx.request = Some(request.clone());
                            ctx.sas = None;
                            stored_request = true;
                        }
                    }

                    if stored_request {
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
                        Self::spawn_incoming_request_watcher(request, request_closed_callback);
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
        let client_for_verification = client.clone();
        verification_debug!("registering incoming user-verification request handler");
        client.add_event_handler(move |ev: OriginalSyncRoomMessageEvent, room: Room| {
            let verification_ctx = verification_ctx.clone();
            let verification_state_callback = verification_state_callback.clone();
            let incoming_user_request_callback = incoming_user_request_callback.clone();
            let request_closed_callback = request_closed_callback.clone();
            let client = client_for_verification.clone();
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
                        ctx.request = Some(request.clone());
                        ctx.sas = None;
                        stored = true;
                    }
                }

                if stored {
                    let cb = lock_verification_mutex(
                        &incoming_user_request_callback,
                        "incoming_user_verification_request_callback",
                    );
                    if let Some(ref f) = *cb {
                        f(&flow_id, sender.as_str(), &display_name);
                    }
                    Self::spawn_incoming_request_watcher(request, request_closed_callback);
                }

                let cb = lock_verification_mutex(
                    &verification_state_callback,
                    "verification_state_callback",
                );
                if let Some(ref f) = *cb {
                    f(VerificationState::WaitingForReady as u32, &flow_id);
                }
            }
        });
    }

    pub(crate) async fn start_sas_verification_for(
        &self,
        expected_flow_id: &str,
    ) -> Result<Vec<SasEmoji>> {
        let request = self
            .active_verification_request_for(expected_flow_id)
            .await
            .map_err(|e| anyhow!("No matching verification request: {e}"))?;
        self.start_sas_verification_from_request(request, Some(expected_flow_id))
            .await
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

    pub(crate) async fn start_sas_verification_checked(
        &self,
        client: Client,
        expected_flow_id: Option<&str>,
    ) -> Result<Vec<SasEmoji>> {
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

        self.start_sas_verification_from_request(request, expected_flow_id)
            .await
    }

    /// Start an interactive SAS (emoji) verification of ANOTHER user's identity.
    /// Sends an in-room verification request to the user (the SDK routes it
    /// through a shared DM, creating one if none exists), waits for it to become
    /// ready, then starts SAS and returns the emojis to compare. On a successful
    /// SAS the SDK signs the user's master key with our user-signing key.
    pub(crate) async fn start_user_verification(
        &self,
        client: Client,
        user_id: &str,
    ) -> Result<Vec<SasEmoji>> {
        let _start_guard = self.start_guard.lock().await;
        // `ensure_user_verification_request` performs its own flow-scoped context
        // cleanup on failure, so the outer handler only surfaces the error state;
        // a blanket `reset_context()` here would wipe a concurrent flow (e.g. an
        // incoming request that replaced `ctx` while we waited for readiness).
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
        self.start_sas_verification_from_request(request, None)
            .await
    }

    /// Create + send an outgoing verification request to another user and store
    /// it in the active context so `start_sas_verification_from_request` accepts
    /// it. Fetches the target's cross-signing identity (cache first, then a
    /// server key query) — a user without one cannot be verified.
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

        {
            let mut ctx = self.ctx.lock().await;
            ctx.request = Some(request.clone());
            ctx.sas = None;
        }

        if let Err(e) = self.wait_for_verification_request_ready(&request).await {
            // Only clear the context if it is still ours — an incoming request may
            // have replaced it while we waited (these handlers don't hold
            // `start_guard`).
            self.reset_context_if_current(Some(&flow_id)).await;
            return Err(e);
        }
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

        sas.confirm().await?;

        let mut changes = sas.changes();

        let result = tokio::time::timeout(Duration::from_secs(30), async {
            // `changes()` does not replay the current state, so if the other
            // device already confirmed/cancelled by the time we subscribe, the
            // next transition never comes. Check the terminal state once first.
            if sas.is_done() {
                return Ok(());
            }
            if sas.is_cancelled() {
                return Err(anyhow!("SAS cancelled after confirm"));
            }
            while let Some(state) = changes.next().await {
                match state {
                    SasState::Done { .. } => return Ok(()),
                    SasState::Cancelled(info) => {
                        return Err(anyhow!("SAS cancelled after confirm: {}", info.reason()));
                    }
                    _ => continue,
                }
            }
            Err(anyhow!("SAS stream ended without reaching Done"))
        })
        .await;

        match result {
            Ok(Ok(())) => {
                self.emit_state(VerificationState::Done);
                self.reset_context().await;
                Ok(())
            }
            Ok(Err(e)) => {
                self.emit_state(VerificationState::Cancelled);
                self.reset_context().await;
                Err(e)
            }
            Err(_) => {
                if sas.is_done() {
                    self.emit_state(VerificationState::Done);
                    self.reset_context().await;
                    Ok(())
                } else {
                    self.emit_state(VerificationState::Cancelled);
                    self.reset_context().await;
                    Err(anyhow!(
                        "SAS confirmation timed out before the other device confirmed"
                    ))
                }
            }
        }
    }

    pub(crate) async fn start_qr_verification_for(
        &self,
        expected_flow_id: &str,
    ) -> Result<QrCodeImage> {
        let request = self
            .active_verification_request_for(expected_flow_id)
            .await
            .map_err(|e| anyhow!("No matching verification request: {e}"))?;
        self.start_qr_verification_from_request(request, Some(expected_flow_id))
            .await
    }

    pub(crate) async fn start_qr_verification_checked(
        &self,
        client: Client,
        expected_flow_id: Option<&str>,
    ) -> Result<QrCodeImage> {
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
        self.start_qr_verification_from_request(request, expected_flow_id)
            .await
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
        qr.confirm().await?;

        let mut changes = qr.changes();
        let result = tokio::time::timeout(Duration::from_secs(30), async {
            // `changes()` does not replay current state — check terminal states
            // once before awaiting the next transition.
            if qr.is_done() {
                return Ok(());
            }
            if qr.is_cancelled() {
                return Err(anyhow!("QR cancelled after confirm"));
            }
            while let Some(state) = changes.next().await {
                match state {
                    QrVerificationState::Done { .. } => return Ok(()),
                    QrVerificationState::Cancelled(info) => {
                        return Err(anyhow!("QR cancelled after confirm: {}", info.reason()));
                    }
                    _ => continue,
                }
            }
            Err(anyhow!("QR stream ended without reaching Done"))
        })
        .await;

        match result {
            Ok(Ok(())) => {
                self.emit_state(VerificationState::Done);
                self.reset_context().await;
                Ok(())
            }
            Ok(Err(e)) => {
                self.emit_state(VerificationState::Cancelled);
                self.reset_context().await;
                Err(e)
            }
            Err(_) => {
                if qr.is_done() {
                    self.emit_state(VerificationState::Done);
                    self.reset_context().await;
                    Ok(())
                } else {
                    self.emit_state(VerificationState::Cancelled);
                    self.reset_context().await;
                    Err(anyhow!(
                        "QR confirmation timed out before the other device confirmed"
                    ))
                }
            }
        }
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

    async fn reset_context_if_current(&self, expected_flow_id: Option<&str>) {
        let mut ctx = self.ctx.lock().await;
        if let Some(expected) = expected_flow_id {
            let still_current = ctx
                .request
                .as_ref()
                .map(|request| request.flow_id() == expected)
                .unwrap_or(false);
            if !still_current {
                return;
            }
        }
        ctx.reset();
    }

    async fn start_sas_verification_from_request(
        &self,
        request: VerificationRequest,
        expected_flow_id: Option<&str>,
    ) -> Result<Vec<SasEmoji>> {
        let selected_flow_id = request.flow_id().to_string();
        self.set_current_flow_id(&selected_flow_id);
        if let Some(expected) = expected_flow_id {
            if selected_flow_id != expected {
                return Err(anyhow!(
                    "Verification flow_id={} does not match expected flow_id={}",
                    selected_flow_id,
                    expected
                ));
            }
        }

        if !request.is_ready() {
            if let Err(e) = self.wait_for_verification_request_ready(&request).await {
                self.emit_state(VerificationState::Cancelled);
                self.reset_context_if_current(expected_flow_id).await;
                return Err(e);
            }
        }

        {
            let ctx = self.ctx.lock().await;
            let active_flow_id = ctx
                .request
                .as_ref()
                .map(|request| request.flow_id().to_string());
            if active_flow_id.as_deref() != Some(selected_flow_id.as_str()) {
                drop(ctx);
                // Emit a terminal state tagged with OUR flow id so this flow's UI
                // hides, without disturbing the flow that replaced us.
                self.emit_state_for(VerificationState::Cancelled, &selected_flow_id);
                return Err(anyhow!(
                    "Active verification flow does not match selected flow_id={}",
                    selected_flow_id
                ));
            }
        }

        if !request.is_ready() {
            self.emit_state(VerificationState::Cancelled);
            self.reset_context_if_current(expected_flow_id).await;
            return Err(anyhow!("Verification request is not ready"));
        }

        self.emit_state(VerificationState::SasStarted);

        let sas = request
            .start_sas()
            .await?
            .ok_or_else(|| anyhow!("Failed to start SAS verification"))?;

        let mut changes = sas.changes();
        let emojis_result = tokio::time::timeout(Duration::from_secs(60), async {
            if sas.can_be_presented() {
                if let Some(emoji_arr) = sas.emoji() {
                    return Ok(emoji_arr);
                }
            }

            while let Some(state) = changes.next().await {
                match state {
                    SasState::KeysExchanged { .. } => {
                        if let Some(emoji_arr) = sas.emoji() {
                            return Ok(emoji_arr);
                        }
                    }
                    SasState::Done { .. } => {
                        // `changes()` coalesces transitions, so KeysExchanged may
                        // be folded into a single Done delivery. Take the emojis
                        // if they are present rather than failing outright.
                        if let Some(emoji_arr) = sas.emoji() {
                            return Ok(emoji_arr);
                        }
                        return Err(anyhow!("SAS completed before emojis shown"));
                    }
                    SasState::Cancelled(info) => {
                        return Err(anyhow!("SAS cancelled: {}", info.reason()));
                    }
                    _ => {
                        if let Some(emoji_arr) = sas.emoji() {
                            return Ok(emoji_arr);
                        }
                    }
                }
            }
            Err(anyhow!("SAS stream ended without emojis"))
        })
        .await
        .map_err(|_| anyhow!("Timed out waiting for SAS emojis"));

        let emojis: [matrix_sdk::encryption::verification::Emoji; 7] = match emojis_result {
            Ok(Ok(e)) => e,
            Ok(Err(e)) | Err(e) => {
                self.emit_state(VerificationState::Cancelled);
                self.reset_context_if_current(expected_flow_id).await;
                return Err(e);
            }
        };

        let sas_clone = sas.clone();
        let spawned_generation;
        {
            let mut ctx = self.ctx.lock().await;
            let still_current = ctx
                .request
                .as_ref()
                .map(|request| request.flow_id() == selected_flow_id.as_str())
                .unwrap_or(false);
            if !still_current {
                drop(ctx);
                let _ = sas.cancel().await;
                self.emit_state_for(VerificationState::Cancelled, &selected_flow_id);
                return Err(anyhow!(
                    "Active verification request changed before SAS was stored"
                ));
            }
            ctx.sas = Some(sas);
            spawned_generation = ctx.generation;
        }

        self.emit_state(VerificationState::SasEmojisAvailable);

        {
            let verification_ctx = self.ctx.clone();
            let state_cb = self.state_callback.clone();
            let spawned_flow_id = selected_flow_id.clone();
            tokio::spawn(async move {
                let mut changes = sas_clone.changes();
                // `changes()` does not replay the current state, so a Cancelled
                // that landed before we subscribed would be missed. Seed the
                // loop with the current state, then follow future transitions.
                let mut next_state = Some(sas_clone.state());
                while let Some(state) = match next_state.take() {
                    Some(state) => Some(state),
                    None => changes.next().await,
                } {
                    match state {
                        SasState::Cancelled(_) => {
                            let mut ctx = verification_ctx.lock().await;
                            let same_flow = ctx
                                .request
                                .as_ref()
                                .map(|request| request.flow_id() == spawned_flow_id.as_str())
                                .unwrap_or(false);
                            if ctx.generation != spawned_generation || !same_flow {
                                break;
                            }
                            let cb =
                                lock_verification_mutex(&state_cb, "verification_state_callback");
                            if let Some(ref f) = *cb {
                                f(VerificationState::Cancelled as u32, &spawned_flow_id);
                            }
                            ctx.reset();
                            break;
                        }
                        SasState::Done { .. } => break,
                        _ => continue,
                    }
                }
            });
        }

        Ok(emojis
            .iter()
            .map(|e| SasEmoji {
                emoji: e.symbol.to_string(),
                label: e.description.to_string(),
            })
            .collect())
    }

    async fn start_qr_verification_from_request(
        &self,
        request: VerificationRequest,
        expected_flow_id: Option<&str>,
    ) -> Result<QrCodeImage> {
        let selected_flow_id = request.flow_id().to_string();
        self.set_current_flow_id(&selected_flow_id);
        if let Some(expected) = expected_flow_id {
            if selected_flow_id != expected {
                return Err(anyhow!(
                    "Verification flow_id={} does not match expected flow_id={}",
                    selected_flow_id,
                    expected
                ));
            }
        }

        if !request.is_ready() {
            if let Err(e) = self.wait_for_verification_request_ready(&request).await {
                self.emit_state(VerificationState::Cancelled);
                self.reset_context_if_current(expected_flow_id).await;
                return Err(e);
            }
        }

        let qr = match request.generate_qr_code().await {
            Ok(Some(qr)) => qr,
            Ok(None) => {
                self.emit_state(VerificationState::Cancelled);
                self.reset_context_if_current(expected_flow_id).await;
                return Err(anyhow!(
                    "QR verification unavailable (the other device can't scan, or cross-signing is missing)"
                ));
            }
            Err(e) => {
                self.emit_state(VerificationState::Cancelled);
                self.reset_context_if_current(expected_flow_id).await;
                return Err(anyhow!("Failed to generate QR code: {e}"));
            }
        };

        let code = qr
            .to_qr_code()
            .map_err(|e| anyhow!("Failed to render QR code: {e}"))?;
        let size = code.width();
        let modules: Vec<u8> = code
            .to_colors()
            .into_iter()
            .map(|c| u8::from(c == qrcode::Color::Dark))
            .collect();

        let qr_clone = qr.clone();
        let spawned_generation;
        {
            let mut ctx = self.ctx.lock().await;
            let still_current = ctx
                .request
                .as_ref()
                .map(|request| request.flow_id() == selected_flow_id.as_str())
                .unwrap_or(false);
            if !still_current {
                drop(ctx);
                let _ = qr.cancel().await;
                self.emit_state_for(VerificationState::Cancelled, &selected_flow_id);
                return Err(anyhow!(
                    "Active verification request changed before QR was stored"
                ));
            }
            ctx.qr = Some(qr);
            spawned_generation = ctx.generation;
        }

        self.emit_state(VerificationState::QrCodeReady);

        {
            let verification_ctx = self.ctx.clone();
            let state_cb = self.state_callback.clone();
            let spawned_flow_id = selected_flow_id.clone();
            tokio::spawn(async move {
                let mut changes = qr_clone.changes();
                // `changes()` does not replay the current state, so a Scanned (or
                // Cancelled) that landed before we subscribed would be missed and
                // the "Continue" button would never enable. Seed the loop with the
                // current state, then follow future transitions.
                let mut next_state = Some(qr_clone.state());
                while let Some(state) = match next_state.take() {
                    Some(state) => Some(state),
                    None => changes.next().await,
                } {
                    match state {
                        QrVerificationState::Scanned => {
                            let ctx = verification_ctx.lock().await;
                            let same_flow = ctx
                                .request
                                .as_ref()
                                .map(|request| request.flow_id() == spawned_flow_id.as_str())
                                .unwrap_or(false);
                            if ctx.generation != spawned_generation || !same_flow {
                                break;
                            }
                            let cb =
                                lock_verification_mutex(&state_cb, "verification_state_callback");
                            if let Some(ref f) = *cb {
                                f(VerificationState::QrCodeScanned as u32, &spawned_flow_id);
                            }
                        }
                        QrVerificationState::Cancelled(_) => {
                            let mut ctx = verification_ctx.lock().await;
                            let same_flow = ctx
                                .request
                                .as_ref()
                                .map(|request| request.flow_id() == spawned_flow_id.as_str())
                                .unwrap_or(false);
                            if ctx.generation != spawned_generation || !same_flow {
                                break;
                            }
                            let cb =
                                lock_verification_mutex(&state_cb, "verification_state_callback");
                            if let Some(ref f) = *cb {
                                f(VerificationState::Cancelled as u32, &spawned_flow_id);
                            }
                            drop(cb);
                            ctx.reset();
                            break;
                        }
                        // Done is driven by confirm_qr_scanned, which emits Done
                        // and resets — just stop watching here.
                        QrVerificationState::Done { .. } => break,
                        _ => continue,
                    }
                }
            });
        }

        Ok(QrCodeImage { size, modules })
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

    async fn own_device_counts_for_verification(
        client: &Client,
        own_user_id: &OwnedUserId,
    ) -> Result<(usize, usize)> {
        let devices = client.encryption().get_user_devices(own_user_id).await?;
        let current_device_id = client.device_id().map(|id| id.to_string());
        let mut other_device_count = 0;
        let mut signed_other_device_count = 0;
        for device in devices.devices() {
            if Some(device.device_id().as_str()) == current_device_id.as_deref() {
                continue;
            }
            other_device_count += 1;
            let signed = device.is_cross_signed_by_owner();
            verification_debug!(
                "verification candidate device id={} signed_by_owner={} has_curve25519={} dehydrated={}",
                device.device_id(),
                signed,
                device.curve25519_key().is_some(),
                device.is_dehydrated()
            );
            if signed {
                signed_other_device_count += 1;
            }
        }
        Ok((other_device_count, signed_other_device_count))
    }

    async fn wait_for_verification_transport_ready(
        &self,
        client: &Client,
        own_user_id: &OwnedUserId,
    ) -> Result<UserIdentity> {
        verification_debug!("waiting for verification transport readiness");
        client
            .encryption()
            .wait_for_e2ee_initialization_tasks()
            .await;
        verification_debug!("e2ee initialization tasks completed before verification request");

        let started = Instant::now();
        let mut last_wait_log = Duration::ZERO;
        loop {
            let last_error = if self.sync_state.load(Ordering::Acquire) != SYNC_STATE_SYNCED {
                "initial sync has not completed".to_string()
            } else {
                match Self::own_identity_for_verification(client, own_user_id).await {
                    Ok(identity) => {
                        let has_verification_target =
                            match client.encryption().has_devices_to_verify_against().await {
                                Ok(value) => value,
                                Err(e) => {
                                    warn!("Failed to check devices to verify against: {e}");
                                    true
                                }
                            };
                        match Self::own_device_counts_for_verification(client, own_user_id).await {
                            Ok((other_count, signed_other_count)) => {
                                if has_verification_target && signed_other_count > 0 {
                                    if started.elapsed() > Duration::from_millis(250) {
                                        info!(
                                            "Verification transport ready after {:?}: other={}, signed_other={}",
                                            started.elapsed(),
                                            other_count,
                                            signed_other_count
                                        );
                                    }
                                    return Ok(identity);
                                }
                                format!(
                                    "no signed verification recipient yet (other={other_count}, signed_other={signed_other_count}, has_target={has_verification_target})"
                                )
                            }
                            Err(e) => format!("own device list unavailable: {e}"),
                        }
                    }
                    Err(e) => format!("own identity unavailable: {e}"),
                }
            };

            if started.elapsed() >= VERIFICATION_TRANSPORT_READY_TIMEOUT {
                return Err(anyhow!(
                    "Timed out waiting for verification transport readiness: {}",
                    last_error
                ));
            }
            let elapsed = started.elapsed();
            if elapsed.saturating_sub(last_wait_log) >= Duration::from_secs(1) {
                verification_debug!(
                    "verification transport not ready after {:?}: {}",
                    elapsed,
                    last_error
                );
                last_wait_log = elapsed;
            }
            tokio::time::sleep(Duration::from_millis(250)).await;
        }
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
            Self::emit_request_closed(&closed_callback, &flow_id);
        });
    }

    fn verification_request_state_name(state: &VerificationRequestState) -> &'static str {
        match state {
            VerificationRequestState::Created { .. } => "Created",
            VerificationRequestState::Requested { .. } => "Requested",
            VerificationRequestState::Ready { .. } => "Ready",
            VerificationRequestState::Transitioned { .. } => "Transitioned",
            VerificationRequestState::Done => "Done",
            VerificationRequestState::Cancelled(_) => "Cancelled",
        }
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

    async fn wait_for_verification_request_ready(
        &self,
        request: &VerificationRequest,
    ) -> Result<()> {
        self.wait_for_verification_request_ready_for(request, VERIFICATION_READY_TIMEOUT)
            .await
    }

    async fn wait_for_verification_request_ready_for(
        &self,
        request: &VerificationRequest,
        timeout: Duration,
    ) -> Result<()> {
        verification_debug!(
            "waiting for request Ready flow_id={} we_started={} self_verification={} timeout={:?}",
            request.flow_id(),
            request.we_started(),
            request.is_self_verification(),
            timeout
        );
        self.emit_state(VerificationState::WaitingForReady);

        let started = Instant::now();
        let mut changes = request.changes();
        let mut accepted_incoming = false;
        let mut last_logged_state = "";
        loop {
            let state = request.state();
            let state_name = Self::verification_request_state_name(&state);
            if state_name != last_logged_state {
                verification_debug!(
                    "request state flow_id={} state={}",
                    request.flow_id(),
                    Self::verification_request_state_details(&state)
                );
                last_logged_state = state_name;
            }

            match state {
                VerificationRequestState::Ready { .. } => {
                    self.emit_state(VerificationState::Ready);
                    return Ok(());
                }
                VerificationRequestState::Requested { .. } if !accepted_incoming => {
                    verification_debug!(
                        "Accepting incoming verification request, flow_id={}",
                        request.flow_id()
                    );
                    request
                        .accept_with_methods(Self::accepted_verification_methods())
                        .await?;
                    verification_debug!(
                        "accepted incoming verification request, flow_id={}",
                        request.flow_id()
                    );
                    accepted_incoming = true;
                    continue;
                }
                VerificationRequestState::Done => {
                    return Err(anyhow!(
                        "Verification request completed without becoming ready"
                    ));
                }
                VerificationRequestState::Cancelled(info) => {
                    return Err(anyhow!("Verification request cancelled: {}", info.reason()));
                }
                VerificationRequestState::Transitioned { .. } => {
                    return Err(anyhow!(VERIFICATION_TRANSITIONED_BEFORE_SELECTED_METHOD));
                }
                _ => {}
            }

            let elapsed = started.elapsed();
            if elapsed >= timeout {
                return Err(anyhow!(
                    "Verification request timed out waiting for other device"
                ));
            }

            let remaining = timeout.saturating_sub(elapsed);
            let wait_for = remaining.min(Duration::from_millis(250));
            match tokio::time::timeout(wait_for, changes.next()).await {
                Ok(Some(state)) => {
                    let state_name = Self::verification_request_state_name(&state);
                    if state_name != last_logged_state {
                        verification_debug!(
                            "request stream state flow_id={} state={}",
                            request.flow_id(),
                            Self::verification_request_state_details(&state)
                        );
                        last_logged_state = state_name;
                    }
                    match state {
                        VerificationRequestState::Ready { .. } => {
                            self.emit_state(VerificationState::Ready);
                            return Ok(());
                        }
                        VerificationRequestState::Requested { .. } if !accepted_incoming => {
                            verification_debug!(
                                "Accepting incoming verification request, flow_id={}",
                                request.flow_id()
                            );
                            request
                                .accept_with_methods(Self::accepted_verification_methods())
                                .await?;
                            verification_debug!(
                                "accepted incoming verification request, flow_id={}",
                                request.flow_id()
                            );
                            accepted_incoming = true;
                        }
                        VerificationRequestState::Done => {
                            return Err(anyhow!(
                                "Verification request completed without becoming ready"
                            ));
                        }
                        VerificationRequestState::Cancelled(info) => {
                            return Err(anyhow!(
                                "Verification request cancelled: {}",
                                info.reason()
                            ));
                        }
                        VerificationRequestState::Transitioned { .. } => {
                            return Err(anyhow!(VERIFICATION_TRANSITIONED_BEFORE_SELECTED_METHOD));
                        }
                        _ => {}
                    }
                }
                Err(_) => {}
                Ok(None) => {
                    return Err(anyhow!("Verification request stream ended unexpectedly"));
                }
            }
        }
    }

    async fn send_element_style_own_verification_broadcast(
        client: &Client,
        own_user_id: &OwnedUserId,
        request: &VerificationRequest,
    ) -> Result<()> {
        let Some(own_device_id) = client.device_id() else {
            return Err(anyhow!("No own device id for verification broadcast"));
        };

        let event_content = AnyToDeviceEventContent::KeyVerificationRequest(
            ToDeviceKeyVerificationRequestEventContent::new(
                own_device_id.to_owned(),
                request.flow_id().to_owned().into(),
                Self::outgoing_verification_methods(),
                MilliSecondsSinceUnixEpoch::now(),
            ),
        );
        let event_type = event_content.event_type();
        let raw_content = Raw::new(&event_content)
            .map_err(|e| anyhow!("Failed to serialize verification request: {e}"))?;

        let mut device_messages = BTreeMap::new();
        device_messages.insert(DeviceIdOrAllDevices::AllDevices, raw_content);
        let mut messages = BTreeMap::new();
        messages.insert(own_user_id.to_owned(), device_messages);

        let to_device_request =
            RumaToDeviceRequest::new_raw(event_type, TransactionId::new(), messages);

        verification_debug!(
            "sending Element-style own verification broadcast flow_id={} to device_id=* methods=[{}]",
            request.flow_id(),
            Self::verification_methods_debug(&Self::outgoing_verification_methods())
        );
        client.send(to_device_request).await?;
        verification_debug!(
            "sent Element-style own verification broadcast flow_id={} to device_id=*",
            request.flow_id()
        );
        Ok(())
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
                if req.is_ready() {
                    verification_debug!(
                        "existing verification request already ready flow_id={}",
                        req.flow_id()
                    );
                    return Ok(req.clone());
                }
                if !req.is_done() && !req.is_cancelled() {
                    let request = req.clone();
                    drop(ctx);
                    if let Err(e) = self.wait_for_verification_request_ready(&request).await {
                        verification_debug!(
                            "existing verification request wait failed flow_id={} error={}",
                            request.flow_id(),
                            e
                        );
                        let mut ctx = self.ctx.lock().await;
                        ctx.reset();
                        return Err(e);
                    }
                    verification_debug!(
                        "existing verification request became ready flow_id={}",
                        request.flow_id()
                    );
                    return Ok(request);
                }
            }
        }

        verification_debug!(
            "creating outgoing self-verification request methods=[{}]",
            Self::verification_methods_debug(&Self::outgoing_verification_methods())
        );
        self.emit_state(VerificationState::RequestingVerification);

        let identity = self
            .wait_for_verification_transport_ready(&client, &own_user_id)
            .await?;

        let request = identity
            .request_verification_with_methods(Self::outgoing_verification_methods())
            .await?;
        Self::send_element_style_own_verification_broadcast(&client, &own_user_id, &request)
            .await?;
        verification_debug!(
            "created outgoing self-verification request source=own-user flow_id={} state={}",
            request.flow_id(),
            Self::verification_request_state_details(&request.state())
        );

        {
            let mut ctx = self.ctx.lock().await;
            ctx.request = Some(request.clone());
            ctx.sas = None;
        }

        if let Err(e) = self.wait_for_verification_request_ready(&request).await {
            verification_debug!(
                "outgoing verification request wait failed source=own-user flow_id={} error={}",
                request.flow_id(),
                e
            );
            let mut ctx = self.ctx.lock().await;
            ctx.reset();
            Err(e)
        } else {
            verification_debug!(
                "outgoing verification request became ready source=own-user flow_id={}",
                request.flow_id()
            );
            Ok(request)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicU32;
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
        let service = VerificationService::new(Arc::new(AtomicU32::new(0)));
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
        let service = VerificationService::new(Arc::new(AtomicU32::new(0)));
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
        let service = VerificationService::new(Arc::new(AtomicU32::new(0)));
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
    // and a blanket reset there would strand that other flow.
    #[tokio::test]
    async fn reset_context_if_current_is_flow_scoped() {
        let service = VerificationService::new(Arc::new(AtomicU32::new(0)));
        let gen_before = service.ctx.lock().await.generation;

        // No request stored: a flow-scoped reset for some other flow is a no-op.
        service
            .reset_context_if_current(Some("$other:example.org"))
            .await;
        assert_eq!(service.ctx.lock().await.generation, gen_before);

        // An unconditional reset always advances the generation.
        service.reset_context_if_current(None).await;
        assert_eq!(service.ctx.lock().await.generation, gen_before + 1);
    }

    // The flow id tagged onto an emitted state is the current flow id, so the UI
    // can attribute states to the right flow (P0-5).
    #[test]
    fn emitted_state_carries_current_flow_id() {
        let service = VerificationService::new(Arc::new(AtomicU32::new(0)));
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
}

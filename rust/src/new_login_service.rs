// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! "New login. Was this you?" detection.
//!
//! Reactively watches the account's own device list
//! (`Encryption::devices_stream`) and alerts once for a *newly discovered,
//! unverified* session that wasn't present when this session started.
//! The alert only fires once we could actually act on it — cross-signing ready
//! and this device already verified (an unverified current session can't verify
//! or trust-manage others).
//!
//! The pure [`should_alert_new_login`] decision is kept free of SDK types so it
//! is unit-testable, mirroring [`crate::notification_service::should_notify`].

use std::sync::{Arc, Mutex};

/// The new-login callback. Args: device_id, display_name, last_seen_ip,
/// last_seen_ts (UNIX secs, 0 if unknown).
pub(crate) type NewLoginFn = Box<dyn Fn(&str, &str, &str, u64) + Send>;
/// Storage slot for the (single, process-wide) new-login callback.
pub(crate) type NewLoginCallbackSlot = Arc<Mutex<Option<NewLoginFn>>>;

/// Whether a device appearing on the account should raise a "new login" alert.
///
/// `already_known` is whether the device id was in the baseline set seeded from
/// the account's device list at session start (so pre-existing sessions never
/// alert). The `cross_signing_ready` / `current_device_verified` gate means we
/// only alert when we can actually verify or sign the device out.
pub(crate) fn should_alert_new_login(
    is_current_device: bool,
    is_verified: bool,
    already_known: bool,
    cross_signing_ready: bool,
    current_device_verified: bool,
) -> bool {
    if is_current_device || is_verified || already_known {
        return false;
    }
    cross_signing_ready && current_device_verified
}

#[cfg(test)]
mod tests {
    use super::*;

    // Baseline: a brand-new unverified session on a fully set-up account.
    fn args() -> (bool, bool, bool, bool, bool) {
        (false, false, false, true, true)
    }

    fn alert(a: (bool, bool, bool, bool, bool)) -> bool {
        should_alert_new_login(a.0, a.1, a.2, a.3, a.4)
    }

    #[test]
    fn alerts_new_unverified_session() {
        assert!(alert(args()));
    }

    #[test]
    fn ignores_our_own_current_device() {
        let mut a = args();
        a.0 = true;
        assert!(!alert(a));
    }

    #[test]
    fn ignores_already_verified_session() {
        // A session that cross-signed itself is trusted, not a concern.
        let mut a = args();
        a.1 = true;
        assert!(!alert(a));
    }

    #[test]
    fn ignores_sessions_present_at_startup() {
        let mut a = args();
        a.2 = true;
        assert!(!alert(a));
    }

    #[test]
    fn suppressed_until_cross_signing_ready() {
        let mut a = args();
        a.3 = false;
        assert!(!alert(a));
    }

    #[test]
    fn suppressed_while_current_device_unverified() {
        // Can't verify or trust-manage others from an unverified session.
        let mut a = args();
        a.4 = false;
        assert!(!alert(a));
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use anyhow::{anyhow, Result};
use matrix_sdk::encryption::recovery::RecoveryError;
use matrix_sdk::ruma::api::client::uiaa;
use matrix_sdk::Client;
use matrix_sdk_crypto::secret_storage::SecretStorageKey;
use tokio::sync::Mutex;

use crate::types::{
    DeviceSession, DeviceSessionList, DeviceVerificationState, EncryptionHealthState,
    EncryptionOverview, ImportKeysResult, ResetIdentityResult,
};

type UserAgentParts = (
    Option<String>,
    Option<String>,
    Option<String>,
    Option<String>,
    Option<String>,
);

/// Why provisioning recovery failed. `BackupExistsOnServer` is kept apart from every other
/// failure because it is the one the user can act on: the account carries a key backup that no
/// recovery key unlocks, and clearing it destroys that backup, so it needs a confirmation rather
/// than an error message.
#[derive(Debug)]
pub enum RecoverySetupError {
    BackupExistsOnServer,
    Other(String),
}

/// Error codes crossing the FFI boundary. Kept in sync with `IntroSetupEncryption`'s states.
pub(crate) const RECOVERY_SETUP_ERROR_BACKUP_EXISTS: u32 = 1;
pub(crate) const RECOVERY_SETUP_ERROR_OTHER: u32 = 2;

impl RecoverySetupError {
    pub(crate) fn code(&self) -> u32 {
        match self {
            Self::BackupExistsOnServer => RECOVERY_SETUP_ERROR_BACKUP_EXISTS,
            Self::Other(_) => RECOVERY_SETUP_ERROR_OTHER,
        }
    }

    pub(crate) fn message(&self) -> String {
        match self {
            Self::BackupExistsOnServer => {
                "A key backup already exists on the homeserver".to_owned()
            }
            Self::Other(message) => message.clone(),
        }
    }
}

impl From<RecoveryError> for RecoverySetupError {
    fn from(error: RecoveryError) -> Self {
        match error {
            RecoveryError::BackupExistsOnServer => Self::BackupExistsOnServer,
            other => Self::Other(other.to_string()),
        }
    }
}

#[derive(Clone, Default)]
pub(crate) struct EncryptionService {
    // Zeroizing: the pending recovery key is a plaintext 256-bit secret; scrub it
    // from the heap when cleared (logout) or committed rather than leaving it in
    // freed memory for the rest of the process lifetime.
    pending_recovery_key: std::sync::Arc<Mutex<Option<zeroize::Zeroizing<String>>>>,
}

impl EncryptionService {
    pub(crate) fn new() -> Self {
        Self::default()
    }

    pub(crate) async fn get_own_devices(&self, client: Client) -> Result<DeviceSessionList> {
        let current_device_id = client
            .device_id()
            .map(|d| d.to_string())
            .unwrap_or_default();

        let response = client
            .devices()
            .await
            .map_err(|e| anyhow!("Failed to get devices: {e}"))?;

        let encryption = client.encryption();
        let user_id = client.user_id().ok_or_else(|| anyhow!("No user ID"))?;

        let mut sessions = Vec::with_capacity(response.devices.len());
        for device in response.devices {
            let device_id_str = device.device_id.to_string();
            let is_current = device_id_str == current_device_id;

            let verification_state = match encryption.get_device(user_id, &device.device_id).await {
                Ok(Some(crypto_device)) => {
                    if crypto_device.is_verified() {
                        DeviceVerificationState::Verified
                    } else {
                        DeviceVerificationState::Unverified
                    }
                }
                _ => DeviceVerificationState::Unverifiable,
            };

            // The ruma Device struct does not expose `last_seen_user_agent` yet.
            // When it does, replace `None` below and parse from it.
            let last_seen_user_agent: Option<String> = None;
            let (app_name, app_version, device_model, os, browser) = last_seen_user_agent
                .as_deref()
                .map(parse_user_agent)
                .unwrap_or((None, None, None, None, None));

            sessions.push(DeviceSession {
                device_id: device_id_str,
                display_name: device.display_name,
                is_current,
                is_dehydrated: false,
                last_seen_ts: device.last_seen_ts.map(|ts| u64::from(ts.as_secs())),
                last_seen_ip: device.last_seen_ip,
                last_seen_user_agent,
                app_name,
                app_version,
                device_model,
                os,
                browser,
                verification_state,
            });
        }

        Ok(DeviceSessionList {
            current_device_id,
            sessions,
        })
    }

    pub(crate) async fn rename_device(
        &self,
        client: Client,
        device_id: &str,
        display_name: &str,
    ) -> Result<()> {
        let device_id: matrix_sdk::ruma::OwnedDeviceId = device_id.into();
        client
            .rename_device(&device_id, display_name)
            .await
            .map_err(|e| anyhow!("Failed to rename device: {e}"))?;
        Ok(())
    }

    pub(crate) async fn delete_devices(
        &self,
        client: Client,
        device_ids: &[String],
        auth_json: &str,
    ) -> Result<crate::types::DeleteDevicesResult> {
        let owned_ids: Vec<matrix_sdk::ruma::OwnedDeviceId> =
            device_ids.iter().map(|id| id.as_str().into()).collect();

        let auth_data = parse_device_delete_auth(auth_json)?;

        match client.delete_devices(&owned_ids, auth_data).await {
            Ok(_) => Ok(crate::types::DeleteDevicesResult {
                completed: true,
                challenge_json: None,
                account_management_url: None,
            }),
            Err(e) => {
                if let Some(uiaa_info) = e.as_uiaa_response() {
                    let challenge = serde_json::to_string(uiaa_info).unwrap_or_default();
                    Ok(crate::types::DeleteDevicesResult {
                        completed: false,
                        challenge_json: Some(challenge),
                        account_management_url: None,
                    })
                } else if matches!(
                    e.client_api_error_kind(),
                    Some(matrix_sdk::ruma::api::error::ErrorKind::Unrecognized)
                ) {
                    // MAS/OAuth homeservers (MSC3861) disable the legacy
                    // device-management endpoint and answer M_UNRECOGNIZED.
                    // Devices there are managed via the web account portal.
                    Ok(crate::types::DeleteDevicesResult {
                        completed: false,
                        challenge_json: None,
                        account_management_url: account_management_delete_url(&client, &owned_ids)
                            .await,
                    })
                } else {
                    Err(anyhow!("Failed to delete devices: {e}"))
                }
            }
        }
    }

    pub(crate) async fn get_encryption_overview(
        &self,
        client: Client,
    ) -> Result<EncryptionOverview> {
        let encryption = client.encryption();

        let device_id = client
            .device_id()
            .map(|d| d.to_string())
            .unwrap_or_default();

        let ed25519 = encryption.ed25519_key().await;
        let verification_state = encryption.verification_state().get();
        let is_verified = matches!(
            verification_state,
            matrix_sdk::encryption::VerificationState::Verified
        );

        let cross_signing_status = encryption.cross_signing_status().await;
        let cross_signing_ready = cross_signing_status
            .as_ref()
            .map(|s| s.is_complete())
            .unwrap_or(false);
        let cross_signing_keys_cached = cross_signing_status
            .as_ref()
            .map(|s| s.has_master && s.has_self_signing)
            .unwrap_or(false);

        let recovery = encryption.recovery();
        let recovery_state = recovery.state();
        let secret_storage_ready = matches!(
            recovery_state,
            matrix_sdk::encryption::recovery::RecoveryState::Enabled
        );

        let backups = encryption.backups();
        let backup_enabled = backups.are_enabled().await;
        let history_decryptable = is_verified && backup_enabled;

        let health_state = if !is_verified {
            EncryptionHealthState::VerifyThisSession
        } else if !cross_signing_keys_cached {
            if secret_storage_ready {
                EncryptionHealthState::KeyStorageOutOfSync
            } else {
                EncryptionHealthState::IdentityNeedsReset
            }
        } else if !backup_enabled {
            EncryptionHealthState::TurnOnKeyStorage
        } else if !secret_storage_ready {
            EncryptionHealthState::SetUpRecovery
        } else {
            EncryptionHealthState::Ok
        };

        Ok(EncryptionOverview {
            device_id,
            device_ed25519: ed25519,
            is_current_device_verified: is_verified,
            cross_signing_ready,
            cross_signing_keys_cached_locally: cross_signing_keys_cached,
            cross_signing_keys_in_secret_storage: secret_storage_ready,
            secret_storage_ready,
            secret_storage_default_key_id: None,
            key_backup_upload_active: backup_enabled,
            backup_key_cached: false,
            backup_key_stored_in_4s: false,
            backup_disabled_account_flag: false,
            recovery_disabled_account_flag: false,
            history_decryptable,
            health_state,
        })
    }

    pub(crate) async fn set_key_storage_enabled(
        &self,
        client: Client,
        enabled: bool,
    ) -> Result<()> {
        let encryption = client.encryption();
        if enabled {
            encryption
                .recovery()
                .enable()
                .await
                .map_err(|e| anyhow!("Failed to enable key storage: {e}"))?;
        } else {
            encryption
                .recovery()
                .disable()
                .await
                .map_err(|e| anyhow!("Failed to disable key storage: {e}"))?;
        }
        Ok(())
    }

    /// Provision recovery for an account that has none, returning the recovery key.
    ///
    /// `Recovery::enable()` creates the server-side key backup if it is missing, creates the
    /// secret store (4S), and uploads the cross-signing keys and the backup key into it. The
    /// `String` it resolves to is the recovery key itself — the only moment it is ever available
    /// in plaintext, so the caller must show it to the user.
    pub(crate) async fn setup_recovery(
        &self,
        client: Client,
    ) -> std::result::Result<String, RecoverySetupError> {
        // The SDK bootstraps cross-signing and auto-creates the key backup in a background task
        // spawned at login/registration. Enabling recovery before those settle would upload a
        // secret store with no cross-signing keys in it.
        client
            .encryption()
            .wait_for_e2ee_initialization_tasks()
            .await;

        client
            .encryption()
            .recovery()
            .enable()
            .await
            .map_err(RecoverySetupError::from)
    }

    /// Replace an orphaned server-side key backup — one whose key was never in the secret store,
    /// so nobody can unlock it — with a fresh backup and recovery key. Destructive, and only ever
    /// called after the user confirms.
    pub(crate) async fn reset_recovery(
        &self,
        client: Client,
    ) -> std::result::Result<String, RecoverySetupError> {
        let recovery = client.encryption().recovery();
        recovery.disable().await.map_err(RecoverySetupError::from)?;
        recovery.enable().await.map_err(RecoverySetupError::from)
    }

    pub(crate) async fn enter_recovery_key(
        &self,
        client: Client,
        recovery_key: &str,
    ) -> Result<()> {
        client
            .encryption()
            .recovery()
            .recover(recovery_key)
            .await
            .map_err(|e| anyhow!("Recovery key rejected: {e}"))
    }

    pub(crate) async fn create_recovery_key(&self) -> Result<String> {
        let key = SecretStorageKey::new();
        let encoded = key.to_base58();
        let mut guard = self.pending_recovery_key.lock().await;
        *guard = Some(zeroize::Zeroizing::new(encoded.clone()));
        Ok(encoded)
    }

    /// Drop any uncommitted recovery key held in memory. Called on logout so a
    /// created-but-not-committed key (a plaintext 256-bit secret) doesn't linger
    /// for the remaining process lifetime.
    pub(crate) async fn clear_pending_recovery_key(&self) {
        let mut guard = self.pending_recovery_key.lock().await;
        *guard = None;
    }

    pub(crate) async fn commit_recovery_key(
        &self,
        client: Client,
        recovery_key: &str,
    ) -> Result<()> {
        let pending = {
            let mut guard = self.pending_recovery_key.lock().await;
            guard
                .take()
                .ok_or_else(|| anyhow!("No pending recovery key to commit"))?
        };

        if normalize_recovery_key(&pending) != normalize_recovery_key(recovery_key) {
            let mut guard = self.pending_recovery_key.lock().await;
            *guard = Some(pending);
            return Err(anyhow!(
                "Pending recovery key does not match confirmation key"
            ));
        }

        let result = client
            .encryption()
            .recovery()
            .reset_key()
            .with_passphrase(recovery_key)
            .await
            .map(|_| ())
            .map_err(|e| anyhow!("Failed to commit recovery key: {e}"));
        if result.is_err() {
            let mut guard = self.pending_recovery_key.lock().await;
            *guard = Some(pending);
        }
        result
    }

    pub(crate) async fn reset_identity(
        &self,
        client: Client,
        auth_json: &str,
    ) -> Result<ResetIdentityResult> {
        let handle = client
            .encryption()
            .recovery()
            .reset_identity()
            .await
            .map_err(|e| anyhow!("Failed to reset identity: {e}"))?;

        match handle {
            Some(reset_handle) => {
                let auth_data = if auth_json.is_empty() {
                    None
                } else {
                    parse_reset_identity_auth(auth_json)?
                };
                match reset_handle.reset(auth_data).await {
                    Ok(()) => Ok(ResetIdentityResult {
                        completed: true,
                        challenge_json: None,
                    }),
                    Err(e) => Err(anyhow!("Identity reset failed: {e}")),
                }
            }
            None => Ok(ResetIdentityResult {
                completed: true,
                challenge_json: None,
            }),
        }
    }

    pub(crate) async fn export_e2e_keys(
        &self,
        client: Client,
        path: &str,
        passphrase: &str,
    ) -> Result<()> {
        client
            .encryption()
            .export_room_keys(path.into(), passphrase, |_| true)
            .await
            .map_err(|e| anyhow!("Failed to export E2E keys: {e}"))
    }

    pub(crate) async fn import_e2e_keys(
        &self,
        client: Client,
        path: &str,
        passphrase: &str,
    ) -> Result<ImportKeysResult> {
        let result = client
            .encryption()
            .import_room_keys(path.into(), passphrase)
            .await
            .map_err(|e| anyhow!("Failed to import E2E keys: {e}"))?;
        Ok(ImportKeysResult {
            imported_count: result.imported_count as u32,
            total_count: result.total_count as u32,
        })
    }
}

/// Build the homeserver's web account-management URL for signing out a session,
/// deep-linked to the target device when exactly one is being removed. Returns
/// `None` if the server advertises no account-management URI (e.g. non-MAS).
async fn account_management_delete_url(
    client: &Client,
    device_ids: &[matrix_sdk::ruma::OwnedDeviceId],
) -> Option<String> {
    use matrix_sdk::ruma::api::client::discovery::get_authorization_server_metadata::v1::{
        AccountManagementActionData, DeviceDeleteData,
    };

    let metadata = client.oauth().server_metadata().await.ok()?;
    let url = if device_ids.len() == 1 {
        metadata
            .account_management_url_with_action(AccountManagementActionData::DeviceDelete(
                DeviceDeleteData::new(&device_ids[0]),
            ))
            .or_else(|| {
                metadata
                    .account_management_url_with_action(AccountManagementActionData::DevicesList)
            })
    } else {
        metadata.account_management_url_with_action(AccountManagementActionData::DevicesList)
    };
    url.map(|u| u.to_string())
}

fn parse_device_delete_auth(auth_json: &str) -> Result<Option<uiaa::AuthData>> {
    if auth_json.is_empty() {
        return Ok(None);
    }

    let parsed: serde_json::Value =
        serde_json::from_str(auth_json).map_err(|e| anyhow!("Invalid auth JSON: {e}"))?;

    let auth = uiaa::AuthData::FallbackAcknowledgement(uiaa::FallbackAcknowledgement::new(
        parsed["session"].as_str().unwrap_or_default().to_owned(),
    ));

    let user_field = parsed["user"]
        .as_str()
        .or_else(|| parsed["identifier"]["user"].as_str());
    if let (Some(user), Some(password)) = (user_field, parsed["password"].as_str()) {
        let session = parsed["session"].as_str().map(|s| s.to_owned());
        let mut pw = uiaa::Password::new(
            uiaa::UserIdentifier::Matrix(uiaa::MatrixUserIdentifier::new(user.to_owned())),
            password.to_owned(),
        );
        pw.session = session;
        Ok(Some(uiaa::AuthData::Password(pw)))
    } else {
        Ok(Some(auth))
    }
}

fn parse_reset_identity_auth(auth_json: &str) -> Result<Option<uiaa::AuthData>> {
    let parsed: serde_json::Value =
        serde_json::from_str(auth_json).map_err(|e| anyhow!("Invalid auth JSON: {e}"))?;
    let user = parsed["user"].as_str();
    let password = parsed["password"].as_str();
    if let (Some(user), Some(password)) = (user, password) {
        let session = parsed["session"].as_str().map(|s| s.to_owned());
        let mut pw = uiaa::Password::new(
            uiaa::UserIdentifier::Matrix(uiaa::MatrixUserIdentifier::new(user.to_owned())),
            password.to_owned(),
        );
        pw.session = session;
        Ok(Some(uiaa::AuthData::Password(pw)))
    } else {
        Ok(None)
    }
}

/// Log this device's E2EE key-acquisition state at a startup/auth boundary:
/// whether it holds the cross-signing private keys, the recovery state, and the
/// backup state. Cheap (local state only — no network) and invaluable for
/// diagnosing "messages won't decrypt" reports from logs alone.
pub(crate) async fn log_e2ee_diagnostics(client: &Client, context: &str) {
    let enc = client.encryption();
    let verification_state = enc.verification_state().get();
    let cross_signing = enc.cross_signing_status().await;
    let recovery_state = enc.recovery().state();
    let backups = enc.backups();
    let backup_enabled = backups.are_enabled().await;
    let backup_state = backups.state();
    tracing::info!(
        "[e2ee {context}] verification_state={verification_state:?} \
         cross_signing={cross_signing:?} recovery={recovery_state:?} \
         backup_enabled={backup_enabled} backup_state={backup_state:?}"
    );
}

/// Parse and normalize recovery key tokens by dropping whitespace.
fn normalize_recovery_key(key: &str) -> String {
    key.chars().filter(|c| !c.is_whitespace()).collect()
}

fn parse_user_agent(ua: &str) -> UserAgentParts {
    let ua = ua.trim();
    if ua.is_empty() {
        return (None, None, None, None, None);
    }

    if ua.starts_with("Mozilla/") {
        let os = extract_parenthesized(ua)
            .and_then(|paren| paren.split(';').next().map(|s| s.trim().to_string()));
        let browser = detect_browser(ua);
        return (Some("Web Browser".to_string()), None, None, os, browser);
    }

    if let Some(slash_pos) = ua.find('/') {
        let app_name_raw = &ua[..slash_pos];
        let rest = &ua[slash_pos + 1..];
        let version_end = rest.find([' ', '(']).unwrap_or(rest.len());
        let version = rest[..version_end].trim();

        let app_name = app_name_raw.trim().to_string();
        let app_version = if version.is_empty() {
            None
        } else {
            Some(version.to_string())
        };

        let paren = extract_parenthesized(ua);
        let (device_model, os) = if let Some(paren_content) = paren {
            let parts: Vec<&str> = paren_content.split(';').map(|s| s.trim()).collect();
            if parts.len() >= 2 {
                let first = parts[0];
                let second = parts[1];
                if is_os_like(first) {
                    (None, Some(first.to_string()))
                } else {
                    (Some(first.to_string()), Some(second.to_string()))
                }
            } else if parts.len() == 1 && !parts[0].is_empty() {
                (None, Some(parts[0].to_string()))
            } else {
                (None, None)
            }
        } else {
            (None, None)
        };

        return (Some(app_name), app_version, device_model, os, None);
    }

    (None, None, None, None, None)
}

/// Extract the content inside the first pair of parentheses.
fn extract_parenthesized(s: &str) -> Option<String> {
    let start = s.find('(')? + 1;
    let end = s[start..].find(')')? + start;
    let content = s[start..end].trim();
    if content.is_empty() {
        None
    } else {
        Some(content.to_string())
    }
}

/// Return true if the token looks like an OS name rather than a device model.
fn is_os_like(token: &str) -> bool {
    let lower = token.to_lowercase();
    lower.contains("macos")
        || lower.contains("mac os")
        || lower.contains("windows")
        || lower.contains("linux")
        || lower.contains("android")
        || lower.contains("ios")
        || lower.contains("ubuntu")
        || lower.contains("fedora")
        || lower.contains("freebsd")
        || lower.contains("chrome os")
}

/// Detect the browser name + major version from a Mozilla-style UA string.
fn detect_browser(ua: &str) -> Option<String> {
    let browsers = [
        ("Edg/", "Edge"),
        ("OPR/", "Opera"),
        ("Vivaldi/", "Vivaldi"),
        ("Brave", "Brave"),
        ("Firefox/", "Firefox"),
        ("Chrome/", "Chrome"),
        ("Safari/", "Safari"),
    ];
    for (token, name) in &browsers {
        if let Some(pos) = ua.find(token) {
            let from = pos;
            let end = ua[from..].find(' ').map(|i| from + i).unwrap_or(ua.len());
            let fragment = &ua[from..end];
            if fragment.contains('/') {
                return Some(format!(
                    "{}/{}",
                    name,
                    fragment.split('/').nth(1).unwrap_or("")
                ));
            }
            return Some(name.to_string());
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_ua_empty_returns_none() {
        let (app, ver, model, os, browser) = parse_user_agent("");
        assert!(app.is_none());
        assert!(ver.is_none());
        assert!(model.is_none());
        assert!(os.is_none());
        assert!(browser.is_none());
    }

    #[test]
    fn parse_ua_element_desktop() {
        let (app, ver, model, os, browser) =
            parse_user_agent("Element/1.11.80 (macOS; Electron 33.2.1)");
        assert_eq!(app.as_deref(), Some("Element"));
        assert_eq!(ver.as_deref(), Some("1.11.80"));
        assert!(model.is_none());
        assert_eq!(os.as_deref(), Some("macOS"));
        assert!(browser.is_none());
    }

    #[test]
    fn parse_ua_element_android() {
        let (app, ver, model, os, _browser) =
            parse_user_agent("Element Android/1.6.24 (Google Pixel 8; Android 14)");
        assert_eq!(app.as_deref(), Some("Element Android"));
        assert_eq!(ver.as_deref(), Some("1.6.24"));
        assert_eq!(model.as_deref(), Some("Google Pixel 8"));
        assert_eq!(os.as_deref(), Some("Android 14"));
    }

    #[test]
    fn parse_ua_chrome_browser() {
        let ua = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
        let (app, _ver, _model, os, browser) = parse_user_agent(ua);
        assert_eq!(app.as_deref(), Some("Web Browser"));
        assert_eq!(os.as_deref(), Some("Macintosh"));
        assert_eq!(browser.as_deref(), Some("Chrome/131.0.0.0"));
    }

    #[test]
    fn parse_ua_firefox_browser() {
        let ua = "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:120.0) Gecko/20100101 Firefox/120.0";
        let (app, _ver, _model, os, browser) = parse_user_agent(ua);
        assert_eq!(app.as_deref(), Some("Web Browser"));
        assert_eq!(os.as_deref(), Some("Windows NT 10.0"));
        assert_eq!(browser.as_deref(), Some("Firefox/120.0"));
    }

    #[test]
    fn parse_ua_edge_browser() {
        let ua = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36 Edg/131.0.0.0";
        let (_app, _ver, _model, _os, browser) = parse_user_agent(ua);
        assert_eq!(browser.as_deref(), Some("Edge/131.0.0.0"));
    }

    #[test]
    fn parse_ua_element_ios() {
        let (app, ver, model, os, _browser) =
            parse_user_agent("Element/2.3.1 (iPhone 15 Pro; iOS 17.2)");
        assert_eq!(app.as_deref(), Some("Element"));
        assert_eq!(ver.as_deref(), Some("2.3.1"));
        assert_eq!(model.as_deref(), Some("iPhone 15 Pro"));
        assert_eq!(os.as_deref(), Some("iOS 17.2"));
    }

    #[test]
    fn parse_ua_unknown_garbage() {
        let (app, ver, model, os, browser) = parse_user_agent("some random string");
        assert!(app.is_none());
        assert!(ver.is_none());
        assert!(model.is_none());
        assert!(os.is_none());
        assert!(browser.is_none());
    }

    #[test]
    fn normalize_recovery_key_removes_whitespace() {
        assert_eq!(normalize_recovery_key("  abC   12 "), "abC12");
        assert_eq!(normalize_recovery_key("a b\tc\n1 2"), "abc12");
    }

    #[test]
    fn recovery_setup_error_codes_separate_the_actionable_case() {
        let existing: RecoverySetupError = RecoveryError::BackupExistsOnServer.into();
        assert_eq!(existing.code(), RECOVERY_SETUP_ERROR_BACKUP_EXISTS);

        let other = RecoverySetupError::Other("network down".to_owned());
        assert_eq!(other.code(), RECOVERY_SETUP_ERROR_OTHER);
        assert_eq!(other.message(), "network down");
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Mutex, OnceLock};
use std::time::Duration;

use anyhow::{anyhow, Result};
use serde::{Deserialize, Serialize};
use tracing::{error, warn};
use zeroize::{Zeroize, Zeroizing};

use crate::secret_vault::KdfParams;

const SERVICE_NAME: &str = "dev.telematrix.TeleMatrix";
// Addressed instead of SERVICE_NAME by builds whose code identity is not stable across
// rebuilds — see `service_name()`.
#[cfg(target_os = "macos")]
const SERVICE_NAME_UNSTABLE: &str = "dev.telematrix.TeleMatrix.adhoc";
const BUNDLE_KEY: &str = "app_secrets";
const BUNDLE_VERSION: u32 = 1;
// Declaring the keychain unavailable is expensive — the UI's only remedy is a
// master-password vault — so a probe must fail repeatedly before we believe it.
const KEYCHAIN_PROBE_ATTEMPTS: u32 = 3;
const KEYCHAIN_PROBE_RETRY_DELAY: Duration = Duration::from_millis(120);
const LEGACY_DIRECT_KEYS: &[&str] = &[
    "access_token",
    "sdk_store_passphrase",
    "search_index_key",
    "app_cache_passphrase",
    "preview_cache_passphrase",
    "media_cache_passphrase",
];

#[derive(Clone, Default, Deserialize, Serialize)]
struct SecretBundle {
    version: u32,
    #[serde(default)]
    secrets: HashMap<String, String>,
}

impl SecretBundle {
    fn new() -> Self {
        Self {
            version: BUNDLE_VERSION,
            secrets: HashMap::new(),
        }
    }
}

impl Drop for SecretBundle {
    fn drop(&mut self) {
        // Keys are non-secret identifiers; scrub the values so dropped clones
        // don't leave tokens/passphrases in freed heap.
        for value in self.secrets.values_mut() {
            value.zeroize();
        }
    }
}

static BUNDLE_CACHE: OnceLock<Mutex<Option<SecretBundle>>> = OnceLock::new();

fn cache() -> &'static Mutex<Option<SecretBundle>> {
    BUNDLE_CACHE.get_or_init(|| Mutex::new(None))
}

// Serializes every load→mutate→persist sequence on the bundle. The cache and
// backend mutexes protect their own state, but without this outer lock two
// concurrent writers could each load the same bundle and silently drop the
// other's insert — or interleave create/truncate/rename on the vault temp file.
// Lock order: mutation_guard BEFORE cache()/backend(); nothing takes them in the
// reverse order.
static MUTATION_LOCK: Mutex<()> = Mutex::new(());

fn mutation_guard() -> std::sync::MutexGuard<'static, ()> {
    // The guarded sections hold no invariants beyond serialization, so a
    // poisoned lock is safe to reuse.
    MUTATION_LOCK.lock().unwrap_or_else(|p| p.into_inner())
}

// ----- Secret backend: OS keychain vs. master-password file vault -----

const VAULT_FILE: &str = "secret_vault.bin";

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum SecretBackend {
    OsKeychain,
    FileVault,
}

impl SecretBackend {
    pub fn from_i32(value: i32) -> Self {
        match value {
            1 => SecretBackend::FileVault,
            _ => SecretBackend::OsKeychain,
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum SecretStoreState {
    KeychainReady,
    KeychainUnavailable,
    VaultLocked,
    VaultUnlocked,
    VaultAbsent,
}

impl SecretStoreState {
    pub fn as_i32(self) -> i32 {
        match self {
            SecretStoreState::KeychainReady => 0,
            SecretStoreState::KeychainUnavailable => 1,
            SecretStoreState::VaultLocked => 2,
            SecretStoreState::VaultUnlocked => 3,
            SecretStoreState::VaultAbsent => 4,
        }
    }
}

struct VaultState {
    key: Zeroizing<[u8; 32]>,
    salt: [u8; 16],
    params: KdfParams,
}

struct BackendState {
    backend: SecretBackend,
    data_dir: PathBuf,
    vault: Option<VaultState>,
}

impl Default for BackendState {
    fn default() -> Self {
        Self {
            backend: SecretBackend::OsKeychain,
            data_dir: PathBuf::new(),
            vault: None,
        }
    }
}

static BACKEND: OnceLock<Mutex<BackendState>> = OnceLock::new();

fn backend() -> &'static Mutex<BackendState> {
    BACKEND.get_or_init(|| Mutex::new(BackendState::default()))
}

/// A lock-free copy of the backend state for I/O outside the mutex. The vault key
/// is copied into its own `Zeroizing`, scrubbed when the snapshot drops.
struct BackendSnapshot {
    backend: SecretBackend,
    data_dir: PathBuf,
    vault: Option<(Zeroizing<[u8; 32]>, [u8; 16], KdfParams)>,
}

fn snapshot() -> Result<BackendSnapshot> {
    let st = backend()
        .lock()
        .map_err(|_| anyhow!("secret backend state poisoned"))?;
    let vault = st
        .vault
        .as_ref()
        .map(|v| (Zeroizing::new(*v.key), v.salt, v.params));
    Ok(BackendSnapshot {
        backend: st.backend,
        data_dir: st.data_dir.clone(),
        vault,
    })
}

/// Delete a file if present; a missing file is success. Logs (but doesn't fail on)
/// other errors — used for best-effort cleanup of the non-active backend's storage.
fn best_effort_remove(path: &Path) {
    match std::fs::remove_file(path) {
        Ok(()) => {}
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
        Err(e) => warn!("[SECRET_STORE] failed to remove {}: {e}", path.display()),
    }
}

// Backend-dispatching bundle I/O. `load_bundle`/`store_bundle` (the cache layer)
// call these; every public secret accessor rides on top unchanged.
fn load_bundle_from_store() -> Result<SecretBundle> {
    let snap = snapshot()?;
    match snap.backend {
        SecretBackend::OsKeychain => load_bundle_from_keychain(),
        SecretBackend::FileVault => {
            let (key, _, _) = snap
                .vault
                .as_ref()
                .ok_or_else(|| anyhow!("secret vault is locked"))?;
            let path = snap.data_dir.join(VAULT_FILE);
            if !path.exists() {
                // Key set but nothing persisted yet (fresh session pre-first-write).
                return Ok(SecretBundle::new());
            }
            let bytes =
                std::fs::read(&path).map_err(|e| anyhow!("failed to read secret vault: {e}"))?;
            let json = crate::secret_vault::open_with_key(&bytes, key)?;
            let mut bundle: SecretBundle = serde_json::from_slice(json.as_slice())
                .map_err(|e| anyhow!("secret vault decode error: {e}"))?;
            if bundle.version == 0 {
                bundle.version = BUNDLE_VERSION;
            }
            Ok(bundle)
        }
    }
}

fn save_bundle_to_store(bundle: &SecretBundle) -> Result<()> {
    let snap = snapshot()?;
    match snap.backend {
        SecretBackend::OsKeychain => save_bundle_to_keychain(bundle),
        SecretBackend::FileVault => {
            let (key, salt, params) = snap
                .vault
                .as_ref()
                .ok_or_else(|| anyhow!("secret vault is locked"))?;
            let json = Zeroizing::new(
                serde_json::to_vec(bundle)
                    .map_err(|e| anyhow!("secret vault encode error: {e}"))?,
            );
            let bytes = crate::secret_vault::seal(json.as_slice(), key, salt, params)?;
            let path = snap.data_dir.join(VAULT_FILE);
            crate::secret_vault::write_atomic(&path, &bytes)?;
            Ok(())
        }
    }
}

// ----- Linux: which Secret Service collection keyring should use -----
//
// KWallet serves no `default` collection alias: ReadAlias("default") answers "/"
// even with a wallet present. That breaks keyring specifically for the *default*
// target. Its read path (map_matching_items) starts with a service-wide
// search_items, which is fine — but when that finds nothing AND the target is
// "default", it falls back to map_matching_legacy_items, which resolves
// get_default_collection(). On KWallet that alias lookup fails, the
// secret-service crate reports NoResult and keyring maps it to NoStorageAccess.
// So a first read on KDE — the empty-keychain case every fresh install hits —
// raises a hard error instead of NoEntry, and `keychain_reachable` reads that as
// "this device has no keychain" and offers the master-password vault.
//
// Naming a non-"default" target skips that legacy fallback entirely: an empty
// search then returns NoEntry (reachable-but-empty, which is the truth), and
// writes go through get_collection/create_collection, which match by label via
// get_all_collections() — something KWallet does implement. The label is fixed
// rather than "whichever collection came back first" so the target can never
// shift between runs as other wallets appear; writing to one wallet and reading
// from another would look exactly like a lost session.
#[cfg(target_os = "linux")]
const COLLECTION_LABEL: &str = "TeleMatrix";

#[cfg(target_os = "linux")]
enum Routing {
    /// The `default` alias resolves (GNOME Keyring): keep keyring's own path so
    /// secrets already stored there stay reachable.
    DefaultAlias,
    /// No `default` alias (KWallet): address a collection by this label.
    Label(&'static str),
}

/// Cached routing. `None` = not probed yet; a failed probe is deliberately NOT
/// cached, so a provider that starts after us is still picked up later.
#[cfg(target_os = "linux")]
static COLLECTION_ROUTING: Mutex<Option<Option<&'static str>>> = Mutex::new(None);

#[cfg(target_os = "linux")]
fn probe_routing() -> Option<Routing> {
    use zbus::blocking::{Connection, Proxy};
    let conn = Connection::session()
        .inspect_err(|e| warn!("[SECRET_STORE] collection probe: no session bus: {e}"))
        .ok()?;
    let service = Proxy::new(
        &conn,
        "org.freedesktop.secrets",
        "/org/freedesktop/secrets",
        "org.freedesktop.Secret.Service",
    )
    .inspect_err(|e| warn!("[SECRET_STORE] collection probe: no Secret Service proxy: {e}"))
    .ok()?;
    let alias: zbus::zvariant::OwnedObjectPath = service
        .call("ReadAlias", &"default")
        .inspect_err(|e| warn!("[SECRET_STORE] collection probe: ReadAlias failed: {e}"))
        .ok()?;
    if alias.as_str() == "/" {
        tracing::info!(
            "[SECRET_STORE] no `default` collection alias (KWallet); \
             targeting the {COLLECTION_LABEL} collection by label"
        );
        Some(Routing::Label(COLLECTION_LABEL))
    } else {
        tracing::info!("[SECRET_STORE] `default` collection alias resolves to {alias}");
        Some(Routing::DefaultAlias)
    }
}

/// Resolved routing: `Some(Some(label))` = address that collection by label,
/// `Some(None)` = use keyring's default path, `None` = the service did not answer.
#[cfg(target_os = "linux")]
fn routing() -> Option<Option<&'static str>> {
    let mut guard = COLLECTION_ROUTING
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    if let Some(cached) = *guard {
        return Some(cached);
    }
    // A failed probe stays uncached, so a provider that starts after us is still
    // picked up; only a definite answer is remembered.
    let resolved = match probe_routing()? {
        Routing::Label(label) => Some(label),
        Routing::DefaultAlias => None,
    };
    *guard = Some(resolved);
    Some(resolved)
}

/// The collection label to address, or `None` to use keyring's default path.
#[cfg(target_os = "linux")]
fn collection_target() -> Option<&'static str> {
    routing().flatten()
}

/// Whether a collection can actually be addressed — the question the bus-name
/// probe cannot answer on its own.
#[cfg(target_os = "linux")]
fn collection_usable() -> bool {
    routing().is_some()
}

// ----- macOS: which service name this build is allowed to address -----

/// Whether this process's signature is certificate-backed, i.e. its designated
/// requirement is stable across rebuilds.
///
/// macOS pins a keychain item's ACL to the designated requirement of the binary that
/// *created* it. A Developer ID requirement (`identifier "…" and … certificate
/// leaf[subject.OU] = "…"`) is stable, so every later build — rebuilt, re-signed,
/// moved, or shipped as an update — keeps access. An ad-hoc requirement is
/// `cdhash H"…"`, which changes on every single rebuild: the next build reads the
/// previous build's item as `errSecAuthFailed`, and worse, an ad-hoc build that
/// creates the item locks the *released* app out of it. Such builds get their own
/// service name so they can never read, overwrite or wipe the real one.
#[cfg(target_os = "macos")]
fn has_stable_code_identity() -> bool {
    use core_foundation::base::TCFType;
    use core_foundation::string::CFString;
    use core_foundation_sys::base::CFRelease;
    use security_framework_sys::code_signing::{
        SecCodeCheckValidity, SecCodeCopySelf, SecCodeRef, SecRequirementCreateWithString,
        SecRequirementRef,
    };

    // "anchor apple generic" holds for anything signed by an Apple-issued certificate
    // (Developer ID, App Store) and fails for ad-hoc and unsigned code.
    unsafe {
        let mut code: SecCodeRef = std::ptr::null_mut();
        if SecCodeCopySelf(0, &mut code) != 0 || code.is_null() {
            return false;
        }
        let text = CFString::new("anchor apple generic");
        let mut requirement: SecRequirementRef = std::ptr::null_mut();
        let stable =
            SecRequirementCreateWithString(text.as_concrete_TypeRef(), 0, &mut requirement) == 0
                && !requirement.is_null()
                && SecCodeCheckValidity(code, 0, requirement) == 0;
        if !requirement.is_null() {
            CFRelease(requirement.cast());
        }
        CFRelease(code.cast());
        stable
    }
}

fn service_name() -> &'static str {
    #[cfg(target_os = "macos")]
    {
        static STABLE: OnceLock<bool> = OnceLock::new();
        if !*STABLE.get_or_init(|| {
            let stable = has_stable_code_identity();
            if !stable {
                warn!(
                    "[SECRET_STORE] this build is not signed with a stable identity; \
                     addressing {SERVICE_NAME_UNSTABLE} so it cannot disturb {SERVICE_NAME}"
                );
            }
            stable
        }) {
            return SERVICE_NAME_UNSTABLE;
        }
    }
    SERVICE_NAME
}

fn entry(key: &str) -> Result<keyring::Entry> {
    let service = service_name();
    #[cfg(target_os = "linux")]
    if let Some(target) = collection_target() {
        return keyring::Entry::new_with_target(target, service, key)
            .map_err(|e| anyhow!("Keychain entry error: {e}"));
    }
    keyring::Entry::new(service, key).map_err(|e| anyhow!("Keychain entry error: {e}"))
}

fn direct_store(key: &str, value: &str) -> Result<()> {
    entry(key)?
        .set_password(value)
        .map_err(|e| anyhow!("Keychain store error: {e}"))
}

fn direct_load(key: &str) -> Result<Option<String>> {
    match entry(key)?.get_password() {
        Ok(value) => Ok(Some(value)),
        Err(keyring::Error::NoEntry) => Ok(None),
        Err(e) => Err(anyhow!("Keychain load error: {e}")),
    }
}

fn direct_delete(key: &str) -> Result<()> {
    match entry(key)?.delete_credential() {
        Ok(()) => Ok(()),
        Err(keyring::Error::NoEntry) => Ok(()),
        Err(e) => Err(anyhow!("Keychain delete error: {e}")),
    }
}

fn load_bundle_from_keychain() -> Result<SecretBundle> {
    // Retried like the reachability probe, and for the same reason: a refused read is
    // not an empty keychain. macOS pins the item to the designated requirement of the
    // binary that wrote it (see `has_stable_code_identity`), so an item left behind by
    // a differently-signed build is refused rather than reported missing. The caller
    // cannot tell those apart from the outside, and reads "no secrets" as "dead
    // session" — so a single bad read must not be allowed to stand.
    let mut stored = None;
    let mut failure = None;
    for attempt in 1..=KEYCHAIN_PROBE_ATTEMPTS {
        match direct_load(BUNDLE_KEY) {
            Ok(value) => {
                stored = value;
                failure = None;
                break;
            }
            Err(e) => {
                warn!(
                    "[KEYCHAIN] bundle read failed \
                     (attempt {attempt}/{KEYCHAIN_PROBE_ATTEMPTS}): {e}"
                );
                failure = Some(e);
                if attempt < KEYCHAIN_PROBE_ATTEMPTS {
                    std::thread::sleep(KEYCHAIN_PROBE_RETRY_DELAY);
                }
            }
        }
    }
    if let Some(e) = failure {
        error!("[KEYCHAIN] bundle unreadable after {KEYCHAIN_PROBE_ATTEMPTS} attempts");
        return Err(e);
    }
    if stored.is_none() {
        // Expected before the first sign-in. Afterwards it means the keychain is
        // answering "no such item" for a bundle that was written — the caller must
        // not read that as "the session is gone" (see HasSecureSessionSecrets).
        warn!("[KEYCHAIN] no secret bundle stored");
    }

    let mut bundle = match stored {
        Some(data) => serde_json::from_str::<SecretBundle>(&data)
            .map_err(|e| anyhow!("Keychain bundle decode error: {e}"))?,
        None => SecretBundle::new(),
    };

    if bundle.version == 0 {
        bundle.version = BUNDLE_VERSION;
    }
    Ok(bundle)
}

fn save_bundle_to_keychain(bundle: &SecretBundle) -> Result<()> {
    let data =
        serde_json::to_string(bundle).map_err(|e| anyhow!("Keychain bundle encode error: {e}"))?;
    direct_store(BUNDLE_KEY, &data)
}

fn load_bundle() -> Result<SecretBundle> {
    let mut guard = cache()
        .lock()
        .map_err(|_| anyhow!("Keychain bundle cache poisoned"))?;
    if let Some(bundle) = guard.as_ref() {
        return Ok(bundle.clone());
    }
    let bundle = load_bundle_from_store()?;
    *guard = Some(bundle.clone());
    Ok(bundle)
}

fn store_bundle(bundle: SecretBundle) -> Result<()> {
    save_bundle_to_store(&bundle)?;
    let mut guard = cache()
        .lock()
        .map_err(|_| anyhow!("Keychain bundle cache poisoned"))?;
    *guard = Some(bundle);
    Ok(())
}

/// Store a secret in the system keychain.
pub fn store_secret(key: &str, value: &str) -> Result<()> {
    let _write = mutation_guard();
    let mut bundle = load_bundle()?;
    bundle.secrets.insert(key.to_string(), value.to_string());
    store_bundle(bundle)?;
    bump_secret_epoch();
    Ok(())
}

/// Store several secrets with a single keychain write.
pub fn store_secrets(entries: &[(&str, &str)]) -> Result<()> {
    let _write = mutation_guard();
    let mut bundle = load_bundle()?;
    for (key, value) in entries {
        bundle
            .secrets
            .insert((*key).to_string(), (*value).to_string());
    }
    store_bundle(bundle)?;
    bump_secret_epoch();
    Ok(())
}

/// Load a secret from the system keychain.
pub fn load_secret(key: &str) -> Result<Option<String>> {
    let bundle = load_bundle()?;
    if let Some(value) = bundle.secrets.get(key) {
        return Ok(Some(value.clone()));
    }
    Ok(None)
}

/// Drop the cached bundle so the next read goes back to the keychain.
///
/// Without this a retry is pointless: a keychain that answered "no such item" —
/// because the OS was withholding the item rather than because it is absent —
/// caches an empty bundle, and every later read is served from that.
pub fn forget_cached_bundle() {
    if let Ok(mut guard) = cache().lock() {
        *guard = None;
    }
}

/// Delete a secret from the system keychain.
pub fn delete_secret(key: &str) -> Result<()> {
    let _write = mutation_guard();
    delete_keys_locked(&[key])
}

/// Delete several secrets in a single bundle load/store, under one mutation guard.
/// Deleting an absent key is a no-op. Used by per-account logout to remove exactly
/// the departing account's keys while every other account's secrets survive (the
/// wholesale wipe is only for the last-account path). See code-review-2026-07-19 MA-1.
pub fn delete_secrets(keys: &[String]) -> Result<()> {
    let _write = mutation_guard();
    let refs: Vec<&str> = keys.iter().map(String::as_str).collect();
    delete_keys_locked(&refs)
}

/// Remove the given keys from the bundle and persist once if anything changed.
/// Caller holds `mutation_guard`.
fn delete_keys_locked(keys: &[&str]) -> Result<()> {
    let mut bundle = load_bundle()?;
    let mut changed = false;
    for key in keys {
        if bundle.secrets.remove(*key).is_some() {
            changed = true;
        }
    }
    if changed {
        store_bundle(bundle)?;
    }
    Ok(())
}

/// Configure the secret backend for this process (call once at startup, before
/// any secret access). `requested` is honored on every platform — the vault is a
/// user-selectable alternative to the OS keychain, not a Linux-only fallback.
pub fn init(data_dir: PathBuf, requested: SecretBackend) {
    if let Ok(mut st) = backend().lock() {
        st.data_dir = data_dir;
        st.backend = requested;
    }
}

/// The current secret-store state, so the startup gate can distinguish "proceed",
/// "prompt to unlock", and "wipe" — never conflating an unreachable backend with
/// genuinely-absent secrets.
pub fn state() -> SecretStoreState {
    let snap = match snapshot() {
        Ok(s) => s,
        Err(_) => return SecretStoreState::KeychainUnavailable,
    };
    match snap.backend {
        SecretBackend::OsKeychain => {
            if keychain_reachable() {
                SecretStoreState::KeychainReady
            } else {
                SecretStoreState::KeychainUnavailable
            }
        }
        SecretBackend::FileVault => {
            // A retained key means unlocked even before the first write creates the
            // file (vault chosen at intro, secrets not yet persisted). Only with no
            // key does the file's presence distinguish "locked" from "never set up".
            if snap.vault.is_some() {
                SecretStoreState::VaultUnlocked
            } else if snap.data_dir.join(VAULT_FILE).exists() {
                SecretStoreState::VaultLocked
            } else {
                SecretStoreState::VaultAbsent
            }
        }
    }
}

fn keychain_reachable() -> bool {
    // Serialize against an in-flight mutation: logout's `clear_all_secrets` deletes
    // this very entry, and an unsynchronized read racing that delete could fail
    // spuriously — which the caller reads as "this device has no keychain" and
    // offers to move it onto a master-password vault. Never probe mid-wipe.
    let _guard = mutation_guard();

    // Reachable => a bundle read returns a value or a clean NoEntry; a dead or
    // locked backend surfaces NoStorageAccess/PlatformFailure. Only the latter is
    // "unavailable", so a genuinely-empty-but-reachable keychain still wipes.
    // Retried: one bad read (a momentarily locked keychain, a denied access
    // prompt after the app was re-signed) must not condemn the whole backend.
    for attempt in 1..=KEYCHAIN_PROBE_ATTEMPTS {
        let probe = entry(BUNDLE_KEY).and_then(|entry| match entry.get_password() {
            Ok(_) | Err(keyring::Error::NoEntry) => Ok(()),
            Err(e) => Err(anyhow!("{e}")),
        });
        match probe {
            Ok(()) => return true,
            Err(e) => {
                warn!(
                    "[SECRET_STORE] keychain probe failed \
                     (attempt {attempt}/{KEYCHAIN_PROBE_ATTEMPTS}): {e}"
                );
                if attempt < KEYCHAIN_PROBE_ATTEMPTS {
                    std::thread::sleep(KEYCHAIN_PROBE_RETRY_DELAY);
                }
            }
        }
    }
    error!("[SECRET_STORE] keychain unreachable after {KEYCHAIN_PROBE_ATTEMPTS} attempts");
    false
}

/// Whether the OS Secret Service is reachable (a provider owns
/// `org.freedesktop.secrets`). Decides keychain-vs-vault at fresh login on Linux;
/// always true off Linux (a native keychain is always present).
pub fn service_available() -> bool {
    secret_service_status() == SECRET_SERVICE_OK
}

// Granular Secret Service reachability, for user-facing diagnostics.
const SECRET_SERVICE_OK: i32 = 0;
#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
const SECRET_SERVICE_NO_DBUS: i32 = 1; // no D-Bus session bus at all
#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
const SECRET_SERVICE_NO_PROVIDER: i32 = 2; // D-Bus up, but nothing serves secrets

/// Why the Secret Service is or isn't reachable: 0 = available, 1 = no D-Bus
/// session bus, 2 = D-Bus up but no provider. Always 0 off Linux.
pub fn service_status() -> i32 {
    secret_service_status()
}

#[cfg(target_os = "linux")]
fn secret_service_status() -> i32 {
    use zbus::blocking::{fdo::DBusProxy, Connection};
    const NAME: &str = "org.freedesktop.secrets";
    // Every branch logs: this verdict disables the keychain in the UI, and without
    // a trace there is no way to tell a genuinely providerless desktop from a
    // probe that failed for its own reasons.
    let conn = match Connection::session() {
        Ok(conn) => conn,
        Err(e) => {
            warn!("[SECRET_STORE] no D-Bus session bus: {e}");
            return SECRET_SERVICE_NO_DBUS;
        }
    };
    let proxy = match DBusProxy::new(&conn) {
        Ok(proxy) => proxy,
        Err(e) => {
            warn!("[SECRET_STORE] D-Bus proxy unavailable: {e}");
            return SECRET_SERVICE_NO_DBUS;
        }
    };
    let bus_name = match zbus::names::BusName::try_from(NAME) {
        Ok(name) => name,
        Err(e) => {
            warn!("[SECRET_STORE] invalid bus name {NAME}: {e}");
            return SECRET_SERVICE_NO_PROVIDER;
        }
    };
    match proxy.name_has_owner(bus_name) {
        Ok(true) => {
            // Owning the bus name is not the same as being usable: KWallet owns it
            // yet serves no `default` alias, which used to leave this probe saying
            // "keychain available" while every actual read failed at the startup
            // gate. Confirm a collection can be addressed before promising one.
            return if collection_usable() {
                tracing::info!("[SECRET_STORE] {NAME} is owned and usable");
                SECRET_SERVICE_OK
            } else {
                warn!("[SECRET_STORE] {NAME} is owned but no collection is addressable");
                SECRET_SERVICE_NO_PROVIDER
            };
        }
        Ok(false) => {}
        Err(e) => warn!("[SECRET_STORE] NameHasOwner({NAME}) failed: {e}"),
    }
    // Not currently running but D-Bus-activatable still counts as available.
    match proxy.list_activatable_names() {
        Ok(names) => {
            if names.iter().any(|n| n.as_str() == NAME) {
                tracing::info!("[SECRET_STORE] {NAME} is D-Bus-activatable; keychain available");
                return SECRET_SERVICE_OK;
            }
            warn!(
                "[SECRET_STORE] {NAME} is neither owned nor activatable among {} names \
                 (no GNOME Keyring / KWallet Secret Service provider on this session)",
                names.len()
            );
        }
        Err(e) => warn!("[SECRET_STORE] ListActivatableNames failed: {e}"),
    }
    SECRET_SERVICE_NO_PROVIDER
}

#[cfg(not(target_os = "linux"))]
fn secret_service_status() -> i32 {
    SECRET_SERVICE_OK
}

/// Why `unlock` failed, for user-facing wording across the FFI.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UnlockFailure {
    /// Wrong password — or ciphertext corruption, which AEAD can't tell apart.
    WrongPassword,
    /// The file is not a supported vault (magic/version/params/truncation).
    BadFormat,
    /// The vault file could not be read, or the backend is not the file vault.
    Unavailable,
    /// Decrypted fine but the bundle JSON didn't parse (on-disk corruption).
    Decode,
}

impl UnlockFailure {
    pub fn as_i32(self) -> i32 {
        match self {
            UnlockFailure::WrongPassword => 1,
            UnlockFailure::BadFormat => 2,
            UnlockFailure::Unavailable => 3,
            UnlockFailure::Decode => 4,
        }
    }
}

/// Unlock the file vault with the master password: decrypt it, retain the derived
/// key for later writes, and populate the in-memory bundle cache.
pub fn unlock(password: &str) -> std::result::Result<(), UnlockFailure> {
    let _write = mutation_guard();
    let (backend_kind, data_dir) = {
        let st = backend().lock().map_err(|_| UnlockFailure::Unavailable)?;
        (st.backend, st.data_dir.clone())
    };
    // Guard the sticky-backend invariant: a stale vault file must not be able to
    // flip a keychain-backed process onto the vault.
    if backend_kind != SecretBackend::FileVault {
        warn!("[SECRET_STORE] unlock called on a non-vault backend");
        return Err(UnlockFailure::Unavailable);
    }
    let path = data_dir.join(VAULT_FILE);
    let bytes = std::fs::read(&path).map_err(|e| {
        warn!("[SECRET_STORE] failed to read secret vault: {e}");
        UnlockFailure::Unavailable
    })?;
    let opened = crate::secret_vault::open(&bytes, password).map_err(|e| match e {
        crate::secret_vault::OpenFailure::BadFormat => UnlockFailure::BadFormat,
        crate::secret_vault::OpenFailure::WrongPassword => UnlockFailure::WrongPassword,
    })?;
    let bundle: SecretBundle =
        serde_json::from_slice(opened.plaintext.as_slice()).map_err(|e| {
            warn!("[SECRET_STORE] secret vault decode error: {e}");
            UnlockFailure::Decode
        })?;
    {
        let mut st = backend().lock().map_err(|_| UnlockFailure::Unavailable)?;
        st.backend = SecretBackend::FileVault;
        st.vault = Some(VaultState {
            key: opened.key,
            salt: opened.salt,
            params: opened.params,
        });
    }
    let mut guard = cache().lock().map_err(|_| UnlockFailure::Unavailable)?;
    *guard = Some(bundle);
    Ok(())
}

/// Set or replace the vault master password: derive and retain a fresh key and
/// switch to the file-vault backend. Nothing is written until there are actual
/// secrets to protect (a fresh login persists on its first secret write, so an
/// aborted login leaves no vault file); a non-empty cached bundle is re-sealed
/// immediately.
pub fn set_passphrase(password: &str) -> Result<()> {
    let _write = mutation_guard();
    let salt = crate::secret_vault::new_salt();
    let params = crate::secret_vault::DEFAULT_PARAMS;
    let key = crate::secret_vault::derive_key(password, &salt, &params)?;
    {
        let mut st = backend()
            .lock()
            .map_err(|_| anyhow!("secret backend state poisoned"))?;
        st.backend = SecretBackend::FileVault;
        st.vault = Some(VaultState { key, salt, params });
    }
    let cached = {
        let guard = cache()
            .lock()
            .map_err(|_| anyhow!("Keychain bundle cache poisoned"))?;
        guard.clone()
    };
    if let Some(bundle) = cached {
        // Persist only when there's something to protect: after the no-session
        // startup cleanup the cache holds Some(empty bundle), and sealing that
        // would leave an empty vault file behind when a login is aborted.
        if !bundle.secrets.is_empty() {
            save_bundle_to_store(&bundle)?;
        }
    }
    // The new vault key is secret state a belated teardown must not drop.
    bump_secret_epoch();
    Ok(())
}

/// Bumped by every write that *establishes* secrets (never by a delete), so a
/// teardown that started earlier can tell that a newer sign-in has since written
/// its own secrets and must not be wiped. See `clear_all_secrets_if_unchanged`.
static SECRET_EPOCH: AtomicU64 = AtomicU64::new(0);

/// The current secret epoch. Snapshot it before a teardown that will end in a
/// wipe; hand it back to `clear_all_secrets_if_unchanged`.
pub fn secret_epoch() -> u64 {
    SECRET_EPOCH.load(Ordering::SeqCst)
}

fn bump_secret_epoch() {
    SECRET_EPOCH.fetch_add(1, Ordering::SeqCst);
}

/// Delete all TeleMatrix secrets, but only if no secrets have been written since
/// `expected` was taken. Returns whether the wipe ran.
///
/// The C++ leftover-data cleanup re-enables the login form on a safety timer even
/// when this logout is still running, so a sign-in can complete underneath a
/// belated teardown. The comparison happens under `mutation_guard`, the same lock
/// every write takes, so the check and the delete are atomic with respect to
/// `store_secrets`: either the new session's secrets are already in (epoch moved →
/// we skip) or they land after the wipe (and survive it).
pub fn clear_all_secrets_if_unchanged(expected: u64) -> Result<bool> {
    let _write = mutation_guard();
    if secret_epoch() != expected {
        return Ok(false);
    }
    clear_all_secrets_locked()?;
    Ok(true)
}

/// Delete all TeleMatrix secrets (on logout).
pub fn clear_all_secrets() -> Result<()> {
    let _write = mutation_guard();
    clear_all_secrets_locked()
}

/// The wipe itself. Caller holds `mutation_guard`.
fn clear_all_secrets_locked() -> Result<()> {
    let (backend_kind, dir) = {
        let st = backend()
            .lock()
            .map_err(|_| anyhow!("secret backend state poisoned"))?;
        (st.backend, st.data_dir.clone())
    };
    let vault_path = dir.join(VAULT_FILE);
    // Both backends' storage is wiped: a prior backend switch may have left a stale
    // copy in the other store, and this is the logout backstop that reclaims it.
    // Only the *active* backend's failure propagates (it's the authoritative wipe).
    match backend_kind {
        SecretBackend::OsKeychain => {
            // All current secrets live inside the single BUNDLE_KEY blob, not as
            // their own Keychain items, so deleting that blob is what actually
            // wipes them. (LEGACY_DIRECT_KEYS are pre-bundle leftovers.)
            for key in LEGACY_DIRECT_KEYS {
                let _ = direct_delete(key);
            }
            // Propagate failure *before* touching the in-memory cache: if the
            // delete failed the secret is still persisted, so clearing the cache
            // here would make a failed wipe look successful (and a later
            // load_secret would re-read the secret the caller believed deleted).
            direct_delete(BUNDLE_KEY)?;
            best_effort_remove(&vault_path);
        }
        SecretBackend::FileVault => {
            match std::fs::remove_file(&vault_path) {
                Ok(()) => {}
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
                Err(e) => return Err(anyhow!("failed to delete secret vault: {e}")),
            }
            // Drop the retained key so no later write can re-persist the vault.
            {
                let mut st = backend()
                    .lock()
                    .map_err(|_| anyhow!("secret backend state poisoned"))?;
                st.vault = None;
            }
            for key in LEGACY_DIRECT_KEYS {
                let _ = direct_delete(key);
            }
            let _ = direct_delete(BUNDLE_KEY);
        }
    }

    let mut guard = cache()
        .lock()
        .map_err(|_| anyhow!("Keychain bundle cache poisoned"))?;
    *guard = Some(SecretBundle::new());
    Ok(())
}

/// Migrate all secrets to a different backend. The current backend must be
/// readable/unlocked (caller guarantees). Writes the bundle to the target store
/// first; only then removes the old storage. A new-store write failure aborts with
/// nothing changed; an old-store delete failure is logged, not fatal (logout's
/// dual-backend wipe is the backstop). `passphrase` is required for the vault
/// target, ignored for the keychain target.
pub fn switch_backend(target: SecretBackend, passphrase: Option<&str>) -> Result<()> {
    let _write = mutation_guard();
    let (current, dir, has_key) = {
        let st = backend()
            .lock()
            .map_err(|_| anyhow!("secret backend state poisoned"))?;
        (st.backend, st.data_dir.clone(), st.vault.is_some())
    };
    if current == target {
        return Ok(());
    }
    // Source bundle. A FileVault source with no retained key is either absent
    // (nothing to migrate) or locked (can't read — refuse rather than lose data).
    let bundle = if current == SecretBackend::FileVault && !has_key {
        if dir.join(VAULT_FILE).exists() {
            return Err(anyhow!(
                "cannot switch from a locked vault; unlock it first"
            ));
        }
        SecretBundle::new()
    } else {
        load_bundle()?
    };
    match target {
        SecretBackend::FileVault => {
            let password =
                passphrase.ok_or_else(|| anyhow!("vault backend requires a master password"))?;
            let salt = crate::secret_vault::new_salt();
            let params = crate::secret_vault::DEFAULT_PARAMS;
            let key = crate::secret_vault::derive_key(password, &salt, &params)?;
            // Persist the new vault first (only when there's something to write, to
            // mirror set_passphrase — an empty pre-login bundle just flips backend).
            if !bundle.secrets.is_empty() {
                let json = Zeroizing::new(
                    serde_json::to_vec(&bundle)
                        .map_err(|e| anyhow!("secret vault encode error: {e}"))?,
                );
                let bytes = crate::secret_vault::seal(json.as_slice(), &key, &salt, &params)?;
                crate::secret_vault::write_atomic(&dir.join(VAULT_FILE), &bytes)?;
            }
            {
                let mut st = backend()
                    .lock()
                    .map_err(|_| anyhow!("secret backend state poisoned"))?;
                st.backend = SecretBackend::FileVault;
                st.vault = Some(VaultState { key, salt, params });
            }
            // Old keychain bundle is now stale.
            for k in LEGACY_DIRECT_KEYS {
                let _ = direct_delete(k);
            }
            if let Err(e) = direct_delete(BUNDLE_KEY) {
                warn!("[SECRET_STORE] failed to remove old keychain bundle after switch: {e}");
            }
        }
        SecretBackend::OsKeychain => {
            // Persist to the keychain first; a failure aborts the switch (no data loss).
            if !bundle.secrets.is_empty() {
                save_bundle_to_keychain(&bundle)?;
            }
            {
                let mut st = backend()
                    .lock()
                    .map_err(|_| anyhow!("secret backend state poisoned"))?;
                st.backend = SecretBackend::OsKeychain;
                st.vault = None;
            }
            // The old vault file is now stale.
            best_effort_remove(&dir.join(VAULT_FILE));
        }
    }
    // Keep the cache consistent with the migrated bundle (the empty-source branch
    // never populated it via load_bundle).
    if let Ok(mut guard) = cache().lock() {
        *guard = Some(bundle);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};

    // The backend/cache are process globals, so the vault tests below serialize on
    // this lock and reset the state between runs. They exercise FileVault paths
    // only — never the real OS keychain (kept empty-bundle so no keychain write).
    static TEST_LOCK: Mutex<()> = Mutex::new(());

    fn test_dir(tag: &str) -> PathBuf {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0);
        let dir = std::env::temp_dir().join(format!(
            "telematrix-keychain-test-{tag}-{nanos}-{}",
            std::process::id()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    /// Point the global backend at `dir` in FileVault mode with the given vault
    /// state (None = locked) and drop the bundle cache.
    fn set_vault_backend(dir: &Path, vault: Option<VaultState>) {
        {
            let mut st = backend().lock().unwrap();
            st.data_dir = dir.to_path_buf();
            st.backend = SecretBackend::FileVault;
            st.vault = vault;
        }
        *cache().lock().unwrap() = None;
    }

    #[test]
    fn state_unlocked_with_key_and_no_file() {
        let _guard = TEST_LOCK.lock().unwrap();
        let dir = test_dir("state-nofile");
        set_vault_backend(&dir, None);
        // Passphrase set but nothing cached => no file is written yet…
        set_passphrase("pw").unwrap();
        assert!(!dir.join(VAULT_FILE).exists());
        // …yet the retained key means the vault is unlocked, not absent.
        assert_eq!(state(), SecretStoreState::VaultUnlocked);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn switch_to_keychain_removes_vault_files() {
        let _guard = TEST_LOCK.lock().unwrap();
        let dir = test_dir("switch-keychain");
        set_vault_backend(&dir, None);
        set_passphrase("pw").unwrap();
        // store then delete leaves an empty-secrets bundle but a real vault file on
        // disk, so the switch (empty bundle) never writes to the real keychain.
        store_secret("k", "v").unwrap();
        delete_secret("k").unwrap();
        assert!(dir.join(VAULT_FILE).exists());

        switch_backend(SecretBackend::OsKeychain, None).unwrap();
        assert!(!dir.join(VAULT_FILE).exists());
        // Check the enum directly rather than state(), which would probe the real
        // OS keychain (unavailable on headless CI).
        assert_eq!(backend().lock().unwrap().backend, SecretBackend::OsKeychain);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn unlock_classifies_failures() {
        let _guard = TEST_LOCK.lock().unwrap();
        let dir = test_dir("unlock-classify");
        set_vault_backend(&dir, None);
        set_passphrase("right").unwrap();
        store_secret("k", "v").unwrap(); // writes the vault file
        set_vault_backend(&dir, None); // locked again: key + cache dropped

        assert_eq!(unlock("wrong").err(), Some(UnlockFailure::WrongPassword));

        let path = dir.join(VAULT_FILE);
        let bytes = std::fs::read(&path).unwrap();
        std::fs::write(&path, &bytes[..10]).unwrap(); // truncated: not a vault
        assert_eq!(unlock("right").err(), Some(UnlockFailure::BadFormat));

        std::fs::remove_file(&path).unwrap();
        assert_eq!(unlock("right").err(), Some(UnlockFailure::Unavailable));

        // A stale vault file must not flip a keychain-backed process (L1).
        std::fs::write(&path, &bytes).unwrap();
        {
            let mut st = backend().lock().unwrap();
            st.backend = SecretBackend::OsKeychain;
        }
        assert_eq!(unlock("right").err(), Some(UnlockFailure::Unavailable));
        assert_eq!(backend().lock().unwrap().backend, SecretBackend::OsKeychain);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn set_passphrase_skips_writing_an_empty_bundle() {
        let _guard = TEST_LOCK.lock().unwrap();
        let dir = test_dir("empty-skip");
        set_vault_backend(&dir, None);
        // The no-session startup cleanup leaves Some(empty) in the cache;
        // creating the vault then must not seal an empty file to disk.
        *cache().lock().unwrap() = Some(SecretBundle::new());
        set_passphrase("pw").unwrap();
        assert!(!dir.join(VAULT_FILE).exists());
        assert_eq!(state(), SecretStoreState::VaultUnlocked);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_belated_wipe_spares_secrets_written_by_a_newer_sign_in() {
        let _guard = TEST_LOCK.lock().unwrap();
        let dir = test_dir("belated-wipe");
        set_vault_backend(&dir, None);
        set_passphrase("pw").unwrap();

        // A logout snapshots the epoch, then stalls (the C++ safety timer re-enables
        // the login form) while a new sign-in writes its session secrets.
        let epoch = secret_epoch();
        store_secret("session_access_token", "fresh-login-token").unwrap();

        // The belated wipe must recognize the newer secrets and leave them alone.
        assert!(!clear_all_secrets_if_unchanged(epoch).unwrap());
        assert_eq!(
            load_secret("session_access_token").unwrap().as_deref(),
            Some("fresh-login-token")
        );
        assert_eq!(state(), SecretStoreState::VaultUnlocked); // vault key still held

        // With no newer write, the same call wipes as before.
        assert!(clear_all_secrets_if_unchanged(secret_epoch()).unwrap());
        assert_eq!(load_secret("session_access_token").unwrap(), None);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn concurrent_store_secret_keeps_every_key() {
        let _guard = TEST_LOCK.lock().unwrap();
        let dir = test_dir("concurrent-writes");
        set_vault_backend(&dir, None);
        set_passphrase("pw").unwrap();
        let handles: Vec<_> = (0..8)
            .map(|i| {
                std::thread::spawn(move || {
                    store_secret(&format!("k{i}"), &format!("v{i}")).unwrap();
                })
            })
            .collect();
        for h in handles {
            h.join().unwrap();
        }
        // Warm-cache reads, then a cold read-back from disk: the persisted vault
        // must hold all 8 keys (no lost update, no torn temp file).
        for i in 0..8 {
            assert_eq!(
                load_secret(&format!("k{i}")).unwrap().as_deref(),
                Some(format!("v{i}").as_str())
            );
        }
        *cache().lock().unwrap() = None;
        for i in 0..8 {
            assert_eq!(
                load_secret(&format!("k{i}")).unwrap().as_deref(),
                Some(format!("v{i}").as_str())
            );
        }
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn secret_bundle_roundtrips() {
        let mut bundle = SecretBundle::new();
        bundle
            .secrets
            .insert("search_index_key".to_string(), "abc123".to_string());
        let json = serde_json::to_string(&bundle).unwrap();
        let parsed: SecretBundle = serde_json::from_str(&json).unwrap();
        assert_eq!(parsed.version, BUNDLE_VERSION);
        assert_eq!(
            parsed.secrets.get("search_index_key").map(String::as_str),
            Some("abc123")
        );
    }

    #[test]
    fn secret_bundle_tolerates_missing_secrets_field() {
        // Forward/backward compatibility: an older or partial bundle without the
        // `secrets` map must deserialize to an empty map, not fail.
        let parsed: SecretBundle = serde_json::from_str(r#"{"version":1}"#).unwrap();
        assert_eq!(parsed.version, 1);
        assert!(parsed.secrets.is_empty());
    }

    #[test]
    fn secret_bundle_default_is_empty() {
        let bundle = SecretBundle::default();
        assert!(bundle.secrets.is_empty());
    }
}

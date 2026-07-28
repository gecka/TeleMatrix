// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::path::PathBuf;
use std::time::Duration;

use anyhow::{anyhow, Result};
use matrix_sdk::Client;
use tracing::info;

use crate::local_cache_service::LocalCacheService;
use crate::session_storage_service::SessionStorageService;
use crate::types::{
    RegistrationChallenge, RegistrationRequest, RegistrationResult, UserProfile,
    UsernameAvailability,
};

/// The MSC2965 auth_metadata fields we can point a user at.
struct AuthMetadata {
    issuer: String,
    account_management_uri: Option<String>,
    /// MSC4191 `account_management_actions_supported` — which deep links the
    /// account page honours (matrix.org advertises `org.matrix.profile`).
    account_management_actions: Vec<String>,
}

pub(crate) struct AuthRegistrationOutcome {
    pub(crate) result: RegistrationResult,
    pub(crate) client_for_sync: Option<Client>,
}

pub(crate) struct AuthService {
    data_dir: PathBuf,
    local_cache: LocalCacheService,
    session_storage: SessionStorageService,
}

impl AuthService {
    pub(crate) fn new(
        data_dir: PathBuf,
        local_cache: LocalCacheService,
        session_storage: SessionStorageService,
    ) -> Self {
        Self {
            data_dir,
            local_cache,
            session_storage,
        }
    }

    pub(crate) async fn build_client(
        &self,
        homeserver: &str,
        store_passphrase: &str,
    ) -> Result<Client> {
        // Serialize against concurrent builds/wipes on the same data dir
        // (e.g. the old bridge's logout cleanup): see store_guard.rs.
        let _store_lock = crate::store_guard::lock_store_dir(&self.data_dir).await;
        let store_path = self.data_dir.join("store");

        let app_cache_store = self.local_cache.app_cache_store();
        let cached_versions = app_cache_store.lock().ok().and_then(|guard| {
            guard
                .as_ref()
                .and_then(|store| store.load_server_versions(homeserver).ok())
        });

        // Open the four encrypted SQLite stores CONCURRENTLY. `sqlite_store()`
        // makes the SDK open them sequentially inside `build()`, and each
        // encrypted store open is a fixed ~1.8s cost (passphrase KDF, independent
        // of DB size), so four in series added ~7.4s of cold-start latency that
        // gated the whole UI — including the message timeline, which needs the
        // Client. Opening them in parallel collapses that to ~one store's time.
        // Cap the per-store connection pool (default is physical cores × 4 per store,
        // ~128 blocking threads across 4 stores). With up to 6 account runtimes this
        // is the main OS-thread multiplier. Keep the DEFAULT 2MB page cache — the
        // with_low_memory_config cache shrink regressed key-import decryption. See
        // docs/thread-count-review-2026-07-08.md and code-review-2026-07-19 PERF-1.
        let pool_size = std::thread::available_parallelism()
            .map(|n| n.get())
            .unwrap_or(4);
        let sqlite_config = matrix_sdk::SqliteStoreConfig::new(&store_path)
            .pool_max_size(pool_size)
            .passphrase(Some(store_passphrase));
        // Each open runs on its OWN tokio task so the per-store passphrase KDF
        // runs on separate worker threads concurrently. (Polling them on one
        // task via `try_join!` left them serial, because the open work doesn't
        // yield cooperatively.)
        let stores_started = std::time::Instant::now();
        // open_with_config now borrows the config; give each spawned task its own
        // owned clone so the futures stay 'static for tokio::spawn.
        let cfg_state = sqlite_config.clone();
        let open_state = tokio::spawn(async move {
            matrix_sdk::SqliteStateStore::open_with_config(&cfg_state).await
        });
        let cfg_crypto = sqlite_config.clone();
        let open_crypto = tokio::spawn(async move {
            matrix_sdk::SqliteCryptoStore::open_with_config(&cfg_crypto).await
        });
        let cfg_event = sqlite_config.clone();
        let open_event_cache = tokio::spawn(async move {
            matrix_sdk::SqliteEventCacheStore::open_with_config(&cfg_event).await
        });
        let cfg_media = sqlite_config.clone();
        let open_media = tokio::spawn(async move {
            matrix_sdk::SqliteMediaStore::open_with_config(&cfg_media).await
        });
        let (state_store, crypto_store, event_cache_store, media_store) =
            tokio::try_join!(open_state, open_crypto, open_event_cache, open_media)
                .map_err(|e| anyhow!("SQLite store open task failed: {e}"))?;
        let state_store = state_store.map_err(|e| anyhow!("state store: {e}"))?;
        let crypto_store = crypto_store.map_err(|e| anyhow!("crypto store: {e}"))?;
        let event_cache_store = event_cache_store.map_err(|e| anyhow!("event cache store: {e}"))?;
        let media_store = media_store.map_err(|e| anyhow!("media store: {e}"))?;
        info!(
            "Opened encrypted SQLite stores in parallel in {:?}",
            stores_started.elapsed()
        );
        let store_config = matrix_sdk::store::StoreConfig::new(
            matrix_sdk::cross_process_lock::CrossProcessLockConfig::SingleProcess,
        )
        .state_store(state_store)
        .crypto_store(crypto_store)
        .event_cache_store(event_cache_store)
        .media_store(media_store);

        let mut builder = Client::builder()
            .homeserver_url(homeserver)
            .store_config(store_config)
            .with_encryption_settings(matrix_sdk::encryption::EncryptionSettings {
                // KEEP AfterDecryptionFailure. It is the only strategy that installs
                // the SDK's ongoing per-UTD `BackupDownloadTask` (encryption/mod.rs:
                // initialize_tasks), so keys backed up *after* the initial sync are
                // still fetched on demand. `OneShot` does a single unpaginated bulk
                // download once (the SDK itself notes it "doesn't work for any
                // sizeable account") and installs no ongoing task — a known
                // regression (see docs / e2ee notes). Do NOT switch to OneShot to
                // speed up bulk decryption; pre-fetch keys explicitly instead.
                backup_download_strategy:
                    matrix_sdk::encryption::BackupDownloadStrategy::AfterDecryptionFailure,
                auto_enable_backups: true,
                // Bootstraps cross-signing on password/MSC3967 login
                // (no-op on SSO/OIDC — the verify / recovery-key gate still applies).
                auto_enable_cross_signing: true,
            });

        if let Some(versions) = &cached_versions {
            builder = builder.server_versions(versions.clone());
        }

        let build_fut = builder.build();
        let client = tokio::time::timeout(Duration::from_secs(15), build_fut)
            .await
            .map_err(|_| anyhow!("Connection to {} timed out", homeserver))?
            .map_err(|e| anyhow!("Failed to build client: {e}"))?;

        // NOTE: the event cache is subscribed in `start_sync` (post-auth), NOT
        // here. The SDK's R2D2 redecryptor starts on subscribe() and subscribes to
        // the OlmMachine's room-key stream; if the OlmMachine does not exist yet
        // (build_client runs BEFORE login/restore), it permanently shuts down and
        // arriving keys never re-decrypt cached UTDs. `subscribe()` is idempotent.

        let hs = homeserver.to_string();
        let app_cache_store = self.local_cache.app_cache_store();
        tokio::spawn(async move {
            if let Ok(resp) = reqwest::get(format!(
                "{}/_matrix/client/versions",
                hs.trim_end_matches('/')
            ))
            .await
            {
                if let Ok(body) = resp.bytes().await {
                    if let Ok(guard) = app_cache_store.lock() {
                        if let Some(store) = guard.as_ref() {
                            let _ = store.save_server_versions_from_json(&hs, &body);
                        }
                    }
                }
            }
        });

        Ok(client)
    }

    pub(crate) async fn login(
        &self,
        client: &Client,
        user: &str,
        pass: &str,
        store_passphrase: &str,
    ) -> Result<UserProfile> {
        client
            .matrix_auth()
            .login_username(user, pass)
            .initial_device_display_name(&device_display_name())
            .await
            .map_err(|e| anyhow!("Login failed: {e}"))?;

        self.activate_authenticated_session(client, store_passphrase)
            .await?;

        let user_id = client.user_id().map(|u| u.to_string()).unwrap_or_default();
        let display_name = client
            .account()
            .get_display_name()
            .await
            .ok()
            .flatten()
            .unwrap_or_else(|| user.to_string());
        let avatar_url = client
            .account()
            .get_avatar_url()
            .await
            .ok()
            .flatten()
            .map(|u| u.to_string());

        info!("Logged in as {user_id}");

        Ok(UserProfile {
            user_id,
            display_name,
            avatar_url,
        })
    }

    pub(crate) async fn register(
        &self,
        client: Client,
        request: &RegistrationRequest,
        store_passphrase: &str,
    ) -> Result<AuthRegistrationOutcome> {
        use matrix_sdk::ruma::api::client::account::register;
        use matrix_sdk::ruma::api::client::uiaa;

        let mut reg_request = register::v3::Request::new();
        reg_request.username = Some(request.username.clone());
        reg_request.password = Some(request.password.clone());
        reg_request.initial_device_display_name = Some(device_display_name());
        reg_request.inhibit_login = false;

        if let (Some(session), Some(auth_json)) = (&request.session, &request.auth_json) {
            let auth_obj: serde_json::Map<String, serde_json::Value> =
                serde_json::from_str(auth_json).unwrap_or_default();
            let auth_type = auth_obj
                .get("type")
                .and_then(|v| v.as_str())
                .unwrap_or("m.login.dummy")
                .to_string();
            let mut extra = auth_obj;
            extra.remove("type");
            extra.remove("session");
            reg_request.auth = Some(
                uiaa::AuthData::new(&auth_type, Some(session.clone()), extra)
                    .map_err(|e| anyhow!("Invalid auth data: {e}"))?,
            );
        }

        match client.matrix_auth().register(reg_request).await {
            Ok(response) => {
                let user_id = response.user_id.to_string();
                let display_name = request.username.clone();
                let avatar_url = None;

                info!("Registered as {user_id}");

                if client.matrix_auth().session().is_none() {
                    let access_token = response
                        .access_token
                        .clone()
                        .ok_or_else(|| anyhow!("Registration succeeded without access token"))?;
                    let device_id = response
                        .device_id
                        .clone()
                        .ok_or_else(|| anyhow!("Registration succeeded without device ID"))?;
                    let session = matrix_sdk::authentication::matrix::MatrixSession {
                        meta: matrix_sdk::SessionMeta {
                            user_id: response.user_id.clone(),
                            device_id,
                        },
                        tokens: matrix_sdk::authentication::SessionTokens {
                            access_token,
                            refresh_token: response.refresh_token.clone(),
                        },
                    };
                    client
                        .restore_session(session)
                        .await
                        .map_err(|e| anyhow!("Failed to activate registered session: {e}"))?;
                }

                self.activate_authenticated_session(&client, store_passphrase)
                    .await?;

                Ok(AuthRegistrationOutcome {
                    result: RegistrationResult::Success(UserProfile {
                        user_id,
                        display_name,
                        avatar_url,
                    }),
                    client_for_sync: Some(client),
                })
            }
            Err(e) => {
                if let Some(uiaa_info) = e.as_uiaa_response() {
                    Ok(AuthRegistrationOutcome {
                        result: RegistrationResult::Challenge(Self::uiaa_to_challenge(uiaa_info)),
                        client_for_sync: None,
                    })
                } else {
                    let msg = format!("{e}");
                    Err(anyhow!("Registration failed: {msg}"))
                }
            }
        }
    }

    pub(crate) async fn check_username_available(
        homeserver: &str,
        username: &str,
    ) -> Result<UsernameAvailability> {
        let client = Client::builder()
            .homeserver_url(homeserver)
            .build()
            .await
            .map_err(|e| anyhow!("Failed to build client: {e}"))?;

        use matrix_sdk::ruma::api::client::account::get_username_availability;
        let request = get_username_availability::v3::Request::new(username.to_string());
        match client.send(request).await {
            Ok(response) => {
                if response.available {
                    Ok(UsernameAvailability::Available)
                } else {
                    Ok(UsernameAvailability::Unavailable)
                }
            }
            Err(e) => {
                let msg = format!("{e}");
                if msg.contains("M_USER_IN_USE") {
                    Ok(UsernameAvailability::Unavailable)
                } else if msg.contains("M_INVALID_USERNAME") {
                    Ok(UsernameAvailability::Invalid)
                } else {
                    Ok(UsernameAvailability::Error(msg))
                }
            }
        }
    }

    /// Discover the Matrix homeserver URL for a domain via `.well-known`.
    pub(crate) async fn discover_homeserver(domain: &str) -> Result<String> {
        let url = format!("https://{domain}/.well-known/matrix/client");
        let resp = reqwest::get(&url).await?;
        if !resp.status().is_success() {
            return Err(anyhow!(
                "Homeserver discovery failed for {domain}: HTTP {}",
                resp.status()
            ));
        }
        let body: serde_json::Value = resp.json().await?;
        let hs_url = body["m.homeserver"]["base_url"]
            .as_str()
            .ok_or_else(|| anyhow!("No m.homeserver.base_url in .well-known response"))?;
        Ok(hs_url.trim_end_matches('/').to_string())
    }

    /// Parse an MSC2965 auth_metadata body. None when it doesn't describe a
    /// delegated-auth server.
    fn parse_auth_metadata(body: &[u8]) -> Option<AuthMetadata> {
        let json: serde_json::Value = serde_json::from_slice(body).ok()?;
        let issuer = json["issuer"].as_str().map(str::trim)?;
        if issuer.is_empty() {
            return None;
        }
        Some(AuthMetadata {
            issuer: issuer.to_string(),
            account_management_uri: json["account_management_uri"]
                .as_str()
                .map(str::trim)
                .filter(|url| !url.is_empty())
                .map(str::to_string),
            account_management_actions: json["account_management_actions_supported"]
                .as_array()
                .map(|actions| {
                    actions
                        .iter()
                        .filter_map(|action| action.as_str().map(str::to_string))
                        .collect()
                })
                .unwrap_or_default(),
        })
    }

    /// Fetch a homeserver's MSC2965 auth metadata, stable path first.
    async fn fetch_auth_metadata(client: &reqwest::Client, base_url: &str) -> Option<AuthMetadata> {
        let base = base_url.trim_end_matches('/');
        for path in [
            "/_matrix/client/v1/auth_metadata",
            "/_matrix/client/unstable/org.matrix.msc2965/auth_metadata",
        ] {
            let Ok(resp) = client.get(format!("{base}{path}")).send().await else {
                continue;
            };
            if !resp.status().is_success() {
                continue;
            }
            if let Ok(body) = resp.bytes().await {
                if let Some(meta) = Self::parse_auth_metadata(&body) {
                    return Some(meta);
                }
            }
        }
        None
    }

    /// The page where a delegated-auth (OIDC/MAS) homeserver manages account
    /// details — email addresses and phone numbers among them. Such servers turn
    /// the legacy 3PID API off entirely (matrix.org answers M_UNRECOGNIZED to
    /// `/account/3pid/*/requestToken`), so this link is the only way for the user
    /// to get at them. None when the server manages 3PIDs itself.
    pub(crate) async fn probe_account_management(base_url: &str) -> Option<String> {
        let client = reqwest::Client::builder()
            .timeout(Duration::from_secs(4))
            .build()
            .ok()?;
        let meta = Self::fetch_auth_metadata(&client, base_url).await?;
        let account = meta.account_management_uri?;
        // MSC4191: deep-link straight to profile management when the server says
        // it honours that action; otherwise land on the account page itself.
        const PROFILE_ACTION: &str = "org.matrix.profile";
        if meta
            .account_management_actions
            .iter()
            .any(|action| action == PROFILE_ACTION)
        {
            let separator = if account.contains('?') { '&' } else { '?' };
            return Some(format!("{account}{separator}action={PROFILE_ACTION}"));
        }
        Some(account)
    }

    /// Where a delegated-auth homeserver lets someone reset a *forgotten*
    /// password. Takes raw user input ("matrix.org") and resolves it, like
    /// `classify_registration` does. `None` when the server has no such page —
    /// which is the normal answer for a homeserver that handles password reset
    /// itself, and the caller should say nothing extra in that case.
    pub(crate) async fn probe_password_reset_page(input: &str) -> Option<String> {
        let client = reqwest::Client::builder()
            .timeout(Duration::from_secs(4))
            .build()
            .ok()?;
        let base = Self::resolve_base_url(input).await;
        let meta = Self::fetch_auth_metadata(&client, &base).await?;
        Self::password_reset_page(&client, &meta).await
    }

    /// The page that actually resets a password.
    ///
    /// There is no account-management action for it: matrix.org advertises
    /// profile / devices / cross-signing and nothing for passwords. And the bare
    /// `account_management_uri` is worse than useless here — it 303s to a
    /// sign-in page, which is precisely what someone who has forgotten their
    /// password cannot get past. MAS serves recovery at `<issuer>/recover`, so
    /// use that when the server really answers there (same probe-then-fall-back
    /// shape as `registration_page`).
    async fn password_reset_page(client: &reqwest::Client, meta: &AuthMetadata) -> Option<String> {
        let candidate = format!("{}/recover", meta.issuer.trim_end_matches('/'));
        if let Ok(resp) = client.get(&candidate).send().await {
            if resp.status().is_success() {
                return Some(candidate);
            }
        }
        meta.account_management_uri.clone()
    }

    /// The page that actually creates an account. MSC2965 advertises no such URL:
    /// `account_management_uri` is for *managing* an existing account and on
    /// matrix.org it 303s to a sign-in page, which is the last thing someone
    /// pressing "create account" wants. MAS serves registration at
    /// `<issuer>/register`, so use that when the server really answers there, and
    /// otherwise fall back to what the metadata does advertise.
    async fn registration_page(client: &reqwest::Client, meta: &AuthMetadata) -> String {
        let candidate = format!("{}/register", meta.issuer.trim_end_matches('/'));
        if let Ok(resp) = client.get(&candidate).send().await {
            if resp.status().is_success() {
                return candidate;
            }
        }
        meta.account_management_uri
            .clone()
            .unwrap_or_else(|| meta.issuer.clone())
    }

    /// Detect whether a homeserver delegates auth to OIDC/MAS (MSC3861) — in
    /// that case legacy registration always 403s and accounts are created on
    /// the server's website. Returns that website's signup URL. Advisory: any
    /// network/HTTP/parse failure is reported as "not delegated".
    pub(crate) async fn probe_auth_delegation(base_url: &str) -> Option<String> {
        let client = reqwest::Client::builder()
            .timeout(Duration::from_secs(4))
            .build()
            .ok()?;
        let meta = Self::fetch_auth_metadata(&client, base_url).await?;
        Some(Self::registration_page(&client, &meta).await)
    }

    /// Resolve a user-typed homeserver (bare domain or full URL) to a base URL:
    /// `.well-known` discovery for a bare domain, else the value itself with an
    /// `https://` default.
    async fn resolve_base_url(input: &str) -> String {
        let trimmed = input.trim().trim_end_matches('/');
        if trimmed.starts_with("http://") || trimmed.starts_with("https://") {
            return trimmed.to_string();
        }
        match Self::discover_homeserver(trimmed).await {
            Ok(url) => url,
            Err(_) => format!("https://{trimmed}"),
        }
    }

    /// Whether `base_url` answers `/_matrix/client/versions` like a Matrix server
    /// (a `{"versions": [...]}` body). The reliable "is this a Matrix server"
    /// signal — `.well-known` discovery and the delegation probe both stay silent
    /// for a plain non-delegated server, so neither can rule a URL out on its own.
    async fn is_matrix_homeserver(base_url: &str) -> bool {
        let Ok(client) = reqwest::Client::builder()
            .timeout(Duration::from_secs(4))
            .build()
        else {
            return false;
        };
        let url = format!("{}/_matrix/client/versions", base_url.trim_end_matches('/'));
        let Ok(resp) = client.get(&url).send().await else {
            return false;
        };
        if !resp.status().is_success() {
            return false;
        }
        let Ok(body) = resp.bytes().await else {
            return false;
        };
        serde_json::from_slice::<serde_json::Value>(&body)
            .ok()
            .and_then(|v| {
                v.get("versions")
                    .and_then(|x| x.as_array())
                    .map(|a| !a.is_empty())
            })
            .unwrap_or(false)
    }

    /// Classify a homeserver for the two-step registration flow. Returns a status
    /// and a URL:
    /// - `0` = not a Matrix server (url empty),
    /// - `1` = password registration path (url = resolved base URL to register on),
    /// - `2` = auth delegated to OIDC/MAS (url = the website that creates accounts).
    pub(crate) async fn classify_registration(input: &str) -> (i32, String) {
        let base = Self::resolve_base_url(input).await;
        if !Self::is_matrix_homeserver(&base).await {
            return (0, String::new());
        }
        if let Some(reg_url) = Self::probe_auth_delegation(&base).await {
            return (2, reg_url);
        }
        (1, base)
    }

    /// Classify an `/account/3pid/email/requestToken` response body. Some(false)
    /// when the homeserver says it cannot verify email at all; Some(true) when it
    /// got far enough to complain about the (deliberately empty) request body,
    /// which it only does once the medium itself is accepted; None when the answer
    /// says nothing either way.
    fn parse_email_threepid_support(status: u16, body: &[u8]) -> Option<bool> {
        if status != 400 {
            return None;
        }
        let json: serde_json::Value = serde_json::from_slice(body).ok()?;
        let errcode = json["errcode"].as_str().unwrap_or_default();
        let message = json["error"].as_str().unwrap_or_default();

        // MSC4178 gave this its own errcode; servers predating it (Synapse < 1.130)
        // raise a bare 400 whose message is the only thing left to go on.
        if errcode == "M_THREEPID_MEDIUM_NOT_SUPPORTED"
            || message.starts_with("Adding an email to your account is disabled")
            || message.starts_with("3PID changes are disabled")
        {
            return Some(false);
        }
        // The medium gate passed and the body was rejected instead.
        if errcode == "M_BAD_JSON" || errcode == "M_MISSING_PARAM" {
            return Some(true);
        }
        None
    }

    /// Whether a homeserver can verify email addresses, asked without sending one.
    ///
    /// There is no capability for this — `m.3pid_changes` is a single aggregate
    /// flag that says nothing about a specific medium, and it reads `true` on a
    /// server that merely has no mail configured. The only per-medium signal is the
    /// `requestToken` error, which normally costs a real verification email to
    /// obtain. It doesn't have to: the server rejects an unsupported medium *before*
    /// it parses the request body, so an empty body separates "can't verify email"
    /// from "would have tried" while giving it nothing to send to.
    ///
    /// None when the server answers in a way that settles nothing (the caller must
    /// then assume email works and let the user try).
    pub(crate) async fn probe_email_threepid_support(base_url: &str) -> Option<bool> {
        let client = reqwest::Client::builder()
            .timeout(Duration::from_secs(4))
            .build()
            .ok()?;
        let url = format!(
            "{}/_matrix/client/v3/account/3pid/email/requestToken",
            base_url.trim_end_matches('/')
        );
        let resp = client
            .post(&url)
            .json(&serde_json::json!({}))
            .send()
            .await
            .ok()?;
        let status = resp.status().as_u16();
        let body = resp.bytes().await.ok()?;
        Self::parse_email_threepid_support(status, &body)
    }

    /// Request a password reset token via email (unauthenticated).
    ///
    /// Calls `POST /_matrix/client/v3/account/password/email/requestToken`.
    /// Returns `(sid, submit_url)` on success. The caller should generate and
    /// pass in a `client_secret` so it can be reused for the subsequent
    /// `reset_password` call.
    pub(crate) async fn request_password_reset_token(
        homeserver: &str,
        email: &str,
        client_secret: &str,
    ) -> Result<(String, Option<String>)> {
        let url = format!(
            "{}/_matrix/client/v3/account/password/email/requestToken",
            homeserver.trim_end_matches('/')
        );
        let body = serde_json::json!({
            "client_secret": client_secret,
            "email": email,
            "send_attempt": 1,
        });
        let resp = reqwest::Client::new()
            .post(&url)
            .json(&body)
            .send()
            .await
            .map_err(|e| anyhow!("Network error: {e}"))?;

        if !resp.status().is_success() {
            let err: serde_json::Value = resp.json().await.unwrap_or_default();
            let errcode = err["errcode"].as_str().unwrap_or_default();
            let msg = err["error"]
                .as_str()
                .unwrap_or("Failed to send password reset email");
            // Prefix the errcode so the UI can map known codes to friendly copy.
            if errcode.is_empty() {
                return Err(anyhow!("{msg}"));
            }
            return Err(anyhow!("{errcode}: {msg}"));
        }

        let result: serde_json::Value = resp.json().await?;
        let sid = result["sid"].as_str().unwrap_or_default().to_string();
        let submit_url = result["submit_url"].as_str().map(|s| s.to_string());
        Ok((sid, submit_url))
    }

    /// Reset password using the email verification token (unauthenticated).
    ///
    /// Calls `POST /_matrix/client/v3/account/password` with
    /// `m.login.email.identity` auth data containing the `sid` and
    /// `client_secret` obtained from `request_password_reset_token`.
    pub(crate) async fn reset_password(
        homeserver: &str,
        new_password: &str,
        sid: &str,
        client_secret: &str,
    ) -> Result<()> {
        let url = format!(
            "{}/_matrix/client/v3/account/password",
            homeserver.trim_end_matches('/')
        );
        let body = serde_json::json!({
            "new_password": new_password,
            "logout_devices": true,
            "auth": {
                "type": "m.login.email.identity",
                "threepid_creds": {
                    "sid": sid,
                    "client_secret": client_secret,
                },
                "threepidCreds": {
                    "sid": sid,
                    "client_secret": client_secret,
                },
            },
        });
        let resp = reqwest::Client::new()
            .post(&url)
            .json(&body)
            .send()
            .await
            .map_err(|e| anyhow!("Network error: {e}"))?;

        if !resp.status().is_success() {
            let err: serde_json::Value = resp.json().await.unwrap_or_default();
            let errcode = err["errcode"].as_str().unwrap_or_default();
            let msg = err["error"].as_str().unwrap_or("Password reset failed");
            if errcode.is_empty() {
                return Err(anyhow!("{msg}"));
            }
            return Err(anyhow!("{errcode}: {msg}"));
        }

        Ok(())
    }

    async fn activate_authenticated_session(
        &self,
        client: &Client,
        store_passphrase: &str,
    ) -> Result<()> {
        let dir_ns = crate::session_storage_service::dir_namespace(&self.data_dir);
        let (app_cache_passphrase, preview_cache_passphrase, _) = self
            .session_storage
            .commit_session_secrets(client, store_passphrase, &dir_ns)?;
        if let Err(err) = self
            .local_cache
            .ensure_local_cache_stores_open(&app_cache_passphrase, &preview_cache_passphrase)
        {
            let _ = client.matrix_auth().logout().await;
            let store_path = self.data_dir.join("store");
            if store_path.exists() {
                let _ = tokio::fs::remove_dir_all(&store_path).await;
            }
            return Err(anyhow!("Failed to save secure session secrets: {err}"));
        }
        self.session_storage.clear_pending_auth_store_passphrase();
        Ok(())
    }

    fn uiaa_to_challenge(
        info: &matrix_sdk::ruma::api::client::uiaa::UiaaInfo,
    ) -> RegistrationChallenge {
        let flows = info
            .flows
            .iter()
            .map(|f| f.stages.iter().map(|s| s.to_string()).collect())
            .collect();
        let completed = info.completed.iter().map(|s| s.to_string()).collect();
        let mut params = std::collections::HashMap::new();
        if let Some(ref raw_params) = info.params {
            if let Ok(serde_json::Value::Object(map)) =
                serde_json::from_str::<serde_json::Value>(raw_params.get())
            {
                for (key, value) in map {
                    params.insert(key, value.to_string());
                }
            }
        }
        let (errcode, error_msg) = if let Some(ref std_err) = info.auth_error {
            (
                Some(format!("{:?}", std_err.kind)),
                Some(std_err.message.clone()),
            )
        } else {
            (None, None)
        };
        RegistrationChallenge {
            session: info.session.clone().unwrap_or_default(),
            flows,
            completed,
            params,
            errcode,
            error: error_msg,
        }
    }
}

/// Session name other clients show in their device list, e.g.
/// "TeleMatrix Desktop: macOS". Deliberately untranslated: the homeserver
/// stores it verbatim and renders it to *other* users, in their locale.
fn device_display_name() -> String {
    format!("TeleMatrix Desktop: {}", os_label(std::env::consts::OS))
}

/// Split from `device_display_name` so every platform's label is testable from
/// any host — `#[cfg(target_os)]` constants would ship untested on CI.
fn os_label(os: &str) -> &str {
    match os {
        "macos" => "macOS",
        "windows" => "Windows",
        "linux" => "Linux",
        other => other,
    }
}

#[cfg(test)]
mod tests {
    use super::{device_display_name, os_label, AuthService};
    use wiremock::matchers::{method, path};
    use wiremock::{Mock, MockServer, ResponseTemplate};

    /// auth_metadata as matrix.org serves it: the issuer is the MAS origin and
    /// `account_management_uri` points at the *manage* page (which redirects to a
    /// sign-in form — never the signup page we want).
    fn metadata_json(issuer: &str) -> String {
        format!(r#"{{"issuer": "{issuer}/", "account_management_uri": "{issuer}/account/"}}"#)
    }

    fn mock_metadata(issuer: &str) -> ResponseTemplate {
        ResponseTemplate::new(200).set_body_raw(metadata_json(issuer), "application/json")
    }

    #[test]
    fn parse_auth_metadata_reads_issuer_and_account_uri() {
        let body = br#"{
            "issuer": "https://account.example.org/",
            "account_management_uri": "https://account.example.org/account/"
        }"#;
        let meta = AuthService::parse_auth_metadata(body).unwrap();
        assert_eq!(meta.issuer, "https://account.example.org/");
        assert_eq!(
            meta.account_management_uri.as_deref(),
            Some("https://account.example.org/account/")
        );

        let issuer_only = br#"{"issuer": "https://a.example/", "account_management_uri": "  "}"#;
        let meta = AuthService::parse_auth_metadata(issuer_only).unwrap();
        assert_eq!(meta.issuer, "https://a.example/");
        assert_eq!(meta.account_management_uri, None);
    }

    #[test]
    fn parse_auth_metadata_rejects_non_delegated_bodies() {
        assert!(AuthService::parse_auth_metadata(b"{}").is_none());
        assert!(AuthService::parse_auth_metadata(br#"{"issuer": ""}"#).is_none());
        assert!(AuthService::parse_auth_metadata(b"not json").is_none());
    }

    #[tokio::test]
    async fn probe_points_at_the_signup_page_not_the_manage_page() {
        let server = MockServer::start().await;
        let issuer = server.uri();
        Mock::given(method("GET"))
            .and(path("/_matrix/client/v1/auth_metadata"))
            .respond_with(mock_metadata(&issuer))
            .mount(&server)
            .await;
        // MAS serves the registration form here; account_management_uri does not.
        Mock::given(method("GET"))
            .and(path("/register"))
            .respond_with(ResponseTemplate::new(200))
            .mount(&server)
            .await;

        assert_eq!(
            AuthService::probe_auth_delegation(&server.uri())
                .await
                .as_deref(),
            Some(format!("{issuer}/register").as_str())
        );
    }

    #[tokio::test]
    async fn probe_falls_back_when_the_server_has_no_register_page() {
        let server = MockServer::start().await;
        let issuer = server.uri();
        Mock::given(method("GET"))
            .and(path("/_matrix/client/v1/auth_metadata"))
            .respond_with(mock_metadata(&issuer))
            .mount(&server)
            .await;
        // No /register mounted: 404 -> keep whatever the metadata advertises.
        assert_eq!(
            AuthService::probe_auth_delegation(&server.uri())
                .await
                .as_deref(),
            Some(format!("{issuer}/account/").as_str())
        );
    }

    #[tokio::test]
    async fn probe_falls_back_to_the_unstable_endpoint() {
        let server = MockServer::start().await;
        let issuer = server.uri();
        Mock::given(method("GET"))
            .and(path("/_matrix/client/v1/auth_metadata"))
            .respond_with(ResponseTemplate::new(404))
            .mount(&server)
            .await;
        Mock::given(method("GET"))
            .and(path(
                "/_matrix/client/unstable/org.matrix.msc2965/auth_metadata",
            ))
            .respond_with(mock_metadata(&issuer))
            .mount(&server)
            .await;
        Mock::given(method("GET"))
            .and(path("/register"))
            .respond_with(ResponseTemplate::new(200))
            .mount(&server)
            .await;

        assert_eq!(
            AuthService::probe_auth_delegation(&server.uri())
                .await
                .as_deref(),
            Some(format!("{issuer}/register").as_str())
        );
    }

    #[tokio::test]
    async fn account_management_deep_links_to_profile_when_supported() {
        let server = MockServer::start().await;
        let issuer = server.uri();
        let body = format!(
            r#"{{"issuer": "{issuer}/",
                 "account_management_uri": "{issuer}/account/",
                 "account_management_actions_supported": ["org.matrix.profile", "org.matrix.sessions_list"]}}"#
        );
        Mock::given(method("GET"))
            .and(path("/_matrix/client/v1/auth_metadata"))
            .respond_with(ResponseTemplate::new(200).set_body_raw(body, "application/json"))
            .mount(&server)
            .await;

        assert_eq!(
            AuthService::probe_account_management(&server.uri())
                .await
                .as_deref(),
            Some(format!("{issuer}/account/?action=org.matrix.profile").as_str())
        );
    }

    #[tokio::test]
    async fn account_management_skips_an_unadvertised_action() {
        let server = MockServer::start().await;
        let issuer = server.uri();
        // metadata_json() advertises no actions at all.
        Mock::given(method("GET"))
            .and(path("/_matrix/client/v1/auth_metadata"))
            .respond_with(mock_metadata(&issuer))
            .mount(&server)
            .await;

        assert_eq!(
            AuthService::probe_account_management(&server.uri())
                .await
                .as_deref(),
            Some(format!("{issuer}/account/").as_str())
        );
    }

    #[tokio::test]
    async fn account_management_is_absent_on_a_self_managing_server() {
        let server = MockServer::start().await;
        // A homeserver that manages 3PIDs itself advertises no auth metadata.
        assert_eq!(
            AuthService::probe_account_management(&server.uri()).await,
            None
        );
    }

    #[tokio::test]
    async fn probe_treats_a_legacy_server_as_not_delegated() {
        let server = MockServer::start().await;
        // No mocks mounted: both endpoints 404.
        assert_eq!(
            AuthService::probe_auth_delegation(&server.uri()).await,
            None
        );
    }

    fn threepid_error(errcode: &str, message: &str) -> ResponseTemplate {
        ResponseTemplate::new(400).set_body_raw(
            format!(r#"{{"errcode": "{errcode}", "error": "{message}"}}"#),
            "application/json",
        )
    }

    async fn mount_email_requesttoken(server: &MockServer, response: ResponseTemplate) {
        Mock::given(method("POST"))
            .and(path("/_matrix/client/v3/account/3pid/email/requestToken"))
            .respond_with(response)
            .mount(server)
            .await;
    }

    #[tokio::test]
    async fn email_support_probe_reads_the_msc4178_errcode() {
        let server = MockServer::start().await;
        mount_email_requesttoken(
            &server,
            threepid_error(
                "M_THREEPID_MEDIUM_NOT_SUPPORTED",
                "Adding an email to your account is disabled on this server",
            ),
        )
        .await;
        assert_eq!(
            AuthService::probe_email_threepid_support(&server.uri()).await,
            Some(false)
        );
    }

    #[tokio::test]
    async fn email_support_probe_reads_a_pre_msc4178_server() {
        let server = MockServer::start().await;
        // Synapse < 1.130 omits the errcode entirely; only the message identifies it.
        mount_email_requesttoken(
            &server,
            threepid_error(
                "M_UNKNOWN",
                "Adding an email to your account is disabled on this server",
            ),
        )
        .await;
        assert_eq!(
            AuthService::probe_email_threepid_support(&server.uri()).await,
            Some(false)
        );
    }

    #[tokio::test]
    async fn email_support_probe_reads_3pid_changes_being_off() {
        let server = MockServer::start().await;
        mount_email_requesttoken(
            &server,
            threepid_error("M_FORBIDDEN", "3PID changes are disabled on this server"),
        )
        .await;
        assert_eq!(
            AuthService::probe_email_threepid_support(&server.uri()).await,
            Some(false)
        );
    }

    #[tokio::test]
    async fn email_support_probe_reads_a_rejected_body_as_supported() {
        let server = MockServer::start().await;
        // The server got past the medium gate and complained about the empty body
        // instead — which it only does when it can verify email. Nothing was sent.
        mount_email_requesttoken(
            &server,
            threepid_error(
                "M_BAD_JSON",
                "2 validation errors for EmailRequestTokenBody",
            ),
        )
        .await;
        assert_eq!(
            AuthService::probe_email_threepid_support(&server.uri()).await,
            Some(true)
        );
    }

    #[tokio::test]
    async fn email_support_probe_is_undecided_on_an_unknown_answer() {
        let server = MockServer::start().await;
        // A delegated-auth server answers M_UNRECOGNIZED here; rate limiting and
        // outages are equally uninformative. None keeps the form on offer.
        mount_email_requesttoken(
            &server,
            threepid_error("M_UNRECOGNIZED", "Unrecognized request"),
        )
        .await;
        assert_eq!(
            AuthService::probe_email_threepid_support(&server.uri()).await,
            None
        );
    }

    #[tokio::test]
    async fn email_support_probe_is_undecided_when_the_server_is_unreachable() {
        let server = MockServer::start().await;
        // No mock mounted: the endpoint 404s, which settles nothing either way.
        assert_eq!(
            AuthService::probe_email_threepid_support(&server.uri()).await,
            None
        );
    }

    #[test]
    fn os_label_capitalizes_the_desktop_platforms() {
        assert_eq!(os_label("macos"), "macOS");
        assert_eq!(os_label("windows"), "Windows");
        assert_eq!(os_label("linux"), "Linux");
    }

    #[test]
    fn os_label_passes_through_unknown_platforms() {
        assert_eq!(os_label("freebsd"), "freebsd");
    }

    #[test]
    fn device_display_name_is_prefixed_and_names_this_platform() {
        let name = device_display_name();
        assert_eq!(
            name,
            format!("TeleMatrix Desktop: {}", os_label(std::env::consts::OS))
        );
        assert!(name.starts_with("TeleMatrix Desktop: "));
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use anyhow::Result;
use matrix_sdk::Client;

use crate::types::{
    AccountActionResult, AccountCapabilities, AccountSummary, ThreePid, ThreePidMedium,
    ThreePidTokenResponse,
};

#[derive(Clone, Default)]
pub(crate) struct AccountService;

impl AccountService {
    pub(crate) fn new() -> Self {
        Self
    }

    pub(crate) async fn get_summary(&self, client: Client) -> Result<AccountSummary> {
        let own_user_id = client.user_id().map(|user_id| user_id.to_owned());
        let user_id = own_user_id
            .as_ref()
            .map(|user_id| user_id.to_string())
            .unwrap_or_default();
        let fallback_name = own_user_id
            .as_ref()
            .map(|user_id| user_id.localpart().to_string())
            .unwrap_or_else(|| user_id.clone());
        let display_name = client
            .account()
            .get_display_name()
            .await
            .ok()
            .flatten()
            .unwrap_or(fallback_name);
        let avatar_url = client
            .account()
            .get_avatar_url()
            .await
            .ok()
            .flatten()
            .map(|url| url.to_string());

        // 0.17 replaced Client::get_capabilities() with the lazy/cached
        // HomeserverCapabilities helper; each check shares one fetch.
        let caps = client.homeserver_capabilities();
        let capabilities = AccountCapabilities {
            can_change_password: caps.can_change_password().await.unwrap_or(false),
            can_set_display_name: caps.can_change_displayname().await.unwrap_or(false),
            can_set_avatar_url: caps.can_change_avatar().await.unwrap_or(false),
            can_change_3pid: caps.can_change_thirdparty_ids().await.unwrap_or(false),
        };

        Ok(AccountSummary {
            user_id,
            display_name,
            avatar_url,
            capabilities,
        })
    }

    pub(crate) async fn set_display_name(&self, client: Client, name: &str) -> Result<()> {
        let name_opt = if name.is_empty() { None } else { Some(name) };
        client.account().set_display_name(name_opt).await?;
        Ok(())
    }

    pub(crate) async fn set_avatar_url(&self, client: Client, mxc_url: Option<&str>) -> Result<()> {
        match mxc_url {
            Some(url) if !url.is_empty() => {
                let parsed = matrix_sdk::ruma::OwnedMxcUri::from(url.to_string());
                client.account().set_avatar_url(Some(&parsed)).await?;
            }
            _ => {
                client
                    .account()
                    .set_avatar_url(None::<&matrix_sdk::ruma::MxcUri>)
                    .await?;
            }
        }
        Ok(())
    }

    pub(crate) async fn upload_avatar(
        &self,
        client: Client,
        data: Vec<u8>,
        content_type: &str,
    ) -> Result<String> {
        let mime: mime::Mime = content_type
            .parse()
            .unwrap_or(mime::APPLICATION_OCTET_STREAM);
        let response = client.media().upload(&mime, data, None).await?;
        let mxc_url = response.content_uri.to_string();
        client
            .account()
            .set_avatar_url(Some(&response.content_uri))
            .await?;
        Ok(mxc_url)
    }

    pub(crate) async fn get_3pids(&self, client: Client) -> Result<Vec<ThreePid>> {
        let response = client
            .send(matrix_sdk::ruma::api::client::account::get_3pids::v3::Request::new())
            .await?;
        let mut result = Vec::new();
        for threepid in response.threepids {
            let medium = match threepid.medium {
                matrix_sdk::ruma::thirdparty::Medium::Email => ThreePidMedium::Email,
                matrix_sdk::ruma::thirdparty::Medium::Msisdn => ThreePidMedium::Msisdn,
                _ => continue,
            };
            let validated_secs = threepid.validated_at.as_secs();
            let added_secs = threepid.added_at.as_secs();
            result.push(ThreePid {
                medium,
                address: threepid.address,
                validated_at: Some(validated_secs.into()),
                added_at: Some(added_secs.into()),
            });
        }
        Ok(result)
    }

    pub(crate) async fn request_3pid_token(
        &self,
        client: Client,
        medium: ThreePidMedium,
        address: &str,
        country: &str,
        client_secret: &str,
        send_attempt: u32,
    ) -> Result<ThreePidTokenResponse> {
        let client_secret = matrix_sdk::ruma::ClientSecret::parse(client_secret)?;
        match medium {
            ThreePidMedium::Email => {
                let request =
                    matrix_sdk::ruma::api::client::account::request_3pid_management_token_via_email::v3::Request::new(
                        client_secret,
                        address.to_owned(),
                        send_attempt.into(),
                    );
                let response = client.send(request).await?;
                Ok(ThreePidTokenResponse {
                    sid: response.sid.to_string(),
                    submit_url: response.submit_url.map(|url| url.to_string()),
                })
            }
            ThreePidMedium::Msisdn => {
                let country = if country.trim().is_empty() {
                    "US".to_owned()
                } else {
                    country.trim().to_ascii_uppercase()
                };
                let request =
                    matrix_sdk::ruma::api::client::account::request_3pid_management_token_via_msisdn::v3::Request::new(
                        client_secret,
                        country,
                        address.to_owned(),
                        send_attempt.into(),
                    );
                let response = client.send(request).await?;
                Ok(ThreePidTokenResponse {
                    sid: response.sid.to_string(),
                    submit_url: response.submit_url.map(|url| url.to_string()),
                })
            }
        }
    }

    pub(crate) async fn add_3pid(
        &self,
        client: Client,
        client_secret: &str,
        sid: &str,
        auth_json: Option<&str>,
    ) -> Result<AccountActionResult> {
        let client_secret = matrix_sdk::ruma::ClientSecret::parse(client_secret)?;
        let session_id = matrix_sdk::ruma::SessionId::parse(sid)?;
        let mut request = matrix_sdk::ruma::api::client::account::add_3pid::v3::Request::new(
            client_secret,
            session_id,
        );
        if let Some(json) = auth_json {
            if let Ok(auth_data) = serde_json::from_str(json) {
                request.auth = Some(auth_data);
            }
        }
        match client.send(request).await {
            Ok(_) => Ok(Self::completed_action_result()),
            Err(error) => Ok(Self::account_action_error_result(error.into())),
        }
    }

    pub(crate) async fn delete_3pid(
        &self,
        client: Client,
        medium: ThreePidMedium,
        address: &str,
    ) -> Result<()> {
        let ruma_medium = match medium {
            ThreePidMedium::Email => matrix_sdk::ruma::thirdparty::Medium::Email,
            ThreePidMedium::Msisdn => matrix_sdk::ruma::thirdparty::Medium::Msisdn,
        };
        let request = matrix_sdk::ruma::api::client::account::delete_3pid::v3::Request::new(
            ruma_medium,
            address.to_owned(),
        );
        client.send(request).await?;
        Ok(())
    }

    pub(crate) async fn change_password(
        &self,
        client: Client,
        new_password: &str,
        auth_json: Option<&str>,
    ) -> Result<AccountActionResult> {
        let mut request = matrix_sdk::ruma::api::client::account::change_password::v3::Request::new(
            new_password.to_owned(),
        );
        if let Some(json) = auth_json {
            if let Ok(auth_data) = serde_json::from_str(json) {
                request.auth = Some(auth_data);
            }
        }
        match client.send(request).await {
            Ok(_) => Ok(Self::completed_action_result()),
            Err(error) => Ok(Self::account_action_error_result(error.into())),
        }
    }

    pub(crate) async fn deactivate_account(
        &self,
        client: Client,
        erase_data: bool,
        auth_json: Option<&str>,
    ) -> Result<AccountActionResult> {
        let mut request = matrix_sdk::ruma::api::client::account::deactivate::v3::Request::new();
        request.erase = erase_data;
        if let Some(json) = auth_json {
            if let Ok(auth_data) = serde_json::from_str(json) {
                request.auth = Some(auth_data);
            }
        }
        match client.send(request).await {
            Ok(_) => Ok(Self::completed_action_result()),
            Err(error) => Ok(Self::account_action_error_result(error.into())),
        }
    }

    fn completed_action_result() -> AccountActionResult {
        AccountActionResult {
            completed: true,
            error_message: None,
            uia_session: None,
            uia_flows_json: None,
        }
    }

    fn account_action_error_result(error: matrix_sdk::Error) -> AccountActionResult {
        if let Some(uiaa) = error.as_uiaa_response() {
            AccountActionResult {
                completed: false,
                error_message: None,
                uia_session: uiaa.session.clone(),
                uia_flows_json: serde_json::to_string(&uiaa.flows).ok(),
            }
        } else {
            AccountActionResult {
                completed: false,
                error_message: Some(format!("{error}")),
                uia_session: None,
                uia_flows_json: None,
            }
        }
    }
}

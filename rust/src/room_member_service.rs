// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::collections::HashMap;
use std::sync::Arc;
use std::time::Duration;

use anyhow::{anyhow, Result};
use matrix_sdk::room::RoomMember;
use matrix_sdk::ruma::events::room::power_levels::RoomPowerLevels;
use matrix_sdk::ruma::events::SyncStateEvent;
use matrix_sdk::ruma::{Int, OwnedUserId};
use matrix_sdk::{Client, Room, RoomMemberships, RoomState};
use tokio::sync::RwLock;

use crate::presence_typing_service::PresenceTypingService;
use crate::types::{
    HistoryVisibility, MemberRole, MembershipState, RoomAccess, RoomMemberInfo,
    RoomMembersSnapshot, RoomNotificationMode, RoomSettingsSnapshot, UserProfile,
    UserProfileDetails,
};

pub(crate) struct RoomMemberService;

impl RoomMemberService {
    pub(crate) async fn get_room_members(room: Room, room_id: &str) -> Result<Vec<UserProfile>> {
        let members =
            tokio::time::timeout(Duration::from_secs(5), room.members(RoomMemberships::JOIN))
                .await
                .map_err(|_| anyhow!("Room members fetch timed out for {room_id}"))?
                .map_err(|e| anyhow!(e))?;
        Ok(members
            .into_iter()
            .map(|m| UserProfile {
                user_id: m.user_id().to_string(),
                display_name: m
                    .display_name()
                    .unwrap_or_else(|| m.user_id().localpart())
                    .to_string(),
                avatar_url: m.avatar_url().map(|u| u.to_string()),
            })
            .collect())
    }

    pub(crate) async fn get_room_settings_for_client(
        client: Client,
        room_id: &str,
        notification_overrides: Arc<RwLock<HashMap<String, RoomNotificationMode>>>,
    ) -> Result<RoomSettingsSnapshot> {
        let room_id_parsed = matrix_sdk::ruma::RoomId::parse(room_id)
            .map_err(|e| anyhow!("Invalid room ID: {e}"))?;
        let room = client
            .get_room(&room_id_parsed)
            .ok_or_else(|| anyhow!("Room not found"))?;
        Self::get_room_settings(room, room_id, notification_overrides).await
    }

    pub(crate) async fn get_room_settings(
        room: Room,
        room_id: &str,
        notification_overrides: Arc<RwLock<HashMap<String, RoomNotificationMode>>>,
    ) -> Result<RoomSettingsSnapshot> {
        let display_name = room
            .display_name()
            .await
            .map(|dn| dn.to_string())
            .unwrap_or_default();

        let canonical_alias = room.canonical_alias().map(|a| a.to_string());

        let notification_mode = {
            let overrides = notification_overrides.read().await;
            if let Some(&mode) = overrides.get(&room_id.to_string()) {
                mode
            } else {
                let sdk_mode = room.notification_mode().await;
                match sdk_mode {
                    Some(
                        matrix_sdk::notification_settings::RoomNotificationMode::MentionsAndKeywordsOnly,
                    ) => RoomNotificationMode::MentionsOnly,
                    Some(matrix_sdk::notification_settings::RoomNotificationMode::Mute) => {
                        RoomNotificationMode::Mute
                    }
                    _ => RoomNotificationMode::AllMessages,
                }
            }
        };
        let is_muted = notification_mode == RoomNotificationMode::Mute;

        let member_count = room.joined_members_count();

        let is_encrypted = room.encryption_state().is_encrypted();
        let encryption_algorithm = if is_encrypted {
            Some("m.megolm.v1.aes-sha2".to_string())
        } else {
            None
        };

        use matrix_sdk::ruma::events::room::history_visibility::HistoryVisibility as RumaHV;
        let history_visibility = match room.history_visibility() {
            Some(RumaHV::Joined) => HistoryVisibility::Joined,
            Some(RumaHV::Invited) => HistoryVisibility::Invited,
            Some(RumaHV::Shared) => HistoryVisibility::Shared,
            Some(RumaHV::WorldReadable) => HistoryVisibility::WorldReadable,
            _ => HistoryVisibility::Unknown,
        };

        let new_members_can_see_history = matches!(
            history_visibility,
            HistoryVisibility::Shared | HistoryVisibility::WorldReadable
        );

        let access = match room.join_rule() {
            Some(matrix_sdk::ruma::room::JoinRule::Public) => RoomAccess::Public,
            Some(matrix_sdk::ruma::room::JoinRule::Knock) => RoomAccess::Knock,
            Some(matrix_sdk::ruma::room::JoinRule::Restricted(_)) => RoomAccess::Restricted,
            Some(matrix_sdk::ruma::room::JoinRule::KnockRestricted(_)) => {
                RoomAccess::KnockRestricted
            }
            Some(matrix_sdk::ruma::room::JoinRule::Private) => RoomAccess::Private,
            Some(matrix_sdk::ruma::room::JoinRule::Invite) => RoomAccess::InviteOnly,
            _ => RoomAccess::Unknown,
        };

        let own_user_id = room.own_user_id();
        let power_levels = room.power_levels().await.ok();
        let can_invite = power_levels
            .as_ref()
            .map(|pls| pls.user_can_invite(own_user_id))
            .unwrap_or(false);
        let can_kick = power_levels
            .as_ref()
            .map(|pls| pls.user_can_kick(own_user_id))
            .unwrap_or(false);
        let can_ban = power_levels
            .as_ref()
            .map(|pls| pls.user_can_ban(own_user_id))
            .unwrap_or(false);
        let can_change_avatar = power_levels
            .as_ref()
            .map(|pls| {
                pls.user_can_send_state(
                    own_user_id,
                    matrix_sdk::ruma::events::StateEventType::RoomAvatar,
                )
            })
            .unwrap_or(false);
        let can_change_name = power_levels
            .as_ref()
            .map(|pls| {
                pls.user_can_send_state(
                    own_user_id,
                    matrix_sdk::ruma::events::StateEventType::RoomName,
                )
            })
            .unwrap_or(false);
        let can_change_topic = power_levels
            .as_ref()
            .map(|pls| {
                pls.user_can_send_state(
                    own_user_id,
                    matrix_sdk::ruma::events::StateEventType::RoomTopic,
                )
            })
            .unwrap_or(false);
        let can_change_encryption = power_levels
            .as_ref()
            .map(|pls| {
                pls.user_can_send_state(
                    own_user_id,
                    matrix_sdk::ruma::events::StateEventType::RoomEncryption,
                )
            })
            .unwrap_or(false);
        let can_change_access = power_levels
            .as_ref()
            .map(|pls| {
                pls.user_can_send_state(
                    own_user_id,
                    matrix_sdk::ruma::events::StateEventType::RoomJoinRules,
                )
            })
            .unwrap_or(false);
        let can_change_history_visibility = power_levels
            .as_ref()
            .map(|pls| {
                pls.user_can_send_state(
                    own_user_id,
                    matrix_sdk::ruma::events::StateEventType::RoomHistoryVisibility,
                )
            })
            .unwrap_or(false);

        Ok(RoomSettingsSnapshot {
            room_id: room_id.to_string(),
            display_name,
            canonical_alias,
            notification_mode,
            is_muted,
            member_count,
            is_encrypted,
            encryption_algorithm,
            access,
            history_visibility,
            new_members_can_see_history,
            can_invite,
            can_kick,
            can_ban,
            can_change_avatar,
            can_change_name,
            can_change_topic,
            can_change_encryption,
            can_change_access,
            can_change_history_visibility,
        })
    }

    pub(crate) async fn enable_room_encryption(room: Room) -> Result<()> {
        use matrix_sdk::ruma::events::room::encryption::RoomEncryptionEventContent;
        room.send_state_event(RoomEncryptionEventContent::with_recommended_defaults())
            .await
            .map_err(|e| anyhow!("Failed to enable encryption: {e}"))?;
        Ok(())
    }

    pub(crate) async fn set_room_access(room: Room, access: RoomAccess) -> Result<()> {
        use matrix_sdk::ruma::room::JoinRule;
        let rule = match access {
            RoomAccess::Public => JoinRule::Public,
            RoomAccess::Knock => JoinRule::Knock,
            RoomAccess::InviteOnly | RoomAccess::Private => JoinRule::Invite,
            RoomAccess::Restricted | RoomAccess::KnockRestricted => {
                return Err(anyhow!(
                    "Restricted room access requires allow rules and cannot be set directly"
                ));
            }
            RoomAccess::Unknown => return Err(anyhow!("Unknown room access cannot be set")),
        };
        room.privacy_settings()
            .update_join_rule(rule)
            .await
            .map_err(|e| anyhow!("Failed to update room access: {e}"))?;
        Ok(())
    }

    pub(crate) async fn set_room_history_visibility(
        room: Room,
        visibility: HistoryVisibility,
    ) -> Result<()> {
        use matrix_sdk::ruma::events::room::history_visibility::HistoryVisibility as RumaHV;
        let next = match visibility {
            HistoryVisibility::Joined => RumaHV::Joined,
            HistoryVisibility::Invited => RumaHV::Invited,
            HistoryVisibility::Shared => RumaHV::Shared,
            HistoryVisibility::WorldReadable => RumaHV::WorldReadable,
            HistoryVisibility::Unknown => {
                return Err(anyhow!("Unknown history visibility cannot be set"));
            }
        };
        room.privacy_settings()
            .update_room_history_visibility(next)
            .await
            .map_err(|e| anyhow!("Failed to update history visibility: {e}"))?;
        Ok(())
    }

    /// The member's display name, falling back to the name from their PREVIOUS
    /// membership (the current member event's `unsigned.prev_content`) when the
    /// current event carries none — e.g. a `leave`/`ban` event that dropped the
    /// displayname. This keeps a departed member showing the name they had while in
    /// the room (what Element shows) instead of collapsing to their MXID, and it is
    /// the only local source once their global `/profile` is gone (404). `None` when
    /// neither the current event nor prev_content has a non-empty name.
    pub(crate) fn member_display_name(member: &RoomMember) -> Option<String> {
        if let Some(name) = member.display_name() {
            if !name.is_empty() {
                return Some(name.to_string());
            }
        }
        let SyncStateEvent::Original(orig) = member.event().as_sync()? else {
            return None;
        };
        orig.unsigned
            .prev_content
            .as_ref()?
            .displayname
            .clone()
            .filter(|s| !s.is_empty())
    }

    pub(crate) async fn get_user_profile_details(
        client: Client,
        room: Room,
        room_id: &str,
        user_id: &str,
        presence_typing: &PresenceTypingService,
    ) -> Result<UserProfileDetails> {
        let uid: OwnedUserId = user_id
            .try_into()
            .map_err(|_| anyhow!("Invalid user ID: {user_id}"))?;

        let member = room
            .get_member(&uid)
            .await
            .map_err(|e| anyhow!(e))?
            .ok_or_else(|| anyhow!("User {user_id} not found in room {room_id}"))?;

        let display_name = Self::member_display_name(&member)
            .unwrap_or_else(|| member.user_id().localpart().to_string());
        let avatar_url = member.avatar_url().map(|u| u.to_string());
        let membership = Self::membership_state(member.membership());
        let power_level = Self::member_power_level(member.power_level());

        let my_uid = room.own_user_id();
        let my_member = room.get_member(my_uid).await.ok().flatten();
        let my_pl = my_member
            .as_ref()
            .map(|m| Self::member_power_level(m.power_level()))
            .unwrap_or(0);
        let is_self = uid == my_uid;
        let power_levels = room.power_levels().await.ok();
        let role = Self::derive_member_role(power_levels.as_ref(), &uid);
        let (presence, last_active_ts) = presence_typing.member_presence(user_id);

        let can_invite = !is_self
            && power_levels
                .as_ref()
                .map(|pls| pls.user_can_invite(my_uid))
                .unwrap_or(false);
        let can_kick = !is_self
            && power_levels
                .as_ref()
                .map(|pls| pls.user_can_kick_user(my_uid, &uid))
                .unwrap_or(false);
        let can_ban = !is_self
            && power_levels
                .as_ref()
                .map(|pls| pls.user_can_ban_user(my_uid, &uid))
                .unwrap_or(false);
        let can_mute = !is_self && my_pl > power_level;
        let can_change_power_level = power_levels
            .as_ref()
            .map(|pls| pls.user_can_change_user_power_level(my_uid, &uid))
            .unwrap_or(false);
        let max_assignable_power_level = if can_change_power_level && my_pl > i64::MIN {
            my_pl.saturating_sub(1)
        } else {
            -1
        };

        let is_ignored = client.is_user_ignored(&uid).await;
        let mut dm_room_id = None;
        for candidate in client.rooms() {
            if candidate.state() == RoomState::Left {
                continue;
            }
            if !candidate.is_direct().await.unwrap_or(false) {
                continue;
            }
            if candidate
                .direct_targets()
                .iter()
                .any(|target| *target == user_id)
            {
                dm_room_id = Some(candidate.room_id().to_string());
                break;
            }
        }

        Ok(UserProfileDetails {
            room_id: room_id.to_string(),
            user_id: user_id.to_string(),
            display_name,
            avatar_url,
            presence,
            last_active_ts,
            membership,
            power_level,
            role,
            is_ignored,
            dm_room_id,
            can_invite,
            can_kick,
            can_ban,
            can_mute,
            can_change_power_level,
            max_assignable_power_level,
        })
    }

    /// Profile from the GLOBAL `/profile/{user_id}` endpoint (name + avatar
    /// only), for when the room member isn't locally available — chiefly an
    /// unjoined room open in preview mode, where `get_user_profile_details`'s
    /// `room.get_member()` can't work but the timeline still shows the avatar
    /// from the peeked member state. Room-specific fields (membership, roles,
    /// moderation) are neutral defaults; none apply when we aren't joined.
    pub(crate) async fn get_user_profile_global(
        client: Client,
        room_id: &str,
        user_id: &str,
        presence_typing: &PresenceTypingService,
    ) -> Result<UserProfileDetails> {
        let uid: OwnedUserId = user_id
            .try_into()
            .map_err(|_| anyhow!("Invalid user ID: {user_id}"))?;

        let profile = client.account().fetch_user_profile_of(&uid).await.ok();
        let display_name = profile
            .as_ref()
            .and_then(|p| p.get("displayname"))
            .and_then(|v| v.as_str())
            .filter(|s| !s.is_empty())
            .map(str::to_string)
            .unwrap_or_else(|| uid.localpart().to_string());
        let avatar_url = profile
            .as_ref()
            .and_then(|p| p.get("avatar_url"))
            .and_then(|v| v.as_str())
            .filter(|s| !s.is_empty())
            .map(str::to_string);

        let (presence, last_active_ts) = presence_typing.member_presence(user_id);
        let is_ignored = client.is_user_ignored(&uid).await;

        Ok(UserProfileDetails {
            room_id: room_id.to_string(),
            user_id: user_id.to_string(),
            display_name,
            avatar_url,
            presence,
            last_active_ts,
            membership: MembershipState::Join,
            power_level: 0,
            role: MemberRole::User,
            is_ignored,
            dm_room_id: None,
            can_invite: false,
            can_kick: false,
            can_ban: false,
            can_mute: false,
            can_change_power_level: false,
            max_assignable_power_level: -1,
        })
    }

    pub(crate) async fn get_room_members_snapshot(
        room: Room,
        room_id: &str,
        force_refresh: bool,
    ) -> Result<RoomMembersSnapshot> {
        let my_uid = room.own_user_id().to_owned();

        let memberships = RoomMemberships::JOIN | RoomMemberships::INVITE | RoomMemberships::BAN;

        // Matrix has no server-side member pagination, so the SDK loads the whole
        // set at once. Serve the local state-store snapshot instantly and only hit
        // the server when explicitly refreshing or when nothing is cached yet — the
        // old unconditional 5s server fetch made large/cold rooms fail to empty.
        let raw_members = if force_refresh {
            tokio::time::timeout(Duration::from_secs(20), room.members(memberships))
                .await
                .map_err(|_| anyhow!("Room members refresh timed out for {room_id}"))?
                .map_err(|e| anyhow!(e))?
        } else {
            let cached = room
                .members_no_sync(memberships)
                .await
                .map_err(|e| anyhow!(e))?;
            if cached.is_empty() {
                tokio::time::timeout(Duration::from_secs(20), room.members(memberships))
                    .await
                    .map_err(|_| anyhow!("Room members fetch timed out for {room_id}"))?
                    .map_err(|e| anyhow!(e))?
            } else {
                cached
            }
        };

        let power_levels = room.power_levels().await.ok();
        let can_invite = power_levels
            .as_ref()
            .map(|pls| pls.user_can_invite(&my_uid))
            .unwrap_or(false);
        let mut can_remove_any = false;

        let mut members = Vec::with_capacity(raw_members.len());
        for m in &raw_members {
            let target_uid = m.user_id();
            let is_self = target_uid == my_uid;
            let target_pl = Self::member_power_level(m.power_level());
            let membership = Self::membership_state(m.membership());
            let role = Self::derive_member_role(power_levels.as_ref(), target_uid);

            let can_be_removed = !is_self
                && power_levels
                    .as_ref()
                    .map(|pls| pls.user_can_kick_user(&my_uid, target_uid))
                    .unwrap_or(false)
                && (membership == MembershipState::Join || membership == MembershipState::Invite);
            let can_be_banned = !is_self
                && membership != MembershipState::Ban
                && power_levels
                    .as_ref()
                    .map(|pls| pls.user_can_ban_user(&my_uid, target_uid))
                    .unwrap_or(false);
            let can_be_unbanned = membership == MembershipState::Ban
                && power_levels
                    .as_ref()
                    .map(|pls| pls.user_can_unban_user(&my_uid, target_uid))
                    .unwrap_or(false);

            if can_be_removed {
                can_remove_any = true;
            }

            members.push(RoomMemberInfo {
                user_id: target_uid.to_string(),
                display_name: m
                    .display_name()
                    .unwrap_or_else(|| m.user_id().localpart())
                    .to_string(),
                avatar_url: m.avatar_url().map(|u| u.to_string()),
                membership,
                power_level: target_pl,
                role,
                is_self,
                can_be_removed_by_me: can_be_removed,
                can_be_banned_by_me: can_be_banned,
                can_be_unbanned_by_me: can_be_unbanned,
            });
        }

        // Admins/mods first, then alphabetical by display name.
        Self::sort_members_for_display(&mut members);

        Ok(RoomMembersSnapshot {
            room_id: room_id.to_string(),
            my_user_id: my_uid.to_string(),
            can_invite,
            can_remove_any,
            members,
        })
    }

    pub(crate) async fn set_user_power_level(
        room: Room,
        user_id: &str,
        power_level: i64,
    ) -> Result<()> {
        let uid: OwnedUserId = user_id
            .try_into()
            .map_err(|_| anyhow!("Invalid user ID: {user_id}"))?;
        let new_level = Int::try_from(power_level)
            .map_err(|_| anyhow!("Invalid power level: {power_level}"))?;

        let power_levels = room.power_levels().await?;
        let my_uid = room.own_user_id();
        if !power_levels.user_can_change_user_power_level(my_uid, &uid) {
            return Err(anyhow!(
                "You don't have permission to change this user's power level"
            ));
        }

        let my_power_level = match power_levels.for_user(my_uid) {
            matrix_sdk::ruma::events::room::power_levels::UserPowerLevel::Infinite => i64::MAX,
            matrix_sdk::ruma::events::room::power_levels::UserPowerLevel::Int(v) => i64::from(v),
            _ => 0,
        };
        if power_level >= my_power_level {
            return Err(anyhow!(
                "Power level must be lower than your own power level"
            ));
        }

        room.update_power_levels(vec![(&uid, new_level)])
            .await
            .map_err(|e| anyhow!("Failed to update power level: {e}"))?;
        Ok(())
    }

    pub(crate) async fn create_direct_room(client: Client, user_id: &str) -> Result<String> {
        let uid: OwnedUserId = user_id
            .try_into()
            .map_err(|_| anyhow!("Invalid user ID: {user_id}"))?;

        if let Some(room) = client.get_dm_room(&uid) {
            return Ok(room.room_id().to_string());
        }

        let room = client
            .create_dm(&uid)
            .await
            .map_err(|e| anyhow!("Failed to create direct room: {e}"))?;
        Ok(room.room_id().to_string())
    }

    pub(crate) async fn kick_user(room: Room, user_id: &str, reason: Option<&str>) -> Result<()> {
        let uid: OwnedUserId = user_id
            .try_into()
            .map_err(|_| anyhow!("Invalid user ID: {user_id}"))?;
        room.kick_user(&uid, reason).await.map_err(|e| anyhow!(e))
    }

    pub(crate) async fn ban_user(room: Room, user_id: &str, reason: Option<&str>) -> Result<()> {
        let uid: OwnedUserId = user_id
            .try_into()
            .map_err(|_| anyhow!("Invalid user ID: {user_id}"))?;
        room.ban_user(&uid, reason).await.map_err(|e| anyhow!(e))
    }

    pub(crate) async fn unban_user(room: Room, user_id: &str) -> Result<()> {
        let uid: OwnedUserId = user_id
            .try_into()
            .map_err(|_| anyhow!("Invalid user ID: {user_id}"))?;
        room.unban_user(&uid, None).await.map_err(|e| anyhow!(e))
    }

    pub(crate) async fn invite_user(room: Room, user_id: &str) -> Result<()> {
        let uid: OwnedUserId = user_id
            .try_into()
            .map_err(|_| anyhow!("Invalid user ID: {user_id}"))?;
        room.invite_user_by_id(&uid).await.map_err(|e| anyhow!(e))
    }

    /// Returns `(results, limited)`; `limited` is true when the homeserver
    /// capped the results and more matches exist (the directory endpoint has no
    /// offset pagination, so callers should prompt the user to narrow the query).
    pub(crate) async fn search_user_directory(
        client: Client,
        query: &str,
        limit: u64,
    ) -> Result<(Vec<UserProfile>, bool)> {
        let search_term = query.trim();
        if search_term.is_empty() {
            return Ok((Vec::new(), false));
        }

        let own_user_id = client.user_id().map(|id| id.to_owned());
        let response = client
            .search_users(search_term, limit)
            .await
            .map_err(|e| anyhow!("Failed to search user directory: {e}"))?;

        let limited = response.limited;
        let results = response
            .results
            .into_iter()
            .filter(|user| Some(user.user_id.as_ref()) != own_user_id.as_deref())
            .map(|user| UserProfile {
                user_id: user.user_id.to_string(),
                display_name: user
                    .display_name
                    .unwrap_or_else(|| user.user_id.localpart().to_string()),
                avatar_url: user.avatar_url.map(|url| url.to_string()),
            })
            .collect();
        Ok((results, limited))
    }

    pub(crate) async fn set_user_ignored(
        client: Client,
        user_id: &str,
        ignored: bool,
    ) -> Result<()> {
        let uid: OwnedUserId = user_id
            .try_into()
            .map_err(|_| anyhow!("Invalid user ID: {user_id}"))?;
        if ignored {
            client
                .account()
                .ignore_user(&uid)
                .await
                .map_err(|e| anyhow!(e))
        } else {
            client
                .account()
                .unignore_user(&uid)
                .await
                .map_err(|e| anyhow!(e))
        }
    }

    fn derive_member_role(
        power_levels: Option<&RoomPowerLevels>,
        user_id: &matrix_sdk::ruma::UserId,
    ) -> MemberRole {
        use matrix_sdk::ruma::events::StateEventType;

        let Some(power_levels) = power_levels else {
            return MemberRole::User;
        };

        if power_levels.user_can_send_state(user_id, StateEventType::RoomPowerLevels) {
            MemberRole::Administrator
        } else if power_levels.user_can_ban(user_id) || power_levels.user_can_kick(user_id) {
            MemberRole::Moderator
        } else {
            MemberRole::User
        }
    }

    fn membership_state(
        membership: &matrix_sdk::ruma::events::room::member::MembershipState,
    ) -> MembershipState {
        match membership {
            matrix_sdk::ruma::events::room::member::MembershipState::Join => MembershipState::Join,
            matrix_sdk::ruma::events::room::member::MembershipState::Invite => {
                MembershipState::Invite
            }
            matrix_sdk::ruma::events::room::member::MembershipState::Leave => {
                MembershipState::Leave
            }
            matrix_sdk::ruma::events::room::member::MembershipState::Ban => MembershipState::Ban,
            matrix_sdk::ruma::events::room::member::MembershipState::Knock => {
                MembershipState::Knock
            }
            _ => MembershipState::Leave,
        }
    }

    fn member_power_level(
        power_level: matrix_sdk::ruma::events::room::power_levels::UserPowerLevel,
    ) -> i64 {
        match power_level {
            matrix_sdk::ruma::events::room::power_levels::UserPowerLevel::Infinite => i64::MAX,
            matrix_sdk::ruma::events::room::power_levels::UserPowerLevel::Int(v) => i64::from(v),
            _ => 0,
        }
    }

    /// Display order: admins/mods first (power level desc), then alphabetical by
    /// display name (case-insensitive).
    fn sort_members_for_display(members: &mut [RoomMemberInfo]) {
        members.sort_by(|a, b| {
            b.power_level.cmp(&a.power_level).then_with(|| {
                a.display_name
                    .to_lowercase()
                    .cmp(&b.display_name.to_lowercase())
            })
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn member(name: &str, power: i64) -> RoomMemberInfo {
        RoomMemberInfo {
            user_id: format!("@{}:s", name.to_lowercase()),
            display_name: name.to_string(),
            avatar_url: None,
            membership: MembershipState::Join,
            power_level: power,
            role: MemberRole::User,
            is_self: false,
            can_be_removed_by_me: false,
            can_be_banned_by_me: false,
            can_be_unbanned_by_me: false,
        }
    }

    #[test]
    fn sort_orders_by_power_then_name_case_insensitive() {
        let mut members = vec![
            member("Zoe", 0),
            member("Bob", 100),
            member("alice", 0),
            member("dave", 100),
        ];
        RoomMemberService::sort_members_for_display(&mut members);
        let order: Vec<&str> = members.iter().map(|m| m.display_name.as_str()).collect();
        // Power 100 first (Bob, dave), then power 0 alphabetically ignoring case
        // (alice before Zoe, which raw byte order would reverse).
        assert_eq!(order, vec!["Bob", "dave", "alice", "Zoe"]);
    }
}

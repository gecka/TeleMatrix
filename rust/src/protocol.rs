// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use anyhow::Result;
use async_trait::async_trait;

type PresenceChangedCallback = Box<dyn Fn(&str, u32, u64) + Send>;

use crate::encryption_service::RecoverySetupError;
use crate::types::{
    AccountActionResult, AccountSummary, CreateRoomRequest, DeleteDevicesResult, DeviceSessionList,
    EncryptionOverview, FolderMeta, HistoryVisibility, ImportKeysResult, MessageContent,
    RegistrationRequest, RegistrationResult, ResetIdentityResult, RoomAccess, RoomDirectoryPage,
    RoomDirectoryRequest, RoomMembersSnapshot, RoomNotificationMode, RoomPreviewInfo,
    RoomSettingsSnapshot, RoomSummary, SearchPage, SearchRequest, SpaceHierarchyRequest, ThreePid,
    ThreePidMedium, ThreePidTokenResponse, TimelineItem, TimelineSlice, UserProfile,
    UserProfileDetails, UsernameAvailability, VerificationCapabilities,
};

/// The core protocol trait that both mock and future Matrix implementations must satisfy.
#[async_trait]
pub trait ProtocolClient: Send + Sync {
    /// Authenticate with a homeserver. Returns the logged-in user's profile.
    async fn login(&self, homeserver: &str, user: &str, pass: &str) -> Result<UserProfile>;

    /// Get the list of rooms the user has joined.
    async fn get_rooms(&self) -> Result<Vec<RoomSummary>>;

    /// Get the list of members in a room.
    async fn get_room_members(&self, room_id: &str) -> Result<Vec<UserProfile>>;

    /// Send a text message to a room. Returns the event ID.
    async fn send_message(
        &self,
        room_id: &str,
        body: &str,
        formatted_body: Option<&str>,
        reply_to_event_id: Option<&str>,
    ) -> Result<String>;

    /// Edit an existing message. Returns the edited event ID. When `as_media_caption` is set the
    /// target is a media message and only its caption is changed (an empty body clears it), so the
    /// file is preserved instead of being replaced by a text message.
    async fn edit_message(
        &self,
        room_id: &str,
        event_id: &str,
        body: &str,
        formatted_body: Option<&str>,
        as_media_caption: bool,
    ) -> Result<String>;

    /// Delete a message.
    async fn delete_message(&self, room_id: &str, event_id: &str) -> Result<()>;

    /// Pin or unpin a message.
    async fn pin_message(&self, room_id: &str, event_id: &str, pinned: bool) -> Result<()>;

    /// Fetch pinned messages for a room (content from server).
    async fn get_pinned_messages(&self, room_id: &str) -> Result<Vec<TimelineItem>>;

    /// Persist a locally-learned audio duration (ms) for a media mxc URL, so the
    /// bubble can show the length on later loads without re-probing/playing.
    async fn set_audio_duration(&self, mxc: &str, duration_ms: u64) -> Result<()>;

    /// Unpin all messages in a room (single state event, no race condition).
    async fn unpin_all_messages(&self, room_id: &str) -> Result<()>;

    /// Pin or unpin a room in dialogs list.
    /// Pin/unpin a room. `order` is its position among the pinned rooms, recorded in
    /// the `m.favourite` tag so the arrangement outlives this device.
    async fn pin_room(&self, room_id: &str, pinned: bool, order: Option<f64>) -> Result<()>;

    /// Rewrite the order of every pinned room's tag. `room_ids` is top-first.
    async fn set_pinned_order(&self, room_ids: Vec<String>) -> Result<()>;

    /// Set explicit notification mode for a room.
    async fn set_room_notification_mode(
        &self,
        room_id: &str,
        mode: RoomNotificationMode,
    ) -> Result<()>;

    /// Read the account-global "Notifications for chats" settings (DM/room default
    /// levels + keyword rules).
    async fn get_notification_settings(
        &self,
    ) -> Result<crate::notification_settings_service::NotificationSettings>;

    /// Set a chat category's default notification level (AllMessages / MentionsOnly).
    async fn set_category_notification_level(
        &self,
        category: crate::notification_settings_service::ChatCategory,
        level: RoomNotificationMode,
    ) -> Result<()>;

    /// Reconcile the user's keyword rules to the comma-separated `csv`.
    async fn set_keywords_setting(&self, csv: &str) -> Result<()>;

    /// Toggle a mention/keyword master switch (display name / username / @room / keywords).
    async fn set_notification_toggle(
        &self,
        toggle: crate::notification_settings_service::NotificationToggle,
        enabled: bool,
    ) -> Result<()>;

    /// Mark room as read/unread.
    async fn mark_room_read(&self, room_id: &str, read: bool) -> Result<()>;

    /// Send a read receipt for a specific event in a room.
    async fn send_read_receipt(&self, room_id: &str, event_id: &str) -> Result<()>;

    /// Create a new room. Returns the created room ID.
    async fn create_room(&self, request: CreateRoomRequest) -> Result<String>;

    /// Upload room avatar data and send m.room.avatar. Returns the new mxc:// URL.
    async fn upload_room_avatar(
        &self,
        _room_id: &str,
        _data: Vec<u8>,
        _content_type: &str,
    ) -> Result<String> {
        Err(anyhow::anyhow!("upload_room_avatar not supported"))
    }

    /// Remove the room avatar by sending an empty m.room.avatar state event.
    async fn delete_room_avatar(&self, _room_id: &str) -> Result<()> {
        Err(anyhow::anyhow!("delete_room_avatar not supported"))
    }

    /// Set the room name by sending an m.room.name state event.
    async fn set_room_name(&self, _room_id: &str, _name: &str) -> Result<()> {
        Err(anyhow::anyhow!("set_room_name not supported"))
    }

    /// Set the room topic by sending an m.room.topic state event.
    async fn set_room_topic(&self, _room_id: &str, _topic: &str) -> Result<()> {
        Err(anyhow::anyhow!("set_room_topic not supported"))
    }

    /// Leave (remove) a room.
    async fn leave_room(&self, room_id: &str) -> Result<()>;

    /// Drop a no-longer-viewed room's resident timeline state (window, caches)
    /// to bound memory across a long multi-room session. Rooms-list state and
    /// notifications are unaffected; re-opening rebuilds the timeline. Best
    /// effort — no error surface.
    async fn release_room_timeline(&self, _room_id: &str) {}

    /// Accept a room invite (join the room).
    async fn accept_invite(&self, room_id: &str) -> Result<()>;

    /// Search the homeserver's public room directory by name/topic/alias.
    async fn search_public_rooms(
        &self,
        _request: RoomDirectoryRequest,
    ) -> Result<RoomDirectoryPage> {
        Err(anyhow::anyhow!("search_public_rooms not supported"))
    }

    /// One page of a space's immediate children.
    async fn space_children(&self, _request: SpaceHierarchyRequest) -> Result<RoomDirectoryPage> {
        Err(anyhow::anyhow!("space_children not supported"))
    }

    /// What can be shown about a room before joining it.
    async fn room_preview(
        &self,
        _room_id_or_alias: &str,
        _via: Vec<String>,
    ) -> Result<RoomPreviewInfo> {
        Err(anyhow::anyhow!("room_preview not supported"))
    }

    /// Read a page of history for an unjoined room, paginating backward. `from` is `None` for the
    /// newest page. Only succeeds for world-readable rooms. Returns the items plus the token for the
    /// next-older page (`None` at the start of history).
    async fn preview_messages(
        &self,
        _room_id: &str,
        _from: Option<String>,
        _limit: u32,
    ) -> Result<(Vec<TimelineItem>, Option<String>)> {
        Err(anyhow::anyhow!("preview_messages not supported"))
    }

    /// Join a room by ID or alias. Returns the resolved room ID.
    async fn join_room(&self, _room_id_or_alias: &str, _via: Vec<String>) -> Result<String> {
        Err(anyhow::anyhow!("join_room not supported"))
    }

    /// Knock on a room (request to join). Returns the resolved room ID.
    async fn knock_room(&self, _room_id_or_alias: &str, _via: Vec<String>) -> Result<String> {
        Err(anyhow::anyhow!("knock_room not supported"))
    }

    /// Toggle a room's membership in a folder section (a `u.*` tag).
    async fn add_room_to_folder(&self, room_id: &str, tag_key: &str) -> Result<()>;

    /// Create a new empty folder from a name. Returns its tag key.
    async fn create_folder(&self, name: &str) -> Result<String>;

    /// Rename a folder (re-tags every member room). Returns the new tag key.
    async fn edit_folder(&self, tag_key: &str, name: &str) -> Result<String>;

    /// Delete a folder: strip its tag from all rooms and drop it from the order.
    async fn delete_folder(&self, tag_key: &str) -> Result<()>;

    /// Replace the unified sidebar order (folders + spaces).
    async fn set_sidebar_order(&self, order: Vec<crate::room_folders::SidebarRef>) -> Result<()>;

    /// Get the list of custom folders (native `u.*` sections).
    async fn get_folders(&self) -> Result<Vec<FolderMeta>>;

    /// Get the unified sidebar order (folders + spaces).
    async fn get_sidebar_order(&self) -> Result<Vec<crate::room_folders::SidebarRef>>;

    /// Enumerate the joined Matrix spaces (rendered as folder-like tabs).
    async fn get_joined_spaces(&self) -> Result<Vec<crate::types::SpaceInfo>>;

    /// Forward a message to another room. Returns the new event ID in the destination room.
    async fn forward_message(
        &self,
        src_room_id: &str,
        event_id: &str,
        dst_room_id: &str,
    ) -> Result<String>;

    /// Send a media message. Returns the local transaction ID.
    async fn send_media(
        &self,
        room_id: &str,
        content: MessageContent,
        transaction_id: Option<String>,
    ) -> Result<String>;

    /// Set or unset a reaction on a message.
    async fn set_reaction(
        &self,
        room_id: &str,
        event_id: &str,
        key: &str,
        active: bool,
    ) -> Result<()>;

    /// Send a poll response with one or more selected option IDs.
    async fn send_poll_vote(
        &self,
        room_id: &str,
        poll_event_id: &str,
        option_ids: Vec<String>,
    ) -> Result<String>;

    /// Get a snapshot of a room's settings and security state.
    async fn get_room_settings(&self, room_id: &str) -> Result<RoomSettingsSnapshot>;

    /// Enable end-to-end encryption for a room.
    /// This is a one-way operation — once enabled, encryption cannot be disabled.
    async fn enable_room_encryption(&self, room_id: &str) -> Result<()>;

    /// Update room access / join rule.
    async fn set_room_access(&self, room_id: &str, access: RoomAccess) -> Result<()>;

    /// Update room history visibility.
    async fn set_room_history_visibility(
        &self,
        room_id: &str,
        visibility: HistoryVisibility,
    ) -> Result<()>;

    /// Get a detailed member snapshot with permission flags. When `force_refresh`
    /// is false the local state-store snapshot is served instantly; when true the
    /// full member set is fetched from the server.
    async fn get_room_members_snapshot(
        &self,
        room_id: &str,
        force_refresh: bool,
    ) -> Result<RoomMembersSnapshot>;

    /// Search messages in one room or across all rooms.
    async fn search_messages(&self, request: SearchRequest) -> Result<SearchPage>;

    /// Start SAS emoji verification. Initiate-only: success returns the started
    /// flow's id, and the emojis arrive through the SAS-emojis callback.
    async fn start_sas_verification(&self) -> Result<String> {
        Err(anyhow::anyhow!("SAS verification not supported"))
    }

    /// Confirm that the SAS emojis match on both sides.
    async fn confirm_sas_match(&self) -> Result<()> {
        Err(anyhow::anyhow!("SAS verification not supported"))
    }

    /// Show a QR code for device verification. Initiate-only: success returns
    /// the started flow's id and the module grid arrives through the QR-data
    /// callback.
    async fn start_qr_verification(&self) -> Result<String> {
        Err(anyhow::anyhow!("QR verification not supported"))
    }

    /// Confirm the other device scanned our QR code.
    async fn confirm_qr_scanned(&self) -> Result<()> {
        Err(anyhow::anyhow!("QR verification not supported"))
    }

    /// Verify the session using a recovery key string.
    async fn verify_with_recovery_key(&self, _key: &str) -> Result<()> {
        Err(anyhow::anyhow!("Recovery key verification not supported"))
    }

    /// Cancel an active verification flow.
    async fn cancel_verification(&self) -> Result<()> {
        Err(anyhow::anyhow!("Cancel verification not supported"))
    }

    /// Reject SAS verification due to an emoji mismatch (sends `MismatchedSas`).
    async fn mismatch_sas(&self) -> Result<()> {
        Err(anyhow::anyhow!("SAS verification not supported"))
    }

    /// Skip verification — cancel any in-flight request and mark as done.
    async fn skip_verification(&self) -> Result<()> {
        Ok(())
    }

    /// Get verification capabilities (what methods are available).
    async fn get_verification_capabilities(&self) -> Result<VerificationCapabilities> {
        Ok(VerificationCapabilities {
            can_verify_with_device: false,
            can_verify_with_recovery: false,
            sas_supported: false,
            qr_supported: false,
        })
    }

    /// Get detailed user profile for the profile popup.
    async fn get_user_profile_details(
        &self,
        room_id: &str,
        user_id: &str,
    ) -> Result<UserProfileDetails>;

    /// Set a room member's power level.
    async fn set_user_power_level(
        &self,
        _room_id: &str,
        _user_id: &str,
        _power_level: i64,
    ) -> Result<()> {
        Err(anyhow::anyhow!("set_user_power_level not supported"))
    }

    /// Open or create a direct-message room with a user.
    async fn create_direct_room(&self, _user_id: &str) -> Result<String> {
        Err(anyhow::anyhow!("create_direct_room not supported"))
    }

    /// Resolve the Saved Messages room. With `create` set (an explicit forward
    /// / open) it creates + mutes the room on first use; without it (a passive
    /// session start) it only adopts an already-existing room and never
    /// creates. Returns the room id, or None when there is no saved room.
    async fn ensure_saved_messages_room(&self, _create: bool) -> Result<Option<String>> {
        Err(anyhow::anyhow!("ensure_saved_messages_room not supported"))
    }

    /// Permanently delete Saved Messages: clear the marker and leave + forget
    /// the room. Returns the id it held, or None when there was no saved room.
    async fn delete_saved_messages_room(&self) -> Result<Option<String>> {
        Err(anyhow::anyhow!("delete_saved_messages_room not supported"))
    }

    /// Kick a user from a room.
    async fn kick_user(&self, _room_id: &str, _user_id: &str, _reason: Option<&str>) -> Result<()> {
        Err(anyhow::anyhow!("kick_user not supported"))
    }

    /// Ban a user from a room.
    async fn ban_user(&self, _room_id: &str, _user_id: &str, _reason: Option<&str>) -> Result<()> {
        Err(anyhow::anyhow!("ban_user not supported"))
    }

    /// Unban a user from a room.
    async fn unban_user(&self, _room_id: &str, _user_id: &str) -> Result<()> {
        Err(anyhow::anyhow!("unban_user not supported"))
    }

    /// Invite a user to a room.
    async fn invite_user(&self, _room_id: &str, _user_id: &str) -> Result<()> {
        Err(anyhow::anyhow!("invite_user not supported"))
    }

    /// Search the homeserver user directory. Returns `(results, limited)` where
    /// `limited` is true when the server capped the results.
    async fn search_user_directory(
        &self,
        _query: &str,
        _limit: u64,
    ) -> Result<(Vec<UserProfile>, bool)> {
        Err(anyhow::anyhow!("search_user_directory not supported"))
    }

    /// Set/unset a user as ignored.
    async fn set_user_ignored(&self, _user_id: &str, _ignored: bool) -> Result<()> {
        Err(anyhow::anyhow!("set_user_ignored not supported"))
    }

    /// Register a new account. Returns success (with auto-login) or a UIA challenge.
    async fn register(&self, _request: RegistrationRequest) -> Result<RegistrationResult> {
        Err(anyhow::anyhow!("Registration not supported"))
    }

    /// Submit UIA auth data for a pending registration.
    async fn submit_registration_auth(
        &self,
        _request: RegistrationRequest,
    ) -> Result<RegistrationResult> {
        Err(anyhow::anyhow!("Registration not supported"))
    }

    /// Check if a username is available on the homeserver.
    async fn check_username_available(
        &self,
        _homeserver: &str,
        _username: &str,
    ) -> Result<UsernameAvailability> {
        Err(anyhow::anyhow!("Username check not supported"))
    }
    // --- Account settings ---

    /// Get a combined account summary (profile + capabilities).
    async fn get_account_summary(&self) -> Result<AccountSummary>;

    /// Set the user's display name. Pass empty string to clear.
    async fn set_display_name(&self, name: &str) -> Result<()>;

    /// Set the user's avatar URL. Pass None to remove.
    async fn set_avatar_url(&self, mxc_url: Option<&str>) -> Result<()>;

    /// Upload avatar data and set it. Returns the new mxc:// URL.
    async fn upload_avatar(&self, data: Vec<u8>, content_type: &str) -> Result<String>;

    /// Get the list of 3PIDs bound to the account.
    async fn get_3pids(&self) -> Result<Vec<ThreePid>>;

    /// Request a verification token for adding a 3PID.
    async fn request_3pid_token(
        &self,
        medium: ThreePidMedium,
        address: &str,
        country: &str,
        client_secret: &str,
        send_attempt: u32,
    ) -> Result<ThreePidTokenResponse>;

    /// Add a 3PID after verification. Returns UIA result.
    async fn add_3pid(
        &self,
        client_secret: &str,
        sid: &str,
        auth_json: Option<&str>,
    ) -> Result<AccountActionResult>;

    /// Remove a 3PID from the account.
    async fn delete_3pid(&self, medium: ThreePidMedium, address: &str) -> Result<()>;

    /// Change the account password. Returns UIA result.
    async fn change_password(
        &self,
        new_password: &str,
        auth_json: Option<&str>,
    ) -> Result<AccountActionResult>;

    /// Deactivate the account. Returns UIA result.
    async fn deactivate_account(
        &self,
        erase_data: bool,
        auth_json: Option<&str>,
    ) -> Result<AccountActionResult>;

    /// Register a callback that fires when the room list changes.
    fn on_room_list_changed(&self, callback: Box<dyn Fn() + Send>);

    /// Register a callback that fires when a room's timeline changes.
    fn on_timeline_changed(&self, room_id: &str, callback: Box<dyn Fn() + Send>);

    /// Get the current timeline slice with pagination metadata.
    async fn get_timeline_slice(&self, room_id: &str) -> Result<TimelineSlice>;

    /// Get the latest incremental timeline update, falling back to a full slice.
    async fn get_timeline_update(&self, room_id: &str) -> Result<TimelineSlice>;

    /// Trigger backward pagination (non-blocking, spawns on Tokio).
    fn paginate_back(&self, room_id: &str, count: u16);

    /// Trigger forward pagination (Event mode only, non-blocking).
    fn paginate_forward(&self, room_id: &str, count: u16);

    /// Switch to focused timeline centered on event_id (async).
    async fn focus_on_event(&self, room_id: &str, event_id: &str) -> Result<()>;

    /// Switch back to live timeline.
    fn return_to_live(&self, room_id: &str);

    /// Cancel an in-progress media upload by transaction ID.
    async fn cancel_upload(&self, room_id: &str, transaction_id: &str) -> Result<()>;

    /// Register a callback that fires when a user's presence changes.
    /// Arguments: user_id, state (0=offline, 1=online, 2=unavailable), last_active epoch secs.
    fn on_presence_changed(&self, _cb: PresenceChangedCallback) {}

    // --- Sessions + Encryption ---

    /// Get the list of devices/sessions for the current user.
    async fn get_own_devices(&self) -> Result<DeviceSessionList> {
        Err(anyhow::anyhow!("get_own_devices not supported"))
    }

    /// Rename a device's display name.
    async fn rename_device(&self, _device_id: &str, _display_name: &str) -> Result<()> {
        Err(anyhow::anyhow!("rename_device not supported"))
    }

    /// Delete one or more devices. `auth_json` is the serialized UIA response
    /// (empty string for first attempt). Returns challenge if UIA is required.
    async fn delete_devices(
        &self,
        _device_ids: &[String],
        _auth_json: &str,
    ) -> Result<DeleteDevicesResult> {
        Err(anyhow::anyhow!("delete_devices not supported"))
    }

    /// Get the current encryption overview/health snapshot.
    async fn get_encryption_overview(&self) -> Result<EncryptionOverview> {
        Err(anyhow::anyhow!("get_encryption_overview not supported"))
    }

    /// Enable or disable key storage (key backup).
    async fn set_key_storage_enabled(&self, _enabled: bool) -> Result<()> {
        Err(anyhow::anyhow!("set_key_storage_enabled not supported"))
    }

    /// Provision recovery on an account that has none. Returns the new recovery key.
    async fn setup_recovery(&self) -> std::result::Result<String, RecoverySetupError> {
        Err(RecoverySetupError::Other(
            "setup_recovery not supported".to_owned(),
        ))
    }

    /// Replace an unusable server-side key backup with a fresh one. Returns the new recovery key.
    async fn reset_recovery(&self) -> std::result::Result<String, RecoverySetupError> {
        Err(RecoverySetupError::Other(
            "reset_recovery not supported".to_owned(),
        ))
    }

    /// Submit a recovery key to unlock secret storage.
    async fn enter_recovery_key(&self, _recovery_key: &str) -> Result<()> {
        Err(anyhow::anyhow!("enter_recovery_key not supported"))
    }

    /// Generate a new recovery key. Returns the generated key string.
    async fn create_recovery_key(&self) -> Result<String> {
        Err(anyhow::anyhow!("create_recovery_key not supported"))
    }

    /// Commit (finalize) a new recovery key after user confirmation.
    async fn commit_recovery_key(&self, _recovery_key: &str) -> Result<()> {
        Err(anyhow::anyhow!("commit_recovery_key not supported"))
    }

    /// Reset the cryptographic identity. `auth_json` is UIA response if needed.
    async fn reset_identity(&self, _auth_json: &str) -> Result<ResetIdentityResult> {
        Err(anyhow::anyhow!("reset_identity not supported"))
    }

    /// Export E2E encryption keys to a file encrypted with passphrase.
    async fn export_e2e_keys(&self, _path: &str, _passphrase: &str) -> Result<()> {
        Err(anyhow::anyhow!("export_e2e_keys not supported"))
    }

    /// Import E2E encryption keys from a file.
    async fn import_e2e_keys(&self, _path: &str, _passphrase: &str) -> Result<ImportKeysResult> {
        Err(anyhow::anyhow!("import_e2e_keys not supported"))
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

// matrix-sdk's deeply-nested async futures overflow rustc's default
// recursion_limit (128) when computing the layout of our sync()/timeline
// futures. matrix-sdk sets 256 internally; mirror it here for our own crate.
#![recursion_limit = "256"]

#[cfg(test)]
mod integration_tests;

mod account_service;
pub mod app_cache_store;
mod audio_duration_store;
mod auth_service;
pub mod cache_manager;
mod container_store;
pub mod encrypted_sqlite;
mod encryption_service;
pub mod ffi;
mod folder_service;
pub mod keychain;
pub mod link_preview_rules;
mod local_cache_service;
mod log_noise;
pub mod matrix;
pub mod media_blob_store;
mod media_cache_service;
pub mod media_stream;
mod media_transfer_service;
mod message_action_service;
mod new_login_service;
mod notification_service;
mod notification_settings_service;
mod presence_typing_service;
mod preview_fetch_signal;
mod preview_service;
pub mod preview_store;
mod profile_cache_store;
pub mod protocol;
mod recent_emoji;
mod recent_emoji_service;
mod room_action_service;
mod room_creation_service;
mod room_directory_service;
mod room_folders;
mod room_invite_service;
mod room_list_service;
mod room_member_service;
mod room_summary_service;
mod runtime_stats;
mod saved_messages;
mod saved_messages_service;
pub mod search_backfill;
pub mod search_index;
mod search_service;
mod secret_vault;
mod session_invalidation;
mod session_lifecycle_service;
mod session_storage_service;
mod session_task_service;
mod sliding_sync_service;
mod store_guard;
mod sync_loop_service;
mod timeline_cache_service;
mod timeline_conversion_service;
mod timeline_navigation_service;
mod timeline_service;
mod timeline_update_service;
pub mod timeline_window;
mod timeline_window_service;
mod trash;
pub mod types;
mod unread_count_service;
pub mod update_service;
mod upload_limit;
mod upload_progress;
mod upload_seed_store;
mod upload_tasks;
mod verification_service;
mod video_thumbnail_service;

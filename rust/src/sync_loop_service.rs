// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::collections::HashMap;
use std::sync::atomic::AtomicU32;
use std::sync::{Arc, Mutex};

use tokio::sync::RwLock;

use crate::folder_service::FolderService;
use crate::local_cache_service::LocalCacheService;
use crate::presence_typing_service::PresenceTypingService;
use crate::session_task_service::SessionTaskService;
use crate::timeline_window_service::TimelineRuntime;
use crate::types::{RoomNotificationMode, RoomSummary, TimelineItem};
use crate::verification_service::VerificationService;

type TimelineCache = Arc<RwLock<HashMap<String, Vec<TimelineItem>>>>;
type RoomsCache = Arc<RwLock<Vec<RoomSummary>>>;
type NotificationOverrides = Arc<RwLock<HashMap<String, RoomNotificationMode>>>;
type RoomListCallback = Arc<Mutex<Option<Box<dyn Fn() + Send>>>>;
type SyncStateCallback = Arc<Mutex<Option<Box<dyn Fn(u32) + Send>>>>;
type SearchIndex = Arc<std::sync::Mutex<Option<crate::search_index::SearchIndex>>>;

/// Shared session-scoped handles plumbed into the sync backend. The sliding
/// backend reads what it needs (`crate::sliding_sync_service`); some fields are
/// retained for backends/features that consume them.
#[derive(Clone)]
#[allow(dead_code)]
pub(crate) struct SyncLoopRuntime {
    pub(crate) session_tasks: SessionTaskService,
    pub(crate) verification: VerificationService,
    pub(crate) presence_typing: PresenceTypingService,
    pub(crate) rooms_cache: RoomsCache,
    pub(crate) room_list_callback: RoomListCallback,
    pub(crate) notification_overrides: NotificationOverrides,
    pub(crate) timeline_cache: TimelineCache,
    pub(crate) timeline_runtime: TimelineRuntime,
    pub(crate) sync_state: Arc<AtomicU32>,
    pub(crate) sync_state_callback: SyncStateCallback,
    pub(crate) folders: FolderService,
    pub(crate) local_cache: LocalCacheService,
    pub(crate) search_index: SearchIndex,
}

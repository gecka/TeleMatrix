// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! The on-open key prefetch must not burn its one claim per room while the key
//! backup is still unusable.
//!
//! Opening a room claims it in `backup_prefetched_rooms` and spawns the
//! prioritized prefetch. That claim is deliberately never released on the happy
//! path — one bulk download per room per session is the point. But the whole
//! post-verification window is exactly when backups are NOT yet usable (the
//! backup key is still being gossiped from the other device), so every room
//! opened while waiting used to claim itself, find nothing to fetch from, and
//! return — permanently excluded from the prioritized fetch for the rest of the
//! session. That is what left rooms sitting on glowing bubbles.
//!
//! The prefetch now parks on the backup state instead, and releases the claim if
//! the backup never becomes usable within its bound so a later reopen retries.

use std::collections::HashMap;
use std::sync::Arc;
use std::time::Duration;

use matrix_sdk::ruma::OwnedRoomId;
use tokio::sync::RwLock;

use super::common::mock_server_and_client;
use crate::matrix::{run_on_open_prefetch, wait_for_backups_enabled, BackupRetryRooms};

/// Short enough to keep the suite fast, and the outcome does not depend on it:
/// no backup ever becomes usable in these tests, so the wait can only end on its
/// deadline. (Real time rather than `start_paused` — tokio's `full` feature does
/// not include `test-util`.)
const TEST_BOUND: Duration = Duration::from_millis(50);

#[tokio::test]
async fn wait_for_backups_enabled_gives_up_after_the_bound() {
    let (_server, client) = mock_server_and_client().await;

    let enabled = wait_for_backups_enabled(&client, TEST_BOUND).await;

    assert!(
        !enabled,
        "a backup that never becomes usable must report not-enabled, not hang"
    );
}

#[tokio::test]
async fn on_open_prefetch_releases_its_claim_when_backup_never_becomes_usable() {
    let (_server, client) = mock_server_and_client().await;
    let room: OwnedRoomId = "!room:example.org".try_into().unwrap();
    let claims = BackupRetryRooms::default();
    let cache = Arc::new(RwLock::new(HashMap::new()));

    // The room-open path claims before spawning, so the task starts already owning it.
    assert!(claims.claim(room.clone()));

    run_on_open_prefetch(client, room.clone(), cache, claims.clone(), TEST_BOUND).await;

    assert!(
        claims.claim(room),
        "giving up on an unusable backup must release the claim so reopening the \
         room retries — otherwise the room never gets a prioritized fetch again"
    );
}

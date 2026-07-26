// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::sync::{Arc, Mutex, MutexGuard};
use std::time::Duration;

use matrix_sdk::room::MessagesOptions;
use matrix_sdk::ruma::events::room::message::MessageType;
use matrix_sdk::ruma::events::{AnySyncMessageLikeEvent, AnySyncTimelineEvent};
use matrix_sdk::ruma::UInt;
use matrix_sdk::{Client, Room, RoomState};
use tracing::warn;

use crate::search_index::SearchIndex;

const BACKFILL_START_DELAY: Duration = Duration::from_secs(30);
const BACKFILL_BATCH_LIMIT: u32 = 100;
const BACKFILL_BATCH_DELAY: Duration = Duration::from_millis(500);
const BACKFILL_ROOM_DELAY: Duration = Duration::from_secs(2);
/// Re-enumeration interval. On sliding sync only materialized (windowed/opened)
/// rooms exist in `client.rooms()` at any moment; rooms slide in as they enter
/// the recency window or are opened. Re-scanning periodically picks those up.
const BACKFILL_RESCAN_INTERVAL: Duration = Duration::from_secs(45);

fn lock_search_index<'a>(
    search_index: &'a Mutex<Option<SearchIndex>>,
) -> MutexGuard<'a, Option<SearchIndex>> {
    match search_index.lock() {
        Ok(guard) => guard,
        Err(poisoned) => {
            warn!("[BACKFILL] Recovering poisoned search index mutex");
            poisoned.into_inner()
        }
    }
}

/// Background worker: backfill search index for E2EE rooms.
///
/// Fetches old messages via the SDK's room messages API and indexes them for
/// local FTS5 search. Runs at low priority with delays between batches to avoid
/// overloading the homeserver. Checkpoints are persisted so backfill can resume
/// across restarts.
///
/// Enumeration note (sliding sync): `client.rooms()` only contains the
/// *materialized* rooms — those currently in the recency window or that have
/// been opened. More rooms slide in over time, so this re-enumerates on an
/// interval rather than snapshotting once. `room.messages()` is a `/messages`
/// REST call that works on any joined room id regardless of windowing, so
/// enumeration was the only limit. Rooms already fully backfilled are skipped
/// cheaply via the persisted checkpoints. Inherent limitation: a room that has
/// never once entered the recency window nor been opened cannot be enumerated
/// here yet, so it stays unindexed until it first materializes.
///
/// Spawned under `session_tasks`, so it is aborted on session teardown/logout.
pub async fn run(client: Client, search_index: Arc<Mutex<Option<SearchIndex>>>) {
    // Wait for initial sync to settle before the first scan.
    tokio::time::sleep(BACKFILL_START_DELAY).await;

    loop {
        // Re-enumerate the currently-materialized joined E2EE rooms each pass so
        // rooms that slid into the window after startup also get indexed. Non-E2EE
        // rooms are skipped (they use the server `/search` API instead).
        let rooms: Vec<_> = client
            .rooms()
            .into_iter()
            .filter(|r| r.state() == RoomState::Joined && r.encryption_state().is_encrypted())
            .collect();

        for room in rooms {
            let room_id = room.room_id().to_string();

            // Skip rooms already fully backfilled (cheap checkpoint read). Bail out
            // entirely if the index disappeared (logout/session teardown).
            {
                let idx = lock_search_index(&search_index);
                match idx.as_ref() {
                    Some(idx) => {
                        if idx.is_backfill_done(&room_id).unwrap_or(true) {
                            continue;
                        }
                    }
                    None => return,
                }
            }

            backfill_room(&room, &room_id, &search_index).await;

            // Pause between rooms.
            tokio::time::sleep(BACKFILL_ROOM_DELAY).await;
        }

        // Bail out if the index is gone (session torn down between scans).
        {
            let idx = lock_search_index(&search_index);
            if idx.is_none() {
                return;
            }
        }

        // Wait before re-enumerating to pick up newly-materialized rooms.
        tokio::time::sleep(BACKFILL_RESCAN_INTERVAL).await;
    }
}

/// Backfill one E2EE room: page backward through its history (resuming from the
/// persisted checkpoint), decrypt + index each batch, and mark the room done
/// once history is exhausted.
async fn backfill_room(room: &Room, room_id: &str, search_index: &Arc<Mutex<Option<SearchIndex>>>) {
    // Get checkpoint (pagination token) for this room.
    let checkpoint = {
        let idx = lock_search_index(search_index);
        idx.as_ref()
            .and_then(|i| i.get_backfill_checkpoint(room_id).ok().flatten())
    };

    // Fetch messages in batches using SDK's room messages API.
    let mut from = checkpoint;
    loop {
        let mut options = MessagesOptions::backward();
        options.limit = UInt::from(BACKFILL_BATCH_LIMIT);
        if let Some(ref token) = from {
            options = options.from(token.as_str());
        }

        let messages = match room.messages(options).await {
            Ok(m) => m,
            Err(e) => {
                warn!("[BACKFILL] Failed to fetch messages for {room_id}: {e}");
                break;
            }
        };

        if messages.chunk.is_empty() {
            // No more messages — mark as done.
            let idx = lock_search_index(search_index);
            if let Some(ref idx) = *idx {
                let _ = idx.mark_backfill_done(room_id);
            }
            break;
        }

        // Decrypt and index the messages.
        // The SDK automatically decrypts events for E2EE rooms in the
        // messages() response, so we just need to deserialize and extract
        // text content.
        let mut batch = Vec::new();
        for event in &messages.chunk {
            // Get the (potentially decrypted) raw event and try to
            // deserialize it.
            let raw = event.kind.raw();
            if let Ok(deserialized) = raw.deserialize() {
                if let AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::RoomMessage(
                    msg,
                )) = &deserialized
                {
                    if let Some(original) = msg.as_original() {
                        let body = match &original.content.msgtype {
                            MessageType::Text(t) => Some(t.body.clone()),
                            MessageType::Emote(e) => Some(format!("* {}", e.body)),
                            MessageType::Notice(n) => Some(n.body.clone()),
                            _ => None,
                        };
                        if let Some(body) = body {
                            let event_id = deserialized.event_id().to_string();
                            let sender = deserialized.sender().to_string();
                            let ts = event.timestamp.map(|t| i64::from(t.0) / 1000).unwrap_or(0);
                            batch.push((
                                event_id,
                                room_id.to_string(),
                                sender.clone(),
                                sender, // sender_name = sender_id for backfill
                                body,
                                ts,
                            ));
                        }
                    }
                }
            }
        }

        // Index the batch on a blocking thread: index_batch does a
        // synchronous SQLCipher write while holding the std::sync index
        // mutex, which would otherwise stall the async executor thread.
        if !batch.is_empty() {
            let search_index = search_index.clone();
            let room_id_for_log = room_id.to_string();
            let _ = tokio::task::spawn_blocking(move || {
                let idx = lock_search_index(&search_index);
                if let Some(ref idx) = *idx {
                    if let Err(e) = idx.index_batch(&batch) {
                        warn!("[BACKFILL] Index batch failed for {room_id_for_log}: {e}");
                    }
                }
            })
            .await;
        }

        // Save checkpoint for resume.
        if let Some(ref token) = messages.end {
            let idx = lock_search_index(search_index);
            if let Some(ref idx) = *idx {
                let _ = idx.set_backfill_checkpoint(room_id, token);
            }
            from = Some(token.clone());
        } else {
            // No more pages.
            let idx = lock_search_index(search_index);
            if let Some(ref idx) = *idx {
                let _ = idx.mark_backfill_done(room_id);
            }
            break;
        }

        // Throttle: wait between batches to avoid overloading server.
        tokio::time::sleep(BACKFILL_BATCH_DELAY).await;
    }
}

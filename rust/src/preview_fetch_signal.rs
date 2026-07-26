// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Per-session slot for the "URL link-preview fetch in progress" callback.
//!
//! The UI glows a message's URL only while its link-preview (OG card) is actually
//! being fetched. The fetch lifecycle lives entirely in
//! `timeline_update_service::fetch_url_previews_and_notify` (the `preview_inflight`
//! set); this slot lets it notify C++ when a given event's fetch starts (true) and
//! ends (false — success or failure alike), keyed by `(room_id, event_id)`.
//!
//! The slot is owned by `MatrixProtocol` and threaded to the fetch through
//! `TimelineRuntime`, so each session signals only its own consumer.

use std::sync::{Arc, Mutex};

/// Args: room_id, event_id, whether a fetch is now in progress.
pub(crate) type PreviewFetchFn = Box<dyn Fn(&str, &str, bool) + Send>;
pub(crate) type PreviewFetchCallbackSlot = Arc<Mutex<Option<PreviewFetchFn>>>;

/// Notify that `event_id` (in `room_id`) started (`true`) or finished (`false`)
/// fetching its URL preview. No-op if no callback is installed.
pub(crate) fn emit(slot: &PreviewFetchCallbackSlot, room_id: &str, event_id: &str, fetching: bool) {
    if let Ok(guard) = slot.lock() {
        if let Some(cb) = guard.as_ref() {
            cb(room_id, event_id, fetching);
        }
    }
}

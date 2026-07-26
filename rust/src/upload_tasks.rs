// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Registry of in-flight direct media-upload tasks, keyed by transaction id.
//!
//! Direct uploads (see `message_action_service::send_media`) bypass the SDK send
//! queue, so there is no local echo to abort. Instead we keep each upload task's
//! [`AbortHandle`] here; cancelling drops the task, which drops the in-flight
//! HTTP request (and the file read) at any point.

use std::collections::HashMap;
use std::sync::{LazyLock, Mutex};

use tokio::task::AbortHandle;

static TASKS: LazyLock<Mutex<HashMap<String, AbortHandle>>> =
    LazyLock::new(|| Mutex::new(HashMap::new()));

/// Track an upload task so it can be cancelled by transaction id.
pub(crate) fn register(txn_id: String, handle: AbortHandle) {
    if let Ok(mut map) = TASKS.lock() {
        map.insert(txn_id, handle);
    }
}

/// Stop tracking a completed/failed upload task (no abort).
pub(crate) fn remove(txn_id: &str) {
    if let Ok(mut map) = TASKS.lock() {
        map.remove(txn_id);
    }
}

/// Abort and forget the in-flight upload for `txn_id`. Returns true if one was
/// tracked (i.e. the cancel landed on a live task).
pub(crate) fn abort(txn_id: &str) -> bool {
    let handle = TASKS.lock().ok().and_then(|mut map| map.remove(txn_id));
    if let Some(handle) = handle {
        handle.abort();
        true
    } else {
        false
    }
}

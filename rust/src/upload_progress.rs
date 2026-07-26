// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Per-session media-upload progress callback.
//!
//! Direct uploads (see `message_action_service::send_media`) report byte
//! progress via the SDK's `with_send_progress_observable`. Since they bypass the
//! send queue, that progress is NOT reflected on a timeline item; instead we
//! forward it (keyed by transaction id) to the C++ UI, which updates the
//! optimistic upload echo. Mirrors the notification-callback pattern: the slot
//! is owned by `MatrixProtocol` and passed down to the upload task.

use std::sync::{Arc, Mutex};

/// Args: transaction_id, bytes sent so far, total bytes.
pub(crate) type ProgressFn = Box<dyn Fn(&str, u64, u64) + Send>;
pub(crate) type UploadProgressCallbackSlot = Arc<Mutex<Option<ProgressFn>>>;

/// Forward an upload-progress tick to the UI, if a callback is installed.
pub(crate) fn report(slot: &UploadProgressCallbackSlot, txn_id: &str, current: u64, total: u64) {
    if let Ok(slot) = slot.lock() {
        if let Some(callback) = slot.as_ref() {
            callback(txn_id, current, total);
        }
    }
}

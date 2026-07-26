// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Cached homeserver maximum upload size (`m.upload.size`), used by the C++ UI
//! to reject oversized files before starting an upload. Loaded once per session
//! (see `matrix.rs::start_sync`) and read synchronously over FFI.
//!
//! Per-session: the limit is a property of the account's homeserver, so each
//! `MatrixProtocol` owns its own value rather than sharing one process-wide.

use std::sync::atomic::AtomicU64;
use std::sync::Arc;

/// Homeserver max upload size in bytes; 0 means "not known yet".
pub(crate) type MaxUploadSizeSlot = Arc<AtomicU64>;

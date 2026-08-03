// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! In-crate integration tests that drive our `pub(crate)` services against a
//! mocked homeserver ([`matrix_sdk::test_utils::mocks::MatrixMockServer`]).
//!
//! These live inside the crate (not in `rust/tests/`) because the services they
//! exercise are `pub(crate)`. They run only under `cargo test` (the `testing`
//! feature is a dev-dependency), never in the shipped staticlib.

mod common;

mod backup_prefetch;
mod room_directory;
mod rooms_list;
mod session_invalidation;
mod session_teardown;
mod smoke;

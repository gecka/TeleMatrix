// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Room unread-count helper.
//!
//! `room_counts_from_sdk` is the single source of truth: it reads the SDK's
//! server (`unread_notification_counts`) and client-side
//! (`num_unread_messages` / `num_unread_mentions`) counts directly. The C++
//! `UnreadStateStore` owns all optimistic masking, so there is no Rust-side
//! unread cache.
//!
//! **Server-first, local only as a fallback** — never `max()` of the two.
//! The SDK's client-side count is not a count of unread messages; when our own
//! read receipt is not inside the loaded linked chunk (the normal state for a
//! room with a large unread backlog) it degrades to "countable events currently
//! loaded" and is recomputed on every event-cache post-processing pass
//! (`compute_unread_counts`, matrix-sdk `caches/read_receipts.rs`). Back
//! pagination grows that chunk, so under `max()` the badge climbed page by page
//! while the user scrolled. The server count doesn't move and arrives with the
//! first sync, which is what makes the badge instant and stable.
//!
//! The fallback is load-bearing, so don't drop it: muted and mentions-only
//! rooms have a server `notification_count` of 0 by design (the mute is a
//! server-side push rule), and the local count is the only unread number they
//! have. It can still drift with pagination there — accepted, since the
//! alternative is no badge at all.

use matrix_sdk::Room;

use crate::types::RoomUnreadCounts;

pub(crate) struct UnreadCountService;

impl UnreadCountService {
    pub(crate) fn room_counts_from_sdk(room: &Room) -> RoomUnreadCounts {
        let server = room.unread_notification_counts();
        RoomUnreadCounts {
            unread_count: Self::effective(server.notification_count, room.num_unread_messages()),
            highlight_count: Self::effective(server.highlight_count, room.num_unread_mentions()),
        }
    }

    /// The number to show: the server's, whenever it has one.
    fn effective(server: u64, client: u64) -> u32 {
        Self::to_u32(if server > 0 { server } else { client })
    }

    fn to_u32(value: u64) -> u32 {
        value.try_into().unwrap_or(u32::MAX)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_server_count_wins_whenever_it_has_one() {
        // The regression this exists to stop: back pagination inflates the
        // client-side count, and under the old `max()` that inflated number
        // became the badge. The server's stays put.
        assert_eq!(UnreadCountService::effective(3, 57), 3);
        assert_eq!(UnreadCountService::effective(3, 3), 3);
        assert_eq!(UnreadCountService::effective(9, 0), 9);
    }

    #[test]
    fn a_zero_server_count_falls_back_to_the_client_side_one() {
        // Muted and mentions-only rooms: the server deliberately reports 0, so
        // the local count is the only badge they can have.
        assert_eq!(UnreadCountService::effective(0, 12), 12);
        assert_eq!(UnreadCountService::effective(0, 0), 0);
    }

    #[test]
    fn counts_saturate_at_u32_max() {
        assert_eq!(UnreadCountService::to_u32(0), 0);
        assert_eq!(UnreadCountService::to_u32(42), 42);
        assert_eq!(UnreadCountService::to_u32(u64::from(u32::MAX)), u32::MAX);
        assert_eq!(
            UnreadCountService::to_u32(u64::from(u32::MAX) + 1),
            u32::MAX
        );
        assert_eq!(UnreadCountService::to_u32(u64::MAX), u32::MAX);
        assert_eq!(UnreadCountService::effective(u64::MAX, 0), u32::MAX);
        assert_eq!(UnreadCountService::effective(0, u64::MAX), u32::MAX);
    }
}

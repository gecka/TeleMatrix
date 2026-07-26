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

use matrix_sdk::Room;

use crate::types::RoomUnreadCounts;

pub(crate) struct UnreadCountService;

impl UnreadCountService {
    pub(crate) fn room_counts_from_sdk(room: &Room) -> RoomUnreadCounts {
        let server = room.unread_notification_counts();
        let server_unread = server.notification_count;
        let server_highlight = server.highlight_count;
        RoomUnreadCounts {
            unread_count: Self::to_u32(room.num_unread_messages().max(server_unread)),
            highlight_count: Self::to_u32(room.num_unread_mentions().max(server_highlight)),
        }
    }

    fn to_u32(value: u64) -> u32 {
        value.try_into().unwrap_or(u32::MAX)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn to_u32_saturates_at_max() {
        assert_eq!(UnreadCountService::to_u32(0), 0);
        assert_eq!(UnreadCountService::to_u32(42), 42);
        assert_eq!(UnreadCountService::to_u32(u64::from(u32::MAX)), u32::MAX);
        assert_eq!(
            UnreadCountService::to_u32(u64::from(u32::MAX) + 1),
            u32::MAX
        );
        assert_eq!(UnreadCountService::to_u32(u64::MAX), u32::MAX);
    }
}

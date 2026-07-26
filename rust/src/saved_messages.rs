// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Per-account "Saved Messages" room marker. The room itself is an ordinary
//! encrypted private room; this global account-data event records which one it
//! is (`org.telematrix.saved_messages`), so every device agrees. The room is
//! never auto-created — it exists only after the user forwards to it or opens
//! it from the menu — so an absent (or empty) marker is the normal state.
//! "Deleting" it is permanent: the room is left/forgotten and the marker
//! cleared, so there is no hidden-but-kept state to track.

use serde::{Deserialize, Serialize};

pub const SAVED_MESSAGES_EVENT_TYPE: &str = "org.telematrix.saved_messages";

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct SavedMessagesPayload {
    /// The saved room's id, or empty when there is no saved room (never
    /// created, or permanently deleted).
    pub room_id: String,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) enum MarkerRoomState {
    Joined,
    Left,
    /// Not in the local store (yet) — normal right after a fresh login, before
    /// the first sync delivers the room list.
    Unknown,
}

#[derive(Debug, Clone, PartialEq)]
pub(crate) enum EnsureAction {
    UseExisting(String),
    CreateNew,
    /// No saved room, and this caller may not create one (a passive session
    /// start rather than an explicit forward / open).
    NoneExists,
}

/// Pure decision for ensure_room. The marker is trusted unless the room is
/// KNOWN to be left: `Unknown` reuses the marker, because ensure runs at login
/// before the first sync — treating an empty store as "not joined" once minted
/// a duplicate saved room on every fresh sign-in. Creation happens ONLY when
/// `create` is set (an explicit forward / open); a passive caller with no live
/// room gets `NoneExists`.
pub(crate) fn decide_ensure_action(
    marker: Option<String>,
    marker_room_state: MarkerRoomState,
    create: bool,
) -> EnsureAction {
    match (marker, marker_room_state) {
        (Some(room_id), MarkerRoomState::Joined | MarkerRoomState::Unknown) => {
            EnsureAction::UseExisting(room_id)
        }
        // No marker, or the marked room was left: the saved room is gone.
        _ if create => EnsureAction::CreateNew,
        _ => EnsureAction::NoneExists,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn payload_roundtrips() {
        let payload = SavedMessagesPayload {
            room_id: "!abc:example.org".into(),
        };
        let json = serde_json::to_string(&payload).unwrap();
        assert_eq!(json, r#"{"room_id":"!abc:example.org"}"#);
        assert_eq!(
            serde_json::from_str::<SavedMessagesPayload>(&json).unwrap(),
            payload
        );
    }

    #[test]
    fn passive_caller_never_creates() {
        // Session start (create=false): no marker means no saved room, and we
        // must NOT mint one.
        assert_eq!(
            decide_ensure_action(None, MarkerRoomState::Unknown, false),
            EnsureAction::NoneExists
        );
    }

    #[test]
    fn explicit_caller_creates_when_absent() {
        // Forward / open (create=true): no marker means create the room now.
        assert_eq!(
            decide_ensure_action(None, MarkerRoomState::Unknown, true),
            EnsureAction::CreateNew
        );
    }

    #[test]
    fn joined_marker_is_reused_either_way() {
        assert_eq!(
            decide_ensure_action(Some("!a:x".into()), MarkerRoomState::Joined, false),
            EnsureAction::UseExisting("!a:x".into())
        );
        assert_eq!(
            decide_ensure_action(Some("!a:x".into()), MarkerRoomState::Joined, true),
            EnsureAction::UseExisting("!a:x".into())
        );
    }

    #[test]
    fn unknown_marker_room_is_trusted_not_recreated() {
        // Fresh login: store empty, room not synced yet. Creating here would
        // mint a duplicate saved room on every sign-in.
        assert_eq!(
            decide_ensure_action(Some("!a:x".into()), MarkerRoomState::Unknown, true),
            EnsureAction::UseExisting("!a:x".into())
        );
    }

    #[test]
    fn left_marker_room_creates_only_when_allowed() {
        assert_eq!(
            decide_ensure_action(Some("!a:x".into()), MarkerRoomState::Left, true),
            EnsureAction::CreateNew
        );
        assert_eq!(
            decide_ensure_action(Some("!a:x".into()), MarkerRoomState::Left, false),
            EnsureAction::NoneExists
        );
    }
}

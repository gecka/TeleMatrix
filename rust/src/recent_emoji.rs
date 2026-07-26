// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Recent compose emojis, synced via Matrix global account data.
//!
//! Stored under the cross-client account-data event type `io.element.recent_emoji`
//! so recents follow the account across devices (and interop with other clients
//! that use the same event type). The
//! content is `{ "recent_emoji": [[emoji, count], ...] }`. A per-session
//! callback (mirroring `upload_progress`) forwards the current list to the C++
//! UI, which keeps the picker's in-memory list in sync.

use std::sync::{Arc, Mutex};

use serde::{Deserialize, Serialize};

/// Cross-client account-data event type.
pub(crate) const RECENT_EMOJI_EVENT_TYPE: &str = "io.element.recent_emoji";

/// Account-data content: an array of `[emoji, count]` pairs.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub(crate) struct RecentEmojiPayload {
    pub recent_emoji: Vec<(String, u32)>,
}

/// Serialize pairs to the JSON array string passed across FFI: `[["😀",5],...]`.
pub(crate) fn to_json_pairs(pairs: &[(String, u32)]) -> String {
    serde_json::to_string(pairs).unwrap_or_else(|_| "[]".to_string())
}

/// Parse the JSON array string from C++ into pairs.
pub(crate) fn from_json_pairs(json: &str) -> Vec<(String, u32)> {
    serde_json::from_str(json).unwrap_or_default()
}

/// Arg: JSON array string of `[emoji, count]` pairs.
pub(crate) type RecentEmojiFn = Box<dyn Fn(&str) + Send>;
pub(crate) type RecentEmojiCallbackSlot = Arc<Mutex<Option<RecentEmojiFn>>>;

/// Forward the current recent-emoji list to the UI, if a callback is installed.
pub(crate) fn emit(slot: &RecentEmojiCallbackSlot, pairs: &[(String, u32)]) {
    if let Ok(slot) = slot.lock() {
        if let Some(callback) = slot.as_ref() {
            callback(&to_json_pairs(pairs));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn payload_roundtrip_element_format() {
        let json = r#"{"recent_emoji":[["😀",5],["❤️",3]]}"#;
        let parsed: RecentEmojiPayload = serde_json::from_str(json).unwrap();
        assert_eq!(parsed.recent_emoji.len(), 2);
        assert_eq!(parsed.recent_emoji[0], ("😀".to_string(), 5));
        let back = serde_json::to_string(&parsed).unwrap();
        assert!(back.contains("recent_emoji"));
    }

    #[test]
    fn json_pairs_roundtrip() {
        let pairs = vec![("😀".to_string(), 2u32), ("🎉".to_string(), 1u32)];
        let s = to_json_pairs(&pairs);
        assert_eq!(from_json_pairs(&s), pairs);
    }

    #[test]
    fn from_json_pairs_bad_input_is_empty() {
        assert!(from_json_pairs("not json").is_empty());
    }
}

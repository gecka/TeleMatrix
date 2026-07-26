// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Folders are Element's room-list **custom sections**, adopted verbatim for
//! two-way interop with Element:
//!
//! - **Membership** = an `m.tag` `element.io.section.<uuid>` on each member room.
//! - **Name / existence / order** = Element's account-level settings, stored as
//!   keys inside the `im.vector.web.settings` global account-data event:
//!     * `RoomList.CustomSectionData` — `{ <tag>: { tag, name, spaceId } }`
//!     * `RoomList.OrderedCustomSections` — `[ <tag>, …, "chats" ]`
//!
//! An empty section persists purely in the settings (no room tag), exactly like
//! Element. TeleMatrix's own rail interleaves these sections with joined spaces;
//! that unified order (which Element has no concept of) lives separately in
//! [`FOLDERS_EVENT_TYPE`].

use std::collections::HashMap;
use std::sync::{Mutex, OnceLock};
use std::time::{SystemTime, UNIX_EPOCH};

use rand::RngCore;

/// TeleMatrix's own account-data event: the unified left-rail order (folders +
/// spaces interleaved). Element has no equivalent, so this stays private.
pub(crate) const FOLDERS_EVENT_TYPE: &str = "io.telematrix.folders.v1";

/// Element's global account-data event that holds account-level settings.
pub(crate) const ELEMENT_SETTINGS_EVENT_TYPE: &str = "im.vector.web.settings";

/// Keys inside `im.vector.web.settings` that hold Element's custom sections.
pub(crate) const CUSTOM_SECTION_DATA_KEY: &str = "RoomList.CustomSectionData";
pub(crate) const ORDERED_SECTIONS_KEY: &str = "RoomList.OrderedCustomSections";

/// Element's prefix for a custom-section tag.
pub(crate) const SECTION_TAG_PREFIX: &str = "element.io.section.";

/// Synthetic marker Element puts in the order list for the "Chats" (untagged)
/// section. We keep it in place so Element renders Chats where the user left it.
pub(crate) const CHATS_TAG: &str = "chats";

/// Element defaults a section's `spaceId` to `MetaSpace.Home` so it shows in the
/// Home view (this is what controls empty-section visibility on Element's side).
pub(crate) const META_SPACE_HOME: &str = "home-space";

/// True for a tag key that represents a folder section.
pub(crate) fn is_section_tag(tag_key: &str) -> bool {
    tag_key.starts_with(SECTION_TAG_PREFIX)
}

/// Generate a fresh `element.io.section.<uuid-v4>` tag. Element only checks the
/// prefix (not that the suffix is a real UUID), but we mint a proper v4 anyway.
pub(crate) fn new_section_tag() -> String {
    let mut bytes = [0u8; 16];
    rand::thread_rng().fill_bytes(&mut bytes);
    bytes[6] = (bytes[6] & 0x0f) | 0x40; // version 4
    bytes[8] = (bytes[8] & 0x3f) | 0x80; // variant
    let uuid = format!(
        "{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15],
    );
    format!("{SECTION_TAG_PREFIX}{uuid}")
}

/// One entry in Element's `RoomList.CustomSectionData` map.
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct CustomSection {
    pub tag: String,
    pub name: String,
    #[serde(rename = "spaceId", default, skip_serializing_if = "Option::is_none")]
    pub space_id: Option<String>,
}

/// TeleMatrix's unified sidebar-order payload (`io.telematrix.folders.v1`).
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct FoldersPayload {
    pub version: u32,
    #[serde(default)]
    pub sidebar_order: Vec<SidebarRef>,
    #[serde(default)]
    pub updated_at_ms: u64,
}

impl FoldersPayload {
    pub(crate) fn new(sidebar_order: Vec<SidebarRef>) -> Self {
        Self {
            version: 2,
            sidebar_order,
            updated_at_ms: SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap_or_default()
                .as_millis() as u64,
        }
    }
}

/// One entry in the unified sidebar order.
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
pub struct SidebarRef {
    pub kind: SidebarKind,
    /// Folder ⇒ the `element.io.section.*` tag; Space ⇒ the space room id.
    pub key: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum SidebarKind {
    Folder,
    Space,
}

/// Process-global, monotonic tag → runtime-handle registry.
///
/// The C++ UI keys folders on an `int` handle; the durable identity is the
/// section tag. Both the folder-list path and the per-room `filter_ids` path
/// resolve handles through this single map so they always agree, and monotonic
/// assignment is collision-free. Handles are per-session only. Same pattern as
/// `invite_sort_timestamp`.
pub(crate) fn section_handle(tag_key: &str) -> i32 {
    registry()
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .get_or_assign(tag_key)
}

/// Reverse lookup: the tag a runtime handle was assigned to, if any. Used by the
/// local cache to persist the durable tag rather than the per-session handle.
pub(crate) fn section_key(handle: i32) -> Option<String> {
    registry()
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .by_handle
        .get(&handle)
        .cloned()
}

fn registry() -> &'static Mutex<SectionRegistry> {
    static REGISTRY: OnceLock<Mutex<SectionRegistry>> = OnceLock::new();
    REGISTRY.get_or_init(|| Mutex::new(SectionRegistry::new()))
}

struct SectionRegistry {
    by_key: HashMap<String, i32>,
    by_handle: HashMap<i32, String>,
    // Handles 0/1/2 are reserved for the built-in All/Personal/Unread filters.
    next: i32,
}

impl SectionRegistry {
    fn new() -> Self {
        Self {
            by_key: HashMap::new(),
            by_handle: HashMap::new(),
            next: 3,
        }
    }

    fn get_or_assign(&mut self, tag_key: &str) -> i32 {
        if let Some(&handle) = self.by_key.get(tag_key) {
            return handle;
        }
        let handle = self.next;
        self.next += 1;
        self.by_key.insert(tag_key.to_string(), handle);
        self.by_handle.insert(handle, tag_key.to_string());
        handle
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn section_tag_prefix_matches_element() {
        let tag = new_section_tag();
        assert!(tag.starts_with("element.io.section."));
        assert!(is_section_tag(&tag));
        assert!(!is_section_tag("u.Work"));
        assert!(!is_section_tag("m.favourite"));
        assert!(!is_section_tag(CHATS_TAG));
    }

    #[test]
    fn new_section_tags_are_unique() {
        let a = new_section_tag();
        let b = new_section_tag();
        assert_ne!(a, b);
    }

    #[test]
    fn custom_section_json_uses_element_shape() {
        let section = CustomSection {
            tag: "element.io.section.abc".to_string(),
            name: "Work".to_string(),
            space_id: Some(META_SPACE_HOME.to_string()),
        };
        let json = serde_json::to_string(&section).unwrap();
        assert!(json.contains("\"spaceId\":\"home-space\""));
        assert!(json.contains("\"name\":\"Work\""));
        // Round-trips (Element writes exactly these fields).
        let parsed: CustomSection = serde_json::from_str(&json).unwrap();
        assert_eq!(parsed.name, "Work");
        assert_eq!(parsed.space_id.as_deref(), Some("home-space"));
    }

    #[test]
    fn custom_section_tolerates_missing_space_id() {
        let parsed: CustomSection =
            serde_json::from_str(r#"{"tag":"element.io.section.x","name":"Fam"}"#).unwrap();
        assert_eq!(parsed.name, "Fam");
        assert_eq!(parsed.space_id, None);
    }

    #[test]
    fn section_handle_stable_distinct_and_reversible() {
        let h = section_handle("element.io.section.__test");
        assert_eq!(h, section_handle("element.io.section.__test"));
        assert!(h >= 3);
        assert_eq!(section_key(h).as_deref(), Some("element.io.section.__test"));
        assert_ne!(h, section_handle("element.io.section.__other"));
    }

    #[test]
    fn payload_roundtrip() {
        let payload = FoldersPayload::new(vec![
            SidebarRef {
                kind: SidebarKind::Folder,
                key: "element.io.section.abc".to_string(),
            },
            SidebarRef {
                kind: SidebarKind::Space,
                key: "!room:server".to_string(),
            },
        ]);
        let json = serde_json::to_string(&payload).unwrap();
        let parsed: FoldersPayload = serde_json::from_str(&json).unwrap();
        assert_eq!(parsed.sidebar_order.len(), 2);
        assert_eq!(parsed.sidebar_order[0].kind, SidebarKind::Folder);
        assert_eq!(parsed.sidebar_order[1].kind, SidebarKind::Space);
    }
}

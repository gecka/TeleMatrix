// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Folders are Element's room-list **custom sections**, adopted verbatim for
//! two-way interop (see [`crate::room_folders`]):
//!
//! - **Membership** = `m.tag` `element.io.section.<uuid>` on member rooms.
//! - **Name / existence / order** = Element's `im.vector.web.settings` account
//!   data (keys `RoomList.CustomSectionData` + `RoomList.OrderedCustomSections`).
//! - **Rail interleave with spaces** (Element has no such concept) = our own
//!   `io.telematrix.folders.v1` `sidebar_order`.

use std::sync::{Arc, Mutex};

use anyhow::{anyhow, Result};
use matrix_sdk::ruma::api::client::config::get_global_account_data;
use matrix_sdk::ruma::events::tag::{TagInfo, TagName};
use matrix_sdk::ruma::events::{AnyGlobalAccountDataEventContent, GlobalAccountDataEventType};
use matrix_sdk::ruma::serde::Raw;
use matrix_sdk::{Client, Room, RoomState};
use serde_json::{json, Map, Value};
use tokio::sync::RwLock;
use tracing::warn;

use crate::app_cache_store::AppCacheStore;
use crate::room_folders::{
    is_section_tag, new_section_tag, section_handle, CustomSection, FoldersPayload, SidebarKind,
    SidebarRef, CHATS_TAG, CUSTOM_SECTION_DATA_KEY, ELEMENT_SETTINGS_EVENT_TYPE,
    FOLDERS_EVENT_TYPE, META_SPACE_HOME, ORDERED_SECTIONS_KEY,
};
use crate::types::FolderMeta;

#[derive(Clone, Default)]
struct FolderState {
    folders: Vec<FolderMeta>,
    /// Element's section registry: tag -> (name, section json). Ordered form is
    /// `section_order`.
    section_data: Vec<CustomSection>,
    /// Element's ordered section tags (custom tags only; the `"chats"` marker is
    /// stripped here and re-inserted on write).
    section_order: Vec<String>,
    /// TeleMatrix's unified rail order (folders + spaces).
    sidebar_order: Vec<SidebarRef>,
    /// Whether the config has been loaded from the server at least once. Lets a
    /// genuinely-empty config be told apart from "not fetched yet".
    loaded: bool,
}

#[derive(Clone)]
pub(crate) struct FolderService {
    state: Arc<RwLock<FolderState>>,
    app_cache_store: Arc<Mutex<Option<AppCacheStore>>>,
}

impl FolderService {
    pub(crate) fn new(app_cache_store: Arc<Mutex<Option<AppCacheStore>>>) -> Self {
        Self {
            state: Arc::new(RwLock::new(FolderState::default())),
            app_cache_store,
        }
    }

    // --- Per-room membership (called from the room-summary build) ---

    /// The `element.io.section.*` folder tag keys set on a room.
    pub(crate) async fn folder_keys_for_room(room: &Room) -> Vec<String> {
        let tags = match room.tags().await {
            Ok(Some(tags)) => tags,
            _ => return Vec::new(),
        };
        tags.keys()
            .map(|tag_name| tag_name.to_string())
            .filter(|key| is_section_tag(key))
            .collect()
    }

    /// The room's folder membership as runtime handles (the ints the UI keys on).
    pub(crate) async fn folder_handles_for_room(room: &Room) -> Vec<i32> {
        let mut ids: Vec<i32> = Self::folder_keys_for_room(room)
            .await
            .iter()
            .map(|key| section_handle(key))
            .collect();
        ids.sort_unstable();
        ids
    }

    // --- Element settings (`im.vector.web.settings`) read/write ---

    /// Fetch the full `im.vector.web.settings` content object from the server, or
    /// an empty map when absent. Preserving the whole object matters: Element
    /// keeps many unrelated settings here, and our writes must not drop them.
    async fn load_element_settings(client: &Client) -> Map<String, Value> {
        match Self::get_global_event_json(client, ELEMENT_SETTINGS_EVENT_TYPE).await {
            Some(Value::Object(map)) => map,
            _ => Map::new(),
        }
    }

    /// The raw content object of a global account-data event, if present.
    ///
    /// Local-first: a previous sync persisted the event in the state store, so
    /// the sections + sidebar order are available INSTANTLY at startup, before
    /// the initial sync (or a network round-trip) completes. Reading only from
    /// the network here meant the rail rendered in DEFAULT order first and then
    /// re-sorted once the real order arrived ("folders sorted after displayed").
    /// The network path is the fallback for a fresh login whose store is empty.
    async fn get_global_event_json(client: &Client, event_type: &str) -> Option<Value> {
        let ev_type = GlobalAccountDataEventType::from(event_type);
        if let Ok(Some(raw)) = client.account().account_data_raw(ev_type.clone()).await {
            if let Ok(value) = serde_json::from_str::<Value>(raw.json().get()) {
                return Some(value);
            }
        }
        let own_user = client.user_id()?.to_owned();
        let request = get_global_account_data::v3::Request::new(own_user, ev_type);
        let response = client.send(request).await.ok()?;
        serde_json::from_str::<Value>(response.account_data.json().get()).ok()
    }

    /// Parse Element's two section keys out of a settings object.
    fn parse_sections(settings: &Map<String, Value>) -> (Vec<CustomSection>, Vec<String>) {
        let mut sections = Vec::new();
        if let Some(Value::Object(data)) = settings.get(CUSTOM_SECTION_DATA_KEY) {
            for (tag, value) in data {
                if !is_section_tag(tag) {
                    continue;
                }
                if let Ok(section) = serde_json::from_value::<CustomSection>(value.clone()) {
                    if section.tag == *tag {
                        sections.push(section);
                    }
                }
            }
        }
        let mut order = Vec::new();
        if let Some(Value::Array(arr)) = settings.get(ORDERED_SECTIONS_KEY) {
            for entry in arr {
                if let Some(tag) = entry.as_str() {
                    if is_section_tag(tag) {
                        order.push(tag.to_string());
                    }
                }
            }
        }
        (sections, order)
    }

    /// Write Element's two section keys back into `im.vector.web.settings`,
    /// preserving every other key (read-modify-write against a fresh load). The
    /// order array gets the `"chats"` marker appended so Element keeps rendering
    /// its Chats section after the custom ones.
    async fn save_element_sections(
        client: &Client,
        sections: &[CustomSection],
        order: &[String],
    ) -> Result<()> {
        let mut settings = Self::load_element_settings(client).await;

        let mut data = Map::new();
        for section in sections {
            data.insert(
                section.tag.clone(),
                serde_json::to_value(section).unwrap_or(Value::Null),
            );
        }
        settings.insert(CUSTOM_SECTION_DATA_KEY.to_string(), Value::Object(data));

        let mut ordered: Vec<Value> = order.iter().map(|t| json!(t)).collect();
        ordered.push(json!(CHATS_TAG));
        settings.insert(ORDERED_SECTIONS_KEY.to_string(), Value::Array(ordered));

        let raw: Raw<AnyGlobalAccountDataEventContent> = Raw::from_json(
            serde_json::value::to_raw_value(&Value::Object(settings))
                .map_err(|e| anyhow!("Failed to serialize Element settings: {e}"))?,
        );
        client
            .account()
            .set_account_data_raw(
                GlobalAccountDataEventType::from(ELEMENT_SETTINGS_EVENT_TYPE),
                raw,
            )
            .await
            .map_err(|e| anyhow!("Failed to save Element settings: {e}"))?;
        Ok(())
    }

    // --- TeleMatrix's unified sidebar order (`io.telematrix.folders.v1`) ---

    async fn load_sidebar_order(client: &Client) -> Vec<SidebarRef> {
        match Self::get_global_event_json(client, FOLDERS_EVENT_TYPE).await {
            Some(value) => serde_json::from_value::<FoldersPayload>(value)
                .map(|p| p.sidebar_order)
                .unwrap_or_default(),
            None => Vec::new(),
        }
    }

    async fn save_sidebar_order(client: &Client, order: &[SidebarRef]) -> Result<()> {
        let payload = FoldersPayload::new(order.to_vec());
        let raw: Raw<AnyGlobalAccountDataEventContent> = Raw::from_json(
            serde_json::value::to_raw_value(&payload)
                .map_err(|e| anyhow!("Failed to serialize sidebar order: {e}"))?,
        );
        client
            .account()
            .set_account_data_raw(GlobalAccountDataEventType::from(FOLDERS_EVENT_TYPE), raw)
            .await
            .map_err(|e| anyhow!("Failed to save sidebar order: {e}"))?;
        Ok(())
    }

    // --- Folder-list derivation ---

    /// Build the folder list from Element's section registry, ordered by
    /// `OrderedCustomSections` then any registry entries not in that order.
    fn assemble_folders(sections: &[CustomSection], order: &[String]) -> Vec<FolderMeta> {
        let mut folders = Vec::new();
        let mut seen = std::collections::HashSet::new();
        for tag in order {
            if let Some(section) = sections.iter().find(|s| s.tag == *tag) {
                if seen.insert(tag.clone()) {
                    folders.push(Self::folder_meta(section));
                }
            }
        }
        for section in sections {
            if seen.insert(section.tag.clone()) {
                folders.push(Self::folder_meta(section));
            }
        }
        folders
    }

    fn folder_meta(section: &CustomSection) -> FolderMeta {
        FolderMeta {
            id: section_handle(&section.tag),
            tag_key: section.tag.clone(),
            name: section.name.clone(),
        }
    }

    /// Authoritative rebuild: (re)load Element's sections + our sidebar order,
    /// cache, persist. Uses the in-memory cache once loaded so this is cheap to
    /// call on every account-data change.
    async fn rebuild(&self, client: &Client) -> Result<Vec<FolderMeta>> {
        let loaded = self.state.read().await.loaded;
        let (sections, order, sidebar_order) = if loaded {
            let state = self.state.read().await;
            (
                state.section_data.clone(),
                state.section_order.clone(),
                state.sidebar_order.clone(),
            )
        } else {
            let settings = Self::load_element_settings(client).await;
            let (sections, order) = Self::parse_sections(&settings);
            let sidebar_order = Self::load_sidebar_order(client).await;
            (sections, order, sidebar_order)
        };
        let folders = Self::assemble_folders(&sections, &order);
        self.replace_state_and_persist(folders.clone(), sections, order, sidebar_order)
            .await;
        Ok(folders)
    }

    pub(crate) async fn get_folders(&self, client: Option<Client>) -> Result<Vec<FolderMeta>> {
        if let Some(client) = client {
            match self.rebuild(&client).await {
                Ok(folders) => Ok(folders),
                Err(e) => {
                    warn!("[folders] rebuild failed ({e}); serving local mirror");
                    Ok(self.load_local_folders())
                }
            }
        } else {
            let cache = self.state.read().await;
            if !cache.folders.is_empty() {
                Ok(cache.folders.clone())
            } else {
                Ok(self.load_local_folders())
            }
        }
    }

    pub(crate) async fn cached_sidebar_order(&self) -> Vec<SidebarRef> {
        self.state.read().await.sidebar_order.clone()
    }

    /// A sync-delivered account-data event may carry a fresh Element settings blob
    /// (sections created/renamed elsewhere) or our own sidebar-order event. Update
    /// the in-memory caches; the caller re-derives + notifies the UI. Returns
    /// whether any folder-relevant event was seen.
    pub(crate) async fn apply_sync_account_data_jsons<I, S>(&self, jsons: I) -> bool
    where
        I: IntoIterator<Item = S>,
        S: AsRef<str>,
    {
        let mut changed = false;
        for json in jsons {
            let json_str = json.as_ref();
            let is_settings = json_str.contains(ELEMENT_SETTINGS_EVENT_TYPE);
            let is_sidebar = json_str.contains(FOLDERS_EVENT_TYPE);
            if !is_settings && !is_sidebar {
                continue;
            }
            let Ok(wrapper) = serde_json::from_str::<Value>(json_str) else {
                continue;
            };
            let Some(content) = wrapper.get("content") else {
                continue;
            };
            if is_settings {
                if let Value::Object(map) = content {
                    let (sections, order) = Self::parse_sections(map);
                    let mut state = self.state.write().await;
                    state.section_data = sections;
                    state.section_order = order;
                    state.loaded = true;
                    changed = true;
                }
            }
            if is_sidebar {
                if let Ok(payload) = serde_json::from_value::<FoldersPayload>(content.clone()) {
                    let mut state = self.state.write().await;
                    state.sidebar_order = payload.sidebar_order;
                    state.loaded = true;
                    changed = true;
                }
            }
        }
        changed
    }

    // --- Mutations ---

    /// Create an empty section. Returns its `element.io.section.*` tag. Writes the
    /// section to Element's settings (name + order) so it shows in Element too,
    /// and registers it in our unified rail order.
    pub(crate) async fn create_folder(&self, client: Client, name: &str) -> Result<String> {
        let trimmed = name.trim();
        if trimmed.is_empty() {
            return Err(anyhow!("Folder name is empty"));
        }
        let tag = new_section_tag();

        let settings = Self::load_element_settings(&client).await;
        let (mut sections, mut order) = Self::parse_sections(&settings);
        sections.push(CustomSection {
            tag: tag.clone(),
            name: trimmed.to_string(),
            space_id: Some(META_SPACE_HOME.to_string()),
        });
        order.push(tag.clone());
        Self::save_element_sections(&client, &sections, &order).await?;

        let mut sidebar = Self::load_sidebar_order(&client).await;
        sidebar.push(SidebarRef {
            kind: SidebarKind::Folder,
            key: tag.clone(),
        });
        Self::save_sidebar_order(&client, &sidebar).await.ok();

        {
            let mut state = self.state.write().await;
            state.section_data = sections;
            state.section_order = order;
            state.sidebar_order = sidebar;
            state.loaded = true;
        }
        self.rebuild(&client).await.ok();
        Ok(tag)
    }

    /// Toggle a room's membership in a section (its `element.io.section.*` tag).
    pub(crate) async fn add_room_to_folder(&self, room: Room, tag_key: &str) -> Result<()> {
        if !is_section_tag(tag_key) {
            return Err(anyhow!("Invalid section tag"));
        }
        let tag_name = TagName::from(tag_key.to_string());
        let has_tag = matches!(room.tags().await, Ok(Some(tags)) if tags.contains_key(&tag_name));
        if has_tag {
            room.remove_tag(tag_name)
                .await
                .map_err(|e| anyhow!("Failed to remove section tag: {e}"))?;
        } else {
            room.set_tag(tag_name, TagInfo::new())
                .await
                .map_err(|e| anyhow!("Failed to set section tag: {e}"))?;
        }
        Ok(())
    }

    /// Rename a section — just its name in Element's settings. The tag (a UUID) is
    /// stable, so no room re-tagging is needed (a clean win over name-in-tag).
    pub(crate) async fn rename_folder(
        &self,
        client: Client,
        tag_key: &str,
        new_name: &str,
    ) -> Result<String> {
        let trimmed = new_name.trim();
        if trimmed.is_empty() {
            return Err(anyhow!("Folder name is empty"));
        }
        let settings = Self::load_element_settings(&client).await;
        let (mut sections, order) = Self::parse_sections(&settings);
        let Some(section) = sections.iter_mut().find(|s| s.tag == tag_key) else {
            return Err(anyhow!("Section not found"));
        };
        section.name = trimmed.to_string();
        Self::save_element_sections(&client, &sections, &order).await?;
        {
            let mut state = self.state.write().await;
            state.section_data = sections;
            state.section_order = order;
            state.loaded = true;
        }
        self.rebuild(&client).await.ok();
        Ok(tag_key.to_string())
    }

    /// Delete a section: strip its tag from every non-left room, drop it from
    /// Element's settings and from our sidebar order.
    pub(crate) async fn delete_folder(&self, client: Client, tag_key: &str) -> Result<()> {
        let tag_name = TagName::from(tag_key.to_string());
        for room in client.rooms() {
            if room.state() == RoomState::Left {
                continue;
            }
            let has_tag =
                matches!(room.tags().await, Ok(Some(tags)) if tags.contains_key(&tag_name));
            if has_tag {
                if let Err(e) = room.remove_tag(tag_name.clone()).await {
                    warn!("Delete: failed to remove tag from {}: {e}", room.room_id());
                }
            }
        }

        let settings = Self::load_element_settings(&client).await;
        let (mut sections, mut order) = Self::parse_sections(&settings);
        sections.retain(|s| s.tag != tag_key);
        order.retain(|t| t != tag_key);
        Self::save_element_sections(&client, &sections, &order).await?;

        let mut sidebar = Self::load_sidebar_order(&client).await;
        sidebar.retain(|e| !(e.kind == SidebarKind::Folder && e.key == tag_key));
        Self::save_sidebar_order(&client, &sidebar).await.ok();

        {
            let mut state = self.state.write().await;
            state.section_data = sections;
            state.section_order = order;
            state.sidebar_order = sidebar;
            state.loaded = true;
        }
        self.rebuild(&client).await.ok();
        Ok(())
    }

    /// Replace the unified rail order (folders + spaces). Also mirrors the
    /// folder-only order into Element's `OrderedCustomSections` so Element shows
    /// the same section order.
    pub(crate) async fn set_sidebar_order(
        &self,
        client: Client,
        order: Vec<SidebarRef>,
    ) -> Result<()> {
        Self::save_sidebar_order(&client, &order).await?;

        // Mirror the folder subset (in rail order) into Element's section order.
        let folder_order: Vec<String> = order
            .iter()
            .filter(|e| e.kind == SidebarKind::Folder && is_section_tag(&e.key))
            .map(|e| e.key.clone())
            .collect();
        let settings = Self::load_element_settings(&client).await;
        let (sections, _prev_order) = Self::parse_sections(&settings);
        // Keep only tags that still exist as sections; append any missing ones.
        let mut new_order: Vec<String> = folder_order
            .into_iter()
            .filter(|t| sections.iter().any(|s| s.tag == *t))
            .collect();
        for s in &sections {
            if !new_order.contains(&s.tag) {
                new_order.push(s.tag.clone());
            }
        }
        Self::save_element_sections(&client, &sections, &new_order)
            .await
            .ok();

        {
            let mut state = self.state.write().await;
            state.sidebar_order = order;
            state.section_data = sections;
            state.section_order = new_order;
            state.loaded = true;
        }
        self.rebuild(&client).await.ok();
        Ok(())
    }

    // --- Helpers ---

    fn load_local_folders(&self) -> Vec<FolderMeta> {
        if let Ok(guard) = self.app_cache_store.lock() {
            if let Some(store) = guard.as_ref() {
                return store.load_folders().unwrap_or_default();
            }
        }
        Vec::new()
    }

    async fn replace_state_and_persist(
        &self,
        folders: Vec<FolderMeta>,
        section_data: Vec<CustomSection>,
        section_order: Vec<String>,
        sidebar_order: Vec<SidebarRef>,
    ) {
        {
            let mut state = self.state.write().await;
            state.folders = folders.clone();
            state.section_data = section_data;
            state.section_order = section_order;
            state.sidebar_order = sidebar_order;
            state.loaded = true;
        }
        self.persist_folders(&folders);
    }

    fn persist_folders(&self, folders: &[FolderMeta]) {
        // Off the async worker: save_folders is a synchronous SQLCipher write.
        // See PERF-6 / [[sync-sqlite-on-async-workers]].
        let store = self.app_cache_store.clone();
        let folders = folders.to_vec();
        tokio::task::spawn_blocking(move || {
            if let Ok(guard) = store.lock() {
                if let Some(store) = guard.as_ref() {
                    let _ = store.save_folders(&folders);
                }
            }
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn section(tag: &str, name: &str) -> CustomSection {
        CustomSection {
            tag: tag.to_string(),
            name: name.to_string(),
            space_id: Some(META_SPACE_HOME.to_string()),
        }
    }

    #[test]
    fn assemble_orders_by_section_order_then_appends() {
        let sections = vec![
            section("element.io.section.a", "Work"),
            section("element.io.section.b", "Family"),
            section("element.io.section.c", "Travel"),
        ];
        let order = vec![
            "element.io.section.b".to_string(),
            "element.io.section.a".to_string(),
        ];
        let folders = FolderService::assemble_folders(&sections, &order);
        let names: Vec<_> = folders.iter().map(|f| f.name.as_str()).collect();
        // Ordered first (Family, Work), then the leftover (Travel).
        assert_eq!(names, vec!["Family", "Work", "Travel"]);
    }

    #[test]
    fn assemble_keeps_empty_section() {
        // A section in the registry with no member rooms still shows.
        let sections = vec![section("element.io.section.x", "Empty")];
        let folders = FolderService::assemble_folders(&sections, &[]);
        assert_eq!(folders.len(), 1);
        assert_eq!(folders[0].name, "Empty");
        assert_eq!(folders[0].tag_key, "element.io.section.x");
    }

    #[test]
    fn parse_sections_reads_element_shape() {
        let settings: Map<String, Value> = serde_json::from_str(
            r#"{
                "RoomList.CustomSectionData": {
                    "element.io.section.aa": {"tag":"element.io.section.aa","name":"Test","spaceId":"home-space"}
                },
                "RoomList.OrderedCustomSections": ["element.io.section.aa","chats"],
                "some.other.setting": true
            }"#,
        )
        .unwrap();
        let (sections, order) = FolderService::parse_sections(&settings);
        assert_eq!(sections.len(), 1);
        assert_eq!(sections[0].name, "Test");
        // "chats" is filtered out of the parsed order (re-added on write).
        assert_eq!(order, vec!["element.io.section.aa".to_string()]);
    }

    #[test]
    fn save_preserves_other_settings_and_readds_chats() {
        // Simulate the write transform on a settings blob with unrelated keys.
        let mut settings: Map<String, Value> =
            serde_json::from_str(r#"{"im.vector.setting.breadcrumbs": true}"#).unwrap();
        let sections = vec![section("element.io.section.aa", "Test")];
        let order = ["element.io.section.aa".to_string()];

        let mut data = Map::new();
        for s in &sections {
            data.insert(s.tag.clone(), serde_json::to_value(s).unwrap());
        }
        settings.insert(CUSTOM_SECTION_DATA_KEY.to_string(), Value::Object(data));
        let mut ordered: Vec<Value> = order.iter().map(|t| json!(t)).collect();
        ordered.push(json!(CHATS_TAG));
        settings.insert(ORDERED_SECTIONS_KEY.to_string(), Value::Array(ordered));

        // Unrelated key survives.
        assert_eq!(
            settings.get("im.vector.setting.breadcrumbs"),
            Some(&json!(true))
        );
        // Chats marker present at the end of the order.
        let order_val = settings
            .get(ORDERED_SECTIONS_KEY)
            .unwrap()
            .as_array()
            .unwrap();
        assert_eq!(order_val.last().unwrap(), &json!("chats"));
    }
}

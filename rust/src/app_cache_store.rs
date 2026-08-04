// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! App-owned SQLCipher cache for lightweight UI startup data.
//!
//! This database replaces derived JSON cache files. It intentionally does not
//! store settings; settings remain in the C++ `settings.json` file.

use std::collections::HashMap;
use std::path::Path;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use anyhow::{anyhow, Result};
use matrix_sdk::ruma::api::MatrixVersion;
use rusqlite::{params, Connection};
use tracing::warn;

use crate::types::{FolderMeta, MembershipState, RoomNotificationMode, RoomSummary, SendState};

pub struct AppCacheStore {
    conn: Connection,
}

impl AppCacheStore {
    pub fn open(data_dir: &Path, key_material: &str) -> Result<Self> {
        std::fs::create_dir_all(data_dir)?;
        let db_path = data_dir.join("app_cache.db");
        let conn =
            match crate::encrypted_sqlite::open(&db_path, b"telematrix-app-cache-v1", key_material)
                .and_then(|conn| {
                    Self::init_schema(&conn)?;
                    Ok(conn)
                }) {
                Ok(conn) => conn,
                Err(err) => {
                    warn!("Failed to open encrypted app cache ({err}); rebuilding");
                    crate::encrypted_sqlite::delete_database_files(&db_path)?;
                    let conn = crate::encrypted_sqlite::open(
                        &db_path,
                        b"telematrix-app-cache-v1",
                        key_material,
                    )?;
                    Self::init_schema(&conn)?;
                    conn
                }
            };
        Ok(Self { conn })
    }

    fn init_schema(conn: &Connection) -> Result<()> {
        conn.pragma_update(None, "journal_mode", "WAL")?;
        conn.pragma_update(None, "foreign_keys", "ON")?;

        let version: u32 = conn.pragma_query_value(None, "user_version", |r| r.get(0))?;
        if version == 0 {
            conn.execute_batch(
                "CREATE TABLE IF NOT EXISTS cached_rooms (
                    room_id                     TEXT PRIMARY KEY,
                    sort_index                  INTEGER NOT NULL,
                    display_name                TEXT NOT NULL,
                    canonical_alias             TEXT,
                    avatar_url                  TEXT,
                    avatar_entity_id            TEXT NOT NULL,
                    last_event_text             TEXT NOT NULL,
                    last_event_sender           TEXT NOT NULL,
                    last_event_timestamp        INTEGER NOT NULL,
                    unread_count                INTEGER NOT NULL,
                    highlight_count             INTEGER NOT NULL,
                    notification_mode           INTEGER NOT NULL,
                    is_muted                    INTEGER NOT NULL,
                    is_pinned                   INTEGER NOT NULL,
                    is_marked_unread            INTEGER NOT NULL,
                    is_direct                   INTEGER NOT NULL,
                    is_last_event_outgoing      INTEGER NOT NULL,
                    is_last_event_service       INTEGER NOT NULL,
                    last_event_send_state       INTEGER NOT NULL,
                    member_count                INTEGER NOT NULL,
                    can_pin_messages            INTEGER NOT NULL,
                    peer_presence               INTEGER NOT NULL,
                    membership                  INTEGER NOT NULL,
                    inviter_user_id             TEXT NOT NULL,
                    inviter_display_name        TEXT NOT NULL,
                    inviter_avatar_url          TEXT NOT NULL,
                    room_topic                  TEXT NOT NULL,
                    updated_at                  INTEGER NOT NULL
                );
                CREATE TABLE IF NOT EXISTS cached_room_filters (
                    room_id     TEXT NOT NULL,
                    sort_index  INTEGER NOT NULL,
                    filter_id   INTEGER NOT NULL,
                    PRIMARY KEY (room_id, filter_id),
                    FOREIGN KEY (room_id) REFERENCES cached_rooms(room_id) ON DELETE CASCADE
                );
                CREATE TABLE IF NOT EXISTS cached_folders (
                    id          INTEGER PRIMARY KEY,
                    sort_index  INTEGER NOT NULL,
                    name        TEXT NOT NULL,
                    updated_at  INTEGER NOT NULL
                );
                CREATE TABLE IF NOT EXISTS server_versions (
                    homeserver  TEXT NOT NULL,
                    sort_index  INTEGER NOT NULL,
                    version     TEXT NOT NULL,
                    fetched_at  INTEGER NOT NULL,
                    PRIMARY KEY (homeserver, version)
                );
                PRAGMA user_version = 1;",
            )?;
        }

        // Migration to v2: server-synced recent-emoji cache (mirror of the
        // `io.element.recent_emoji` account data; source of truth is the server).
        let version: u32 = conn.pragma_query_value(None, "user_version", |r| r.get(0))?;
        if version < 2 {
            conn.execute_batch(
                "CREATE TABLE IF NOT EXISTS cached_recent_emoji (
                    sort_index  INTEGER PRIMARY KEY,
                    emoji       TEXT NOT NULL,
                    rating      INTEGER NOT NULL
                );
                PRAGMA user_version = 2;",
            )?;
        }

        // Migration to v3: the pinned rooms' `m.favourite` tag order. Cached so the
        // list is in the user's order on the first frame, not just after sync lands.
        let version: u32 = conn.pragma_query_value(None, "user_version", |r| r.get(0))?;
        if version < 3 {
            conn.execute_batch(
                "ALTER TABLE cached_rooms ADD COLUMN pinned_order REAL;
                PRAGMA user_version = 3;",
            )?;
        }

        // Migration to v4: join-rule publicness. Both `Room::is_public()` and
        // `Room::join_rule()` answer from LOCAL state, so on a cold start — before a
        // room's state has synced — both are None and the room reads as private. That
        // silently disabled "hide system messages in public rooms" for the whole time
        // such a room stayed open: the open-time seed was false, and the async
        // correction ignores an Unknown join rule by design (it must not un-seed a
        // correct value). Persisting the last known answer makes the seed survive a
        // restart instead of waiting on sync. NULL = never learned.
        let version: u32 = conn.pragma_query_value(None, "user_version", |r| r.get(0))?;
        if version < 4 {
            conn.execute_batch(
                "ALTER TABLE cached_rooms ADD COLUMN is_public INTEGER;
                PRAGMA user_version = 4;",
            )?;
        }

        // Migration to v5: folders are native `u.*` tags now. The durable identity
        // is the tag key, not the (per-session) int handle — so both folder tables
        // re-key on `tag_key`. Drop-and-recreate (no data preserved): the numeric
        // scheme is gone, and cached membership is a cold-start nicety that refills
        // from sync on the next rebuild.
        let version: u32 = conn.pragma_query_value(None, "user_version", |r| r.get(0))?;
        if version < 5 {
            conn.execute_batch(
                "DROP TABLE IF EXISTS cached_room_filters;
                DROP TABLE IF EXISTS cached_folders;
                CREATE TABLE cached_room_filters (
                    room_id     TEXT NOT NULL,
                    sort_index  INTEGER NOT NULL,
                    tag_key     TEXT NOT NULL,
                    PRIMARY KEY (room_id, tag_key),
                    FOREIGN KEY (room_id) REFERENCES cached_rooms(room_id) ON DELETE CASCADE
                );
                CREATE TABLE cached_folders (
                    tag_key     TEXT PRIMARY KEY,
                    sort_index  INTEGER NOT NULL,
                    name        TEXT NOT NULL,
                    updated_at  INTEGER NOT NULL
                );
                PRAGMA user_version = 5;",
            )?;
        }
        Ok(())
    }

    pub fn load_rooms(&self) -> Result<Vec<RoomSummary>> {
        // Membership is cached as durable tag keys; map each to this session's
        // runtime handle so the ints match the freshly-derived folder list.
        let mut filters: HashMap<String, Vec<i32>> = HashMap::new();
        {
            let mut stmt = self.conn.prepare(
                "SELECT room_id, tag_key
                 FROM cached_room_filters
                 ORDER BY room_id ASC, sort_index ASC",
            )?;
            let rows = stmt.query_map([], |row| {
                Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?))
            })?;
            for row in rows {
                let (room_id, tag_key) = row?;
                filters
                    .entry(room_id)
                    .or_default()
                    .push(crate::room_folders::section_handle(&tag_key));
            }
        }

        let mut stmt = self.conn.prepare(
            "SELECT room_id, display_name, canonical_alias, avatar_url,
                    avatar_entity_id, last_event_text, last_event_sender,
                    last_event_timestamp, unread_count, highlight_count,
                    notification_mode, is_muted, is_pinned, is_marked_unread,
                    is_direct, is_last_event_outgoing, is_last_event_service,
                    last_event_send_state, member_count, can_pin_messages,
                    peer_presence, membership, inviter_user_id,
                    inviter_display_name, inviter_avatar_url, room_topic,
                    pinned_order, is_public
             FROM cached_rooms
             ORDER BY sort_index ASC",
        )?;

        let rows = stmt.query_map([], |row| {
            let room_id: String = row.get(0)?;
            Ok(RoomSummary {
                filter_ids: filters.remove(&room_id).unwrap_or_default(),
                space_ids: Vec::new(),
                room_id,
                display_name: row.get(1)?,
                canonical_alias: empty_to_none(row.get(2)?),
                avatar_url: empty_to_none(row.get(3)?),
                avatar_entity_id: row.get(4)?,
                last_event_text: row.get(5)?,
                last_event_sender: row.get(6)?,
                last_event_timestamp: epoch_to_system_time(row.get::<_, i64>(7)?),
                unread_count: row.get::<_, i64>(8)?.max(0) as u32,
                highlight_count: row.get::<_, i64>(9)?.max(0) as u32,
                notification_mode: notification_mode_from_u32(row.get::<_, u32>(10)?),
                is_muted: row.get::<_, bool>(11)?,
                is_pinned: row.get::<_, bool>(12)?,
                pinned_order: row.get::<_, Option<f64>>(26)?,
                is_marked_unread: row.get::<_, bool>(13)?,
                is_direct: row.get::<_, bool>(14)?,
                // NULL (never learned) reads as private, same as before the column existed,
                // but stays flagged unknown so a rebuild doesn't treat it as an answer.
                is_public: row.get::<_, Option<bool>>(27)?.unwrap_or(false),
                is_public_known: row.get::<_, Option<bool>>(27)?.is_some(),
                is_last_event_outgoing: row.get::<_, bool>(15)?,
                is_last_event_service: row.get::<_, bool>(16)?,
                last_event_send_state: send_state_from_u32(row.get::<_, u32>(17)?),
                member_count: row.get::<_, i64>(18)?.max(0) as u64,
                can_pin_messages: row.get::<_, bool>(19)?,
                peer_presence: row.get::<_, i64>(20)?.max(0) as u32,
                membership: membership_from_u32(row.get::<_, u32>(21)?),
                inviter_user_id: row.get(22)?,
                inviter_display_name: row.get(23)?,
                inviter_avatar_url: row.get(24)?,
                room_topic: row.get(25)?,
            })
        })?;

        let mut rooms = Vec::new();
        for row in rows {
            rooms.push(row?);
        }
        Ok(rooms)
    }

    pub fn save_rooms(&self, rooms: &[RoomSummary]) -> Result<()> {
        self.conn.execute_batch("BEGIN IMMEDIATE")?;
        let result = (|| -> Result<()> {
            self.conn.execute("DELETE FROM cached_room_filters", [])?;
            self.conn.execute("DELETE FROM cached_rooms", [])?;

            let now = now_epoch();
            let mut room_stmt = self.conn.prepare(
                "INSERT INTO cached_rooms (
                    room_id, sort_index, display_name, canonical_alias, avatar_url,
                    avatar_entity_id, last_event_text, last_event_sender,
                    last_event_timestamp, unread_count, highlight_count,
                    notification_mode, is_muted, is_pinned, is_marked_unread,
                    is_direct, is_last_event_outgoing, is_last_event_service,
                    last_event_send_state, member_count, can_pin_messages,
                    peer_presence, membership, inviter_user_id, inviter_display_name,
                    inviter_avatar_url, room_topic, updated_at, pinned_order, is_public
                 ) VALUES (
                    ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12,
                    ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20, ?21, ?22,
                    ?23, ?24, ?25, ?26, ?27, ?28, ?29, ?30
                 )",
            )?;
            let mut filter_stmt = self.conn.prepare(
                "INSERT OR IGNORE INTO cached_room_filters (room_id, sort_index, tag_key)
                 VALUES (?1, ?2, ?3)",
            )?;

            for (index, room) in rooms.iter().enumerate() {
                room_stmt.execute(params![
                    room.room_id,
                    index as i64,
                    room.display_name,
                    room.canonical_alias,
                    room.avatar_url,
                    room.avatar_entity_id,
                    room.last_event_text,
                    room.last_event_sender,
                    system_time_to_epoch(room.last_event_timestamp),
                    room.unread_count as i64,
                    room.highlight_count as i64,
                    room.notification_mode as u32,
                    room.is_muted,
                    room.is_pinned,
                    room.is_marked_unread,
                    room.is_direct,
                    room.is_last_event_outgoing,
                    room.is_last_event_service,
                    room.last_event_send_state as u32,
                    room.member_count as i64,
                    room.can_pin_messages,
                    room.peer_presence as i64,
                    room.membership as u32,
                    room.inviter_user_id,
                    room.inviter_display_name,
                    room.inviter_avatar_url,
                    room.room_topic,
                    now,
                    room.pinned_order,
                    // NULL when the join rule never synced, so a reload can tell
                    // "not learned yet" from a real private room.
                    room.is_public_known.then_some(room.is_public),
                ])?;
                for (filter_index, filter_id) in room.filter_ids.iter().enumerate() {
                    // Persist the durable tag key, not the per-session handle.
                    if let Some(tag_key) = crate::room_folders::section_key(*filter_id) {
                        filter_stmt.execute(params![room.room_id, filter_index as i64, tag_key])?;
                    }
                }
            }
            Ok(())
        })();

        finish_transaction(&self.conn, result)
    }

    pub fn load_folders(&self) -> Result<Vec<FolderMeta>> {
        let mut stmt = self.conn.prepare(
            "SELECT tag_key, name
             FROM cached_folders
             ORDER BY sort_index ASC",
        )?;
        let rows = stmt.query_map([], |row| {
            let tag_key: String = row.get(0)?;
            Ok(FolderMeta {
                id: crate::room_folders::section_handle(&tag_key),
                tag_key,
                name: row.get(1)?,
            })
        })?;

        let mut folders = Vec::new();
        for row in rows {
            folders.push(row?);
        }
        Ok(folders)
    }

    pub fn save_folders(&self, folders: &[FolderMeta]) -> Result<()> {
        self.conn.execute_batch("BEGIN IMMEDIATE")?;
        let result = (|| -> Result<()> {
            self.conn.execute("DELETE FROM cached_folders", [])?;
            let now = now_epoch();
            let mut stmt = self.conn.prepare(
                "INSERT OR IGNORE INTO cached_folders (tag_key, sort_index, name, updated_at)
                 VALUES (?1, ?2, ?3, ?4)",
            )?;
            for (index, folder) in folders.iter().enumerate() {
                stmt.execute(params![folder.tag_key, index as i64, folder.name, now])?;
            }
            Ok(())
        })();

        finish_transaction(&self.conn, result)
    }

    pub fn load_recent_emoji(&self) -> Result<Vec<(String, u32)>> {
        let mut stmt = self.conn.prepare(
            "SELECT emoji, rating
             FROM cached_recent_emoji
             ORDER BY sort_index ASC",
        )?;
        let rows = stmt.query_map([], |row| {
            Ok((row.get::<_, String>(0)?, row.get::<_, i64>(1)? as u32))
        })?;
        let mut out = Vec::new();
        for row in rows {
            out.push(row?);
        }
        Ok(out)
    }

    pub fn save_recent_emoji(&self, items: &[(String, u32)]) -> Result<()> {
        self.conn.execute_batch("BEGIN IMMEDIATE")?;
        let result = (|| -> Result<()> {
            self.conn.execute("DELETE FROM cached_recent_emoji", [])?;
            let mut stmt = self.conn.prepare(
                "INSERT INTO cached_recent_emoji (sort_index, emoji, rating)
                 VALUES (?1, ?2, ?3)",
            )?;
            for (index, (emoji, rating)) in items.iter().enumerate() {
                stmt.execute(params![index as i64, emoji, *rating as i64])?;
            }
            Ok(())
        })();

        finish_transaction(&self.conn, result)
    }

    pub fn load_server_versions(&self, homeserver: &str) -> Result<Vec<MatrixVersion>> {
        let mut stmt = self.conn.prepare(
            "SELECT version
             FROM server_versions
             WHERE homeserver = ?1
             ORDER BY sort_index ASC",
        )?;
        let rows = stmt.query_map(params![homeserver], |row| row.get::<_, String>(0))?;

        let mut result = Vec::new();
        for row in rows {
            if let Some(version) = parse_matrix_version(&row?) {
                result.push(version);
            }
        }
        if result.is_empty() {
            Err(anyhow!("No cached server versions"))
        } else {
            Ok(result)
        }
    }

    pub fn save_server_versions_from_json(&self, homeserver: &str, body: &[u8]) -> Result<()> {
        let json: serde_json::Value = serde_json::from_slice(body)?;
        let versions = json
            .get("versions")
            .and_then(|v| v.as_array())
            .ok_or_else(|| anyhow!("Missing versions array"))?;

        self.conn.execute_batch("BEGIN IMMEDIATE")?;
        let result = (|| -> Result<()> {
            self.conn.execute(
                "DELETE FROM server_versions WHERE homeserver = ?1",
                params![homeserver],
            )?;
            let now = now_epoch();
            let mut stmt = self.conn.prepare(
                "INSERT INTO server_versions (homeserver, sort_index, version, fetched_at)
                 VALUES (?1, ?2, ?3, ?4)",
            )?;
            for (index, value) in versions.iter().enumerate() {
                if let Some(version) = value.as_str() {
                    stmt.execute(params![homeserver, index as i64, version, now])?;
                }
            }
            Ok(())
        })();

        finish_transaction(&self.conn, result)
    }

    pub fn clear_all(&self) -> Result<()> {
        self.conn.execute_batch(
            "DELETE FROM cached_room_filters;
			 DELETE FROM cached_rooms;
			 DELETE FROM cached_folders;
			 DELETE FROM cached_recent_emoji;
			 DELETE FROM server_versions;
			 VACUUM;",
        )?;
        Ok(())
    }
}

fn finish_transaction(conn: &Connection, result: Result<()>) -> Result<()> {
    match result {
        Ok(()) => {
            conn.execute_batch("COMMIT")?;
            Ok(())
        }
        Err(err) => {
            let _ = conn.execute_batch("ROLLBACK");
            Err(err)
        }
    }
}

fn empty_to_none(value: Option<String>) -> Option<String> {
    value.filter(|s| !s.is_empty())
}

fn now_epoch() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs() as i64
}

fn system_time_to_epoch(value: SystemTime) -> i64 {
    value
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs() as i64
}

fn epoch_to_system_time(value: i64) -> SystemTime {
    UNIX_EPOCH + Duration::from_secs(value.max(0) as u64)
}

fn notification_mode_from_u32(value: u32) -> RoomNotificationMode {
    match value {
        1 => RoomNotificationMode::MentionsOnly,
        2 => RoomNotificationMode::Mute,
        _ => RoomNotificationMode::AllMessages,
    }
}

fn send_state_from_u32(value: u32) -> SendState {
    match value {
        0 => SendState::Sending,
        3 => SendState::Failed,
        2 => SendState::Read,
        _ => SendState::Sent,
    }
}

fn membership_from_u32(value: u32) -> MembershipState {
    match value {
        1 => MembershipState::Invite,
        2 => MembershipState::Leave,
        3 => MembershipState::Ban,
        4 => MembershipState::Knock,
        _ => MembershipState::Join,
    }
}

fn parse_matrix_version(value: &str) -> Option<MatrixVersion> {
    match value {
        "v1.1" => Some(MatrixVersion::V1_1),
        "v1.2" => Some(MatrixVersion::V1_2),
        "v1.3" => Some(MatrixVersion::V1_3),
        "v1.4" => Some(MatrixVersion::V1_4),
        "v1.5" => Some(MatrixVersion::V1_5),
        _ if value.starts_with("v1.") || value.starts_with("r0.") => Some(MatrixVersion::V1_0),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::room_summary_service::RoomSummaryService;

    // Caller passes its own name: tests run in parallel threads of ONE process, and the
    // old `&() as *const ()` discriminator is a ZST pointer (always 0x1), so every test
    // shared a directory and wiped the others' database out from under them.
    fn temp_dir(name: &str) -> std::path::PathBuf {
        let dir =
            std::env::temp_dir().join(format!("tm-app-cache-test-{}-{name}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        dir
    }

    fn summary(room_id: &str, text: &str, secs: u64) -> RoomSummary {
        RoomSummary {
            room_id: room_id.into(),
            display_name: room_id.into(),
            canonical_alias: None,
            avatar_url: None,
            avatar_entity_id: String::new(),
            last_event_text: text.into(),
            last_event_sender: String::new(),
            last_event_timestamp: UNIX_EPOCH + Duration::from_secs(secs),
            unread_count: 0,
            highlight_count: 0,
            notification_mode: RoomNotificationMode::AllMessages,
            is_muted: false,
            is_pinned: false,
            pinned_order: None,
            is_marked_unread: false,
            is_direct: false,
            is_public: false,
            is_public_known: false,
            filter_ids: Vec::new(),
            space_ids: Vec::new(),
            is_last_event_outgoing: false,
            is_last_event_service: false,
            last_event_send_state: SendState::Sent,
            member_count: 0,
            can_pin_messages: false,
            peer_presence: 0,
            membership: MembershipState::Join,
            inviter_user_id: String::new(),
            inviter_display_name: String::new(),
            inviter_avatar_url: String::new(),
            room_topic: String::new(),
        }
    }

    // The restart flow that used to blank churny public rooms: a previous session persisted a real
    // last message; after restart the in-memory cache is empty and gets seeded from disk; sync
    // rebuilds a BLANK summary (newest event is a membership change); merging against the seeded
    // prior must restore the preview and its sort timestamp. Regresses if persistence drops the
    // preview fields or the seed/merge is skipped.
    #[test]
    fn persisted_preview_survives_restart_seed_and_merge() {
        let dir = temp_dir("preview-restart");
        {
            let store = AppCacheStore::open(&dir, "test-key").expect("open app cache");
            store
                .save_rooms(&[summary("!kde:x", "last real message", 100)])
                .expect("save rooms");
        }

        // Restart: reopen and load the snapshot to seed the (empty) in-memory cache.
        let store = AppCacheStore::open(&dir, "test-key").expect("reopen app cache");
        let seeded = store.load_rooms().expect("load rooms");
        assert_eq!(seeded.len(), 1);
        assert_eq!(seeded[0].last_event_text, "last real message");

        // Sync rebuilds a blank summary (system event newest) — merge must restore from the seed.
        let mut fresh = vec![summary("!kde:x", "", 0)];
        RoomSummaryService::merge_sticky_previews(&mut fresh, &seeded);
        assert_eq!(fresh[0].last_event_text, "last real message");
        assert_eq!(
            fresh[0].last_event_timestamp,
            UNIX_EPOCH + Duration::from_secs(100)
        );

        let _ = std::fs::remove_dir_all(&dir);
    }

    // Publicness must survive a restart. Room::is_public() answers from local state, so on a cold
    // start a not-yet-synced room reads as private — which silently disabled "hide system messages
    // in public rooms", since the open-time seed comes from this snapshot and the async correction
    // ignores an Unknown join rule by design.
    #[test]
    fn persisted_publicness_survives_restart() {
        let dir = temp_dir("publicness-restart");
        {
            let store = AppCacheStore::open(&dir, "test-key").expect("open app cache");
            let mut public = summary("!public:x", "hi", 100);
            public.is_public = true;
            public.is_public_known = true;
            let mut private = summary("!private:x", "hi", 100);
            private.is_public_known = true;
            store.save_rooms(&[public, private]).expect("save rooms");
        }

        let store = AppCacheStore::open(&dir, "test-key").expect("reopen app cache");
        let seeded = store.load_rooms().expect("load rooms");
        assert_eq!(seeded.len(), 2);
        assert!(seeded[0].is_public, "a public room must reload as public");
        assert!(!seeded[1].is_public, "a private room must stay private");

        let _ = std::fs::remove_dir_all(&dir);
    }

    // Known-ness must round-trip too, not just the flattened bool: a room whose join rule never
    // synced persists as NULL, and only that lets the next rebuild tell "not learned yet" from a
    // real private room (a rebuilt-unknown must not clobber a cached public — see
    // RoomSummaryService::preserve_publicness_if_unknown).
    #[test]
    fn persisted_publicness_distinguishes_unknown_from_private() {
        let dir = temp_dir("publicness-known");
        {
            let store = AppCacheStore::open(&dir, "test-key").expect("open app cache");
            let unknown = summary("!unknown:x", "hi", 100); // is_public_known: false
            let mut private = summary("!private:x", "hi", 100);
            private.is_public_known = true;
            let mut public = summary("!public:x", "hi", 100);
            public.is_public = true;
            public.is_public_known = true;
            store
                .save_rooms(&[unknown, private, public])
                .expect("save rooms");
        }

        let store = AppCacheStore::open(&dir, "test-key").expect("reopen app cache");
        let seeded = store.load_rooms().expect("load rooms");
        assert_eq!(seeded.len(), 3);
        assert!(
            !seeded[0].is_public_known,
            "a never-learned join rule must reload as unknown"
        );
        assert!(!seeded[0].is_public, "unknown still reads as private");
        assert!(
            seeded[1].is_public_known && !seeded[1].is_public,
            "a known-private room must reload as known-private"
        );
        assert!(
            seeded[2].is_public_known && seeded[2].is_public,
            "a known-public room must reload as known-public"
        );

        let _ = std::fs::remove_dir_all(&dir);
    }
}

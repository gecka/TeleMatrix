// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use anyhow::{anyhow, Result};
use rusqlite::{params, Connection};
use std::path::Path;
use tracing::warn;

/// FTS5 search index for E2EE room messages.
///
/// The index is always persisted through SQLCipher. If SQLCipher is not linked,
/// opening the index fails instead of falling back to plaintext or memory-only
/// storage.
pub struct SearchIndex {
    conn: Connection,
}

pub struct SearchHit {
    pub event_id: String,
    pub room_id: String,
    pub sender_id: String,
    pub sender_name: String,
    pub body: String,
    pub timestamp: i64,
}

impl SearchIndex {
    /// Open or create the SQLCipher-encrypted search index database.
    pub fn open(data_dir: &Path, key_material: Option<&str>) -> Result<Self> {
        std::fs::create_dir_all(data_dir)?;
        let db_path = data_dir.join("search_index.db");
        let key_material = key_material
            .filter(|value| !value.is_empty())
            .ok_or_else(|| anyhow!("missing E2EE search index key material"))?;

        match crate::encrypted_sqlite::open(&db_path, b"telematrix-search-index-v1", key_material)
            .and_then(|conn| {
                Self::init_schema(&conn)?;
                Ok(conn)
            }) {
            Ok(conn) => Ok(Self { conn }),
            Err(err) => {
                warn!("Failed to open encrypted search index ({err}); rebuilding");
                crate::encrypted_sqlite::delete_database_files(&db_path)?;
                let conn = crate::encrypted_sqlite::open(
                    &db_path,
                    b"telematrix-search-index-v1",
                    key_material,
                )?;
                Self::init_schema(&conn)?;
                Ok(Self { conn })
            }
        }
    }

    fn init_schema(conn: &Connection) -> Result<()> {
        conn.execute_batch(
            "
            CREATE TABLE IF NOT EXISTS messages (
                event_id TEXT PRIMARY KEY,
                room_id TEXT NOT NULL,
                sender_id TEXT NOT NULL,
                sender_name TEXT NOT NULL,
                body TEXT NOT NULL,
                timestamp INTEGER NOT NULL
            );

            CREATE VIRTUAL TABLE IF NOT EXISTS messages_fts USING fts5(
                body,
                content='messages',
                content_rowid='rowid'
            );

            -- Triggers to keep FTS index in sync.
            CREATE TRIGGER IF NOT EXISTS messages_ai AFTER INSERT ON messages BEGIN
                INSERT INTO messages_fts(rowid, body) VALUES (new.rowid, new.body);
            END;
            CREATE TRIGGER IF NOT EXISTS messages_ad AFTER DELETE ON messages BEGIN
                INSERT INTO messages_fts(messages_fts, rowid, body) VALUES('delete', old.rowid, old.body);
            END;
            CREATE TRIGGER IF NOT EXISTS messages_au AFTER UPDATE ON messages BEGIN
                INSERT INTO messages_fts(messages_fts, rowid, body) VALUES('delete', old.rowid, old.body);
                INSERT INTO messages_fts(rowid, body) VALUES (new.rowid, new.body);
            END;

            CREATE INDEX IF NOT EXISTS idx_messages_room ON messages(room_id);

            CREATE TABLE IF NOT EXISTS backfill_checkpoints (
                room_id TEXT PRIMARY KEY,
                prev_batch TEXT,
                is_done INTEGER NOT NULL DEFAULT 0
            );
        ",
        )?;
        Ok(())
    }

    /// Index a single decrypted message.
    pub fn index_message(
        &self,
        event_id: &str,
        room_id: &str,
        sender_id: &str,
        sender_name: &str,
        body: &str,
        timestamp: i64,
    ) -> Result<()> {
        self.conn.execute(
            "INSERT OR REPLACE INTO messages
             (event_id, room_id, sender_id, sender_name, body, timestamp)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
            params![event_id, room_id, sender_id, sender_name, body, timestamp],
        )?;
        Ok(())
    }

    /// Batch index multiple messages (uses a transaction for speed).
    pub fn index_batch(
        &self,
        messages: &[(String, String, String, String, String, i64)],
    ) -> Result<()> {
        let tx = self.conn.unchecked_transaction()?;
        {
            let mut stmt = tx.prepare(
                "INSERT OR REPLACE INTO messages
                 (event_id, room_id, sender_id, sender_name, body, timestamp)
                 VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
            )?;
            for (event_id, room_id, sender_id, sender_name, body, timestamp) in messages {
                stmt.execute(params![
                    event_id,
                    room_id,
                    sender_id,
                    sender_name,
                    body,
                    timestamp
                ])?;
            }
        }
        tx.commit()?;
        Ok(())
    }

    /// Search messages using FTS5 full-text search.
    pub fn search(
        &self,
        query: &str,
        room_id: Option<&str>,
        sender_id: Option<&str>,
        limit: usize,
        offset: usize,
    ) -> Result<(Vec<SearchHit>, u32)> {
        if query.trim().is_empty() {
            return Ok((Vec::new(), 0));
        }

        // Build FTS5 query: escape special characters and add prefix matching.
        let fts_query = Self::build_fts_query(query);

        // Count total matches first.
        let count = self.count_matches(&fts_query, room_id, sender_id)?;

        // Fetch page of results.
        let mut sql = String::from(
            "SELECT m.event_id, m.room_id, m.sender_id, m.sender_name, m.body, m.timestamp
             FROM messages m
             JOIN messages_fts ON messages_fts.rowid = m.rowid
             WHERE messages_fts MATCH ?1",
        );
        let mut param_values: Vec<Box<dyn rusqlite::types::ToSql>> = Vec::new();
        param_values.push(Box::new(fts_query.clone()));

        if let Some(rid) = room_id {
            sql += " AND m.room_id = ?2";
            param_values.push(Box::new(rid.to_string()));
        }
        if let Some(sid) = sender_id {
            let idx = param_values.len() + 1;
            sql.push_str(&format!(" AND m.sender_id = ?{idx}"));
            param_values.push(Box::new(sid.to_string()));
        }

        sql += " ORDER BY rank, m.timestamp DESC";
        sql.push_str(&format!(" LIMIT {limit} OFFSET {offset}"));

        let mut stmt = self.conn.prepare(&sql)?;
        let params_ref: Vec<&dyn rusqlite::types::ToSql> =
            param_values.iter().map(|p| p.as_ref()).collect();
        let hits = stmt
            .query_map(params_ref.as_slice(), |row| {
                Ok(SearchHit {
                    event_id: row.get(0)?,
                    room_id: row.get(1)?,
                    sender_id: row.get(2)?,
                    sender_name: row.get(3)?,
                    body: row.get(4)?,
                    timestamp: row.get(5)?,
                })
            })?
            .filter_map(|r| r.ok())
            .collect();

        Ok((hits, count))
    }

    /// Recency-ordered, keyset-paginated full-text search across **all** rooms.
    ///
    /// Unlike [`Self::search`] (which is room-scoped and ranks by FTS relevance),
    /// this orders strictly by `(timestamp DESC, event_id DESC)` so its output can
    /// be merged with the server-side `OrderBy::Recent` results for global search.
    ///
    /// `before` is an exclusive upper bound `(timestamp, event_id)` for keyset
    /// pagination: pass `None` for the first page, then the last returned hit's
    /// `(timestamp, event_id)` for each subsequent page. Keyset (rather than
    /// LIMIT/OFFSET) means newly indexed messages can't shift rows across page
    /// boundaries and cause drops or duplicates.
    pub fn search_global(
        &self,
        query: &str,
        sender_id: Option<&str>,
        before: Option<(i64, &str)>,
        limit: usize,
    ) -> Result<Vec<SearchHit>> {
        if query.trim().is_empty() {
            return Ok(Vec::new());
        }

        let fts_query = Self::build_fts_query(query);

        let mut sql = String::from(
            "SELECT m.event_id, m.room_id, m.sender_id, m.sender_name, m.body, m.timestamp
             FROM messages m
             JOIN messages_fts ON messages_fts.rowid = m.rowid
             WHERE messages_fts MATCH ?1",
        );
        let mut param_values: Vec<Box<dyn rusqlite::types::ToSql>> = Vec::new();
        param_values.push(Box::new(fts_query.clone()));

        if let Some(sid) = sender_id {
            let idx = param_values.len() + 1;
            sql.push_str(&format!(" AND m.sender_id = ?{idx}"));
            param_values.push(Box::new(sid.to_string()));
        }
        if let Some((ts, ev)) = before {
            // Keyset upper bound (exclusive): rows strictly older than (ts, ev) in
            // (timestamp DESC, event_id DESC) order. ?ts is referenced twice.
            let ts_idx = param_values.len() + 1;
            let ev_idx = param_values.len() + 2;
            sql.push_str(&format!(
                " AND (m.timestamp < ?{ts_idx} \
                   OR (m.timestamp = ?{ts_idx} AND m.event_id < ?{ev_idx}))"
            ));
            param_values.push(Box::new(ts));
            param_values.push(Box::new(ev.to_string()));
        }

        sql += " ORDER BY m.timestamp DESC, m.event_id DESC";
        sql.push_str(&format!(" LIMIT {limit}"));

        let mut stmt = self.conn.prepare(&sql)?;
        let params_ref: Vec<&dyn rusqlite::types::ToSql> =
            param_values.iter().map(|p| p.as_ref()).collect();
        let hits = stmt
            .query_map(params_ref.as_slice(), |row| {
                Ok(SearchHit {
                    event_id: row.get(0)?,
                    room_id: row.get(1)?,
                    sender_id: row.get(2)?,
                    sender_name: row.get(3)?,
                    body: row.get(4)?,
                    timestamp: row.get(5)?,
                })
            })?
            .filter_map(|r| r.ok())
            .collect();

        Ok(hits)
    }

    /// Total number of matches across all rooms (for `total_approx` reporting).
    pub fn count_global(&self, query: &str, sender_id: Option<&str>) -> Result<u32> {
        if query.trim().is_empty() {
            return Ok(0);
        }
        let fts_query = Self::build_fts_query(query);
        self.count_matches(&fts_query, None, sender_id)
    }

    fn count_matches(
        &self,
        fts_query: &str,
        room_id: Option<&str>,
        sender_id: Option<&str>,
    ) -> Result<u32> {
        let mut sql = String::from(
            "SELECT COUNT(*) FROM messages m
             JOIN messages_fts ON messages_fts.rowid = m.rowid
             WHERE messages_fts MATCH ?1",
        );
        let mut param_values: Vec<Box<dyn rusqlite::types::ToSql>> = Vec::new();
        param_values.push(Box::new(fts_query.to_string()));

        if let Some(rid) = room_id {
            sql += " AND m.room_id = ?2";
            param_values.push(Box::new(rid.to_string()));
        }
        if let Some(sid) = sender_id {
            let idx = param_values.len() + 1;
            sql.push_str(&format!(" AND m.sender_id = ?{idx}"));
            param_values.push(Box::new(sid.to_string()));
        }

        let params_ref: Vec<&dyn rusqlite::types::ToSql> =
            param_values.iter().map(|p| p.as_ref()).collect();
        let count: u32 = self
            .conn
            .query_row(&sql, params_ref.as_slice(), |row| row.get(0))?;
        Ok(count)
    }

    /// Build FTS5-compatible query from user input.
    /// Wraps each word in quotes for exact token matching with prefix support.
    fn build_fts_query(query: &str) -> String {
        query
            .split_whitespace()
            .map(|word| {
                // Escape double quotes in the word.
                let escaped = word.replace('"', "\"\"");
                format!("\"{escaped}\"*")
            })
            .collect::<Vec<_>>()
            .join(" ")
    }

    /// Get the pagination token for backfill in a room (None = start from latest).
    pub fn get_backfill_checkpoint(&self, room_id: &str) -> Result<Option<String>> {
        let mut stmt = self.conn.prepare(
            "SELECT prev_batch FROM backfill_checkpoints WHERE room_id = ?1 AND is_done = 0",
        )?;
        let token = stmt
            .query_row(params![room_id], |row| row.get::<_, String>(0))
            .ok();
        Ok(token)
    }

    /// Save a pagination token for resumable backfill.
    pub fn set_backfill_checkpoint(&self, room_id: &str, prev_batch: &str) -> Result<()> {
        self.conn.execute(
            "INSERT OR REPLACE INTO backfill_checkpoints (room_id, prev_batch, is_done)
             VALUES (?1, ?2, 0)",
            params![room_id, prev_batch],
        )?;
        Ok(())
    }

    /// Mark backfill as complete for a room (reached beginning of history).
    pub fn mark_backfill_done(&self, room_id: &str) -> Result<()> {
        self.conn.execute(
            "INSERT OR REPLACE INTO backfill_checkpoints (room_id, prev_batch, is_done)
             VALUES (?1, '', 1)",
            params![room_id],
        )?;
        Ok(())
    }

    /// Check if backfill is already complete for a room.
    pub fn is_backfill_done(&self, room_id: &str) -> Result<bool> {
        let done: bool = self
            .conn
            .query_row(
                "SELECT is_done FROM backfill_checkpoints WHERE room_id = ?1",
                params![room_id],
                |row| row.get(0),
            )
            .unwrap_or(false);
        Ok(done)
    }

    /// Delete all indexed messages for a specific room.
    pub fn clear_room(&self, room_id: &str) -> Result<()> {
        self.conn
            .execute("DELETE FROM messages WHERE room_id = ?1", params![room_id])?;
        self.conn.execute(
            "DELETE FROM backfill_checkpoints WHERE room_id = ?1",
            params![room_id],
        )?;
        Ok(())
    }

    /// Delete all indexed messages.
    pub fn clear_all(&self) -> Result<()> {
        self.conn.execute_batch(
            "
            DELETE FROM messages;
            DELETE FROM backfill_checkpoints;
            INSERT INTO messages_fts(messages_fts) VALUES('rebuild');
        ",
        )?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;
    use std::time::{SystemTime, UNIX_EPOCH};

    #[test]
    fn linked_sqlite_has_sqlcipher() {
        crate::encrypted_sqlite::require_sqlcipher_available()
            .expect("E2EE search persistence requires rusqlite bundled-sqlcipher support");
    }

    fn test_index(name: &str) -> (SearchIndex, PathBuf) {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let dir = std::env::temp_dir().join(format!(
            "telematrix-search-index-{name}-{}-{nanos}",
            std::process::id()
        ));
        let idx = SearchIndex::open(&dir, Some("test-key-material")).expect("open index");
        (idx, dir)
    }

    /// (event_id, room_id, sender_id, sender_name, body, timestamp)
    fn msg(
        event_id: &str,
        room_id: &str,
        sender: &str,
        ts: i64,
    ) -> (String, String, String, String, String, i64) {
        (
            event_id.to_string(),
            room_id.to_string(),
            sender.to_string(),
            sender.to_string(),
            "hello world".to_string(),
            ts,
        )
    }

    #[test]
    fn search_global_orders_by_recency_desc() {
        let (idx, dir) = test_index("recency");
        idx.index_batch(&[
            msg("$e1", "!r:s", "@a:s", 100),
            msg("$e3", "!r:s", "@a:s", 300),
            msg("$e2", "!r:s", "@a:s", 200),
        ])
        .unwrap();

        let hits = idx.search_global("hello", None, None, 10).unwrap();
        let ids: Vec<&str> = hits.iter().map(|h| h.event_id.as_str()).collect();
        assert_eq!(ids, vec!["$e3", "$e2", "$e1"]);

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn search_global_keyset_pagination_has_no_gaps_or_overlap() {
        let (idx, dir) = test_index("keyset");
        idx.index_batch(&[
            msg("$e1", "!r:s", "@a:s", 100),
            msg("$e2", "!r:s", "@a:s", 200),
            msg("$e3", "!r:s", "@a:s", 300),
        ])
        .unwrap();

        let page1 = idx.search_global("hello", None, None, 2).unwrap();
        let ids1: Vec<&str> = page1.iter().map(|h| h.event_id.as_str()).collect();
        assert_eq!(ids1, vec!["$e3", "$e2"]);

        let last = page1.last().unwrap();
        let page2 = idx
            .search_global("hello", None, Some((last.timestamp, &last.event_id)), 2)
            .unwrap();
        let ids2: Vec<&str> = page2.iter().map(|h| h.event_id.as_str()).collect();
        assert_eq!(ids2, vec!["$e1"]);

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn search_global_breaks_timestamp_ties_by_event_id() {
        let (idx, dir) = test_index("ties");
        // Two messages share a timestamp; keyset must page through both exactly once.
        idx.index_batch(&[
            msg("$a", "!r:s", "@a:s", 500),
            msg("$b", "!r:s", "@a:s", 500),
        ])
        .unwrap();

        let page1 = idx.search_global("hello", None, None, 1).unwrap();
        assert_eq!(page1.len(), 1);
        // event_id DESC tie-break => "$b" before "$a".
        assert_eq!(page1[0].event_id, "$b");

        let last = &page1[0];
        let page2 = idx
            .search_global("hello", None, Some((last.timestamp, &last.event_id)), 1)
            .unwrap();
        assert_eq!(page2.len(), 1);
        assert_eq!(page2[0].event_id, "$a");

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn search_global_filters_by_sender() {
        let (idx, dir) = test_index("sender");
        idx.index_batch(&[
            msg("$e1", "!r:s", "@a:s", 100),
            msg("$e2", "!r:s", "@b:s", 200),
        ])
        .unwrap();

        let hits = idx.search_global("hello", Some("@a:s"), None, 10).unwrap();
        let ids: Vec<&str> = hits.iter().map(|h| h.event_id.as_str()).collect();
        assert_eq!(ids, vec!["$e1"]);

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn search_global_empty_query_returns_nothing() {
        let (idx, dir) = test_index("empty");
        idx.index_batch(&[msg("$e1", "!r:s", "@a:s", 100)]).unwrap();
        assert!(idx.search_global("   ", None, None, 10).unwrap().is_empty());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn delete_database_files_removes_index_and_wal_shm() {
        // The contract the E2EE-search disable path relies on: after dropping the
        // connection, delete_database_files removes the .db + WAL/SHM so disabling
        // search reclaims disk (matrix.rs::set_e2ee_search_enabled(false)).
        let (idx, dir) = test_index("delete");
        let db = dir.join("search_index.db");
        idx.index_batch(&[msg("$e1", "!r:s", "@a:s", 100)]).unwrap();
        assert!(db.exists(), "db exists after open+index");
        drop(idx); // release the SQLite connection, as `*guard = None` does
        crate::encrypted_sqlite::delete_database_files(&db).unwrap();
        assert!(!db.exists(), "db removed");
        assert!(!dir.join("search_index.db-wal").exists(), "wal removed");
        assert!(!dir.join("search_index.db-shm").exists(), "shm removed");
        let _ = std::fs::remove_dir_all(&dir);
    }
}

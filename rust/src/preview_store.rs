// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Persistent URL preview cache backed by SQLCipher.
//!
//! Stores fetched URL previews so that `cache_timeline_snapshot` can apply them
//! synchronously on first render, eliminating layout shifts when loading focused
//! timelines (jump-to-message) for previously-seen URLs.

use std::collections::HashMap;
use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

use rusqlite::{params, Connection};
use tracing::warn;

use crate::types::{PreviewType, UrlPreview};

/// Persistent store for URL preview metadata.
pub struct PreviewStore {
    conn: Connection,
}

impl PreviewStore {
    /// Open (or create) the encrypted preview database at `{data_dir}/preview_cache.db`.
    pub fn open(data_dir: &Path, key_material: &str) -> anyhow::Result<Self> {
        let db_path = data_dir.join("preview_cache.db");
        let conn = match crate::encrypted_sqlite::open(
            &db_path,
            b"telematrix-preview-cache-v1",
            key_material,
        )
        .and_then(|conn| {
            Self::init_schema(&conn)?;
            Ok(conn)
        }) {
            Ok(conn) => conn,
            Err(err) => {
                warn!("Failed to open encrypted preview cache ({err}); rebuilding");
                crate::encrypted_sqlite::delete_database_files(&db_path)?;
                let conn = crate::encrypted_sqlite::open(
                    &db_path,
                    b"telematrix-preview-cache-v1",
                    key_material,
                )?;
                Self::init_schema(&conn)?;
                conn
            }
        };
        Ok(Self { conn })
    }

    fn init_schema(conn: &Connection) -> anyhow::Result<()> {
        // WAL mode for better concurrent read performance.
        conn.pragma_update(None, "journal_mode", "WAL")?;

        let version: u32 = conn.pragma_query_value(None, "user_version", |r| r.get(0))?;
        if version == 0 {
            conn.execute_batch(
                "CREATE TABLE IF NOT EXISTS url_previews (
                    url                 TEXT PRIMARY KEY,
                    site_name           TEXT,
                    title               TEXT,
                    description         TEXT,
                    image_url           TEXT,
                    image_width         INTEGER NOT NULL DEFAULT 0,
                    image_height        INTEGER NOT NULL DEFAULT 0,
                    preview_type        INTEGER NOT NULL DEFAULT 1,
                    duration_secs       INTEGER NOT NULL DEFAULT 0,
                    author              TEXT,
                    has_large_media     INTEGER NOT NULL DEFAULT 0,
                    site_name_canonical TEXT,
                    is_none             INTEGER NOT NULL DEFAULT 0,
                    fetched_at          INTEGER NOT NULL
                );
                PRAGMA user_version = 1;",
            )?;
        }
        Ok(())
    }

    /// Load all cached previews into a HashMap matching the in-memory
    /// `preview_cache` format.
    pub fn load_all(&self) -> anyhow::Result<HashMap<String, Option<UrlPreview>>> {
        let mut stmt = self.conn.prepare(
            "SELECT url, site_name, title, description, image_url,
                    image_width, image_height, preview_type, duration_secs,
                    author, has_large_media, site_name_canonical, is_none
             FROM url_previews",
        )?;

        let mut map = HashMap::new();
        let rows = stmt.query_map([], |row| {
            let url: String = row.get(0)?;
            let is_none: bool = row.get(12)?;
            if is_none {
                Ok((url, None))
            } else {
                Ok((
                    url.clone(),
                    Some(UrlPreview {
                        url,
                        site_name: row.get(1)?,
                        title: row.get(2)?,
                        description: row.get(3)?,
                        image_url: row.get(4)?,
                        image_width: row.get::<_, u32>(5)?,
                        image_height: row.get::<_, u32>(6)?,
                        preview_type: PreviewType::from_u32(row.get::<_, u32>(7)?),
                        duration_secs: row.get(8)?,
                        author: row.get(9)?,
                        has_large_media: row.get(10)?,
                        site_name_canonical: row.get(11)?,
                    }),
                ))
            }
        })?;

        for row in rows {
            let (url, preview) = row?;
            map.insert(url, preview);
        }

        Ok(map)
    }

    /// Save a single URL preview (or negative result) to the store.
    pub fn save(&self, url: &str, preview: &Option<UrlPreview>) -> anyhow::Result<()> {
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs() as i64;

        match preview {
            Some(p) => {
                self.conn.execute(
                    "INSERT OR REPLACE INTO url_previews
                     (url, site_name, title, description, image_url,
                      image_width, image_height, preview_type, duration_secs,
                      author, has_large_media, site_name_canonical, is_none, fetched_at)
                     VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, 0, ?13)",
                    params![
                        url,
                        p.site_name,
                        p.title,
                        p.description,
                        p.image_url,
                        p.image_width,
                        p.image_height,
                        p.preview_type as u32,
                        p.duration_secs,
                        p.author,
                        p.has_large_media,
                        p.site_name_canonical,
                        now,
                    ],
                )?;
            }
            None => {
                self.conn.execute(
                    "INSERT OR REPLACE INTO url_previews
                     (url, is_none, fetched_at)
                     VALUES (?1, 1, ?2)",
                    params![url, now],
                )?;
            }
        }
        Ok(())
    }

    /// Delete all cached previews (called on logout).
    pub fn delete_all(&self) -> anyhow::Result<()> {
        self.conn.execute("DELETE FROM url_previews", [])?;
        Ok(())
    }

    /// Purge negative-cached entries whose URL has an unbalanced '(' with no
    /// ')'. The old extractor blindly trimmed a trailing ')', so a link like
    /// `..._(programming_language)` was fetched as `..._(programming_language`,
    /// got no OG data, and was negative-cached for 30 days. Now that extraction
    /// keeps balanced parens, drop those poisoned rows so the corrected URL is
    /// fetched fresh instead of waiting out the expiry.
    pub fn delete_unbalanced_paren_negatives(&self) -> anyhow::Result<u64> {
        let deleted = self.conn.execute(
            "DELETE FROM url_previews
             WHERE is_none = 1 AND url LIKE '%(%' AND url NOT LIKE '%)%'",
            [],
        )?;
        Ok(deleted as u64)
    }

    /// Delete entries older than `max_age_days`.
    pub fn delete_expired(&self, max_age_days: u32) -> anyhow::Result<u64> {
        let cutoff = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs() as i64
            - (max_age_days as i64 * 86400);

        let deleted = self.conn.execute(
            "DELETE FROM url_previews WHERE fetched_at < ?1",
            params![cutoff],
        )?;
        Ok(deleted as u64)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU32, Ordering};

    static COUNTER: AtomicU32 = AtomicU32::new(0);

    fn temp_store() -> (PreviewStore, std::path::PathBuf) {
        let dir = std::env::temp_dir().join(format!(
            "tm_preview_store_test_{}_{}",
            std::process::id(),
            COUNTER.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::create_dir_all(&dir).unwrap();
        let store = PreviewStore::open(&dir, "test-key-material").unwrap();
        (store, dir)
    }

    fn positive(url: &str) -> UrlPreview {
        UrlPreview {
            url: url.to_string(),
            site_name: None,
            title: Some("t".to_string()),
            description: None,
            image_url: None,
            image_width: 0,
            image_height: 0,
            preview_type: PreviewType::Article,
            duration_secs: 0,
            author: None,
            has_large_media: false,
            site_name_canonical: None,
        }
    }

    // Only negative entries with an unbalanced '(' (no ')') are purged; balanced
    // negatives, plain negatives, and any positive entry are kept.
    #[test]
    fn purges_only_unbalanced_paren_negatives() {
        let (store, dir) = temp_store();
        store
            .save("https://x.com/wiki/Rust_(programming_language", &None)
            .unwrap(); // poisoned negative -> purge
        store.save("https://x.com/a(b)", &None).unwrap(); // balanced negative -> keep
        store.save("https://x.com/plain", &None).unwrap(); // plain negative -> keep
        let pos = positive("https://x.com/pos_(");
        store.save("https://x.com/pos_(", &Some(pos)).unwrap(); // positive, unbalanced -> keep (is_none guard)

        let removed = store.delete_unbalanced_paren_negatives().unwrap();
        assert_eq!(removed, 1);

        let all = store.load_all().unwrap();
        assert!(!all.contains_key("https://x.com/wiki/Rust_(programming_language"));
        assert!(all.contains_key("https://x.com/a(b)"));
        assert!(all.contains_key("https://x.com/plain"));
        assert!(all.contains_key("https://x.com/pos_("));

        drop(store);
        let _ = std::fs::remove_dir_all(&dir);
    }
}

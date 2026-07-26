// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::path::{Path, PathBuf};

use anyhow::{anyhow, Result};
use hkdf::Hkdf;
use rusqlite::Connection;
use sha2::Sha256;

const SQLCIPHER_KEY_INFO: &[u8] = b"telematrix-sqlcipher-key-v2";

fn bytes_to_hex(bytes: &[u8]) -> String {
    let mut hex = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        use std::fmt::Write as _;
        let _ = write!(&mut hex, "{byte:02x}");
    }
    hex
}

pub fn require_sqlcipher_available() -> Result<()> {
    let conn = Connection::open_in_memory()?;
    let version = conn
        .query_row("PRAGMA cipher_version", [], |row| row.get::<_, String>(0))
        .map_err(|err| anyhow!("SQLCipher is required but unavailable: {err}"))?;
    if version.trim().is_empty() {
        return Err(anyhow!("SQLCipher is required but cipher_version is empty"));
    }
    Ok(())
}

pub fn derive_key(context: &[u8], key_material: &str) -> Result<String> {
    if key_material.is_empty() {
        return Err(anyhow!("missing encrypted sqlite key material"));
    }
    let hkdf = Hkdf::<Sha256>::new(Some(context), key_material.as_bytes());
    let mut key = [0u8; 32];
    hkdf.expand(SQLCIPHER_KEY_INFO, &mut key)
        .map_err(|_| anyhow!("failed to derive encrypted sqlite key"))?;
    Ok(bytes_to_hex(&key))
}

pub fn open(db_path: &Path, context: &[u8], key_material: &str) -> Result<Connection> {
    require_sqlcipher_available()?;
    let key = derive_key(context, key_material)?;
    let conn = Connection::open(db_path)?;
    conn.pragma_update(None, "key", format!("x'{key}'"))?;
    conn.query_row("PRAGMA cipher_version", [], |row| row.get::<_, String>(0))
        .map_err(|err| anyhow!("SQLCipher did not accept key: {err}"))?;
    Ok(conn)
}

pub fn delete_database_files(db_path: &Path) -> Result<()> {
    for suffix in ["", "-wal", "-shm"] {
        let path = if suffix.is_empty() {
            db_path.to_path_buf()
        } else {
            PathBuf::from(format!("{}{}", db_path.to_string_lossy(), suffix))
        };
        match std::fs::remove_file(&path) {
            Ok(()) => {}
            Err(err) if err.kind() == std::io::ErrorKind::NotFound => {}
            Err(err) => {
                return Err(anyhow!(
                    "failed to delete sqlite database file {}: {err}",
                    path.display()
                ));
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bytes_to_hex_lowercase_two_digits() {
        assert_eq!(bytes_to_hex(&[]), "");
        // Pins the `{:02x}` format: 0x00 keeps both digits, 0xff is lowercase.
        assert_eq!(bytes_to_hex(&[0x00, 0xff, 0x0a, 0xab]), "00ff0aab");
    }

    #[test]
    fn derive_key_rejects_empty_material() {
        assert!(derive_key(b"context", "").is_err());
    }

    #[test]
    fn derive_key_is_deterministic_and_32_bytes() {
        let a = derive_key(b"app-cache", "passphrase").unwrap();
        let b = derive_key(b"app-cache", "passphrase").unwrap();
        assert_eq!(a, b, "HKDF must be deterministic for the same inputs");
        // 32-byte key rendered as hex = 64 lowercase hex chars.
        assert_eq!(a.len(), 64);
        assert!(a.bytes().all(|c| c.is_ascii_hexdigit()));
    }

    #[test]
    fn derive_key_separates_by_context_and_material() {
        let base = derive_key(b"app-cache", "passphrase").unwrap();
        // Different context (domain separation) must change the key.
        assert_ne!(base, derive_key(b"media-cache", "passphrase").unwrap());
        // Different key material must change the key.
        assert_ne!(base, derive_key(b"app-cache", "other").unwrap());
    }
}

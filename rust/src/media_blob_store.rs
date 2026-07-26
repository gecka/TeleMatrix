// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::io::ErrorKind;
use std::path::{Path, PathBuf};

use anyhow::{anyhow, Result};
use chacha20poly1305::aead::{Aead, KeyInit, Payload};
use chacha20poly1305::{Key, XChaCha20Poly1305, XNonce};
use hkdf::Hkdf;
use rand::{rngs::SysRng, TryRng};
use sha2::{Digest, Sha256};
use tokio::io::{AsyncReadExt, AsyncWriteExt};

const MAGIC: &[u8; 8] = b"TMMED02\0";
const CHUNK_SIZE: usize = 1024 * 1024;
const NONCE_PREFIX_LEN: usize = 16;
const TAG_LEN: usize = 16;
const MEDIA_KEY_INFO: &[u8] = b"telematrix-media-cache-key-v2";
const MEDIA_KEY_SALT: &[u8] = b"telematrix-media-cache-v2";

pub fn hash_cache_key(cache_key: &str) -> String {
    let mut hasher = Sha256::new();
    hasher.update(cache_key.as_bytes());
    bytes_to_hex(&hasher.finalize())
}

fn bytes_to_hex(bytes: &[u8]) -> String {
    let mut hex = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        use std::fmt::Write as _;
        let _ = write!(&mut hex, "{byte:02x}");
    }
    hex
}

fn plaintext_temp_root(data_dir: &Path) -> PathBuf {
    let mut hasher = Sha256::new();
    hasher.update(data_dir.to_string_lossy().as_bytes());
    let digest = hasher.finalize();
    std::env::temp_dir()
        .join("telematrix-media-cache")
        .join(bytes_to_hex(&digest[..8]))
}

pub fn encrypted_path(media_cache_dir: &Path, hash: &str) -> PathBuf {
    media_cache_dir.join(format!("{hash}.tmenc"))
}

/// Encrypted path for a derived thumbnail, kept in a separate `thumbnails/`
/// namespace so it survives media-cache pruning policies independently and
/// never collides with the full-media blobs sharing the same cache dir.
pub fn thumbnail_encrypted_path(media_cache_dir: &Path, hash: &str) -> PathBuf {
    media_cache_dir
        .join("thumbnails")
        .join(format!("{hash}.tmenc"))
}

/// Store thumbnail bytes encrypted at rest in the `thumbnails/` namespace.
pub async fn store_thumbnail_bytes(
    key_material: &str,
    aad: &[u8],
    media_cache_dir: &Path,
    hash: &str,
    bytes: &[u8],
) -> Result<()> {
    let path = thumbnail_encrypted_path(media_cache_dir, hash);
    encrypt_bytes(key_material, aad, bytes, &path).await
}

/// Load and decrypt thumbnail bytes from the `thumbnails/` namespace, or
/// `None` if no cached thumbnail exists for this hash.
pub async fn load_thumbnail_bytes(
    key_material: &str,
    aad: &[u8],
    media_cache_dir: &Path,
    hash: &str,
) -> Result<Option<Vec<u8>>> {
    let path = thumbnail_encrypted_path(media_cache_dir, hash);
    if !path.exists() {
        return Ok(None);
    }
    decrypt_to_bytes(key_material, aad, &path).await.map(Some)
}

pub fn plaintext_temp_path(data_dir: &Path, hash: &str, extension: Option<&str>) -> PathBuf {
    let mut name = hash.to_string();
    if let Some(extension) = extension.filter(|value| !value.is_empty()) {
        name.push('.');
        name.push_str(extension.trim_start_matches('.'));
    }
    plaintext_temp_root(data_dir).join(name)
}

pub fn work_path(data_dir: &Path, hash: &str, kind: &str) -> PathBuf {
    plaintext_temp_root(data_dir).join(format!("{hash}.{kind}"))
}

pub fn delete_plaintext_temp_files(data_dir: &Path) {
    let _ = std::fs::remove_dir_all(plaintext_temp_root(data_dir));
    let media_cache_dir = data_dir.join("media_cache");
    if let Ok(entries) = std::fs::read_dir(&media_cache_dir) {
        for entry in entries.flatten() {
            let path = entry.path();
            if !path.is_file() {
                continue;
            }
            let keep = path.extension().and_then(|v| v.to_str()) == Some("tmenc");
            if !keep {
                let _ = std::fs::remove_file(path);
            }
        }
    }
}

fn media_key(key_material: &str) -> Result<[u8; 32]> {
    if key_material.is_empty() {
        return Err(anyhow!("missing encrypted media cache key material"));
    }
    let hkdf = Hkdf::<Sha256>::new(Some(MEDIA_KEY_SALT), key_material.as_bytes());
    let mut key = [0u8; 32];
    hkdf.expand(MEDIA_KEY_INFO, &mut key)
        .map_err(|_| anyhow!("failed to derive encrypted media cache key"))?;
    Ok(key)
}

fn legacy_media_key(key_material: &str) -> Result<[u8; 32]> {
    if key_material.is_empty() {
        return Err(anyhow!("missing encrypted media cache key material"));
    }
    let mut hasher = Sha256::new();
    hasher.update(b"telematrix-media-cache-v1\0");
    hasher.update(key_material.as_bytes());
    Ok(hasher.finalize().into())
}

fn cipher(key_material: &str) -> Result<XChaCha20Poly1305> {
    let key = media_key(key_material)?;
    Ok(XChaCha20Poly1305::new(&Key::from(key)))
}

fn legacy_cipher(key_material: &str) -> Result<XChaCha20Poly1305> {
    let key = legacy_media_key(key_material)?;
    Ok(XChaCha20Poly1305::new(&Key::from(key)))
}

fn chunk_nonce(prefix: &[u8; NONCE_PREFIX_LEN], index: u64) -> XNonce {
    let mut nonce = [0u8; 24];
    nonce[..NONCE_PREFIX_LEN].copy_from_slice(prefix);
    nonce[NONCE_PREFIX_LEN..].copy_from_slice(&index.to_le_bytes());
    XNonce::from(nonce)
}

fn chunk_aad(aad: &[u8], index: u64) -> Vec<u8> {
    let mut result = Vec::with_capacity(aad.len() + 8);
    result.extend_from_slice(aad);
    result.extend_from_slice(&index.to_le_bytes());
    result
}

async fn ensure_parent(path: &Path) -> Result<()> {
    if let Some(parent) = path.parent() {
        tokio::fs::create_dir_all(parent).await?;
    }
    Ok(())
}

pub async fn encrypt_file(
    key_material: &str,
    aad: &[u8],
    plaintext_path: &Path,
    encrypted_path: &Path,
) -> Result<()> {
    ensure_parent(encrypted_path).await?;
    let temp_path = encrypted_path.with_extension("tmenc-writing");
    let _ = tokio::fs::remove_file(&temp_path).await;

    let cipher = cipher(key_material)?;
    let mut prefix = [0u8; NONCE_PREFIX_LEN];
    SysRng.try_fill_bytes(&mut prefix)?;

    let mut input = tokio::fs::File::open(plaintext_path).await?;
    let mut output = tokio::fs::File::create(&temp_path).await?;
    output.write_all(MAGIC).await?;
    output.write_all(&prefix).await?;

    let mut index = 0u64;
    let mut buffer = vec![0u8; CHUNK_SIZE];
    loop {
        let read = input.read(&mut buffer).await?;
        if read == 0 {
            break;
        }
        let aad = chunk_aad(aad, index);
        let encrypted = cipher
            .encrypt(
                &chunk_nonce(&prefix, index),
                Payload {
                    msg: &buffer[..read],
                    aad: &aad,
                },
            )
            .map_err(|_| anyhow!("media cache encryption failed"))?;
        output
            .write_all(&(encrypted.len() as u32).to_le_bytes())
            .await?;
        output.write_all(&encrypted).await?;
        index = index
            .checked_add(1)
            .ok_or_else(|| anyhow!("media cache chunk index overflow"))?;
    }
    output.flush().await?;
    drop(output);
    tokio::fs::rename(&temp_path, encrypted_path).await?;
    Ok(())
}

pub async fn encrypt_bytes(
    key_material: &str,
    aad: &[u8],
    bytes: &[u8],
    encrypted_path: &Path,
) -> Result<()> {
    ensure_parent(encrypted_path).await?;
    let temp_path = encrypted_path.with_extension("tmenc-writing");
    let _ = tokio::fs::remove_file(&temp_path).await;

    let cipher = cipher(key_material)?;
    let mut prefix = [0u8; NONCE_PREFIX_LEN];
    SysRng.try_fill_bytes(&mut prefix)?;

    let mut output = tokio::fs::File::create(&temp_path).await?;
    output.write_all(MAGIC).await?;
    output.write_all(&prefix).await?;
    for (index, chunk) in bytes.chunks(CHUNK_SIZE).enumerate() {
        let index = index as u64;
        let aad = chunk_aad(aad, index);
        let encrypted = cipher
            .encrypt(
                &chunk_nonce(&prefix, index),
                Payload {
                    msg: chunk,
                    aad: &aad,
                },
            )
            .map_err(|_| anyhow!("media cache encryption failed"))?;
        output
            .write_all(&(encrypted.len() as u32).to_le_bytes())
            .await?;
        output.write_all(&encrypted).await?;
    }
    output.flush().await?;
    drop(output);
    tokio::fs::rename(&temp_path, encrypted_path).await?;
    Ok(())
}

pub async fn decrypt_to_file(
    key_material: &str,
    aad: &[u8],
    encrypted_path: &Path,
    plaintext_path: &Path,
) -> Result<PathBuf> {
    if plaintext_path.exists() {
        return Ok(plaintext_path.to_path_buf());
    }
    ensure_parent(plaintext_path).await?;
    let temp_path = plaintext_path.with_extension("plain-writing");
    let _ = tokio::fs::remove_file(&temp_path).await;

    let result = decrypt_to_temp_file(key_material, aad, encrypted_path, &temp_path).await;
    if let Err(err) = result {
        let _ = tokio::fs::remove_file(&temp_path).await;
        return Err(err);
    }
    tokio::fs::rename(&temp_path, plaintext_path).await?;
    Ok(plaintext_path.to_path_buf())
}

pub async fn decrypt_to_bytes(
    key_material: &str,
    aad: &[u8],
    encrypted_path: &Path,
) -> Result<Vec<u8>> {
    match decrypt_to_bytes_with_cipher(&cipher(key_material)?, aad, encrypted_path).await {
        Ok(bytes) => Ok(bytes),
        Err(primary_err) => decrypt_to_bytes_with_cipher(
            &legacy_cipher(key_material)?,
            aad,
            encrypted_path,
        )
        .await
        .map_err(|legacy_err| {
            anyhow!(
                "media cache decryption failed with current and legacy keys: {primary_err}; {legacy_err}"
            )
        }),
    }
}

async fn decrypt_to_temp_file(
    key_material: &str,
    aad: &[u8],
    encrypted_path: &Path,
    temp_path: &Path,
) -> Result<()> {
    match decrypt_to_temp_file_with_cipher(&cipher(key_material)?, aad, encrypted_path, temp_path)
        .await
    {
        Ok(()) => Ok(()),
        Err(primary_err) => {
            let _ = tokio::fs::remove_file(temp_path).await;
            decrypt_to_temp_file_with_cipher(
                &legacy_cipher(key_material)?,
                aad,
                encrypted_path,
                temp_path,
            )
            .await
            .map_err(|legacy_err| {
                anyhow!(
                    "media cache decryption failed with current and legacy keys: {primary_err}; {legacy_err}"
                )
            })
        }
    }
}

async fn decrypt_to_temp_file_with_cipher(
    cipher: &XChaCha20Poly1305,
    aad: &[u8],
    encrypted_path: &Path,
    temp_path: &Path,
) -> Result<()> {
    let mut input = tokio::fs::File::open(encrypted_path).await?;
    let mut magic = [0u8; 8];
    input
        .read_exact(&mut magic)
        .await
        .map_err(|e| anyhow!("invalid encrypted media cache header: {e}"))?;
    if &magic != MAGIC {
        return Err(anyhow!("invalid encrypted media cache magic"));
    }
    let mut prefix = [0u8; NONCE_PREFIX_LEN];
    input.read_exact(&mut prefix).await?;

    let mut output = tokio::fs::File::create(&temp_path).await?;
    let mut index = 0u64;
    loop {
        let mut len_buf = [0u8; 4];
        match input.read_exact(&mut len_buf).await {
            Ok(_) => {}
            Err(err) if err.kind() == ErrorKind::UnexpectedEof => break,
            Err(err) => return Err(err.into()),
        }
        let len = u32::from_le_bytes(len_buf) as usize;
        if !(TAG_LEN..=CHUNK_SIZE + TAG_LEN).contains(&len) {
            return Err(anyhow!("invalid encrypted media chunk length"));
        }
        let mut encrypted = vec![0u8; len];
        input.read_exact(&mut encrypted).await?;
        let aad = chunk_aad(aad, index);
        let decrypted = cipher
            .decrypt(
                &chunk_nonce(&prefix, index),
                Payload {
                    msg: &encrypted,
                    aad: &aad,
                },
            )
            .map_err(|_| anyhow!("media cache decryption failed"))?;
        output.write_all(&decrypted).await?;
        index = index
            .checked_add(1)
            .ok_or_else(|| anyhow!("media cache chunk index overflow"))?;
    }
    output.flush().await?;
    drop(output);
    Ok(())
}

async fn decrypt_to_bytes_with_cipher(
    cipher: &XChaCha20Poly1305,
    aad: &[u8],
    encrypted_path: &Path,
) -> Result<Vec<u8>> {
    let mut input = tokio::fs::File::open(encrypted_path).await?;
    let mut magic = [0u8; 8];
    input
        .read_exact(&mut magic)
        .await
        .map_err(|e| anyhow!("invalid encrypted media cache header: {e}"))?;
    if &magic != MAGIC {
        return Err(anyhow!("invalid encrypted media cache magic"));
    }
    let mut prefix = [0u8; NONCE_PREFIX_LEN];
    input.read_exact(&mut prefix).await?;

    let mut output = Vec::new();
    let mut index = 0u64;
    loop {
        let mut len_buf = [0u8; 4];
        match input.read_exact(&mut len_buf).await {
            Ok(_) => {}
            Err(err) if err.kind() == ErrorKind::UnexpectedEof => break,
            Err(err) => return Err(err.into()),
        }
        let len = u32::from_le_bytes(len_buf) as usize;
        if !(TAG_LEN..=CHUNK_SIZE + TAG_LEN).contains(&len) {
            return Err(anyhow!("invalid encrypted media chunk length"));
        }
        let mut encrypted = vec![0u8; len];
        input.read_exact(&mut encrypted).await?;
        let aad = chunk_aad(aad, index);
        let decrypted = cipher
            .decrypt(
                &chunk_nonce(&prefix, index),
                Payload {
                    msg: &encrypted,
                    aad: &aad,
                },
            )
            .map_err(|_| anyhow!("media cache decryption failed"))?;
        output.extend_from_slice(&decrypted);
        index = index
            .checked_add(1)
            .ok_or_else(|| anyhow!("media cache chunk index overflow"))?;
    }
    Ok(output)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn test_dir(name: &str) -> PathBuf {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        std::env::temp_dir().join(format!(
            "telematrix-media-blob-store-{name}-{}-{nanos}",
            std::process::id()
        ))
    }

    #[tokio::test]
    async fn encrypted_media_round_trips_to_plaintext() {
        let dir = test_dir("round-trip");
        let encrypted = dir.join("cache.tmenc");
        let plaintext = dir.join("plain").join("media.jpg");
        let bytes = b"cached media bytes";

        encrypt_bytes("local secret", b"mxc://server/id", bytes, &encrypted)
            .await
            .unwrap();
        let result = decrypt_to_file("local secret", b"mxc://server/id", &encrypted, &plaintext)
            .await
            .unwrap();

        assert_eq!(result, plaintext);
        assert_eq!(tokio::fs::read(&plaintext).await.unwrap(), bytes);
        assert_ne!(tokio::fs::read(&encrypted).await.unwrap(), bytes);
        let _ = tokio::fs::remove_dir_all(&dir).await;
    }

    #[tokio::test]
    async fn encrypted_media_round_trips_to_memory() {
        let dir = test_dir("memory-round-trip");
        let encrypted = dir.join("cache.tmenc");
        let bytes = b"cached media bytes in memory";

        encrypt_bytes("local secret", b"mxc://server/id", bytes, &encrypted)
            .await
            .unwrap();
        let result = decrypt_to_bytes("local secret", b"mxc://server/id", &encrypted)
            .await
            .unwrap();

        assert_eq!(result, bytes);
        assert_ne!(tokio::fs::read(&encrypted).await.unwrap(), bytes);
        let _ = tokio::fs::remove_dir_all(&dir).await;
    }

    #[tokio::test]
    async fn failed_decrypt_removes_partial_plaintext_temp_file() {
        let dir = test_dir("failed-decrypt");
        let encrypted = dir.join("cache.tmenc");
        let plaintext = dir.join("plain").join("media.jpg");

        encrypt_bytes(
            "local secret",
            b"mxc://server/right",
            b"cached media bytes",
            &encrypted,
        )
        .await
        .unwrap();

        let result = decrypt_to_file(
            "local secret",
            b"mxc://server/wrong",
            &encrypted,
            &plaintext,
        )
        .await;

        assert!(result.is_err());
        assert!(!plaintext.exists());
        assert!(!plaintext.with_extension("plain-writing").exists());
        let _ = tokio::fs::remove_dir_all(&dir).await;
    }
}

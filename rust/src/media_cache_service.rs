// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::path::{Path, PathBuf};

use anyhow::{anyhow, Result};

#[derive(Clone)]
pub(crate) struct MediaCacheService {
    data_dir: PathBuf,
}

impl MediaCacheService {
    pub(crate) fn new(data_dir: PathBuf) -> Self {
        Self { data_dir }
    }

    /// Directory of the streaming-proxy progressive cache, also used as a
    /// thumbnail first-frame source (see `media_stream::cache`).
    pub(crate) fn stream_cache_dir(&self) -> PathBuf {
        self.data_dir.join("media-stream-cache")
    }

    pub(crate) fn cleanup_plaintext(&self) {
        crate::media_blob_store::delete_plaintext_temp_files(&self.data_dir);
    }

    pub(crate) async fn ensure_cache_dir(&self) -> Result<()> {
        tokio::fs::create_dir_all(self.cache_dir()).await?;
        Ok(())
    }

    pub(crate) async fn delete_cache_dir(&self) {
        let _ = tokio::fs::remove_dir_all(self.cache_dir()).await;
    }

    pub(crate) fn require_key(&self) -> Result<()> {
        let _ = self.key_material()?;
        Ok(())
    }

    pub(crate) async fn clear_media_files(
        &self,
        max_age_days: u32,
        size_limit_bytes: u64,
    ) -> Result<u64> {
        let freed =
            crate::cache_manager::clear_media_files(&self.data_dir, max_age_days, size_limit_bytes)
                .await?;
        self.cleanup_plaintext();
        Ok(freed)
    }

    pub(crate) fn work_path(&self, cache_key: &str, kind: &str) -> PathBuf {
        let hash = crate::media_blob_store::hash_cache_key(cache_key);
        crate::media_blob_store::work_path(&self.data_dir, &hash, kind)
    }

    pub(crate) fn encrypted_path(&self, cache_key: &str) -> PathBuf {
        let hash = crate::media_blob_store::hash_cache_key(cache_key);
        crate::media_blob_store::encrypted_path(&self.cache_dir(), &hash)
    }

    pub(crate) async fn cached_plaintext_path(
        &self,
        cache_key: &str,
        extension: Option<&str>,
    ) -> Result<Option<PathBuf>> {
        let key_material = self.key_material()?;
        let hash = crate::media_blob_store::hash_cache_key(cache_key);
        let encrypted_path = crate::media_blob_store::encrypted_path(&self.cache_dir(), &hash);
        let temp_path =
            crate::media_blob_store::plaintext_temp_path(&self.data_dir, &hash, extension);

        if encrypted_path.exists() {
            // Refresh mtime so the on-disk LRU treats this as recently used.
            crate::cache_manager::touch_mtime(&encrypted_path);
            if temp_path.exists() {
                return Ok(Some(temp_path));
            }
            return crate::media_blob_store::decrypt_to_file(
                &key_material,
                cache_key.as_bytes(),
                &encrypted_path,
                &temp_path,
            )
            .await
            .map(Some);
        }

        let legacy_plain_path = self.cache_dir().join(&hash);
        if legacy_plain_path.exists() {
            if let Some(parent) = temp_path.parent() {
                tokio::fs::create_dir_all(parent).await?;
            }
            let _ = tokio::fs::remove_file(&temp_path).await;
            tokio::fs::rename(&legacy_plain_path, &temp_path).await?;
            crate::media_blob_store::encrypt_file(
                &key_material,
                cache_key.as_bytes(),
                &temp_path,
                &encrypted_path,
            )
            .await?;
            return Ok(Some(temp_path));
        }

        Ok(None)
    }

    pub(crate) async fn cached_bytes(
        &self,
        cache_key: &str,
        max_bytes: u64,
    ) -> Result<Option<Vec<u8>>> {
        let key_material = self.key_material()?;
        let hash = crate::media_blob_store::hash_cache_key(cache_key);
        let encrypted_path = crate::media_blob_store::encrypted_path(&self.cache_dir(), &hash);

        if encrypted_path.exists() {
            // Refresh mtime so the on-disk LRU treats this as recently used.
            crate::cache_manager::touch_mtime(&encrypted_path);
            let encrypted_size = tokio::fs::metadata(&encrypted_path).await?.len();
            if encrypted_size > max_bytes + 1024 * 1024 {
                return Err(anyhow!("cached media is too large for memory resolve"));
            }
            let bytes = crate::media_blob_store::decrypt_to_bytes(
                &key_material,
                cache_key.as_bytes(),
                &encrypted_path,
            )
            .await?;
            if bytes.len() as u64 > max_bytes {
                return Err(anyhow!("cached media is too large for memory resolve"));
            }
            return Ok(Some(bytes));
        }

        let legacy_plain_path = self.cache_dir().join(&hash);
        if legacy_plain_path.exists() {
            let legacy_size = tokio::fs::metadata(&legacy_plain_path).await?.len();
            if legacy_size > max_bytes {
                return Err(anyhow!("legacy media is too large for memory resolve"));
            }
            let bytes = tokio::fs::read(&legacy_plain_path).await?;
            crate::media_blob_store::encrypt_bytes(
                &key_material,
                cache_key.as_bytes(),
                &bytes,
                &encrypted_path,
            )
            .await?;
            let _ = tokio::fs::remove_file(&legacy_plain_path).await;
            return Ok(Some(bytes));
        }

        Ok(None)
    }

    pub(crate) async fn store_plaintext(
        &self,
        cache_key: &str,
        plaintext_path: &Path,
        extension: Option<&str>,
    ) -> Result<PathBuf> {
        let key_material = self.key_material()?;
        let hash = crate::media_blob_store::hash_cache_key(cache_key);
        let encrypted_path = crate::media_blob_store::encrypted_path(&self.cache_dir(), &hash);
        let temp_path =
            crate::media_blob_store::plaintext_temp_path(&self.data_dir, &hash, extension);

        crate::media_blob_store::encrypt_file(
            &key_material,
            cache_key.as_bytes(),
            plaintext_path,
            &encrypted_path,
        )
        .await?;
        if let Some(parent) = temp_path.parent() {
            tokio::fs::create_dir_all(parent).await?;
        }
        let _ = tokio::fs::remove_file(&temp_path).await;
        tokio::fs::rename(plaintext_path, &temp_path).await?;
        crate::cache_manager::signal_media_stored();
        Ok(temp_path)
    }

    pub(crate) async fn store_bytes(
        &self,
        cache_key: &str,
        bytes: &[u8],
        extension: Option<&str>,
    ) -> Result<PathBuf> {
        let key_material = self.key_material()?;
        let hash = crate::media_blob_store::hash_cache_key(cache_key);
        let encrypted_path = crate::media_blob_store::encrypted_path(&self.cache_dir(), &hash);
        let temp_path =
            crate::media_blob_store::plaintext_temp_path(&self.data_dir, &hash, extension);

        crate::media_blob_store::encrypt_bytes(
            &key_material,
            cache_key.as_bytes(),
            bytes,
            &encrypted_path,
        )
        .await?;
        if let Some(parent) = temp_path.parent() {
            tokio::fs::create_dir_all(parent).await?;
        }
        tokio::fs::write(&temp_path, bytes).await?;
        crate::cache_manager::signal_media_stored();
        Ok(temp_path)
    }

    /// Load a previously-derived thumbnail from the `thumbnails/` namespace.
    /// `cache_key` (e.g. `"vidthumb:" + event_id`) is hashed and also used as the
    /// AAD so a blob can only be decrypted under its own key.
    pub(crate) async fn cached_thumbnail_bytes(&self, cache_key: &str) -> Result<Option<Vec<u8>>> {
        let key_material = self.key_material()?;
        let hash = crate::media_blob_store::hash_cache_key(cache_key);
        crate::media_blob_store::load_thumbnail_bytes(
            &key_material,
            cache_key.as_bytes(),
            &self.cache_dir(),
            &hash,
        )
        .await
    }

    /// Store derived thumbnail bytes encrypted in the `thumbnails/` namespace.
    pub(crate) async fn store_thumbnail_bytes(&self, cache_key: &str, bytes: &[u8]) -> Result<()> {
        let key_material = self.key_material()?;
        let hash = crate::media_blob_store::hash_cache_key(cache_key);
        crate::media_blob_store::store_thumbnail_bytes(
            &key_material,
            cache_key.as_bytes(),
            &self.cache_dir(),
            &hash,
            bytes,
        )
        .await
    }

    pub(crate) async fn store_encrypted_bytes(&self, cache_key: &str, bytes: &[u8]) -> Result<()> {
        let key_material = self.key_material()?;
        crate::media_blob_store::encrypt_bytes(
            &key_material,
            cache_key.as_bytes(),
            bytes,
            &self.encrypted_path(cache_key),
        )
        .await?;
        crate::cache_manager::signal_media_stored();
        Ok(())
    }

    pub(crate) async fn decrypt_cache_to_file(
        &self,
        cache_key: &str,
        target_path: &Path,
    ) -> Result<bool> {
        let encrypted_path = self.encrypted_path(cache_key);
        if !encrypted_path.exists() {
            return Ok(false);
        }
        let key_material = self.key_material()?;
        crate::media_blob_store::decrypt_to_file(
            &key_material,
            cache_key.as_bytes(),
            &encrypted_path,
            target_path,
        )
        .await?;
        Ok(true)
    }

    /// Populate the cache slot for `cache_key` from a file we do NOT own (an
    /// upload's source; see `upload_seed_store`), leaving it in place.
    ///
    /// Encrypts through a per-key work file rather than straight into the slot:
    /// a download racing us for the same key writes its own `.tmenc-writing`
    /// temp under `media_cache/`, so neither can interleave with the other, and
    /// the final rename is atomic — whoever lands last leaves a valid blob.
    pub(crate) async fn seed_from_file(
        &self,
        cache_key: &str,
        plaintext_path: &Path,
    ) -> Result<()> {
        let key_material = self.key_material()?;
        let work_path = self.work_path(cache_key, "seed");
        crate::media_blob_store::encrypt_file(
            &key_material,
            cache_key.as_bytes(),
            plaintext_path,
            &work_path,
        )
        .await?;

        let encrypted_path = self.encrypted_path(cache_key);
        if let Some(parent) = encrypted_path.parent() {
            tokio::fs::create_dir_all(parent).await?;
        }
        if let Err(err) = tokio::fs::rename(&work_path, &encrypted_path).await {
            let _ = tokio::fs::remove_file(&work_path).await;
            return Err(err.into());
        }
        crate::cache_manager::signal_media_stored();
        Ok(())
    }

    pub(crate) async fn encrypt_file_to_cache(
        &self,
        cache_key: &str,
        plaintext_path: &Path,
    ) -> Result<()> {
        let key_material = self.key_material()?;
        crate::media_blob_store::encrypt_file(
            &key_material,
            cache_key.as_bytes(),
            plaintext_path,
            &self.encrypted_path(cache_key),
        )
        .await
    }

    fn cache_dir(&self) -> PathBuf {
        self.data_dir.join("media_cache")
    }

    /// This account's media-cache key. Namespaced by data dir (see
    /// `SessionStorageService::local_secret_key`), so each account decrypts only
    /// the media cache it wrote.
    fn key_material(&self) -> Result<String> {
        let dir_ns = crate::session_storage_service::dir_namespace(&self.data_dir);
        crate::session_storage_service::SessionStorageService::load_local_secret(
            &dir_ns,
            "media_cache_passphrase",
        )
        .ok_or_else(|| anyhow!("Missing secure local cache secret: media_cache_passphrase"))
    }
}

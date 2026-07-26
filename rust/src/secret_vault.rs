// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Master-password encrypted vault for the app secret bundle.
//!
//! A user-selectable alternative to the OS keychain on every platform: the same
//! `SecretBundle` JSON that would go into the OS keychain is sealed here with
//! XChaCha20-Poly1305 under a key derived from a user master password via
//! Argon2id. The KDF parameters and salt live in the (authenticated) file
//! header, so the format is self-describing and the params are upgradeable.
//!
//! This module is pure crypto + file I/O — it knows nothing about `keychain`
//! internals — so it is testable in isolation. `keychain.rs` owns backend
//! selection and the retained key.

use std::fs::{self, File};
use std::io::Write as _;
use std::path::Path;

use anyhow::{anyhow, Result};
use argon2::{Algorithm, Argon2, Params, Version};
use chacha20poly1305::{
    aead::{Aead, KeyInit, Payload},
    Key, XChaCha20Poly1305, XNonce,
};
use rand::{rngs::OsRng, RngCore};
use zeroize::Zeroizing;

const MAGIC: &[u8; 5] = b"TMVLT";
const FORMAT_VERSION: u8 = 1;
const KDF_ARGON2ID: u8 = 1;
const SALT_LEN: usize = 16;
const NONCE_LEN: usize = 24;
/// magic(5) + version(1) + kdf(1) + m/t/p(4·3) + salt(16) + nonce(24)
const HEADER_LEN: usize = 5 + 1 + 1 + 12 + SALT_LEN + NONCE_LEN;

/// Argon2id cost parameters. Interactive desktop unlock: memory-hard, ~100ms.
/// OWASP 2024+ baseline is m=19MiB/t=2; we go a bit higher (64MiB/t=3).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct KdfParams {
    /// Memory cost in KiB.
    pub m_kib: u32,
    /// Time cost (iterations).
    pub t: u32,
    /// Parallelism (lanes).
    pub p: u32,
}

pub const DEFAULT_PARAMS: KdfParams = KdfParams {
    m_kib: 64 * 1024,
    t: 3,
    p: 1,
};

/// Upper bounds accepted from a vault-file header. The header is read before
/// authentication, so a tampered file could otherwise request an absurd Argon2
/// memory/time cost and OOM or freeze the unlock (local DoS).
const MAX_KDF_M_KIB: u32 = 512 * 1024; // 512 MiB — 8× the default
const MAX_KDF_T: u32 = 16;
const MAX_KDF_P: u32 = 8;

/// Result of opening a vault: the decrypted bytes plus the material needed to
/// re-seal later without re-running the KDF (same salt/params, fresh nonce).
pub struct Opened {
    pub plaintext: Zeroizing<Vec<u8>>,
    pub key: Zeroizing<[u8; 32]>,
    pub salt: [u8; SALT_LEN],
    pub params: KdfParams,
}

pub fn new_salt() -> [u8; SALT_LEN] {
    let mut salt = [0u8; SALT_LEN];
    OsRng.fill_bytes(&mut salt);
    salt
}

pub fn derive_key(
    password: &str,
    salt: &[u8; SALT_LEN],
    params: &KdfParams,
) -> Result<Zeroizing<[u8; 32]>> {
    if password.is_empty() {
        return Err(anyhow!("empty vault master password"));
    }
    let argon_params = Params::new(params.m_kib, params.t, params.p, Some(32))
        .map_err(|e| anyhow!("invalid argon2 params: {e}"))?;
    let argon2 = Argon2::new(Algorithm::Argon2id, Version::V0x13, argon_params);
    let mut key = Zeroizing::new([0u8; 32]);
    argon2
        .hash_password_into(password.as_bytes(), salt, &mut key[..])
        .map_err(|e| anyhow!("argon2 key derivation failed: {e}"))?;
    Ok(key)
}

/// Encrypt `plaintext` with an already-derived key. Generates a fresh random
/// nonce and emits `header || ciphertext`.
pub fn seal(
    plaintext: &[u8],
    key: &[u8; 32],
    salt: &[u8; SALT_LEN],
    params: &KdfParams,
) -> Result<Vec<u8>> {
    let mut nonce = [0u8; NONCE_LEN];
    OsRng.fill_bytes(&mut nonce);
    let header = build_header(salt, &nonce, params);
    let cipher = XChaCha20Poly1305::new(Key::from_slice(key));
    let ciphertext = cipher
        .encrypt(
            XNonce::from_slice(&nonce),
            Payload {
                msg: plaintext,
                aad: &header,
            },
        )
        .map_err(|_| anyhow!("vault seal failed"))?;
    let mut out = header;
    out.extend_from_slice(&ciphertext);
    Ok(out)
}

/// Why a password-based open failed. AEAD cannot distinguish a wrong password
/// from corrupted ciphertext, so `WrongPassword` covers both; `BadFormat` is a
/// file that is not a (supported) TeleMatrix vault at all.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OpenFailure {
    BadFormat,
    WrongPassword,
}

/// Open a vault from raw file bytes using the master password (re-derives the key).
pub fn open(file: &[u8], password: &str) -> std::result::Result<Opened, OpenFailure> {
    let header = parse_header(file).map_err(|_| OpenFailure::BadFormat)?;
    // Params were range-checked in parse_header, so a derive failure here means
    // an unusable (empty) password.
    let key = derive_key(password, &header.salt, &header.params)
        .map_err(|_| OpenFailure::WrongPassword)?;
    let plaintext = decrypt(file, &header, &key).map_err(|_| OpenFailure::WrongPassword)?;
    Ok(Opened {
        plaintext,
        key,
        salt: header.salt,
        params: header.params,
    })
}

/// Open a vault using an already-retained key (no KDF). Used when the vault is
/// unlocked but the in-memory bundle cache has been dropped.
pub fn open_with_key(file: &[u8], key: &[u8; 32]) -> Result<Zeroizing<Vec<u8>>> {
    let header = parse_header(file)?;
    decrypt(file, &header, key)
}

/// Atomically write vault bytes to `path` with owner-only permissions: the temp
/// file is created 0600 + create_new (never readable by other users, and a
/// concurrent writer's temp can't be silently truncated), then renamed into
/// place. A stale temp left by a crash is removed and retried once. On Windows
/// the user-profile ACLs already scope access to the owner.
pub fn write_atomic(path: &Path, bytes: &[u8]) -> Result<()> {
    let tmp = path.with_extension("tmp");
    let mut f = match create_private(&tmp) {
        Ok(f) => f,
        Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => {
            fs::remove_file(&tmp)?;
            create_private(&tmp)?
        }
        Err(e) => return Err(e.into()),
    };
    f.write_all(bytes)?;
    f.sync_all()?;
    drop(f);
    fs::rename(&tmp, path)?;
    Ok(())
}

fn create_private(tmp: &Path) -> std::io::Result<File> {
    let mut opts = fs::OpenOptions::new();
    opts.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        opts.mode(0o600);
    }
    opts.open(tmp)
}

struct Header {
    params: KdfParams,
    salt: [u8; SALT_LEN],
    nonce: [u8; NONCE_LEN],
}

fn build_header(salt: &[u8; SALT_LEN], nonce: &[u8; NONCE_LEN], params: &KdfParams) -> Vec<u8> {
    let mut h = Vec::with_capacity(HEADER_LEN);
    h.extend_from_slice(MAGIC);
    h.push(FORMAT_VERSION);
    h.push(KDF_ARGON2ID);
    h.extend_from_slice(&params.m_kib.to_le_bytes());
    h.extend_from_slice(&params.t.to_le_bytes());
    h.extend_from_slice(&params.p.to_le_bytes());
    h.extend_from_slice(salt);
    h.extend_from_slice(nonce);
    debug_assert_eq!(h.len(), HEADER_LEN);
    h
}

fn parse_header(file: &[u8]) -> Result<Header> {
    if file.len() < HEADER_LEN {
        return Err(anyhow!("vault file too short"));
    }
    if &file[0..5] != MAGIC {
        return Err(anyhow!("not a TeleMatrix vault file"));
    }
    if file[5] != FORMAT_VERSION {
        return Err(anyhow!("unsupported vault format version {}", file[5]));
    }
    if file[6] != KDF_ARGON2ID {
        return Err(anyhow!("unsupported vault KDF id {}", file[6]));
    }
    let m_kib = u32::from_le_bytes(file[7..11].try_into().unwrap());
    let t = u32::from_le_bytes(file[11..15].try_into().unwrap());
    let p = u32::from_le_bytes(file[15..19].try_into().unwrap());
    if !(1..=MAX_KDF_M_KIB).contains(&m_kib)
        || !(1..=MAX_KDF_T).contains(&t)
        || !(1..=MAX_KDF_P).contains(&p)
    {
        return Err(anyhow!("vault KDF parameters out of range"));
    }
    let mut salt = [0u8; SALT_LEN];
    salt.copy_from_slice(&file[19..19 + SALT_LEN]);
    let mut nonce = [0u8; NONCE_LEN];
    nonce.copy_from_slice(&file[19 + SALT_LEN..HEADER_LEN]);
    Ok(Header {
        params: KdfParams { m_kib, t, p },
        salt,
        nonce,
    })
}

fn decrypt(file: &[u8], header: &Header, key: &[u8; 32]) -> Result<Zeroizing<Vec<u8>>> {
    let cipher = XChaCha20Poly1305::new(Key::from_slice(key));
    cipher
        .decrypt(
            XNonce::from_slice(&header.nonce),
            Payload {
                msg: &file[HEADER_LEN..],
                // AAD is the exact header bytes — binds params/salt/nonce to the
                // ciphertext, so tampering with any of them fails authentication.
                aad: &file[..HEADER_LEN],
            },
        )
        .map(Zeroizing::new)
        .map_err(|_| anyhow!("vault decrypt failed (wrong password or corrupt vault)"))
}

#[cfg(test)]
mod tests {
    use super::*;

    // Small params keep the tests fast; production uses DEFAULT_PARAMS.
    const TEST_PARAMS: KdfParams = KdfParams {
        m_kib: 512,
        t: 1,
        p: 1,
    };

    fn sealed(pt: &[u8], pw: &str) -> (Vec<u8>, [u8; SALT_LEN]) {
        let salt = new_salt();
        let key = derive_key(pw, &salt, &TEST_PARAMS).unwrap();
        (seal(pt, &key, &salt, &TEST_PARAMS).unwrap(), salt)
    }

    #[test]
    fn seal_open_roundtrip() {
        let pt = br#"{"version":1,"secrets":{"token":"abc"}}"#;
        let (file, _) = sealed(pt, "correct horse");
        let opened = open(&file, "correct horse").unwrap();
        assert_eq!(opened.plaintext.as_slice(), &pt[..]);
        assert_eq!(opened.params, TEST_PARAMS);
    }

    #[test]
    fn wrong_password_fails_with_auth_error() {
        let (file, _) = sealed(b"secret", "right");
        // Must be an error (auth failure), not a panic and not garbage plaintext.
        // `.err()`: Opened holds the vault key and has no Debug/PartialEq impl.
        assert_eq!(open(&file, "wrong").err(), Some(OpenFailure::WrongPassword));
    }

    #[test]
    fn open_with_retained_key_matches() {
        let salt = new_salt();
        let key = derive_key("pw", &salt, &TEST_PARAMS).unwrap();
        let file = seal(b"payload", &key, &salt, &TEST_PARAMS).unwrap();
        assert_eq!(open_with_key(&file, &key).unwrap().as_slice(), b"payload");
    }

    #[test]
    fn header_params_are_preserved() {
        let params = KdfParams {
            m_kib: 1024,
            t: 2,
            p: 1,
        };
        let salt = new_salt();
        let key = derive_key("pw", &salt, &params).unwrap();
        let file = seal(b"x", &key, &salt, &params).unwrap();
        let header = parse_header(&file).unwrap();
        assert_eq!(header.params, params);
        assert_eq!(header.salt, salt);
    }

    #[test]
    fn tampered_header_fails_authentication() {
        let (mut file, _) = sealed(b"data", "pw");
        // Flip an m_kib byte in the AAD-bound header. The result stays inside the
        // accepted parameter range, so it survives parse_header and dies at AEAD
        // authentication — which is what this test pins.
        file[7] ^= 0xff;
        assert_eq!(open(&file, "pw").err(), Some(OpenFailure::WrongPassword));
    }

    #[test]
    fn rejects_absurd_kdf_params_before_deriving() {
        let (mut file, _) = sealed(b"data", "pw");
        // Patch m_kib (header bytes 7..11) to u32::MAX KiB (~4 TiB) — must fail
        // fast at header parse, not attempt the allocation in Argon2.
        file[7..11].copy_from_slice(&u32::MAX.to_le_bytes());
        assert_eq!(open(&file, "pw").err(), Some(OpenFailure::BadFormat));
    }

    #[cfg(unix)]
    #[test]
    fn write_atomic_is_owner_only_and_replaces_stale_tmp() {
        use std::os::unix::fs::PermissionsExt;
        let dir = std::env::temp_dir().join(format!("tm-vault-atomic-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("v.bin");
        // A stale temp left by a crash must not break the write.
        std::fs::write(path.with_extension("tmp"), b"stale").unwrap();
        write_atomic(&path, b"payload").unwrap();
        assert_eq!(std::fs::read(&path).unwrap(), b"payload");
        assert!(!path.with_extension("tmp").exists());
        let mode = std::fs::metadata(&path).unwrap().permissions().mode() & 0o777;
        assert_eq!(mode, 0o600);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn rejects_foreign_or_short_files() {
        assert_eq!(open(b"", "pw").err(), Some(OpenFailure::BadFormat));
        assert_eq!(
            open(
                b"NOTAVAULT........................................................",
                "pw"
            )
            .err(),
            Some(OpenFailure::BadFormat)
        );
    }
}

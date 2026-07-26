// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use aes::cipher::{KeyIvInit, StreamCipher, StreamCipherSeek};
use aes::Aes256;
use anyhow::{anyhow, Result};
use matrix_sdk::ruma::events::room::{EncryptedFile, EncryptedFileInfo};

type Aes256Ctr = ctr::Ctr128BE<Aes256>;

/// Decrypt ciphertext `buf` whose first byte is at absolute offset `start` in the
/// full attachment. Matrix attachments are AES-256-CTR (a stream cipher — ciphertext
/// maps 1:1 to plaintext by byte and every offset is seekable), so this yields
/// `plaintext[start .. start + buf.len()]` without touching the earlier bytes.
pub fn decrypt_ctr_range(key: &[u8; 32], iv: &[u8; 16], start: u64, buf: &mut [u8]) {
    let mut cipher = Aes256Ctr::new(key.into(), iv.into());
    cipher.seek(start);
    cipher.apply_keystream(buf);
}

/// Pull the 32-byte AES key and 16-byte IV out of a Matrix `EncryptedFile`.
///
/// In ruma-events 0.34 the key and IV moved from top-level `EncryptedFile`
/// fields into `EncryptedFileInfo::V2(V2EncryptedFileInfo)`.  Both values are
/// stored as ruma's `Base64<_, [u8; N]>` — they already hold the decoded bytes;
/// `as_inner()` gives a `&[u8; N]` directly (no runtime base64 decoding).
pub fn key_iv_from_encrypted(file: &EncryptedFile) -> Result<([u8; 32], [u8; 16])> {
    match &file.info {
        EncryptedFileInfo::V2(v2) => {
            let key: [u8; 32] = *v2.k.as_inner();
            let iv: [u8; 16] = *v2.iv.as_inner();
            Ok((key, iv))
        }
        _ => Err(anyhow!(
            "unsupported attachment encryption version (expected v2)"
        )),
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Can this video be played progressively, judged from its leading bytes?
//!
//! An mp4 whose `moov` atom sits at the END of the file ("non-faststart") can't:
//! the player reads `ftyp`, hits `mdat`, then seeks to the tail for `moov`.
//! Matrix homeservers ignore `Range`, so the proxy's upstream download is linear
//! and that seek blocks at the write frontier until the file is essentially
//! complete — the whole video downloads before the first frame appears.
//!
//! Reading the top-level box order tells us this from the first ~40 bytes,
//! instead of the players' fallback heuristic ("20% downloaded and still no
//! frame ⇒ it must be non-faststart"), which for a 700 MB video means 140 MB of
//! indeterminate spinner.

/// What the leading bytes say about progressive playback.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Container {
    /// Unrecognised bytes (a decryption failure?) or a header we couldn't walk.
    /// Callers fall back to their own heuristics.
    Unknown = 0,
    /// Decodes from the front: `moov` precedes `mdat`, or it's a container that
    /// doesn't have the problem at all (mkv/webm, avi, ogg, flv).
    Faststart = 1,
    /// `mdat` precedes `moov`: the whole file downloads before the first frame.
    MoovAtEnd = 2,
}

/// One classification attempt over a prefix. `NeedMore` means the walk ran off
/// the end of the buffer mid-skip — feed more bytes, or give up with `Unknown`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Verdict {
    Decided(Container),
    NeedMore,
}

/// Classify from a prefix of the (decrypted) file.
pub fn classify(buf: &[u8]) -> Verdict {
    // Containers that decode from the front regardless of where their index
    // lives. mkv/webm put Cues at the end, but playback doesn't wait for them.
    if buf.starts_with(&[0x1A, 0x45, 0xDF, 0xA3]) // EBML (mkv/webm)
        || buf.starts_with(b"RIFF") // avi/wav
        || buf.starts_with(b"OggS")
        || buf.starts_with(b"FLV")
    {
        return Verdict::Decided(Container::Faststart);
    }
    if buf.len() < 8 {
        return Verdict::NeedMore;
    }
    if &buf[4..8] != b"ftyp" {
        return Verdict::Decided(Container::Unknown);
    }
    walk_iso_bmff(buf)
}

/// Walk the top-level box list. Only `moov` and `mdat` decide; every other box
/// (`free`, `skip`, `wide`, `pnot`, `uuid`, `styp`, `moof`, …) is skipped by its
/// size, so no allow-list is needed. A fragmented mp4 puts `moov` (carrying
/// `mvex`) before its first `moof`, and so classifies as Faststart correctly.
fn walk_iso_bmff(buf: &[u8]) -> Verdict {
    let mut pos: u64 = 0;
    loop {
        let Ok(p) = usize::try_from(pos) else {
            return Verdict::Decided(Container::Unknown);
        };
        // Slice from `p` first: a bogus size can push `pos` past usize range, and
        // `buf.get(p..p + 8)` would overflow computing the end.
        let Some(rest) = buf.get(p..) else {
            return Verdict::NeedMore;
        };
        let Some(header) = rest.get(..8) else {
            return Verdict::NeedMore;
        };
        let size32 = u32::from_be_bytes([header[0], header[1], header[2], header[3]]);
        let typ = &header[4..8];

        // A garbage type means a previous box's size walked us into data, so the
        // walk is worthless from here (this also catches decrypted-to-garbage).
        if !typ.iter().all(|c| (0x20..=0x7e).contains(c)) {
            return Verdict::Decided(Container::Unknown);
        }
        // The decision boxes resolve on the TYPE alone: a multi-gigabyte `mdat`
        // is classified from its 8-byte header, its body never read.
        if typ == b"moov" {
            return Verdict::Decided(Container::Faststart);
        }
        if typ == b"mdat" {
            return Verdict::Decided(Container::MoovAtEnd);
        }

        let size: u64 = match size32 {
            // 64-bit `largesize` follows the header.
            1 => {
                let Some(large) = rest.get(8..16) else {
                    return Verdict::NeedMore;
                };
                let bytes: [u8; 8] = large.try_into().unwrap_or([0; 8]);
                match u64::from_be_bytes(bytes) {
                    s if s < 16 => return Verdict::Decided(Container::Unknown), // malformed
                    s => s,
                }
            }
            // Runs to EOF. Only reachable on a skip box (moov/mdat returned
            // above), so it swallows the rest of the file: nothing left to find.
            0 => return Verdict::Decided(Container::Unknown),
            // A box smaller than its own header is malformed.
            s if s < 8 => return Verdict::Decided(Container::Unknown),
            s => u64::from(s),
        };
        let Some(next) = pos.checked_add(size) else {
            return Verdict::Decided(Container::Unknown);
        };
        pos = next;
    }
}

/// Human-readable container name for logs. An `UNRECOGNISED` header on the
/// streaming path means the decrypted bytes are garbage (key/iv mismatch); a
/// recognised container that still won't play points at an unsupported codec.
pub fn describe(buf: &[u8]) -> &'static str {
    if buf.len() >= 8 && &buf[4..8] == b"ftyp" {
        match classify(buf) {
            Verdict::Decided(Container::MoovAtEnd) => "ISO-BMFF (mp4/mov, moov at end)",
            Verdict::Decided(Container::Faststart) => "ISO-BMFF (mp4/mov, faststart)",
            _ => "ISO-BMFF (mp4/mov)",
        }
    } else if buf.starts_with(&[0x1A, 0x45, 0xDF, 0xA3]) {
        "EBML (mkv/webm)"
    } else if buf.starts_with(b"RIFF") {
        "RIFF (avi/wav)"
    } else if buf.starts_with(b"OggS") {
        "Ogg"
    } else if buf.starts_with(b"FLV") {
        "FLV"
    } else {
        "UNRECOGNISED (decryption failure or unsupported container)"
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A top-level box header: 32-bit size + 4-byte type, then `body` bytes.
    fn boxed(typ: &[u8; 4], body: &[u8]) -> Vec<u8> {
        let size = (8 + body.len()) as u32;
        let mut v = size.to_be_bytes().to_vec();
        v.extend_from_slice(typ);
        v.extend_from_slice(body);
        v
    }

    fn ftyp() -> Vec<u8> {
        boxed(b"ftyp", b"isom\0\0\x02\0isomiso2avc1mp41")
    }

    fn decided(buf: &[u8]) -> Container {
        match classify(buf) {
            Verdict::Decided(c) => c,
            Verdict::NeedMore => panic!("expected a decision, got NeedMore"),
        }
    }

    #[test]
    fn faststart_when_moov_precedes_mdat() {
        let mut b = ftyp();
        b.extend(boxed(b"moov", &[0u8; 32]));
        b.extend(boxed(b"mdat", &[0u8; 64]));
        assert_eq!(decided(&b), Container::Faststart);
    }

    #[test]
    fn moov_at_end_when_mdat_comes_first() {
        let mut b = ftyp();
        b.extend(boxed(b"mdat", &[0u8; 64]));
        b.extend(boxed(b"moov", &[0u8; 32]));
        assert_eq!(decided(&b), Container::MoovAtEnd);
    }

    #[test]
    fn skip_boxes_are_walked_over() {
        for pad in [b"free", b"skip", b"wide", b"pnot"] {
            let mut b = ftyp();
            b.extend(boxed(pad, &[0u8; 16]));
            b.extend(boxed(b"mdat", &[0u8; 8]));
            assert_eq!(decided(&b), Container::MoovAtEnd, "pad={pad:?}");
        }
    }

    #[test]
    fn fragmented_mp4_is_faststart() {
        // A valid fMP4 puts moov (with mvex) ahead of the first moof/mdat pair.
        let mut b = ftyp();
        b.extend(boxed(b"moov", &boxed(b"mvex", &[0u8; 8])));
        b.extend(boxed(b"moof", &[0u8; 16]));
        b.extend(boxed(b"mdat", &[0u8; 32]));
        assert_eq!(decided(&b), Container::Faststart);
    }

    #[test]
    fn huge_mdat_decides_without_its_body() {
        // Declared size far beyond the buffer: the type alone settles it.
        let mut b = ftyp();
        b.extend_from_slice(&u32::MAX.to_be_bytes());
        b.extend_from_slice(b"mdat");
        assert_eq!(decided(&b), Container::MoovAtEnd);
    }

    #[test]
    fn largesize_skip_box_is_walked() {
        let mut b = ftyp();
        // size==1 ⇒ 64-bit largesize at +8; header is 16 bytes total.
        b.extend_from_slice(&1u32.to_be_bytes());
        b.extend_from_slice(b"free");
        b.extend_from_slice(&24u64.to_be_bytes()); // 16 header + 8 body
        b.extend_from_slice(&[0u8; 8]);
        b.extend(boxed(b"moov", &[0u8; 8]));
        assert_eq!(decided(&b), Container::Faststart);
    }

    #[test]
    fn malformed_largesize_below_header_is_unknown() {
        let mut b = ftyp();
        b.extend_from_slice(&1u32.to_be_bytes());
        b.extend_from_slice(b"free");
        b.extend_from_slice(&8u64.to_be_bytes()); // < 16 ⇒ malformed
        assert_eq!(decided(&b), Container::Unknown);
    }

    #[test]
    fn size_zero_skip_box_runs_to_eof() {
        let mut b = ftyp();
        b.extend_from_slice(&0u32.to_be_bytes());
        b.extend_from_slice(b"free");
        b.extend_from_slice(&[0u8; 16]);
        assert_eq!(decided(&b), Container::Unknown);
    }

    #[test]
    fn size_below_header_is_unknown() {
        let mut b = ftyp();
        b.extend_from_slice(&4u32.to_be_bytes());
        b.extend_from_slice(b"free");
        assert_eq!(decided(&b), Container::Unknown);
    }

    #[test]
    fn size_overflow_is_unknown() {
        let mut b = ftyp();
        b.extend_from_slice(&1u32.to_be_bytes());
        b.extend_from_slice(b"free");
        b.extend_from_slice(&u64::MAX.to_be_bytes()); // pos + size overflows
        assert_eq!(decided(&b), Container::Unknown);
    }

    #[test]
    fn truncated_mid_skip_needs_more() {
        let mut b = ftyp();
        b.extend_from_slice(&64u32.to_be_bytes()); // skip box extends past the buffer
        b.extend_from_slice(b"free");
        assert_eq!(classify(&b), Verdict::NeedMore);
    }

    #[test]
    fn truncated_header_needs_more() {
        assert_eq!(classify(&[]), Verdict::NeedMore);
        assert_eq!(classify(b"\0\0\0"), Verdict::NeedMore);
    }

    #[test]
    fn garbage_after_ftyp_is_unknown() {
        // A non-printable box type means the walk landed inside data.
        let mut b = ftyp();
        b.extend_from_slice(&16u32.to_be_bytes());
        b.extend_from_slice(&[0x00, 0xFF, 0x01, 0x02]);
        assert_eq!(decided(&b), Container::Unknown);
    }

    #[test]
    fn non_iso_containers_stream_from_the_front() {
        assert_eq!(
            decided(&[0x1A, 0x45, 0xDF, 0xA3, 0, 0, 0, 0]),
            Container::Faststart
        );
        assert_eq!(decided(b"RIFF____AVI "), Container::Faststart);
        assert_eq!(decided(b"OggS____"), Container::Faststart);
        assert_eq!(decided(b"FLV\x01\x05\0\0\0\x09"), Container::Faststart);
    }

    #[test]
    fn unrecognised_bytes_are_unknown() {
        // What a wrong decryption key produces.
        assert_eq!(
            decided(&[0x9f, 0x3c, 0x00, 0x11, 0xa2, 0xb4, 0xc6, 0xd8]),
            Container::Unknown
        );
    }

    #[test]
    fn describe_reports_the_moov_position() {
        let mut b = ftyp();
        b.extend(boxed(b"mdat", &[0u8; 8]));
        assert!(describe(&b).contains("moov at end"));
        let mut f = ftyp();
        f.extend(boxed(b"moov", &[0u8; 8]));
        assert!(describe(&f).contains("faststart"));
        assert!(describe(&[0u8; 4]).starts_with("UNRECOGNISED"));
    }
}

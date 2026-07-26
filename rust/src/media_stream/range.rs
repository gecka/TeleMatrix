// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ByteRange {
    pub start: u64,
    pub end_inclusive: u64,
}

impl ByteRange {
    // An inclusive byte range is always ≥ 1 byte (constructors reject
    // start > end_inclusive), so there's no meaningful `is_empty` to pair with
    // `len` — the lint would otherwise demand one.
    #[allow(clippy::len_without_is_empty)]
    pub fn len(&self) -> u64 {
        self.end_inclusive + 1 - self.start
    }
}

/// Parse a single `bytes=` range against a known `total` length. Returns `None`
/// (meaning "serve the whole file") for absent, multi-range, or malformed values.
pub fn parse_range(header: Option<&str>, total: u64) -> Option<ByteRange> {
    let spec = header?.trim().strip_prefix("bytes=")?;
    if spec.contains(',') || total == 0 {
        return None; // multi-range unsupported -> whole file
    }
    let (a, b) = spec.split_once('-')?;
    let (start, end_inclusive) = match (a.trim(), b.trim()) {
        ("", suffix) => {
            let n: u64 = suffix.parse().ok()?;
            (total.saturating_sub(n).min(total - 1), total - 1)
        }
        (s, "") => (s.parse().ok()?, total - 1),
        (s, e) => (s.parse().ok()?, e.parse::<u64>().ok()?.min(total - 1)),
    };
    if start > end_inclusive || start >= total {
        return None;
    }
    Some(ByteRange {
        start,
        end_inclusive,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn open_ended_from_offset() {
        let r = parse_range(Some("bytes=100-"), 1000).unwrap();
        assert_eq!(r.start, 100);
        assert_eq!(r.end_inclusive, 999);
        assert_eq!(r.len(), 900);
    }

    #[test]
    fn bounded_range_is_exact() {
        let r = parse_range(Some("bytes=0-32767"), 1_000_000).unwrap();
        assert_eq!(r.start, 0);
        assert_eq!(r.end_inclusive, 32767);
        assert_eq!(r.len(), 32768);
    }

    #[test]
    fn bounded_end_clamped_to_total() {
        let r = parse_range(Some("bytes=10-99999"), 1000).unwrap();
        assert_eq!(r.start, 10);
        assert_eq!(r.end_inclusive, 999);
    }

    #[test]
    fn suffix_range() {
        let r = parse_range(Some("bytes=-200"), 1000).unwrap();
        assert_eq!(r.start, 800);
        assert_eq!(r.end_inclusive, 999);
    }

    #[test]
    fn suffix_larger_than_file_clamps_to_whole() {
        let r = parse_range(Some("bytes=-5000"), 1000).unwrap();
        assert_eq!(r.start, 0);
        assert_eq!(r.end_inclusive, 999);
    }

    #[test]
    fn absent_or_malformed_is_none() {
        assert!(parse_range(None, 1000).is_none());
        assert!(parse_range(Some("100-200"), 1000).is_none()); // no bytes= prefix
        assert!(parse_range(Some("bytes=abc"), 1000).is_none());
        assert!(parse_range(Some("bytes=0-1,5-6"), 1000).is_none()); // multi-range
    }

    #[test]
    fn zero_total_is_none() {
        assert!(parse_range(Some("bytes=0-10"), 0).is_none());
    }

    #[test]
    fn start_past_end_is_none() {
        assert!(parse_range(Some("bytes=500-100"), 1000).is_none());
        assert!(parse_range(Some("bytes=1000-"), 1000).is_none()); // start >= total
    }
}

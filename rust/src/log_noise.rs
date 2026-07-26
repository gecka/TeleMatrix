//! Collapses the expected undecryptable-message warning flood.
//!
//! Every failed decryption attempt makes the SDK log two WARN lines, and the
//! redecryptor re-attempts *every* still-stuck session each time any one session
//! succeeds — so a post-verification backlog emits on the order of N² of them.
//! They are expected rather than actionable (the message simply has no key yet),
//! and at that volume the formatting and stderr writes are themselves a cost on
//! the decryption hot path, on top of burying every other log line.
//!
//! Dropping them at the filter means the message is never formatted at all. The
//! information worth keeping is the *count*, which [`suppressed_count`] reports;
//! it doubles as the cheapest available measure of how much redundant decryption
//! work the SDK is doing.
//!
//! Set `TM_LOG_ALL_UTD_WARNINGS=1` to get them back.

use std::fmt::Write as _;
use std::sync::atomic::{AtomicU64, Ordering};
use tracing::field::{Field, Visit};
use tracing::{Event, Metadata, Subscriber};
use tracing_subscriber::layer::{Context, Filter};

static SUPPRESSED: AtomicU64 = AtomicU64::new(0);

/// Expected-UTD warnings dropped since startup.
pub(crate) fn suppressed_count() -> u64 {
    SUPPRESSED.load(Ordering::Relaxed)
}

/// Modules that emit the flood. Checked first because it is a cheap pointer-length
/// comparison, and it keeps the message match off every unrelated event.
const NOISY_TARGETS: &[&str] = &[
    "matrix_sdk_crypto::machine",
    "matrix_sdk::event_cache::redecryptor",
    "matrix_sdk_crypto::backups",
];

/// Matched on the message, not just the target: these same modules also report
/// invalid session keys, unsupported algorithms and cross-signing failures, which
/// are rare and worth seeing.
///
/// The backup-key one fires once per upload attempt for as long as this device
/// lacks the backup decryption key — seconds apart, indefinitely. It says exactly
/// what the `[keys] usable=false` probe line says once every 15s, so the probe
/// supersedes it rather than hiding it.
const NOISY_MESSAGES: &[&str] = &[
    "Failed to decrypt a room event",
    "Failed to redecrypt an event despite",
    "Trying to backup room keys but no backup key was found",
];

/// Accumulates only enough of a message to classify it, so suppressed events never
/// pay for formatting their (often long) error tail.
#[derive(Default)]
struct Head {
    buf: String,
}

impl std::fmt::Write for Head {
    fn write_str(&mut self, s: &str) -> std::fmt::Result {
        if self.buf.len() < 64 {
            self.buf.push_str(s);
        }
        Ok(())
    }
}

#[derive(Default)]
struct MessageMatcher {
    matched: bool,
}

impl MessageMatcher {
    fn test(&mut self, message: &str) {
        self.matched = NOISY_MESSAGES.iter().any(|p| message.starts_with(p));
    }
}

impl Visit for MessageMatcher {
    fn record_debug(&mut self, field: &Field, value: &dyn std::fmt::Debug) {
        if field.name() == "message" {
            let mut head = Head::default();
            let _ = write!(head, "{value:?}");
            self.test(&head.buf);
        }
    }

    fn record_str(&mut self, field: &Field, value: &str) {
        if field.name() == "message" {
            self.test(value);
        }
    }
}

pub(crate) struct DropExpectedUtdWarnings {
    enabled: bool,
}

impl DropExpectedUtdWarnings {
    pub(crate) fn from_env() -> Self {
        Self {
            enabled: std::env::var_os("TM_LOG_ALL_UTD_WARNINGS").is_none(),
        }
    }
}

impl<S: Subscriber> Filter<S> for DropExpectedUtdWarnings {
    fn enabled(&self, _meta: &Metadata<'_>, _cx: &Context<'_, S>) -> bool {
        // Per-event classification needs the fields, which only `event_enabled`
        // sees; the level/target cut has already been made by the `EnvFilter`.
        true
    }

    fn event_enabled(&self, event: &Event<'_>, _cx: &Context<'_, S>) -> bool {
        if !self.enabled {
            return true;
        }
        let target = event.metadata().target();
        if !NOISY_TARGETS.iter().any(|t| target.starts_with(t)) {
            return true;
        }
        let mut matcher = MessageMatcher::default();
        event.record(&mut matcher);
        if matcher.matched {
            SUPPRESSED.fetch_add(1, Ordering::Relaxed);
            return false;
        }
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn classifies_only_the_expected_utd_messages() {
        let mut m = MessageMatcher::default();
        m.test("Failed to decrypt a room event: Can't find the room key");
        assert!(m.matched);

        let mut m = MessageMatcher::default();
        m.test("Failed to redecrypt an event despite receiving a room key");
        assert!(m.matched);

        let mut m = MessageMatcher::default();
        m.test("Trying to backup room keys but no backup key was found");
        assert!(m.matched);

        // Same modules, but these are rare and actionable.
        let mut m = MessageMatcher::default();
        m.test("Received a room key event which contained an invalid session key: x");
        assert!(!m.matched);

        let mut m = MessageMatcher::default();
        m.test("Couldn't sign the message using the cross signing master key");
        assert!(!m.matched);
    }

    #[test]
    fn head_bounds_what_it_collects() {
        let mut head = Head::default();
        let _ = write!(head, "{}", "x".repeat(500));
        assert!(head.buf.len() < 600, "one overshooting write is the bound");
        let _ = write!(head, "{}", "y".repeat(500));
        assert!(
            !head.buf.contains('y'),
            "stops collecting once past the cap"
        );
    }
}

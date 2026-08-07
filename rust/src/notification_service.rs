// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Per-event desktop-notification decision and emission.
//!
//! A desktop notification is raised only for a genuinely-new
//! (`EventItemOrigin::Sync`) incoming, notifiable message, once we are caught up,
//! respecting push rules / room mode. The UI-only gates (app focused on the
//! room, `desktopNotify` setting) live in C++.

use std::collections::{HashMap, HashSet};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{SystemTime, UNIX_EPOCH};

use matrix_sdk::ruma::events::room::member::MembershipState;
use matrix_sdk::{Room, RoomMemberships};
use matrix_sdk_ui::timeline::{EventItemOrigin, TimelineItem as SdkTimelineItem};
use tokio::sync::RwLock;
use tracing::{info, warn};

use crate::matrix::SYNC_STATE_SYNCED;
use crate::room_summary_service::RoomSummaryService;
use crate::types::{MessageContent, RoomNotificationMode, TimelineItem};

/// The notification callback itself. Args:
/// room_id, event_id, sender_display_name, sender_avatar_url, room_display_name,
/// body, is_direct, is_mention, timestamp_secs.
pub(crate) type NotificationFn =
    Box<dyn Fn(&str, &str, &str, &str, &str, &str, bool, bool, u64) + Send>;
/// Storage slot for the (single, process-wide) notification callback.
pub(crate) type NotificationCallbackSlot = Arc<Mutex<Option<NotificationFn>>>;

/// The invite-notification callback. Args:
/// room_id, inviter_display_name, inviter_avatar_url, room_display_name, is_direct.
pub(crate) type InviteNotificationFn = Box<dyn Fn(&str, &str, &str, &str, bool) + Send>;
/// Storage slot for the (single, process-wide) invite-notification callback.
pub(crate) type InviteNotificationCallbackSlot = Arc<Mutex<Option<InviteNotificationFn>>>;

type TimelineCacheRef = Arc<RwLock<HashMap<String, Vec<TimelineItem>>>>;

/// Inputs distilled from a timeline edge item + room state. Deliberately free of
/// SDK types so the decision is unit-testable in isolation.
#[derive(Debug, Clone)]
pub(crate) struct NotificationDecision {
    /// Item came from a live `/sync` (`EventItemOrigin::Sync`) — not pagination,
    /// cache restore, local echo, or a re-decryption `Set`.
    pub is_sync_origin: bool,
    /// Event was sent by us.
    pub is_own: bool,
    /// Content is a user-facing message (text/media/poll/sticker/UTD), not a
    /// service/state event.
    pub is_notifiable_message: bool,
    /// Event is at/after our read frontier (genuinely unread).
    pub is_unread: bool,
    /// First sync + post-sync refresh complete (`SYNC_STATE_SYNCED`).
    pub caught_up: bool,
    /// Push-rule outcome for this event. `None` => fall back to `mode`.
    pub push_should_notify: Option<bool>,
    /// Effective room notification mode (used when push actions are absent).
    pub mode: RoomNotificationMode,
    /// Event triggers a highlight (mention/keyword).
    pub is_highlight: bool,
}

impl NotificationDecision {
    /// True iff this event should raise a desktop notification (before the
    /// C++-side focus/settings gates).
    pub(crate) fn should_notify(&self) -> bool {
        if !self.caught_up
            || !self.is_sync_origin
            || self.is_own
            || !self.is_notifiable_message
            || !self.is_unread
        {
            return false;
        }
        if let Some(push) = self.push_should_notify {
            return push;
        }
        match self.mode {
            RoomNotificationMode::Mute => false,
            RoomNotificationMode::MentionsOnly => self.is_highlight,
            RoomNotificationMode::AllMessages => true,
        }
    }
}

/// What the room-list consumer should do about a room's invite status this
/// batch. Keeps the (pure) decision separate from the SDK-driven emission so it
/// is unit-testable, mirroring [`NotificationDecision`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum InviteAction {
    /// Track the room as a known invite without toasting (pre-SYNCED seed, so the
    /// startup backlog of pending invites is silent; or an already-tracked one).
    Record,
    /// Track the room and raise a desktop notification (a genuinely-new invite
    /// seen after we're caught up).
    Notify,
    /// Room is no longer an invite (joined/declined) — forget it so a future
    /// re-invite notifies again.
    Forget,
    /// Nothing to do.
    Ignore,
}

/// Decide the invite action for one affected room. `already_known` is whether the
/// room id is already in the caller's notified-invites set.
pub(crate) fn invite_action(
    is_invited: bool,
    caught_up: bool,
    already_known: bool,
) -> InviteAction {
    if is_invited {
        if already_known {
            InviteAction::Ignore
        } else if caught_up {
            InviteAction::Notify
        } else {
            InviteAction::Record
        }
    } else if already_known {
        InviteAction::Forget
    } else {
        InviteAction::Ignore
    }
}

/// Notifiable body text, or `None` for service/state/redacted events.
/// Reuses the chat-list preview logic so the allowlist stays in one place; a
/// brand-new still-encrypted (UTD) arrival is notifiable with a generic body.
fn notifiable_body(content: &MessageContent) -> Option<String> {
    match RoomSummaryService::content_to_preview(content) {
        Some(text) => Some(text),
        None => match content {
            MessageContent::UnableToDecrypt { .. } => Some(String::from("New message")),
            _ => None,
        },
    }
}

fn sdk_mode_to_app(
    mode: Option<matrix_sdk::notification_settings::RoomNotificationMode>,
) -> RoomNotificationMode {
    use matrix_sdk::notification_settings::RoomNotificationMode as Sdk;
    match mode {
        Some(Sdk::MentionsAndKeywordsOnly) => RoomNotificationMode::MentionsOnly,
        Some(Sdk::Mute) => RoomNotificationMode::Mute,
        _ => RoomNotificationMode::AllMessages,
    }
}

/// Event ids our own read receipts point at (we've read up to and including
/// each). Queried across public/private × unthreaded/main so a receipt from any
/// device — e.g. one our phone just sent — advances the frontier.
async fn own_read_receipt_targets(room: &Room) -> HashSet<String> {
    use matrix_sdk::ruma::events::receipt::{ReceiptThread, ReceiptType};
    let own = room.own_user_id().to_owned();
    let mut targets = HashSet::new();
    for (receipt_type, thread) in [
        (ReceiptType::Read, ReceiptThread::Unthreaded),
        (ReceiptType::Read, ReceiptThread::Main),
        (ReceiptType::ReadPrivate, ReceiptThread::Unthreaded),
        (ReceiptType::ReadPrivate, ReceiptThread::Main),
    ] {
        if let Ok(Some((event_id, _))) = room.load_user_receipt(receipt_type, thread, &own).await {
            targets.insert(event_id.to_string());
        }
    }
    targets
}

/// Furthest index in this batch whose event id one of our read receipts points
/// at — everything at or before it is already read. `None` when no read target
/// appears in the batch (receipts outside it can't be positioned here; the
/// C++ clear-on-read path covers those). Pure, for unit testing.
fn read_through_index(
    event_ids: &[Option<String>],
    read_targets: &HashSet<String>,
) -> Option<usize> {
    if read_targets.is_empty() {
        return None;
    }
    event_ids
        .iter()
        .enumerate()
        .filter_map(|(idx, id)| read_targets.contains(id.as_deref()?).then_some(idx))
        .max()
}

/// For each bottom-edge new item, decide and fire the notification callback.
/// `new_items` are the `Append`/`PushBack` SDK items for this diff batch; the
/// already-converted bodies/senders are read from `cache` (refreshed just before
/// this call), so we don't re-run content conversion.
pub(crate) async fn evaluate_and_emit(
    room_id: &str,
    room: &Room,
    new_items: &[Arc<SdkTimelineItem>],
    cache: &TimelineCacheRef,
    sync_state: &Arc<AtomicU32>,
    callback: &NotificationCallbackSlot,
) {
    if new_items.is_empty() {
        return;
    }
    // Caught-up gate: suppress the entire initial-sync / backfill burst.
    if sync_state.load(Ordering::SeqCst) != SYNC_STATE_SYNCED {
        return;
    }

    // No callback registered -> nothing to deliver; skip the push-rule loop.
    if callback.lock().map(|g| g.is_none()).unwrap_or(true) {
        return;
    }

    // Event ids of this batch's live, non-own arrivals. We pull only these few
    // converted items from the cache instead of cloning the whole timeline.
    let wanted: HashSet<String> = new_items
        .iter()
        .filter_map(|item| {
            let event = item.as_event()?;
            if !matches!(event.origin(), Some(EventItemOrigin::Sync)) || event.is_own() {
                return None;
            }
            event.event_id().map(|id| id.to_string())
        })
        .collect();
    if wanted.is_empty() {
        return;
    }

    // One read pass: clone only the wanted items (body/sender/timestamp), keyed by
    // event id for O(1) lookup — never the full Vec, never an O(N) scan per event.
    let lookup: HashMap<String, TimelineItem> = {
        let guard = cache.read().await;
        let Some(items) = guard.get(room_id) else {
            return;
        };
        items
            .iter()
            .filter(|i| wanted.contains(&i.event_id))
            .map(|i| (i.event_id.clone(), i.clone()))
            .collect()
    };

    let is_direct = room.direct_targets_length() > 0;
    let room_display_name = crate::room_summary_service::room_display_name(
        room,
        room.cached_display_name()
            .map(|name| name.to_string())
            .unwrap_or_default(),
    );
    let mode = sdk_mode_to_app(room.notification_mode().await);

    // Pre-show read-elsewhere gate (A2): a read receipt from another device can
    // land in the same sync as the message. Compute how far our own receipt
    // reaches inside THIS batch; items at or before it are already read and must
    // not toast (avoids a brief toast-then-clear flash). Reads that arrive in a
    // *later* sync are handled reactively on the C++ side via clear-on-read.
    let event_ids: Vec<Option<String>> = new_items
        .iter()
        .map(|item| {
            item.as_event()
                .and_then(|event| event.event_id())
                .map(|id| id.to_string())
        })
        .collect();
    let read_targets = own_read_receipt_targets(room).await;
    let read_through = read_through_index(&event_ids, &read_targets);

    // Buffer notifications so the callback mutex is taken once, after the
    // await-bearing push-rule loop — not once per event.
    // (event_id, sender_display_name, sender_avatar_url, body, is_highlight, ts_secs)
    let mut to_emit: Vec<(String, String, String, String, bool, u64)> = Vec::new();

    for (idx, item) in new_items.iter().enumerate() {
        let Some(event) = item.as_event() else {
            continue;
        };
        // Live arrival only — excludes pagination, cache restore, local echo, and
        // re-decryption (which arrives as a `Set` diff, never a bottom append).
        if !matches!(event.origin(), Some(EventItemOrigin::Sync)) {
            continue;
        }
        if event.is_own() {
            continue;
        }
        // Already read on another device (a read receipt for this or a later item
        // arrived in the same sync) — don't toast it.
        if read_through.is_some_and(|frontier| idx <= frontier) {
            continue;
        }
        let Some(event_id) = event_ids[idx].clone() else {
            continue;
        };
        let Some(cached) = lookup.get(&event_id) else {
            continue;
        };
        let Some(body) = notifiable_body(&cached.content) else {
            continue;
        };

        // Local push-rule evaluation (no network) on the raw event.
        let (push_should_notify, is_highlight) = match event.original_json() {
            Some(raw) => match room.event_push_actions(raw).await {
                Ok(Some(actions)) => (
                    Some(actions.iter().any(|a| a.should_notify())),
                    actions.iter().any(|a| a.is_highlight()),
                ),
                _ => (None, false),
            },
            None => (None, false),
        };

        let decision = NotificationDecision {
            is_sync_origin: true,
            is_own: false,
            is_notifiable_message: true,
            // Items already read are dropped by the read-frontier skip above, so
            // what reaches here is genuinely unread; push rules cover the muted
            // case. A read that arrives in a *later* sync dismisses the toast
            // reactively on the C++ side (`UnreadStateStore::roomReadProgressed`
            // -> `clearFromRoom`).
            is_unread: true,
            caught_up: true,
            push_should_notify,
            mode,
            is_highlight,
        };
        if !decision.should_notify() {
            continue;
        }

        let timestamp_secs = cached
            .timestamp
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0);

        to_emit.push((
            event_id,
            cached.sender.display_name.clone(),
            // mxc:// avatar url (empty if the sender has none / it isn't cached);
            // resolved to a local image file on the C++ side via MediaCache.
            cached.sender.avatar_url.clone().unwrap_or_default(),
            body,
            is_highlight,
            timestamp_secs,
        ));
    }

    if to_emit.is_empty() {
        return;
    }
    match callback.lock() {
        Ok(guard) => {
            if let Some(cb) = guard.as_ref() {
                for (event_id, sender, avatar, body, is_highlight, timestamp_secs) in &to_emit {
                    cb(
                        room_id,
                        event_id,
                        sender,
                        avatar,
                        &room_display_name,
                        body,
                        is_direct,
                        *is_highlight,
                        *timestamp_secs,
                    );
                }
            }
        }
        Err(_) => warn!("notification callback mutex poisoned; skipping"),
    }
}

/// Sliding-sync notification path: evaluate a room's current latest event and
/// fire the notification callback if warranted.
///
/// The classic backend notifies from per-room timeline windows (see
/// [`evaluate_and_emit`]), which the sliding backend doesn't keep for background
/// rooms. Instead, notifications are driven from the room-list diff stream — a new
/// message bubbles its room up the recency-sorted list — and the caller (the
/// sliding consumer) gates on caught-up (`SYNCED`) and dedups by latest event id,
/// so this only ever sees a genuinely-new latest event. Push-rule evaluation,
/// own/notifiable filtering and room mode are applied here, mirroring the classic
/// path's [`NotificationDecision`].
pub(crate) async fn notify_room_latest_event(room: &Room, callback: &NotificationCallbackSlot) {
    use matrix_sdk::latest_events::LatestEventValue;

    // Only a remote message can notify (not None / invites / local echoes).
    let LatestEventValue::Remote(event) = room.latest_event() else {
        return;
    };
    let raw = event.raw();
    let Ok(any_event) = raw.deserialize() else {
        return;
    };

    // Recency gate: notify only for a genuinely fresh arrival. A new message bumps
    // its room to the top of the recency-sorted list and is processed within
    // seconds; a room first seen in the diff stream *after* SYNCED (e.g. the
    // `Growing` sync loading the tail of a large account) carries an OLD latest
    // event we must not notify. The dedup map can't tell these apart on first
    // sight (both have no prior id), so gate on the event timestamp.
    let event_ms = u64::from(any_event.origin_server_ts().get());
    let now_ms = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0);
    const RECENT_WINDOW_MS: u64 = 60_000;
    if now_ms.saturating_sub(event_ms) > RECENT_WINDOW_MS {
        return;
    }

    if any_event.sender() == room.own_user_id() {
        return;
    }
    // Body via the shared preview extractor (service/state/undecryptable -> None,
    // which means "not a notifiable message").
    let Some(body) = RoomSummaryService::extract_event_body(&any_event) else {
        return;
    };

    // Per-event push-rule evaluation (no network), as in evaluate_and_emit.
    let (push_should_notify, is_highlight) = match room.event_push_actions(raw).await {
        Ok(Some(actions)) => (
            Some(actions.iter().any(|a| a.should_notify())),
            actions.iter().any(|a| a.is_highlight()),
        ),
        _ => (None, false),
    };

    let decision = NotificationDecision {
        is_sync_origin: true,
        is_own: false,
        is_notifiable_message: true,
        is_unread: true,
        caught_up: true,
        push_should_notify,
        mode: sdk_mode_to_app(room.notification_mode().await),
        is_highlight,
    };
    if !decision.should_notify() {
        return;
    }

    let sender_id = any_event.sender();
    // Resolve display name + avatar from one member lookup. The avatar is the
    // mxc:// url (empty if none / not cached); the C++ side resolves it to a local
    // file via MediaCache, mirroring evaluate_and_emit's callback args.
    let (sender_name, sender_avatar_url) = match room.get_member_no_sync(sender_id).await {
        Ok(Some(member)) => (
            member
                .display_name()
                .unwrap_or_else(|| sender_id.as_str())
                .to_string(),
            member
                .avatar_url()
                .map(|url| url.to_string())
                .unwrap_or_default(),
        ),
        _ => (sender_id.as_str().to_string(), String::new()),
    };
    let is_direct = room.direct_targets_length() > 0;
    let room_display_name = crate::room_summary_service::room_display_name(
        room,
        room.cached_display_name()
            .map(|name| name.to_string())
            .unwrap_or_default(),
    );
    let timestamp_secs = event_ms / 1000;

    match callback.lock() {
        Ok(guard) => {
            if let Some(cb) = guard.as_ref() {
                cb(
                    room.room_id().as_str(),
                    any_event.event_id().as_ref(),
                    &sender_name,
                    &sender_avatar_url,
                    &room_display_name,
                    &body,
                    is_direct,
                    is_highlight,
                    timestamp_secs,
                );
            }
        }
        Err(_) => warn!("notification callback mutex poisoned; skipping"),
    }
}

/// Sliding-sync invite path: resolve the inviter + room and fire the invite
/// notification callback. The caller (the room-list diff consumer) gates on
/// caught-up and dedups per room via [`invite_action`], so this only ever sees a
/// genuinely-new invite. Unlike messages an invite has no timeline event to
/// evaluate push rules against, so it always notifies (subject to the C++-side
/// focus/`desktopNotify` gates) — matching Matrix's default `.m.rule.invite_for_me`.
pub(crate) async fn notify_room_invite(room: &Room, callback: &InviteNotificationCallbackSlot) {
    // No callback registered -> nothing to deliver; skip the member lookup.
    if callback.lock().map(|g| g.is_none()).unwrap_or(true) {
        return;
    }

    // Inviter = the joined member who isn't us (mirrors
    // room_summary_service::invited_room_to_summary's extraction).
    let mut inviter_name = String::new();
    let mut inviter_avatar = String::new();
    if let Ok(members) = room.members(RoomMemberships::empty()).await {
        for member in &members {
            if member.user_id() != room.own_user_id()
                && *member.membership() == MembershipState::Join
            {
                inviter_name = member
                    .display_name()
                    .unwrap_or_else(|| member.user_id().as_str())
                    .to_string();
                inviter_avatar = member
                    .avatar_url()
                    .map(|u| u.to_string())
                    .unwrap_or_default();
                break;
            }
        }
    }

    let room_name = room
        .cached_display_name()
        .map(|n| n.to_string())
        .unwrap_or_default();
    let is_direct = room.direct_targets_length() > 0;

    match callback.lock() {
        Ok(guard) => {
            if let Some(cb) = guard.as_ref() {
                info!("[invite] desktop notification for {}", room.room_id());
                cb(
                    room.room_id().as_str(),
                    &inviter_name,
                    &inviter_avatar,
                    &room_name,
                    is_direct,
                );
            }
        }
        Err(_) => warn!("invite notification callback mutex poisoned; skipping"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn base() -> NotificationDecision {
        NotificationDecision {
            is_sync_origin: true,
            is_own: false,
            is_notifiable_message: true,
            is_unread: true,
            caught_up: true,
            push_should_notify: None,
            mode: RoomNotificationMode::AllMessages,
            is_highlight: false,
        }
    }

    #[test]
    fn notifies_live_unread_message() {
        assert!(base().should_notify());
    }

    #[test]
    fn suppresses_backfill_or_redecrypt() {
        let mut d = base();
        d.is_sync_origin = false;
        assert!(!d.should_notify());
    }

    #[test]
    fn suppresses_own_message() {
        let mut d = base();
        d.is_own = true;
        assert!(!d.should_notify());
    }

    #[test]
    fn suppresses_before_caught_up() {
        let mut d = base();
        d.caught_up = false;
        assert!(!d.should_notify());
    }

    #[test]
    fn suppresses_already_read() {
        let mut d = base();
        d.is_unread = false;
        assert!(!d.should_notify());
    }

    #[test]
    fn suppresses_service_event() {
        let mut d = base();
        d.is_notifiable_message = false;
        assert!(!d.should_notify());
    }

    #[test]
    fn mute_suppresses() {
        let mut d = base();
        d.mode = RoomNotificationMode::Mute;
        assert!(!d.should_notify());
    }

    #[test]
    fn mentions_only_requires_highlight() {
        let mut d = base();
        d.mode = RoomNotificationMode::MentionsOnly;
        assert!(!d.should_notify());
        d.is_highlight = true;
        assert!(d.should_notify());
    }

    #[test]
    fn push_actions_override_mode() {
        let mut d = base();
        d.mode = RoomNotificationMode::Mute;
        d.push_should_notify = Some(true);
        assert!(d.should_notify(), "a mention overrides a muted room");
        d.push_should_notify = Some(false);
        assert!(!d.should_notify());
    }

    fn ids(slice: &[&str]) -> Vec<Option<String>> {
        slice.iter().map(|s| Some((*s).to_string())).collect()
    }

    fn targets(slice: &[&str]) -> HashSet<String> {
        slice.iter().map(|s| (*s).to_string()).collect()
    }

    #[test]
    fn read_through_marks_items_up_to_receipt() {
        // Our receipt points at $c: $a,$b,$c are read (idx 0..=2); $d is unread.
        let batch = ids(&["$a", "$b", "$c", "$d"]);
        assert_eq!(read_through_index(&batch, &targets(&["$c"])), Some(2));
    }

    #[test]
    fn read_through_takes_furthest_receipt() {
        // Public + private receipts at $a and $c -> frontier is the furthest, $c.
        let batch = ids(&["$a", "$b", "$c"]);
        assert_eq!(read_through_index(&batch, &targets(&["$a", "$c"])), Some(2));
    }

    #[test]
    fn read_through_none_when_receipt_outside_batch() {
        let batch = ids(&["$a", "$b"]);
        assert_eq!(read_through_index(&batch, &targets(&["$z"])), None);
        assert_eq!(read_through_index(&batch, &HashSet::new()), None);
    }

    #[test]
    fn read_through_ignores_missing_event_ids() {
        // A non-event item (None) at idx 0 must not match or shift the frontier.
        let batch = vec![None, Some("$b".to_string()), Some("$c".to_string())];
        assert_eq!(read_through_index(&batch, &targets(&["$b"])), Some(1));
    }

    #[test]
    fn invite_new_after_caught_up_notifies() {
        assert_eq!(invite_action(true, true, false), InviteAction::Notify);
    }

    #[test]
    fn invite_before_caught_up_is_seeded_silently() {
        // Pending invites present at startup are recorded, never toasted.
        assert_eq!(invite_action(true, false, false), InviteAction::Record);
    }

    #[test]
    fn invite_already_known_is_ignored() {
        assert_eq!(invite_action(true, true, true), InviteAction::Ignore);
        assert_eq!(invite_action(true, false, true), InviteAction::Ignore);
    }

    #[test]
    fn leaving_invite_state_forgets_it() {
        // Joined/declined: drop it so a later re-invite notifies again.
        assert_eq!(invite_action(false, true, true), InviteAction::Forget);
        assert_eq!(invite_action(false, false, true), InviteAction::Forget);
    }

    #[test]
    fn non_invite_untracked_room_is_ignored() {
        assert_eq!(invite_action(false, true, false), InviteAction::Ignore);
        assert_eq!(invite_action(false, false, false), InviteAction::Ignore);
    }
}

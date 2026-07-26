// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::collections::{HashMap, HashSet};
use std::sync::Arc;
use std::time::{Duration, UNIX_EPOCH};

use matrix_sdk::ruma::events::room::message::{AudioMessageEventContent, MessageType};
use matrix_sdk::ruma::events::room::MediaSource;
use matrix_sdk::ruma::{OwnedEventId, OwnedUserId};
use matrix_sdk_crypto::types::events::UtdCause;
use matrix_sdk_ui::timeline::{
    EncryptedMessage, EventSendState, EventTimelineItem, MembershipChange, MsgLikeKind,
    Timeline as SdkTimeline, TimelineItemContent,
};

use crate::room_summary_service::RoomSummaryService;
use crate::types::{
    AudioInfo, ForwardedFrom, MessageContent, PollInfo, PollKind, PollOption, ReactionInfo,
    ReplyPreview, SendState, TimelineItem, UserProfile,
};

pub(crate) struct TimelineConversionService;

impl TimelineConversionService {
    fn utd_cause_message(cause: UtdCause) -> &'static str {
        match cause {
            UtdCause::SentBeforeWeJoined => {
                "Sent before you joined this chat, so its keys were never shared with you."
            }
            UtdCause::VerificationViolation => "The sender's verified identity has changed.",
            UtdCause::UnsignedDevice => "Sent from a device the sender hasn't verified.",
            UtdCause::UnknownDevice => "Sent from a device that's no longer available.",
            UtdCause::HistoricalMessageAndBackupIsDisabled => {
                "Sent before this device was set up. Earlier messages can't be opened here."
            }
            UtdCause::WithheldForUnverifiedOrInsecureDevice => {
                "The sender didn't share the keys with this device."
            }
            UtdCause::WithheldBySender => {
                "The sender chose not to share the keys for this message."
            }
            UtdCause::HistoricalMessageAndDeviceIsUnverified => {
                "Verify this device to read messages sent before you signed in."
            }
            UtdCause::Unknown => {
                "Re-establishing the secure session may restore access to this message."
            }
        }
    }

    pub(crate) fn convert_timeline_item(
        item: &matrix_sdk_ui::timeline::TimelineItem,
        pinned_event_ids: &HashSet<String>,
        media_sources: &Arc<std::sync::RwLock<HashMap<String, MediaSource>>>,
        own_user_id: &OwnedUserId,
        is_encrypted: bool,
        cached_reply_preview: Option<&ReplyPreview>,
    ) -> Option<TimelineItem> {
        let event = item.as_event()?;

        let event_id = event
            .event_id()
            .map(|id| id.to_string())
            .unwrap_or_default();
        // `transaction_id()` only covers LOCAL echoes. For our own already-sent
        // events (notably direct-upload media, which bypasses the send queue and
        // has no local echo) recover the server-echoed txn id from `unsigned` so
        // the C++ optimistic upload echo reconciles by transaction id.
        let transaction_id = event.transaction_id().map(|t| t.to_string()).or_else(|| {
            if !event.is_own() {
                return None;
            }
            #[derive(serde::Deserialize)]
            struct UnsignedTxn {
                transaction_id: Option<String>,
            }
            event
                .original_json()
                .and_then(|raw| raw.get_field::<UnsignedTxn>("unsigned").ok().flatten())
                .and_then(|unsigned| unsigned.transaction_id)
        });

        let sender_id = event.sender().to_string();
        let (sender_name, sender_avatar_url) = match event.sender_profile() {
            matrix_sdk_ui::timeline::TimelineDetails::Ready(profile) => (
                profile
                    .display_name
                    .as_deref()
                    .unwrap_or(&sender_id)
                    .to_string(),
                profile.avatar_url.as_ref().map(|u| u.to_string()),
            ),
            _ => (sender_id.clone(), None),
        };

        let timestamp_ms: u64 = event.timestamp().get().into();
        let timestamp = UNIX_EPOCH + Duration::from_millis(timestamp_ms);

        let is_outgoing = event.is_own();
        let (send_state, upload_progress) = Self::derive_send_state(
            event.send_state(),
            is_outgoing,
            &sender_id,
            event.read_receipts().keys().map(|u| u.as_str()),
        );

        let content_info = Self::convert_content(event, media_sources, own_user_id, &sender_name)?;

        // First point where an upload's transaction id and its final mxc meet:
        // cache the file we just sent, so rendering this echo doesn't download
        // it back. Remote echoes only — a local one has no event id.
        if is_outgoing && !event_id.is_empty() {
            if let (Some(txn_id), Some(url)) =
                (transaction_id.as_deref(), content_info.content.media_url())
            {
                crate::upload_seed_store::seed_if_pending(txn_id, url);
            }
        }

        let reply_preview = match event.content() {
            TimelineItemContent::MsgLike(msg_like) => Self::merge_reply_preview(
                Self::extract_reply_preview(msg_like, media_sources),
                None,
                cached_reply_preview,
            ),
            _ => None,
        };

        let resolved_event_id = if !event_id.is_empty() {
            event_id
        } else if let Some(txid) = transaction_id.clone() {
            txid
        } else {
            // Fallback for rare SDK items that don't expose event_id/txid yet.
            format!("$tm-{timestamp_ms}-{sender_id}")
        };

        let decryption_error = match &content_info.content {
            MessageContent::UnableToDecrypt { body, .. } => Some(body.clone()),
            _ => None,
        };

        Some(TimelineItem {
            is_pinned: pinned_event_ids.contains(&resolved_event_id),
            event_id: resolved_event_id,
            transaction_id,
            sender: UserProfile {
                user_id: sender_id,
                display_name: sender_name,
                avatar_url: sender_avatar_url,
            },
            timestamp,
            content: content_info.content,
            reply_to_event_id: content_info.reply_to,
            reply_preview,
            forwarded_from: Self::extract_forwarded_from(event),
            is_edited: content_info.is_edited,
            reactions: content_info.reactions,
            send_state,
            upload_progress,
            is_outgoing,
            is_deleted: content_info.is_redacted,
            url_preview: None,
            is_encrypted,
            decryption_error,
        })
    }

    /// Derive our `SendState` from the SDK event's local send state, ownership,
    /// sender, and read receipts.
    ///
    /// Rules:
    /// - Local echo + `NotSentYet`    -> `Sending`
    /// - Local echo + `SendingFailed` -> `Failed`
    /// - Local echo + `Sent`          -> `Sent` (server acknowledged but not yet
    ///   confirmed via remote echo)
    /// - Remote own event: if any receipt user != author -> `Read`, else `Sent`
    /// - Remote non-own event         -> `Sent` (we don't track read status for
    ///   other people's messages)
    pub(crate) fn derive_send_state<'a>(
        sdk_send_state: Option<&EventSendState>,
        is_own: bool,
        author_id: &str,
        receipt_user_ids: impl Iterator<Item = &'a str>,
    ) -> (SendState, f64) {
        // Local echo: use SDK sub-state.
        if let Some(state) = sdk_send_state {
            return match state {
                EventSendState::NotSentYet { progress } => {
                    let upload_progress = match progress {
                        Some(ref p) if p.progress.total > 0 => {
                            p.progress.current as f64 / p.progress.total as f64
                        }
                        Some(_) => 0.0,
                        None => -1.0,
                    };
                    (SendState::Sending, upload_progress)
                }
                EventSendState::SendingFailed { .. } => (SendState::Failed, -1.0),
                EventSendState::Sent { .. } => (SendState::Sent, -1.0),
            };
        }

        // Remote event.
        if is_own {
            // Check if any non-author user has a read receipt on this event.
            for uid in receipt_user_ids {
                if uid != author_id {
                    return (SendState::Read, -1.0);
                }
            }
            (SendState::Sent, -1.0)
        } else {
            (SendState::Sent, -1.0)
        }
    }

    pub(crate) fn convert_reactions(
        msg_like: &matrix_sdk_ui::timeline::MsgLikeContent,
        own_user_id: &OwnedUserId,
    ) -> Vec<ReactionInfo> {
        let mut reactions = Vec::new();
        for (key, senders) in msg_like.reactions.iter() {
            let count = senders.len() as u32;
            if count > 0 {
                reactions.push(ReactionInfo {
                    key: key.clone(),
                    count,
                    is_self: senders.contains_key(<&matrix_sdk::ruma::UserId>::from(own_user_id)),
                });
            }
        }
        reactions
    }

    /// Extract `com.telematrix.forwarded_from` from the raw event JSON stored
    /// on the server.  Returns `None` for local echoes (not yet echoed back)
    /// or events that were not sent via our forwarding path.
    /// Sender name + avatar from the SDK item's (possibly lazily-resolved)
    /// profile, falling back to the raw MXID when it isn't `Ready`. Mirrors the
    /// inline extraction in `convert_timeline_item`; used by
    /// `cache_timeline_snapshot`'s item-reuse fast path to refresh the sender
    /// when `fetch_members` resolves it — a profile-only change keeps the same
    /// content fingerprint, so a reused item would otherwise keep the MXID.
    pub(crate) fn extract_sender_profile(event: &EventTimelineItem) -> UserProfile {
        let sender_id = event.sender().to_string();
        match event.sender_profile() {
            matrix_sdk_ui::timeline::TimelineDetails::Ready(profile) => UserProfile {
                display_name: profile
                    .display_name
                    .as_deref()
                    .unwrap_or(&sender_id)
                    .to_string(),
                avatar_url: profile.avatar_url.as_ref().map(|u| u.to_string()),
                user_id: sender_id,
            },
            _ => UserProfile {
                display_name: sender_id.clone(),
                avatar_url: None,
                user_id: sender_id,
            },
        }
    }

    fn extract_forwarded_from(event: &EventTimelineItem) -> Option<ForwardedFrom> {
        let raw = event.original_json()?;
        let content: serde_json::Value = raw.get_field("content").ok().flatten()?;
        let fwd = content.get("com.telematrix.forwarded_from")?;
        let sender_name = fwd.get("sender_name")?.as_str()?;
        let opt = |key: &str| {
            fwd.get(key)
                .and_then(|v| v.as_str())
                .unwrap_or_default()
                .to_string()
        };
        Some(ForwardedFrom {
            sender_display_name: sender_name.to_string(),
            sender_id: opt("sender_id"),
            avatar_url: opt("sender_avatar_url"),
        })
    }

    fn convert_content(
        event: &EventTimelineItem,
        media_sources: &Arc<std::sync::RwLock<HashMap<String, MediaSource>>>,
        own_user_id: &OwnedUserId,
        sender_name: &str,
    ) -> Option<ContentInfo> {
        let timeline_content = event.content();

        match timeline_content {
            TimelineItemContent::MsgLike(msg_like) => {
                let reply_to = msg_like
                    .in_reply_to
                    .as_ref()
                    .map(|r| r.event_id.to_string());

                let reactions = Self::convert_reactions(msg_like, own_user_id);

                match &msg_like.kind {
                    MsgLikeKind::Message(message) => {
                        // In-room m.key.verification.request events are
                        // verification-protocol transport, not chat; rendered as
                        // a message they show up as "[Unsupported message type]".
                        // Drop them entirely (the verification banner/dialog
                        // drives the actual flow).
                        if matches!(message.msgtype(), MessageType::VerificationRequest(_)) {
                            return None;
                        }
                        let is_edited = message.is_edited();
                        let mut content =
                            Self::convert_message_type(message.msgtype(), media_sources);
                        // Inject blurhash from raw event JSON into Image/Video content.
                        if let Some(hash) = Self::extract_blurhash(event) {
                            match &mut content {
                                MessageContent::Image { blurhash, .. } => *blurhash = Some(hash),
                                MessageContent::Video { blurhash, .. } => *blurhash = Some(hash),
                                _ => {}
                            }
                        }
                        Some(ContentInfo {
                            content,
                            reply_to,
                            is_edited,
                            is_redacted: false,
                            reactions,
                        })
                    }
                    MsgLikeKind::Redacted => Some(ContentInfo {
                        content: MessageContent::Text {
                            body: String::from("[Message deleted]"),
                            formatted_body: None,
                        },
                        reply_to,
                        is_edited: false,
                        is_redacted: true,
                        reactions,
                    }),
                    MsgLikeKind::UnableToDecrypt(utd) => {
                        let (cause, session_id) = match utd {
                            EncryptedMessage::MegolmV1AesSha2 {
                                cause, session_id, ..
                            } => (*cause, Some(session_id.clone())),
                            _ => (UtdCause::Unknown, None),
                        };
                        // Glow (0) while decryption can still resolve; terminal (1)
                        // only when the key is genuinely unrecoverable. The historical
                        // causes clear once this device is verified + the backup is
                        // enabled (exactly the post-verification flow), so they must
                        // glow during that window — otherwise a fresh device shows
                        // static "unable to decrypt" cards that then silently turn into
                        // plaintext. The C++ 55s safety downgrade turns a genuinely
                        // stuck glow into a terminal card.
                        let utd_state = match cause {
                            UtdCause::Unknown
                            | UtdCause::HistoricalMessageAndDeviceIsUnverified
                            | UtdCause::HistoricalMessageAndBackupIsDisabled => 0,
                            _ => 1,
                        };
                        Some(ContentInfo {
                            content: MessageContent::UnableToDecrypt {
                                body: String::from(Self::utd_cause_message(cause)),
                                cause: cause as u8,
                                utd_state,
                                session_id,
                            },
                            reply_to,
                            is_edited: false,
                            is_redacted: false,
                            reactions,
                        })
                    }
                    MsgLikeKind::Sticker(_) => Some(ContentInfo {
                        content: MessageContent::Text {
                            body: String::from("[Sticker]"),
                            formatted_body: None,
                        },
                        reply_to,
                        is_edited: false,
                        is_redacted: false,
                        reactions,
                    }),
                    MsgLikeKind::Poll(poll_state) => {
                        let results = poll_state.results();
                        let question = results.question.clone();
                        let kind = match results.kind {
                            matrix_sdk::ruma::events::poll::start::PollKind::Disclosed => {
                                PollKind::Disclosed
                            }
                            _ => PollKind::Undisclosed,
                        };
                        let max_selections = results.max_selections.try_into().unwrap_or(1);
                        let is_closed = results.end_time.is_some();
                        let own_user_id = own_user_id.as_str();

                        let mut options = Vec::with_capacity(results.answers.len());
                        let mut all_voters = HashSet::new();
                        let mut has_voted = false;

                        for answer in &results.answers {
                            let votes = results.votes.get(&answer.id).cloned().unwrap_or_default();
                            let vote_count = votes.len() as u32;
                            let is_chosen = votes.iter().any(|user_id| user_id == own_user_id);
                            if is_chosen {
                                has_voted = true;
                            }
                            for user_id in &votes {
                                all_voters.insert(user_id.clone());
                            }
                            options.push(PollOption {
                                id: answer.id.clone(),
                                text: answer.text.clone(),
                                vote_count,
                                is_chosen,
                                is_correct: false,
                            });
                        }

                        Some(ContentInfo {
                            content: MessageContent::Poll {
                                info: PollInfo {
                                    question,
                                    kind,
                                    max_selections,
                                    is_closed,
                                    is_quiz: false,
                                    total_voters: all_voters.len() as u32,
                                    options,
                                    has_voted,
                                },
                            },
                            reply_to,
                            is_edited: false,
                            is_redacted: false,
                            reactions,
                        })
                    }
                    MsgLikeKind::Other(other) => {
                        let event_type = other.event_type();
                        // Hide in-room verification-protocol events
                        // (ready/start/key/mac/done/cancel/accept) — transport,
                        // like the request message handled above.
                        if event_type.to_string().starts_with("m.key.verification.") {
                            return None;
                        }
                        Some(ContentInfo {
                            content: MessageContent::Service {
                                body: format!("Event: {event_type}"),
                            },
                            reply_to,
                            is_edited: false,
                            is_redacted: false,
                            reactions,
                        })
                    }
                    MsgLikeKind::LiveLocation(_) => Some(ContentInfo {
                        content: MessageContent::Text {
                            body: String::from("[Location]"),
                            formatted_body: None,
                        },
                        reply_to,
                        is_edited: false,
                        is_redacted: false,
                        reactions,
                    }),
                }
            }
            TimelineItemContent::MembershipChange(change) => Some(ContentInfo {
                content: MessageContent::Service {
                    body: Self::format_membership_change(change),
                },
                reply_to: None,
                is_edited: false,
                is_redacted: false,
                reactions: Vec::new(),
            }),
            TimelineItemContent::ProfileChange(change) => Some(ContentInfo {
                content: MessageContent::Service {
                    body: Self::format_profile_change(change, sender_name),
                },
                reply_to: None,
                is_edited: false,
                is_redacted: false,
                reactions: Vec::new(),
            }),
            TimelineItemContent::OtherState(state) => Some(ContentInfo {
                content: MessageContent::Service {
                    body: Self::format_other_state(state, sender_name),
                },
                reply_to: None,
                is_edited: false,
                is_redacted: false,
                reactions: Vec::new(),
            }),
            TimelineItemContent::CallInvite => Some(ContentInfo {
                content: MessageContent::Service {
                    body: format!("{sender_name} started a call"),
                },
                reply_to: None,
                is_edited: false,
                is_redacted: false,
                reactions: Vec::new(),
            }),
            TimelineItemContent::RtcNotification { .. } => Some(ContentInfo {
                content: MessageContent::Service {
                    body: format!("{sender_name} started a call"),
                },
                reply_to: None,
                is_edited: false,
                is_redacted: false,
                reactions: Vec::new(),
            }),
            TimelineItemContent::FailedToParseMessageLike { event_type, .. } => Some(ContentInfo {
                content: MessageContent::Service {
                    body: format!("Unsupported event: {event_type}"),
                },
                reply_to: None,
                is_edited: false,
                is_redacted: false,
                reactions: Vec::new(),
            }),
            TimelineItemContent::FailedToParseState {
                event_type,
                state_key,
                ..
            } => Some(ContentInfo {
                content: MessageContent::Service {
                    body: format!("Unsupported state event: {event_type} ({state_key})"),
                },
                reply_to: None,
                is_edited: false,
                is_redacted: false,
                reactions: Vec::new(),
            }),
        }
    }

    /// Extract blurhash from the raw event's `content.info.xyz.amorgan.blurhash`.
    fn extract_blurhash(event: &EventTimelineItem) -> Option<String> {
        let raw = event.original_json()?;
        let content: serde_json::Value = raw.get_field("content").ok().flatten()?;
        let info = content.get("info")?;
        info.get("xyz.amorgan.blurhash")
            .and_then(|v| v.as_str())
            .map(|s| s.to_string())
    }

    fn format_membership_change(change: &matrix_sdk_ui::timeline::RoomMembershipChange) -> String {
        let subject = change
            .display_name()
            .filter(|name| !name.trim().is_empty())
            .unwrap_or_else(|| change.user_id().to_string());

        let action = match change.change() {
            Some(MembershipChange::Joined) => "joined the room",
            Some(MembershipChange::Left) => "left the room",
            Some(MembershipChange::Banned) => "was banned",
            Some(MembershipChange::Unbanned) => "was unbanned",
            Some(MembershipChange::Kicked) => "was kicked",
            Some(MembershipChange::Invited) => "was invited",
            Some(MembershipChange::KickedAndBanned) => "was kicked and banned",
            Some(MembershipChange::InvitationAccepted) => "accepted the invitation",
            Some(MembershipChange::InvitationRejected) => "rejected the invitation",
            Some(MembershipChange::InvitationRevoked) => "had the invitation revoked",
            Some(MembershipChange::Knocked) => "knocked",
            Some(MembershipChange::KnockAccepted) => "had the knock accepted",
            Some(MembershipChange::KnockRetracted) => "retracted the knock",
            Some(MembershipChange::KnockDenied) => "had the knock denied",
            Some(MembershipChange::None)
            | Some(MembershipChange::Error)
            | Some(MembershipChange::NotImplemented)
            | None => "membership changed",
        };

        format!("{subject} {action}")
    }

    fn format_profile_change(
        change: &matrix_sdk_ui::timeline::MemberProfileChange,
        sender: &str,
    ) -> String {
        match (
            change.displayname_change().is_some(),
            change.avatar_url_change().is_some(),
        ) {
            (true, true) => format!("{sender} changed display name and avatar"),
            (true, false) => format!("{sender} changed display name"),
            (false, true) => format!("{sender} changed avatar"),
            (false, false) => format!("{sender} updated profile"),
        }
    }

    fn format_other_state(state: &matrix_sdk_ui::timeline::OtherState, sender: &str) -> String {
        let event_type = state.content().event_type().to_string();
        match event_type.as_str() {
            "m.room.create" => format!("{sender} created the room"),
            "m.room.name" => format!("{sender} changed the room name"),
            "m.room.topic" => format!("{sender} changed the room topic"),
            "m.room.avatar" => format!("{sender} changed the room avatar"),
            "m.room.pinned_events" => format!("{sender} changed pinned messages"),
            "m.room.join_rules" => format!("{sender} changed join rules"),
            "m.room.power_levels" => format!("{sender} changed permissions"),
            "m.room.history_visibility" => format!("{sender} changed history visibility"),
            "m.room.guest_access" => format!("{sender} changed guest access"),
            "m.room.encryption" => format!("{sender} enabled encryption"),
            "m.room.tombstone" => format!("{sender} replaced the room"),
            "m.room.canonical_alias" => format!("{sender} changed the room address"),
            "m.room.server_acl" => format!("{sender} changed server access rules"),
            "m.room.third_party_invite" => format!("{sender} sent a third-party invite"),
            _ => format!("{sender}: state event {event_type}"),
        }
    }

    fn reply_preview_sender_name(
        profile: &matrix_sdk_ui::timeline::TimelineDetails<matrix_sdk_ui::timeline::Profile>,
        sender: &OwnedUserId,
    ) -> String {
        match profile {
            matrix_sdk_ui::timeline::TimelineDetails::Ready(profile) => profile
                .display_name
                .clone()
                .unwrap_or_else(|| sender.to_string()),
            _ => sender.to_string(),
        }
    }

    fn reply_preview_from_message_content(content: &MessageContent) -> ReplyPreviewInfo {
        match content {
            MessageContent::Image {
                caption,
                thumbnail_url,
                url,
                ..
            } => ReplyPreviewInfo {
                text: caption
                    .clone()
                    .filter(|text| !text.is_empty())
                    .unwrap_or_else(|| String::from("Photo")),
                thumb_url: thumbnail_url
                    .clone()
                    .or_else(|| (!url.is_empty()).then_some(url.clone())),
                has_thumb: thumbnail_url.is_some() || !url.is_empty(),
                is_text_colorized: caption.as_ref().map(|text| text.is_empty()).unwrap_or(true),
                ..ReplyPreviewInfo::default()
            },
            MessageContent::File {
                filename, caption, ..
            } => ReplyPreviewInfo {
                text: caption
                    .clone()
                    .filter(|text| !text.is_empty())
                    .or_else(|| (!filename.is_empty()).then_some(filename.clone()))
                    .unwrap_or_else(|| String::from("File")),
                is_text_colorized: caption
                    .as_ref()
                    .map(|text| text.is_empty())
                    .unwrap_or(filename.is_empty()),
                ..ReplyPreviewInfo::default()
            },
            MessageContent::Audio { info } => ReplyPreviewInfo {
                text: if !info.filename.is_empty() {
                    info.filename.clone()
                } else {
                    String::from("Audio")
                },
                is_text_colorized: info.filename.is_empty(),
                ..ReplyPreviewInfo::default()
            },
            MessageContent::Video {
                caption,
                thumbnail_url,
                url,
                ..
            } => ReplyPreviewInfo {
                text: caption
                    .clone()
                    .filter(|text| !text.is_empty())
                    .unwrap_or_else(|| String::from("Video")),
                thumb_url: thumbnail_url
                    .clone()
                    .or_else(|| (!url.is_empty()).then_some(url.clone())),
                has_thumb: thumbnail_url.is_some() || !url.is_empty(),
                is_text_colorized: caption.as_ref().map(|text| text.is_empty()).unwrap_or(true),
                ..ReplyPreviewInfo::default()
            },
            MessageContent::Service { body } => ReplyPreviewInfo {
                text: if body.is_empty() {
                    String::from("Service message")
                } else {
                    body.clone()
                },
                ..ReplyPreviewInfo::default()
            },
            MessageContent::Poll { info } => ReplyPreviewInfo {
                text: if !info.question.is_empty() {
                    info.question.clone()
                } else {
                    String::from("Poll")
                },
                is_text_colorized: info.question.is_empty(),
                ..ReplyPreviewInfo::default()
            },
            MessageContent::UnableToDecrypt { body, .. } => ReplyPreviewInfo {
                text: body.clone(),
                ..ReplyPreviewInfo::default()
            },
            MessageContent::Text { body, .. } => ReplyPreviewInfo {
                text: if body.is_empty() {
                    String::from("Message")
                } else {
                    body.clone()
                },
                ..ReplyPreviewInfo::default()
            },
        }
    }

    fn reply_preview_from_timeline_content(
        content: &TimelineItemContent,
        sender_name: &str,
        media_sources: &Arc<std::sync::RwLock<HashMap<String, MediaSource>>>,
    ) -> ReplyPreviewInfo {
        match content {
            TimelineItemContent::MsgLike(msg_like) => match &msg_like.kind {
                MsgLikeKind::Message(message) => Self::reply_preview_from_message_content(
                    &Self::convert_message_type(message.msgtype(), media_sources),
                ),
                MsgLikeKind::Redacted => ReplyPreviewInfo {
                    text: String::from("Deleted message"),
                    is_deleted: true,
                    ..ReplyPreviewInfo::default()
                },
                MsgLikeKind::UnableToDecrypt(_) => ReplyPreviewInfo {
                    text: String::from("Unable to decrypt this message"),
                    ..ReplyPreviewInfo::default()
                },
                MsgLikeKind::Sticker(_) => ReplyPreviewInfo {
                    text: String::from("[Sticker]"),
                    ..ReplyPreviewInfo::default()
                },
                MsgLikeKind::Poll(poll_state) => ReplyPreviewInfo {
                    text: poll_state.results().question.clone(),
                    ..ReplyPreviewInfo::default()
                },
                MsgLikeKind::Other(other) => ReplyPreviewInfo {
                    text: format!("Event: {}", other.event_type()),
                    ..ReplyPreviewInfo::default()
                },
                MsgLikeKind::LiveLocation(_) => ReplyPreviewInfo {
                    text: String::from("[Location]"),
                    ..ReplyPreviewInfo::default()
                },
            },
            TimelineItemContent::MembershipChange(change) => ReplyPreviewInfo {
                text: Self::format_membership_change(change),
                ..ReplyPreviewInfo::default()
            },
            TimelineItemContent::ProfileChange(change) => ReplyPreviewInfo {
                text: Self::format_profile_change(change, sender_name),
                ..ReplyPreviewInfo::default()
            },
            TimelineItemContent::OtherState(state) => ReplyPreviewInfo {
                text: Self::format_other_state(state, sender_name),
                ..ReplyPreviewInfo::default()
            },
            TimelineItemContent::CallInvite | TimelineItemContent::RtcNotification { .. } => {
                ReplyPreviewInfo {
                    text: format!("{sender_name} started a call"),
                    ..ReplyPreviewInfo::default()
                }
            }
            TimelineItemContent::FailedToParseMessageLike { event_type, .. } => ReplyPreviewInfo {
                text: format!("Unsupported event: {event_type}"),
                ..ReplyPreviewInfo::default()
            },
            TimelineItemContent::FailedToParseState {
                event_type,
                state_key,
                ..
            } => ReplyPreviewInfo {
                text: format!("Unsupported state event: {event_type} ({state_key})"),
                ..ReplyPreviewInfo::default()
            },
        }
    }

    pub(crate) fn extract_reply_preview(
        msg_like: &matrix_sdk_ui::timeline::MsgLikeContent,
        media_sources: &Arc<std::sync::RwLock<HashMap<String, MediaSource>>>,
    ) -> Option<ReplyPreview> {
        let details = msg_like.in_reply_to.as_ref()?;
        match &details.event {
            matrix_sdk_ui::timeline::TimelineDetails::Ready(event) => {
                let sender_name =
                    Self::reply_preview_sender_name(&event.sender_profile, &event.sender);
                let mut preview = Self::reply_preview_from_timeline_content(
                    &event.content,
                    &sender_name,
                    media_sources,
                );
                preview.sender_display_name = sender_name;
                Some(ReplyPreview {
                    sender_display_name: preview.sender_display_name,
                    text: preview.text,
                    thumb_url: preview.thumb_url,
                    has_thumb: preview.has_thumb,
                    is_text_colorized: preview.is_text_colorized,
                    is_deleted: preview.is_deleted,
                    is_unavailable: preview.is_unavailable,
                })
            }
            matrix_sdk_ui::timeline::TimelineDetails::Unavailable
            | matrix_sdk_ui::timeline::TimelineDetails::Pending
            | matrix_sdk_ui::timeline::TimelineDetails::Error(_) => Some(ReplyPreview {
                sender_display_name: String::new(),
                text: String::new(),
                thumb_url: None,
                has_thumb: false,
                is_text_colorized: false,
                is_deleted: false,
                is_unavailable: true,
            }),
        }
    }

    pub(crate) fn merge_reply_preview(
        current: Option<ReplyPreview>,
        active_slice_cached: Option<&ReplyPreview>,
        persisted: Option<&ReplyPreview>,
    ) -> Option<ReplyPreview> {
        let stable = active_slice_cached
            .filter(|preview| !preview.is_unavailable)
            .or_else(|| persisted.filter(|preview| !preview.is_unavailable));
        match current {
            Some(preview) if preview.is_unavailable => stable.cloned().or(Some(preview)),
            Some(preview) => Some(preview),
            None => stable.cloned(),
        }
    }

    pub(crate) async fn schedule_reply_details_prefetch(
        timeline: &SdkTimeline,
        items: &[Arc<matrix_sdk_ui::timeline::TimelineItem>],
    ) -> bool {
        let mut requested: HashSet<OwnedEventId> = HashSet::new();
        for entry in items.iter() {
            let Some(item) = entry.as_event() else {
                continue;
            };
            let TimelineItemContent::MsgLike(msg_like) = item.content() else {
                continue;
            };
            let Some(in_reply_to) = msg_like.in_reply_to.as_ref() else {
                continue;
            };
            if !matches!(
                &in_reply_to.event,
                matrix_sdk_ui::timeline::TimelineDetails::Unavailable
            ) {
                continue;
            }
            let Some(event_id): Option<OwnedEventId> = item
                .event_id()
                .map(|id: &matrix_sdk::ruma::EventId| id.to_owned())
            else {
                continue;
            };
            if !requested.insert(event_id.clone()) {
                continue;
            }
            let _ = timeline.fetch_details_for_event(&event_id).await;
            if requested.len() >= 12 {
                break;
            }
        }
        !requested.is_empty()
    }

    fn extract_mxc_url(source: &MediaSource) -> String {
        match source {
            MediaSource::Plain(mxc_uri) => mxc_uri.to_string(),
            MediaSource::Encrypted(encrypted) => encrypted.url.to_string(),
        }
    }

    pub(crate) fn remember_media_source(
        source: &MediaSource,
        media_sources: &Arc<std::sync::RwLock<HashMap<String, MediaSource>>>,
    ) -> String {
        let url = Self::extract_mxc_url(source);
        if url.starts_with("mxc://") {
            if let Ok(mut cache) = media_sources.write() {
                cache.insert(url.clone(), source.clone());
            }
        }
        url
    }

    /// Quantize a raw 16-bit waveform amplitude into the 0..=31 bucket
    /// range: scale by 31/1024 with rounding (the `+512` half-step), then hard-cap
    /// at 31. Pure so the formula and the cap are unit-testable without an SDK event.
    fn scale_waveform_amplitude(raw: u16) -> u8 {
        let scaled = ((u32::from(raw) * 31) + 512) / 1024;
        u8::try_from(scaled.min(31)).unwrap_or(31)
    }

    fn extract_waveform(audio: &AudioMessageEventContent) -> Vec<u8> {
        audio
            .audio
            .as_ref()
            .map(|details| {
                details
                    .waveform
                    .iter()
                    .map(|amplitude| {
                        // Defensive: amplitudes are spec'd 0..=1024 but clamp to u16.
                        let raw = u16::try_from(u64::from(amplitude.get())).unwrap_or(0);
                        Self::scale_waveform_amplitude(raw)
                    })
                    .collect()
            })
            .unwrap_or_default()
    }

    pub(crate) fn convert_message_type(
        msgtype: &MessageType,
        media_sources: &Arc<std::sync::RwLock<HashMap<String, MediaSource>>>,
    ) -> MessageContent {
        match msgtype {
            MessageType::Text(text) => MessageContent::Text {
                body: text.body.clone(),
                formatted_body: text.formatted.as_ref().map(|f| f.body.clone()),
            },
            MessageType::Emote(emote) => MessageContent::Text {
                body: format!("* {}", emote.body),
                formatted_body: emote.formatted.as_ref().map(|f| format!("* {}", f.body)),
            },
            MessageType::Notice(notice) => MessageContent::Text {
                body: notice.body.clone(),
                formatted_body: notice.formatted.as_ref().map(|f| f.body.clone()),
            },
            MessageType::Image(image) => {
                let info = image.info.as_ref();
                // Debug logging moved to convert_content (event not in scope here).
                let mime = info
                    .and_then(|i| i.mimetype.clone())
                    .unwrap_or_else(|| String::from("image/jpeg"));
                let filename = image.filename().to_string();
                // Some Matrix clients send PDFs as m.image — treat as file.
                let is_pdf = mime == "application/pdf" || filename.to_lowercase().ends_with(".pdf");
                if is_pdf {
                    MessageContent::File {
                        url: Self::remember_media_source(&image.source, media_sources),
                        mime_type: mime,
                        filename,
                        caption: image.caption().map(|s| s.to_string()),
                        size: info.and_then(|i| i.size).map(u64::from).unwrap_or(0),
                        duration_ms: 0,
                    }
                } else {
                    MessageContent::Image {
                        url: Self::remember_media_source(&image.source, media_sources),
                        mime_type: mime,
                        filename,
                        caption: image.caption().map(|s| s.to_string()),
                        thumbnail_url: info
                            .and_then(|i| i.thumbnail_source.as_ref())
                            .map(|source| Self::remember_media_source(source, media_sources)),
                        blurhash: None, // injected from raw event JSON in convert_content
                        size: info.and_then(|i| i.size).map(u64::from).unwrap_or(0),
                        width: info
                            .and_then(|i| i.width)
                            .map(|v| u64::from(v) as u32)
                            .unwrap_or(0),
                        height: info
                            .and_then(|i| i.height)
                            .map(|v| u64::from(v) as u32)
                            .unwrap_or(0),
                    }
                }
            }
            MessageType::File(file) => {
                let info = file.info.as_ref();
                MessageContent::File {
                    url: Self::remember_media_source(&file.source, media_sources),
                    mime_type: info
                        .and_then(|i| i.mimetype.clone())
                        .unwrap_or_else(|| String::from("application/octet-stream")),
                    filename: file.filename().to_string(),
                    caption: file.caption().map(|s| s.to_string()),
                    size: info.and_then(|i| i.size).map(u64::from).unwrap_or(0),
                    duration_ms: 0,
                }
            }
            MessageType::Video(video) => {
                let info = video.info.as_ref();
                MessageContent::Video {
                    url: Self::remember_media_source(&video.source, media_sources),
                    mime_type: info
                        .and_then(|i| i.mimetype.clone())
                        .unwrap_or_else(|| String::from("video/mp4")),
                    filename: video.filename().to_string(),
                    caption: video.caption().map(|s| s.to_string()),
                    thumbnail_url: info
                        .and_then(|i| i.thumbnail_source.as_ref())
                        .map(|source| Self::remember_media_source(source, media_sources)),
                    blurhash: None, // injected from raw event JSON in convert_content
                    size: info.and_then(|i| i.size).map(u64::from).unwrap_or(0),
                    width: info
                        .and_then(|i| i.width)
                        .map(|v| u64::from(v) as u32)
                        .unwrap_or(0),
                    height: info
                        .and_then(|i| i.height)
                        .map(|v| u64::from(v) as u32)
                        .unwrap_or(0),
                    duration_ms: info
                        .and_then(|i| i.duration)
                        .map(|d| d.as_millis() as u64)
                        .unwrap_or(0),
                }
            }
            MessageType::Audio(audio) => {
                let info = audio.info.as_ref();
                let unstable_audio = audio.audio.as_ref();
                let url = Self::remember_media_source(&audio.source, media_sources);
                let mut duration_ms = info
                    .and_then(|i| i.duration)
                    .or_else(|| unstable_audio.map(|details| details.duration))
                    .map(|d| d.as_millis() as u64)
                    .unwrap_or(0);
                if duration_ms == 0 {
                    // Server omitted the duration; use the value learned from a
                    // previous play/probe (persisted in the state store).
                    duration_ms = crate::audio_duration_store::cached_duration_ms(&url);
                }
                MessageContent::Audio {
                    info: AudioInfo {
                        url,
                        mime_type: info
                            .and_then(|i| i.mimetype.clone())
                            .unwrap_or_else(|| String::from("audio/ogg")),
                        filename: audio.filename().to_string(),
                        size: info.and_then(|i| i.size).map(u64::from).unwrap_or(0),
                        duration_ms,
                        is_voice: RoomSummaryService::detect_voice_message(audio),
                        waveform: Self::extract_waveform(audio),
                    },
                }
            }
            _ => MessageContent::Text {
                body: String::from("[Unsupported message type]"),
                formatted_body: None,
            },
        }
    }
}

struct ContentInfo {
    content: MessageContent,
    reply_to: Option<String>,
    is_edited: bool,
    is_redacted: bool,
    reactions: Vec<ReactionInfo>,
}

#[derive(Debug, Clone, Default)]
struct ReplyPreviewInfo {
    sender_display_name: String,
    text: String,
    thumb_url: Option<String>,
    has_thumb: bool,
    is_text_colorized: bool,
    is_deleted: bool,
    is_unavailable: bool,
}

#[cfg(test)]
mod tests {
    use super::*;

    // ---- scale_waveform_amplitude: the 0..=31 quantization formula ----

    #[test]
    fn waveform_zero_is_zero() {
        assert_eq!(TimelineConversionService::scale_waveform_amplitude(0), 0);
    }

    #[test]
    fn waveform_rounds_with_half_step() {
        // (512*31 + 512) / 1024 = 16384 / 1024 = 16  (exact)
        assert_eq!(TimelineConversionService::scale_waveform_amplitude(512), 16);
        // (256*31 + 512) / 1024 = 8448 / 1024 = 8     (truncated 8.25)
        assert_eq!(TimelineConversionService::scale_waveform_amplitude(256), 8);
    }

    #[test]
    fn waveform_reaches_cap_at_expected_boundary() {
        // raw=1008 → (1008*31+512)/1024 = 31744/1024 = 31 (first value hitting 31).
        assert_eq!(
            TimelineConversionService::scale_waveform_amplitude(1008),
            31
        );
        // raw=1007 → 30 (one below the boundary).
        assert_eq!(
            TimelineConversionService::scale_waveform_amplitude(1007),
            30
        );
    }

    #[test]
    fn waveform_caps_large_values_at_31() {
        // Above the spec'd 0..=1024 range the .min(31) cap must hold.
        assert_eq!(
            TimelineConversionService::scale_waveform_amplitude(1040),
            31
        );
        assert_eq!(
            TimelineConversionService::scale_waveform_amplitude(u16::MAX),
            31
        );
    }

    // ---- derive_send_state: branch missing from matrix.rs's suite -------------

    #[test]
    fn local_not_sent_yet_with_zero_total_progress_is_indeterminate() {
        // `progress: Some(_)` but total == 0 must map to 0.0 (not divide-by-zero,
        // not the -1.0 "no progress info" sentinel).
        let state = EventSendState::NotSentYet {
            progress: Some(matrix_sdk_ui::timeline::MediaUploadProgress {
                index: 0,
                progress: matrix_sdk::send_queue::AbstractProgress {
                    current: 0,
                    total: 0,
                },
            }),
        };
        let result = TimelineConversionService::derive_send_state(
            Some(&state),
            true,
            "@alice:example.org",
            std::iter::empty(),
        );
        assert_eq!(result, (SendState::Sending, 0.0));
    }

    // ---- convert_message_type: msgtype -> MessageContent mapping --------------

    use matrix_sdk::ruma::events::room::message::{
        FileMessageEventContent, ImageMessageEventContent, TextMessageEventContent,
    };
    use matrix_sdk::ruma::events::room::ImageInfo;
    use matrix_sdk::ruma::OwnedMxcUri;
    use std::sync::RwLock as StdRwLock;

    fn media_sources() -> Arc<StdRwLock<HashMap<String, MediaSource>>> {
        Arc::new(StdRwLock::new(HashMap::new()))
    }

    fn mxc() -> OwnedMxcUri {
        OwnedMxcUri::from("mxc://example.org/abc")
    }

    #[test]
    fn text_maps_with_formatted_body() {
        let mut text = TextMessageEventContent::plain("hi");
        text.formatted =
            Some(matrix_sdk::ruma::events::room::message::FormattedBody::html("<b>hi</b>"));
        let content = TimelineConversionService::convert_message_type(
            &MessageType::Text(text),
            &media_sources(),
        );
        match content {
            MessageContent::Text {
                body,
                formatted_body,
            } => {
                assert_eq!(body, "hi");
                assert_eq!(formatted_body.as_deref(), Some("<b>hi</b>"));
            }
            other => panic!("expected Text, got {other:?}"),
        }
    }

    // Regression: some clients send PDFs as m.image. By mimetype it must redirect
    // to a File, not render as a (broken) image.
    #[test]
    fn image_with_pdf_mimetype_redirects_to_file() {
        let mut info = ImageInfo::new();
        info.mimetype = Some("application/pdf".to_string());
        let mut image = ImageMessageEventContent::plain("report".to_string(), mxc());
        image.info = Some(Box::new(info));
        let content = TimelineConversionService::convert_message_type(
            &MessageType::Image(image),
            &media_sources(),
        );
        assert!(
            matches!(content, MessageContent::File { .. }),
            "PDF-mimetype image must become a File, got {content:?}"
        );
    }

    // The same redirect by filename extension, with no info block at all.
    #[test]
    fn image_with_pdf_filename_redirects_to_file() {
        let image = ImageMessageEventContent::plain("invoice.pdf".to_string(), mxc());
        let content = TimelineConversionService::convert_message_type(
            &MessageType::Image(image),
            &media_sources(),
        );
        assert!(matches!(content, MessageContent::File { .. }));
    }

    #[test]
    fn plain_image_keeps_image_kind_with_default_mime() {
        // No info block -> the default image mime is applied, not octet-stream.
        let image = ImageMessageEventContent::plain("photo".to_string(), mxc());
        let content = TimelineConversionService::convert_message_type(
            &MessageType::Image(image),
            &media_sources(),
        );
        match content {
            MessageContent::Image {
                mime_type,
                filename,
                ..
            } => {
                assert_eq!(mime_type, "image/jpeg");
                assert_eq!(filename, "photo");
            }
            other => panic!("expected Image, got {other:?}"),
        }
    }

    #[test]
    fn file_without_info_defaults_to_octet_stream() {
        let file = FileMessageEventContent::plain("data.bin".to_string(), mxc());
        let content = TimelineConversionService::convert_message_type(
            &MessageType::File(file),
            &media_sources(),
        );
        match content {
            MessageContent::File { mime_type, .. } => {
                assert_eq!(mime_type, "application/octet-stream");
            }
            other => panic!("expected File, got {other:?}"),
        }
    }
}

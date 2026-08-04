// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

use std::collections::{HashMap, HashSet};
use std::sync::Arc;
use std::time::Duration;

use anyhow::{anyhow, Result};
use matrix_sdk::attachment::{
    AttachmentConfig, AttachmentInfo, BaseAudioInfo, BaseFileInfo, BaseImageInfo, BaseVideoInfo,
};
use matrix_sdk::room::edit::EditedContent;
use matrix_sdk::ruma::events::room::message::{
    AudioMessageEventContent, FileMessageEventContent, FormattedBody, ImageMessageEventContent,
    MessageType, RoomMessageEventContent, RoomMessageEventContentWithoutRelation,
    VideoMessageEventContent,
};
use matrix_sdk::ruma::events::room::MediaSource;
use matrix_sdk::ruma::events::Mentions;
use matrix_sdk::ruma::{OwnedEventId, OwnedTransactionId, OwnedUserId, TransactionId};
use matrix_sdk::{Client, Room};
use matrix_sdk_ui::timeline::{Timeline as SdkTimeline, TimelineEventItemId};
use tokio::sync::RwLock;
use tracing::{info, warn};

use crate::timeline_cache_service::TimelineCacheService;
use crate::timeline_conversion_service::TimelineConversionService;
use crate::timeline_update_service::TimelineUpdateService;
use crate::types::{MessageContent, TimelineItem, UrlPreview};

type TimelineCache = Arc<RwLock<HashMap<String, Vec<TimelineItem>>>>;
type PendingReactionOverrides = Arc<RwLock<HashMap<(String, String, String), bool>>>;

pub(crate) struct ForwardMessagePayload {
    content: MessageContent,
    source_sender_name: String,
    source_sender_id: String,
    source_sender_avatar: String,
}

pub(crate) struct MessageActionService;

impl MessageActionService {
    pub(crate) async fn send_message(
        timeline: Arc<SdkTimeline>,
        body: &str,
        formatted_body: Option<&str>,
        reply_to_event_id: Option<&str>,
    ) -> Result<String> {
        let mentions = Self::extract_mentions(body, formatted_body);

        if let Some(reply_to) = reply_to_event_id {
            let event_id: OwnedEventId = reply_to
                .try_into()
                .map_err(|_| anyhow!("Invalid reply-to event ID"))?;
            let mut content = match formatted_body {
                Some(html) => RoomMessageEventContentWithoutRelation::text_html(body, html),
                None => RoomMessageEventContentWithoutRelation::text_plain(body),
            };
            if let Some(mentions) = mentions.clone() {
                content = content.add_mentions(mentions);
            }
            timeline.send_reply(content, event_id).await?;
        } else {
            let mut content = match formatted_body {
                Some(html) => RoomMessageEventContent::text_html(body, html),
                None => RoomMessageEventContent::text_plain(body),
            };
            if let Some(mentions) = mentions {
                content = content.add_mentions(mentions);
            }
            timeline.send(content.into()).await?;
        }

        Ok(String::new())
    }

    pub(crate) async fn edit_message(
        timeline: Arc<SdkTimeline>,
        event_id: &str,
        body: &str,
        formatted_body: Option<&str>,
        as_media_caption: bool,
    ) -> Result<String> {
        let mentions = Self::extract_mentions(body, formatted_body);

        // Accept a transaction id too, so a still-sending local echo (which has
        // no server event id yet) can be edited — the SDK's edit() supports
        // local echoes via TimelineItemHandle::Local.
        let item_id = Self::resolve_timeline_item_id(event_id, "edit")?;

        let edited = if as_media_caption {
            // Editing a media message's caption: keep the file and change only the caption. An
            // empty body clears the caption (None) rather than turning the message into empty text.
            let caption = if body.is_empty() {
                None
            } else {
                Some(body.to_string())
            };
            let formatted_caption = formatted_body.map(FormattedBody::html);
            EditedContent::MediaCaption {
                caption,
                formatted_caption,
                mentions,
            }
        } else {
            let mut new_content = match formatted_body {
                Some(html) => RoomMessageEventContentWithoutRelation::text_html(body, html),
                None => RoomMessageEventContentWithoutRelation::text_plain(body),
            };
            if let Some(mentions) = mentions {
                new_content = new_content.add_mentions(mentions);
            }
            EditedContent::RoomMessage(new_content)
        };

        timeline
            .edit(&item_id, edited)
            .await
            .map_err(|e| anyhow!("Edit failed: {e}"))?;

        Ok(event_id.to_string())
    }

    pub(crate) async fn delete_message(timeline: Arc<SdkTimeline>, event_id: &str) -> Result<()> {
        let item_id = Self::resolve_timeline_item_id(event_id, "delete")?;
        timeline
            .redact(&item_id, None)
            .await
            .map_err(|e| anyhow!("Redact failed: {e}"))?;
        Ok(())
    }

    pub(crate) async fn get_pinned_messages(
        room: Room,
        room_id: &str,
        client: Client,
        reply_preview_cache: Arc<RwLock<HashMap<(String, String), crate::types::ReplyPreview>>>,
        preview_cache: Arc<RwLock<HashMap<String, Option<UrlPreview>>>>,
    ) -> Result<Vec<TimelineItem>> {
        use matrix_sdk_ui::timeline::{RoomExt as _, TimelineFocus};

        let pinned_timeline = room
            .timeline_builder()
            .with_focus(TimelineFocus::PinnedEvents)
            .build()
            .await?;

        let pinned_timeline = Arc::new(pinned_timeline);
        let own_user_id = room.own_user_id().to_owned();
        let room_is_encrypted = room.encryption_state().is_encrypted();
        let pinned_event_ids: HashSet<String> = room
            .pinned_event_ids()
            .unwrap_or_default()
            .into_iter()
            .map(|id| id.to_string())
            .collect();
        let media_sources = Arc::new(std::sync::RwLock::new(HashMap::<String, MediaSource>::new()));

        let mut all_items: Vec<_> = pinned_timeline.items().await.into_iter().collect();

        // The pinned (focused) timeline back-paginates its events asynchronously, so
        // the first read right after build() is often empty/partial — which made the
        // pinned list appear empty on first open and only fill on reopen. Wait
        // (bounded) until all pinned events are present before returning.
        if !pinned_event_ids.is_empty() {
            let step = Duration::from_millis(100);
            let max_wait = Duration::from_secs(3);
            let mut waited = Duration::ZERO;
            loop {
                let found = all_items
                    .iter()
                    .filter(|item| {
                        item.as_event()
                            .and_then(|event| event.event_id())
                            .is_some_and(|id| pinned_event_ids.contains(id.as_str()))
                    })
                    .count();
                if found >= pinned_event_ids.len() || waited >= max_wait {
                    break;
                }
                tokio::time::sleep(step).await;
                waited += step;
                all_items = pinned_timeline.items().await.into_iter().collect();
            }
        }

        if TimelineConversionService::fetch_reply_details(&pinned_timeline, &all_items).await {
            all_items = pinned_timeline.items().await.into_iter().collect();
        }
        let cached_reply_previews: HashMap<String, crate::types::ReplyPreview> = {
            let guard = reply_preview_cache.read().await;
            guard
                .iter()
                .filter(|&((cached_room_id, _event_id), _preview)| cached_room_id == room_id)
                .map(|((cached_room_id, event_id), preview)| {
                    let _ = cached_room_id;
                    (event_id.clone(), preview.clone())
                })
                .collect()
        };

        let mut items: Vec<TimelineItem> = all_items
            .iter()
            .filter_map(|item| {
                TimelineConversionService::convert_timeline_item(
                    item.as_ref(),
                    &pinned_event_ids,
                    &media_sources,
                    &own_user_id,
                    room_is_encrypted,
                    cached_reply_previews.get(
                        &item
                            .as_event()
                            .and_then(|event| {
                                event
                                    .event_id()
                                    .map(|id| id.to_string())
                                    .or_else(|| event.transaction_id().map(|txid| txid.to_string()))
                            })
                            .unwrap_or_default(),
                    ),
                )
            })
            .collect();

        {
            let pc = preview_cache.read().await;
            for item in &mut items {
                if item.url_preview.is_some() {
                    continue;
                }
                let body = match &item.content {
                    MessageContent::Text { body, .. } => body.as_str(),
                    _ => continue,
                };
                if let Some(url) = TimelineCacheService::extract_url(body) {
                    if let Some(Some(preview)) = pc.get(&url) {
                        item.url_preview = Some(preview.clone());
                    }
                }
            }
        }

        for item in &mut items {
            if item.url_preview.is_some() {
                continue;
            }
            let body = match &item.content {
                MessageContent::Text { body, .. } => body.as_str(),
                _ => continue,
            };
            if let Some(url) = TimelineCacheService::extract_url(body) {
                if let Some(preview) = TimelineUpdateService::fetch_url_preview(&client, &url).await
                {
                    // Cache it so a re-open (and the main timeline) reuses the
                    // result instead of refetching on every pinned-section open.
                    preview_cache
                        .write()
                        .await
                        .insert(url.clone(), Some(preview.clone()));
                    item.url_preview = Some(preview);
                }
            }
        }

        // The pinned (focused) timeline only loads Annotation + Replacement
        // relations, so poll *responses* (Reference relations) and the poll end
        // are missing — every pinned poll renders as if no one voted. Re-tally
        // each poll from its own relations so the pinned section matches the
        // main timeline (works for old polls outside the loaded chat window).
        Self::fill_pinned_poll_results(&room, &mut items).await;

        Ok(items)
    }

    /// Recompute the vote tally for every poll in `items` by fetching the
    /// poll's `m.reference` relations (responses + end) directly, then mapping
    /// them onto the item's `PollInfo`. See the call site for why this is
    /// needed. Failures are non-fatal: the poll is left as-is (unvoted).
    async fn fill_pinned_poll_results(room: &Room, items: &mut [TimelineItem]) {
        use matrix_sdk::config::RequestConfig;
        use matrix_sdk::ruma::events::relation::RelationType;
        use matrix_sdk::ruma::events::{AnySyncMessageLikeEvent, AnySyncTimelineEvent};

        let own_user = room.own_user_id().as_str().to_string();

        for item in items.iter_mut() {
            let MessageContent::Poll { info } = &mut item.content else {
                continue;
            };
            let Ok(event_id) = OwnedEventId::try_from(item.event_id.as_str()) else {
                continue;
            };

            // Reference relations carry the poll responses (votes) and the end.
            let relations = match room
                .load_or_fetch_event_with_relations(
                    &event_id,
                    Some(vec![RelationType::Reference]),
                    Some(RequestConfig::default().retry_limit(2)),
                )
                .await
            {
                Ok((_target, relations)) => relations,
                Err(err) => {
                    warn!(
                        "pinned poll {}: failed to load responses: {err}",
                        item.event_id
                    );
                    continue;
                }
            };

            let mut responses: Vec<(String, u64, Vec<String>)> = Vec::new();
            let mut end_ts: Option<u64> = None;
            for relation in &relations {
                let Ok(mut parsed) = relation.raw().deserialize() else {
                    continue;
                };
                // Cached relations are already decrypted; relations fetched from
                // the server in an encrypted room arrive as m.room.encrypted, so
                // decrypt those before we can read the poll response.
                if matches!(
                    parsed,
                    AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::RoomEncrypted(_))
                ) {
                    // Safe: the matches! above confirmed this is an encrypted
                    // event, so the JSON is a valid m.room.encrypted event.
                    match room
                        .decrypt_event(relation.raw().cast_ref_unchecked(), None)
                        .await
                    {
                        Ok(decrypted) => match decrypted.raw().deserialize() {
                            Ok(ev) => parsed = ev,
                            Err(_) => continue,
                        },
                        Err(_) => continue,
                    }
                }

                match parsed {
                    AnySyncTimelineEvent::MessageLike(
                        AnySyncMessageLikeEvent::UnstablePollResponse(ev),
                    ) => {
                        if let Some(original) = ev.as_original() {
                            responses.push((
                                original.sender.to_string(),
                                u64::from(original.origin_server_ts.0),
                                original.content.poll_response.answers.clone(),
                            ));
                        }
                    }
                    AnySyncTimelineEvent::MessageLike(
                        AnySyncMessageLikeEvent::UnstablePollEnd(ev),
                    ) => {
                        if let Some(original) = ev.as_original() {
                            let ts = u64::from(original.origin_server_ts.0);
                            end_ts = Some(end_ts.map_or(ts, |cur| cur.min(ts)));
                        }
                    }
                    _ => {}
                }
            }

            let answer_ids: Vec<String> = info
                .options
                .iter()
                .map(|option| option.id.clone())
                .collect();
            let selections = resolve_poll_selections(
                &answer_ids,
                info.max_selections as usize,
                &responses,
                end_ts,
            );

            let own_choice = selections.get(own_user.as_str());
            for option in &mut info.options {
                option.vote_count = selections
                    .values()
                    .filter(|chosen| chosen.iter().any(|id| id == &option.id))
                    .count() as u32;
                option.is_chosen =
                    own_choice.is_some_and(|chosen| chosen.iter().any(|id| id == &option.id));
            }
            info.total_voters = selections
                .values()
                .filter(|chosen| !chosen.is_empty())
                .count() as u32;
            info.has_voted = own_choice.is_some_and(|chosen| !chosen.is_empty());
            if end_ts.is_some() {
                info.is_closed = true;
            }
        }
    }

    pub(crate) async fn pin_message(room: Room, event_id: &str, pinned: bool) -> Result<()> {
        // No local permission pre-check: it evaluates against cached
        // power-levels state, which is often absent right after a cold room
        // open — ruma then denies with defaults and the very first pin in a
        // room silently no-ops (optimistic UI reverts). The server is the
        // authority; a genuine 403 flows into the same failure path with a
        // correct toast.
        let eid: OwnedEventId = event_id
            .try_into()
            .map_err(|_| anyhow!("Invalid event ID for pin"))?;

        let mut pinned_ids = room.pinned_event_ids().unwrap_or_default();

        if pinned {
            if !pinned_ids.contains(&eid) {
                pinned_ids.push(eid);
            }
        } else {
            pinned_ids.retain(|id| id != &eid);
        }

        use matrix_sdk::ruma::events::room::pinned_events::RoomPinnedEventsEventContent;
        let content = RoomPinnedEventsEventContent::new(pinned_ids);
        room.send_state_event(content)
            .await
            .map_err(|e| anyhow!("Failed to send pin state event: {e}"))?;
        Ok(())
    }

    pub(crate) async fn unpin_all_messages(room: Room) -> Result<()> {
        use matrix_sdk::ruma::events::room::pinned_events::RoomPinnedEventsEventContent;
        let content = RoomPinnedEventsEventContent::new(Vec::new());
        room.send_state_event(content)
            .await
            .map_err(|e| anyhow!("Failed to unpin all: {e}"))?;
        Ok(())
    }

    pub(crate) async fn prepare_forward_message(
        src_room_id: &str,
        event_id: &str,
        cache: Arc<RwLock<HashMap<String, Vec<TimelineItem>>>>,
    ) -> Result<ForwardMessagePayload> {
        let cache = cache.read().await;
        let items = cache
            .get(src_room_id)
            .ok_or_else(|| anyhow!("Source room not in cache"))?;
        let item = items
            .iter()
            .find(|i| i.event_id == event_id)
            .ok_or_else(|| anyhow!("Event not found in cache"))?;

        let content = item.content.clone();
        let source_sender_name = item.sender.display_name.clone();
        let source_sender_id = item.sender.user_id.clone();
        let source_sender_avatar = item.sender.avatar_url.clone().unwrap_or_default();
        drop(cache);

        Ok(ForwardMessagePayload {
            content,
            source_sender_name,
            source_sender_id,
            source_sender_avatar,
        })
    }

    pub(crate) async fn send_forwarded_message(
        dst_timeline: Arc<SdkTimeline>,
        payload: ForwardMessagePayload,
        media_sources: &Arc<std::sync::RwLock<HashMap<String, MediaSource>>>,
        pending_forward_meta: &crate::timeline_cache_service::PendingForwardMeta,
    ) -> Result<String> {
        let meta = crate::types::ForwardedFrom {
            sender_display_name: payload.source_sender_name.clone(),
            sender_id: payload.source_sender_id.clone(),
            avatar_url: payload.source_sender_avatar.clone(),
        };
        let msg = Self::forward_content_to_message(payload.content, media_sources);
        let mut value = serde_json::to_value(&msg)
            .map_err(|e| anyhow!("Failed to serialize forward content: {e}"))?;
        if let serde_json::Value::Object(ref mut map) = value {
            map.insert(
                "com.telematrix.forwarded_from".to_string(),
                serde_json::json!({
                    "sender_name": payload.source_sender_name,
                    "sender_id": payload.source_sender_id,
                    "sender_avatar_url": payload.source_sender_avatar,
                }),
            );
        }

        let json_str = serde_json::to_string(&value)
            .map_err(|e| anyhow!("Failed to re-serialize forward content: {e}"))?;
        let raw = matrix_sdk::ruma::serde::Raw::from_json_string(json_str)
            .map_err(|e| anyhow!("Failed to create raw content: {e}"))?;
        // Direct send, not the send queue: in an E2EE room the metadata just
        // embedded is unreadable from this session's own echoes (the remote
        // echo keeps the encrypted envelope), so conversion needs a side
        // channel keyed by event id — which only the direct send's response
        // provides (the queue's SendHandle hides its transaction id). The
        // room's send path still encrypts for E2EE rooms.
        let response = dst_timeline
            .room()
            .send_raw("m.room.message", raw)
            .await
            .map_err(|e| anyhow!("Failed to send forwarded message: {e}"))?;

        let event_id = response.response.event_id.to_string();
        {
            let mut map = match pending_forward_meta.lock() {
                Ok(guard) => guard,
                Err(poisoned) => poisoned.into_inner(),
            };
            // Session-scoped scratch; forwards are rare, so a hard cap is
            // enough to keep a pathological session bounded.
            if map.len() > 256 {
                map.clear();
            }
            map.insert(event_id.clone(), meta);
        }

        Ok(event_id)
    }

    /// Upload an attachment DIRECTLY, bypassing the SDK send queue.
    ///
    /// The send queue is store-and-forward: it encrypts + persists the whole
    /// file into the local (encrypted) media store before the HTTP upload, a
    /// size-proportional "preparing" stall. Going direct skips that — the upload
    /// starts immediately and reports byte progress (forwarded by `txn_id` to the
    /// C++ optimistic echo). The task is tracked in `upload_tasks` so a cancel
    /// can abort it mid-flight. Trade-off: no offline queueing / auto-retry /
    /// cross-restart resilience. Returns the sent event id.
    async fn upload_attachment_direct(
        timeline: &SdkTimeline,
        path: &std::path::Path,
        mime: &mime::Mime,
        filename: String,
        config: AttachmentConfig,
        txn_id: &str,
        progress_callback: &crate::upload_progress::UploadProgressCallbackSlot,
    ) -> Result<String> {
        let data = tokio::fs::read(path)
            .await
            .map_err(|e| anyhow!("failed to read {} for upload: {e}", path.display()))?;

        let progress = eyeball::SharedObservable::new(matrix_sdk::TransmissionProgress::default());
        let mut subscriber = progress.subscribe();
        let txn = txn_id.to_string();
        let slot = progress_callback.clone();
        let forwarder = tokio::spawn(async move {
            while let Some(p) = subscriber.next().await {
                crate::upload_progress::report(&slot, &txn, p.current as u64, p.total as u64);
            }
        });

        let response = timeline
            .room()
            .send_attachment(filename, mime, data, config)
            .with_send_progress_observable(progress)
            .await;
        forwarder.abort();

        let response = response.map_err(|e| anyhow!("direct attachment upload failed: {e}"))?;
        Ok(response.event_id.to_string())
    }

    pub(crate) async fn send_media(
        timeline: Arc<SdkTimeline>,
        content: MessageContent,
        transaction_id: Option<String>,
        progress_callback: crate::upload_progress::UploadProgressCallbackSlot,
    ) -> Result<String> {
        let txn_id: OwnedTransactionId = transaction_id
            .filter(|value| !value.is_empty())
            .map(|value| value.as_str().into())
            .unwrap_or_else(TransactionId::new);

        match content {
            MessageContent::Image {
                url,
                mime_type,
                filename,
                caption,
                size,
                width,
                height,
                ..
            } => {
                let mime: mime::Mime = mime_type.parse().unwrap_or(mime::APPLICATION_OCTET_STREAM);
                let path = std::path::Path::new(&url);

                let mut config = AttachmentConfig::default();
                if let Some(cap) = caption {
                    config.caption = Some(
                        matrix_sdk::ruma::events::room::message::TextMessageEventContent::plain(
                            cap,
                        ),
                    );
                }
                let mut info = BaseImageInfo::default();
                if width > 0 {
                    info.width = Some(width.into());
                }
                if height > 0 {
                    info.height = Some(height.into());
                }
                if size > 0 {
                    if let Ok(size_u32) = u32::try_from(size) {
                        info.size = Some(size_u32.into());
                    }
                }
                if info.width.is_some() || info.height.is_some() || info.size.is_some() {
                    config.info = Some(AttachmentInfo::Image(info));
                }
                config.txn_id = Some(txn_id.clone());

                Self::upload_attachment_direct(
                    &timeline,
                    path,
                    &mime,
                    filename,
                    config,
                    txn_id.as_str(),
                    &progress_callback,
                )
                .await
            }
            MessageContent::Video {
                url,
                mime_type,
                filename,
                caption,
                size,
                width,
                height,
                duration_ms,
                ..
            } => {
                let mime: mime::Mime = mime_type.parse().unwrap_or(mime::APPLICATION_OCTET_STREAM);
                let path = std::path::Path::new(&url);

                let mut config = AttachmentConfig::default();
                if let Some(cap) = caption {
                    config.caption = Some(
                        matrix_sdk::ruma::events::room::message::TextMessageEventContent::plain(
                            cap,
                        ),
                    );
                }
                let mut info = BaseVideoInfo::default();
                if width > 0 {
                    info.width = Some(width.into());
                }
                if height > 0 {
                    info.height = Some(height.into());
                }
                if size > 0 {
                    if let Ok(size_u32) = u32::try_from(size) {
                        info.size = Some(size_u32.into());
                    }
                }
                if duration_ms > 0 {
                    info.duration = Some(Duration::from_millis(duration_ms));
                }
                if info.width.is_some()
                    || info.height.is_some()
                    || info.size.is_some()
                    || info.duration.is_some()
                {
                    config.info = Some(AttachmentInfo::Video(info));
                }
                config.txn_id = Some(txn_id.clone());

                Self::upload_attachment_direct(
                    &timeline,
                    path,
                    &mime,
                    filename,
                    config,
                    txn_id.as_str(),
                    &progress_callback,
                )
                .await
            }
            MessageContent::File {
                url,
                mime_type,
                filename,
                caption,
                size,
                ..
            } => {
                let mime: mime::Mime = mime_type.parse().unwrap_or(mime::APPLICATION_OCTET_STREAM);
                let path = std::path::Path::new(&url);

                let mut config = AttachmentConfig::default();
                if let Some(cap) = caption {
                    config.caption = Some(
                        matrix_sdk::ruma::events::room::message::TextMessageEventContent::plain(
                            cap,
                        ),
                    );
                }
                if size > 0 {
                    let mut info = BaseFileInfo::default();
                    if let Ok(size_u32) = u32::try_from(size) {
                        info.size = Some(size_u32.into());
                    }
                    if info.size.is_some() {
                        config.info = Some(AttachmentInfo::File(info));
                    }
                }
                config.txn_id = Some(txn_id.clone());

                Self::upload_attachment_direct(
                    &timeline,
                    path,
                    &mime,
                    filename,
                    config,
                    txn_id.as_str(),
                    &progress_callback,
                )
                .await
            }
            MessageContent::Audio { info } => {
                let mime: mime::Mime = info
                    .mime_type
                    .parse()
                    .unwrap_or(mime::APPLICATION_OCTET_STREAM);
                let path = std::path::Path::new(&info.url);

                let mut config = matrix_sdk_ui::timeline::AttachmentConfig::default();
                let mut audio = BaseAudioInfo::default();
                if info.size > 0 {
                    if let Ok(size_u32) = u32::try_from(info.size) {
                        audio.size = Some(size_u32.into());
                    }
                }
                if info.duration_ms > 0 {
                    audio.duration = Some(Duration::from_millis(info.duration_ms));
                }
                if !info.waveform.is_empty() {
                    audio.waveform = Some(
                        info.waveform
                            .iter()
                            .map(|sample| (f32::from((*sample).min(31))) / 31.0)
                            .collect(),
                    );
                }
                let has_audio_info =
                    audio.size.is_some() || audio.duration.is_some() || audio.waveform.is_some();
                config.info = if info.is_voice {
                    Some(AttachmentInfo::Voice(audio))
                } else if has_audio_info {
                    Some(AttachmentInfo::Audio(audio))
                } else {
                    None
                };
                config.txn_id = Some(txn_id.clone());

                timeline
                    .send_attachment(path, mime, config)
                    .use_send_queue()
                    .await?;
                Ok(txn_id.to_string())
            }
            MessageContent::Text { body, .. } => {
                Self::send_message(timeline, &body, None, None).await
            }
            MessageContent::Poll { .. } => Err(anyhow!("Invalid media content type: poll")),
            MessageContent::Service { .. } => Err(anyhow!("Invalid media content type: service")),
            MessageContent::UnableToDecrypt { .. } => {
                Err(anyhow!("Invalid media content type: unable_to_decrypt"))
            }
        }
    }

    /// Cancel an upload in ANY state. `id` is the bubble's id (the transaction
    /// id while sending, or the event id once sent). First try to abort the
    /// in-flight/queued local echo (clean cancel before it's sent); if it's
    /// already sent/promoted (or not a local echo), redact it instead, so cancel
    /// always removes the message.
    pub(crate) async fn cancel_upload(timeline: Arc<SdkTimeline>, id: &str) -> Result<()> {
        // A cancel aborts the spawned upload task, so nothing after its `.await`
        // runs — drop the pending cache seed here instead.
        crate::upload_seed_store::remove(id);

        // Direct uploads (the default path) have no local echo — abort the
        // in-flight task instead, which drops the HTTP request / file read.
        if crate::upload_tasks::abort(id) {
            info!("aborted in-flight upload: {id}");
            return Ok(());
        }

        // Otherwise: a send-queue echo (e.g. voice) still in flight, or an
        // already-sent event to redact.
        let txn_id: OwnedTransactionId = id.into();
        let items = timeline.items().await;
        for item in items.iter() {
            let Some(event) = item.as_event() else {
                continue;
            };
            let matches = event.transaction_id() == Some(&txn_id)
                || event.event_id().map(|e| e.as_str()) == Some(id);
            if !matches {
                continue;
            }
            // Abort the in-flight/queued local echo if it can still be cancelled.
            if let Some(handle) = event.local_echo_send_handle() {
                match handle.abort().await {
                    Ok(true) => {
                        info!("aborted in-flight upload: {id}");
                        return Ok(());
                    }
                    Ok(false) => {} // already sent — redact below
                    Err(e) => warn!("upload abort failed ({id}): {e} — redacting instead"),
                }
            }
            // Already sent (or not abortable): redact by the REAL event id so an
            // already-uploaded media event is removed too.
            if let Some(event_id) = event.event_id() {
                timeline
                    .redact(&TimelineEventItemId::EventId(event_id.to_owned()), None)
                    .await
                    .map_err(|e| anyhow!("cancel upload redact failed: {e}"))?;
                info!("redacted already-sent upload: {id}");
            }
            return Ok(());
        }

        // No echo exists yet — the SDK reads the whole file into memory before
        // creating its local echo, so a cancel during that read finds nothing.
        // Not an error: the UI re-issues the cancel once the echo appears.
        info!("cancel upload deferred (no echo yet): {id}");
        Ok(())
    }

    pub(crate) async fn set_reaction(
        timeline: Arc<SdkTimeline>,
        room_id: &str,
        event_id: &str,
        key: &str,
        active: bool,
        cache: TimelineCache,
        pending_reaction_overrides: PendingReactionOverrides,
    ) -> Result<()> {
        let eid: OwnedEventId = event_id
            .try_into()
            .map_err(|_| anyhow!("Invalid event ID for reaction"))?;
        let pending_key = Self::pending_reaction_key(room_id, event_id, key);

        let current_state = {
            let pending = pending_reaction_overrides.read().await;
            if let Some(&state) = pending.get(&pending_key) {
                state
            } else {
                drop(pending);
                let cache = cache.read().await;
                cache.get(room_id).is_some_and(|items| {
                    items.iter().any(|item| {
                        item.event_id == event_id
                            && item.reactions.iter().any(|r| r.key == key && r.is_self)
                    })
                })
            }
        };

        if current_state == active {
            return Ok(());
        }

        {
            let mut pending = pending_reaction_overrides.write().await;
            pending.insert(pending_key.clone(), active);
        }

        let item_id = TimelineEventItemId::EventId(eid);
        if let Err(e) = timeline.toggle_reaction(&item_id, key).await {
            let mut pending = pending_reaction_overrides.write().await;
            if pending.get(&pending_key).copied() == Some(active) {
                pending.remove(&pending_key);
            }
            return Err(anyhow!(e));
        }
        Ok(())
    }

    pub(crate) async fn send_poll_vote(
        timeline: Arc<SdkTimeline>,
        poll_event_id: &str,
        option_ids: Vec<String>,
    ) -> Result<String> {
        use matrix_sdk::ruma::events::poll::unstable_response::UnstablePollResponseEventContent;

        let poll_event_id: OwnedEventId = poll_event_id
            .parse()
            .map_err(|e| anyhow!("Invalid poll event ID: {e}"))?;
        let content = UnstablePollResponseEventContent::new(option_ids, poll_event_id);
        timeline.send(content.into()).await?;
        Ok(String::new())
    }

    pub(crate) fn extract_mentions(body: &str, formatted_body: Option<&str>) -> Option<Mentions> {
        let mut mentions = Mentions::new();

        if let Some(html) = formatted_body {
            for user_id in Self::extract_user_mentions_from_html(html) {
                mentions.user_ids.insert(user_id);
            }
        }

        if Self::contains_room_mention(body) {
            mentions.room = true;
        }

        if mentions.room || !mentions.user_ids.is_empty() {
            Some(mentions)
        } else {
            None
        }
    }

    fn extract_user_mentions_from_html(html: &str) -> Vec<OwnedUserId> {
        let lower = html.to_ascii_lowercase();
        let mut offset = 0usize;
        let mut result = Vec::new();

        while let Some(rel_pos) = lower[offset..].find("href=") {
            offset += rel_pos + 5;

            let trim_advance = html[offset..].len() - html[offset..].trim_start().len();
            offset += trim_advance;
            let rest = &html[offset..];
            if rest.is_empty() {
                break;
            }

            let (href, advance) = if let Some(s) = rest.strip_prefix('"') {
                let end = s.find('"').unwrap_or(s.len());
                (&s[..end], end + 2)
            } else if let Some(s) = rest.strip_prefix('\'') {
                let end = s.find('\'').unwrap_or(s.len());
                (&s[..end], end + 2)
            } else {
                let end = rest
                    .find(|c: char| c.is_ascii_whitespace() || c == '>')
                    .unwrap_or(rest.len());
                (&rest[..end], end)
            };

            if let Some(user_id) = Self::extract_matrix_to_user_id(href) {
                result.push(user_id);
            }

            offset = offset.saturating_add(advance);
            if offset >= html.len() {
                break;
            }
        }

        result
    }

    fn extract_matrix_to_user_id(href: &str) -> Option<OwnedUserId> {
        let marker = "/#/";
        let hash_idx = href.find(marker)?;
        let matrix_id = href[(hash_idx + marker.len())..]
            .split(['?', '#'])
            .next()
            .unwrap_or_default()
            .trim();
        if !matrix_id.starts_with('@') {
            return None;
        }
        OwnedUserId::try_from(matrix_id.to_owned()).ok()
    }

    fn contains_room_mention(body: &str) -> bool {
        const TOKEN: &str = "@room";
        let mut offset = 0usize;
        while let Some(rel_pos) = body[offset..].find(TOKEN) {
            let token_pos = offset + rel_pos;
            let token_end = token_pos + TOKEN.len();

            let before_ok = body[..token_pos]
                .chars()
                .next_back()
                .map(Self::is_room_mention_boundary)
                .unwrap_or(true);
            let after_ok = body[token_end..]
                .chars()
                .next()
                .map(Self::is_room_mention_boundary)
                .unwrap_or(true);

            if before_ok && after_ok {
                return true;
            }

            offset = token_end;
        }

        false
    }

    fn is_room_mention_boundary(ch: char) -> bool {
        !ch.is_alphanumeric() && ch != '_'
    }

    pub(crate) fn resolve_timeline_item_id(
        item_id: &str,
        action: &str,
    ) -> Result<TimelineEventItemId> {
        if item_id.is_empty() {
            return Err(anyhow!("Missing event or transaction ID for {action}"));
        }
        if let Ok(event_id) = item_id.parse::<OwnedEventId>() {
            return Ok(TimelineEventItemId::EventId(event_id));
        }
        let transaction_id: OwnedTransactionId = item_id.into();
        Ok(TimelineEventItemId::TransactionId(transaction_id))
    }

    fn pending_reaction_key(room_id: &str, event_id: &str, key: &str) -> (String, String, String) {
        (room_id.to_string(), event_id.to_string(), key.to_string())
    }

    /// Resolve the original media source for a forwarded media URL. Forwarded
    /// content is rebuilt from the cached `MessageContent`, which only keeps the
    /// mxc string — so look up the original `MediaSource` (which carries the
    /// E2EE file/key/iv for encrypted rooms) and reuse it; otherwise a forwarded
    /// image/video/file from an encrypted room would be sent as Plain and render
    /// blank for everyone (no decryption keys). Falls back to Plain when unknown.
    fn forward_source(
        url: &str,
        media_sources: &Arc<std::sync::RwLock<HashMap<String, MediaSource>>>,
    ) -> MediaSource {
        media_sources
            .read()
            .ok()
            .and_then(|m| m.get(url).cloned())
            .unwrap_or_else(|| MediaSource::Plain(url.to_owned().into()))
    }

    fn forward_content_to_message(
        content: MessageContent,
        media_sources: &Arc<std::sync::RwLock<HashMap<String, MediaSource>>>,
    ) -> RoomMessageEventContent {
        match content {
            MessageContent::Text {
                body,
                formatted_body,
            } => match formatted_body {
                Some(html) => RoomMessageEventContent::text_html(&body, &html),
                None => RoomMessageEventContent::text_plain(&body),
            },
            MessageContent::Image {
                url,
                mime_type: _,
                filename,
                caption,
                thumbnail_url,
                blurhash: _,
                size,
                width,
                height,
            } => {
                let source = Self::forward_source(&url, media_sources);
                let thumb_source = thumbnail_url
                    .as_ref()
                    .map(|t| Self::forward_source(t, media_sources));
                let mxc: matrix_sdk::ruma::OwnedMxcUri = url.into();
                let body = caption.unwrap_or(filename);
                let mut img = ImageMessageEventContent::plain(body, mxc);
                // Keep the original (possibly encrypted) source so a forwarded
                // image stays decryptable instead of rendering blank.
                img.source = source;
                let mut info = matrix_sdk::ruma::events::room::ImageInfo::new();
                if width > 0 {
                    info.width = Some(width.into());
                }
                if height > 0 {
                    info.height = Some(height.into());
                }
                if size > 0 {
                    if let Ok(size_u32) = u32::try_from(size) {
                        info.size = Some(size_u32.into());
                    }
                }
                info.thumbnail_source = thumb_source;
                img.info = Some(Box::new(info));
                RoomMessageEventContent::new(MessageType::Image(img))
            }
            MessageContent::File {
                url,
                mime_type,
                filename,
                caption,
                size,
                ..
            } => {
                let source = Self::forward_source(&url, media_sources);
                let mxc: matrix_sdk::ruma::OwnedMxcUri = url.into();
                let body = caption.unwrap_or(filename.clone());
                let mut file = FileMessageEventContent::plain(body, mxc);
                file.source = source;
                file.filename = Some(filename);
                let mut info = matrix_sdk::ruma::events::room::message::FileInfo::new();
                if size > 0 {
                    if let Ok(size_u32) = u32::try_from(size) {
                        info.size = Some(size_u32.into());
                    }
                }
                if let Ok(m) = mime_type.parse::<mime::Mime>() {
                    info.mimetype = Some(m.to_string());
                }
                file.info = Some(Box::new(info));
                RoomMessageEventContent::new(MessageType::File(file))
            }
            MessageContent::Video {
                url,
                mime_type: _,
                filename,
                caption,
                thumbnail_url,
                blurhash: _,
                size,
                width,
                height,
                duration_ms,
            } => {
                let source = Self::forward_source(&url, media_sources);
                let thumb_source = thumbnail_url
                    .as_ref()
                    .map(|t| Self::forward_source(t, media_sources));
                let mxc: matrix_sdk::ruma::OwnedMxcUri = url.into();
                let body = caption.unwrap_or(filename);
                let mut vid = VideoMessageEventContent::plain(body, mxc);
                vid.source = source;
                let mut info = matrix_sdk::ruma::events::room::message::VideoInfo::new();
                if width > 0 {
                    info.width = Some(width.into());
                }
                if height > 0 {
                    info.height = Some(height.into());
                }
                if size > 0 {
                    if let Ok(size_u32) = u32::try_from(size) {
                        info.size = Some(size_u32.into());
                    }
                }
                if duration_ms > 0 {
                    info.duration = Some(Duration::from_millis(duration_ms));
                }
                info.thumbnail_source = thumb_source;
                vid.info = Some(Box::new(info));
                RoomMessageEventContent::new(MessageType::Video(vid))
            }
            MessageContent::Audio { info } => {
                let source = Self::forward_source(&info.url, media_sources);
                let mxc: matrix_sdk::ruma::OwnedMxcUri = info.url.into();
                let mut audio = AudioMessageEventContent::plain(info.filename.clone(), mxc);
                audio.source = source;
                audio.filename = Some(info.filename.clone());
                let mut media_info = matrix_sdk::ruma::events::room::message::AudioInfo::new();
                if info.size > 0 {
                    if let Ok(size_u32) = u32::try_from(info.size) {
                        media_info.size = Some(size_u32.into());
                    }
                }
                if info.duration_ms > 0 {
                    media_info.duration = Some(Duration::from_millis(info.duration_ms));
                }
                if let Ok(m) = info.mime_type.parse::<mime::Mime>() {
                    media_info.mimetype = Some(m.to_string());
                }
                audio.info = Some(Box::new(media_info));
                if info.is_voice {
                    audio.voice = Some(
                        matrix_sdk::ruma::events::room::message::UnstableVoiceContentBlock::new(),
                    );
                }
                if !info.waveform.is_empty() || info.duration_ms > 0 {
                    let waveform = info
                        .waveform
                        .iter()
                        .map(|sample| {
                            let expanded = ((u16::from(*sample) * 1024) + 15) / 31;
                            matrix_sdk::ruma::events::room::message::UnstableAmplitude::from(
                                expanded,
                            )
                        })
                        .collect();
                    audio.audio = Some(
                        matrix_sdk::ruma::events::room::message::UnstableAudioDetailsContentBlock::new(
                            Duration::from_millis(info.duration_ms),
                            waveform,
                        ),
                    );
                }
                RoomMessageEventContent::new(MessageType::Audio(audio))
            }
            MessageContent::Service { body } => RoomMessageEventContent::text_plain(&body),
            MessageContent::Poll { info } => {
                RoomMessageEventContent::text_plain(format!("Poll: {}", info.question))
            }
            MessageContent::UnableToDecrypt { body, .. } => {
                RoomMessageEventContent::text_plain(&body)
            }
        }
    }
}

/// Resolve each voter's effective answer-id selections for a poll, following the
/// MSC3381 tally rules (mirrors ruma's `compile_unstable_poll_results`):
///
/// * only each user's latest response counts (ties keep the earliest seen);
/// * a response selecting any unknown answer id is spoiled → no selection;
/// * a valid response is truncated to `max_selections`;
/// * an empty (valid) response retracts the vote;
/// * when `end_ts` is set, responses sent after it are ignored.
///
/// Returns voter → chosen answer ids; an empty vec means the user responded but
/// is not counted as a voter (spoiled or retracted).
fn resolve_poll_selections(
    answer_ids: &[String],
    max_selections: usize,
    responses: &[(String, u64, Vec<String>)],
    end_ts: Option<u64>,
) -> std::collections::BTreeMap<String, Vec<String>> {
    use std::collections::{BTreeMap, HashSet};

    let known: HashSet<&str> = answer_ids.iter().map(String::as_str).collect();
    let max_selections = max_selections.max(1);

    // Keep only each user's latest response (strictly-later wins; on a tie the
    // first one seen stays, matching ruma's fold).
    let mut latest: BTreeMap<&str, (u64, &[String])> = BTreeMap::new();
    for (user, ts, answers) in responses {
        if end_ts.is_some_and(|end| *ts > end) {
            continue;
        }
        match latest.get(user.as_str()) {
            Some((prev_ts, _)) if *prev_ts >= *ts => {}
            _ => {
                latest.insert(user.as_str(), (*ts, answers.as_slice()));
            }
        }
    }

    let mut result = BTreeMap::new();
    for (user, (_, answers)) in latest {
        let chosen = if answers.iter().any(|a| !known.contains(a.as_str())) {
            // An unknown answer invalidates the whole response.
            Vec::new()
        } else {
            answers.iter().take(max_selections).cloned().collect()
        };
        result.insert(user.to_string(), chosen);
    }
    result
}

#[cfg(test)]
mod tests {
    use super::resolve_poll_selections;

    fn ids(list: &[&str]) -> Vec<String> {
        list.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn single_vote_is_counted() {
        let responses = vec![("@u:x".to_string(), 10, ids(&["a"]))];
        let out = resolve_poll_selections(&ids(&["a", "b"]), 1, &responses, None);
        assert_eq!(out.get("@u:x"), Some(&ids(&["a"])));
    }

    #[test]
    fn latest_response_wins() {
        let responses = vec![
            ("@u:x".to_string(), 10, ids(&["a"])),
            ("@u:x".to_string(), 20, ids(&["b"])),
        ];
        let out = resolve_poll_selections(&ids(&["a", "b"]), 1, &responses, None);
        assert_eq!(out.get("@u:x"), Some(&ids(&["b"])));
    }

    #[test]
    fn tie_keeps_first_seen() {
        let responses = vec![
            ("@u:x".to_string(), 10, ids(&["a"])),
            ("@u:x".to_string(), 10, ids(&["b"])),
        ];
        let out = resolve_poll_selections(&ids(&["a", "b"]), 1, &responses, None);
        assert_eq!(out.get("@u:x"), Some(&ids(&["a"])));
    }

    #[test]
    fn unknown_answer_spoils_vote() {
        let responses = vec![("@u:x".to_string(), 10, ids(&["zzz"]))];
        let out = resolve_poll_selections(&ids(&["a", "b"]), 1, &responses, None);
        assert_eq!(out.get("@u:x"), Some(&Vec::<String>::new()));
    }

    #[test]
    fn empty_response_retracts_vote() {
        let responses = vec![
            ("@u:x".to_string(), 10, ids(&["a"])),
            ("@u:x".to_string(), 20, ids(&[])),
        ];
        let out = resolve_poll_selections(&ids(&["a", "b"]), 1, &responses, None);
        assert_eq!(out.get("@u:x"), Some(&Vec::<String>::new()));
    }

    #[test]
    fn selection_truncated_to_max() {
        let responses = vec![("@u:x".to_string(), 10, ids(&["a", "b", "c"]))];
        let out = resolve_poll_selections(&ids(&["a", "b", "c"]), 2, &responses, None);
        assert_eq!(out.get("@u:x"), Some(&ids(&["a", "b"])));
    }

    #[test]
    fn responses_after_end_ts_ignored() {
        let responses = vec![
            ("@u:x".to_string(), 10, ids(&["a"])),
            ("@u:x".to_string(), 30, ids(&["b"])),
        ];
        let out = resolve_poll_selections(&ids(&["a", "b"]), 1, &responses, Some(20));
        assert_eq!(out.get("@u:x"), Some(&ids(&["a"])));
    }

    #[test]
    fn multiple_voters_tallied() {
        let responses = vec![
            ("@u1:x".to_string(), 10, ids(&["a"])),
            ("@u2:x".to_string(), 11, ids(&["a"])),
            ("@u3:x".to_string(), 12, ids(&["b"])),
        ];
        let out = resolve_poll_selections(&ids(&["a", "b"]), 1, &responses, None);
        let count_a = out
            .values()
            .filter(|c| c.iter().any(|id| id == "a"))
            .count();
        let count_b = out
            .values()
            .filter(|c| c.iter().any(|id| id == "b"))
            .count();
        assert_eq!(count_a, 2);
        assert_eq!(count_b, 1);
        assert_eq!(out.values().filter(|c| !c.is_empty()).count(), 3);
    }
}

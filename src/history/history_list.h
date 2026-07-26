// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QHash>
#include <QImage>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVariantAnimation>
#include <QVector>

#include <functional>
#include <optional>

#include "ui/rp_widget.h"
#include "../protocol/protocol_types.h"
#include "history_audio_playback_state.h"
#include "history_inline_video.h"
#include "history_selection_state.h"
#include "message_index.h"
#include "theme/chat_background_style.h"
#include "history_message.h" // MessagePaintContext + TimelineItemLookup

class QMouseEvent;
class QKeyEvent;
class QContextMenuEvent;
class QWheelEvent;
class QPainter;
class QMediaPlayer;
class QAudioOutput;
class QBuffer;

namespace TeleMatrix {

struct MessagePaintContext;
class HistoryVideoThumbnailProbeState;
class TimestampTooltip;
namespace HistoryPopupMenuStyle { class PopupMenu; }

/// Scrollable message list widget.
/// Paints message bubbles with date separators.
/// Bottom-anchored: newest messages at the bottom.
class HistoryList : public Ui::RpWidget {
    Q_OBJECT

public:
    struct LayoutItem {
        int y = 0;      // top of the full row, including date/unread separators
        int height = 0;  // full row height, including separators and message margins
        int messageY = 0; // top of the message area, after separators
        int messageHeight = 0; // message height including margins, excluding separators
        int cachedBubbleHeight = -1; // cached result of bubbleHeight()
        int cachedWidth = -1;        // width at which cachedBubbleHeight was computed
        bool cachedSameSenderAbove = false; // grouping state used for cachedBubbleHeight
        bool showDate = false;    // show date separator above
        bool showUnreadBar = false; // show unread bar above this message
        bool sameSenderAbove = false; // collapse sender info
        bool sameSenderBelow = false; // next message is same sender
        int senderGroupFirstIndex = -1;
        int senderGroupLastIndex = -1;
        int senderGroupTopY = 0;
        int senderGroupBottomY = 0;
    };

    explicit HistoryList(QWidget *parent = nullptr);
    ~HistoryList() override;

    [[nodiscard]] HistoryInlineVideoPlayer *inlineVideoPlayer() const {
        return _inlineVideo;
    }

    /// Set the messages to display.
    void setMessages(const QVector<TimelineItem> &messages);

    /// Append a message (local echo or live update).
    void appendMessage(const TimelineItem &message);

    /// Append multiple messages without replacing existing ones.
    /// Does not affect scroll position. Used for incremental timeline updates.
    void appendMessages(const QVector<TimelineItem> &messages);

    /// Optimistically update pin state for a message by event id.
    void setMessagePinState(const QString &eventId, bool pinned);

    /// Update send state for an existing message by event id.
    /// If `newEventId` is non-empty, message id is replaced (local echo -> server id).
    bool updateMessageSendState(
        const QString &eventId,
        SendState sendState,
        const QString &newEventId = QString());

    /// Update upload progress (0..1, or <0 for "preparing") for a message by id
    /// and repaint its bubble. Used by direct uploads, whose progress arrives via
    /// the bridge callback rather than on a timeline item.
    bool updateMessageUploadProgress(const QString &eventId, double progress);

    /// Remove a local-only message from the list without asking the backend.
    bool removeMessage(const QString &eventId);

    /// Set the current room id (used for message-link generation).
    void setRoomId(const QString &roomId);

    /// Set whether the current user can pin messages in this room.
    void setCanPinMessages(bool canPin) { _canPinMessages = canPin; }
    [[nodiscard]] bool canPinMessages() const { return _canPinMessages; }

    /// "Pinned messages" mode: same widget, reused as the pinned-section view.
    /// In this mode the list omits the "Reply" context-menu actions, draws a
    /// per-row "go to message" jump button on the right edge (reserving gutter
    /// width for it), does not paginate or mark messages read, and always shows
    /// the Unpin affordance. All other interactions behave like the timeline.
    void setPinnedMode(bool v) {
        if (_pinnedMode == v) {
            return;
        }
        _pinnedMode = v;
        // Width reservation for the jump button changes, so invalidate layout.
        _lastLayoutWidth = -1;
        update();
    }
    [[nodiscard]] bool pinnedMode() const { return _pinnedMode; }

    /// Preview (unjoined room): messages are shown read-only. Like pinned mode it omits
    /// Reply/Edit/Delete/React and the hover pills, but it is NOT pinned — no jump button,
    /// no Pin/Forward (there is no room membership to act with). Only copy actions remain.
    void setReadOnly(bool v) {
        if (_readOnly == v) {
            return;
        }
        _readOnly = v;
        update();
    }
    [[nodiscard]] bool readOnly() const { return _readOnly; }
    /// True when interactive affordances (reactions, reply/edit/delete) must be suppressed —
    /// in the pinned section or an unjoined-room preview.
    [[nodiscard]] bool readOnlyView() const { return _pinnedMode || _readOnly; }

    /// Show avatars for outgoing messages in direct chats.
    void setShowOutgoingPrivateAvatars(bool show);
    /// Saved Messages timeline: no start-of-conversation pill, no message links.
    void setSavedMessagesMode(bool saved);

    /// Whether the view is scrolled to the bottom.
    bool isAtBottom() const;

    /// Scroll to the bottom of the message list.
    void scrollToBottom();

    /// Find the event ID of the message closest to the given timestamp.
    /// Returns empty string if no messages loaded.
    QString eventIdNearDate(qint64 timestamp) const;
    /// Resolve a timeline event id to its y-position in this list.
    int yForEventId(const QString &eventId) const;
    /// Resolve a timeline event id to its row height.
    int rowHeightForEventId(const QString &eventId) const;
    /// Highlight a message row with a fade-out overlay.
    void highlightMessage(const QString &eventId);
    /// Update top visible offset from scroll area (triggers the scroll-date).
    void updateVisibleTop(int visibleTop);
    /// Enter/exit message-level selection mode.
    void enterSelectionMode(int messageIndex);
    void exitSelectionMode();
    /// Emit selection actions for currently selected messages.
    void requestForwardSelected();
    bool inSelectionMode() const { return _selection.inSelectionMode(); }

    /// Show a room we have not joined: its name and description centred over an empty timeline.
    /// Matrix serves no history to a non-member, so there is nothing else to paint. An empty `name`
    /// clears it.
    void setPreviewInfo(const QString &name, const QString &topic);

    /// Show/hide "Waiting for network..." pill at top of timeline.
    void setSyncing(bool syncing);

    /// Show/hide "Syncing..." pill when room timeline is loading.
    void setLoadingTimeline(bool loading);

    /// Opaque empty-timeline cover with a centered "Loading…" shown while a jump
    /// fetches its focused context; messages populate underneath but stay hidden
    /// until the reveal. Distinct from setLoadingTimeline (the legacy in-place
    /// preloader), which jumps no longer use.
    void setJumpLoadingCover(bool on);

    /// Reset probed-event tracking so video/audio thumbnails can be
    /// re-extracted after cache clear.
    void clearProbedState();

    /// Set the unread bar position. Empty eventId removes the bar.
    void setUnreadBar(const QString &firstUnreadEventId, int unreadCount);
    /// Clear the unread bar.
    void clearUnreadBar();
    /// Advancing first-unread used ONLY by read detection (receipts / optimistic
    /// count). Independent of _firstUnreadEventId, which is the FROZEN visual bar.
    /// Fed by HistoryWidget::pushReadFrontier; empty resets the detector.
    void setReadFrontier(const QString &eventId);
    /// Get the first unread event ID (for preserving across reloads).
    [[nodiscard]] QString firstUnreadEventId() const { return _firstUnreadEventId; }
    /// Get the current unread bar count (for preserving across reloads).
    [[nodiscard]] int unreadBarCount() const { return _unreadBarCount; }

    /// Current timeline messages in visual order.
    const QVector<TimelineItem> &messages() const { return _messages; }

    /// Mark/clear a message as "deleting": its bubble is dimmed and goes inert
    /// (playback and downloads stopped, every interaction swallowed) until the
    /// redaction lands (delivery.deleted) or the delete fails and it's cleared.
    void markDeleting(const QString &eventId);
    void clearDeleting(const QString &eventId);

    /// Glow a message's URL while its link-preview (OG card) is being fetched.
    void setPreviewFetching(const QString &eventId, bool fetching);

    /// Set whether paint-driven read marking is active.
    /// Only true when window is active and room is loaded.
    void setMarkingMessagesRead(bool marking);
    /// Result of an optimistic reaction toggle.
    struct ReactionResult {
        bool changed = false;
        int heightDelta = 0; // Change in message height (e.g. +32 when first reaction added).
        int messageY = 0;    // Widget Y position of the affected message before layout change.
    };

    /// Optimistically apply a reaction toggle to a visible message.
    ReactionResult applyReactionLocally(
        const QString &eventId,
        const QString &key,
        bool active);

    /// Invalidate cached layout for messages referencing an mxc:// URL.
    /// Call when media is resolved so bubble heights can be recalculated.
    void invalidateLayoutForMedia(const QString &mxcUrl);
    void invalidateLayoutForMedia(const QSet<QString> &mediaUrls);

    /// "Display background doodles": tile the doodle over the wallpaper gradient.
    void setBackgroundDoodlesEnabled(bool enabled);

    /// Anchor the wallpaper to `anchor` (the chat column) rather than to the
    /// scroll viewport, so chrome must be an ancestor of this list's viewport.
    void setBackgroundAnchor(QWidget *anchor);

    /// Invalidate cached gradient background (call on theme change).
    /// Rebuild the wallpaper from the current theme's corner colours + doodle.
    void invalidateBackground();

    /// Apply reaction-only updates from incoming messages.
    /// Returns the scroll delta for messages above scrollTop.
    int applyReactionUpdates(
        const QVector<TimelineItem> &incoming,
        int scrollTop);

    /// Set whether large emoji rendering is enabled (from settings).
    void setLargeEmojiEnabled(bool enabled);

    /// Set whether the hover reply / reaction buttons are shown (from settings).
    void setReplyButtonEnabled(bool enabled);
    void setReactionButtonEnabled(bool enabled);

    /// Set recent reaction emoji for the hover reaction column. The 7 quick
    /// reactions are prepended automatically; recents fill the rest.
    void setReactionRecentEmojis(const QVector<QString> &emojis);

    /// Whether the current session/device is verified.
    void setSessionVerified(bool verified);
    [[nodiscard]] bool isSessionVerified() const { return _sessionVerified; }

    /// Replace the current slice with a new one, using diffing to avoid
    /// full-replace when only a prepend/append/edit occurred.
    void setSlice(const TimelineSlice& slice);
    /// Whether any live read frontier is currently armed.
    [[nodiscard]] QString readFrontierEventId() const { return _readFrontierEventId; }

    /// Origin timestamp (epoch seconds) of a loaded event, 0 when not loaded.
    [[nodiscard]] qint64 eventTimestamp(const QString &eventId) const;

    /// Re-arm read detection after a programmatic viewport move (call on genuine
    /// user scroll input or explicit navigation to the live tail).
    void resetReadDetectionHold() { _readDetectionHold = false; }

    /// True while read detection is suppressed after a programmatic viewport move
    /// (a gappy-sync reset teleport). The read-consuming gate keys on this so a
    /// forced scroll-to-bottom is not mistaken for the user parking at the tail.
    [[nodiscard]] bool readDetectionHeld() const { return _readDetectionHold; }

    /// Prepend messages to the top with scroll anchoring.
    void prependMessages(const QVector<TimelineItem>& items);

    /// Recompute layout at the current geometry and repaint. Useful when the
    /// list was populated before it had its final size (e.g. the pinned section
    /// lays out via setMessages() before updateControlsGeometry() sizes it).
    void relayout();

    /// Save the current scroll anchor (first visible message + pixel offset).
    /// Remember a message to re-pin after a relayout. `avoidRows` (optional) are
    /// row indices whose height is about to change; the anchor skips them so it
    /// lands on a row that will still be where the user expects it.
    void saveScrollAnchor(const QSet<int> *avoidRows = nullptr);
    /// Restore scroll position to the saved anchor after layout change.
    void restoreScrollAnchor();

    bool canPaginateBack() const { return _canPaginateBack; }
    bool canPaginateForward() const { return _canPaginateForward; }
    bool hitTimelineStart() const { return _hitTimelineStart; }
    bool isLive() const { return _isLive; }
    QString focusEventId() const { return _focusEventId; }
    bool hasMessage(const QString& eventId) const { return _messageIndex.contains(eventId); }

    int layoutCount() const { return _layout.size(); }
    const LayoutItem& layoutAt(int i) const { return _layout[i]; }
    int messageCount() const { return _messages.size(); }
    const TimelineItem& messageAt(int i) const { return _messages[i]; }
    // Apply `mutator` to every message in place, then repaint. Use this instead
    // of const_cast'ing messageAt().
    void enrichMessages(const std::function<void(TimelineItem &)> &mutator);

    /// Whether any loaded message is from `userId` (cheap in-memory relevance
    /// filter for the trust-warning bar — avoids loading the full member list).
    [[nodiscard]] bool hasSender(const QString &userId) const;
    /// A display name for `userId` taken from loaded messages (empty if none).
    [[nodiscard]] QString senderName(const QString &userId) const;

    /// Start playback of an audio file. Public so HistoryWidget can trigger
    /// auto-play after a pending download completes.
    void playAudio(const QString &eventId, const QString &filePath);
    void playAudioBytes(const QString &eventId, const QString &mxcUrl);

signals:
    /// Emitted after paint when messages have been read up to eventId.
    void messagesReadTill(const QString &eventId);
    /// Emitted when the user clicks a code block copy header.
    void codeCopied();
    /// Show a brief feedback toast in the parent history widget.
    void contextActionFeedback(const QString &text);
    /// Emitted when user requests deleting a message.
    void deleteMessageRequested(const QString &eventId);
    /// Emitted when user requests resending a failed message.
    void resendRequested(const QString &eventId);
    /// Emitted when user requests pin/unpin.
    void pinMessageRequested(const QString &eventId, bool pinned);
    /// Emitted when user requests editing a message.
    void editMessageRequested(
        const QString &eventId,
        const QString &senderName,
        const QString &body,
        const QString &formattedBody);
    /// Emitted when user requests replying to a message.
    void replyRequested(
        const QString &eventId,
        const QString &senderName,
        const QString &body,
        const QString &quotedText);
    /// Emitted when user requests forwarding a message.
    void forwardRequested(const QString &eventId);
    /// Emitted when user requests adding/removing a reaction.
    void reactionRequested(const QString &eventId, const QString &key, bool active);
    /// Emitted when user votes on a poll option.
    void pollVoteRequested(
        const QString &roomId,
        const QString &pollEventId,
        const QStringList &optionIds);
    /// Emitted when user clicks a reply preview block in a message bubble.
    void replyToMessageRequested(const QString &eventId, const QString &originEventId);
    /// Pinned mode only: the per-row "go to message" jump button was clicked.
    void jumpToMessageRequested(const QString &eventId);
    /// Emitted when list enters/exits message-level selection mode.
    void selectionModeChanged(bool active);
    /// Emitted when selected message count changes.
    void selectedCountChanged(int count);
    /// Emitted when user triggers forward for selected messages.
    void forwardSelectedRequested(const QStringList &eventIds);
    /// Open media viewer with image/video items in current timeline context.
    void openMediaViewRequested(const QVector<TimelineItem> &items, int index);
    /// Emitted when user clicks a sender avatar in the timeline.
    void userAvatarClicked(const QString &userId, const QString &eventId);
    /// Emitted when a matrix.to room/event link is clicked (for in-app navigation).
    void matrixLinkActivated(
        const QString &roomId, const QString &eventId, const QStringList &via);
    /// Emitted when a matrix.to user link is clicked.
    void matrixUserLinkActivated(const QString &userId);
    /// Emitted when user clicks the + button on the reaction bar.
    void reactionPanelRequested(const QString &eventId, const QPoint &anchor);
    /// Emitted when user clicks the cancel button on an uploading media.
    void cancelUploadRequested(const QString &eventId);
    /// Emitted when user clicks the cancel button on an in-flight media download.
    void mediaDownloadCancelRequested(const QString &mxcUrl);
    /// Emitted when user clicks a UTD (Unable to Decrypt) message bubble.
    void verifySessionRequested();
    /// Emitted when scroll anchor restoration requests a specific scroll position.
    void scrollToRequested(int position);
    /// Emitted when user clicks an undownloaded file attachment.
    void fileDownloadRequested(const QString &mxcUrl);
    /// Emitted when user clicks an already-downloaded file attachment.
    void fileOpenRequested(const QString &mxcUrl, const QString &filename, const QString &mime);
    /// Emitted when user clicks an audio play button but file isn't downloaded yet.
    void audioDownloadRequested(const QString &mxcUrl, const QString &eventId);
    /// Emitted when an audio file's real duration becomes known (played or
    /// probed) so it can be persisted and reused on later loads.
    void audioDurationLearned(const QString &mxcUrl, quint64 durationMs);
    /// Emitted when user clicks a video that needs local media resolution.
    void videoDownloadRequested(const QString &mxcUrl, const QString &eventId);
    /// Emitted when user saves remote Matrix media without a plaintext local path.
    void mediaExportRequested(const QString &mxcUrl, const QString &targetPath);
    /// Emitted to extract a local video thumbnail (FFmpeg via FFI) for a
    /// message that has no sender-provided thumbnail. Result is cached under
    /// "vidthumb:<eventId>" and repainted by the owner.
    void videoLocalThumbnailRequested(const QString &eventId, const QString &mxcUrl);
protected:
    bool event(QEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;

private:
    void recalculateLayout();
    // Recompute layout from `firstDirtyIndex` down, reusing the y/height prefix
    // above it (falls back to a full recalculateLayout for index <= 0). Used when
    // only a suffix of rows changed height (e.g. media resolving to real sizes).
    void recalculateLayoutFrom(int firstDirtyIndex);
    void recalculateLayoutAppend(int firstNewIndex);
    void recalculateLayoutPrepend(int prependedCount);
    // Real body of setSlice(); the public wrapper runs checkReadProgress() after
    // it so read detection re-runs on every content/layout change regardless of
    // which diff path the slice took.
    void setSliceImpl(const TimelineSlice &slice);
    // Full-window replaces are deferred to scroll-settle; returns true (and
    // stashes) while scrolling so the caller skips the immediate setMessages.
    [[nodiscard]] bool deferReplaceWhileScrolling(const TimelineSlice &slice);
    // setMessages() for a LIVE slice, keeping the reader where they were (bottom
    // pin, else scroll anchor). Never use for a room switch — there the caller
    // decides the position.
    void replaceSliceKeepingViewport(const QVector<TimelineItem> &items);
    // Explicit read-progress detector (replaces the former paintEvent side
    // effect). Emits messagesReadTill for the furthest fully-visible message at
    // or after the read frontier. Cheap and idempotent (dedups on last emit) so
    // it is safe to call from scroll, slice, and marking-mode changes.
    void checkReadProgress();
    void updateSenderGroupBounds();
    // Repaint just the row for `eventId` (falls back to a full update when the id
    // isn't currently laid out). Used by the inline video and hover paths.
    void updateRowByEventId(const QString &eventId);
    // Resolve a loaded message by event id via _messageIndex → &_messages[idx]
    // (replaced the old by-id copy map). Null if the id isn't in the window.
    [[nodiscard]] const TimelineItem *messageById(const QString &eventId) const;
    [[nodiscard]] TimelineItem *messageById(const QString &eventId);
    void paintDateSeparator(QPainter &p, qint64 timestamp, int y, int width);

    // The wallpaper is composited for the whole chat column and then slid up by
    // backgroundOriginY() (fillHeight = main-widget height, fromy =
    // -getMainSectionTop()). Anchoring
    // to the column and not to the scroll viewport is what keeps the doodle from
    // rescaling when the top bar appears or the composer grows.
    [[nodiscard]] QSize backgroundArea() const;
    [[nodiscard]] int backgroundOriginY() const;

    // Per-item glow: true iff the item is a glowing UTD (utdState==0) that hasn't
    // exceeded the safety window. Stamps firstSeen for new glowing UTDs.
    [[nodiscard]] bool itemGlowActive(const TimelineItem &item);
    // Start/stop the shimmer timer based on whether any message is glowing,
    // and prune firstSeen entries for items no longer present.
    void refreshDecryptingGlowState();
    // Invalidate + recalc UTD rows so glow→terminal-card height changes apply.
    void relayoutUtdRows();

    int messageIndexAt(int y) const;
    /// True while `index` names a message whose redaction is in flight.
    [[nodiscard]] bool isDeletingIndex(int index) const;
    /// Stop everything a message being deleted still owns: inline playback, audio,
    /// an in-flight download, and any hover affordance or scrub aimed at its row.
    void quiesceForDeleting(const QString &eventId);
    // Width available for message bubble geometry. Equals width() normally; in
    // pinned mode it is reduced by the jump-button gutter so wide bubbles never
    // sit under the button. Used consistently for layout, paint, and hit-test.
    int messageContextWidth() const;
    // Pinned mode only: the circular "go to message" button rect for the row of
    // message `index`, in widget coordinates. Empty if not applicable.
    QRect jumpButtonRect(int index) const;
    bool tryShowContextMenuAt(const QPoint &globalPos, const QPoint &localPos);
    void showMessageContextMenu(const QPoint &globalPos, const QPoint &localPos, int msgIndex);
    QString messageLink(const TimelineItem &item) const;
    void selectWholeMessage(int msgIndex);
    bool selectedTextForMessage(int msgIndex, QString &text) const;
    TextCursor cursorFromPoint(QPoint pos) const;
    void clearSelection();
    void normalizedSelection(TextCursor &from, TextCursor &to) const;
    bool selectionForMessage(int msgIndex, int &start, int &end) const;
    QString collectSelectedText() const;
    void rebuildMessageIndex();
    void updateSendStateAnimationTimer();
    QStringList selectedEventIdsInOrder() const;
    QStringList currentPollSelection(const TimelineItem &item) const;
    QStringList normalizePollSelection(
        const TimelineItem &item,
        const QStringList &optionIds) const;
    void applyPendingPollSelection(TimelineItem &item);
    bool setPollSelectionLocally(int messageIndex, const QStringList &optionIds);
    void toggleScrollDateShown(bool shown);
    void touchScrollDate(qint64 visibleTopTimestamp);
    int stickyDateIndexAtVisibleTop(int visibleTop) const;
    void refreshHoverFromPosition(const QPoint &localPos, Qt::MouseButtons buttons = Qt::NoButton);
    void refreshHoverFromCursor();

    // Hover reaction button + in-place vertical expansion (no animation).
    MessagePaintContext reactionPaintCtx(int index) const;
    void buildReactionColumnEmojis();
    void expandReactionColumn(int index);
    int reactionColumnCellHit(QPoint widgetPos) const;
    void hideReactionAffordance();
    // Widget-coord extent of the hover affordances last painted (reply pill,
    // reaction button/column). They hang OUTSIDE the row rect, so the
    // region-culled paint path won't erase their pixels — any repaint that
    // hides or moves them must invalidate this region or they ghost.
    [[nodiscard]] QRegion affordanceDirtyRegion() const;
    // Row rect inflated by a resting affordance's overhang (reply pill above the
    // bubble top, reaction corner button below the bubble bottom) so a
    // newly-hovered affordance paints unclipped under narrow invalidation.
    [[nodiscard]] QRect affordanceRowRect(int index) const;

    // Audio playback (private helpers).
    void pauseAudio();
    void resumeAudio();
    void stopAudio();
    void seekAudio(qint64 positionMs);

    // Inline-video seek-bar scrubbing: the active video's seek-bar rect in widget
    // coordinates (null when no active video with a rendered frame), and a
    // seek-to-x that maps a widget x within that rect to a clamped fraction.
    [[nodiscard]] QRect activeVideoSeekBarRect() const;
    void seekActiveVideoToX(int x, const QRect &bar);

    QVector<TimelineItem> _messages;
    MessageIndex _messageIndex; // eventId → physical index in _messages
    // Reply-parent resolver handed to the paint context (index + item vector);
    // replaced the old full-item _messagesById copy. Wired in the constructor.
    TimelineItemLookup _timelineLookup;
    QString _roomId;
    QVector<LayoutItem> _layout;
    QVector<int> _dateIndices; // indices into _layout where showDate==true (for binary search)
    int _totalHeight = 0;
    // Offset to bottom-align content when shorter than viewport.
    int _contentOffset = 0;
    bool _isGroup = true; // assume group chat for sender names
    bool _savedMessagesMode = false;
    bool _showOutgoingPrivateAvatars = false;
    bool _canPinMessages = false; // user has power level to pin
    bool _pinnedMode = false; // reused as the pinned-messages section view
    bool _readOnly = false; // unjoined-room preview: read-only, no jump/pin/forward
    int _pressedJumpButtonIndex = -1; // pinned mode: row whose jump button was pressed
    bool _largeEmojiEnabled = true; // from settings: render isolated emoji as large
    bool _replyButtonEnabled = true;    // from settings: show hover reply pill
    bool _reactionButtonEnabled = true; // from settings: show hover reaction pill
    bool _sessionVerified = false; // true when device is verified (UTD not clickable)
    int _hoveredIndex = -1; // index of message under cursor
    int _hoveredLinkStart = -1; // char offset of hovered link
    QString _hoveredLinkUrl;    // URL of hovered link
    bool _hoveredAvatar = false;     // true when cursor is over a sender avatar
    bool _hoveredFastReply = false; // true when cursor is over inline fast-reply action
    QString _hoveredVoiceSeekEventId; // active voice message whose waveform is hovered
    double _hoveredVoiceSeekProgress = -1.0; // 0..1 hover ratio, -1 when not over waveform
    bool _videoSeekDragging = false; // scrubbing the active inline-video seek bar
    int _replyPillIndex = -1;       // message index showing reply pill (-1 = none)
    qreal _replyPillOpacity = 0.0;  // 0..1 animation progress
    QVariantAnimation *_replyPillAnim = nullptr; // scale+opacity animation
    QTimer _replyPillHideTimer;     // 300ms delay before hiding pill
    QRect _replyPillWidgetRect;     // pill rect in widget coordinates (from last paint)
    // Hover reaction button + in-place vertical expansion (bottom-corner
    // button; no animation, only a hide debounce).
    int _reactionPillIndex = -1;        // message index showing the affordance
    bool _reactionExpanded = false;     // pill expanded into the emoji column
    bool _reactionExpandUp = false;     // column grows upward (viewport fit)
    int _reactionScroll = 0;            // px scroll offset within the column
    int _reactionHovered = -1;          // hovered cell index (-1 = none)
    QRect _reactionButtonWidgetRect;    // resting button rect, widget coords
    QRect _reactionColumnWidgetRect;    // expanded column rect, widget coords
    QVector<QString> _reactionColumnEmojis; // quick + recents (built on expand)
    QVector<QString> _reactionRecentEmojis; // recents pushed from HistoryWidget
    QTimer _reactionHideTimer;          // 300ms hide debounce (no animation)
    QTimer _reactionShowTimer;          // delay before the resting pill appears
    int _reactionPendingIndex = -1;     // message index the show timer will commit to
    QTimer _reactionExpandTimer;        // delay before a hover over the pill expands the column
    int _reactionExpandPendingIndex = -1; // message index the expand timer targets
    bool _hoveredCopyButton = false; // true when cursor is over a code block copy header
    bool _overLinkPreview = false;   // true when cursor is over a link preview card
    bool _hoveredTimestamp = false;  // true when cursor is over timestamp area
    QPointer<HistoryPopupMenuStyle::PopupMenu> _activeMenu;
    TimestampTooltip *_timestampTooltip = nullptr;
    QTimer _tooltipTimer;            // 1s delay before showing timestamp tooltip
    QTimer _sendStateTimer;          // 100ms tick for sending clock animation
    QTimer _decryptingAnimTimer;
    qreal _decryptingGlowProgress = 0.0; // 0..1 within the 2s glow cycle
    qint64 _decryptingGlowStartMs = 0;   // glow cycle start timestamp
    // First time each glowing UTD (utdState==0) was seen, keyed by eventId; a
    // stuck UTD is downgraded to a terminal card after kUtdGlowSafetyMs.
    QHash<QString, qint64> _utdGlowFirstSeenMs;
    // Event ids of currently-glowing UTDs. The 30fps shimmer timer iterates this
    // set instead of scanning all _messages each tick (O(glowing), not O(loaded)).
    QSet<QString> _glowingEventIds;
    // Event ids the user has just deleted (awaiting redaction): their bubbles
    // render dimmed until `delivery.deleted` flips or the delete fails.
    QSet<QString> _deletingEventIds;
    // Event ids whose URL link-preview is currently being fetched; their URL
    // glows until the fetch finishes (success or failure).
    QSet<QString> _previewGlowEventIds;
    // Glowing-UTD count at the previous anim tick; a decrease means a subset
    // safety-expired and the now-terminal rows must be reflowed.
    int _lastGlowingCount = 0;
    QTimer _highlightTimer;          // 60fps fade for reply target highlight
    QTimer _scrollDateHideTimer;
    QVariantAnimation _scrollDateOpacityAnimation;
    bool _scrollDateShown = false;
    qreal _scrollDateOpacity = 0.0;
    qint64 _scrollDateTopTimestamp = 0;
    int _visibleTop = 0;
    int _lastScrollVisibleTop = -1;
    int _lastScrollDateIndex = -1;
    // True while actively scrolling: paint suppresses per-row animation-repaint
    // scheduling (glow etc.) so it doesn't defeat the scroll-blit optimization.
    // Cleared by _scrollSettleTimer a short moment after scrolling stops, which
    // repaints once to resume the (time-based) animations.
    bool _scrolling = false;
    QTimer _scrollSettleTimer;
    // A full-window replace (setMessages) is a whole-list relayout + repaint —
    // ~20-40ms — and live slice deliveries can trigger it several times a second
    // while the user is flinging through history, producing visible frame
    // hitches (measured as the max-µs spikes under TM_PAINT_STATS, cause=slice).
    // While scrolling, the freshest such slice is stashed here instead and
    // applied once the scroll settles. Incremental prepend/append/overlap
    // updates still apply immediately (they are cheap and, for pagination
    // prepends, essential) and clear the stash, since they leave _messages
    // already current — so re-applying a now-stale replace can never clobber
    // them. The flush re-runs setSlice, which re-diffs against the current
    // _messages, so a genuine window change still lands correctly.
    std::optional<TimelineSlice> _deferredReplaceSlice;
    int _sendStateTick = 0;
    int _sendingCount = 0;             // count of messages with SendState::Sending
    QString _highlightedEventId;
    float _highlightOpacity = 0.0f;
    qint64 _highlightStartTime = 0;
    QPoint _tooltipGlobalPos;        // mouse position for delayed tooltip
    QPoint _tooltipShownAt;          // anchor position when tooltip was shown
    QString _tooltipText;            // formatted text for delayed tooltip

    // Chat wallpaper: gradient + soft-light doodle, composited at viewport size.
    // Rebuilt off a debounce so a window drag stretches the stale pixmap rather
    // than re-tiling every frame.
    Theme::ChatBackgroundCache _bgCache;
    QTimer _bgRebuildTimer;         // 150ms debounce on resize
    QPointer<QWidget> _bgAnchor;    // chat column; null falls back to viewport

    HistorySelectionState _selection;
    // Room we have not joined: its name/description are all there is to show.
    QString _previewName;
    QString _previewTopic;
    bool _syncing = false;
    bool _loadingTimeline = false;
    bool _loadingTimelineVisible = false; // preloader shown while a jump fetch is in flight
    bool _jumpLoadingCover = false; // opaque jump "Loading…" cover (see setJumpLoadingCover)
    QHash<QString, QStringList> _pendingPollSelections;

    // Event IDs with locally-applied reaction changes not yet confirmed by server.
    // While pending, in-place slice updates preserve local reactions instead of
    // overwriting with the (stale) server version.
    QSet<QString> _pendingReactionEventIds;

    // Slice state (timeline navigation).
    bool _canPaginateBack = false;
    bool _canPaginateForward = false;
    bool _hitTimelineStart = false;
    bool _isLive = true;
    QString _focusEventId;

    // Scroll anchor for prepend.
    QString _scrollAnchorEventId;
    int _scrollAnchorPixelOffset = 0;

    // Unread bar separator state.
    QString _firstUnreadEventId;   // event ID of first unread message (empty = no bar)
    int _unreadBarCount = 0;       // unread count at the bar anchor, not visibility state

    // Read-detection frontier — decoupled from the frozen visual bar above. The
    // detector anchors here (falling back to _firstUnreadEventId before the
    // first push) so scroll-reading still decrements the count even after the
    // bar is consumed, unloaded, or never placed. See checkReadProgress().
    QString _readFrontierEventId;
    QString _lastReadTillEmitted;  // dedup: last id emitted via messagesReadTill
    // Suppresses read detection across a programmatic viewport move (a gappy-sync
    // full-replace that clamps the scroll and makes the viewport span the whole
    // content). Set when non-empty content is replaced wholesale; cleared by
    // genuine user input / explicit navigation. See checkReadProgress().
    bool _readDetectionHold = false;

    // Paint-driven read marking: true when window is active and room is loaded.
    bool _markingMessagesRead = false;

    bool _inResize = false;
    int _lastLayoutWidth = -1; // width at which recalculateLayout last ran

    // Audio playback state.
    QMediaPlayer *_mediaPlayer = nullptr;
    QAudioOutput *_audioOutput = nullptr;
    QBuffer *_audioPlaybackBuffer = nullptr;
    HistoryAudioPlaybackState _audioPlayback;
    QTimer _playbackRepaintTimer;

    // Single shared inline video player (one video plays at a time).
    HistoryInlineVideoPlayer *_inlineVideo = nullptr;

    // Audio duration probe: extracts duration from local files when server
    // metadata is missing. Probed values are written back onto TimelineItem.
    QMediaPlayer *_probePlayer = nullptr;
    QString _probeEventId;
    QSet<QString> _probedEventIds;  // already probed (avoid re-probing)
    QHash<QString, qint64> _cachedAudioDurations;  // persists probed durations across setMessages()
    void probeAudioDuration(const QString &eventId, const QString &filePath);

    // Video thumbnail probe: extracts first frame from video files when
    // server doesn't provide a thumbnail. Frames are stored in MediaCache.
    HistoryVideoThumbnailProbeState *_videoThumbnailProbe = nullptr;
};

} // namespace TeleMatrix

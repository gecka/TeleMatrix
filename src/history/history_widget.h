// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QDate>
#include <QHash>
#include <QSet>
#include <QLabel>
#include <QPointer>
#include <QQueue>
#include <QTimer>
#include <QVariantAnimation>
#include <functional>
#include <optional>

#include "ui/rp_widget.h"
#include "history_draft_state.h"
#include "history_typing_state.h"
#include "history_scroll_state.h"
#include "history_pinned_state.h"
#include "jump_load_controller.h"
#include "history_return_stack.h"
#include "history_media_request_state.h"
#include "history_pending_local_media.h"
#include "history_read_receipt_state.h"
#include "../protocol/protocol_types.h"

namespace Ui {
class ScrollArea;
class RpWidget;
class TextButton;
class ToastWidget;
} // namespace Ui

namespace TeleMatrix {

class AppController;
class ProtocolBridge;
class HistoryList;
class HistoryInlineVideoPlayer;
class HistoryInput;
class HistoryEmojiPicker;
class HistoryDownButton;
class HistoryPinnedBar;
class NoChatPlaceholder;
class HistoryAttachPopup;
class HistoryTopBar;
class UnreadStateStore;
struct UnreadRoomState;
struct PreparedFile;

/// Container for the message timeline: top bar + message list + input area.
class HistoryWidget : public Ui::RpWidget {
    Q_OBJECT

public:
    explicit HistoryWidget(
        AppController *controller,
        QWidget *parent,
        ProtocolBridge *bridge);
    ~HistoryWidget() override;

    /// Apply ConvertScale to topbar pixel constants (call once at startup).
    static void initTopBarPxValues();

    /// Load and display the timeline for a room.
    void loadRoom(const QString &roomId);

    /// Show a room we have not joined: its name and description centred over an empty timeline,
    /// with a Join bar where the composer would be. Matrix serves no message history to a
    /// non-member, so there is nothing else that could be shown.
    void loadRoomPreview(const QString &roomIdOrAlias, const QStringList &via);
    /// True while showing such a room. It is deliberately absent from the rooms list, so callers
    /// that reconcile the open room against that list must not treat it as gone.
    [[nodiscard]] bool isPreviewingRoom() const { return _previewMode; }
    /// Called when the fullscreen viewer closes: if an inline player is still active
    /// (paused) for this mxc — i.e. it handed off to fullscreen — re-sync it to the
    /// final position so it shows a paused frame with controls.
    void onFullscreenVideoClosed(const QString &mxcUrl, qint64 positionMs);
    /// Close the current room and return to the placeholder state.
    void closeRoom();
    /// Run the same Escape priority chain as the compose input.
    void escape();

    /// React to OS network reachability changes: while offline, in-flight uploads
    /// show "Waiting for network..."; on reconnect, failed direct uploads
    /// auto-resend (they have no send-queue retry of their own).
    void onNetworkOnlineChanged(bool online);

    /// Scroll to first unread message, or to bottom if no unreads.
    void scrollToUnreadOrBottom();

    /// Navigate to a specific message. Loads the room if different from current.
    void showMessage(const QString &roomId, const QString &eventId);

    /// Navigate to a message that is expected to be at the live tail — what a
    /// desktop notification points at. Unlike showMessage() this keeps (or
    /// restores) the live timeline and only falls back to a focused fetch when
    /// the target really is outside the live window. See history/jump_routing.h.
    void showMessageLive(const QString &roomId, const QString &eventId);

    /// Jump to a message in the current room; fetches via focusOnEvent if not loaded.
    void jumpToMessage(const QString &eventId);
    /// Start a focused-slice fetch for eventId without choosing a preloader location.
    void beginFocusFetch(const QString &eventId);

    /// Scroll to the message closest to the given date.
    void scrollToDate(const QDate &date);

    /// Export the current room timeline to a file.
    void exportRoomHistory(const QString &roomId);

    /// Insert a mention into the compose field for the active room.
    void mentionUser(const QString &userId, const QString &displayName);

    /// Clear the search-active highlight on the top bar search icon.
    void clearSearchActive();

    /// Start in-room search (top-bar button / Ctrl+F). Returns false when no
    /// room is open or the room is only previewed.
    bool requestSearchInCurrentRoom();

    /// Set the window-active state (called by parent when focus changes).
    void setWindowActive(bool active);

    /// Show/hide the "connecting…" status with a spinner in the room header.
    void setConnecting(bool connecting);
    [[nodiscard]] bool isWindowActive() const { return _windowActive; }
    /// Whether the focused widget belongs to the compose input.
    [[nodiscard]] bool inputHasFocus() const;

signals:
    /// Emitted when a room's draft text changes (for dialogs list preview).
    void draftChanged(const QString &roomId, const QString &text);
    /// Emitted when timeline requests opening the media viewer.
    void openMediaViewRequested(const QVector<TimelineItem> &items, int index);
    /// Route top-bar search to the dialogs sidebar search field.
    void searchInChatRequested(const QString &roomId, const QString &roomName, bool isDirect);
    /// Emitted when Escape closes the current chat.
    void cancelRequests();
    /// Emitted when unread count changes for the current room (for chat list badge).
    void unreadCountChanged(const QString &roomId, int count);
    /// Emitted when the user requests leaving the active room from the top-bar menu.
    void leaveRoomRequested(const QString &roomId);
    /// Emitted when room settings should be opened (top bar name click or quick menu).
    void openRoomSettingsRequested(const QString &roomId);
    /// Emitted when the room settings Members section should be opened.
    void openRoomMembersSettingsRequested(const QString &roomId);
    /// Emitted when a user profile popup should be opened (avatar click in timeline).
    void openUserProfileRequested(
        const QString &roomId, const QString &userId, const QString &displayName);
    /// Emitted when showMessage navigates to a different room (so chat list can sync).
    /// `via` carries matrix.to routing hints for rooms the main widget will
    /// have to preview/join over federation; empty for plain switches.
    void roomSwitchRequested(
        const QString &roomId, const QStringList &via = QStringList());

protected:
    void resizeEvent(QResizeEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragLeaveEvent(QDragLeaveEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    void setupTopBar();
    void showRoomQuickMenu();
    void setupMessageList();
    /// Wire an inline video player's stream-url / progress / resolve callbacks
    /// to the bridge. Shared by the timeline and pinned-messages lists.
    void wireInlineVideoPlayer(HistoryInlineVideoPlayer *player);
    void setupPinnedBar();
    void setupInput();
    void setupCornerButtons();
    void setupPlaceholder();
    void showChatControls(bool show);
    void showInvitePanel(const RoomSummary &room);
    void hideInvitePanel();

    void setupJoinBar();
    void exitPreviewMode();
    void onPreviewReady(
        const QString &roomIdOrAlias,
        bool success,
        const RoomPreviewInfo &preview,
        const QString &error);
    void onPreviewMessagesReady(
        const QString &roomId,
        bool success,
        const QVector<TimelineItem> &items,
        const QString &nextToken,
        const QString &error);
    // Request the next-older page of preview history (scroll-to-top in preview mode).
    void loadMorePreviewMessages();
    // Keep pulling older preview pages until the viewport fills or the empty-page streak caps out.
    void maybeContinuePreviewPagination();
    // Show the loading pill, the timeline, or the name+description placeholder per the preview state.
    void updatePreviewDisplay();
    void onJoinResult(
        const QString &roomIdOrAlias,
        bool success,
        const QString &roomId,
        const QString &error);
    void onKnockResult(
        const QString &roomIdOrAlias,
        bool success,
        const QString &roomId,
        const QString &error);
    /// Heavy data loading (FFI calls + processing).  Called deferred on
    /// room switch so the empty-timeline UI paints immediately.
    void loadRoomData(
        const QString &roomId,
        bool switchingRoom,
        int previousTop);
    void scheduleInitialRoomScroll(
        const QString &roomId,
        bool switchingRoom,
        int previousTop,
        bool initialSliceIsLive);
    void updateControlsGeometry();
    bool _inUpdateControls = false;
    void updateCornerButtonPositions();
    bool cornerButtonsDownShown() const;
    void animateInputVisibility(bool visible);
    void animatePinnedBarVisibility(bool visible);
    void scrollToMessageAndHighlight(const QString &eventId);
    enum class JumpSource { Normal, Pinned };
    // Single funnel for every jump entry point (search / link / reply / pinned):
    // instant scroll when hasMessage, else an empty "Loading…" cover (1s floor)
    // then reveal, else fallback to live + toast. See docs/jump-to-message-redesign-design.md.
    void jumpTo(const QString &roomId, const QString &eventId, JumpSource source);
    void enterJumpLoad(const QString &eventId, JumpSource source);
    void startJumpFloorTimer();
    void finishJumpReveal();
    void finishJumpFallback();
    /// Resolve the forward-dialog "Saved Messages" pending sentinel into a
    /// real room id, creating the room when needed, then call `forward`.
    void resolveForwardDestination(
        const QString &dstRoomId, std::function<void(const QString &)> forward);
    void showPinnedSection();
    void applyPinnedScroll();
    void openPinnedMessages();
    void closePinnedSection();
    void returnToLive();
    void checkReturnStack();
    void onTimelineChanged(const QString &roomId);
    void applyTimelineSlice(const QString &roomId, TimelineSlice slice);
    void checkPaginationThresholds();
    void scheduleDraftChanged(const QString &roomId);
    void flushDraftChanged();
    void hydratePendingLocalMedia(QVector<TimelineItem> &messages);
    void retireAcknowledgedPendingLocalMedia(const QVector<TimelineItem> &messages);
    void adoptUploadedMediaAsResolved(const TimelineItem &item);
    void normalizeDormantLocalMediaPlaceholders(QVector<TimelineItem> &messages) const;
    void removePendingLocalMediaUpload(const QString &eventId);
    void clearPendingLocalMediaUploads();
    /// True while `eventId` names one of our media uploads still transferring.
    [[nodiscard]] bool isUploadInFlight(const QString &eventId) const;
    /// Abort that upload and drop its bubble at once. The backend aborts the
    /// transfer, or redacts the event if it landed while we were asking.
    void cancelUploadForEvent(const QString &eventId);
    // Re-send every failed upload (any room) with its retained params + original
    // transaction id. Called on network reconnect.
    void resendFailedUploads();
    // Dispatch the dialog's accepted files: caption handling + per-file send,
    // recompressing images off the UI thread when requested.
    void sendDialogFiles(
        const QVector<PreparedFile> &files,
        const QString &caption,
        bool compress);
    void sendPreparedFile(
        const PreparedFile &file,
        const QString &caption,
        bool compress);
    // Append the optimistic upload bubble immediately (before any recompression
    // or SDK ingestion, both of which scale with file size), keyed by
    // transactionId for later reconciliation with the SDK's own echo.
    void appendOptimisticMediaEcho(
        const PreparedFile &file,
        const QString &caption,
        const QString &transactionId);
    // Dispatch the actual upload to the SDK send queue. `sendPath` may be a
    // recompressed image; the bubble was already shown by the call above.
    void sendPreparedMedia(
        const PreparedFile &file,
        const QString &sendPath,
        const QString &caption,
        const QString &transactionId);
    void onSendMessage(
        const QString &text,
        const QString &formattedBody,
        const QString &replyToEventId = QString());
    void saveScrollState();
    [[nodiscard]] QString formatLastSeen(
        const QVector<TimelineItem> &messages) const;
    [[nodiscard]] static QString formatLastSeenTimestamp(qint64 ts);
    void refreshLastSeenSubtitle();
    /// Update unread count, down button badge, and notify chat list.
    void updateUnreadCount(int count, bool syncToStore = true);
	[[nodiscard]] static bool countsTowardsUnread(const TimelineItem &item);
	[[nodiscard]] int messageIndexById(const QString &eventId) const;
	[[nodiscard]] int unreadBarScrollTop() const;
	[[nodiscard]] int unreadBarPassedThreshold() const;
	[[nodiscard]] bool canPlaceUnreadBarAt(const QString &eventId) const;
	[[nodiscard]] bool scrollToUnreadBar();
	[[nodiscard]] bool requestInitialUnreadBackfill();
	bool tryApplyInitialUnreadScroll();
	[[nodiscard]] int unreadCountInRange(
		const QString &firstEventId,
		const QString &lastEventId) const;
	[[nodiscard]] QString nextUnreadEventIdAfter(const QString &eventId) const;
	void setUnreadBarPreservingScroll(
		const QString &eventId,
		int unreadCount);
	void clearUnreadBarPreservingScroll();
	// Place the unread delimiter at the frozen per-session anchor (captured on
	// first placement). force=true shows it at the start of history even when no
	// read message precedes it; otherwise canPlaceUnreadBarAt() gates placement.
	void placeUnreadBar(int unreadCount, bool force = false);
	// Destroy the visual delimiter and drop the frozen session anchor.
	void destroyUnreadBar();
	void applyOptimisticReadProgress(const QString &eventId);
	void applyLiveUnreadState(const TimelineSlice &slice);
	void applyStoreUnreadState(const UnreadRoomState &state);
	void syncReadMarkingMode();
	// Tell the store whether this room is currently consuming reads instantly
	// (window focused + parked at the live bottom, not held after a programmatic
	// teleport). While it is, the store clamps the room's displayed unread count
	// so a message arriving at the bottom never blinks the rooms-list badge.
	// Evaluated only at settled scroll positions (scrollbar valueChanged, window
	// activation, room switch) — never mid-slice, where the layout is transient.
	void updateReadConsumingGate();
	// Mirror the optimistic first-unread frontier into the list's read
	// detector. Called wherever _optimisticUnreadFrontierEventId settles so
	// scroll-reading decrements the count independently of the frozen bar.
	void pushReadFrontier();
	void applyDeferredMediaUpdates();
	void maybeFinalizeUnreadBarAfterScroll();
	void sendTrackedReadReceipt(const QString &eventId);
    /// Optimistically apply pin state to timeline and pinned bar.
    void applyPinStateLocally(const QString &eventId, bool pinned);
    void refreshPinnedBar();
    void schedulePendingJumpVisibilityTimeout(
        const QString &roomId,
        const QString &eventId,
        quint64 requestId);
    void cancelPinnedJump(const QString &eventId);
    void completePinnedJump();
    void updatePinnedMessagesFromSlice(const QStringList &pinnedEventIds,
                                       const QVector<TimelineItem> &messages);
    void resetCurrentRoomPermissions();
    // True when the current room's system/service messages should be hidden (public room + setting).
    [[nodiscard]] bool shouldHideSystemMessages() const;
    /// The open room is this account's Saved Messages room.
    [[nodiscard]] bool isSavedMessagesRoom() const;
    /// `roomId` is a joined room in the bridge's cached room list.
    [[nodiscard]] bool isJoinedRoom(const QString &roomId) const;
    /// Preview could not load at all: offer opening the permalink in the
    /// browser (closed federation / room unknown to the homeserver).
    void offerBrowserFallback(const QString &roomIdOrAlias, const QStringList &via);
    /// Minimal info card for the Saved Messages room (title click).
    void showSavedMessagesInfo();
    // Re-fetch the current room's full timeline so a filter-state change rebuilds the list.
    void refilterCurrentTimeline();
    [[nodiscard]] bool currentRoomCanManageMembers() const;

    AppController *_controller = nullptr;
    ProtocolBridge *_bridge = nullptr;
    UnreadStateStore *_unreadStateStore = nullptr;
    bool _windowActive = false;
    bool _readConsumingGate = false; // last pushed read-consuming state (see updateReadConsumingGate)
    QString _currentRoomId;
    int _roomUnreadCount = 0;
    int _firstUnreadIndex = -1; // index into messages of first unread
    QString _optimisticUnreadFrontierEventId;
    QString _optimisticReadTillEventId;
    // Whether the latest slice's read marker is present in the loaded window
    // (from TimelineSlice.readMarkerLoaded). When true the first-unread anchor is
    // the confirmed boundary, so canPlaceUnreadBarAt places at once instead of
    // waiting for a visible read message to precede it — which the "hide system
    // messages in public rooms" filter can erase. Refreshed on every slice.
    bool _readMarkerLoaded = false;
    // Frozen first-unread boundary for the open room: captured once when the
    // delimiter is first placed, then the bar is only ever (re)placed here —
    // never at the drifting live frontier. It SURVIVES transient conditions
    // (unread count reaching zero, or the anchor's message scrolling out of the
    // loaded window) so the bar returns to the same event when it reloads.
    // Cleared only at genuine session boundaries: room switch, sending, or the
    // bar being destroyed (scroll-to-bottom / fully read). See
    // UnreadBar::resolveAnchor / decide (unread_bar_placement.h).
    QString _sessionUnreadBarEventId;
    // Latched once the room's delimiter is decided during initial entry: while
    // set, live updates (new messages, reads, receipts) never place, move, or
    // clear the drawn delimiter — it stays frozen for as long as the room is
    // open. Reset only when leaving / switching rooms.
    bool _unreadBarResolved = false;
    // Bounded initial-entry attempts. If the unread boundary can never be
    // loaded (e.g. it lies beyond pageable history), tryApplyInitialUnreadScroll
    // would otherwise keep _initialUnreadScrollNeeded true forever and the latch
    // above would never set. Reset per room session (alongside _unreadBarResolved).
    int _unreadEntryAttempts = 0;

    // Per-chat scroll position.
    // Stores anchor event + pixel offset so position survives timeline reloads.
    HistoryScrollStateStore _scrollStates;
    HistoryDraftStore _drafts;
    HistoryPinnedState _pinnedState;

    HistoryTopBar *_topBar = nullptr;
    Ui::RpWidget *_topBarShadow = nullptr;
    HistoryPinnedBar *_pinnedBar = nullptr;
    Ui::ScrollArea *_scroll = nullptr;
    HistoryList *_list = nullptr;
    HistoryInput *_input = nullptr;
    qreal _inputShownProgress = 1.0;
    QVariantAnimation *_inputVisibilityAnimation = nullptr;
    qreal _pinnedBarShownProgress = 0.0;
    QVariantAnimation *_pinnedBarAnimation = nullptr;
    QTimer *_draftChangedTimer = nullptr;
    QString _pendingDraftRoomId;
    qint64 _draftChangeBurstStarted = 0;

    // "Select a chat to start messaging" placeholder.
    NoChatPlaceholder *_noChat = nullptr;
    bool _synced = false; // true after initial sync completes

    // Invite preview panel (replaces timeline for invited rooms).
    QWidget *_invitePanel = nullptr;
    bool _isInvitedRoom = false;

    // Preview mode: a room we have not joined. Unlike the invite panel above, this is a real
    // bottom bar — it is laid out in updateControlsGeometry and reserves height, so the timeline
    // shrinks to fit it instead of being covered by an overlay.
    QWidget *_joinBar = nullptr;
    ::Ui::TextButton *_joinButton = nullptr;
    QLabel *_joinError = nullptr;
    bool _previewMode = false;
    QString _previewRoomIdOrAlias;
    /// Joined room to restore when a preview fails to load (see onPreviewReady).
    QString _previewReturnRoomId;
    // The resolved room id from the preview (differs from the alias the user clicked); used to
    // match the async preview-messages result.
    QString _previewResolvedRoomId;
    QStringList _previewVia;
    // The details behind the current preview, shown by the room-info popup on name-click.
    RoomPreviewInfo _previewInfo;
    // Backward-pagination state for the preview (world-readable rooms scroll their full history).
    QString _previewNextToken;      // token for the next-older page, empty at the start of history
    bool _previewReachedStart = false;
    bool _previewLoadingMore = false; // guards against a burst of scroll events firing many requests
    // Rooms that are mostly bridge join/leave churn (e.g. KDE User Help) return whole pages that map
    // to zero displayable messages. Keep paginating through them, but bound the streak of empty pages
    // so an all-bot room can't paginate its entire history.
    int _previewEmptyPageStreak = 0;
    // Drive the loading pill → timeline / placeholder transition. The placeholder (name+description)
    // only appears once we've CONFIRMED there is no readable history; until then it's just the pill.
    bool _previewSummaryReady = false;   // getRoomPreview resolved (success or failure)
    bool _previewMessagesDone = false;   // the peek settled with no messages to show

    // Corner buttons are children of _scroll.
    HistoryDownButton *_downButton = nullptr;
    bool _downButtonShown = false;
    qreal _downButtonShownProgress = 0.0;
    QVariantAnimation *_downButtonAnimation = nullptr;
    QVariantAnimation *_scrollToAnimation = nullptr;
    QTimer *_readReceiptTimer = nullptr;
	HistoryReadReceiptState _readReceipts;
	bool _initialScrollPending = false; // true until first queued scroll after room switch
	bool _pendingInitialRoomScroll = false;
	QString _pendingInitialRoomScrollRoomId;
	bool _pendingInitialRoomScrollSwitchingRoom = false;
	int _pendingInitialRoomScrollPreviousTop = 0;
	bool _jumpScrollApplied = false;   // true when jump scroll was set in loadRoomData
	bool _scrollToBottomPending = false; // true after sending, cleared on next scroll-to-bottom
	bool _forceBottomEntryUntilLiveSlice = false; // startup-sync room entry stays visually pinned to the latest slice without consuming unread state
	bool _initialUnreadScrollNeeded = false; // scroll to unread on first slice delivery
	bool _preserveUnreadBarOnEntry = false; // keep unread delimiter stable until the user passes it
	bool _unreadBarDismissed = false; // do not recreate the delimiter after it was passed

    // Cache: outgoing message event id → HTML formattedBody.
    // Populated on send; used as fallback if bridge data lacks formattedBody.
    QHash<QString, QString> _formattedBodies;
    QHash<quint64, QString> _pendingLocalEchoIdsByRequestId;
    quint64 _nextSendRequestId = 1;
    HistoryPendingLocalMediaState _pendingLocalMedia;
    // Optimistic upload echoes (keyed by transaction id): shown immediately when
    // the user sends, BEFORE the SDK ingests the file (which is proportional to
    // file size). Re-injected into incoming slices until the SDK's own echo
    // (same transaction id) appears, so the bubble never disappears mid-upload.
    QHash<QString, TimelineItem> _optimisticMediaEchoes;
    // Upload bubbles cancelled by the user: removed optimistically and filtered
    // out of incoming slices until the backend cancel (abort/redact) lands, so
    // they don't flash back. Cleared on room switch.
    QSet<QString> _cancelledUploadIds;
    // Cancelled ids for which the real backend cancel was already re-issued once
    // the SDK echo appeared (the optimistic cancel can land before the SDK has
    // created its echo — see the file-read note in sendPreparedFile).
    QSet<QString> _cancelRedispatchedIds;
    // Event ids the user has just asked to delete; their bubbles are dimmed
    // (HistoryList::markDeleting) until the redaction lands or the delete fails.
    QSet<QString> _pendingDeleteEventIds;

    // Toast for "Code copied to clipboard."
    Ui::ToastWidget *_toast = nullptr;

	// Debounce timer for rapid onTimelineChanged signals.
	QTimer *_timelineDebounce = nullptr;
	quint64 _latestTimelineSliceRequestId = 0;
	bool _timelineBaselineReady = false;

	// Last-seen timer for direct chats.
    QTimer *_lastSeenTimer = nullptr;
    qint64 _lastSeenTimestamp = 0;
    bool _currentRoomIsDirect = false;
    RoomNotificationMode _currentNotificationMode = RoomNotificationMode::AllMessages;
    QString _directChatUserId;

    // Cross-signing trust-violation warning bar (identity changed) for the open room.
    void showTrustWarning(const QString &userId);
    void hideTrustWarning();
    QWidget *_trustWarningBar = nullptr; // HistoryTrustWarningBar (file-local type)
    bool _trustWarningActive = false;
    QString _trustWarningUserId;
    QSet<QString> _trustWarningDismissed; // per-room-session: don't re-nag after dismiss
    bool _currentRoomPermissionsLoaded = false;
    bool _currentRoomCanInvite = false;
    bool _currentRoomCanKick = false;
    bool _currentRoomCanBan = false;
    // Public rooms can hide their system/service messages (Appearance setting). `_currentRoomIsPublic`
    // is derived from the async room-settings snapshot; `_hideSystemMessages` mirrors the setting.
    bool _currentRoomIsPublic = false;
    bool _currentRoomIsEncrypted = false;
    bool _hideSystemMessages = true;

    // Cached member avatar URLs for the current room.
    // Populated in loadRoomData, reused in onTimelineChanged to avoid
    // blocking getRoomMembers() FFI calls on every timeline update.
    QHash<QString, QString> _memberAvatarCache;
    HistoryMediaRequestState _mediaRequests;

    // Pagination debounce flags — set true when a paginate request is in flight,
    // cleared when onTimelineChanged delivers the updated slice.
    // Safety: auto-reset after 5s if no slice arrives (prevents permanent lock).
    bool _isPaginatingBack = false;
    bool _isPaginatingForward = false;
    QTimer *_paginationTimeoutTimer = nullptr;
    int _paginationRetryCount = 0;
    static constexpr int kMaxPaginationRetries = 3;

    // Pending cross-room or in-room jump.  Scoped to (roomId, eventId) so that
    // loadRoom() can distinguish "jump targets the new room" from "stale jump
    // for a different room".  Cleared once onTimelineChanged confirms the
    // message is in the slice.
    struct PendingJump {
        uint64_t requestId = 0;
        QString roomId;
        QString eventId;
    };
    std::optional<PendingJump> _pendingJump;
    // Set alongside _pendingJump for a notification jump: the target is expected
    // at the live tail, so loadRoomData must NOT focus the window on it. Spent
    // on the first live slice that still lacks the target, which escalates to a
    // focused fetch.
    bool _pendingJumpPreferLive = false;
    uint64_t _nextJumpRequestId = 1;
    // After a jump completes, holds the target event ID so that
    // setSlice anchors to it (not the first visible message).
    // Cleared on return to live mode or room switch.
    QString _focusJumpEventId;

    // Jump loading-cover episode (empty "Loading…" for a 1s floor, then reveal).
    JumpLoadController _jumpLoad;
    QTimer *_jumpFloorTimer = nullptr;
    QString _jumpLoadEventId;
    JumpSource _jumpLoadSource = JumpSource::Normal;

    // Pending pin action for revert on failure.
    QString _pendingPinEventId;
    bool _pendingPinState = false;

    // Pinned messages section (replaces timeline when shown).
    Ui::ScrollArea *_pinnedScroll = nullptr;
    int _pinnedScrollTop = -1; // remembered pinned-list scroll pos across close/open; -1 = start at bottom
    HistoryList *_pinnedList = nullptr;
    bool _pinnedSectionVisible = false;

    // Reaction bar: standalone emoji picker for reaction mode.
    HistoryEmojiPicker *_reactionEmojiPicker = nullptr;

    // Re-check unresolved media after stale paths are cleaned up.
    QTimer *_mediaRecheckTimer = nullptr;
    QTimer *_mediaInvalidationTimer = nullptr; // 50ms coalesce of media relayouts
    void recheckUnresolvedMedia();
    // True when the message's row is on or within one screenful of the viewport;
    // gates media (avatar/thumbnail) fetches so back-scroll doesn't request the
    // whole loaded history's avatars at once.
    [[nodiscard]] bool isMessageNearViewport(const QString &eventId) const;

    // Typing notifications (throttle once per 5s, cancel after 5s idle).
    QTimer *_typingSendTimer = nullptr;
    QTimer *_typingCancelTimer = nullptr;

    // Incoming typing indicator (bouncing dots animation in top bar).
    QTimer *_typingDotTimer = nullptr;
    HistoryTypingState _typingState;
    QString _cachedSubtitle; // subtitle to restore when typing ends
    void refreshTypingSubtitle();

    // Top bar quick menu (tracked for toggle-on-click behavior).
    QPointer<QWidget> _topBarMenu;
    QDateTime _topBarMenuClosedAt;

    HistoryReturnStack _returnStack;

    void pushReplyReturn(const QString &eventId);
    void popReplyReturn();
    void pushReturnPosition();
    void popReturnPosition();

    void openResolvedFile(const QString &mxcUrl, const QString &filename, const QString &mime);
    void openMediaViewAtEvent(const QString &eventId);

    // Repaint / relayout helpers that cover BOTH message lists (timeline +
    // pinned section), so inbound bridge signals don't update only `_list`.
    void updateMessageLists();
    // Pick the list that currently owns an event (pinned section if visible and
    // it holds the event, otherwise the timeline) for playback dispatch.
    [[nodiscard]] HistoryList *listForEvent(const QString &eventId) const;
};

} // namespace TeleMatrix

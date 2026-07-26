// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QDateTime>
#include <QQueue>
#include <QSet>
#include <QVector>
#include <functional>

#include "ui/rp_widget.h"
#include "../protocol/protocol_types.h"

namespace Ui {
class InputField;
class ScrollArea;
} // namespace Ui
class QTimer;

namespace TeleMatrix {

class AppController;
class ProtocolBridge;
class ChatSearchIn;
class DialogsInner;
class MemberPickerBox;
class DialogsFilterSidebar;
class DialogsFoldersBox;
struct FolderManagerEntry;
struct RoomPickEntry;
class UnreadStateStore;

/// Container widget for the chat list sidebar.
/// Includes a search bar at top and a scrollable list of dialog rows.
class DialogsWidget : public Ui::RpWidget {
    Q_OBJECT

public:
    explicit DialogsWidget(
        QWidget *parent,
        AppController *controller,
        ProtocolBridge *bridge);

    /// Reload the room list from the protocol bridge.
    void refreshRooms();
    /// Apply cached rooms during startup and keep them stable until the
    /// initial dialogs load reaches Ready.
    void applyCachedRooms(const QVector<RoomSummary> &rooms);
    void applyRoomsResult(const QVector<RoomSummary> &rooms);
    /// Set or clear local draft preview for a room.
    void setRoomDraft(const QString &roomId, const QString &text);
    /// Optimistically update unread count for a room (from paint-driven read marking).
    void setRoomUnreadCount(const QString &roomId, int count);
    /// Focus chat-list search field and optionally set text.
    void focusSearch(const QString &query = QString());
    /// Focus search scoped to a specific room's messages.
    void focusSearchInChat(const QString &roomId, const QString &roomName, bool isDirect = false);
    /// Deselect the active chat row (used when Escape closes the chat).
    void clearSelection();
    /// Whether any dialogs search UI is currently active.
    [[nodiscard]] bool hasActiveSearch() const;
    /// Run the same Escape / cancel behavior as the search field.
    bool handleSearchEscape();

    // Callback for room selection.
    using RoomSelectedCallback = std::function<void(const QString &roomId)>;
    void setRoomSelectedCallback(RoomSelectedCallback callback);

    /// Programmatically select a room in the chat list by room ID.
    void selectRoomById(const QString &roomId);

    /// X offset of the chat list within this widget (the filters sidebar width),
    /// so the connecting indicator can sit at the chat list's left edge.
    [[nodiscard]] int chatListLeft() const;

protected:
    void resizeEvent(QResizeEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;

private:
    enum class MessageSearchMode {
        None,
        Room,
        MyMessages,
    };

    struct PendingPinRequest {
        QString roomId;
        bool pinned = false;
    };

    /// An in-flight folder-membership toggle. The optimistic change is re-applied
    /// on every rooms refresh (so a stale rebuild can't revert it) until either
    /// the server confirms AND the rebuilt cache reflects it, or it fails.
    struct PendingFolderChange {
        QString roomId;
        int filterId = 0;
        QString sectionKey;
        bool member = false;   // desired end-state (true = in folder)
        bool confirmed = false; // server accepted; stop the preloader for it
    };

    /// A new, unverified session awaiting its turn on the banner strip.
    struct PendingNewLogin {
        QString deviceId;
        QString displayName;
        QString lastSeenIp;
        qint64 lastSeenTs = 0;
    };

    void setupSearchBar();
    void setupVerificationBanner();
    void setupFilterSidebar();
    void setupScrollArea();
    void updateControlsGeometry();
    void showVerificationBanner(
        const QString &transactionId,
        const QString &deviceId,
        const QString &deviceName);
    void showUserVerificationBanner(
        const QString &flowId,
        const QString &userId,
        const QString &displayName);
    void hideVerificationBanner();
    void cancelPendingBannerRequest(const QString &replacingId);
    void setupNewLoginBanner();
    void showNextNewLoginBanner();
    void dismissNewLoginBanner();
    void deferNewLoginBanner();
    void hideNewLoginBanner();
    void showRoomFilterDropdown();
    void applyInitialDialogsLoadState(InitialDialogsLoadState state);
    void handleSyncSynced();
    void applyUnreadStateToRooms(QVector<RoomSummary> &rooms) const;
    void applyUnreadStateToRoom(const QString &roomId);
    void rebuildFilterCountersFromRows();
    // Unified sidebar order (custom folders + spaces interleaved), by token.
    void reorderEntriesInFilters(const QVector<SidebarEntry> &order);
    void applySidebarOrder(const QVector<SidebarEntry> &order);
    void requestSidebarOrder(const QVector<SidebarEntry> &order);
    void startSidebarOrderSave(const QVector<SidebarEntry> &order);
    void handleSidebarOrderSaved(bool success);
    // Lock the sidebar's drag-reorder and refresh the preloader bar while an
    // order save is in flight.
    void updateReorderLockState();
    // Adapter for the folders-manager box, which reorders folders only (by handle).
    void requestCustomFolderOrder(const QVector<int> &folderIds);
    // Folders manager popup (opened from the sidebar Edit button).
    void openFoldersManager();
    void promptCreateFolder(const QSet<QString> &preselected = {});
    void promptEditFolder(int filterId);
    void requestDeleteFolder(int filterId);
    void requestLeaveSpace(const QString &spaceId);
    // Open the space info/actions card (from the space tab's "Edit" menu item).
    void openSpaceInfo(const QString &spaceId);
    // Push the sidebar's active selection (folder OR space) into the inner list.
    void syncInnerActiveSelection();
    // Optimistically toggle a room's folder membership and track it until the
    // server confirms (re-applied on refresh so a rebuild can't revert it).
    void applyFolderMembership(const QString &roomId, int filterId,
        const QString &sectionKey, bool member);
    // Re-apply in-flight membership changes after a rooms rebuild; drop confirmed
    // ones once the cache reflects them.
    void reapplyPendingFolderChanges();
    // Show/hide the folder preloader based on in-flight folder work.
    void updateFolderLoadingBar();
    [[nodiscard]] bool hasPendingFolderWork() const;
    // Show a modal error for a failed folder operation.
    void showFolderError(const QString &message);
    // The durable `u.*` tag key for a custom folder's runtime handle (empty for
    // built-ins/spaces/unknown). Folder mutations are keyed on this, not filterId.
    QString sectionKeyForFilter(int filterId) const;
    void refreshOpenFoldersBox();
    QVector<FolderManagerEntry> buildFolderManagerEntries() const;
    QVector<RoomPickEntry> buildRoomEntries() const;
    QSet<QString> roomsInFolder(int filterId) const;

Q_SIGNALS:
    void mainMenuRequested();
    /// Forwarded from DialogsInner when a search result row is clicked.
    void searchResultClicked(const QString &roomId, const QString &eventId);
    /// Emitted when the in-chat search is fully cleared/closed.
    void searchInChatClosed();
    /// Emitted when the user accepts an incoming self-verification request.
    void verificationRequestAccepted(const QString &transactionId);
    /// Emitted when the user accepts an incoming cross-user verification request.
    void userVerificationRequestAccepted(const QString &flowId, const QString &displayName);
    /// New-login banner: sign the newly-appeared session out (opens the sessions
    /// page's confirm/password flow for this device).
    void signOutDeviceRequested(const QString &deviceId);
    /// Space tab → "Explore": open the room-directory box entered into the space.
    void exploreSpaceRequested(const QString &spaceId, const QString &name);
    /// Space tab → "Edit": open the room settings layer for the space (a space
    /// is an m.space room, so it reuses the standard Room Settings popup).
    void openRoomSettingsRequested(const QString &roomId);
private:
    AppController *_controller = nullptr;
    ProtocolBridge *_bridge = nullptr;
    UnreadStateStore *_unreadStateStore = nullptr;
    QTimer *_refreshRoomsTimer = nullptr;
    QTimer *_loadingSettleTimer = nullptr;
    QDateTime _startupTime = QDateTime::currentDateTime();

    Ui::InputField *_search = nullptr;  // Search/filter bar
    QWidget *_verificationBanner = nullptr;
    QWidget *_newLoginBanner = nullptr;
    DialogsFilterSidebar *_filterSidebar = nullptr; // Left filters sidebar
    DialogsFoldersBox *_openFoldersBox = nullptr;   // Folders manager popup while open
    Ui::ScrollArea *_scroll = nullptr;  // Scroll area containing the inner widget
    QWidget *_initialLoadingOverlay = nullptr;
    DialogsInner *_inner = nullptr;     // The actual row list
    QVector<FolderInfo> _filters;
    QQueue<PendingPinRequest> _pendingPinRequests;
    // In-flight folder-membership toggles (see PendingFolderChange).
    QList<PendingFolderChange> _pendingFolderChanges;
    // A folder-list op (create/rename/delete) is awaiting the server.
    bool _folderListOpInFlight = false;
    // Legacy pins carry no server-side order; theirs is published once per run.
    bool _pinnedOrderPublished = false;
    QVector<SidebarEntry> _optimisticSidebarOrder;
    QVector<SidebarEntry> _activeSidebarOrderSave;
    QVector<SidebarEntry> _queuedSidebarOrder;
    bool _hasOptimisticSidebarOrder = false;
    bool _folderOrderSaveInFlight = false;
    bool _folderOrderSaveQueued = false;
    // The server-synced unified order, applied to the rail on load so a dragged
    // folder/space interleaving survives restart and reaches other devices.
    QVector<SidebarEntry> _persistedSidebarOrder;
    bool _hasPersistedSidebarOrder = false;
    // Throttle the folder re-derive that rides the room-list debounce: each
    // rebuild is a per-room tag store scan, so cap it (new sections still appear
    // within the window). 0 = never fetched.
    qint64 _lastFolderRefetchMs = 0;
    void showFolderLoading();
    void hideFolderLoading();

    int _pendingDeleteFolderId = 0;
    int _pendingSelectFolderId = 0;
    QSet<QString> _pendingNewFolderRooms;
    QWidget *_folderLoadingBar = nullptr;
    QTimer *_searchTimer = nullptr;
    InitialDialogsLoadState _initialDialogsLoadState = InitialDialogsLoadState::NotStarted;
    bool _hasCachedStartupRooms = false;
    bool _deferInitialDialogsReadyUntilLoadingPaint = false;
    // Initial-load gate: the rooms list stays hidden behind the loading overlay
    // until the room list AND the whole sidebar (custom folders, joined spaces
    // and their saved order) have all loaded, so nothing pops in or re-sorts
    // after the list appears — everything shows at its final place at once.
    bool _customFoldersLoaded = false;
    bool _joinedSpacesLoaded = false;
    bool _sidebarOrderLoaded = false;
    bool _initialSidebarFetchStarted = false;
    bool _initialContentRevealed = false;
    QTimer *_initialRevealTimeout = nullptr;
    [[nodiscard]] bool initialSidebarDataReady() const {
        return _customFoldersLoaded && _joinedSpacesLoaded && _sidebarOrderLoaded;
    }
    // Fire the folder/space/order fetches once, the moment the room list is
    // ready — never earlier, so a cold start doesn't reveal an empty rail from
    // a pre-sync fetch and then re-sort when the real data lands.
    void startInitialSidebarFetch();
    // Hide the overlay + show the list, but only when everything is ready.
    void maybeRevealInitialContent();
    // Rebuild the rail (`_filters`) synchronously from the bridge caches +
    // saved order. Cheap and side-effect-free on the room list, so folder/space
    // signals can refresh the rail behind the loading overlay without a room
    // refetch (which is guarded during the cached-startup window).
    void rebuildSidebar();
    quint64 _latestRoomsRequestId = 0;
    bool _roomsRefreshInFlight = false;
    bool _roomsRefreshQueued = false;
    QString _verificationBannerTransactionId;
    bool _verificationBannerIsUser = false;
    QString _verificationBannerDisplayName;
    /// Queued new logins + the one currently on the strip (one banner at a time).
    QQueue<PendingNewLogin> _pendingNewLogins;
    PendingNewLogin _currentNewLogin;

    // Message-search context.
    MessageSearchMode _messageSearchMode = MessageSearchMode::None;
    ChatSearchIn *_chatSearchIn = nullptr;
    QString _searchInRoomId;
    QString _searchInRoomName;
    QString _searchInRoomAvatarUrl;
    bool _searchInRoomIsDirect = false;
    quint64 _searchRequestCounter = 0;
    quint64 _activeSearchRequestId = 0;
    QString _pendingMemberPickerRoomId;
    // Open member-picker box (shown immediately, populated when the async fetch lands) and the
    // members it was filled with (used to resolve the picked member's avatar).
    MemberPickerBox *_memberPicker = nullptr;
    QVector<UserProfile> _memberPickerMembers;

    // Pagination state (Phase 5).
    QString _searchNextToken;
    bool _searchDone = true;
    bool _searchLoadingMore = false;

    // Filter buttons (Phase 4).
    QWidget *_fromUserButton = nullptr;   // "From" person icon
    QString _searchSenderFilter;          // sender user ID
    QString _searchSenderName;            // sender display name

    [[nodiscard]] bool isMessageSearchActive() const {
        return _messageSearchMode != MessageSearchMode::None;
    }
    [[nodiscard]] bool isRoomMessageSearch() const {
        return _messageSearchMode == MessageSearchMode::Room;
    }
    [[nodiscard]] bool isDefaultSearchActive() const;
    [[nodiscard]] bool isServerSearchActive() const;

    void setupFilterButtons();
    void updateFilterButtonsVisibility();
    void ensureChatSearchIn();
    void updateChatSearchInPresentation();
    void showMemberPicker();
    void activateGlobalMyMessagesSearch();
    void clearSearchInChat();
    bool handleSearchCancelled();
    void cancelActiveSearchRequest();
    void resetSearchPaginationState(bool done = true);
    void performServerSearch(const QString &query);
    void loadMoreSearchResults();
};

} // namespace TeleMatrix

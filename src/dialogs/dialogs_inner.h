// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QHash>
#include <QMap>
#include <QPointer>
#include <QVariantAnimation>
#include <QVector>
#include <functional>

#include "ui/rp_widget.h"
#include "dialogs_row.h"

class QContextMenuEvent;
class QTimer;

namespace TeleMatrix::HistoryPopupMenuStyle {
class PopupMenu;
} // namespace TeleMatrix::HistoryPopupMenuStyle

namespace TeleMatrix {

/// Scrollable inner widget for the dialog list.
/// Paints all visible rows using custom painting (DialogsLayout).
/// Handles mouse interaction for selection and hover.
class DialogsInner : public Ui::RpWidget {
    Q_OBJECT

public:
    explicit DialogsInner(QWidget *parent = nullptr);

    /// Replace the row data with a fresh room list.
    void setRooms(const QVector<RoomSummary> &rooms);
    /// Number of rows in the list.
    int rowCount() const { return _rows.size(); }
    /// Room ID at a given index.
    QString roomIdAt(int index) const { return (index >= 0 && index < _rows.size()) ? _rows[index].roomId() : QString(); }
    /// Row data at a given index.
    const DialogsRow &roomAt(int index) const { return _rows[index]; }
    /// Set/clear local draft preview text for a room.
    void setDraft(const QString &roomId, const QString &text);
    /// Optimistically update unread count for a specific room.
    void setRoomUnreadCount(const QString &roomId, int count);
    /// Optimistically set or clear the marked-unread flag for a room.
    void setRoomMarkedUnread(const QString &roomId, bool marked);
    /// Set currently active folder filter (0 = all chats).
    void setActiveFilter(int filterId);
    void setActiveSpace(const QString &spaceId);
    const QString &activeSpaceId() const { return _activeSpaceId; }
    /// Room that presents as Saved Messages (bookmark userpic, no Leave entry).
    void setSavedMessagesRoomId(const QString &roomId);
    /// Set room-name search query (case-insensitive substring).
    void setSearchFilter(const QString &query);
    /// Set available folders for context-menu submenu.
    void setFolders(const QVector<FolderInfo> &folders);
    /// Toggle one folder assignment for a room in the local dialogs model.
    void toggleRoomFolder(const QString &roomId, int folderId);
    // Idempotent set of a room's membership in a folder (for optimistic re-apply).
    void setRoomInFolder(const QString &roomId, int folderId, bool member);
    // Whether a room currently carries the given folder handle (reads _allRows).
    [[nodiscard]] bool roomInFolder(const QString &roomId, int folderId) const;
    void setRoomHighlightCount(const QString &roomId, int count);
    /// Optimistically set pinned state for a room and re-sort rows.
    bool setRoomPinned(const QString &roomId, bool pinned);
    void updateOnlineStatus(const QString &userId, int state);
    void setRoomNotificationMode(const QString &roomId, RoomNotificationMode mode);
    /// Return ordered list of pinned room IDs (by pinnedIndex).
    [[nodiscard]] QVector<QString> pinnedRoomIds() const;
    /// Whether any pinned room has no `order` on the server yet (pinned by an older
    /// build). Those need their local order published once, or it dies with the
    /// settings file.
    [[nodiscard]] bool hasPinnedRoomsWithoutOrder() const;
    /// Apply saved pinned order from settings on startup.
    /// Order the pinned rooms by the server's `m.favourite` tag order, falling back
    /// to `savedIds` (the local list) for rooms the server holds no order for.
    void applyPinnedOrder(const QVector<QString> &savedIds);
    /// Apply saved room-to-folder assignments from settings on startup.
    void applySavedFolderAssignments(const QMap<QString, QVector<int>> &assignments);
    /// Access all rows (for reading folder assignments).
    [[nodiscard]] const QVector<DialogsRow> &allRows() const { return _allRows; }

    /// Set search results for in-room message search display.
    void setSearchResults(const SearchPage &page);
    /// Append additional search results (pagination).
    void appendSearchResults(const SearchPage &page);
    /// Clear search results and reset pagination state.
    void clearSearchResults();
    /// Toggle in-chat message search mode and reserve top space for the banner.
    void setMessageSearchMode(bool active, int topInset = 0);
    /// Toggle the startup dialogs loading state.
    void setInitialLoading(bool loading);
    /// Toggle the initial loading state for the current server-side message search.
    void setSearchLoading(bool loading);
    /// Update the active server-side message-search query for row highlighting.
    void setMessageSearchQuery(const QString &query);
    /// Toggle whether the current message-search scope spans all chats.
    void setMessageSearchGlobalScope(bool globalScope);
    /// Update the visible scroll viewport height for centered startup loading.
    void setViewportHeight(int height);
    /// Navigate to next/previous search result. Returns the selected index.
    int selectNextSearchResult();
    int selectPreviousSearchResult();
    /// Get current search result selection index (-1 if none).
    [[nodiscard]] int searchSelectedIndex() const { return _searchSelectedIndex; }
    /// Get total search results count.
    [[nodiscard]] int searchResultsCount() const { return _searchResults.size(); }
    /// Get approximate total from server.
    [[nodiscard]] int searchTotalApprox() const { return _searchTotalApprox; }
    /// Whether more search results can be loaded.
    [[nodiscard]] bool searchHasMore() const { return !_searchDone; }

    /// Get the currently selected room ID (empty if none).
    const QString &selectedRoomId() const { return _selectedRoomId; }
    /// Deselect the active row (used when Escape closes the chat).
    void clearSelection();

    // Callback for room selection (avoids needing full Qt signal outside QObject).
    using RoomSelectedCallback = std::function<void(const QString &roomId)>;
    void setRoomSelectedCallback(RoomSelectedCallback callback);

    /// Programmatically select a room by its room ID (without firing the callback).
    void selectRoomById(const QString &roomId);

signals:
    void pinRoomRequested(const QString &roomId, bool pinned);
    void roomNotificationModeRequested(const QString &roomId, RoomNotificationMode mode);
    void markReadRequested(const QString &roomId, bool read);
    void addToFolderRequested(const QString &roomId, int folderId);
    void createFolderRequested(const QString &roomId);
    void leaveRoomRequested(const QString &roomId);
    void deleteSavedMessagesRequested();
    void pinnedOrderChanged(const QVector<QString> &pinnedIds);
    void searchResultClicked(const QString &roomId, const QString &eventId);
    void loadMoreSearchResults();
    void initialLoadingPainted();

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;

private:
    int rowAtY(int y) const;
    [[nodiscard]] bool isDefaultSearchVisible() const;
    [[nodiscard]] int filteredRowsHeight() const;
    [[nodiscard]] int defaultSearchResultsTop() const;
    void setHoveredRow(int index);
    void setSelectedRow(int index);
    void updateRowRect(int index);
    void showContextMenu(int rowIndex, const QPoint &globalPos);
    void applyFilters();
    bool reapplyUnreadFilterIfNeeded();
    void updateContentSize();
    void paintDialogRows(QPainter &p, const QRect &clip);
    void paintSearchSection(QPainter &p, const QRect &clip, int contentTop, bool showAllChatsLabel);
    void paintInitialLoading(QPainter &p, const QRect &clip);
    void rebuildAllRowsIndex();

    QVector<DialogsRow> _allRows;
    QHash<QString, int> _allRowsByRoomId;
    QVector<DialogsRow> _rows;
    QHash<QString, QString> _drafts;
    QVector<FolderInfo> _folders;
    int _activeFilterId = 0;
    QString _activeSpaceId;
    QString _savedMessagesRoomId;
    QString _searchQuery;
    int _hoveredIndex = -1;
    int _selectedIndex = -1;
    int _menuRowIndex = -1;
    QString _selectedRoomId;
    QPointer<HistoryPopupMenuStyle::PopupMenu> _contextMenu;

    RoomSelectedCallback _roomSelectedCallback;

    // Search results mode (in-room message search).
    QVector<SearchHit> _searchResults;
    int _searchHoveredIndex = -1;
    int _searchSelectedIndex = -1;
    int _searchTotalApprox = 0;
    bool _searchDone = true;
    bool _messageSearchMode = false;
    int _messageSearchTopInset = 0;
    bool _initialLoading = false;
    bool _initialLoadingPaintedOnce = false;
    bool _searchLoading = false;
    bool _searchResolved = false;
    bool _searchE2eeDisabled = false; // last page was empty: E2EE search off
    bool _searchIndexing = false;     // E2EE room still building its search index
    QString _messageSearchQuery;
    bool _messageSearchGlobalScope = false;
    QTimer *_searchLoadingTimer = nullptr;
    int _viewportHeight = 0;
    int searchResultRowAtY(int y) const;
    void updateSearchResultsSize();

    QHash<QString, QVariantAnimation*> _onlineBadgeAnims;

    // Drag-to-reorder pinned chats.
    int _draggedIndex = -1;
    int _dragStartY = 0;
    int _dragCurrentY = 0;
    int _dropTargetIndex = -1;
    bool _dragStarted = false;
    QString _draggedRoomId;
    static constexpr int kDragThreshold = 5;
};

} // namespace TeleMatrix

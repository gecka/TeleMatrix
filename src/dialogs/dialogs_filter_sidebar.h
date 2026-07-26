// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QVector>
#include <QWidget>

#include "../protocol/protocol_types.h"

class QMouseEvent;
class QPainter;
class QTimer;

namespace TeleMatrix {

namespace HistoryPopupMenuStyle {
class PopupMenu;
} // namespace HistoryPopupMenuStyle

class DialogsFilterSidebar : public QWidget {
    Q_OBJECT

public:
    explicit DialogsFilterSidebar(QWidget *parent = nullptr);

    void setFilters(const QVector<FolderInfo> &filters);
    // Initial-load state: the rail shows ONLY its hamburger (no folder/space
    // buttons, no "Edit folders"), and ignores setFilters, until the dialogs
    // widget has everything ready and clears it. Prevents folders appearing +
    // re-sorting in the rail while the content area shows "Loading…".
    void setLoading(bool loading);
    void setActiveFilter(int filterId);
    void setActiveSpace(const QString &spaceId);
    // While a reorder is being saved, block starting a new drag (selection still
    // works). The rail shows a preloader elsewhere during this time.
    void setReorderLocked(bool locked);
    int activeFilterId() const { return _activeFilterId; }
    const QString &activeSpaceId() const { return _activeSpaceId; }
    bool hasMultipleFilters() const { return _buttons.size() > 1; }

    // The bottom "Edit folders" button stays hidden until the custom folder
    // list has been fetched at least once after startup.
    void setEditButtonVisible(bool visible);

signals:
    void filterSelected(int filterId);
    void spaceSelected(const QString &spaceId);
    void editFolderRequested(int filterId);
    void deleteFolderRequested(int filterId);
    // The full unified order of reorderable entries (custom folders + spaces)
    // after a drag, top to bottom.
    void sidebarReordered(const QVector<SidebarEntry> &order);
    // Space tab context menu.
    void editSpaceRequested(const QString &spaceId);
    void exploreSpaceRequested(const QString &spaceId, const QString &name);
    void leaveSpaceRequested(const QString &spaceId);
    void mainMenuRequested();
    // Bottom "Edit folders" button → open the Folders manager popup.
    void manageFoldersRequested();

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    struct Button {
        FolderInfo folder;
        QString label;
        QString iconName;
        QRect rect;
    };

    void recalcLayout();
    int buttonIndexAt(const QPoint &pos) const;
    void showContextMenu(int buttonIndex, const QPoint &globalPos);
    void drawIcon(
        QPainter &p,
        const QString &iconName,
        const QRect &rect,
        const QPoint &iconPosition,
        const QColor &color) const;

    // Drag-to-reorder helpers.
    struct DragShiftEntry {
        float currentShift = 0.f;
        int targetShift = 0;
    };
    void cancelDrag();
    void updateDragOrder();
    void paintButton(QPainter &p, int index, const QRect &paintRect,
        const QFont &textFont, const QFont &badgeFont) const;

    QVector<Button> _buttons;

    // Drag-to-reorder state (middle-point crossing, animated neighbor
    // shifts, raised button drawn on top).
    QVector<DragShiftEntry> _dragShifts;
    QTimer *_dragAnimTimer = nullptr;
    int _draggedButtonIndex = -1;
    int _dragStartY = 0;
    int _currentDesiredIndex = -1;
    bool _dragStarted = false;
    int _raisedButtonIndex = -1;

    QRect _mainMenuRect;
    QRect _editButtonRect;
    int _activeFilterId = 0;
    QString _activeSpaceId;
    int _hoveredButtonIndex = -1;
    bool _hoveredMainMenu = false;
    bool _hoveredEditButton = false;
    bool _editButtonVisible = false;
    bool _reorderLocked = false;
    bool _loading = false;
    HistoryPopupMenuStyle::PopupMenu *_contextMenu = nullptr;
};

} // namespace TeleMatrix

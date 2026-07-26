// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "dialogs_filter_sidebar.h"

#include <algorithm>
#include <QApplication>
#include <QCoreApplication>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>

#include "history/history_popup_menu_style.h"
#include "protocol/media_cache.h"
#include "styles/style_constants.h"
#include "ui/painter.h"
#include "ui/style/icon_provider.h"

namespace TeleMatrix {

namespace {

QString fallbackLabelForFilter(int filterId) {
    switch (filterId) {
    case 0:
        return QCoreApplication::translate("DialogsFilterSidebar", "All");
    case 1:
        return QCoreApplication::translate("DialogsFilterSidebar", "Personal");
    case 2:
        return QCoreApplication::translate("DialogsFilterSidebar", "Unread");
    default:
        return QCoreApplication::translate("DialogsFilterSidebar", "Folder");
    }
}

QString filterIconName(const FolderInfo &folder) {
    if (folder.filterId == 0) {
        return QStringLiteral("folders/folders_all");
    }
    if (folder.filterId == 1) {
        return QStringLiteral("folders/folders_private");
    }
    if (folder.filterId == 2) {
        return QStringLiteral("folders/folders_unread");
    }

    const auto text = folder.displayName.trimmed().toLower();
    if (text.contains(QStringLiteral("unread"))) {
        return QStringLiteral("folders/folders_unread");
    }
    if (text.contains(QStringLiteral("personal"))
        || text.contains(QStringLiteral("private"))
        || text.contains(QStringLiteral("contact"))) {
        return QStringLiteral("folders/folders_private");
    }
    if (text.contains(QStringLiteral("group"))) {
        return QStringLiteral("folders/folders_group");
    }
    if (text.contains(QStringLiteral("channel"))) {
        return QStringLiteral("folders/folders_channels");
    }
    if (text.contains(QStringLiteral("bot"))) {
        return QStringLiteral("folders/folders_bots");
    }
    if (text.contains(QStringLiteral("unmut"))) {
        return QStringLiteral("folders/folders_unmuted");
    }
    if (text.contains(QStringLiteral("setup"))
        || text.contains(QStringLiteral("settings"))) {
        return QStringLiteral("folders/folders_setup");
    }
    return QStringLiteral("folders/folders_custom");
}

// A space with no avatar of its own falls back to tdesktop's channels glyph
// (folders_channels) — the same icon tdesktop uses for the channels chat
// folder, which reads as "a collection of chats".
QString spaceIconName(const FolderInfo &) {
    return QStringLiteral("folders/folders_channels");
}

// Custom folders (filterId > 2) and spaces can be dragged to reorder; the
// built-in All/Personal/Unread filters are pinned at the top.
bool isReorderable(const FolderInfo &folder) {
    return folder.isSpace || folder.filterId > 2;
}
bool isPinnedBuiltin(const FolderInfo &folder) {
    return !folder.isSpace && folder.filterId <= 2;
}

} // namespace

DialogsFilterSidebar::DialogsFilterSidebar(QWidget *parent)
    : QWidget(parent) {
    setMouseTracking(true);
    setFixedWidth(st::sideBarWidth);

    _dragAnimTimer = new QTimer(this);
    _dragAnimTimer->setInterval(16); // ~60fps
    QObject::connect(_dragAnimTimer, &QTimer::timeout, this, [this] {
        bool anyActive = false;
        for (auto &entry : _dragShifts) {
            const auto diff = float(entry.targetShift) - entry.currentShift;
            if (qAbs(diff) > 0.5f) {
                entry.currentShift += diff * 0.3f;
                anyActive = true;
            } else {
                entry.currentShift = float(entry.targetShift);
            }
        }
        if (!anyActive) {
            _dragAnimTimer->stop();
        }
        update();
    });
}

void DialogsFilterSidebar::setFilters(const QVector<FolderInfo> &filters) {
    if (_loading) {
        // Ignored while the app is still loading — the rail shows only its
        // hamburger. The dialogs widget re-pushes the final list at reveal.
        return;
    }
    cancelDrag();
    _buttons.clear();
    _buttons.reserve(filters.size());

    for (const auto &folder : filters) {
        if (folder.filterId < 0) {
            continue;
        }

        auto resolved = folder;
        if (resolved.displayName.trimmed().isEmpty()) {
            resolved.displayName = fallbackLabelForFilter(resolved.filterId);
        }

        _buttons.push_back(Button{
            .folder = resolved,
            .label = resolved.displayName,
            .iconName = resolved.isSpace ? spaceIconName(resolved)
                                         : filterIconName(resolved),
            .rect = {},
        });
    }

    auto allIndex = -1;
    for (auto i = 0; i < _buttons.size(); ++i) {
        if (_buttons[i].folder.filterId == 0) {
            allIndex = i;
            break;
        }
    }
    if (allIndex < 0) {
        FolderInfo all;
        all.filterId = 0;
        all.displayName = fallbackLabelForFilter(0);
        _buttons.push_front(Button{
            .folder = all,
            .label = all.displayName,
            .iconName = QStringLiteral("folders/folders_all"),
            .rect = {},
        });
    } else if (allIndex > 0) {
        _buttons.move(allIndex, 0);
    }

    if (_buttons.isEmpty()) {
        FolderInfo all;
        all.filterId = 0;
        all.displayName = fallbackLabelForFilter(0);
        _buttons.push_back(Button{
            .folder = all,
            .label = all.displayName,
            .iconName = QStringLiteral("folders/folders_all"),
            .rect = {},
        });
    }

    auto found = false;
    for (const auto &button : _buttons) {
        if (!button.folder.isSpace && button.folder.filterId == _activeFilterId) {
            found = true;
            break;
        }
    }
    if (!found) {
        _activeFilterId = 0;
    }

    // Re-validate the active space: leaving a space (its tab vanishes) must not
    // strand a stale id filtering the list to nothing.
    if (!_activeSpaceId.isEmpty()) {
        auto spaceFound = false;
        for (const auto &button : _buttons) {
            if (button.folder.isSpace && button.folder.spaceId == _activeSpaceId) {
                spaceFound = true;
                break;
            }
        }
        if (!spaceFound) {
            _activeSpaceId.clear();
        }
    }

    recalcLayout();
    update();
}

void DialogsFilterSidebar::setActiveFilter(int filterId) {
    if (_activeFilterId == filterId && _activeSpaceId.isEmpty()) {
        return;
    }
    for (const auto &button : _buttons) {
        if (!button.folder.isSpace && button.folder.filterId == filterId) {
            _activeFilterId = filterId;
            _activeSpaceId.clear();
            update();
            return;
        }
    }
}

void DialogsFilterSidebar::setReorderLocked(bool locked) {
    if (_reorderLocked == locked) {
        return;
    }
    _reorderLocked = locked;
    if (locked && _draggedButtonIndex >= 0) {
        // A save started while a drag was somehow still pending — cancel it.
        cancelDrag();
    }
}

void DialogsFilterSidebar::setActiveSpace(const QString &spaceId) {
    if (_activeSpaceId == spaceId) {
        return;
    }
    for (const auto &button : _buttons) {
        if (button.folder.isSpace && button.folder.spaceId == spaceId) {
            _activeSpaceId = spaceId;
            update();
            return;
        }
    }
}

void DialogsFilterSidebar::setEditButtonVisible(bool visible) {
    if (_loading && visible) {
        return; // no controls but the hamburger while loading
    }
    if (_editButtonVisible == visible) {
        return;
    }
    _editButtonVisible = visible;
    recalcLayout();
    update();
}

void DialogsFilterSidebar::setLoading(bool loading) {
    if (_loading == loading) {
        return;
    }
    _loading = loading;
    if (loading) {
        // Strip the rail down to just its hamburger. setFilters is ignored until
        // the dialogs widget clears this and re-pushes the final list.
        cancelDrag();
        _buttons.clear();
        _editButtonVisible = false;
        recalcLayout();
        update();
    }
}

void DialogsFilterSidebar::paintButton(
        QPainter &p,
        int index,
        const QRect &paintRect,
        const QFont &textFont,
        const QFont &badgeFont) const {
    const auto &button = _buttons[index];
    const auto active = button.folder.isSpace
        ? (button.folder.spaceId == _activeSpaceId)
        : (_activeSpaceId.isEmpty() && button.folder.filterId == _activeFilterId);
    const auto isDragging = _dragStarted && _draggedButtonIndex >= 0;
    const auto hovered = !isDragging && index == _hoveredButtonIndex;

    // Fill background (needed for raised button to cover items underneath).
    p.fillRect(paintRect, st::sideBarBg);
    if (active) {
        p.fillRect(paintRect, st::sideBarBgActive);
    } else if (hovered) {
        p.fillRect(paintRect, st::sideBarBgRipple);
    }

    // A space with a server logo paints its (circular) avatar in the icon slot;
    // logo-less spaces and folders fall back to the tinted glyph.
    bool drewAvatar = false;
    if (button.folder.isSpace && !button.folder.avatarUrl.isEmpty()) {
        const auto dpr = p.device()->devicePixelRatioF();
        const int size = 34;
        const auto pix = MediaCache::loadAvatarPixmapAsync(
            button.folder.avatarUrl, size, dpr,
            const_cast<DialogsFilterSidebar *>(this));
        if (!pix.isNull()) {
            const int x = paintRect.left() + (paintRect.width() - size) / 2;
            const int y = paintRect.top() + st::sideBarIconPosition.y();
            p.drawPixmap(x, y, pix);
            drewAvatar = true;
        }
    }
    if (!drewAvatar) {
        const auto iconColor = active ? st::sideBarIconFgActive : st::sideBarIconFg;
        drawIcon(p, button.iconName, paintRect, st::sideBarIconPosition, iconColor);
    }

    p.setPen(active ? st::sideBarTextFgActive : st::sideBarTextFg);
    p.setFont(textFont);
    const auto labelRect = QRect(
        paintRect.left() + st::sideBarTextSkip,
        paintRect.top() + st::sideBarTextTop,
        paintRect.width() - (2 * st::sideBarTextSkip),
        textFont.pixelSize() + 4);
    const auto label = QFontMetrics(textFont).elidedText(
        button.label,
        Qt::ElideRight,
        labelRect.width());
    p.drawText(
        labelRect,
        Qt::AlignHCenter | Qt::AlignTop | Qt::TextSingleLine,
        label);

    if (button.folder.unreadCount <= 0) {
        return;
    }

    const auto badgeText = (button.folder.unreadCount > 999)
        ? QStringLiteral("99+")
        : QString::number(button.folder.unreadCount);
    p.setFont(badgeFont);
    const auto textWidth = QFontMetrics(badgeFont).horizontalAdvance(badgeText);
    const auto badgeWidth = qMax(
        st::sideBarBadgeHeight,
        textWidth + (2 * st::sideBarBadgeSkip));
    const auto desiredLeft = paintRect.left()
        + (paintRect.width() / 2)
        + st::sideBarBadgePosition.x();
    const auto badgeLeft = std::min(
        desiredLeft,
        paintRect.right() - badgeWidth - 2);
    const auto badgeTop = paintRect.top() + st::sideBarBadgePosition.y();
    const QRect badgeRect(badgeLeft, badgeTop, badgeWidth, st::sideBarBadgeHeight);
    const auto buttonBg = active ? st::sideBarBgActive : st::sideBarBg;
    const auto badgeColor = (button.folder.unreadMuted && !active)
        ? st::sideBarBadgeBgMuted
        : st::sideBarBadgeBg;
    {
        PainterHighQualityEnabler hq(p);
        p.setPen(QPen(buttonBg, 2.0 * st::sideBarBadgeStroke));
        p.setBrush(buttonBg);
        p.drawRoundedRect(
            badgeRect,
            st::sideBarBadgeHeight / 2.0,
            st::sideBarBadgeHeight / 2.0);
        p.setPen(Qt::NoPen);
        p.setBrush(badgeColor);
        p.drawRoundedRect(
            badgeRect,
            st::sideBarBadgeHeight / 2.0,
            st::sideBarBadgeHeight / 2.0);
    }
    p.setPen(st::sideBarBadgeFg);
    p.drawText(badgeRect, Qt::AlignCenter, badgeText);
}

void DialogsFilterSidebar::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), st::sideBarBg);

    if (_hoveredMainMenu) {
        p.fillRect(_mainMenuRect, st::sideBarBgRipple);
    }
    drawIcon(
        p,
        QStringLiteral("dialogs/dialogs_menu"),
        _mainMenuRect,
        st::sideBarMainMenuIconPosition,
        st::sideBarIconFg);

    const auto textFont = st::baseFont(st::sideBarTextFontSize, true);
    const auto badgeFont = st::baseFont(st::sideBarBadgeFontSize, true);
    const auto isDragging = _dragStarted && _draggedButtonIndex >= 0;

    // First pass: paint all buttons except the raised (dragged) one.
    for (auto i = 0; i < _buttons.size(); ++i) {
        if (isDragging && i == _raisedButtonIndex) {
            continue; // Paint raised button last (on top).
        }
        const auto shift = (i < _dragShifts.size())
            ? int(_dragShifts[i].currentShift)
            : 0;
        const auto paintRect = _buttons[i].rect.translated(0, shift);
        paintButton(p, i, paintRect, textFont, badgeFont);
    }

    // Second pass: paint raised button on top.
    if (isDragging && _raisedButtonIndex >= 0
        && _raisedButtonIndex < _buttons.size()) {
        const auto shift = (_raisedButtonIndex < _dragShifts.size())
            ? int(_dragShifts[_raisedButtonIndex].currentShift)
            : 0;
        const auto paintRect = _buttons[_raisedButtonIndex].rect.translated(0, shift);
        paintButton(p, _raisedButtonIndex, paintRect, textFont, badgeFont);
    }

    // Bottom "Edit folders" button — the folders_edit sliders glyph.
    // Click opens the Folders manager popup.
    if (!_editButtonRect.isEmpty()) {
        if (_hoveredEditButton) {
            p.fillRect(_editButtonRect, st::sideBarBgRipple);
        }
        drawIcon(
            p,
            QStringLiteral("folders/folders_edit"),
            _editButtonRect,
            st::sideBarIconPosition,
            st::sideBarIconFg);

        p.setPen(st::sideBarTextFg);
        p.setFont(textFont);
        const auto editLabelRect = QRect(
            _editButtonRect.left() + st::sideBarTextSkip,
            _editButtonRect.top() + st::sideBarTextTop,
            _editButtonRect.width() - (2 * st::sideBarTextSkip),
            textFont.pixelSize() + 4);
        p.drawText(editLabelRect, Qt::AlignHCenter | Qt::AlignTop, QCoreApplication::translate("DialogsFilterSidebar", "Edit"));
    }
}

void DialogsFilterSidebar::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    recalcLayout();
}

void DialogsFilterSidebar::mouseMoveEvent(QMouseEvent *e) {
    // Handle drag-to-reorder.
    if (_draggedButtonIndex >= 0 && (e->buttons() & Qt::LeftButton)) {
        const auto currentY = e->pos().y();
        if (!_dragStarted) {
            const auto delta = QApplication::startDragDistance();
            if (qAbs(currentY - _dragStartY) > delta) {
                _dragStarted = true;
                _raisedButtonIndex = _draggedButtonIndex;
                _currentDesiredIndex = _draggedButtonIndex;
                // Adjust start to remove the threshold gap.
                _dragStartY += (currentY > _dragStartY) ? delta : -delta;
                setCursor(Qt::ClosedHandCursor);
            } else {
                return;
            }
        }
        // Apply shift to dragged button directly (follows cursor).
        const auto shift = currentY - _dragStartY;
        if (_draggedButtonIndex < _dragShifts.size()) {
            _dragShifts[_draggedButtonIndex].currentShift = float(shift);
            _dragShifts[_draggedButtonIndex].targetShift = shift;
        }
        updateDragOrder();
        update();
        return;
    }

    const auto hovered = buttonIndexAt(e->pos());
    const auto hoveredMenu = _mainMenuRect.contains(e->pos());
    const auto hoveredEdit = !_editButtonRect.isEmpty()
        && _editButtonRect.contains(e->pos());
    if (hovered != _hoveredButtonIndex
        || hoveredMenu != _hoveredMainMenu
        || hoveredEdit != _hoveredEditButton) {
        _hoveredButtonIndex = hovered;
        _hoveredMainMenu = hoveredMenu;
        _hoveredEditButton = hoveredEdit;
        update();
    }
    setCursor((hovered >= 0 || hoveredMenu || hoveredEdit)
        ? Qt::PointingHandCursor
        : Qt::ArrowCursor);
    QWidget::mouseMoveEvent(e);
}

void DialogsFilterSidebar::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(e);
        return;
    }
    // Check hamburger / main-menu button first.
    if (!_mainMenuRect.isEmpty() && _mainMenuRect.contains(e->pos())) {
        emit mainMenuRequested();
        return;
    }
    // Check "Edit folders" button.
    if (!_editButtonRect.isEmpty() && _editButtonRect.contains(e->pos())) {
        emit manageFoldersRequested();
        return;
    }
    const auto index = buttonIndexAt(e->pos());
    if (index < 0 || index >= _buttons.size()) {
        return;
    }
    // Custom folders and spaces are draggable; selection happens on release when
    // it turns out to be a click, not a drag. Built-in filters stay fixed. While
    // a reorder save is in flight, dragging is blocked (selection still works).
    if (isReorderable(_buttons[index].folder) && !_reorderLocked) {
        _draggedButtonIndex = index;
        _dragStartY = e->pos().y();
        _dragStarted = false;
        _currentDesiredIndex = index;
        _raisedButtonIndex = -1;
        // Initialize per-button shift entries.
        _dragShifts.resize(_buttons.size());
        for (auto &entry : _dragShifts) {
            entry.currentShift = 0.f;
            entry.targetShift = 0;
        }
        return; // Don't select yet — wait for release or drag.
    }
    // Built-in filter, or a reorder-locked folder/space: select immediately.
    const auto &folder = _buttons[index].folder;
    if (folder.isSpace) {
        if (_activeSpaceId != folder.spaceId) {
            _activeSpaceId = folder.spaceId;
            emit spaceSelected(folder.spaceId);
            update();
        }
    } else if (_activeFilterId != folder.filterId || !_activeSpaceId.isEmpty()) {
        _activeFilterId = folder.filterId;
        _activeSpaceId.clear();
        emit filterSelected(folder.filterId);
        update();
    }
}

void DialogsFilterSidebar::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && _draggedButtonIndex >= 0) {
        const auto index = _draggedButtonIndex;
        const auto desired = _currentDesiredIndex;
        if (_dragStarted && desired >= 0 && desired != index) {
            // Reorder _buttons.
            auto moved = _buttons[index];
            _buttons.remove(index);
            const auto insertAt = qMin(desired, _buttons.size());
            _buttons.insert(insertAt, moved);
            recalcLayout();
            // Emit the full unified order of reorderable entries (folders + spaces).
            QVector<SidebarEntry> order;
            for (const auto &btn : _buttons) {
                if (btn.folder.isSpace) {
                    order.append(SidebarEntry{ true, btn.folder.spaceId });
                } else if (btn.folder.filterId > 2) {
                    order.append(SidebarEntry{ false, btn.folder.sectionKey });
                }
            }
            emit sidebarReordered(order);
        } else if (!_dragStarted) {
            // Was a click, not a drag — select this folder or space.
            const auto &folder = _buttons[index].folder;
            if (folder.isSpace) {
                if (_activeSpaceId != folder.spaceId) {
                    _activeSpaceId = folder.spaceId;
                    emit spaceSelected(folder.spaceId);
                }
            } else if (_activeFilterId != folder.filterId || !_activeSpaceId.isEmpty()) {
                _activeFilterId = folder.filterId;
                _activeSpaceId.clear();
                emit filterSelected(folder.filterId);
            }
        }
        cancelDrag();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

void DialogsFilterSidebar::contextMenuEvent(QContextMenuEvent *e) {
    // Cancel any pending drag before showing the context menu.
    if (_draggedButtonIndex >= 0) {
        cancelDrag();
    }
    const auto index = buttonIndexAt(e->pos());
    if (index < 0 || index >= _buttons.size()) {
        QWidget::contextMenuEvent(e);
        return;
    }
    showContextMenu(index, e->globalPos());
}

void DialogsFilterSidebar::showContextMenu(
        int buttonIndex,
        const QPoint &globalPos) {
    if (buttonIndex < 0 || buttonIndex >= _buttons.size()) {
        return;
    }
    const auto &button = _buttons[buttonIndex];
    const auto filterId = button.folder.filterId;
    const auto isSpace = button.folder.isSpace;

    // Built-in filters (All/Personal/Unread) have no actions.
    if (!isSpace && filterId <= 2) {
        return;
    }

    if (_contextMenu) {
        _contextMenu->deleteLater();
        _contextMenu = nullptr;
    }

    auto *menu = HistoryPopupMenuStyle::createStyledMenu(
        this,
        HistoryPopupMenuStyle::Variant::WithIcons);
    _contextMenu = menu;

    const auto addAction = [menu](
            const QString &label,
            const QString &icon,
            const std::function<void()> &slot,
            bool attention = false) {
        auto *action = menu->addAction(label);
        HistoryPopupMenuStyle::setActionIconName(action, icon);
        if (attention) {
            action->setProperty("_tm_attention", true);
        }
        if (slot) {
            QObject::connect(action, &QAction::triggered, menu, slot);
        }
        return action;
    };

    if (isSpace) {
        const auto spaceId = button.folder.spaceId;
        const auto name = button.folder.displayName;
        // "Edit" (space settings) intentionally omitted for now — the popup and
        // its editSpaceRequested → openSpaceInfo plumbing stay wired for later.
        addAction(
            tr("Explore"),
            QStringLiteral("explore"),
            [this, spaceId, name] { emit exploreSpaceRequested(spaceId, name); });
        menu->addSeparator();
        addAction(
            tr("Leave"),
            QStringLiteral("leave"),
            [this, spaceId] { emit leaveSpaceRequested(spaceId); },
            true /* attention */);
    } else {
        addAction(
            tr("Edit"),
            QStringLiteral("edit"),
            [this, filterId] { emit editFolderRequested(filterId); });
        menu->addSeparator();
        addAction(
            tr("Delete"),
            QStringLiteral("delete"),
            [this, filterId] { emit deleteFolderRequested(filterId); },
            true /* attention */);
    }

    menu->popup(globalPos);
}

void DialogsFilterSidebar::leaveEvent(QEvent *e) {
    if (_hoveredButtonIndex != -1 || _hoveredMainMenu || _hoveredEditButton) {
        _hoveredButtonIndex = -1;
        _hoveredMainMenu = false;
        _hoveredEditButton = false;
        update();
    }
    if (_draggedButtonIndex >= 0) {
        cancelDrag();
    }
    setCursor(Qt::ArrowCursor);
    QWidget::leaveEvent(e);
}

void DialogsFilterSidebar::cancelDrag() {
    _draggedButtonIndex = -1;
    _currentDesiredIndex = -1;
    _dragStarted = false;
    _raisedButtonIndex = -1;
    _dragAnimTimer->stop();
    _dragShifts.clear();
    // Restore the hover cursor rather than resetting to the arrow: a plain click ends a would-be
    // drag while the pointer is still over the folder, so it must stay a pointing hand.
    setCursor((_hoveredButtonIndex >= 0 || _hoveredMainMenu || _hoveredEditButton)
        ? Qt::PointingHandCursor
        : Qt::ArrowCursor);
    update();
}

void DialogsFilterSidebar::updateDragOrder() {
    if (_draggedButtonIndex < 0 || _draggedButtonIndex >= _buttons.size()) {
        return;
    }
    // Use middle-point crossing to determine desired index.
    // Neighbors shift by ±buttonHeight to create a visual gap.
    const auto shift = int(_dragShifts[_draggedButtonIndex].currentShift);
    const auto buttonHeight = st::sideBarButtonHeight;
    const auto draggedMiddle = _buttons[_draggedButtonIndex].rect.top()
        + shift
        + buttonHeight / 2;
    const auto count = _buttons.size();

    _currentDesiredIndex = _draggedButtonIndex;

    auto needsAnim = false;
    if (shift > 0) {
        // Dragging down: check buttons below.
        for (int next = _draggedButtonIndex + 1; next < count; ++next) {
            if (isPinnedBuiltin(_buttons[next].folder)) {
                break;
            }
            const auto &nb = _buttons[next];
            const auto nbMiddle = nb.rect.top() + buttonHeight / 2;
            if (draggedMiddle < nbMiddle) {
                if (_dragShifts[next].targetShift != 0) {
                    _dragShifts[next].targetShift = 0;
                    needsAnim = true;
                }
            } else {
                _currentDesiredIndex = next;
                if (_dragShifts[next].targetShift != -buttonHeight) {
                    _dragShifts[next].targetShift = -buttonHeight;
                    needsAnim = true;
                }
            }
        }
        // Reset shifts for buttons above dragged.
        for (int prev = _draggedButtonIndex - 1; prev >= 0; --prev) {
            if (_dragShifts[prev].targetShift != 0) {
                _dragShifts[prev].targetShift = 0;
                needsAnim = true;
            }
        }
    } else {
        // Dragging up: check buttons above.
        for (int prev = _draggedButtonIndex - 1; prev >= 0; --prev) {
            // Folders and spaces reorder freely, but never cross a built-in filter.
            if (isPinnedBuiltin(_buttons[prev].folder)) {
                break;
            }
            const auto &nb = _buttons[prev];
            const auto nbMiddle = nb.rect.top() + buttonHeight / 2;
            if (draggedMiddle >= nbMiddle) {
                if (_dragShifts[prev].targetShift != 0) {
                    _dragShifts[prev].targetShift = 0;
                    needsAnim = true;
                }
            } else {
                _currentDesiredIndex = prev;
                if (_dragShifts[prev].targetShift != buttonHeight) {
                    _dragShifts[prev].targetShift = buttonHeight;
                    needsAnim = true;
                }
            }
        }
        // Reset shifts for buttons below dragged.
        for (int next = _draggedButtonIndex + 1; next < count; ++next) {
            if (_dragShifts[next].targetShift != 0) {
                _dragShifts[next].targetShift = 0;
                needsAnim = true;
            }
        }
    }

    // Start animation timer if any neighbor needs to animate.
    if (needsAnim && !_dragAnimTimer->isActive()) {
        _dragAnimTimer->start();
    }
}

void DialogsFilterSidebar::recalcLayout() {
    _mainMenuRect = QRect(0, 0, width(), st::sideBarMainButtonHeight);
    auto y = st::sideBarMainButtonHeight;
    for (auto &button : _buttons) {
        button.rect = QRect(0, y, width(), st::sideBarButtonHeight);
        y += st::sideBarButtonHeight;
    }
    _editButtonRect = _editButtonVisible
        ? QRect(0, y, width(), st::sideBarButtonHeight)
        : QRect();
}

int DialogsFilterSidebar::buttonIndexAt(const QPoint &pos) const {
    for (auto i = 0; i < _buttons.size(); ++i) {
        if (_buttons[i].rect.contains(pos)) {
            return i;
        }
    }
    return -1;
}

void DialogsFilterSidebar::drawIcon(
        QPainter &p,
        const QString &iconName,
        const QRect &rect,
        const QPoint &iconPosition,
        const QColor &color) const {
    const auto icon = TeleMatrix::Style::IconProvider::tintedIcon(
        QStringLiteral(":/telematrix/icons/"),
        iconName,
        color);
    if (icon.isNull()) {
        return;
    }

    const auto iconSize = icon.size() / icon.devicePixelRatio();
    const auto x = (iconPosition.x() < 0)
        ? rect.left() + ((rect.width() - iconSize.width()) / 2)
        : rect.left() + iconPosition.x();
    const auto y = (iconPosition.y() < 0)
        ? rect.top() + ((rect.height() - iconSize.height()) / 2)
        : rect.top() + iconPosition.y();
    p.drawImage(QPoint(x, y), icon);
}

} // namespace TeleMatrix

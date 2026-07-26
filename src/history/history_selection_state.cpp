// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_selection_state.h"

#include <climits>
#include <utility>

namespace TeleMatrix {

bool HistorySelectionState::mousePressed() const {
    return _mousePressed;
}

void HistorySelectionState::setMousePressed(bool pressed) {
    _mousePressed = pressed;
}

QPoint HistorySelectionState::mousePressPos() const {
    return _mousePressPos;
}

void HistorySelectionState::setMousePressPos(QPoint pos) {
    _mousePressPos = pos;
}

bool HistorySelectionState::dragStarted() const {
    return _dragStarted;
}

void HistorySelectionState::setDragStarted(bool started) {
    _dragStarted = started;
}

void HistorySelectionState::resetDrag() {
    _mousePressed = false;
    _dragStarted = false;
}

int HistorySelectionState::updateClickCount(QPoint pos, qint64 now, int interval) {
    if ((pos - _lastClickPos).manhattanLength() <= 3
        && (now - _lastClickTime) < interval) {
        ++_clickCount;
    } else {
        _clickCount = 1;
    }
    _lastClickTime = now;
    _lastClickPos = pos;
    return _clickCount;
}

void HistorySelectionState::setDoubleClick(QPoint pos, qint64 now) {
    _clickCount = 2;
    _lastClickTime = now;
    _lastClickPos = pos;
}

bool HistorySelectionState::inSelectionMode() const {
    return _inSelectionMode;
}

void HistorySelectionState::enterSelectionMode(QSet<QString> selectedIds) {
    _inSelectionMode = true;
    _selectedMessageIds = std::move(selectedIds);
}

bool HistorySelectionState::exitSelectionMode() {
    if (!_inSelectionMode) {
        return false;
    }
    _inSelectionMode = false;
    _selectedMessageIds.clear();
    return true;
}

bool HistorySelectionState::selectedContains(const QString &eventId) const {
    return _selectedMessageIds.contains(eventId);
}

int HistorySelectionState::selectedCount() const {
    return _selectedMessageIds.size();
}

bool HistorySelectionState::selectedEmpty() const {
    return _selectedMessageIds.isEmpty();
}

bool HistorySelectionState::toggleSelected(const QString &eventId) {
    if (_selectedMessageIds.contains(eventId)) {
        _selectedMessageIds.remove(eventId);
    } else {
        _selectedMessageIds.insert(eventId);
    }
    return !_selectedMessageIds.isEmpty();
}

void HistorySelectionState::clearTextSelection() {
    _selFrom = {};
    _selTo = {};
    resetDrag();
}

void HistorySelectionState::setTextSelection(TextCursor from, TextCursor to) {
    _selFrom = from;
    _selTo = to;
}

void HistorySelectionState::selectWholeMessage(int messageIndex, int textLength) {
    _selFrom = { messageIndex, 0 };
    _selTo = { messageIndex, textLength };
    resetDrag();
}

void HistorySelectionState::setSelectionEnd(TextCursor to) {
    _selTo = to;
}

void HistorySelectionState::normalizedSelection(TextCursor &from, TextCursor &to) const {
    from = _selFrom;
    to = _selTo;
    if (!from.isValid() || !to.isValid()) {
        return;
    }
    if (from.messageIndex > to.messageIndex
        || (from.messageIndex == to.messageIndex
            && from.textPosition > to.textPosition)) {
        std::swap(from, to);
    }
}

bool HistorySelectionState::selectionForMessage(int msgIndex, int &start, int &end) const {
    TextCursor from, to;
    normalizedSelection(from, to);
    if (!from.isValid() || !to.isValid()) {
        return false;
    }
    if (msgIndex < from.messageIndex || msgIndex > to.messageIndex) {
        return false;
    }
    if (from.messageIndex == to.messageIndex) {
        if (from.textPosition == to.textPosition) {
            return false;
        }
        start = from.textPosition;
        end = to.textPosition;
        return true;
    }
    if (msgIndex == from.messageIndex) {
        start = from.textPosition;
        end = INT_MAX;
        return true;
    }
    if (msgIndex == to.messageIndex) {
        start = 0;
        end = to.textPosition;
        return true;
    }
    start = 0;
    end = INT_MAX;
    return true;
}

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QPoint>
#include <QSet>
#include <QString>
#include <QtGlobal>

namespace TeleMatrix {

/// A cursor position within the message list: message index + text offset.
struct TextCursor {
    int messageIndex = -1;
    int textPosition = -1;
    bool isValid() const { return messageIndex >= 0 && textPosition >= 0; }
};

class HistorySelectionState {
public:
    [[nodiscard]] bool mousePressed() const;
    void setMousePressed(bool pressed);

    [[nodiscard]] QPoint mousePressPos() const;
    void setMousePressPos(QPoint pos);

    [[nodiscard]] bool dragStarted() const;
    void setDragStarted(bool started);
    void resetDrag();

    int updateClickCount(QPoint pos, qint64 now, int interval);
    void setDoubleClick(QPoint pos, qint64 now);

    [[nodiscard]] bool inSelectionMode() const;
    void enterSelectionMode(QSet<QString> selectedIds);
    bool exitSelectionMode();

    [[nodiscard]] bool selectedContains(const QString &eventId) const;
    [[nodiscard]] int selectedCount() const;
    [[nodiscard]] bool selectedEmpty() const;
    bool toggleSelected(const QString &eventId);

    void clearTextSelection();
    void setTextSelection(TextCursor from, TextCursor to);
    void selectWholeMessage(int messageIndex, int textLength);
    void setSelectionEnd(TextCursor to);

    void normalizedSelection(TextCursor &from, TextCursor &to) const;
    bool selectionForMessage(int msgIndex, int &start, int &end) const;

private:
    bool _mousePressed = false;
    QPoint _mousePressPos;
    bool _dragStarted = false;
    TextCursor _selFrom;
    TextCursor _selTo;

    int _clickCount = 0;
    qint64 _lastClickTime = 0;
    QPoint _lastClickPos;

    bool _inSelectionMode = false;
    QSet<QString> _selectedMessageIds;
};

} // namespace TeleMatrix

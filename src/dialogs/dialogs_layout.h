// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QPainter>

class QWidget;

namespace TeleMatrix {

class DialogsRow;

/// Painting context passed to the row painter.
struct DialogsPaintContext {
    int width = 0;     // total width of the row
    bool active = false;  // is this the currently selected row
    bool selected = false; // is the mouse hovering over this row
    bool savedMessages = false; // draw the bookmark userpic instead of the avatar
    QWidget *repaintTarget = nullptr;
};

/// Static helper class that paints a single dialog row.
/// Core row painting using the pixel values from dialogs.style.
namespace DialogsLayout {

/// Paint a single row at position (0, 0) in the painter's coordinate system.
/// The caller is expected to translate the painter to the row's y-offset.
void paintRow(
    QPainter &p,
    DialogsRow &row,
    const DialogsPaintContext &context);

/// Paint the unread badge.
/// Returns the badge width (including padding) so the caller can adjust text layout.
int paintUnreadBadge(
    QPainter &p,
    int count,
    bool muted,
    int right,
    int top,
    bool active,
    bool selected);

} // namespace DialogsLayout

} // namespace TeleMatrix

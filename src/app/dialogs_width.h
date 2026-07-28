// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

namespace TeleMatrix::Dialogs {

// Pixel width to give the rooms-list column when restoring it at startup.
// `savedWidth` is the user's persisted choice (0 = never set → `defaultWidth`).
//
// The window width is deliberately NOT an input. The main widget is built and
// laid out inside a window that is still at its pre-maximize size, so the first
// resize it sees is narrow; a width derived from that (a stored ratio, say)
// pins the column to whatever that transient width implies — and because the
// column has stretch factor 0 it then never grows when the window maximizes.
// A pixel width survives that sequence: QSplitter keeps the requested size as
// its sizer, so even a request the current window is too narrow to honour
// re-expands to it once the window grows.
[[nodiscard]] int RestoredWidth(
    int savedWidth,
    int defaultWidth,
    int minWidth,
    int maxWidth);

} // namespace TeleMatrix::Dialogs

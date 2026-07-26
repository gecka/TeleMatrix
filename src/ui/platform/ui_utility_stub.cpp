// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "ui/platform/ui_utility_mac.h"

// Non-Apple fallbacks for the native window/cursor helpers. The macOS versions
// reach into Cocoa to force the cursor immediately (bypassing Qt's tracking
// areas) and to pin the window's colour space to sRGB. On Windows/Linux Qt's
// own per-widget cursor handling and colour management are sufficient, so these
// are deliberate no-ops. Crucially they must still exist: the call sites in
// history/, dialogs/ and media/ invoke them unconditionally.

namespace TeleMatrix::Platform {

void AcceptAllMouseInput(QWidget *widget) {
    // macOS makes the NSWindow accept mouse events even on transparent pixels;
    // Qt already delivers events to the widget on other platforms.
    (void)widget;
}

void ForcePointingHandCursor() {
    // No-op: the widget's own Qt cursor applies. We avoid touching the
    // QApplication override-cursor stack because these Force* calls have no
    // paired restore and would otherwise leak/stack cursors.
}

void ForceArrowCursor() {
}

void ForceIBeamCursor() {
}

void ForceWindowSRGB(QWidget *widget) {
    // macOS-only colour-space pinning; no equivalent needed elsewhere.
    (void)widget;
}

} // namespace TeleMatrix::Platform

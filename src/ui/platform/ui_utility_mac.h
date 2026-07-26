// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>

namespace TeleMatrix::Platform {

/// Ensures the window receives all mouse events, even on transparent pixels.
void AcceptAllMouseInput(QWidget *widget);

/// Bring the application to the foreground, defeating macOS focus-stealing
/// prevention (used to focus the running instance on a second launch).
void ActivateApp();

/// Force the system cursor immediately, bypassing Qt's tracking areas.
void ForcePointingHandCursor();
void ForceArrowCursor();
void ForceIBeamCursor();

/// Force the window to use sRGB color space so Qt's raster engine
/// output matches Preview.app / Chromium color management.
void ForceWindowSRGB(QWidget *widget);

} // namespace TeleMatrix::Platform

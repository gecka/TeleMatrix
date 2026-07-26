// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.
//
// Windows-only (see if(WIN32) in CMakeLists.txt). Authored on macOS; needs a
// Windows build to verify.

#include "window/notification_platform.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <shellapi.h>

namespace TeleMatrix::Notifications::Platform {

bool shouldSuppressAlerts() {
    QUERY_USER_NOTIFICATION_STATE state;
    if (FAILED(::SHQueryUserNotificationState(&state))) {
        return false;
    }
    switch (state) {
    case QUNS_NOT_PRESENT:            // user away / locked
    case QUNS_BUSY:                   // a full-screen app is running
    case QUNS_RUNNING_D3D_FULL_SCREEN: // fullscreen game
    case QUNS_PRESENTATION_MODE:      // presentation mode
    case QUNS_QUIET_TIME:             // quiet hours / focus assist
        return true;
    default:                          // QUNS_ACCEPTS_NOTIFICATIONS
        return false;
    }
}

} // namespace TeleMatrix::Notifications::Platform

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "window/notification_platform.h"

#import <CoreGraphics/CoreGraphics.h>

namespace TeleMatrix::Notifications::Platform {

bool shouldSuppressAlerts() {
    // macOS exposes no public Do-Not-Disturb / Focus query (the OS itself
    // withholds the toast under Focus). Screen-lock is the state where we
    // additionally mute our own sound/flash.
    bool locked = false;
    if (CFDictionaryRef session = CGSessionCopyCurrentDictionary()) {
        const void *value =
            CFDictionaryGetValue(session, CFSTR("CGSSessionScreenIsLocked"));
        locked = (value == kCFBooleanTrue);
        CFRelease(session);
    }
    return locked;
}

} // namespace TeleMatrix::Notifications::Platform

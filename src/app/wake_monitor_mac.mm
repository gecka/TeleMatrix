// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "app/wake_monitor.h"

#include <Cocoa/Cocoa.h>

namespace TeleMatrix {

WakeMonitor::WakeMonitor(QObject *parent) : QObject(parent) {
    @autoreleasepool {
        // The block runs on the main thread (NSWorkspace posts there), which is
        // also Qt's, so emitting directly is safe. No ARC here, hence the
        // explicit retain to keep the observer token alive past this scope.
        id observer = [[[NSWorkspace sharedWorkspace] notificationCenter]
            addObserverForName:NSWorkspaceDidWakeNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification *) {
                        Q_EMIT woke();
                    }];
        _impl = (void *)[observer retain];
    }
}

WakeMonitor::~WakeMonitor() {
    if (!_impl) {
        return;
    }
    @autoreleasepool {
        id observer = (id)_impl;
        _impl = nullptr;
        [[[NSWorkspace sharedWorkspace] notificationCenter] removeObserver:observer];
        [observer release];
    }
}

// logind-only; NSWorkspace posts the resume directly.
void WakeMonitor::handleSleepStateChanged(bool) {}

} // namespace TeleMatrix

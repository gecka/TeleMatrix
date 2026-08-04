// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.
//
// Platforms with no wake notification wired up (Windows). `woke()` never fires,
// so reconnect there still relies on NetworkMonitor alone, as before.

#include "app/wake_monitor.h"

namespace TeleMatrix {

WakeMonitor::WakeMonitor(QObject *parent) : QObject(parent) {}

WakeMonitor::~WakeMonitor() = default;

void WakeMonitor::handleSleepStateChanged(bool) {}

} // namespace TeleMatrix

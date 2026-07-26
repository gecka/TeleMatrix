// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

namespace TeleMatrix::Notifications::Platform {

/// True when the OS is in a state where we should NOT fire our own
/// (OS-bypassing) sound/flash alerts — screen locked, Do-Not-Disturb,
/// fullscreen, or presentation mode. The visual toast itself is left to the OS,
/// which applies its own suppression (i.e. suppress custom sound and
/// flash/bounce, not the visual notification). Platform-specific
/// implementations live in notification_platform_{mac,win,linux}.
[[nodiscard]] bool shouldSuppressAlerts();

} // namespace TeleMatrix::Notifications::Platform

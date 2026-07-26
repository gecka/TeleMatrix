// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QString>

namespace TeleMatrix::Notifications {

// WinRT toast `Tag`/`Group` are capped at 64 chars (16 on Windows 8.1), while
// Matrix room/event ids are routinely longer. These derive a stable 16-hex key
// from an id so `showNotification` and `clearFromRoom` produce the *same* key
// without a lookup table. Pure (no WinRT) so it is unit-testable on any platform.
//
// Uses FNV-1a/64 over the UTF-8 bytes — fully deterministic across runs and
// platforms, unlike qHash (which is per-process seeded).

/// Per-room key (WinRT toast `Group`) — collapses a room's toasts into one stack
/// and is the handle `clearFromRoom` removes by.
[[nodiscard]] QString toastGroupKey(const QString &roomId);

/// Per-event key (WinRT toast `Tag`) — idempotent identity for a single message.
[[nodiscard]] QString toastTagKey(const QString &eventId);

} // namespace TeleMatrix::Notifications

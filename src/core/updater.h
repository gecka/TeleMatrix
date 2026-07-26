// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QString>

namespace TeleMatrix {
namespace Core {

/// Platform half of the updater: decides whether this install can replace itself
/// and, once a verified payload exists, performs the swap.
///
/// Everything here runs *after* Rust has verified the signature — these
/// functions must never be handed an unverified path.
namespace Updater {

/// Whether this install can update itself in place.
///
/// False for deb/rpm (system dirs need root), a macOS bundle that is
/// translocated / running from a mounted DMG / sitting somewhere unwritable, and
/// an AppImage started without `$APPIMAGE` (an extracted run). Those all degrade
/// to opening the release page.
[[nodiscard]] bool CanSelfUpdate(QString *reason = nullptr);

struct ApplyResult {
    bool ok = false;
    QString error;
    /// Non-empty when the swap already happened and the caller must relaunch
    /// this path itself (AppImage). Empty on success means a detached helper
    /// owns the relaunch and the caller only has to quit.
    QString relaunchPath;
};

/// Stage the verified payload and hand off. On success the app must exit
/// promptly — on macOS/Windows a helper process is already waiting on this PID.
[[nodiscard]] ApplyResult Apply(const QString &localPath);

} // namespace Updater
} // namespace Core
} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

// Windows application identity for WinRT toast notifications. Windows will only
// deliver toasts for a desktop (unpackaged) app whose process AUMID matches an
// installed Start-Menu shortcut carrying the same AUMID — otherwise toasts
// silently never appear. Compiled on Windows only.

namespace TeleMatrix::Platform::Win {

// Stable Application User Model ID. Must match the value stamped into the
// Start-Menu shortcut by ensureStartMenuShortcut(); both the macManager bundle
// id and this share the reverse-DNS form for consistency.
inline constexpr wchar_t kAppUserModelId[] = L"dev.telematrix.TeleMatrix";

/// Set the AUMID on the current process. Call once, as early as possible at
/// startup (before the main window is shown).
void setAppUserModelId();

/// Create the Start-Menu shortcut (stamped with kAppUserModelId) if it is
/// missing. Returns true if the shortcut exists (or was created) so toasts can
/// be delivered. Cheap no-op when the shortcut already exists.
bool ensureStartMenuShortcut();

} // namespace TeleMatrix::Platform::Win

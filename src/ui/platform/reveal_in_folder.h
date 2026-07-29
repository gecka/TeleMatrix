// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

class QString;

namespace TeleMatrix::Platform {

/// Opens the system file manager with `filepath` selected: Finder on macOS,
/// Explorer on Windows, the org.freedesktop.FileManager1 handler on Linux
/// (falling back to just opening the containing folder).
void RevealInFolder(const QString &filepath);

} // namespace TeleMatrix::Platform

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QString>

namespace TeleMatrix {

[[nodiscard]] bool IsSafeExternalUrl(const QString &url);
bool OpenSafeExternalUrl(const QString &url);

} // namespace TeleMatrix

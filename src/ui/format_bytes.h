// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QString>

namespace TeleMatrix {

// Human-readable byte count, binary units: "512 B", "1.4 MB", "2.0 GB".
// Whole bytes print without a fraction; every larger unit gets one decimal.
[[nodiscard]] QString formatBytes(quint64 size);

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "app/forced_sign_out.h"

namespace TeleMatrix {

ForcedSignOutRoute RouteForcedSignOut(
        int accountIndex,
        int activeIndex,
        bool alreadyTearingDown) {
    if (accountIndex < 0 || alreadyTearingDown) {
        return ForcedSignOutRoute::Ignore;
    }
    return (accountIndex == activeIndex)
        ? ForcedSignOutRoute::Active
        : ForcedSignOutRoute::Background;
}

} // namespace TeleMatrix

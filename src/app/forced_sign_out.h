// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

namespace TeleMatrix {

// What to do when the homeserver tells us one account's access token is dead.
enum class ForcedSignOutRoute {
    // Nothing to do: the account is already gone, or its teardown is underway.
    Ignore,
    // It is the account on screen — sign it out through the interactive path
    // (explanatory dialog, then the normal logout handover).
    Active,
    // It is a background account — notify and sign it out in place, without
    // interrupting whichever account the user is actually using.
    Background,
};

// `accountIndex` is where the signalling account sits now, or -1 if it is no
// longer in the domain. It is resolved from the directory name captured when
// the signal was wired, never from "whichever account is active": these arrive
// asynchronously and the user can switch accounts in between, and filing one
// homeserver's answer against another account would sign out the wrong one.
//
// `alreadyTearingDown` covers the repeat: every in-flight request against a
// dead token rejects, and a logout already in progress must not be restarted.
[[nodiscard]] ForcedSignOutRoute RouteForcedSignOut(
    int accountIndex,
    int activeIndex,
    bool alreadyTearingDown);

} // namespace TeleMatrix

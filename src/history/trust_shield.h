// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "../protocol/protocol_types.h"

#include <QColor>
#include <QString>

class QPainter;
class QRect;

namespace TeleMatrix {

/// Short text label describing a user's cross-signing trust, for an inline
/// badge next to their name (member lists, profile popup) in place of the
/// shield. Empty for Unverified. "verified" / "unverified sessions" /
/// "identity changed".
[[nodiscard]] QString trustBadgeText(UserTrustState state);

/// Background colour for the trust badge painted with `trustBadgeText`.
[[nodiscard]] QColor trustBadgeColor(UserTrustState state);

/// Whether a trust shield is drawn for `state`. Only the exact Verified and
/// Violation discriminants render; Unverified and any out-of-range value (the
/// state crosses the FFI boundary as a raw int) render nothing, so a bad value
/// can never surface as a false green "verified" badge. Inline + Qt-free so it
/// is unit-testable without linking the painting code.
[[nodiscard]] inline bool trustShieldVisible(UserTrustState state) {
    return state == UserTrustState::Verified
        || state == UserTrustState::Violation
        || state == UserTrustState::VerifiedWithWarning;
}

/// Paint a small cross-signing trust shield glyph filling `r` in the semantic
/// colour: a green check for Verified, an amber "!" for the unverified-sessions
/// warning, a red "!" for a violation, nothing for Unverified.
void paintTrustShield(QPainter &p, const QRect &r, UserTrustState state);

/// One-line human description of a trust state, for a hover tooltip. `isRoom`
/// selects group-room wording ("Some users…") over the per-user DM wording.
[[nodiscard]] QString trustDescription(UserTrustState state, bool isRoom);

} // namespace TeleMatrix

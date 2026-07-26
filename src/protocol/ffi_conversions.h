// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "protocol_types.h"        // RoomDirectoryJoinRule, RoomMembership
#include "timeline_item_content.h" // ContentType, SendState, PreviewType

#include <cstdint>

namespace TeleMatrix {

// Map the flat FFI discriminants (which mirror the Rust `#[repr(u32)]` enums)
// onto the C++ enums. These are pure free functions deliberately kept out of
// protocol_bridge.cpp (which pulls the generated FFI header and media cache) so
// the Rust<->C++ discriminant contract is unit-testable without the Rust runtime.
//
// The discriminants are NOT a dense 0..N range — `ContentType` skips 6 and
// `SendState`'s default is `Read`, not `Sent` — so the exact mapping matters and
// is what these tests pin.
[[nodiscard]] ContentType convertFfiContentType(uint32_t rawType);
[[nodiscard]] PreviewType convertFfiPreviewType(uint32_t rawType);
[[nodiscard]] SendState convertFfiSendState(uint32_t rawState);
[[nodiscard]] RoomDirectoryJoinRule convertFfiRoomJoinRule(uint32_t rawRule);
[[nodiscard]] RoomMembership convertFfiRoomMembership(uint32_t rawMembership);

} // namespace TeleMatrix

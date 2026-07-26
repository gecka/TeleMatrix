// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "ffi_conversions.h"

namespace TeleMatrix {

ContentType convertFfiContentType(uint32_t rawType) {
    switch (rawType) {
    case 1:
        return ContentType::Image;
    case 2:
        return ContentType::File;
    case 3:
        return ContentType::Video;
    case 4:
        return ContentType::Service;
    case 5:
        return ContentType::Poll;
    case 7:
        return ContentType::Audio;
    case 8:
        return ContentType::UnableToDecrypt;
    default:
        return ContentType::Text;
    }
}

PreviewType convertFfiPreviewType(uint32_t rawType) {
    switch (rawType) {
    case 1:
        return PreviewType::Article;
    case 2:
        return PreviewType::Photo;
    case 3:
        return PreviewType::Video;
    case 4:
        return PreviewType::Document;
    case 5:
        return PreviewType::Profile;
    case 6:
        return PreviewType::Group;
    case 7:
        return PreviewType::Channel;
    default:
        return PreviewType::None;
    }
}

SendState convertFfiSendState(uint32_t rawState) {
    switch (rawState) {
    case 0:
        return SendState::Sending;
    case 1:
        return SendState::Sent;
    case 3:
        return SendState::Failed;
    case 2:
    default:
        return SendState::Read;
    }
}

RoomDirectoryJoinRule convertFfiRoomJoinRule(uint32_t rawRule) {
    switch (rawRule) {
    case 0:
        return RoomDirectoryJoinRule::Public;
    case 1:
        return RoomDirectoryJoinRule::Knock;
    case 2:
        return RoomDirectoryJoinRule::Invite;
    case 3:
        return RoomDirectoryJoinRule::Restricted;
    case 4:
        return RoomDirectoryJoinRule::KnockRestricted;
    case 5:
        return RoomDirectoryJoinRule::Private;
    default:
        return RoomDirectoryJoinRule::Unknown;
    }
}

RoomMembership convertFfiRoomMembership(uint32_t rawMembership) {
    switch (rawMembership) {
    case 1:
        return RoomMembership::Invited;
    case 2:
        return RoomMembership::Joined;
    case 3:
        return RoomMembership::Left;
    case 4:
        return RoomMembership::Knocked;
    case 5:
        return RoomMembership::Banned;
    default:
        return RoomMembership::None;
    }
}

} // namespace TeleMatrix

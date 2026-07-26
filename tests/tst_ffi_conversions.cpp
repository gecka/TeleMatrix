// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "protocol/ffi_conversions.h"

using namespace TeleMatrix;

// These pin the Rust<->C++ discriminant contract: the FFI sends a u32 whose
// value must map to the right C++ enum. The mappings are deliberately NOT a
// dense 0..N range, so the exact values matter.
class TestFfiConversions : public QObject {
    Q_OBJECT

private slots:
    void contentTypeMapping() {
        QCOMPARE(convertFfiContentType(0), ContentType::Text);
        QCOMPARE(convertFfiContentType(1), ContentType::Image);
        QCOMPARE(convertFfiContentType(2), ContentType::File);
        QCOMPARE(convertFfiContentType(3), ContentType::Video);
        QCOMPARE(convertFfiContentType(4), ContentType::Service);
        QCOMPARE(convertFfiContentType(5), ContentType::Poll);
        QCOMPARE(convertFfiContentType(7), ContentType::Audio);
        QCOMPARE(convertFfiContentType(8), ContentType::UnableToDecrypt);
        // 6 is a hole in the discriminant space; unknown -> Text fallback.
        QCOMPARE(convertFfiContentType(6), ContentType::Text);
        QCOMPARE(convertFfiContentType(999), ContentType::Text);
    }

    void previewTypeMapping() {
        QCOMPARE(convertFfiPreviewType(0), PreviewType::None);
        QCOMPARE(convertFfiPreviewType(1), PreviewType::Article);
        QCOMPARE(convertFfiPreviewType(2), PreviewType::Photo);
        QCOMPARE(convertFfiPreviewType(3), PreviewType::Video);
        QCOMPARE(convertFfiPreviewType(4), PreviewType::Document);
        QCOMPARE(convertFfiPreviewType(5), PreviewType::Profile);
        QCOMPARE(convertFfiPreviewType(6), PreviewType::Group);
        QCOMPARE(convertFfiPreviewType(7), PreviewType::Channel);
        // Unknown -> None.
        QCOMPARE(convertFfiPreviewType(8), PreviewType::None);
    }

    void sendStateMapping() {
        QCOMPARE(convertFfiSendState(0), SendState::Sending);
        QCOMPARE(convertFfiSendState(1), SendState::Sent);
        QCOMPARE(convertFfiSendState(2), SendState::Read);
        QCOMPARE(convertFfiSendState(3), SendState::Failed);
        // Note: the default for an unknown send-state is Read (not Sent).
        QCOMPARE(convertFfiSendState(99), SendState::Read);
    }

    void roomJoinRuleMapping() {
        QCOMPARE(convertFfiRoomJoinRule(0), RoomDirectoryJoinRule::Public);
        QCOMPARE(convertFfiRoomJoinRule(1), RoomDirectoryJoinRule::Knock);
        QCOMPARE(convertFfiRoomJoinRule(2), RoomDirectoryJoinRule::Invite);
        QCOMPARE(convertFfiRoomJoinRule(3), RoomDirectoryJoinRule::Restricted);
        QCOMPARE(convertFfiRoomJoinRule(4), RoomDirectoryJoinRule::KnockRestricted);
        QCOMPARE(convertFfiRoomJoinRule(5), RoomDirectoryJoinRule::Private);
        // ruma's JoinRuleKind is #[non_exhaustive], so an unknown rule must degrade, not crash.
        QCOMPARE(convertFfiRoomJoinRule(6), RoomDirectoryJoinRule::Unknown);
        QCOMPARE(convertFfiRoomJoinRule(99), RoomDirectoryJoinRule::Unknown);
    }

    void roomMembershipMapping() {
        QCOMPARE(convertFfiRoomMembership(0), RoomMembership::None);
        QCOMPARE(convertFfiRoomMembership(1), RoomMembership::Invited);
        QCOMPARE(convertFfiRoomMembership(2), RoomMembership::Joined);
        QCOMPARE(convertFfiRoomMembership(3), RoomMembership::Left);
        QCOMPARE(convertFfiRoomMembership(4), RoomMembership::Knocked);
        QCOMPARE(convertFfiRoomMembership(5), RoomMembership::Banned);
        QCOMPARE(convertFfiRoomMembership(99), RoomMembership::None);
    }
};

QTEST_GUILESS_MAIN(TestFfiConversions)
#include "tst_ffi_conversions.moc"

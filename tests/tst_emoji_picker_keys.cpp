// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest/QtTest>

#include "history/emoji_picker_keys.h"

using namespace TeleMatrix;

class TestEmojiPickerKeys : public QObject {
    Q_OBJECT
private slots:
    // Bare modifier presses reach the popup only because the focused search
    // field ignored them — closing the panel on those is the bug.
    void keepsPanelOnBareModifiers() {
        QVERIFY(!EmojiPickerKeys::ShouldForwardToComposer(Qt::Key_Shift));
        QVERIFY(!EmojiPickerKeys::ShouldForwardToComposer(Qt::Key_Control));
        QVERIFY(!EmojiPickerKeys::ShouldForwardToComposer(Qt::Key_Meta));
        QVERIFY(!EmojiPickerKeys::ShouldForwardToComposer(Qt::Key_Alt));
        QVERIFY(!EmojiPickerKeys::ShouldForwardToComposer(Qt::Key_AltGr));
    }
    void keepsPanelOnLockKeys() {
        QVERIFY(!EmojiPickerKeys::ShouldForwardToComposer(Qt::Key_CapsLock));
        QVERIFY(!EmojiPickerKeys::ShouldForwardToComposer(Qt::Key_NumLock));
        QVERIFY(!EmojiPickerKeys::ShouldForwardToComposer(Qt::Key_ScrollLock));
    }
    void keepsPanelOnPlatformSuperKeys() {
        QVERIFY(!EmojiPickerKeys::ShouldForwardToComposer(Qt::Key_Super_L));
        QVERIFY(!EmojiPickerKeys::ShouldForwardToComposer(Qt::Key_Super_R));
        QVERIFY(!EmojiPickerKeys::ShouldForwardToComposer(Qt::Key_Hyper_L));
        QVERIFY(!EmojiPickerKeys::ShouldForwardToComposer(Qt::Key_Hyper_R));
        QVERIFY(!EmojiPickerKeys::ShouldForwardToComposer(Qt::Key_Mode_switch));
    }
    // Escape is the picker's own close key, handled a layer down.
    void leavesEscapeToThePicker() {
        QVERIFY(!EmojiPickerKeys::ShouldForwardToComposer(Qt::Key_Escape));
    }
    void forwardsTypedCharacters() {
        QVERIFY(EmojiPickerKeys::ShouldForwardToComposer(Qt::Key_A));
        QVERIFY(EmojiPickerKeys::ShouldForwardToComposer(Qt::Key_Space));
        QVERIFY(EmojiPickerKeys::ShouldForwardToComposer(Qt::Key_1));
        QVERIFY(EmojiPickerKeys::ShouldForwardToComposer(Qt::Key_Backspace));
    }
    void forwardsSubmitKeys() {
        QVERIFY(EmojiPickerKeys::ShouldForwardToComposer(Qt::Key_Return));
        QVERIFY(EmojiPickerKeys::ShouldForwardToComposer(Qt::Key_Enter));
    }
};

QTEST_MAIN(TestEmojiPickerKeys)
#include "tst_emoji_picker_keys.moc"

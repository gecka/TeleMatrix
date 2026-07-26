// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>

#include "../protocol/protocol_types.h"

class QKeyEvent;
class QLabel;
class QMouseEvent;
class QPaintEvent;
class QVBoxLayout;
class QVariantAnimation;

namespace Ui {
class TextButton;
} // namespace Ui

namespace TeleMatrix {

class ProtocolBridge;

/// A read-only card of room details, shown for a room before joining it (there is no member
/// context to change anything, so nothing here is editable). Opened from the preview top bar.
/// The Saved Messages room reuses the same card (forSavedMessages) with simplified content.
///
/// Fire-and-forget: it deletes itself when dismissed (Escape, outside click, or Close).
class DialogsRoomInfoBox final : public QWidget {
    Q_OBJECT

public:
    DialogsRoomInfoBox(
        const RoomPreviewInfo &info,
        ProtocolBridge *bridge,
        QWidget *parent = nullptr);

    /// Same card and chrome, simplified read-only content: bookmark userpic,
    /// title, one line per property (encryption, access, room id with a copy
    /// glyph) and Close. Actions stay in the room's quick menu.
    static DialogsRoomInfoBox *forSavedMessages(
        const QString &roomId,
        bool encrypted,
        QWidget *parent);

    void showAnimated();

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    struct SavedMessagesTag {};

    DialogsRoomInfoBox(
        SavedMessagesTag,
        const QString &roomId,
        bool encrypted,
        QWidget *parent);

    /// Overlay geometry, fade animations, rounded panel — everything both
    /// variants share. Returns the panel layout the content goes into.
    QVBoxLayout *buildChrome();

    void closeAnimated();

    RoomPreviewInfo _info;
    ProtocolBridge *_bridge = nullptr;
    QWidget *_panel = nullptr;

    qreal _bgOpacity = 0.0;
    qreal _layerOpacity = 0.0;
    QVariantAnimation *_a_shown = nullptr;
    QVariantAnimation *_a_layerShown = nullptr;
    bool _closing = false;
};

} // namespace TeleMatrix

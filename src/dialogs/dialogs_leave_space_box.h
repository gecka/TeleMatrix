// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>

#include "dialogs_include_chats_box.h" // RoomPickEntry, ChatPickInner

class QEventLoop;
class QVariantAnimation;

namespace Ui {
class TextButton;
} // namespace Ui

namespace TeleMatrix {

class ProtocolBridge;

/// Element-parity "Leave space" dialog. Confirms leaving the space and, when the
/// user belongs to any of its rooms, lets them pick which of those to also leave
/// (default: none — the rooms stay in the chat list). Reuses the include-picker's
/// multi-select room list (ChatPickInner); with no member rooms it degrades to a
/// plain confirm.
class DialogsLeaveSpaceBox final : public QWidget {
    Q_OBJECT

public:
    enum DialogCode { Rejected = 0, Accepted = 1 };

    DialogsLeaveSpaceBox(
        const QString &spaceName,
        const QVector<RoomPickEntry> &rooms,
        ProtocolBridge *bridge,
        QWidget *parent = nullptr);

    int exec();

    /// Rooms the user chose to also leave (a subset of the input rooms; empty
    /// when they leave only the space).
    [[nodiscard]] QSet<QString> selectedRoomIds() const { return _result_selected; }

private:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

    void accept();
    void reject();
    void toggleRoom(const QString &roomId);
    void refreshSelectAll();

    QVector<RoomPickEntry> _rooms;
    QSet<QString> _selected; // rooms ticked for leaving (default: empty)

    QWidget *_panel = nullptr;
    ChatPickInner *_inner = nullptr;
    ::Ui::TextButton *_selectAll = nullptr;
    ::Ui::TextButton *_cancel = nullptr;
    // Not `_leave`: MSVC's language extensions make `_leave` a synonym for the
    // SEH keyword `__leave`, so it can't be an identifier.
    ::Ui::TextButton *_leaveButton = nullptr;

    QSet<QString> _result_selected;

    qreal _bgOpacity = 0.0;
    qreal _layerOpacity = 0.0;
    QVariantAnimation *_a_shown = nullptr;
    QVariantAnimation *_a_layerShown = nullptr;

    QEventLoop *_loop = nullptr;
    int _result = Rejected;
};

} // namespace TeleMatrix

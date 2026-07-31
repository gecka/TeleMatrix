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

#include "dialogs_include_chats_box.h" // RoomPickEntry, DialogsIncludeChatsBox

class QEventLoop;
class QLineEdit;
class QScrollArea;
class QVariantAnimation;

namespace Ui {
class EmojiInputField;
class TextButton;
} // namespace Ui

namespace TeleMatrix {

class ProtocolBridge;

/// Inner list of the folder's currently-included chats, each with a remove (×).
class IncludedChatsInner final : public QWidget {
    Q_OBJECT

public:
    explicit IncludedChatsInner(QWidget *parent = nullptr);

    void setRooms(const QVector<RoomPickEntry> &rooms);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void removeRequested(const QString &roomId);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    int indexAt(const QPoint &pos) const;
    QRect removeRectFor(int rowTop) const;
    int contentHeight() const;

    QVector<RoomPickEntry> _rooms;
    int _hoveredRemove = -1;
};

/// Modal "New Folder" / "Edit Folder" box: name + included chats. Simplified
/// filter-edit box (no emoji button, no color, no invite link, no
/// excluded chats). Commit-on-Save: the caller reads folderName() +
/// selectedRoomIds() after exec() and reconciles membership.
class DialogsEditFolderBox final : public QWidget {
    Q_OBJECT

public:
    enum Mode { Create, Edit };
    enum DialogCode { Rejected = 0, Accepted = 1 };

    DialogsEditFolderBox(
        Mode mode,
        int folderId,
        const QString &initialName,
        const QVector<RoomPickEntry> &allRooms,
        const QSet<QString> &initialSelected,
        ProtocolBridge *bridge,
        QWidget *parent = nullptr);

    int exec();

    [[nodiscard]] QString folderName() const;
    [[nodiscard]] QSet<QString> selectedRoomIds() const { return _draft; }

private:
    void paintEvent(QPaintEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

    void accept();
    void reject();
    void openIncludePicker();
    void refreshPreview();
    void updateSaveButton();
    QVector<RoomPickEntry> draftRooms() const;

    Mode _mode = Create;
    int _folderId = 0;
    ProtocolBridge *_bridge = nullptr;
    QVector<RoomPickEntry> _allRooms;
    QSet<QString> _draft;

    QWidget *_panel = nullptr;
    Ui::EmojiInputField *_nameField = nullptr;
    IncludedChatsInner *_preview = nullptr;
    QScrollArea *_scroll = nullptr;
    ::Ui::TextButton *_cancel = nullptr;
    ::Ui::TextButton *_save = nullptr;

    qreal _bgOpacity = 0.0;
    qreal _layerOpacity = 0.0;
    QVariantAnimation *_a_shown = nullptr;
    QVariantAnimation *_a_layerShown = nullptr;

    QEventLoop *_loop = nullptr;
    int _result = Rejected;
};

} // namespace TeleMatrix

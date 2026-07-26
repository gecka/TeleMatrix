// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>

class QEventLoop;
class QLineEdit;
class QScrollArea;
class QVariantAnimation;

namespace Ui {
class TextButton;
} // namespace Ui

namespace TeleMatrix {

class ProtocolBridge;

/// One pickable chat (room) shown in the include picker / folder preview.
struct RoomPickEntry {
    QString id;
    QString name;
    QString avatarUrl;
    QString avatarEntityId;
    QString status; // folders the room already belongs to (list-row subtitle)
};

/// A selected-chat chip: avatar + name pill, removable (× over the avatar on
/// hover).
struct ChatChip {
    QString id;
    QString name;
    QString avatarUrl;
    QString avatarEntityId;
    QRect rect;
};

/// MultiSelect chip bar: wrapping flow of avatar chips + inline search input.
class ChatChipBar final : public QWidget {
    Q_OBJECT

public:
    explicit ChatChipBar(QWidget *parent = nullptr);

    void addChip(const RoomPickEntry &room);
    void removeChip(const QString &id);
    bool hasChip(const QString &id) const;
    [[nodiscard]] QSet<QString> currentIds() const;
    QLineEdit *inputField() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void chipAdded(const QString &id);
    void chipRemoved(const QString &id);
    void heightChanged();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;
    void resizeEvent(QResizeEvent *) override;

private:
    void relayout();
    int chipAt(const QPoint &pos) const;

    QVector<ChatChip> _chips;
    QLineEdit *_input = nullptr;
    int _hoveredChip = -1; // chip under the cursor (shows the × over its avatar)
};

/// Inner list for the include picker: every chat with a multi-select checkmark.
class ChatPickInner final : public QWidget {
    Q_OBJECT

public:
    explicit ChatPickInner(QWidget *parent = nullptr);

    void setRooms(const QVector<RoomPickEntry> &rooms);
    void setSelected(const QSet<QString> &selected);
    void setFilter(const QString &query);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void roomClicked(const QString &roomId);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    int indexAt(const QPoint &pos) const;
    void paintRow(QPainter &p, int filteredIndex, bool hovered);
    int contentHeight() const;

    QVector<RoomPickEntry> _all;
    QVector<int> _filtered;
    QSet<QString> _selected;
    int _hovered = -1;
};

/// Modal "Add chats" picker — multi-select list of chats to include in a folder.
/// A multi-select chip bar (selected chats +
/// inline search) over a fixed-max-height list. No excluded list, no chat types.
class DialogsIncludeChatsBox final : public QWidget {
    Q_OBJECT

public:
    enum DialogCode { Rejected = 0, Accepted = 1 };

    DialogsIncludeChatsBox(
        const QVector<RoomPickEntry> &rooms,
        const QSet<QString> &selected,
        ProtocolBridge *bridge,
        QWidget *parent = nullptr);

    int exec();

    [[nodiscard]] QSet<QString> selectedRoomIds() const;

private:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

    void accept();
    void reject();
    void toggleRoom(const QString &roomId);
    [[nodiscard]] QSet<QString> currentChipIds() const;
    void syncListSelection();

    QHash<QString, RoomPickEntry> _entryById;

    QWidget *_panel = nullptr;
    ChatChipBar *_chipBar = nullptr;
    ChatPickInner *_inner = nullptr;
    QScrollArea *_scroll = nullptr;
    ::Ui::TextButton *_add = nullptr;
    ::Ui::TextButton *_cancel = nullptr;

    QSet<QString> _result_selected;

    qreal _bgOpacity = 0.0;
    qreal _layerOpacity = 0.0;
    QVariantAnimation *_a_shown = nullptr;
    QVariantAnimation *_a_layerShown = nullptr;

    QEventLoop *_loop = nullptr;
    int _result = Rejected;
};

} // namespace TeleMatrix

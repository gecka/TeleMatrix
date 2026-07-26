// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QImage>
#include <QString>
#include "ui/rp_widget.h"

namespace TeleMatrix {

/// "Search in [Room]" banner shown above search results
/// when searching within a specific room. Displays a gray "Search in" label
/// bar, the room row (avatar + name + chevron + cancel), and optionally a
/// sender filter row (avatar + "From: User" + chevron + cancel).
///
/// Pixel values match the search bar / "Search in" row layout.
class ChatSearchIn : public Ui::RpWidget {
    Q_OBJECT

public:
    explicit ChatSearchIn(QWidget *parent = nullptr);

    /// Set the room context (shows room row).
    void setRoom(const QString &roomId, const QString &roomName);
    void setRoomLabel(const QString &label);
    void setRoomAvatar(const QString &avatarUrl);
    void setRoomUseChatsIcon(bool useChatsIcon);
    /// Set the sender filter (shows sender row below room row).
    void setSender(const QString &senderId, const QString &senderName);
    void setSenderAvatar(const QString &avatarUrl);
    /// Remove the sender filter row.
    void clearSender();

    /// Total height including all visible sections.
    [[nodiscard]] int contentHeight() const;

Q_SIGNALS:
    /// Emitted when the close button on the room row is clicked.
    void roomCleared();
    /// Emitted when the close button on the sender row is clicked.
    void senderCleared();
    /// Emitted when the room name / chevron area is clicked (show dropdown).
    void roomFilterClicked();
    /// Emitted when the sender name / chevron area is clicked (show member picker).
    void senderFilterClicked();

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    [[nodiscard]] QRect roomCloseRect() const;
    [[nodiscard]] QRect senderCloseRect() const;
    [[nodiscard]] QRect roomNameRect() const;
    [[nodiscard]] QRect senderNameRect() const;
    void updateHeight();

    void paintIcon(QPainter &p, const QImage &baseIcon,
        const QColor &color, int x, int y) const;
    void paintAvatar(QPainter &p, int left, int top, int size,
        const QString &name, const QString &avatarUrl,
        const QString &entityId) const;
    void paintRow(QPainter &p, int rowTop, const QString &label,
        const QString &avatarName, const QString &avatarUrl,
        const QString &entityId, const QString &prefix,
        bool closeHovered, bool useChatsIcon) const;

    QString _roomId;
    QString _roomName;
    QString _roomLabel;
    QString _roomAvatarUrl;
    QString _senderId;
    QString _senderName;
    QString _senderAvatarUrl;

    // Preloaded icons.
    QImage _cancelIcon;  // dialogs_cancel_search
    QImage _chevronIcon; // intro_country_dropdown
    QImage _chatsIcon;   // menu_chats
    qreal _dpr = 1.0;
    bool _roomUseChatsIcon = false;

    bool _roomCloseHovered = false;
    bool _senderCloseHovered = false;
    bool _roomNameHovered = false;
    bool _senderNameHovered = false;
};

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QImage>
#include <QRect>
#include <QString>
#include <QWidget>

namespace TeleMatrix {

/// Compact pinned-message bar shown above the timeline.
class HistoryPinnedBar : public QWidget {
    Q_OBJECT

public:
    explicit HistoryPinnedBar(QWidget *parent = nullptr);

    void setPinnedMessage(
        const QString &eventId,
        const QString &title,
        const QString &text,
        const QString &previewPath = QString());
    void clearPinnedMessage();
    bool hasPinnedMessage() const { return !_eventId.isEmpty(); }
    const QString &eventId() const { return _eventId; }

    void setPinnedCount(int count);

signals:
    void barClicked(const QString &eventId);
    void showAllClicked();

protected:
    void paintEvent(QPaintEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

private:
    void updateHoverState(const QPoint &pos);
    void ensureShowAllIcon();

    QString _eventId;
    QString _title;
    QString _text;
    QString _previewPath;
    QImage _previewImage;
    QRect _closeRect;
    bool _closeHover = false;
    bool _barPressed = false;
    bool _closePressed = false;
    int _pinnedCount = 1;
    QImage _showAllIcon;
    QImage _showAllIconOver;
};

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>

namespace TeleMatrix {

/// Scroll-to-bottom button.
/// Painted as: shadow circle + white circle + chevron arrow + unread badge.
/// Parented to the scroll area, positioned via moveToRight().
class HistoryDownButton : public QWidget {
    Q_OBJECT

public:
    explicit HistoryDownButton(QWidget *parent);

    void setUnreadCount(int count);
    [[nodiscard]] int unreadCount() const { return _unreadCount; }

    /// When true the arrow points UP (jump-to-latest / return-to-live mode).
    /// When false the arrow points DOWN (scroll-to-bottom mode, default).
    void setJumpToLatestMode(bool jumpToLatest);
    [[nodiscard]] bool isJumpToLatestMode() const { return _jumpToLatest; }

    // Button style values.
    static constexpr int kWidth = 52;
    static constexpr int kHeight = 62;

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    int _unreadCount = 0;
    bool _jumpToLatest = false;
    bool _over = false;
    bool _pressed = false;
};

} // namespace TeleMatrix

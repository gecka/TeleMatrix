// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>

namespace TeleMatrix {

/// Attach chooser popup with two options:
/// "Photo or Video" and "Document".
class HistoryAttachPopup : public QWidget {
    Q_OBJECT
public:
    explicit HistoryAttachPopup(QWidget *parent);

    /// Show the popup anchored above the given widget.
    void showNear(QWidget *anchor);

signals:
    void photoOrVideoChosen();
    void documentChosen();

protected:
    void paintEvent(QPaintEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    [[nodiscard]] int rowAtPos(const QPoint &pos) const;

    int _hoveredRow = -1;

    static constexpr int kRowCount = 2;
    static constexpr int kRowHeight = 40;
    static constexpr int kPopupWidth = 200;
    static constexpr int kPopupRadius = 10;
    static constexpr int kIconSize = 20;
    static constexpr int kPadding = 12;
    static constexpr int kShadowExtend = 10;
    static constexpr int kScrollPaddingTop = 5;
    static constexpr int kScrollPaddingBottom = 5;
};

} // namespace TeleMatrix

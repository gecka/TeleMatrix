// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>

#include "ui/style/runtime_scale.h"

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

    // Scaled at use, not constexpr. The shadow sprites this popup is measured
    // against come from loadScaledMask, which sizes them by Scale() * dpr; fixed
    // geometry left the body's rounded corner smaller than the sprite's corner at
    // any non-100% interface scale, so the sprite's opaque inner quadrant showed
    // through as a black square at each corner.
    [[nodiscard]] static int rowHeight() { return Style::ConvertScale(40); }
    [[nodiscard]] static int popupWidth() { return Style::ConvertScale(200); }
    [[nodiscard]] static int popupRadius() { return Style::ConvertScale(10); }
    [[nodiscard]] static int iconSize() { return Style::ConvertScale(20); }
    [[nodiscard]] static int padding() { return Style::ConvertScale(12); }
    [[nodiscard]] static int shadowExtend() { return Style::ConvertScale(10); }
    [[nodiscard]] static int scrollPaddingTop() { return Style::ConvertScale(5); }
    [[nodiscard]] static int scrollPaddingBottom() { return Style::ConvertScale(5); }
};

} // namespace TeleMatrix

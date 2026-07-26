// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

// Compatibility header: provides Ui::RpWidget.
//
// This is a simplified version of lib_ui's RpWidget.
// When lib_ui is fully integrated, this file should be replaced with
// a direct include of the real lib_ui/ui/rp_widget.h.
#pragma once

#include <QWidget>

namespace Ui {

// Simplified RpWidget: a QWidget with Q_OBJECT for qobject_cast support.
// The real lib_ui RpWidget adds reactive programming (rpl) event streams,
// geometry observation, and RTL support. We'll add those when we integrate lib_ui.
class RpWidget : public QWidget {
    Q_OBJECT

public:
    explicit RpWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {}
    ~RpWidget() override;

    void resizeToWidth(int newWidth) {
        resize(newWidth, height());
    }

    /// lib_ui API: position from right edge of parent.
    void moveToRight(int right, int top) {
        if (auto *p = parentWidget()) {
            move(p->width() - right - width(), top);
        }
    }
};

} // namespace Ui

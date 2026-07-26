// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "ui/rp_widget.h"

class QVBoxLayout;

namespace Ui {
class ScrollArea;
}

namespace TeleMatrix {

class SettingsScrollPage : public ::Ui::RpWidget {
    Q_OBJECT

public:
    explicit SettingsScrollPage(QWidget *parent = nullptr);

    [[nodiscard]] QVBoxLayout *contentLayout() const;
    [[nodiscard]] ::Ui::RpWidget *contentWidget() const;

    void clearContent();
    void scrollToTop();

private:
    ::Ui::ScrollArea *_scroll = nullptr;
    ::Ui::RpWidget *_content = nullptr;
    QVBoxLayout *_layout = nullptr;
};

} // namespace TeleMatrix

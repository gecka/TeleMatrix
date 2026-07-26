// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "settings/settings_page.h"

#include "settings/settings_common_widgets.h"
#include "ui/widgets/scroll_area.h"

#include <QFrame>
#include <QScrollBar>
#include <QVBoxLayout>

namespace TeleMatrix {

SettingsScrollPage::SettingsScrollPage(QWidget *parent)
    : ::Ui::RpWidget(parent) {
    auto *pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);

    _scroll = new ::Ui::ScrollArea(this);
    _scroll->setWidgetResizable(true);
    _scroll->setFrameShape(QFrame::NoFrame);
    _scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    _content = new ::Ui::RpWidget(_scroll);
    _layout = new QVBoxLayout(_content);
    _layout->setContentsMargins(0, 0, 0, 20);
    _layout->setSpacing(0);

    _scroll->setWidget(_content);
    pageLayout->addWidget(_scroll);
}

QVBoxLayout *SettingsScrollPage::contentLayout() const {
    return _layout;
}

::Ui::RpWidget *SettingsScrollPage::contentWidget() const {
    return _content;
}

void SettingsScrollPage::clearContent() {
    clearSettingsLayout(_layout);
}

void SettingsScrollPage::scrollToTop() {
    if (_scroll) {
        _scroll->verticalScrollBar()->setValue(0);
    }
}

} // namespace TeleMatrix

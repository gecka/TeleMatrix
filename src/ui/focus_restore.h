// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QApplication>
#include <QPointer>
#include <QWidget>

namespace TeleMatrix::Focus {

using FocusRestoreTarget = QPointer<QWidget>;

inline FocusRestoreTarget saveFocusForPopup() {
    return QApplication::focusWidget();
}

inline void restoreFocusAfterPopup(FocusRestoreTarget target) {
    if (!target || !target->isVisible() || !target->isEnabled()) {
        return;
    }
    target->setFocus(Qt::OtherFocusReason);
}

} // namespace TeleMatrix::Focus

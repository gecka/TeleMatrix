// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "ui/input_submit_settings.h"

#include <QCoreApplication>

namespace TeleMatrix {

bool ShouldSubmit(
    int key,
    Qt::KeyboardModifiers modifiers,
    InputSubmitSettings setting)
{
    if (key != Qt::Key_Return && key != Qt::Key_Enter) {
        return false;
    }

    switch (setting) {
    case InputSubmitSettings::Enter:
        // Enter sends unless Shift is held (Shift+Enter = newline).
        return !(modifiers & Qt::ShiftModifier);

    case InputSubmitSettings::CtrlEnter:
        // Only Cmd/Ctrl+Enter sends. Plain Enter = newline.
        // On macOS, Qt::ControlModifier maps to Cmd key.
        return (modifiers & Qt::ControlModifier);
    }

    return false;
}

QString LabelForSubmitSetting(InputSubmitSettings setting) {
    switch (setting) {
    case InputSubmitSettings::Enter:
        return QCoreApplication::translate("InputSubmitSettings", "Send messages with Enter");
    case InputSubmitSettings::CtrlEnter:
#ifdef Q_OS_MAC
        return QCoreApplication::translate("InputSubmitSettings", "Send messages with Cmd+Enter");
#else
        return QCoreApplication::translate("InputSubmitSettings", "Send messages with Ctrl+Enter");
#endif
    }
    return QString();
}

} // namespace TeleMatrix

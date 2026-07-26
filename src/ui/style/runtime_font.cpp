// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "ui/style/runtime_font.h"

#include <QFont>

namespace TeleMatrix::Style {
namespace {

QString CustomFontFamily;

} // namespace

void SetCustomFont(const QString &family) {
    CustomFontFamily = family;
}

const QString &CustomFont() {
    return CustomFontFamily;
}

bool HasCustomFont() {
    return !CustomFontFamily.isEmpty();
}

QString EffectiveFontFamily() {
    if (CustomFontFamily.isEmpty()) {
        return QString(); // Caller should use default.
    }
    if (CustomFontFamily == QStringLiteral("system")) {
        return QFont().defaultFamily(); // System default font.
    }
    return CustomFontFamily;
}

} // namespace TeleMatrix::Style

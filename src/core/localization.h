// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QString>
#include <QVector>

namespace TeleMatrix::Core {

struct SupportedLanguage {
    QString id;
    QString name;
    QString nativeName;
};

[[nodiscard]] QVector<SupportedLanguage> supportedLanguages();
[[nodiscard]] bool isSupportedLanguage(const QString &id);
[[nodiscard]] QString normalizeLanguageId(const QString &id);
[[nodiscard]] QString resolveLanguageId(const QString &savedId);
[[nodiscard]] QString languageName(const QString &id);
[[nodiscard]] QString languageNativeName(const QString &id);

} // namespace TeleMatrix::Core

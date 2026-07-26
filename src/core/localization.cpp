// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "localization.h"

#include <QLocale>

namespace TeleMatrix::Core {

namespace {

const QVector<SupportedLanguage> &SupportedLanguages() {
    static const auto languages = QVector<SupportedLanguage>{
        {
            QStringLiteral("en"),
            QStringLiteral("English"),
            QStringLiteral("English"),
        },
        {
            QStringLiteral("es"),
            QStringLiteral("Spanish"),
            QStringLiteral("Español"),
        },
    };
    return languages;
}

QString PrimaryLanguageSubtag(QString id) {
    id = id.trimmed().replace(QLatin1Char('_'), QLatin1Char('-')).toLower();
    const auto dash = id.indexOf(QLatin1Char('-'));
    return (dash > 0) ? id.left(dash) : id;
}

const SupportedLanguage *FindLanguage(const QString &id) {
    const auto normalized = PrimaryLanguageSubtag(id);
    if (normalized.isEmpty()) {
        return nullptr;
    }
    for (const auto &language : SupportedLanguages()) {
        if (language.id == normalized) {
            return &language;
        }
    }
    return nullptr;
}

QString ResolveSystemLanguage() {
    const auto system = QLocale::system();
    for (const auto &language : system.uiLanguages()) {
        if (const auto normalized = normalizeLanguageId(language); !normalized.isEmpty()) {
            return normalized;
        }
    }
    if (const auto normalized = normalizeLanguageId(system.name()); !normalized.isEmpty()) {
        return normalized;
    }
    return QStringLiteral("en");
}

} // namespace

QVector<SupportedLanguage> supportedLanguages() {
    return SupportedLanguages();
}

bool isSupportedLanguage(const QString &id) {
    return !normalizeLanguageId(id).isEmpty();
}

QString normalizeLanguageId(const QString &id) {
    return FindLanguage(id) ? PrimaryLanguageSubtag(id) : QString();
}

QString resolveLanguageId(const QString &savedId) {
    if (const auto normalized = normalizeLanguageId(savedId); !normalized.isEmpty()) {
        return normalized;
    }
    return ResolveSystemLanguage();
}

QString languageName(const QString &id) {
    if (const auto language = FindLanguage(id)) {
        return language->name;
    }
    return id;
}

QString languageNativeName(const QString &id) {
    if (const auto language = FindLanguage(id)) {
        return language->nativeName;
    }
    return id;
}

} // namespace TeleMatrix::Core

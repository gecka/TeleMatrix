// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "safe_url.h"

#include <QDesktopServices>
#include <QSet>
#include <QUrl>

namespace TeleMatrix {

namespace {

[[nodiscard]] QUrl normalizedExternalUrl(const QString &raw) {
    const auto trimmed = raw.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }

    auto url = QUrl(trimmed, QUrl::StrictMode);
    if (!url.isValid() || url.scheme().isEmpty()) {
        url = QUrl::fromUserInput(trimmed);
    }
    if (!url.isValid() || url.scheme().isEmpty()) {
        return {};
    }

    const auto scheme = url.scheme().toLower();
    static const auto allowed = QSet<QString>{
        QStringLiteral("http"),
        QStringLiteral("https"),
        QStringLiteral("ftp"),
    };
    if (!allowed.contains(scheme)) {
        return {};
    }
    return url;
}

} // namespace

bool IsSafeExternalUrl(const QString &url) {
    return normalizedExternalUrl(url).isValid();
}

bool OpenSafeExternalUrl(const QString &url) {
    const auto normalized = normalizedExternalUrl(url);
    if (!normalized.isValid()) {
        return false;
    }
    return QDesktopServices::openUrl(normalized);
}

} // namespace TeleMatrix

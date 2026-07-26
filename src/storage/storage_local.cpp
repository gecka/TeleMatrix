// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "storage_local.h"
#include "core/core_settings.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace TeleMatrix::Local {

namespace {

QString _basePath;

// v3 added the account list: device-level settings sit at "settings", every
// account's own settings live in "accounts", and "activeAccount" points into it.
// Anything older is not read (the reader returns defaults) — there is no upgrade
// path because no build that wrote v2 was ever released.
constexpr int kSettingsJsonVersion = 3;

QString basePath() {
    if (_basePath.isEmpty()) {
        // ~/Library/Application Support/TeleMatrix/
        _basePath = QStandardPaths::writableLocation(
            QStandardPaths::GenericDataLocation)
            + QStringLiteral("/TeleMatrix/");
    }
    return _basePath;
}

QString settingsJsonPath() {
    return basePath() + QStringLiteral("settings.json");
}

bool readSettingsJson(Core::Settings &settings, AccountIndex &accounts) {
    QFile file(settingsJsonPath());
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }

    const auto root = document.object();
    if (root.value(QStringLiteral("version")).toInt() != kSettingsJsonVersion) {
        return false;
    }

    const auto settingsObject = root.value(QStringLiteral("settings")).toObject();
    if (!settings.addFromJson(settingsObject)) {
        return false;
    }
    accounts = ParseAccountIndex(root);

    return true;
}

} // namespace

void start() {
    QDir().mkpath(basePath());
}

bool readSettings(Core::Settings &settings, AccountIndex &accounts) {
    return readSettingsJson(settings, accounts);
}

bool writeSettings(const Core::Settings &settings, const AccountIndex &accounts) {
    QDir().mkpath(basePath());

    QJsonObject root = SerializeAccountIndex(accounts);
    root[QStringLiteral("version")] = kSettingsJsonVersion;
    root[QStringLiteral("settings")] = settings.toJson();

    QSaveFile file(settingsJsonPath());
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.commit();
}

} // namespace TeleMatrix::Local

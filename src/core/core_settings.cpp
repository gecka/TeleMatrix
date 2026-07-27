// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "core_settings.h"

#include <algorithm>
#include <QJsonArray>
#include <QJsonObject>

namespace TeleMatrix::Core {

namespace {

QString NormalizeEmojiPresentation(QString emoji) {
    if (emoji == QString::fromUtf8("\xE2\x9D\xA4")) {
        return QString::fromUtf8("\xE2\x9D\xA4\xEF\xB8\x8F");
    }
    if (emoji == QString::fromUtf8("\xE2\x98\xBA")) {
        return QString::fromUtf8("\xE2\x98\xBA\xEF\xB8\x8F");
    }
    return emoji;
}

QJsonObject SerializeWindowPositionJson(const WindowPosition &pos) {
    return {
        { QStringLiteral("moncrc"), pos.moncrc },
        { QStringLiteral("maximized"), pos.maximized },
        { QStringLiteral("scale"), pos.scale },
        { QStringLiteral("x"), pos.x },
        { QStringLiteral("y"), pos.y },
        { QStringLiteral("w"), pos.w },
        { QStringLiteral("h"), pos.h },
    };
}

WindowPosition DeserializeWindowPositionJson(const QJsonObject &object) {
    WindowPosition pos;
    pos.moncrc = object.value(QStringLiteral("moncrc")).toInt();
    pos.maximized = object.value(QStringLiteral("maximized")).toInt();
    pos.scale = object.value(QStringLiteral("scale")).toInt();
    pos.x = object.value(QStringLiteral("x")).toInt();
    pos.y = object.value(QStringLiteral("y")).toInt();
    pos.w = object.value(QStringLiteral("w")).toInt();
    pos.h = object.value(QStringLiteral("h")).toInt();
    return pos;
}

QString EmojiIdFromOldKey(uint64_t oldKey) {
    auto code = uint32_t(oldKey >> 32);
    auto code2 = uint32_t(oldKey & 0xFFFFFFFFULL);
    if (!code && code2) {
        code = code2;
        code2 = 0;
    }
    if ((code & 0xFFFF0000U) != 0xFFFF0000U) {
        auto result = QString();
        result.reserve(4);
        const auto addCode = [&result](uint32_t value) {
            if (const auto high = (value >> 16)) {
                result.append(QChar(static_cast<ushort>(high & 0xFFFFU)));
            }
            result.append(QChar(static_cast<ushort>(value & 0xFFFFU)));
        };
        addCode(code);
        if (code2) {
            addCode(code2);
        }
        return NormalizeEmojiPresentation(result);
    }

    switch (int(code & 0xFFFFU)) {
    case 0: return QString::fromUtf8("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7");
    case 1: return QString::fromUtf8("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6");
    case 2: return QString::fromUtf8("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA6\xE2\x80\x8D\xF0\x9F\x91\xA6");
    case 3: return QString::fromUtf8("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA7");
    case 4: return QString::fromUtf8("\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA6");
    case 5: return QString::fromUtf8("\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7");
    case 6: return QString::fromUtf8("\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6");
    case 7: return QString::fromUtf8("\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA6\xE2\x80\x8D\xF0\x9F\x91\xA6");
    case 8: return QString::fromUtf8("\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA7");
    case 9: return QString::fromUtf8("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA6");
    case 10: return QString::fromUtf8("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA7");
    case 11: return QString::fromUtf8("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6");
    case 12: return QString::fromUtf8("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA6\xE2\x80\x8D\xF0\x9F\x91\xA6");
    case 13: return QString::fromUtf8("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA7");
    case 14: return QString::fromUtf8("\xF0\x9F\x91\xA9\xE2\x80\x8D\xE2\x9D\xA4\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x91\xA9");
    case 15: return QString::fromUtf8("\xF0\x9F\x91\xA8\xE2\x80\x8D\xE2\x9D\xA4\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x91\xA8");
    case 16: return QString::fromUtf8("\xF0\x9F\x91\xA9\xE2\x80\x8D\xE2\x9D\xA4\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x92\x8B\xE2\x80\x8D\xF0\x9F\x91\xA9");
    case 17: return QString::fromUtf8("\xF0\x9F\x91\xA8\xE2\x80\x8D\xE2\x9D\xA4\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x92\x8B\xE2\x80\x8D\xF0\x9F\x91\xA8");
    case 18: return QString::fromUtf8("\xF0\x9F\x91\x81\xE2\x80\x8D\xF0\x9F\x97\xA8");
    default: return QString();
    }
}

QVector<RecentEmoji> DefaultRecentEmoji() {
    constexpr uint64_t kDefaultRecent[] = {
        0xD83DDE02ULL,
        0xD83DDE18ULL,
        0x2764ULL,
        0xD83DDE0DULL,
        0xD83DDE0AULL,
        0xD83DDE01ULL,
        0xD83DDC4DULL,
        0x263AULL,
        0xD83DDE14ULL,
        0xD83DDE04ULL,
        0xD83DDE2DULL,
        0xD83DDC8BULL,
        0xD83DDE12ULL,
        0xD83DDE33ULL,
        0xD83DDE1CULL,
        0xD83DDE48ULL,
        0xD83DDE09ULL,
        0xD83DDE03ULL,
        0xD83DDE22ULL,
        0xD83DDE1DULL,
        0xD83DDE31ULL,
        0xD83DDE21ULL,
        0xD83DDE0FULL,
        0xD83DDE1EULL,
        0xD83DDE05ULL,
        0xD83DDE1AULL,
        0xD83DDE4AULL,
        0xD83DDE0CULL,
        0xD83DDE00ULL,
        0xD83DDE0BULL,
        0xD83DDE06ULL,
        0xD83DDC4CULL,
        0xD83DDE10ULL,
        0xD83DDE15ULL,
    };

    auto result = QVector<RecentEmoji>();
    result.reserve(int(sizeof(kDefaultRecent) / sizeof(kDefaultRecent[0])));
    for (const auto key : kDefaultRecent) {
        const auto emoji = NormalizeEmojiPresentation(EmojiIdFromOldKey(key));
        if (!emoji.isEmpty()) {
            result.push_back({ emoji, 1 });
        }
    }
    return result;
}

} // namespace

Settings::Settings() = default;

AccountSettings::AccountSettings()
    : _recentEmoji(DefaultRecentEmoji()) {
}

void AccountSettings::clear() {
    _sessionHomeserver.clear();
    _sessionUserId.clear();
    _sessionDeviceId.clear();
    _sessionSecretBackend.clear();
    _legacySessionAccessTokenPresent = false;
    // Wipe this account's user data on logout so nothing of it survives on the
    // device. Device-level preferences (theme, notifications, window, language)
    // live in Settings, and other accounts keep their own copies of all this.
    _pinnedRoomIds.clear();
    _customFolders.clear();
    _roomFolderAssignments.clear();
    _folderOrder.clear();
    _foldersServerMigrated = false;
    _recentEmoji.clear();
    _recentReactions.clear();
}

void AccountSettings::incrementRecentReaction(const QString &key) {
    if (key.isEmpty()) {
        return;
    }

    auto existing = std::find_if(
        _recentReactions.begin(),
        _recentReactions.end(),
        [&](const RecentReaction &item) {
            return item.key == key;
        });
    if (existing != _recentReactions.end()) {
        const auto next = std::min<int>(existing->rating + 1, 0xFFFF);
        existing->rating = static_cast<uint16_t>(std::max(next, 1));
    } else {
        _recentReactions.push_back({ key, 1 });
    }

    // Rebalance ratings to avoid overflow (same pattern as emoji).
    {
        auto maxRating = uint16_t(0);
        for (const auto &item : _recentReactions) {
            maxRating = std::max(maxRating, item.rating);
        }
        if (maxRating > 0x8000U) {
            for (auto &item : _recentReactions) {
                item.rating = std::max<uint16_t>(1, item.rating / 2);
            }
        }
    }

    std::stable_sort(
        _recentReactions.begin(),
        _recentReactions.end(),
        [](const RecentReaction &a, const RecentReaction &b) {
            return a.rating > b.rating;
        });
    if (_recentReactions.size() > kRecentReactionLimit) {
        _recentReactions.resize(kRecentReactionLimit);
    }
}

void AccountSettings::incrementRecentEmoji(const QString &emoji) {
    const auto normalized = NormalizeEmojiPresentation(emoji);
    if (normalized.isEmpty()) {
        return;
    }

    // Recently-used ordering: move the picked emoji to the front. The usage count
    // is still tracked (and capped) for the stored [emoji, count] format and
    // cross-client interop, but it no longer drives our ordering — "Recent" shows the
    // most-recently-used first, which is what users expect (a just-picked emoji
    // jumps to the front instead of having to out-count the most-used one).
    uint16_t rating = 1;
    for (auto it = _recentEmoji.begin(); it != _recentEmoji.end(); ++it) {
        if (it->emoji == normalized) {
            rating = static_cast<uint16_t>(std::min<int>(it->rating + 1, 0xFFFF));
            _recentEmoji.erase(it);
            break;
        }
    }
    _recentEmoji.prepend({ normalized, rating });
    if (_recentEmoji.size() > kRecentEmojiLimit) {
        _recentEmoji.resize(kRecentEmojiLimit);
    }
}

void AccountSettings::setRecentEmoji(QVector<RecentEmoji> items) {
    _recentEmoji = std::move(items);
}

QJsonObject AccountSettings::toJson() const {
    QJsonObject object;

    object[QStringLiteral("session")] = QJsonObject{
        { QStringLiteral("homeserver"), _sessionHomeserver },
        { QStringLiteral("userId"), _sessionUserId },
        { QStringLiteral("deviceId"), _sessionDeviceId },
        { QStringLiteral("secretBackend"), _sessionSecretBackend },
    };

    QJsonArray pinned;
    for (const auto &id : _pinnedRoomIds) {
        pinned.append(id);
    }
    object[QStringLiteral("pinnedRoomIds")] = pinned;

    QJsonArray folders;
    for (const auto &folder : _customFolders) {
        folders.append(QJsonObject{
            { QStringLiteral("id"), folder.id },
            { QStringLiteral("name"), folder.name },
        });
    }
    object[QStringLiteral("customFolders")] = folders;

    QJsonObject assignments;
    for (auto it = _roomFolderAssignments.cbegin(); it != _roomFolderAssignments.cend(); ++it) {
        QJsonArray ids;
        for (const auto id : it.value()) {
            ids.append(id);
        }
        assignments.insert(it.key(), ids);
    }
    object[QStringLiteral("roomFolderAssignments")] = assignments;

    QJsonArray order;
    for (const auto id : _folderOrder) {
        order.append(id);
    }
    object[QStringLiteral("folderOrder")] = order;
    object[QStringLiteral("foldersServerMigrated")] = _foldersServerMigrated;

    // Recent emojis are NOT stored here: they live in Matrix account data
    // (io.element.recent_emoji), cached in app_cache.db (see RecentEmojiService).

    QJsonArray recentReactions;
    for (const auto &item : _recentReactions) {
        recentReactions.append(QJsonObject{
            { QStringLiteral("key"), item.key },
            { QStringLiteral("rating"), int(item.rating) },
        });
    }
    object[QStringLiteral("recentReactions")] = recentReactions;

    return object;
}

bool AccountSettings::addFromJson(const QJsonObject &object) {
    if (object.isEmpty()) {
        return false;
    }

    const auto session = object.value(QStringLiteral("session")).toObject();
    if (!session.isEmpty()) {
        _sessionHomeserver = session.value(QStringLiteral("homeserver")).toString();
        _sessionUserId = session.value(QStringLiteral("userId")).toString();
        _sessionDeviceId = session.value(QStringLiteral("deviceId")).toString();
        _sessionSecretBackend = session.value(QStringLiteral("secretBackend")).toString();
        _legacySessionAccessTokenPresent = session.contains(QStringLiteral("accessToken"));
    }

    if (object.contains(QStringLiteral("pinnedRoomIds"))) {
        _pinnedRoomIds.clear();
        for (const auto &value : object.value(QStringLiteral("pinnedRoomIds")).toArray()) {
            const auto id = value.toString();
            if (!id.isEmpty()) {
                _pinnedRoomIds.push_back(id);
            }
        }
    }

    if (object.contains(QStringLiteral("customFolders"))) {
        _customFolders.clear();
        for (const auto &value : object.value(QStringLiteral("customFolders")).toArray()) {
            const auto folder = value.toObject();
            const auto id = folder.value(QStringLiteral("id")).toInt();
            const auto name = folder.value(QStringLiteral("name")).toString();
            if (id > 2 && !name.isEmpty()) {
                _customFolders.push_back({ id, name });
            }
        }
    }

    if (object.contains(QStringLiteral("roomFolderAssignments"))) {
        _roomFolderAssignments.clear();
        const auto assignments = object.value(QStringLiteral("roomFolderAssignments")).toObject();
        for (auto it = assignments.constBegin(); it != assignments.constEnd(); ++it) {
            QVector<int> ids;
            for (const auto &value : it.value().toArray()) {
                const auto id = value.toInt();
                if (id > 2) {
                    ids.push_back(id);
                }
            }
            if (!it.key().isEmpty() && !ids.isEmpty()) {
                _roomFolderAssignments.insert(it.key(), ids);
            }
        }
    }

    if (object.contains(QStringLiteral("folderOrder"))) {
        _folderOrder.clear();
        for (const auto &value : object.value(QStringLiteral("folderOrder")).toArray()) {
            const auto id = value.toInt();
            if (id > 0) {
                _folderOrder.push_back(id);
            }
        }
    }

    if (object.contains(QStringLiteral("foldersServerMigrated"))) {
        _foldersServerMigrated = object.value(QStringLiteral("foldersServerMigrated")).toBool();
    }

    // recentEmoji intentionally not read here — sourced from account data
    // (io.element.recent_emoji) via RecentEmojiService and hydrated at startup.

    if (object.contains(QStringLiteral("recentReactions"))) {
        _recentReactions.clear();
        for (const auto &value : object.value(QStringLiteral("recentReactions")).toArray()) {
            const auto item = value.toObject();
            const auto key = item.value(QStringLiteral("key")).toString();
            const auto rating = item.value(QStringLiteral("rating")).toInt(1);
            if (!key.isEmpty()) {
                _recentReactions.push_back({
                    key,
                    static_cast<uint16_t>(std::clamp(rating, 1, 0xFFFF))
                });
            }
        }
    }

    return true;
}

QJsonObject Settings::toJson() const {
    QJsonObject object;
    object[QStringLiteral("windowPosition")] = SerializeWindowPositionJson(_windowPosition);
    object[QStringLiteral("dialogsWidthRatio")] = _dialogsWidthRatio;

    object[QStringLiteral("notifications")] = QJsonObject{
        { QStringLiteral("desktopNotify"), _desktopNotify },
        { QStringLiteral("soundNotify"), _soundNotify },
        { QStringLiteral("showSenderName"), _showSenderName },
        { QStringLiteral("showMessagePreview"), _showMessagePreview },
        { QStringLiteral("includeMutedInBadge"), _includeMutedInBadge },
        { QStringLiteral("includeMutedInFolders"), _includeMutedInFolders },
        { QStringLiteral("bounceDockIcon"), _bounceDockIcon },
    };

    // Device-level: the first-run backend choice must outlive any account's
    // sign-out, so it lives here rather than in AccountSettings.
    object[QStringLiteral("preferredSecretBackend")] = _preferredSecretBackend;

    object[QStringLiteral("theme")] = QJsonObject{
        { QStringLiteral("id"), _themeId },
        { QStringLiteral("mode"), std::clamp(_themeMode, 0, 2) },
        { QStringLiteral("systemDarkModeEnabled"), _systemDarkModeEnabled },
    };

    object[QStringLiteral("appearance")] = QJsonObject{
        { QStringLiteral("configScale"), _configScale },
        { QStringLiteral("customFontFamily"), _customFontFamily },
        { QStringLiteral("backgroundDoodles"), _backgroundDoodles },
        { QStringLiteral("largeEmoji"), _largeEmoji },
        { QStringLiteral("replyButtonOnMessages"), _replyButtonOnMessages },
        { QStringLiteral("reactionButtonOnMessages"), _reactionButtonOnMessages },
        { QStringLiteral("hideSystemMessagesInPublicRooms"), _hideSystemMessagesInPublicRooms },
    };

    object[QStringLiteral("preferences")] = QJsonObject{
        { QStringLiteral("languageId"), _languageId },
        { QStringLiteral("sendSubmitWay"), std::clamp(_sendSubmitWay, 0, 1) },
        { QStringLiteral("macWarnBeforeQuit"), _macWarnBeforeQuit },
        { QStringLiteral("compressImages"), _compressImages },
        { QStringLiteral("mediaSaveDir"), _mediaSaveDir },
    };

    object[QStringLiteral("cache")] = QJsonObject{
        { QStringLiteral("sizeLimitMB"), _cacheSizeLimitMB },
        { QStringLiteral("autoCleanup"), _cacheAutoCleanup },
    };

    object[QStringLiteral("search")] = QJsonObject{
        { QStringLiteral("encryptedRooms"), _searchEncryptedRooms },
    };

    object[QStringLiteral("updates")] = QJsonObject{
        { QStringLiteral("policy"), std::clamp(_updatePolicy, 0, 2) },
        { QStringLiteral("beta"), _installBetaVersions },
    };

    return object;
}

bool Settings::addFromJson(const QJsonObject &object) {
    if (object.isEmpty()) {
        return false;
    }

    const auto windowObject = object.value(QStringLiteral("windowPosition")).toObject();
    if (!windowObject.isEmpty()) {
        _windowPosition = DeserializeWindowPositionJson(windowObject);
    }

    if (object.contains(QStringLiteral("dialogsWidthRatio"))) {
        _dialogsWidthRatio = std::clamp(
            object.value(QStringLiteral("dialogsWidthRatio")).toDouble(),
            0.0,
            1.0);
    }

    const auto notifications = object.value(QStringLiteral("notifications")).toObject();
    if (!notifications.isEmpty()) {
        _desktopNotify = notifications.value(QStringLiteral("desktopNotify")).toBool(_desktopNotify);
        _soundNotify = notifications.value(QStringLiteral("soundNotify")).toBool(_soundNotify);
        _showSenderName = notifications.value(QStringLiteral("showSenderName")).toBool(_showSenderName);
        _showMessagePreview = notifications.value(QStringLiteral("showMessagePreview")).toBool(_showMessagePreview);
        _includeMutedInBadge = notifications.value(QStringLiteral("includeMutedInBadge")).toBool(_includeMutedInBadge);
        _includeMutedInFolders = notifications.value(QStringLiteral("includeMutedInFolders")).toBool(_includeMutedInFolders);
        _bounceDockIcon = notifications.value(QStringLiteral("bounceDockIcon")).toBool(_bounceDockIcon);
    }

    _preferredSecretBackend =
        object.value(QStringLiteral("preferredSecretBackend")).toString();

    const auto theme = object.value(QStringLiteral("theme")).toObject();
    if (!theme.isEmpty()) {
        _themeId = theme.value(QStringLiteral("id")).toString(_themeId);
        _themeMode = std::clamp(theme.value(QStringLiteral("mode")).toInt(_themeMode), 0, 2);
        _systemDarkModeEnabled = theme.value(QStringLiteral("systemDarkModeEnabled")).toBool(
            _systemDarkModeEnabled);
    }

    const auto appearance = object.value(QStringLiteral("appearance")).toObject();
    if (!appearance.isEmpty()) {
        _configScale = appearance.value(QStringLiteral("configScale")).toInt(_configScale);
        _customFontFamily = appearance.value(QStringLiteral("customFontFamily")).toString(
            _customFontFamily);
        _backgroundDoodles = appearance.value(
            QStringLiteral("backgroundDoodles")).toBool(_backgroundDoodles);
        _largeEmoji = appearance.value(QStringLiteral("largeEmoji")).toBool(_largeEmoji);
        _replyButtonOnMessages = appearance.value(
            QStringLiteral("replyButtonOnMessages")).toBool(_replyButtonOnMessages);
        _reactionButtonOnMessages = appearance.value(
            QStringLiteral("reactionButtonOnMessages")).toBool(_reactionButtonOnMessages);
        _hideSystemMessagesInPublicRooms = appearance.value(
            QStringLiteral("hideSystemMessagesInPublicRooms"))
            .toBool(_hideSystemMessagesInPublicRooms);
    }

    const auto preferences = object.value(QStringLiteral("preferences")).toObject();
    if (!preferences.isEmpty()) {
        _languageId = preferences.value(QStringLiteral("languageId")).toString(_languageId);
        _sendSubmitWay = std::clamp(
            preferences.value(QStringLiteral("sendSubmitWay")).toInt(_sendSubmitWay),
            0,
            1);
        _macWarnBeforeQuit = preferences.value(QStringLiteral("macWarnBeforeQuit")).toBool(
            _macWarnBeforeQuit);
        _compressImages = preferences.value(QStringLiteral("compressImages")).toBool(
            _compressImages);
        _mediaSaveDir = preferences.value(QStringLiteral("mediaSaveDir")).toString(
            _mediaSaveDir);
    }

    const auto cache = object.value(QStringLiteral("cache")).toObject();
    if (!cache.isEmpty()) {
        _cacheSizeLimitMB = qBound(
            kCacheSizeLimitMinMB,
            cache.value(QStringLiteral("sizeLimitMB")).toInt(_cacheSizeLimitMB),
            kCacheSizeLimitMaxMB);
        _cacheAutoCleanup = cache.value(QStringLiteral("autoCleanup")).toBool(_cacheAutoCleanup);
    }

    const auto search = object.value(QStringLiteral("search")).toObject();
    if (!search.isEmpty()) {
        _searchEncryptedRooms = search.value(
            QStringLiteral("encryptedRooms")).toBool(_searchEncryptedRooms);
    }

    const auto updates = object.value(QStringLiteral("updates")).toObject();
    if (!updates.isEmpty()) {
        _updatePolicy = std::clamp(
            updates.value(QStringLiteral("policy")).toInt(_updatePolicy),
            0,
            2);
        _installBetaVersions = updates.value(
            QStringLiteral("beta")).toBool(_installBetaVersions);
    }

    return true;
}

} // namespace TeleMatrix::Core

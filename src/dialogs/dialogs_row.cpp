// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "dialogs_row.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QLocale>

#include "styles/style_dialogs.h"

using namespace Qt::Literals::StringLiterals;

namespace TeleMatrix {

namespace {

constexpr qint64 kDaySeconds = 24 * 60 * 60;

} // namespace

int DialogsRow::rowHeight() {
    return st::dialogsRowHeight;
}

// Translate known media-type labels that the Rust SDK sends in English.
static QString translateLastMessage(const QString &msg) {
    if (msg == QStringLiteral("Photo"))
        return QCoreApplication::translate("DialogsRow", "Photo");
    if (msg == QStringLiteral("Video"))
        return QCoreApplication::translate("DialogsRow", "Video");
    if (msg == QStringLiteral("File"))
        return QCoreApplication::translate("DialogsRow", "File");
    if (msg == QStringLiteral("Voice message"))
        return QCoreApplication::translate("DialogsRow", "Voice message");
    if (msg == QStringLiteral("Audio"))
        return QCoreApplication::translate("DialogsRow", "Audio");
    if (msg == QStringLiteral("Sticker"))
        return QCoreApplication::translate("DialogsRow", "Sticker");
    if (msg == QStringLiteral("GIF"))
        return QCoreApplication::translate("DialogsRow", "GIF");
    if (msg == QStringLiteral("Poll"))
        return QCoreApplication::translate("DialogsRow", "Poll");
    if (msg == QStringLiteral("Image"))
        return QCoreApplication::translate("DialogsRow", "Image");
    return msg;
}

void DialogsRow::updateFrom(const RoomSummary &room) {
    _roomId = room.roomId;
    _displayName = room.displayName;
    // simplified(): collapse newlines/whitespace runs to single spaces so a
    // multi-line message stays on one preview line. The single-point drawText used
    // for the preview doesn't render '\n' (it dropped them, running words together);
    // matches how the draft preview is normalized.
    _lastMessage = translateLastMessage(room.lastMessage).simplified();
    _lastSender = room.lastSender;
    _timestamp = room.timestamp;
    _unreadCount = room.unreadCount;
    _highlightCount = room.highlightCount;
    _isMarkedUnread = room.isMarkedUnread;
    _isMuted = room.isMuted;
    _notificationMode = room.notificationMode;
    _isPinned = room.isPinned;
    _pinnedOrder = room.pinnedOrder;
    _isDirect = room.isDirect;
    _avatarUrl = room.avatarUrl;
    _avatarEntityId = room.avatarEntityId.isEmpty() ? room.roomId : room.avatarEntityId;
    _filterIds = room.filterIds;
    _spaceIds = room.spaceIds;
    _isLastMessageOutgoing = room.isLastMessageOutgoing;
    _lastMessageSendState = room.lastMessageSendState;
    _peerPresence = room.peerPresence;
    _membership = room.membership;
    _inviterDisplayName = room.inviterDisplayName;
    _roomTopic = room.roomTopic;
    if (_peerPresence == 1 && _onlineBadgeProgress < 1.0) {
        _onlineBadgeProgress = 1.0;
    } else if (_peerPresence != 1 && _onlineBadgeProgress > 0.0) {
        _onlineBadgeProgress = 0.0;
    }

    updateDateText();
    invalidateTextCache();
    invalidateSearchCache();
}

Ui::Text::String &DialogsRow::nameText() {
    if (!_nameTextCache) {
        _nameTextCache.emplace(st::semiboldFont, _displayName);
    }
    return *_nameTextCache;
}

Ui::Text::String &DialogsRow::messageText() {
    if (!_messageTextCache) {
        auto preview = (_lastSender.isEmpty() || _lastMessage.isEmpty())
            ? _lastMessage
            : _lastSender + QStringLiteral(": ") + _lastMessage;
        _messageTextCache.emplace(st::dialogsTextFont, preview);
    }
    return *_messageTextCache;
}

void DialogsRow::setDraft(const QString &text) {
    const auto simplified = text.simplified();
    _hasDraft = !simplified.isEmpty();
    _draftText = simplified;
    invalidateTextCache();
}

void DialogsRow::setFilterIds(const QVector<int> &ids) {
    _filterIds = ids;
}

void DialogsRow::setPinned(bool pinned) {
    _isPinned = pinned;
}

void DialogsRow::updateDateText() {
    if (_timestamp <= 0) {
        _dateText.clear();
        return;
    }

    const auto now = QDateTime::currentDateTime();
    const auto msgTime = QDateTime::fromSecsSinceEpoch(_timestamp);
    const auto today = now.date();
    const auto msgDate = msgTime.date();
    const auto ageSeconds = msgTime.secsTo(now);

    if (ageSeconds >= 0 && ageSeconds < kDaySeconds) {
        _dateText = msgTime.toString(QStringLiteral("HH:mm"));
    } else if (msgDate.addDays(1) == today) {
        _dateText = QCoreApplication::translate("DialogsRow", "Yesterday");
    } else if (msgDate.addDays(7) > today) {
        _dateText = QLocale().dayName(msgDate.dayOfWeek(), QLocale::ShortFormat);
    } else if (msgDate.year() == today.year()) {
        _dateText = QLocale().toString(msgDate, QStringLiteral("MMM d"));
    } else {
        _dateText = QLocale().toString(msgDate, QLocale::ShortFormat);
    }
}

void DialogsRow::invalidateTextCache() {
    _nameTextCache.reset();
    _messageTextCache.reset();
}

void DialogsRow::invalidateSearchCache() {
    _searchableTextDirty = true;
}

const QString &DialogsRow::searchableText() const {
    if (_searchableTextDirty) {
        // Combine display name and room ID for searchable text.
        _searchableText = _displayName.toLower();
        if (!_roomId.isEmpty()) {
            _searchableText += QLatin1Char(' ') + _roomId.toLower();
        }
        _searchableTextDirty = false;
    }
    return _searchableText;
}

int DialogsRow::searchScore(const QString &normalizedQuery) const {
    const auto &text = searchableText();
    const auto name = _displayName.toLower();

    // Prefix match on display name.
    if (name.startsWith(normalizedQuery)) {
        return 0;
    }
    // Word-boundary match on display name.
    const auto words = name.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const auto &word : words) {
        if (word.startsWith(normalizedQuery)) {
            return 1;
        }
    }
    // Substring match on display name.
    if (name.contains(normalizedQuery)) {
        return 2;
    }
    // Match on room ID or other searchable text.
    if (text.contains(normalizedQuery)) {
        return 3;
    }
    return -1; // no match
}

} // namespace TeleMatrix

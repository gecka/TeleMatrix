// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QPixmap>
#include <optional>

#include "ui/text/text_entity.h" // Ui::Text::String from lib_ui

#include "../protocol/protocol_types.h"

namespace TeleMatrix {

/// Data model for a single row in the chat list.
/// Holds the room summary and cached text layout objects for painting.
class DialogsRow {
public:
    DialogsRow() = default;

    /// Update row data from a RoomSummary.
    void updateFrom(const RoomSummary &room);

    // --- Data accessors ---
    const QString &roomId() const { return _roomId; }
    const QString &displayName() const { return _displayName; }
    const QString &lastMessage() const { return _lastMessage; }
    const QString &lastSender() const { return _lastSender; }
    qint64 timestamp() const { return _timestamp; }
    int unreadCount() const { return _unreadCount; }
    void setUnreadCount(int count) { _unreadCount = count; }
    int highlightCount() const { return _highlightCount; }
    void setHighlightCount(int count) { _highlightCount = count; }
    bool hasMentionBadge() const { return _highlightCount > 0; }
    bool isMarkedUnread() const { return _isMarkedUnread; }
    void setMarkedUnread(bool marked) { _isMarkedUnread = marked; }
    /// True when the row has any unread indicator (count > 0 or marked unread).
    bool hasUnreadIndicator() const { return _unreadCount > 0 || _isMarkedUnread || hasMentionBadge(); }
    bool isMuted() const { return _isMuted; }
    void setMuted(bool muted) { _isMuted = muted; }
    RoomNotificationMode notificationMode() const { return _notificationMode; }
    void setNotificationMode(RoomNotificationMode mode) { _notificationMode = mode; _isMuted = (mode == RoomNotificationMode::Mute); }
    bool isPinned() const { return _isPinned; }
    void setPinned(bool pinned);
    int pinnedIndex() const { return _pinnedIndex; }
    void setPinnedIndex(int index) { _pinnedIndex = index; }
    /// The room's place among the pinned ones as the server has it (the
    /// `m.favourite` tag's `order`). Negative when the server holds none.
    double pinnedOrder() const { return _pinnedOrder; }
    void setPinnedOrder(double order) { _pinnedOrder = order; }
    bool isDirect() const { return _isDirect; }
    const QString &avatarUrl() const { return _avatarUrl; }
    const QString &avatarEntityId() const { return _avatarEntityId; }
    const QVector<int> &filterIds() const { return _filterIds; }
    void setFilterIds(const QVector<int> &ids);
    const QStringList &spaceIds() const { return _spaceIds; }
    void setSpaceIds(const QStringList &ids) { _spaceIds = ids; }
    bool isLastMessageOutgoing() const { return _isLastMessageOutgoing; }
    SendState lastMessageSendState() const { return _lastMessageSendState; }
    bool hasDraft() const { return _hasDraft; }
    const QString &draftText() const { return _draftText; }
    void setDraft(const QString &text);
    // Online (1) or Unavailable/Away (2) — both show the green dot.
    bool isPeerOnline() const { return _isDirect && _peerPresence >= 1; }
    int peerPresence() const { return _peerPresence; }
    void setPeerPresence(int state) { _peerPresence = state; }
    qreal onlineBadgeProgress() const { return _onlineBadgeProgress; }
    void setOnlineBadgeProgress(qreal p) { _onlineBadgeProgress = p; }
    MembershipState membership() const { return _membership; }
    const QString &inviterDisplayName() const { return _inviterDisplayName; }
    const QString &roomTopic() const { return _roomTopic; }

    // --- Layout cache ---
    /// Get or create the cached name text layout.
    Ui::Text::String &nameText();

    /// Get or create the cached message preview text layout.
    Ui::Text::String &messageText();

    /// Formatted timestamp string (e.g. "12:34", "Mon", "Jan 5").
    const QString &dateText() const { return _dateText; }

    /// Return the pre-computed lowercase searchable text.
    const QString &searchableText() const;

    /// Compute a search match score against a normalized query (lower = better).
    /// Returns -1 if no match.
    int searchScore(const QString &normalizedQuery) const;

    /// Row height in pixels (runtime-scaled via st::dialogsRowHeight).
    static int rowHeight();

private:
    void updateDateText();
    void invalidateTextCache();
    void invalidateSearchCache();

    QString _roomId;
    QString _displayName;
    QString _lastMessage;
    QString _lastSender;
    qint64 _timestamp = 0;
    int _unreadCount = 0;
    int _highlightCount = 0;
    bool _isMarkedUnread = false;
    bool _isMuted = false;
    RoomNotificationMode _notificationMode = RoomNotificationMode::AllMessages;
    bool _isPinned = false;
    int _pinnedIndex = 0;
    double _pinnedOrder = -1.0;
    bool _isDirect = false;
    QString _avatarUrl;
    QString _avatarEntityId;
    QVector<int> _filterIds;
    QStringList _spaceIds;
    bool _isLastMessageOutgoing = false;
    SendState _lastMessageSendState = SendState::Read;
    QString _draftText;
    bool _hasDraft = false;
    int _peerPresence = 0;
    qreal _onlineBadgeProgress = 0.0;
    MembershipState _membership = MembershipState::Join;
    QString _inviterDisplayName;
    QString _roomTopic;

    QString _dateText;

    // Cached text layout objects (lazy-initialized).
    std::optional<Ui::Text::String> _nameTextCache;
    std::optional<Ui::Text::String> _messageTextCache;

    // Cached searchable text (lowercase, lazy).
    mutable QString _searchableText;
    mutable bool _searchableTextDirty = true;
};

} // namespace TeleMatrix

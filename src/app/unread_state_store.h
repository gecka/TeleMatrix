// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "../protocol/protocol_types.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

namespace TeleMatrix {

struct UnreadRoomState {
	QString roomId;

	int serverUnreadCount = 0;
	int serverHighlightCount = 0;
	bool serverMarkedUnread = false;
	QString serverFirstUnreadEventId;
	qint64 serverLastActivityTs = 0; // last-message time from the latest room-list snapshot
	qint64 readFrontierTs = 0;       // last-activity time we have read up to (optimistic-read base)

	int effectiveUnreadCount = 0;
	int effectiveHighlightCount = 0;
	bool effectiveMarkedUnread = false;
	QString effectiveFirstUnreadEventId;

	// Presentation-only clamp of the effective count for the badge surfaces
	// (dialogs rows, folder counters, dock/tray total). Zeroed while this room
	// is the active read-consuming one, so a message arriving at the live bottom
	// never flashes the rooms-list badge before the auto-read zeroes it. The
	// effective* fields above stay truthful — the read detector and receipts
	// depend on them. See UnreadStateStore::setReadConsumingRoom.
	int displayUnreadCount = 0;
	int displayHighlightCount = 0;

	bool pendingExplicitMarkRead = false;
	bool pendingExplicitMarkUnread = false;
	quint64 pendingExplicitRevision = 0;
	quint64 pendingExplicitRequestId = 0;
	QString pendingReadTillEventId;
	QString pendingOptimisticFrontierEventId;
	int pendingOptimisticUnreadCount = -1;
	quint64 pendingReadRevision = 0;
	quint64 pendingReadRequestId = 0;
	QString lastAckedReceiptEventId;
	quint64 requestRevision = 0;

	RoomNotificationMode notificationMode = RoomNotificationMode::AllMessages;
	bool isMuted = false;
};

class UnreadStateStore : public QObject {
	Q_OBJECT

public:
	explicit UnreadStateStore(QObject *parent = nullptr);

	void setActiveRoomId(const QString &roomId);

	// Mark the room that is currently consuming reads instantly (window focused
	// and parked at the live bottom), or clear it with an empty id. That room's
	// display* counts are clamped to 0 so a fresh arrival never blinks the badge
	// while its auto-read is in flight. Presentation only — effective* is unchanged.
	void setReadConsumingRoom(const QString &roomId);

	void applyRoomListSnapshot(const QVector<RoomSummary> &rooms);
	void applyRoomUnreadSnapshot(
		const QString &roomId,
		const RoomUnreadSnapshot &snapshot);
	void applyTimelineSnapshot(const QString &roomId, const TimelineSlice &slice);
	void setRoomNotificationMode(
		const QString &roomId,
		RoomNotificationMode mode,
		bool isMuted);

	quint64 optimisticMarkRead(const QString &roomId);
	quint64 optimisticMarkUnread(const QString &roomId);
	quint64 optimisticReadProgress(
		const QString &roomId,
		const QString &readTillEventId,
		const QString &nextUnreadEventId,
		int newUnreadCount,
		qint64 readTillTs = 0);
	void optimisticClearOnSend(const QString &roomId);

	void bindExplicitRequest(
		const QString &roomId,
		quint64 revision,
		quint64 requestId);
	void bindReadReceiptRequest(
		const QString &roomId,
		const QString &eventId,
		quint64 revision,
		quint64 requestId);

	void ackMarkRead(
		const QString &roomId,
		bool read,
		quint64 requestId,
		bool success);
	void ackReadReceipt(
		const QString &roomId,
		const QString &eventId,
		quint64 requestId,
		bool success);

	[[nodiscard]] UnreadRoomState roomState(const QString &roomId) const;
	[[nodiscard]] QVector<UnreadRoomState> roomStates() const;
	[[nodiscard]] int totalUnreadCount(bool includeMuted) const;

Q_SIGNALS:
	void roomUnreadStateChanged(const QString &roomId);
	// Effective unread for a room dropped (read locally or — the case that
	// matters — on another device). Listeners dismiss the room's now-stale
	// desktop notifications.
	void roomReadProgressed(const QString &roomId);
	void totalUnreadChanged(int totalUnreadIncludingMuted);
	void activeRoomUnreadStateChanged(
		const QString &roomId,
		int unreadCount,
		const QString &firstUnreadEventId);

private:
	void recomputeEffectiveState(UnreadRoomState &state) const;
	// Derive display* from the final effective* (clamped to 0 for the active
	// read-consuming room). Called at every exit of recomputeEffectiveState.
	void clampDisplayCounts(UnreadRoomState &state) const;
	void emitStateChanges(
		const QString &roomId,
		const UnreadRoomState &before,
		const UnreadRoomState &after);
	void maybeEmitActiveRoomState(const QString &roomId, const UnreadRoomState &state);
	void logStateChange(
		const char *source,
		const QString &roomId,
		const UnreadRoomState &before,
		const UnreadRoomState &after) const;

	QHash<QString, UnreadRoomState> _rooms;
	QString _activeRoomId;
	QString _readConsumingRoomId; // room whose display count is clamped (see setReadConsumingRoom)
	int _lastTotalUnreadIncludingMuted = 0;
};

} // namespace TeleMatrix

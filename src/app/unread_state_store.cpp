// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "unread_state_store.h"

#include <QSet>

namespace TeleMatrix {

namespace {
bool StatePresentationEqual(const UnreadRoomState &a, const UnreadRoomState &b) {
	return a.serverUnreadCount == b.serverUnreadCount
		&& a.serverHighlightCount == b.serverHighlightCount
		&& a.serverMarkedUnread == b.serverMarkedUnread
		&& a.serverFirstUnreadEventId == b.serverFirstUnreadEventId
		&& a.effectiveUnreadCount == b.effectiveUnreadCount
		&& a.effectiveHighlightCount == b.effectiveHighlightCount
		&& a.effectiveMarkedUnread == b.effectiveMarkedUnread
		&& a.effectiveFirstUnreadEventId == b.effectiveFirstUnreadEventId
		&& a.displayUnreadCount == b.displayUnreadCount
		&& a.displayHighlightCount == b.displayHighlightCount
		&& a.pendingExplicitMarkRead == b.pendingExplicitMarkRead
		&& a.pendingExplicitMarkUnread == b.pendingExplicitMarkUnread
		&& a.pendingReadTillEventId == b.pendingReadTillEventId
		&& a.pendingOptimisticFrontierEventId == b.pendingOptimisticFrontierEventId
		&& a.pendingOptimisticUnreadCount == b.pendingOptimisticUnreadCount
		&& a.lastAckedReceiptEventId == b.lastAckedReceiptEventId
		&& a.notificationMode == b.notificationMode
		&& a.isMuted == b.isMuted;
}

void ClearPendingExplicitState(UnreadRoomState &state) {
	state.pendingExplicitMarkRead = false;
	state.pendingExplicitMarkUnread = false;
	state.pendingExplicitRevision = 0;
	state.pendingExplicitRequestId = 0;
}

void ClearPendingReadState(UnreadRoomState &state) {
	state.pendingReadTillEventId.clear();
	state.pendingOptimisticFrontierEventId.clear();
	state.pendingOptimisticUnreadCount = -1;
	state.pendingReadRevision = 0;
	state.pendingReadRequestId = 0;
}

void ClearAllPendingReadState(UnreadRoomState &state) {
	if (!state.pendingExplicitMarkRead
		&& !state.pendingExplicitMarkUnread
		&& !state.pendingExplicitRequestId
		&& !state.pendingExplicitRevision
		&& state.pendingReadTillEventId.isEmpty()
		&& state.pendingOptimisticFrontierEventId.isEmpty()
		&& state.pendingOptimisticUnreadCount < 0
		&& !state.pendingReadRevision
		&& !state.pendingReadRequestId) {
		return;
	}
	ClearPendingExplicitState(state);
	ClearPendingReadState(state);
}

bool MutedForBadge(const UnreadRoomState &state) {
	// Any non-default notification level counts as muted (Mentions-only as well as Mute).
	return roomCountsAsMuted(state.notificationMode, state.isMuted);
}

} // namespace

UnreadStateStore::UnreadStateStore(QObject *parent)
	: QObject(parent) {
}

void UnreadStateStore::setActiveRoomId(const QString &roomId) {
	if (_activeRoomId == roomId) {
		return;
	}
	_activeRoomId = roomId;
	// The read-consuming clamp only ever applies to the active room. When the
	// active room changes, drop a stale clamp so the room we just left shows its
	// real count at once (defensive — the widget re-evaluates the gate too, but
	// this covers a bare room close that never reaches the widget path).
	if (!_readConsumingRoomId.isEmpty() && _readConsumingRoomId != roomId) {
		setReadConsumingRoom(QString());
	}
	if (_activeRoomId.isEmpty()) {
		emit activeRoomUnreadStateChanged(QString(), 0, QString());
		return;
	}
	const auto state = roomState(_activeRoomId);
	emit activeRoomUnreadStateChanged(
		_activeRoomId,
		state.effectiveUnreadCount,
		state.effectiveFirstUnreadEventId);
}

void UnreadStateStore::setReadConsumingRoom(const QString &roomId) {
	if (_readConsumingRoomId == roomId) {
		return;
	}
	const auto previous = _readConsumingRoomId;
	_readConsumingRoomId = roomId;
	// Recompute the room leaving the clamp (reveal its real count) and the room
	// entering it (hide it); emitStateChanges no-ops unless the presentation
	// actually changed, and it also refreshes the badge total.
	const auto refresh = [this](const QString &id) {
		if (id.isEmpty()) {
			return;
		}
		auto it = _rooms.find(id);
		if (it == _rooms.end()) {
			return;
		}
		const auto before = it.value();
		recomputeEffectiveState(it.value());
		emitStateChanges(id, before, it.value());
	};
	refresh(previous);
	refresh(roomId);
}

void UnreadStateStore::applyRoomListSnapshot(const QVector<RoomSummary> &rooms) {
	QSet<QString> seen;
	for (const auto &room : rooms) {
		if (room.roomId.isEmpty()) {
			continue;
		}
		seen.insert(room.roomId);

		const auto before = _rooms.value(room.roomId);
		auto &state = _rooms[room.roomId];
		state.roomId = room.roomId;
		state.serverUnreadCount = room.unreadCount;
		state.serverHighlightCount = room.highlightCount;
		state.serverMarkedUnread = room.isMarkedUnread;
		state.notificationMode = room.notificationMode;
		state.isMuted = room.isMuted;

		// Local read state is preferred over the network: keep an optimistic
		// "read" across sync snapshots and failed read-receipts (no flicker).
		// Clear it only when the server confirms the read, or when genuinely new
		// activity arrives past the point we read up to (so a real new message
		// still raises the badge). A restart resyncs cleanly from the server.
		const bool serverConfirmedRead =
			(state.serverUnreadCount == 0 && !state.serverMarkedUnread);
		const bool newActivity = (room.timestamp > state.readFrontierTs);
		if (serverConfirmedRead || newActivity) {
			if (state.pendingExplicitMarkRead) {
				ClearPendingExplicitState(state);
			}
			ClearPendingReadState(state);
		}
		state.serverLastActivityTs = room.timestamp;

		if (state.pendingExplicitMarkUnread && state.serverMarkedUnread) {
			ClearPendingExplicitState(state);
		}

		recomputeEffectiveState(state);
		emitStateChanges(room.roomId, before, state);
	}

	for (auto it = _rooms.begin(); it != _rooms.end();) {
		if (seen.contains(it.key())) {
			++it;
			continue;
		}
		const auto removedRoomId = it.key();
		const auto before = it.value();
		it = _rooms.erase(it);
		emit roomUnreadStateChanged(removedRoomId);
		if (_activeRoomId == removedRoomId) {
			emit activeRoomUnreadStateChanged(removedRoomId, 0, QString());
		}
		logStateChange("room-list-remove", removedRoomId, before, UnreadRoomState{});
	}

	const auto totalUnread = totalUnreadCount(true);
	if (_lastTotalUnreadIncludingMuted != totalUnread) {
		_lastTotalUnreadIncludingMuted = totalUnread;
		emit totalUnreadChanged(totalUnread);
	}
}

void UnreadStateStore::applyRoomUnreadSnapshot(
		const QString &roomId,
		const RoomUnreadSnapshot &snapshot) {
	if (roomId.isEmpty()) {
		return;
	}

	const auto before = _rooms.value(roomId);
	auto &state = _rooms[roomId];
	state.roomId = roomId;
	state.serverUnreadCount = qMax(0, snapshot.unreadCount);
	state.serverHighlightCount = qMax(0, snapshot.highlightCount);
	state.serverMarkedUnread = snapshot.isMarkedUnread;
	state.notificationMode = snapshot.notificationMode;
	state.isMuted = snapshot.isMuted;
	// Per-room unread snapshot carries no last-activity timestamp, so it never
	// clears the optimistic read on its own (that would revert local state on a
	// stale/failed-receipt update). New-activity clearing happens in
	// applyRoomListSnapshot, which has the room's last-message time.

	if (state.serverUnreadCount <= 0) {
		state.serverFirstUnreadEventId.clear();
	}

	if (state.pendingExplicitMarkRead
		&& state.serverUnreadCount == 0
		&& !state.serverMarkedUnread) {
		ClearPendingExplicitState(state);
		ClearPendingReadState(state);
	}
	if (state.pendingExplicitMarkUnread && state.serverMarkedUnread) {
		ClearPendingExplicitState(state);
	}

	recomputeEffectiveState(state);
	emitStateChanges(roomId, before, state);
}

void UnreadStateStore::applyTimelineSnapshot(
	const QString &roomId,
	const TimelineSlice &slice) {
	if (roomId.isEmpty()) {
		return;
	}
	if (!slice.unreadStateKnown) {
		return;
	}

	const auto before = _rooms.value(roomId);
	auto &state = _rooms[roomId];
	state.roomId = roomId;

	// Trust the freshly-recomputed slice count in both directions. Rust
	// recomputes slice.unreadCount from scratch on every slice, so a stale-high
	// serverUnreadCount must never ratchet a genuine reduction back up — that
	// max() was what pinned the badge until the room was reopened. The
	// pending-read clears below still own optimistic reconciliation.
	state.serverUnreadCount = qMax(0, slice.unreadCount);
	if (state.serverUnreadCount > 0) {
		if (!slice.firstUnreadEventId.isEmpty()) {
			state.serverFirstUnreadEventId = slice.firstUnreadEventId;
		}
	} else {
		state.serverFirstUnreadEventId.clear();
	}

	if (state.pendingExplicitMarkRead && slice.unreadCount == 0) {
		ClearPendingExplicitState(state);
		ClearPendingReadState(state);
	}
	if (state.pendingOptimisticUnreadCount >= 0
		&& slice.unreadCount <= state.pendingOptimisticUnreadCount) {
		ClearPendingReadState(state);
	}

	recomputeEffectiveState(state);
	emitStateChanges(roomId, before, state);

	const auto totalUnread = totalUnreadCount(true);
	if (_lastTotalUnreadIncludingMuted != totalUnread) {
		_lastTotalUnreadIncludingMuted = totalUnread;
		emit totalUnreadChanged(totalUnread);
	}
}

void UnreadStateStore::setRoomNotificationMode(
		const QString &roomId,
		RoomNotificationMode mode,
		bool isMuted) {
	if (roomId.isEmpty()) {
		return;
	}
	const auto before = _rooms.value(roomId);
	auto &state = _rooms[roomId];
	state.roomId = roomId;
	state.notificationMode = mode;
	state.isMuted = isMuted;
	recomputeEffectiveState(state);
	emitStateChanges(roomId, before, state);
}

quint64 UnreadStateStore::optimisticMarkRead(const QString &roomId) {
	if (roomId.isEmpty()) {
		return 0;
	}
	const auto before = _rooms.value(roomId);
	auto &state = _rooms[roomId];
	state.roomId = roomId;
	++state.requestRevision;
	state.pendingExplicitRevision = state.requestRevision;
	state.pendingExplicitRequestId = 0;
	state.pendingExplicitMarkRead = true;
	state.pendingExplicitMarkUnread = false;
	state.readFrontierTs = state.serverLastActivityTs; // read up to the latest seen activity
	ClearPendingReadState(state);
	recomputeEffectiveState(state);
	emitStateChanges(roomId, before, state);
	return state.requestRevision;
}

quint64 UnreadStateStore::optimisticMarkUnread(const QString &roomId) {
	if (roomId.isEmpty()) {
		return 0;
	}
	const auto before = _rooms.value(roomId);
	auto &state = _rooms[roomId];
	state.roomId = roomId;
	++state.requestRevision;
	state.pendingExplicitRevision = state.requestRevision;
	state.pendingExplicitRequestId = 0;
	state.pendingExplicitMarkUnread = true;
	state.pendingExplicitMarkRead = false;
	recomputeEffectiveState(state);
	emitStateChanges(roomId, before, state);
	return state.requestRevision;
}

quint64 UnreadStateStore::optimisticReadProgress(
	const QString &roomId,
	const QString &readTillEventId,
	const QString &nextUnreadEventId,
	int newUnreadCount,
	qint64 readTillTs) {
	if (roomId.isEmpty()) {
		return 0;
	}
	// A raise (no event actually read → empty read-till) is not a read: it must
	// not mint pending read state or advance the read frontier. The raised count
	// reaches the badge through the server-count feeds (applyTimelineSnapshot /
	// applyRoomListSnapshot); minting pending here only re-stamped readFrontierTs
	// and polluted state.
	if (readTillEventId.isEmpty()) {
		return 0;
	}
	const auto before = _rooms.value(roomId);
	auto &state = _rooms[roomId];
	state.roomId = roomId;
	++state.requestRevision;
	// Stamp the frontier at the timestamp of the message actually read, not the
	// latest *seen* activity — otherwise the debounced room-list snapshot that
	// carries this very message classifies it as new activity and reverts the
	// optimistic read (the blink). Never the latest activity either: reading up to
	// an OLDER message would then claim everything newer as read too, and since
	// newActivity is the only thing that releases the qMin pin in
	// recomputeEffectiveState, the badge stays stuck at the optimistic value until
	// something newer still arrives. That is reachable whenever readTillTs is 0 —
	// the read-till event is not in the loaded list, which the "hide system
	// messages in public rooms" filter causes routinely. qMax against the existing
	// frontier keeps a 0/absent ts from regressing what a prior read established.
	// (Explicit mark-read does stamp the latest activity: there the claim is true.)
	state.readFrontierTs = qMax(state.readFrontierTs, readTillTs);
	state.pendingReadTillEventId = readTillEventId;
	state.pendingOptimisticFrontierEventId = nextUnreadEventId;
	state.pendingOptimisticUnreadCount = qMax(0, newUnreadCount);
	state.pendingReadRevision = state.requestRevision;
	state.pendingReadRequestId = 0;
	recomputeEffectiveState(state);
	emitStateChanges(roomId, before, state);
	return state.requestRevision;
}

void UnreadStateStore::optimisticClearOnSend(const QString &roomId) {
	optimisticMarkRead(roomId);
}

void UnreadStateStore::bindExplicitRequest(
	const QString &roomId,
	quint64 revision,
	quint64 requestId) {
	if (roomId.isEmpty() || revision == 0 || requestId == 0) {
		return;
	}
	auto it = _rooms.find(roomId);
	if (it == _rooms.end()) {
		return;
	}
	auto &state = it.value();
	if (!(state.pendingExplicitMarkRead || state.pendingExplicitMarkUnread)
		|| state.pendingExplicitRevision != revision) {
		return;
	}
	state.pendingExplicitRequestId = requestId;
}

void UnreadStateStore::bindReadReceiptRequest(
	const QString &roomId,
	const QString &eventId,
	quint64 revision,
	quint64 requestId) {
	if (roomId.isEmpty()
		|| eventId.isEmpty()
		|| revision == 0
		|| requestId == 0) {
		return;
	}
	auto it = _rooms.find(roomId);
	if (it == _rooms.end()) {
		return;
	}
	auto &state = it.value();
	if (state.pendingReadRevision != revision
		|| state.pendingReadTillEventId != eventId) {
		return;
	}
	state.pendingReadRequestId = requestId;
}

void UnreadStateStore::ackMarkRead(
	const QString &roomId,
	bool read,
	quint64 requestId,
	bool success) {
	if (roomId.isEmpty()) {
		return;
	}
	auto it = _rooms.find(roomId);
	if (it == _rooms.end()) {
		return;
	}
	auto &state = it.value();
	const bool actionMatches = read
		? state.pendingExplicitMarkRead
		: state.pendingExplicitMarkUnread;
	if (!actionMatches) {
		return;
	}
	if (requestId != 0
		&& state.pendingExplicitRequestId != 0
		&& requestId != state.pendingExplicitRequestId) {
		return;
	}
	if (success) {
		if (read) {
			const auto before = state;
			state.pendingExplicitMarkRead = false;
			state.pendingExplicitRevision = 0;
			state.pendingExplicitRequestId = 0;
			recomputeEffectiveState(state);
			emitStateChanges(roomId, before, state);
		} else if (requestId != 0 && state.pendingExplicitRequestId != 0) {
			state.pendingExplicitRequestId = 0;
		}
		return;
	}

	const auto before = state;
	ClearPendingExplicitState(state);
	recomputeEffectiveState(state);
	emitStateChanges(roomId, before, state);
}

void UnreadStateStore::ackReadReceipt(
	const QString &roomId,
	const QString &eventId,
	quint64 requestId,
	bool success) {
	if (roomId.isEmpty() || eventId.isEmpty()) {
		return;
	}
	auto it = _rooms.find(roomId);
	if (it == _rooms.end()) {
		return;
	}
	auto &state = it.value();
	if (state.pendingReadTillEventId != eventId) {
		return;
	}
	if (requestId != 0
		&& state.pendingReadRequestId != 0
		&& requestId != state.pendingReadRequestId) {
		return;
	}
	if (success) {
		state.lastAckedReceiptEventId = eventId;
		if (requestId != 0 && requestId == state.pendingReadRequestId) {
			state.pendingReadRequestId = 0;
		}
		return;
	}

	const auto before = state;
	ClearPendingReadState(state);
	recomputeEffectiveState(state);
	emitStateChanges(roomId, before, state);
}

UnreadRoomState UnreadStateStore::roomState(const QString &roomId) const {
	return _rooms.value(roomId);
}

QVector<UnreadRoomState> UnreadStateStore::roomStates() const {
	QVector<UnreadRoomState> result;
	result.reserve(_rooms.size());
	for (auto it = _rooms.cbegin(); it != _rooms.cend(); ++it) {
		result.push_back(it.value());
	}
	return result;
}

int UnreadStateStore::totalUnreadCount(bool includeMuted) const {
	int total = 0;
	for (auto it = _rooms.cbegin(); it != _rooms.cend(); ++it) {
		const auto &state = it.value();
		if (!includeMuted && MutedForBadge(state)) {
			continue;
		}
		total += state.displayUnreadCount;
	}
	return total;
}

void UnreadStateStore::recomputeEffectiveState(UnreadRoomState &state) const {
	state.effectiveUnreadCount = state.serverUnreadCount;
	state.effectiveHighlightCount = state.serverHighlightCount;
	state.effectiveMarkedUnread = state.serverMarkedUnread;
	state.effectiveFirstUnreadEventId = state.serverFirstUnreadEventId;

	if (state.pendingExplicitMarkRead) {
		state.effectiveUnreadCount = 0;
		state.effectiveHighlightCount = 0;
		state.effectiveMarkedUnread = false;
		state.effectiveFirstUnreadEventId.clear();
		clampDisplayCounts(state);
		return;
	}

	if (state.pendingOptimisticUnreadCount >= 0) {
		// Optimistic masks a stale-high server count, but a genuinely lower
		// server count (e.g. another device read further) must still win — take
		// the min instead of letting the optimistic value pin a stale-high one.
		state.effectiveUnreadCount = qMin(
			state.pendingOptimisticUnreadCount,
			state.serverUnreadCount);
		if (state.effectiveUnreadCount == 0) {
			state.effectiveHighlightCount = 0;
			state.effectiveFirstUnreadEventId.clear();
		} else if (!state.pendingOptimisticFrontierEventId.isEmpty()) {
			state.effectiveFirstUnreadEventId = state.pendingOptimisticFrontierEventId;
		}
	}

	if (state.pendingExplicitMarkUnread) {
		state.effectiveMarkedUnread = true;
	}

	if (state.effectiveUnreadCount <= 0) {
		state.effectiveUnreadCount = 0;
		state.effectiveFirstUnreadEventId.clear();
	}

	clampDisplayCounts(state);
}

void UnreadStateStore::clampDisplayCounts(UnreadRoomState &state) const {
	// The active read-consuming room shows no unread count: a message landing at
	// its live bottom is auto-read immediately, so the raise must never reach the
	// badge. effective* stays truthful (the read detector and receipts depend on
	// it) — only the badge surfaces read display*. The marked-unread flag is
	// deliberately not clamped: it is explicit user intent.
	if (!_readConsumingRoomId.isEmpty() && state.roomId == _readConsumingRoomId) {
		state.displayUnreadCount = 0;
		state.displayHighlightCount = 0;
	} else {
		state.displayUnreadCount = state.effectiveUnreadCount;
		state.displayHighlightCount = state.effectiveHighlightCount;
	}
}

void UnreadStateStore::emitStateChanges(
	const QString &roomId,
	const UnreadRoomState &before,
	const UnreadRoomState &after) {
	if (StatePresentationEqual(before, after)) {
		return;
	}
	emit roomUnreadStateChanged(roomId);
	// A downward step means we've read further in this room. Surfacing a new
	// message is an *increase*, so this never fires on the snapshot that first
	// raised a toast — only when a later sync (e.g. a read receipt from another
	// device) advances our read marker.
	if (after.effectiveUnreadCount < before.effectiveUnreadCount) {
		emit roomReadProgressed(roomId);
	}
	maybeEmitActiveRoomState(roomId, after);

	const auto totalUnread = totalUnreadCount(true);
	const auto muteBadgePolicyChanged = before.notificationMode != after.notificationMode
		|| before.isMuted != after.isMuted;
	if (_lastTotalUnreadIncludingMuted != totalUnread || muteBadgePolicyChanged) {
		_lastTotalUnreadIncludingMuted = totalUnread;
		emit totalUnreadChanged(totalUnread);
	}

	logStateChange("state-change", roomId, before, after);
}

void UnreadStateStore::maybeEmitActiveRoomState(
	const QString &roomId,
	const UnreadRoomState &state) {
	if (roomId != _activeRoomId) {
		return;
	}
	emit activeRoomUnreadStateChanged(
		roomId,
		state.effectiveUnreadCount,
		state.effectiveFirstUnreadEventId);
}

void UnreadStateStore::logStateChange(
	const char *source [[maybe_unused]],
	const QString &roomId [[maybe_unused]],
	const UnreadRoomState &before [[maybe_unused]],
	const UnreadRoomState &after [[maybe_unused]]) const {
}

} // namespace TeleMatrix

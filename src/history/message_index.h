// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QHash>
#include <QString>
#include <QVector>

namespace TeleMatrix {

/// Maps a message event id to its physical index in the timeline vector.
///
/// Prepending older history shifts every existing message's physical index, but
/// re-inserting the whole map on each back-pagination is O(n) and dominates the
/// scroll cost on long histories. Instead this stores a *logical* value
/// (`physical + base`) per id and adjusts a single `base` offset on prepend, so
/// prepend touches only the newly added front entries — O(prepended), not O(n).
/// `physicalIndexOf` resolves back to the real index via `logical - base`.
class MessageIndex {
public:
    void clear();

    /// Rebuild from scratch in visual order. Empty ids are skipped but still
    /// occupy their physical slot. Resets the base offset.
    void rebuild(const QVector<QString> &eventIds);

    /// Record `count = frontIds.size()` messages prepended at the front, in
    /// visual order. Existing entries shift implicitly; only the new front
    /// entries are inserted. Empty ids are skipped but counted in the shift.
    void prependFront(const QVector<QString> &frontIds);

    /// Insert or replace the mapping for `eventId` at a known physical index
    /// (e.g. a message appended at the back, or a local-echo id replaced by the
    /// server id at the same slot).
    void setAt(const QString &eventId, int physicalIndex);

    /// Drop the mapping for `eventId`. Does not re-index later messages; callers
    /// that shift positions should `rebuild` afterwards.
    void remove(const QString &eventId);

    /// Physical index of `eventId`, or -1 if absent.
    [[nodiscard]] int physicalIndexOf(const QString &eventId) const;

    [[nodiscard]] bool contains(const QString &eventId) const;

private:
    QHash<QString, int> _logical; // eventId -> physicalIndex + _base
    int _base = 0;
};

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "dialogs/room_directory_filter.h"

using namespace TeleMatrix;

namespace {

RoomDirectoryEntry entry(const QString &name, const QString &topic) {
    RoomDirectoryEntry e;
    e.roomId = QStringLiteral("!") + name.toLower() + QStringLiteral(":localhost");
    e.name = name;
    e.topic = topic;
    return e;
}

QStringList namesOf(const QVector<RoomDirectoryEntry> &entries) {
    QStringList names;
    for (const auto &e : entries) {
        names.append(e.name);
    }
    return names;
}

} // namespace

// Matrix has no server-side search inside a space, so this local filter is the *whole* search
// mechanism for a space's rooms. That makes its edge cases worth pinning.
class TestRoomDirectoryFilter : public QObject {
    Q_OBJECT

private:
    QVector<RoomDirectoryEntry> sample() const {
        return {
            entry(QStringLiteral("Design"), QStringLiteral("Mockups and critique")),
            entry(QStringLiteral("Governance"), QStringLiteral("Decisions and voting")),
            entry(QStringLiteral("Random"), QString()),
        };
    }

private slots:
    void emptyNeedleKeepsEverything() {
        QCOMPARE(filterRoomEntries(sample(), QString()).size(), 3);
        // Whitespace is not a search term.
        QCOMPARE(filterRoomEntries(sample(), QStringLiteral("   ")).size(), 3);
    }

    void matchesNameCaseInsensitively() {
        const auto result = filterRoomEntries(sample(), QStringLiteral("dEsIgN"));
        QCOMPARE(namesOf(result), QStringList{ QStringLiteral("Design") });
    }

    void matchesTopicNotJustName() {
        // "voting" appears only in Governance's topic — the whole point of searching descriptions.
        const auto result = filterRoomEntries(sample(), QStringLiteral("voting"));
        QCOMPARE(namesOf(result), QStringList{ QStringLiteral("Governance") });
    }

    void matchesAcrossBothFields() {
        // One row matches by name, a different one by topic — a single needle must catch both.
        const QVector<RoomDirectoryEntry> rooms = {
            entry(QStringLiteral("Photos"), QStringLiteral("Share your snapshots")),
            entry(QStringLiteral("Snapshots archive"), QString()),
            entry(QStringLiteral("Unrelated"), QStringLiteral("Nothing to see")),
        };
        const auto result = filterRoomEntries(rooms, QStringLiteral("snapshot"));
        QCOMPARE(
            namesOf(result),
            (QStringList{ QStringLiteral("Photos"), QStringLiteral("Snapshots archive") }));
    }

    void noMatchYieldsEmpty() {
        QVERIFY(filterRoomEntries(sample(), QStringLiteral("zzz")).isEmpty());
    }

    void entryWithNoTopicStillMatchesByName() {
        const auto result = filterRoomEntries(sample(), QStringLiteral("random"));
        QCOMPARE(namesOf(result), QStringList{ QStringLiteral("Random") });
    }
};

QTEST_GUILESS_MAIN(TestRoomDirectoryFilter)
#include "tst_room_directory_filter.moc"

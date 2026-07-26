// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "history/unread_bar_placement.h"

using namespace TeleMatrix;
using Action = UnreadBar::Action;

class TestUnreadBarPlacement : public QObject {
    Q_OBJECT
private slots:
    // resolveAnchor: the frozen anchor always wins once captured; only before a
    // freeze does the (drifting) frontier drive placement.
    void resolveAnchorPrefersFrozen() {
        QCOMPARE(
            UnreadBar::resolveAnchor(QStringLiteral("U1"), QStringLiteral("U9")),
            QStringLiteral("U1"));
    }
    void resolveAnchorUsesFrontierBeforeFreeze() {
        QCOMPARE(
            UnreadBar::resolveAnchor(QString(), QStringLiteral("U9")),
            QStringLiteral("U9"));
    }
    void resolveAnchorEmptyWhenNeither() {
        QVERIFY(UnreadBar::resolveAnchor(QString(), QString()).isEmpty());
    }

    // decide(): unread count > 0.
    void keepsDrawnWhenLoadedAndUnread() {
        QCOMPARE(UnreadBar::decide(QStringLiteral("U1"), true, 3), Action::KeepDrawn);
    }
    void placesWhenNotDrawnAndUnread() {
        QCOMPARE(UnreadBar::decide(QString(), false, 3), Action::Place);
    }
    void placesWhenDrawnButUnloadedAndUnread() {
        // Drawn id set but its message no longer loaded → not "has drawn bar" →
        // (re)place at the frozen anchor (placeUnreadBar no-ops until reload).
        QCOMPARE(UnreadBar::decide(QStringLiteral("U1"), false, 3), Action::Place);
    }

    // decide(): unread count <= 0.
    void keepsCrossedBarAtZero() {
        QCOMPARE(UnreadBar::decide(QStringLiteral("U1"), true, 0), Action::KeepDrawn);
    }
    void clearsDrawnButKeepsAnchorWhenUnloadedAtZero() {
        // THE fix: count hit zero and the anchor's message is unloaded → drop
        // the visible bar only; the frozen anchor is preserved (no ClearAll).
        QCOMPARE(
            UnreadBar::decide(QStringLiteral("U1"), false, 0),
            Action::ClearDrawnKeepAnchor);
    }
    void noopWhenNothingDrawnAtZero() {
        QCOMPARE(UnreadBar::decide(QString(), false, 0), Action::NoOp);
    }

    // The reported bug, end to end at the decision level: read some (bar frozen
    // + drawn at U1) → a Full-replace drops U1 from the loaded window → the
    // count reaches 0 → the frozen anchor must be preserved → the next message
    // re-places at the FROZEN U1, never at the drifted-down frontier U9.
    void readThenNewMessageDoesNotSlideDown() {
        const auto frozen = QStringLiteral("U1");
        const auto driftedFrontier = QStringLiteral("U9");

        // Count 0 with U1 unloaded: keep the anchor (only undraw the bar).
        QCOMPARE(
            UnreadBar::decide(QStringLiteral("U1"), /*drawnLoaded=*/false, 0),
            Action::ClearDrawnKeepAnchor);

        // A new message arrives: bar not currently drawn, count > 0 → Place.
        QCOMPARE(
            UnreadBar::decide(QString(), /*drawnLoaded=*/false, 1),
            Action::Place);

        // Place resolves to the frozen anchor, never the drifted frontier.
        QCOMPARE(UnreadBar::resolveAnchor(frozen, driftedFrontier), frozen);
    }

    // detectorAnchor(): read *detection* follows the advancing frontier — the
    // mirror image of resolveAnchor(), which freezes the visual bar. Decoupling
    // them is what lets scroll-reading keep decrementing the count while the
    // delimiter stays put (or is absent).
    void detectorUsesFrontierWhenBarAbsent() {
        // THE dead case: the visual bar was consumed (down-button / send) or the
        // room opened at 0 unread, so drawnBar is empty — detection must still
        // anchor at the live frontier.
        QCOMPARE(
            UnreadBar::detectorAnchor(QStringLiteral("U5"), QString()),
            QStringLiteral("U5"));
    }
    void detectorPrefersFrontierOverBar() {
        // Frontier has advanced past the frozen entry bar: detection follows the
        // frontier, NOT the (older, frozen) drawn bar.
        QCOMPARE(
            UnreadBar::detectorAnchor(QStringLiteral("U5"), QStringLiteral("U1")),
            QStringLiteral("U5"));
    }
    void detectorFallsBackToEntryBar() {
        // Before the first frontier push (initial entry) detection anchors at
        // the frozen entry bar.
        QCOMPARE(
            UnreadBar::detectorAnchor(QString(), QStringLiteral("U1")),
            QStringLiteral("U1"));
    }
    void detectorOffWhenBothEmpty() {
        QVERIFY(UnreadBar::detectorAnchor(QString(), QString()).isEmpty());
    }

    // canPlaceUnreadBarAt(): the anti-"jumping-bar" heuristic and its
    // boundary-confirmed short-circuit (the "hide system messages" fix).
    //
    // Normal room: a visible regular message precedes the anchor → placeable.
    void placeWhenRegularMessagePrecedesAnchor() {
        const QVector<UnreadBar::PlacementRow> rows{
            { QStringLiteral("R1"), false }, // regular read message
            { QStringLiteral("U1"), false }, // anchor (first unread)
            { QStringLiteral("U2"), false },
        };
        QVERIFY(UnreadBar::canPlaceUnreadBarAt(rows, QStringLiteral("U1"), false));
    }
    // Unconfirmed boundary, window starts at the anchor (nothing regular before
    // it) → withheld, so entry pages back to the true boundary. Unchanged.
    void withholdWhenWindowStartsAtAnchorUnconfirmed() {
        const QVector<UnreadBar::PlacementRow> rows{
            { QStringLiteral("U1"), false }, // anchor is the first loaded row
            { QStringLiteral("U2"), false },
        };
        QVERIFY(!UnreadBar::canPlaceUnreadBarAt(rows, QStringLiteral("U1"), false));
    }
    // Only (now-hidden) service messages precede the anchor, boundary NOT
    // confirmed → still withheld (they were erased, so nothing regular precedes).
    void withholdWhenOnlyServicePrecedesUnconfirmed() {
        const QVector<UnreadBar::PlacementRow> rows{
            { QStringLiteral("S1"), true }, // membership change (in a normal room)
            { QStringLiteral("U1"), false },
        };
        QVERIFY(!UnreadBar::canPlaceUnreadBarAt(rows, QStringLiteral("U1"), false));
    }
    // THE FIX: read boundary confirmed loaded → place immediately even though the
    // filtered window starts at the anchor (the read region was service events
    // hidden by the public-room filter).
    void placeWhenBoundaryConfirmedEvenIfWindowStartsAtAnchor() {
        const QVector<UnreadBar::PlacementRow> rows{
            { QStringLiteral("U1"), false }, // anchor at the top of the filtered window
            { QStringLiteral("U2"), false },
        };
        QVERIFY(UnreadBar::canPlaceUnreadBarAt(rows, QStringLiteral("U1"), true));
    }
    void anchorNotLoadedIsNeverPlaceable() {
        const QVector<UnreadBar::PlacementRow> rows{
            { QStringLiteral("R1"), false },
            { QStringLiteral("R2"), false },
        };
        QVERIFY(!UnreadBar::canPlaceUnreadBarAt(rows, QStringLiteral("U1"), false));
        QVERIFY(!UnreadBar::canPlaceUnreadBarAt(rows, QStringLiteral("U1"), true));
    }
    void emptyAnchorIsNeverPlaceable() {
        const QVector<UnreadBar::PlacementRow> rows{
            { QStringLiteral("R1"), false },
        };
        QVERIFY(!UnreadBar::canPlaceUnreadBarAt(rows, QString(), true));
    }
};

QTEST_APPLESS_MAIN(TestUnreadBarPlacement)
#include "tst_unread_bar_placement.moc"

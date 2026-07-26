// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include <QtTest>

#include "history/trust_shield.h"
#include "protocol/protocol_types.h"

using namespace TeleMatrix;

// The shield glyph itself is verified visually; this covers the decision that
// gates a false-positive "verified" badge — the discriminant crosses the FFI
// boundary as a raw int, so an out-of-range value must render nothing, never a
// green shield.
class TestTrustShield : public QObject {
    Q_OBJECT
private slots:
    void verifiedViolationAndWarningRender() {
        QVERIFY(trustShieldVisible(UserTrustState::Verified));
        QVERIFY(trustShieldVisible(UserTrustState::Violation));
        QVERIFY(trustShieldVisible(UserTrustState::VerifiedWithWarning));
    }

    void unverifiedRendersNothing() {
        QVERIFY(!trustShieldVisible(UserTrustState::Unverified));
    }

    void outOfRangeRendersNothing() {
        // Anything the enum's named values don't cover must be treated as
        // "no shield", not fall through to the green/"good" branch.
        for (const int raw : {4, 7, 42, -1, 255}) {
            const auto state = static_cast<UserTrustState>(raw);
            QVERIFY2(!trustShieldVisible(state),
                     qPrintable(QStringLiteral("raw=%1 must not render").arg(raw)));
        }
    }
};

QTEST_APPLESS_MAIN(TestTrustShield)
#include "tst_trust_shield.moc"

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "trust_shield.h"

#include <QCoreApplication>
#include <QPainter>
#include <QPainterPath>

#include "styles/style_constants.h"

namespace TeleMatrix {

QString trustBadgeText(UserTrustState state) {
    switch (state) {
    case UserTrustState::Verified:
        return QCoreApplication::translate("TeleMatrix::TrustBadge", "verified");
    case UserTrustState::VerifiedWithWarning:
        return QCoreApplication::translate(
            "TeleMatrix::TrustBadge", "unverified sessions");
    case UserTrustState::Violation:
        return QCoreApplication::translate(
            "TeleMatrix::TrustBadge", "identity changed");
    default:
        return QString();
    }
}

QString trustDescription(UserTrustState state, bool isRoom) {
    if (isRoom) {
        switch (state) {
        case UserTrustState::Verified:
            return QCoreApplication::translate(
                "TeleMatrix::TrustBadge", "All members' identities are verified.");
        case UserTrustState::VerifiedWithWarning:
            return QCoreApplication::translate(
                "TeleMatrix::TrustBadge",
                "Some users have one or more unverified sessions.");
        case UserTrustState::Violation:
            return QCoreApplication::translate(
                "TeleMatrix::TrustBadge",
                "Some users' identities have changed since you verified them.");
        default:
            return QString();
        }
    }
    switch (state) {
    case UserTrustState::Verified:
        return QCoreApplication::translate(
            "TeleMatrix::TrustBadge", "You have verified this user's identity.");
    case UserTrustState::VerifiedWithWarning:
        return QCoreApplication::translate(
            "TeleMatrix::TrustBadge",
            "Verified, but this user has one or more unverified sessions.");
    case UserTrustState::Violation:
        return QCoreApplication::translate(
            "TeleMatrix::TrustBadge",
            "This user's identity has changed since you verified them.");
    default:
        return QString();
    }
}

QColor trustBadgeColor(UserTrustState state) {
    switch (state) {
    case UserTrustState::Verified: return st::boxTextFgGood;
    case UserTrustState::Violation: return st::boxTextFgError;
    case UserTrustState::VerifiedWithWarning: return QColor(0xC8, 0x8A, 0x00);
    default: return QColor();
    }
}

void paintTrustShield(QPainter &p, const QRect &r, UserTrustState state) {
    if (!trustShieldVisible(state)) {
        return;
    }
    QColor color;
    switch (state) {
    case UserTrustState::Verified: color = st::boxTextFgGood; break;
    case UserTrustState::Violation: color = st::boxTextFgError; break;
    // Identity verified but the user has an unverified session: amber caution,
    // distinct from the red identity-change violation.
    case UserTrustState::VerifiedWithWarning: color = st::trustWarningFg; break;
    default: return;
    }
    const qreal x = r.x(), y = r.y(), w = r.width(), h = r.height();

    QPainterPath shield;
    shield.moveTo(x, y + h * 0.12);
    shield.quadTo(x + w * 0.5, y, x + w, y + h * 0.12);
    shield.lineTo(x + w, y + h * 0.52);
    shield.quadTo(x + w, y + h * 0.84, x + w * 0.5, y + h);
    shield.quadTo(x, y + h * 0.84, x, y + h * 0.52);
    shield.closeSubpath();

    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawPath(shield);

    QPen glyphPen(st::windowBg);
    glyphPen.setWidthF(qMax(1.5, h * 0.11));
    glyphPen.setCapStyle(Qt::RoundCap);
    glyphPen.setJoinStyle(Qt::RoundJoin);
    p.setPen(glyphPen);
    p.setBrush(Qt::NoBrush);
    if (state == UserTrustState::Verified) {
        QPainterPath check;
        check.moveTo(x + w * 0.30, y + h * 0.46);
        check.lineTo(x + w * 0.44, y + h * 0.60);
        check.lineTo(x + w * 0.72, y + h * 0.30);
        p.drawPath(check);
    } else {
        p.drawLine(
            QPointF(x + w * 0.5, y + h * 0.26),
            QPointF(x + w * 0.5, y + h * 0.52));
        p.setPen(Qt::NoPen);
        p.setBrush(st::windowBg);
        const qreal dotR = qMax(1.0, h * 0.06);
        p.drawEllipse(QPointF(x + w * 0.5, y + h * 0.64), dotR, dotR);
    }
    p.restore();
}

} // namespace TeleMatrix

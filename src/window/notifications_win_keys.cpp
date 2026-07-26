// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "window/notifications_win_keys.h"

namespace TeleMatrix::Notifications {

namespace {

// FNV-1a/64 over UTF-8 bytes, formatted as zero-padded lowercase hex (always 16
// chars, fitting the WinRT Tag/Group limit). Deterministic so the key a toast is
// shown with matches the one it is later cleared with.
QString fnv1a64Hex(const QString &value) {
    const QByteArray bytes = value.toUtf8();
    quint64 hash = 0xcbf29ce484222325ULL; // FNV offset basis
    for (const char c : bytes) {
        hash ^= static_cast<quint8>(c);
        hash *= 0x100000001b3ULL; // FNV prime
    }
    return QStringLiteral("%1").arg(hash, 16, 16, QLatin1Char('0'));
}

} // namespace

QString toastGroupKey(const QString &roomId) {
    return fnv1a64Hex(roomId);
}

QString toastTagKey(const QString &eventId) {
    return fnv1a64Hex(eventId);
}

} // namespace TeleMatrix::Notifications

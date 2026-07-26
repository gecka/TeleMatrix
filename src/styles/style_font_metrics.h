// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QFont>
#include <QFontMetrics>
#include <QHash>

namespace st {

/// Cached QFontMetrics for st:: fonts.  Since these fonts are fixed at
/// startup, the cache never needs invalidation.  Thread-local to avoid
/// mutex overhead (all paint/layout runs on the main thread anyway).
inline const QFontMetrics &fontMetrics(const QFont &font) {
    thread_local QHash<QFont, QFontMetrics> cache;
    auto it = cache.find(font);
    if (it == cache.end()) {
        it = cache.insert(font, QFontMetrics(font));
    }
    return it.value();
}

} // namespace st

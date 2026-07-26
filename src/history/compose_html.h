// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QString>
#include <QTextFormat>

class QTextDocument;

namespace TeleMatrix {

// Custom QTextCharFormat properties stamped onto composer mention runs.
inline constexpr int kMentionUserIdProperty = QTextFormat::UserProperty + 1;
inline constexpr int kRoomMentionProperty = QTextFormat::UserProperty + 2;

// Serialize a compose QTextDocument into clean Matrix formatted-body HTML
// (no QTextEdit style boilerplate). Consecutive quote/pre blocks each coalesce
// into a single <blockquote>/<pre><code> so a multi-line quote or code block
// is one element, not several one-line boxes.
[[nodiscard]] QString buildCleanHtml(const QTextDocument *doc);

} // namespace TeleMatrix

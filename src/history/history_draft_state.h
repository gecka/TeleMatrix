// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QHash>
#include <QString>

namespace TeleMatrix {

class HistoryInput;

struct HistoryDraft {
    QString text;
    QString html;
    bool replyMode = false;
    QString replyEventId;
    QString replySenderName;
    QString replyPreviewText;
    QString replyPreviewPath;
    bool editMode = false;
    QString editEventId;
    QString editSenderName;
    QString editPreviewText;
};

class HistoryDraftStore {
public:
    // Full capture, including QTextEdit::toHtml(). Use only where the HTML is
    // actually read back (room switch-away / close), not per keystroke.
    [[nodiscard]] static HistoryDraft capture(const HistoryInput &input);
    // Cheap text-only capture for the per-keystroke path: skips toHtml().
    [[nodiscard]] static HistoryDraft captureText(
        const HistoryInput &input,
        const QString &text);
    [[nodiscard]] static QString preview(const HistoryDraft &draft);
    // Allocation-free "would preview() be non-empty" check for plain text.
    [[nodiscard]] static bool hasVisibleContent(QStringView text);

    [[nodiscard]] QString update(const QString &roomId, const HistoryDraft &draft);
    [[nodiscard]] QString updateFromInput(const QString &roomId, const HistoryInput &input);
    // Per-keystroke path: text-only capture, no preview computation.
    void updateTextOnly(
        const QString &roomId,
        const HistoryInput &input,
        const QString &text);
    void remove(const QString &roomId);

    [[nodiscard]] HistoryDraft value(const QString &roomId) const;

private:
    static void captureMeta(const HistoryInput &input, HistoryDraft &draft);

    QHash<QString, HistoryDraft> _drafts;
};

} // namespace TeleMatrix

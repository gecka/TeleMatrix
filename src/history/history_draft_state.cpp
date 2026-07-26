// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_draft_state.h"

#include "history_input.h"

#include <algorithm>
#include <utility>

#include <QTextDocument>

namespace TeleMatrix {

void HistoryDraftStore::captureMeta(const HistoryInput &input, HistoryDraft &draft) {
    draft.replyMode = input.isInReplyMode();
    draft.replyEventId = input.replyEventId();
    draft.replySenderName = input.replySenderName();
    draft.replyPreviewText = input.replyPreviewText();
    draft.replyPreviewPath = input.replyPreviewPath();
    draft.editMode = input.isInEditMode();
    draft.editEventId = input.editEventId();
    draft.editSenderName = input.editSenderName();
    draft.editPreviewText = input.editPreviewText();
}

HistoryDraft HistoryDraftStore::capture(const HistoryInput &input) {
    auto draft = HistoryDraft();
    captureMeta(input, draft);
    draft.text = input.fieldText();
    draft.html = input.fieldHtml();
    return draft;
}

// Per-keystroke path: keep draft.html empty rather than serializing the whole
// document on every change. The HTML is read back only on room switch-away/
// close, both of which run the full capture() first, so nothing is lost.
HistoryDraft HistoryDraftStore::captureText(
        const HistoryInput &input,
        const QString &text) {
    auto draft = HistoryDraft();
    captureMeta(input, draft);
    draft.text = text;
    return draft;
}

QString HistoryDraftStore::preview(const HistoryDraft &draft) {
    auto result = draft.text;
    if (result.trimmed().isEmpty() && !draft.html.trimmed().isEmpty()) {
        QTextDocument doc;
        doc.setHtml(draft.html);
        result = doc.toPlainText();
    }
    return result.simplified();
}

bool HistoryDraftStore::hasVisibleContent(QStringView text) {
    return std::any_of(text.begin(), text.end(),
        [](QChar c) { return !c.isSpace(); });
}

QString HistoryDraftStore::update(const QString &roomId, const HistoryDraft &draft) {
    if (roomId.isEmpty()) {
        return QString();
    }
    const auto result = preview(draft);
    if (!result.isEmpty() || draft.replyMode) {
        _drafts.insert(roomId, draft);
        return result;
    }
    _drafts.remove(roomId);
    return QString();
}

QString HistoryDraftStore::updateFromInput(const QString &roomId, const HistoryInput &input) {
    return update(roomId, capture(input));
}

void HistoryDraftStore::updateTextOnly(
        const QString &roomId,
        const HistoryInput &input,
        const QString &text) {
    if (roomId.isEmpty()) {
        return;
    }
    auto draft = captureText(input, text);
    // Same decision update() makes via preview(): text-only drafts preview
    // to text.simplified(), empty iff the text is whitespace-only.
    if (hasVisibleContent(text) || draft.replyMode) {
        _drafts.insert(roomId, std::move(draft));
    } else {
        _drafts.remove(roomId);
    }
}

void HistoryDraftStore::remove(const QString &roomId) {
    _drafts.remove(roomId);
}

HistoryDraft HistoryDraftStore::value(const QString &roomId) const {
    return _drafts.value(roomId);
}

} // namespace TeleMatrix

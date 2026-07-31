// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

// Sprite emoji inside a QTextEdit.
//
// The placeholder trick in ui/text/emoji_text.h needs a QTextLayout it can attach format
// ranges to; a QTextEdit owns its own document layout and gives us no such hook. So the
// composer takes the other route Telegram Desktop takes: every emoji in the document is
// replaced by a single QChar::ObjectReplacementCharacter carrying a QTextImageFormat whose
// name is an `emoji://` URL. Qt then lays the emoji out as an inline image — correct
// wrapping, selection and cursor movement for free, and one backspace deletes a whole
// emoji rather than half a surrogate pair.
//
// The cost is that the document no longer holds the text that will be sent. Every read of
// it has to go through composePlainText() in history/compose_html.h.

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtGui/QTextCharFormat>

#include "ui/emoji_config.h"

class QTextDocument;
class QTextEdit;
class QEvent;

namespace Ui::EmojiObjects {

// The image format for one emoji, sized to the inline slot and inheriting `base` so the
// surrounding text colour and font survive.
[[nodiscard]] QTextImageFormat FormatFor(EmojiPtr emoji, const QTextCharFormat &base);

[[nodiscard]] bool IsEmojiUrl(const QString &name);

// The emoji an `emoji://` URL names, or null.
[[nodiscard]] EmojiPtr FromUrl(const QString &name);

// The pixmap a document should render for `name`. Null for anything that is not one of
// our URLs. Hook this up from QTextEdit::loadResource.
[[nodiscard]] QVariant Resource(const QString &name);

// Replaces every emoji in the document's changed range with an inline object. Returns the
// character format that was in force where the last replacement happened, so the caller
// can restore it — after inserting an image the cursor's current format IS that image
// format, and the next character typed would inherit it.
struct ReplaceResult {
    bool replaced = false;
    QTextCharFormat restoreFormat;
};
ReplaceResult ReplaceRange(QTextDocument *doc, int position, int charsAdded);

// Keeps one QTextEdit's emoji converted to objects: watches the document for changes,
// replaces what the user typed or pasted, and holds the guards that make undo, redo and
// input-method composition behave. Parented to the edit; one per edit.
class Watcher final : public QObject {
public:
    explicit Watcher(QTextEdit *edit);

    // True while the pass is rewriting the document. Its nested edits re-emit
    // textChanged, and a per-keystroke handler must not run twice.
    [[nodiscard]] bool correcting() const {
        return _correcting;
    }

    // For an edit that overrides inputMethodEvent itself.
    void setPreedit(bool preedit) {
        _preedit = preedit;
    }
    void process(int position, int charsAdded);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QTextEdit *_edit = nullptr;
    bool _correcting = false;
    bool _preedit = false;
};

// Installs a Watcher on `edit` and returns it. Use for any QTextEdit that holds text a
// user types — captions, topics — so its emoji match the rest of the app.
Watcher *Install(QTextEdit *edit);

// Re-stamps the canonical image format on every emoji object in the document. Qt's HTML
// writer round-trips `src` but not the sizing, so a draft restored with setHtml() comes
// back with the right emoji and the wrong geometry.
void RestampAll(QTextDocument *doc);

} // namespace Ui::EmojiObjects

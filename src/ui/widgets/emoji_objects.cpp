// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "ui/widgets/emoji_objects.h"

#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtGui/QTextBlock>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>
#include <QtCore/QEvent>
#include <QtCore/QUrl>
#include <QtGui/QInputMethodEvent>
#include <QtWidgets/QTextEdit>
#include <QtGui/QTextFragment>

#include "styles/style_constants.h"
#include "ui/emoji_sprites.h"
#include "ui/style/runtime_scale.h"
#include "ui/text/emoji_text.h"

namespace Ui::EmojiObjects {
namespace {

[[nodiscard]] int BoxWidth() {
    return st::emojiInlineSlot;
}

[[nodiscard]] int BoxHeight() {
    return qMax(int(st::msgFont->height), st::emojiInlineGlyph);
}

// The URL carries the box size, so a scale change yields a different resource key and a
// stale pixmap can never be served for the new geometry.
[[nodiscard]] QString UrlFor(EmojiPtr emoji) {
    return TeleMatrix::EmojiText::EmojiUrl(emoji, BoxWidth(), BoxHeight());
}

// A box-sized transparent pixmap with the sprite centred in it. Qt stretches an inline
// image to the format's width/height, so the padding has to be baked into the pixmap
// rather than left to the format.
[[nodiscard]] QPixmap BoxPixmap(EmojiPtr emoji) {
    static auto cache = QHash<QString, QPixmap>();
    const auto key = UrlFor(emoji);
    const auto i = cache.constFind(key);
    if (i != cache.cend()) {
        return *i;
    }
    const auto glyph = TeleMatrix::Emoji::Pixmap(
        emoji->text(),
        st::emojiInlineGlyph);
    if (glyph.isNull()) {
        return QPixmap();
    }
    const auto ratio = qMax(TeleMatrix::Style::DevicePixelRatio(), 1);
    const auto width = BoxWidth();
    const auto height = BoxHeight();
    auto result = QPixmap(width * ratio, height * ratio);
    result.setDevicePixelRatio(ratio);
    result.fill(Qt::transparent);
    {
        auto p = QPainter(&result);
        p.drawPixmap(
            QPoint(
                (width - st::emojiInlineGlyph) / 2,
                (height - st::emojiInlineGlyph) / 2),
            glyph);
    }
    cache.insert(key, result);
    return result;
}

// The document has to be *handed* the pixmap. QTextDocument::resource() checks its own
// resource map first and only then walks the parent chain looking for a loadResource —
// and a QTextEdit's document is parented to the text control, not the widget, so the
// override there is never reached and the layout draws Qt's missing-image icon instead.
// tests/tst_emoji_text.cpp::imageObjectsNeedAnAddedResource pins this.
void EnsureResource(QTextDocument *doc, EmojiPtr emoji) {
    // Unconditional: addResource is an idempotent hash insert and BoxPixmap is memoised,
    // whereas probing resource() first would re-enter the very lookup that fails.
    const auto pixmap = BoxPixmap(emoji);
    if (!pixmap.isNull()) {
        doc->addResource(
            QTextDocument::ImageResource,
            QUrl(UrlFor(emoji)),
            QVariant(pixmap));
    }
}

} // namespace

QTextImageFormat FormatFor(EmojiPtr emoji, const QTextCharFormat &base) {
    auto result = QTextImageFormat();
    // Inheriting the surrounding format keeps the *following* text's font and colour
    // intact when Qt merges adjacent fragments.
    result.merge(base);
    result.setName(UrlFor(emoji));
    result.setWidth(BoxWidth());
    result.setHeight(BoxHeight());
    result.setVerticalAlignment(QTextCharFormat::AlignBottom);
    return result;
}

bool IsEmojiUrl(const QString &name) {
    return TeleMatrix::EmojiText::IsEmojiUrl(name);
}

EmojiPtr FromUrl(const QString &name) {
    return TeleMatrix::EmojiText::EmojiFromUrl(name);
}

QVariant Resource(const QString &name) {
    const auto emoji = FromUrl(name);
    if (!emoji) {
        return QVariant();
    }
    const auto pixmap = BoxPixmap(emoji);
    return pixmap.isNull() ? QVariant() : QVariant(pixmap);
}

ReplaceResult ReplaceRange(QTextDocument *doc, int position, int charsAdded) {
    auto result = ReplaceResult();
    if (!doc || charsAdded <= 0 || !TeleMatrix::Emoji::Available()) {
        return result;
    }
    const auto changeEnd = position + charsAdded;

    // Collected first and applied last-to-first: replacing an emoji shortens the
    // document, and going backwards keeps every earlier position valid. The alternative
    // — restarting the scan after each hit, as upstream does — is quadratic on a paste.
    struct Hit {
        int position = 0;
        int length = 0;
        EmojiPtr emoji = nullptr;
        QTextCharFormat format;
    };
    auto hits = QList<Hit>();

    for (auto block = doc->findBlock(position);
            block.isValid() && block.position() < changeEnd;
            block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid()) {
                continue;
            }
            const auto format = fragment.charFormat();
            if (format.isImageFormat()) {
                continue;
            }
            const auto start = fragment.position();
            const auto text = fragment.text();
            const auto from = qMax(0, position - start);
            const auto till = qMin(int(text.size()), changeEnd - start);
            for (auto i = from; i < till;) {
                auto length = 0;
                const auto emoji = Ui::Emoji::Find(
                    QStringView(text).mid(i),
                    &length);
                if (emoji && length > 0) {
                    hits.append({ start + i, length, emoji, format });
                    i += length;
                } else {
                    ++i;
                }
            }
        }
    }
    if (hits.isEmpty()) {
        return result;
    }

    auto cursor = QTextCursor(doc);
    // joinPreviousEditBlock, not beginEditBlock: one Ctrl+Z should undo "typed an emoji",
    // not "replaced an emoji" and then "typed one".
    cursor.joinPreviousEditBlock();
    for (auto i = hits.crbegin(); i != hits.crend(); ++i) {
        EnsureResource(doc, i->emoji);
        cursor.setPosition(i->position);
        cursor.setPosition(i->position + i->length, QTextCursor::KeepAnchor);
        cursor.insertText(
            QString(QChar::ObjectReplacementCharacter),
            FormatFor(i->emoji, i->format));
    }
    cursor.endEditBlock();

    result.replaced = true;
    result.restoreFormat = hits.constFirst().format;
    return result;
}

Watcher::Watcher(QTextEdit *edit)
: QObject(edit)
, _edit(edit) {
    edit->installEventFilter(this);
    connect(
        edit->document(),
        &QTextDocument::contentsChange,
        this,
        [this](int position, int charsRemoved, int charsAdded) {
            process(position, charsAdded);
        });
}

void Watcher::process(int position, int charsAdded) {
    // charsAdded == 0 covers plain deletions, the common backspace case. A live preedit
    // is not in the document yet, and rewriting under the input context would reset
    // composition — wait for the commit.
    if (_correcting || _preedit || charsAdded <= 0) {
        return;
    }
    // Never re-run over content a redo is restoring: it is already processed, and
    // reprocessing inside the redo stack can loop.
    if (_edit->document()->availableRedoSteps() != 0) {
        return;
    }

    _correcting = true;
    const auto result = ReplaceRange(_edit->document(), position, charsAdded);
    if (result.replaced) {
        // After inserting an image the cursor's current format IS that image format, so
        // the next typed character would carry an ImageName and the HTML serializer would
        // mistake ordinary text for an emoji object.
        auto cursor = _edit->textCursor();
        if (cursor.charFormat().isImageFormat()) {
            cursor.setCharFormat(result.restoreFormat);
            _edit->setTextCursor(cursor);
        }
        _edit->setCurrentCharFormat(result.restoreFormat);
    }
    _correcting = false;
}

bool Watcher::eventFilter(QObject *watched, QEvent *event) {
    if (watched == _edit && event->type() == QEvent::InputMethod) {
        const auto ime = static_cast<QInputMethodEvent*>(event);
        const auto had = _preedit;
        _preedit = !ime->preeditString().isEmpty();
        if (had && !_preedit && !ime->commitString().isEmpty()) {
            // The commit landed while the gate was closed; process it once it is in.
            const auto length = int(ime->commitString().size());
            const auto result = QObject::eventFilter(watched, event);
            const auto end = _edit->textCursor().position();
            process(qMax(0, end - length), length);
            return result;
        }
    }
    return QObject::eventFilter(watched, event);
}

Watcher *Install(QTextEdit *edit) {
    return edit ? new Watcher(edit) : nullptr;
}

void RestampAll(QTextDocument *doc) {
    if (!doc) {
        return;
    }
    auto cursor = QTextCursor(doc);
    auto pending = QList<QPair<int, EmojiPtr>>();
    for (auto block = doc->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid() || !fragment.charFormat().isImageFormat()) {
                continue;
            }
            const auto name = fragment.charFormat().toImageFormat().name();
            if (const auto emoji = FromUrl(name)) {
                pending.append({ fragment.position(), emoji });
            }
        }
    }
    if (pending.isEmpty()) {
        return;
    }
    cursor.beginEditBlock();
    for (const auto &[position, emoji] : pending) {
        EnsureResource(doc, emoji);
        cursor.setPosition(position);
        cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
        cursor.setCharFormat(FormatFor(emoji, QTextCharFormat()));
    }
    cursor.endEditBlock();
}

} // namespace Ui::EmojiObjects

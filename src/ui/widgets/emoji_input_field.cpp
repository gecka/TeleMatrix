// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "ui/widgets/emoji_input_field.h"

#include <QtCore/QMimeData>
#include <QtCore/QUrl>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtGui/QTextDocument>
#include <QtGui/QTextDocumentFragment>
#include <QtWidgets/QScrollBar>

#include "ui/text/emoji_text.h"

namespace Ui {
namespace {

constexpr auto kFocusAnimationMs = 120;

} // namespace

EmojiInputField::EmojiInputField(
    QWidget *parent,
    const st::InputFieldStyle &style,
    const QString &placeholder)
: QTextEdit(parent)
, _style(style)
, _placeholder(placeholder) {
    setFrameShape(QFrame::NoFrame);
    setAttribute(Qt::WA_MacShowFocusRect, false);
    // Rich text on paste would bring foreign fonts and colours in; the emoji objects are
    // inserted by the watcher afterwards, from plain text.
    setAcceptRichText(false);
    setLineWrapMode(QTextEdit::NoWrap);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setTabChangesFocus(true);
    setFont(st::baseFont(st::fsize));

    auto pal = palette();
    pal.setColor(QPalette::Base, Qt::transparent);
    pal.setColor(QPalette::Text, _style.textFg);
    pal.setColor(QPalette::Highlight, _style.borderFgActive);
    setPalette(pal);

    _focusAnimation.setDuration(kFocusAnimationMs);
    connect(&_focusAnimation, &QVariantAnimation::valueChanged, this,
        [this](const QVariant &value) {
            _focusedProgress = value.toReal();
            update();
        });
    _placeholderShownAnimation.setDuration(kFocusAnimationMs);
    connect(&_placeholderShownAnimation, &QVariantAnimation::valueChanged, this,
        [this](const QVariant &value) {
            _placeholderShownProgress = value.toReal();
            update();
        });

    _emoji = EmojiObjects::Install(this);
    connect(this, &QTextEdit::textChanged, this, [this] {
        enforceMaxLength();
        updatePlaceholderShown();
    });

    applyFieldMetrics();
}

QString EmojiInputField::text() const {
    return TeleMatrix::EmojiText::DocumentText(document());
}

void EmojiInputField::setText(const QString &text) {
    setPlainText(text);
    auto cursor = textCursor();
    cursor.movePosition(QTextCursor::End);
    setTextCursor(cursor);
}

void EmojiInputField::setMaxLength(int length) {
    _maxLength = length;
    enforceMaxLength();
}

void EmojiInputField::setFloatingPlaceholder(bool enabled) {
    if (_floatingPlaceholder == enabled) {
        return;
    }
    _floatingPlaceholder = enabled;
    applyFieldMetrics();
    updatePlaceholderShown();
    update();
}

void EmojiInputField::setPlaceholderText(const QString &text) {
    _placeholder = text;
    update();
}

// One emoji is one character here, same as it is to the user and to the server — the
// document stores it as a single object-replacement character.
void EmojiInputField::enforceMaxLength() {
    if (_maxLength <= 0 || _truncating) {
        return;
    }
    const auto length = int(text().size());
    if (length <= _maxLength) {
        return;
    }
    _truncating = true;
    auto cursor = textCursor();
    const auto position = cursor.position();
    cursor.setPosition(qMax(0, document()->characterCount() - 1 - (length - _maxLength)));
    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    cursor.setPosition(qMin(position, document()->characterCount() - 1));
    setTextCursor(cursor);
    _truncating = false;
}

void EmojiInputField::applyFieldMetrics() {
    if (!_chromeVisible) {
        // The host widget owns geometry and margins in this mode.
        _textMargins = QMargins();
        setViewportMargins(0, 0, 0, 0);
        document()->setDocumentMargin(0);
        return;
    }
    const auto compact = (_style.borderRadius == 0) && !_floatingPlaceholder;
    const auto height = compact ? 38 : qMax(1, _style.heightMin);
    setFixedHeight(height);
    _textMargins = _style.textMargins;
    if (compact) {
        _textMargins.setTop(qMin(_textMargins.top(), 8));
    }
    setViewportMargins(
        _textMargins.left(),
        _textMargins.top(),
        _textMargins.right(),
        _textMargins.bottom());
    document()->setDocumentMargin(0);
}

InputChrome::State EmojiInputField::chromeState() const {
    return {
        .style = &_style,
        .rect = rect(),
        .textMargins = _textMargins,
        .font = font(),
        .placeholder = _placeholder,
        .focusedProgress = _focusedProgress,
        .placeholderShownProgress = _placeholderShownProgress,
        .placeholderAnimating
            = (_placeholderShownAnimation.state() == QAbstractAnimation::Running),
        .focused = hasFocus(),
        .empty = document()->isEmpty(),
        .floating = _floatingPlaceholder,
    };
}

void EmojiInputField::setChromeVisible(bool visible) {
    if (_chromeVisible == visible) {
        return;
    }
    _chromeVisible = visible;
    applyFieldMetrics();
    update();
}

void EmojiInputField::paintEvent(QPaintEvent *e) {
    if (_chromeVisible) {
        auto p = QPainter(viewport());
        // The chrome is painted in widget coordinates; the viewport is inset by the text
        // margins, so shift back before reusing the shared painters.
        p.translate(-_textMargins.left(), -_textMargins.top());
        const auto state = chromeState();
        InputChrome::PaintFlatSurrounding(p, state);
        InputChrome::PaintPlaceholder(p, state);
    }
    QTextEdit::paintEvent(e);
}

void EmojiInputField::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        emit submitted();
        emit editingFinished();
        e->accept();
        return;
    }
    QTextEdit::keyPressEvent(e);
}

void EmojiInputField::focusInEvent(QFocusEvent *e) {
    QTextEdit::focusInEvent(e);
    startFocusAnimation(true);
    updatePlaceholderShown();
}

void EmojiInputField::focusOutEvent(QFocusEvent *e) {
    QTextEdit::focusOutEvent(e);
    startFocusAnimation(false);
    updatePlaceholderShown();
    emit editingFinished();
}

void EmojiInputField::startFocusAnimation(bool focused) {
    _focusAnimation.stop();
    _focusAnimation.setStartValue(_focusedProgress);
    _focusAnimation.setEndValue(focused ? 1. : 0.);
    _focusAnimation.start();
}

void EmojiInputField::updatePlaceholderShown() {
    const auto target = (hasFocus() || !document()->isEmpty()) ? 1. : 0.;
    if (!_floatingPlaceholder || qFuzzyCompare(_placeholderShownProgress, target)) {
        update();
        return;
    }
    _placeholderShownAnimation.stop();
    _placeholderShownAnimation.setStartValue(_placeholderShownProgress);
    _placeholderShownAnimation.setEndValue(target);
    _placeholderShownAnimation.start();
}

void EmojiInputField::insertFromMimeData(const QMimeData *source) {
    if (!source->hasText()) {
        return;
    }
    // Single-line: a pasted paragraph becomes one line rather than silently growing the
    // field past its fixed height.
    auto text = source->text();
    text.replace(QChar::LineSeparator, u' ');
    text.replace(QChar::ParagraphSeparator, u' ');
    text.replace(u'\n', u' ');
    text.replace(u'\r', u' ');
    insertPlainText(text);
}

QVariant EmojiInputField::loadResource(int type, const QUrl &name) {
    if (type == QTextDocument::ImageResource) {
        const auto url = name.toString();
        if (EmojiObjects::IsEmojiUrl(url)) {
            return EmojiObjects::Resource(url);
        }
    }
    return QTextEdit::loadResource(type, name);
}

QMimeData *EmojiInputField::createMimeDataFromSelection() const {
    auto result = QTextEdit::createMimeDataFromSelection();
    if (!result) {
        return result;
    }
    auto cursor = textCursor();
    if (cursor.hasSelection()) {
        auto fragment = QTextDocument();
        fragment.setHtml(cursor.selection().toHtml());
        result->setText(TeleMatrix::EmojiText::DocumentText(&fragment));
    }
    return result;
}

} // namespace Ui

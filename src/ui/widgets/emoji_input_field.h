// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

// A single-line text field that renders emoji from the sprite atlas.
//
// Ui::InputField cannot: it is a QLineEdit, and a QLineEdit has no way to host the inline
// image objects sprite emoji need. It also cannot simply be converted — the password and
// passphrase dialogs depend on setEchoMode() and on QLineEdit::returnPressed. So this is a
// parallel widget for the fields that actually carry prose: room names, folder names, the
// display name. Everything else stays on Ui::InputField.
//
// It shares that class's chrome (Ui::InputChrome) so the two look identical, and behaves
// like a line edit: Return submits rather than inserting a newline, pasted newlines
// collapse to spaces, and there is a character limit.

#include <QtWidgets/QTextEdit>
#include <QtCore/QVariantAnimation>

#include "styles/style_constants.h"
#include "ui/widgets/emoji_objects.h"
#include "ui/widgets/input_fields.h"

namespace Ui {

class EmojiInputField : public QTextEdit {
    Q_OBJECT

public:
    EmojiInputField(
        QWidget *parent,
        const st::InputFieldStyle &style,
        const QString &placeholder);

    // Text with emoji objects expanded back to characters — what to send or save.
    [[nodiscard]] QString text() const;
    void setText(const QString &text);

    void setMaxLength(int length);
    void setFloatingPlaceholder(bool enabled);
    void setPlaceholderText(const QString &text);

    // Off for fields embedded in another design (the room-settings cover name), which
    // paint their own decoration and size themselves.
    void setChromeVisible(bool visible);

    /// Re-read colors from the style struct (call after theme change).
    virtual void refreshStyle(const st::InputFieldStyle &style);

Q_SIGNALS:
    void submitted();
    // QLineEdit::editingFinished's shape: Return, or losing focus.
    void editingFinished();

protected:
    // Paints the field's decoration UNDER its text. Overridden by fields that supply
    // their own (the room-settings cover name); the base draws the shared chrome.
    //
    // This runs on a dedicated layer widget, not in paintEvent, because a QTextEdit's
    // paintEvent targets the viewport — which applyFieldMetrics() insets by the text
    // margins, so it clips away exactly the two strips the chrome needs: the bottom
    // border and the lifted floating caption.
    virtual void paintChrome(QPainter &p);
    // Repaint the decoration after a state change (focus, content, style, metrics).
    void updateChrome();

    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void focusInEvent(QFocusEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;
    void insertFromMimeData(const QMimeData *source) override;
    [[nodiscard]] QVariant loadResource(int type, const QUrl &name) override;
    [[nodiscard]] QMimeData *createMimeDataFromSelection() const override;

private:
    class ChromeLayer;

    [[nodiscard]] InputChrome::State chromeState() const;
    void startFocusAnimation(bool focused);
    void updatePlaceholderShown();
    void applyFieldMetrics();
    void enforceMaxLength();

    st::InputFieldStyle _style;
    QMargins _textMargins;
    QString _placeholder;
    QVariantAnimation _focusAnimation;
    QVariantAnimation _placeholderShownAnimation;
    EmojiObjects::Watcher *_emoji = nullptr;
    ChromeLayer *_chrome = nullptr;
    qreal _focusedProgress = 0.;
    qreal _placeholderShownProgress = 0.;
    int _maxLength = 0;
    bool _floatingPlaceholder = false;
    bool _chromeVisible = true;
    bool _truncating = false;
};

} // namespace Ui

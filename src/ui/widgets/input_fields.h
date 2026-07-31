// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

// Compatibility header: provides Ui::InputField stub.
//
// When lib_ui is fully integrated, replace with real lib_ui InputField.
#pragma once

#include <QFocusEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QLineEdit>
#include <QString>
#include <QVariantAnimation>

#include "styles/style_constants.h"

class QPainter;

// Stub for rpl::producer — in lib_ui this is a reactive stream.
namespace rpl {
template <typename T>
struct producer {
    T value;
};
template <typename T>
inline producer<T> single(T v) { return {std::move(v)}; }
} // namespace rpl

// Stub for object_ptr — in lib_ui this is a smart pointer for QObjects.
template <typename T>
class object_ptr {
public:
    object_ptr() = default;
    explicit object_ptr(T *raw) : _ptr(raw) {}
    static object_ptr fromRaw(T *raw) { return object_ptr(raw); }
    T *data() const { return _ptr; }
    T *operator->() const { return _ptr; }
    T &operator*() const { return *_ptr; }
    operator bool() const { return _ptr != nullptr; }
private:
    T *_ptr = nullptr;
};

namespace Ui {

// The themed chrome — flat background + bottom border, and the placeholder in either its
// "inside" or floating-caption pose. Free functions rather than InputField methods because
// EmojiInputField below is a QTextEdit and cannot inherit them, and two copies of this
// would drift apart on the next theme change.
namespace InputChrome {

struct State {
    const st::InputFieldStyle *style = nullptr;
    QRect rect;
    QMargins textMargins;
    QFont font;
    QString placeholder;
    qreal focusedProgress = 0.;
    qreal placeholderShownProgress = 0.;
    bool placeholderAnimating = false;
    bool focused = false;
    bool empty = true;
    bool floating = false;
};

void PaintFlatSurrounding(QPainter &p, const State &state);
void PaintPlaceholder(QPainter &p, const State &state);

} // namespace InputChrome

// Themed single-line edit that custom-paints its own 1px rounded border with
// live st:: colors (st::inputBorderFg normally, st::activeLineFg on focus) and
// applies horizontal text margins, replacing the old per-dialog QSS
// (compactLineEditStyleSheet: 1px solid st::inputBorderFg, radius 4, padding
// 0 8px, focus border st::activeLineFg). Background / text / selection colors
// come from QPalette (also st:: live). Unlike InputField above it keeps the
// native QLineEdit placeholder and lets the caller set font / echo mode, so the
// password and monospace dialogs can reuse it directly.
class BorderedLineEdit : public QLineEdit {
    Q_OBJECT

public:
    explicit BorderedLineEdit(QWidget *parent);
    ~BorderedLineEdit() override;

protected:
    void paintEvent(QPaintEvent *e) override;
};

class InputField : public QLineEdit {
    Q_OBJECT

public:
    InputField(
        QWidget *parent,
        const st::InputFieldStyle &style,
        rpl::producer<QString> placeholder);

    explicit InputField(QWidget *parent);
    ~InputField() override;

    [[nodiscard]] QString getLastText() const {
        return text();
    }

    /// Re-read colors from the style struct (call after theme change).
    void refreshStyle(const st::InputFieldStyle &style);

    /// Access the current style (e.g. to check borderRadius for style detection).
    [[nodiscard]] const st::InputFieldStyle &currentStyle() const { return _style; }

    /// Update the custom-painted placeholder text.
    /// Empty string restores the original placeholder set at construction.
    void setPlaceholderText(const QString &text);

    /// tdesktop-style floating caption: when enabled, a focused or non-empty
    /// field lifts its placeholder into the top margin as a small accent-tinted
    /// caption (an empty, unfocused field still shows it inside). Off by default
    /// — single-field forms leave it off so no caption is drawn. Needs a style
    /// with room above the text (e.g. st::defaultInputField, top margin 28).
    void setFloatingPlaceholder(bool enabled);

    /// Show or hide the cancel/cross button on the right side.
    void setCancelVisible(bool visible);
    [[nodiscard]] bool cancelVisible() const { return _cancelVisible; }

Q_SIGNALS:
    /// Emitted when the cross/cancel button is clicked.
    void cancelled();

protected:
    void focusInEvent(QFocusEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;

private:
    void init(const st::InputFieldStyle &style, const QString &placeholder);
    void startFocusAnimation(bool focused);
    [[nodiscard]] InputChrome::State chromeState() const;
    void paintRoundSurrounding(QPainter &p) const;
    void paintFlatSurrounding(QPainter &p) const;
    void paintPlaceholder(QPainter &p) const;
    void paintCancelButton(QPainter &p) const;
    [[nodiscard]] QRect cancelButtonRect() const;
    void updateCancelCursor(const QPoint &pos);
    void applyTextMargins(const QMargins &margins);
    // Height + text margins for the current mode: a flat caption field reserves a
    // top strip for the floating caption; with no caption (floating off) it drops
    // that strip and shrinks. Round filter fields keep their own metrics.
    void applyFieldMetrics();
    // Re-target the floating-caption animation from the current focus + content.
    void updatePlaceholderShown();

    st::InputFieldStyle _style;
    QMargins _textMargins;
    QString _placeholder;
    QString _originalPlaceholder;
    QVariantAnimation _focusAnimation;
    // 0 = placeholder inside at text size; 1 = lifted into the top strip, small.
    QVariantAnimation _placeholderShownAnimation;
    qreal _placeholderShownProgress = 0.;
    qreal _focusedProgress = 0.;
    bool _cancelVisible = false;
    bool _cancelHovered = false;
    bool _floatingPlaceholder = false;
};

} // namespace Ui

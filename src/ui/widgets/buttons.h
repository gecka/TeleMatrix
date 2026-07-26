// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

// Compatibility header: provides Ui::IconButton stub.
//
// When lib_ui is fully integrated, replace with real lib_ui IconButton.
#pragma once

#include "styles/style_constants.h"

#include <QAbstractButton>
#include <QColor>
#include <QFont>
#include <QPalette>
#include <QPushButton>
#include <functional>

namespace Ui {

// Simplified IconButton: wraps a QPushButton.
// The real lib_ui IconButton renders an icon with hover/press animations
// and ripple effects.
class IconButton : public QPushButton {
    Q_OBJECT

public:
    // Constructor matching lib_ui's IconButton(parent, style).
    template <typename StyleType>
    IconButton(QWidget *parent, const StyleType &st)
        : QPushButton(parent)
    {
        setFixedSize(st.width, st.height);
        setFlat(true);
        setCursor(Qt::PointingHandCursor);
        // Simple send arrow appearance. Color via QPalette (not a frozen
        // stylesheet) so it tracks the theme; setFlat already removes the border.
        setText(QStringLiteral("\u27A4"));
        QFont f = font();
        f.setPixelSize(18);
        setFont(f);
        QPalette pal = palette();
        pal.setColor(QPalette::ButtonText, st::historySendIconFg);
        setPalette(pal);
    }

    ~IconButton() override;

    // Set a click callback (lib_ui API).
    void setClickedCallback(std::function<void()> callback) {
        _callback = std::move(callback);
        QObject::connect(this, &QPushButton::clicked, this, [this]() {
            if (_callback) _callback();
        });
    }

private:
    std::function<void()> _callback;
};

// Themed flat/filled text button painted with st:: colors instead of an
// inline stylesheet. Colors are passed as POINTERS to st:: globals: those
// globals are mutated in place on theme change, so paintEvent always reads the
// current theme's colors (a stylesheet built from `.name()` would freeze them).
// Pass bg == nullptr for a transparent (flat) normal state.
class TextButton : public QAbstractButton {
    Q_OBJECT

public:
    struct Style {
        const QColor *bg = nullptr;      // normal fill; nullptr = transparent
        const QColor *bgOver = nullptr;  // hover fill; nullptr = same as bg
        const QColor *fg = nullptr;      // text color (required)
        int radius = 0;
        int height = 0;                  // 0 = derive from font
        int paddingH = 15;               // horizontal text padding for sizeHint
    };

    TextButton(const QString &text, const Style &style, QWidget *parent);
    ~TextButton() override;

    void setButtonStyle(const Style &style);

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    Style _style;
    bool _hovered = false;
};

} // namespace Ui

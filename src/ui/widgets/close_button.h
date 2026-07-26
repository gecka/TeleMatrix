// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>
#include <QImage>

// In the global ::Ui namespace alongside the other UI widgets (TextButton,
// RpWidget, …). It must NOT live in TeleMatrix::Ui: that inner namespace would
// shadow the global ::Ui for every `Ui::X` used inside `namespace TeleMatrix`.
namespace Ui {

// The app's single close (×) affordance for content boxes: the settings popups'
// info_close glyph, colorized to the theme and left-inset in a
// settingsCloseButtonSize × boxTitleHeight hit target. Emits clicked() (ignored
// while the widget is disabled, so a busy box can block dismissal). Place it at
// the top-right of the box's title bar.
class CloseButton final : public QWidget {
    Q_OBJECT

public:
    explicit CloseButton(QWidget *parent = nullptr);

Q_SIGNALS:
    void clicked();

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    QImage _icon;
    QImage _iconOver;
    bool _hovered = false;
};

} // namespace Ui

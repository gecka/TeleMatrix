// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>

#include <functional>

class QEventLoop;
class QGraphicsOpacityEffect;
class QLabel;
class QPaintEvent;
class QMouseEvent;
class QKeyEvent;
class QVariantAnimation;

namespace Ui {
class TextButton;
} // namespace Ui

namespace TeleMatrix {

class HistoryConfirmDialog final : public QWidget {
    Q_OBJECT

public:
    enum DialogCode { Rejected = 0, Accepted = 1 };

    // Attention style uses red text for destructive actions.
    enum ConfirmStyle { Normal = 0, Attention = 1, FilledAttention = 2 };

    explicit HistoryConfirmDialog(
        QWidget *parent,
        const QString &title,
        const QString &text,
        const QString &confirmText,
        const QString &cancelText = QString(),
        ConfirmStyle confirmStyle = Normal,
        int customWidth = 0,
        int customButtonBottomPadding = -1,
        bool showCancel = true,
        bool richText = false);

    int exec();

    // Busy mode (sign-out): when set, clicking confirm does NOT close the dialog.
    // Instead it disables both buttons (no preloader) and invokes `callback`. The
    // caller runs its async work and calls finishBusy() to close.
    void setBusyOnConfirm(std::function<void()> callback);
    void finishBusy();

private:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

    void accept();
    void reject();
    void enterBusyState();

    QWidget *_panel = nullptr;
    QLabel *_title = nullptr;
    QLabel *_text = nullptr;
    ::Ui::TextButton *_cancel = nullptr;
    ::Ui::TextButton *_confirm = nullptr;

    // _a_shown (background, easeOutCirc) + _a_layerShown (box, linear).
    // Both st::boxDuration = 200ms, started simultaneously.
    qreal _bgOpacity = 0.0;
    qreal _layerOpacity = 0.0;
    QVariantAnimation *_a_shown = nullptr;
    QVariantAnimation *_a_layerShown = nullptr;
    QGraphicsOpacityEffect *_panelEffect = nullptr;

    QEventLoop *_loop = nullptr;
    int _result = Rejected;

    std::function<void()> _busyOnConfirm;
    bool _busy = false;
};

} // namespace TeleMatrix

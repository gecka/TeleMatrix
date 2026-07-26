// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>

class QVariantAnimation;

namespace TeleMatrix {

class IntroWidget;

/// In-window popup card that hosts the account login/register flow (IntroWidget)
/// when adding a second account, instead of a separate top-level window. Same
/// dimmed-layer + rounded-panel look as the other box dialogs, but non-blocking:
/// the sign-in flow is async (login -> bridge signals -> in-flow verification),
/// so it drives its outcome through a `finished(int)` signal that mirrors
/// QDialog::finished, keeping the controller's existing handler a drop-in.
class DialogsAddAccountBox final : public QWidget {
    Q_OBJECT

public:
    enum DialogCode { Rejected = 0, Accepted = 1 };

    /// Adopts `intro` into the card. Does not take a bridge — the caller builds
    /// the IntroWidget against the pending account's bridge and wires its signals.
    DialogsAddAccountBox(IntroWidget *intro, QWidget *parent = nullptr);

    /// Close as success (login/verification completed). Emits finished(Accepted).
    void accept();

signals:
    /// Mirrors QDialog::finished: the controller branches on the account's
    /// session state, so both accept and reject route through one handler.
    void finished(int result);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    bool eventFilter(QObject *obj, QEvent *event) override;
    void showEvent(QShowEvent *) override;

private:
    void reject();
    void closeWith(int result);
    void updatePanelSize();

    QWidget *_panel = nullptr;
    QWidget *_close = nullptr; // top-right × (cancel), repositioned with the panel
    IntroWidget *_intro = nullptr;

    qreal _bgOpacity = 0.0;
    qreal _layerOpacity = 0.0;
    QVariantAnimation *_a_shown = nullptr;
    QVariantAnimation *_a_layerShown = nullptr;

    bool _shown = false;
    bool _closing = false;
    int _result = Rejected;
};

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>

class QEventLoop;
class QVariantAnimation;

namespace TeleMatrix {

/// In-window popup card hosting an intro-styled flow — adding a second account
/// (IntroWidget) or verifying this session (VerificationFlow) — instead of a
/// separate top-level window. Same dimmed-layer + rounded-panel look as the
/// other box dialogs.
///
/// Two ways to run it, because the hosted flows differ:
///   - show() + finished(int), for a flow that drives its own outcome
///     asynchronously (sign-in -> bridge signals -> in-flow verification);
///   - exec(), which blocks in a nested loop and returns the result, for a
///     caller that acts on the outcome inline.
class DialogsIntroBox final : public QWidget {
    Q_OBJECT

public:
    enum DialogCode { Rejected = 0, Accepted = 1 };

    /// Adopts `content` into the card. Does not take a bridge — the caller
    /// builds the hosted widget and wires its signals.
    DialogsIntroBox(QWidget *content, QWidget *parent = nullptr);

    /// Card height before it is clamped to the window. Defaults to the sign-in
    /// card's (st::introBoxHeight); a host with shorter content passes its own
    /// so the card isn't mostly empty. Call before show().
    void setPreferredHeight(int height);

    /// Close as success. Emits finished(Accepted).
    void accept();

    /// Close as dismissal. Emits finished(Rejected). Also what Escape, the
    /// top-right × and a click outside the card do.
    void reject();

    /// Show and block until closed, returning Accepted or Rejected. The caller
    /// owns the box in this mode (no self-delete) and should deleteLater() it.
    int exec();

signals:
    /// Mirrors QDialog::finished: callers that branch on outside state route
    /// both accept and reject through one handler.
    void finished(int result);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    bool eventFilter(QObject *obj, QEvent *event) override;
    void showEvent(QShowEvent *) override;

private:
    void closeWith(int result);
    void updatePanelSize();

    QWidget *_panel = nullptr;
    QWidget *_close = nullptr; // top-right × (cancel), repositioned with the panel
    QWidget *_content = nullptr;
    int _preferredHeight = 0;  // 0 = st::introBoxHeight, read at layout time

    qreal _bgOpacity = 0.0;
    qreal _layerOpacity = 0.0;
    QVariantAnimation *_a_shown = nullptr;
    QVariantAnimation *_a_layerShown = nullptr;

    // Non-null only while exec() is blocking.
    QEventLoop *_loop = nullptr;

    bool _shown = false;
    bool _closing = false;
    int _result = Rejected;
};

} // namespace TeleMatrix

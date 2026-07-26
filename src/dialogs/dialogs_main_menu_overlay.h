// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QPointer>
#include <QWidget>

class QVariantAnimation;

namespace TeleMatrix {

class AppController;
class DialogsMainMenuPanel;

class DialogsMainMenuOverlay final : public QWidget {
    Q_OBJECT

public:
    explicit DialogsMainMenuOverlay(
        AppController *controller,
        QWidget *parent = nullptr);

    void showAnimated();
    void hideAnimated();
    void toggle();

    [[nodiscard]] bool isShown() const;

Q_SIGNALS:
    void hidden();
    void newChatRequested();
    void exploreRoomsRequested();
    void savedMessagesRequested();
    void newRoomRequested();
    void verifySessionRequested();
    void settingsRequested();
    void signOutRequested();

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;

private:
    enum class State {
        Hidden,
        Opening,
        Visible,
        Closing,
    };

    void startAnimation(qreal to);
    void applyProgress(qreal progress);
    void syncPanelGeometry();
    void restoreFocusBeforeHide();

    DialogsMainMenuPanel *_panel = nullptr;
    QVariantAnimation *_animation = nullptr;
    QPointer<QWidget> _restoreFocus;
    qreal _progress = 0.0;
    State _state = State::Hidden;
};

} // namespace TeleMatrix

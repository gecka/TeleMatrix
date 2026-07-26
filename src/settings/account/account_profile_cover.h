// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "protocol/protocol_types.h"

#include <QImage>
#include <QWidget>

class QTimer;

namespace Ui {
class TextButton;
} // namespace Ui

namespace TeleMatrix {

class AppController;

class AccountProfileCover final : public QWidget {
    Q_OBJECT

public:
    AccountProfileCover(
        AppController *controller,
        QWidget *parent = nullptr);

    void setAccountSummary(const AccountSummary &summary, bool loaded);
    void setAvatarOperationInFlight(bool inFlight);

Q_SIGNALS:
    void uploadAvatarRequested();
    void deleteAvatarRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    [[nodiscard]] QString displayName() const;
    [[nodiscard]] QString userId() const;
    [[nodiscard]] QString avatarUrl() const;
    [[nodiscard]] bool canEditAvatar() const;
    // Lay out the two avatar buttons on one line, centred beside the avatar.
    void positionAvatarButtons();
    void updateAvatarHover(const QPoint &pos);

    AppController *_controller = nullptr;
    AccountSummary _summary;
    bool _summaryLoaded = false;
    bool _avatarHovered = false;
    bool _avatarDeleteHovered = false;
    bool _avatarOperationInFlight = false;
    ::Ui::TextButton *_updateButton = nullptr;
    ::Ui::TextButton *_deleteButton = nullptr;
    QTimer *_preloaderTimer = nullptr;
};

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>

class QEventLoop;
class QVariantAnimation;
class QPaintEvent;
class QKeyEvent;

namespace Ui {
class TextButton;
} // namespace Ui

namespace TeleMatrix {

/// Modal overlay dialog confirming destructive reset of cryptographic identity.
class ResetIdentityDialog final : public QWidget {
    Q_OBJECT

public:
    enum DialogCode { Rejected = 0, Accepted = 1 };

    explicit ResetIdentityDialog(QWidget *parent = nullptr);

    int exec();

private:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

    void accept();
    void reject();

    QWidget *_panel = nullptr;
    ::Ui::TextButton *_cancel = nullptr;
    ::Ui::TextButton *_resetButton = nullptr;
    QWidget *_closeButton = nullptr;

    qreal _bgOpacity = 0.0;
    qreal _layerOpacity = 0.0;
    QVariantAnimation *_a_shown = nullptr;
    QVariantAnimation *_a_layerShown = nullptr;

    QEventLoop *_loop = nullptr;
    int _result = Rejected;
};

} // namespace TeleMatrix

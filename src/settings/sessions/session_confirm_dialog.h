// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>

class QEventLoop;

namespace TeleMatrix {

// Modal confirm box for the sessions area: a title, a word-wrapped message and
// a primary / cancel button pair. Chrome mirrors SessionRenameDialog.
class SessionConfirmDialog final : public QWidget {
public:
    enum DialogCode { Rejected = 0, Accepted = 1 };

    SessionConfirmDialog(
        const QString &title,
        const QString &message,
        const QString &confirmText,
        QWidget *parent);

    int exec();

protected:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void accept();
    void reject();

    QWidget *_panel = nullptr;
    QEventLoop *_loop = nullptr;
    int _result = Rejected;
};

} // namespace TeleMatrix

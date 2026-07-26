// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QEventLoop;

namespace TeleMatrix {

class SessionRenameDialog final : public QWidget {
public:
    enum DialogCode { Rejected = 0, Accepted = 1 };

    SessionRenameDialog(const QString &currentName, QWidget *parent);

    int exec();
    [[nodiscard]] QString text() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void accept();
    void reject();

    QWidget *_panel = nullptr;
    QLineEdit *_field = nullptr;
    QLabel *_error = nullptr;
    QEventLoop *_loop = nullptr;
    int _result = Rejected;
};

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>

namespace TeleMatrix {

class SessionLoadingOverlay final : public QWidget {
public:
    SessionLoadingOverlay(
        const QString &title,
        const QString &text,
        QWidget *parent);

protected:
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    QWidget *_panel = nullptr;
};

} // namespace TeleMatrix

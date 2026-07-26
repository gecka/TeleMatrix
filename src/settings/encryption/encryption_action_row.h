// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QColor>
#include <QWidget>

namespace TeleMatrix {

class EncryptionActionRow final : public QWidget {
    Q_OBJECT

public:
    EncryptionActionRow(
        const QString &text,
        const QColor &color,
        QWidget *parent = nullptr);

Q_SIGNALS:
    void clicked();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QString _text;
    QColor _color;
    bool _hovered = false;
};

} // namespace TeleMatrix

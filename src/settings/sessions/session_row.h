// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "protocol/protocol_types.h"

#include <QWidget>

namespace Ui {
class TextButton;
} // namespace Ui

namespace TeleMatrix {

class SessionRow final : public QWidget {
    Q_OBJECT

public:
    SessionRow(
        const DeviceSession &session,
        const QString &subtitle,
        QWidget *parent = nullptr);

Q_SIGNALS:
    void renameRequested(const QString &deviceId, const QString &currentName);
    void signOutRequested(const QString &deviceId);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void positionButtons();

    QString _deviceName;
    QString _deviceId;
    QString _subtitle;
    bool _verified = false;
    bool _unverifiable = false;
    ::Ui::TextButton *_renameButton = nullptr;
    ::Ui::TextButton *_signOutButton = nullptr;
};

} // namespace TeleMatrix

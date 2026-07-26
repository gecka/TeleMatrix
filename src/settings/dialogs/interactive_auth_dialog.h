// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>

class QEventLoop;
class QLineEdit;
class QLabel;
class QVariantAnimation;
class QPaintEvent;
class QKeyEvent;

namespace Ui {
class TextButton;
} // namespace Ui

namespace TeleMatrix {

/// Modal overlay dialog for User-Interactive Authentication (password re-entry).
/// Used when the server requires re-authentication for destructive actions
/// like deleting devices or resetting identity.
class InteractiveAuthDialog final : public QWidget {
    Q_OBJECT

public:
    enum DialogCode { Rejected = 0, Accepted = 1 };

    InteractiveAuthDialog(
        const QString &userId,
        const QString &challengeJson,
        QWidget *parent = nullptr,
        const QString &title = QString(),
        const QString &description = QString(),
        const QString &confirmText = QString());

    int exec();

    /// Returns the auth JSON payload to send with the retry request.
    [[nodiscard]] QString authJson() const;

private:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

    void accept();
    void reject();
    void updateConfirmButton();

    QWidget *_panel = nullptr;
    QLineEdit *_passwordField = nullptr;
    QLabel *_errorLabel = nullptr;
    ::Ui::TextButton *_cancel = nullptr;
    ::Ui::TextButton *_confirm = nullptr;
    QWidget *_closeButton = nullptr;

    qreal _bgOpacity = 0.0;
    qreal _layerOpacity = 0.0;
    QVariantAnimation *_a_shown = nullptr;
    QVariantAnimation *_a_layerShown = nullptr;

    QEventLoop *_loop = nullptr;
    int _result = Rejected;

    QString _userId;
    QString _session; // UIA session from challengeJson
};

} // namespace TeleMatrix

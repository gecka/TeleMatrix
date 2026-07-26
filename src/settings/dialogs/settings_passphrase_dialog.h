// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>

class QEventLoop;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QPaintEvent;
class QVariantAnimation;

namespace Ui {
class TextButton;
} // namespace Ui

namespace TeleMatrix {

/// Modal overlay dialog requesting an encryption key passphrase.
class SettingsPassphraseDialog final : public QWidget {
    Q_OBJECT

public:
    enum DialogCode { Rejected = 0, Accepted = 1 };

    // requireConfirm adds a second "confirm" field (for first-time creation) and
    // only accepts when both match — there is no password recovery, so a typo on
    // creation would otherwise lock the user out of their own local data.
    SettingsPassphraseDialog(
        const QString &title,
        const QString &description,
        QWidget *parent = nullptr,
        bool requireConfirm = false);

    int exec();
    [[nodiscard]] QString passphrase() const;

private:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

    void accept();
    void reject();

    QWidget *_panel = nullptr;
    QLineEdit *_field = nullptr;
    QLineEdit *_confirm = nullptr;
    QLabel *_error = nullptr;
    bool _requireConfirm = false;
    ::Ui::TextButton *_cancel = nullptr;
    ::Ui::TextButton *_ok = nullptr;

    qreal _bgOpacity = 0.0;
    qreal _layerOpacity = 0.0;
    QVariantAnimation *_a_shown = nullptr;
    QVariantAnimation *_a_layerShown = nullptr;

    QEventLoop *_loop = nullptr;
    int _result = Rejected;
};

} // namespace TeleMatrix

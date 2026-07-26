// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>

class QEventLoop;
class QLineEdit;
class QCheckBox;
class QLabel;
class QAbstractButton;
class QVariantAnimation;
class QPaintEvent;
class QKeyEvent;

namespace Ui {
class TextButton;
} // namespace Ui

namespace TeleMatrix {

/// Modal overlay dialog for displaying or entering a recovery key.
/// Display mode: shows a generated key with copy-to-clipboard and confirmation.
/// Entry mode: lets the user type in a recovery key.
class RecoveryKeyDialog final : public QWidget {
    Q_OBJECT

public:
    enum DialogCode { Rejected = 0, Accepted = 1 };
    enum Mode { Display, Entry };

    RecoveryKeyDialog(Mode mode, const QString &key = QString(), QWidget *parent = nullptr);

    int exec();

    /// Returns the recovery key (entered text in Entry mode, original key in Display mode).
    [[nodiscard]] QString recoveryKey() const;

private:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

    void accept();
    void reject();
    void updateConfirmButton();

    /// Format a key string in 4-char groups separated by spaces.
    static QString formatKey(const QString &key);

    Mode _mode;
    QString _key;

    QWidget *_panel = nullptr;
    QLineEdit *_keyInput = nullptr;
    QLabel *_keyDisplay = nullptr;
    QCheckBox *_savedCheckbox = nullptr;
    QAbstractButton *_copyButton = nullptr;
    ::Ui::TextButton *_cancel = nullptr;
    ::Ui::TextButton *_confirmButton = nullptr;
    QWidget *_closeButton = nullptr;

    qreal _bgOpacity = 0.0;
    qreal _layerOpacity = 0.0;
    QVariantAnimation *_a_shown = nullptr;
    QVariantAnimation *_a_layerShown = nullptr;

    QEventLoop *_loop = nullptr;
    int _result = Rejected;
};

} // namespace TeleMatrix

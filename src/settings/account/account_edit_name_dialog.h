// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>

class QEventLoop;
class QLineEdit;
class QPlainTextEdit;
class QVariantAnimation;
class QPaintEvent;
class QMouseEvent;
class QKeyEvent;

namespace Ui {
class EmojiInputField;
class TextButton;
} // namespace Ui

namespace TeleMatrix {

/// Modal overlay dialog for editing a single name (centered box-style dialog).
/// `title`/`placeholder` default to the account display-name strings; pass them
/// to reuse the box for other names (e.g. a room name).
class AccountEditNameDialog final : public QWidget {
    Q_OBJECT

public:
    enum DialogCode { Rejected = 0, Accepted = 1 };

    explicit AccountEditNameDialog(
        const QString &currentName,
        QWidget *parent = nullptr,
        const QString &title = QString(),
        const QString &placeholder = QString(),
        bool multiline = false);

    int exec();

    [[nodiscard]] QString displayName() const;

private:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

    void accept();
    void reject();
    void updateSaveButton();

    QWidget *_panel = nullptr;
    Ui::EmojiInputField *_nameField = nullptr;
    QPlainTextEdit *_multilineField = nullptr;
    ::Ui::TextButton *_cancel = nullptr;
    ::Ui::TextButton *_save = nullptr;
    QWidget *_closeButton = nullptr;

    qreal _bgOpacity = 0.0;
    qreal _layerOpacity = 0.0;
    QVariantAnimation *_a_shown = nullptr;
    QVariantAnimation *_a_layerShown = nullptr;

    QEventLoop *_loop = nullptr;
    int _result = Rejected;
};

} // namespace TeleMatrix

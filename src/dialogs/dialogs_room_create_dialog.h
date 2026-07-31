// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QString>
#include <QWidget>

class QEventLoop;
class QLineEdit;
class QCheckBox;
class QVariantAnimation;
class QPaintEvent;
class QMouseEvent;
class QKeyEvent;
class QLabel;

namespace Ui {
class EmojiInputField;
class TextButton;
} // namespace Ui

namespace TeleMatrix {

struct CreateRoomRequest;

/// Modal overlay dialog for creating a new room.
/// Uses the layered box dialog style (layers.style).
class DialogsRoomCreateDialog final : public QWidget {
    Q_OBJECT

public:
    enum DialogCode { Rejected = 0, Accepted = 1 };

    explicit DialogsRoomCreateDialog(const QString &serverName = QString(), QWidget *parent = nullptr);

    int exec();
    void accept();
    void reject();

    [[nodiscard]] QString roomName() const;
    [[nodiscard]] QString roomTopic() const;
    [[nodiscard]] bool isPublic() const;
    [[nodiscard]] bool isEncrypted() const;
    [[nodiscard]] QString roomAlias() const;
    [[nodiscard]] QString avatarPath() const;
    [[nodiscard]] int guestAccess() const;
    [[nodiscard]] int historyVisibility() const;
    [[nodiscard]] bool blockFederated() const;

    /// Show an error message in the dialog (e.g. from backend failure).
    void showError(const QString &message);
    /// Set controls enabled/disabled (for in-flight state).
    void setControlsEnabled(bool enabled);
    [[nodiscard]] bool controlsEnabled() const;

signals:
    void createRequested();

private:
    void init();

    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;
    void updateCreateButton();
    void updateVisibilityFields();
    void showGuestAccessOptions();
    void showHistoryVisibilityOptions();
    void updateOptionRows();

    QWidget *_panel = nullptr;
    Ui::EmojiInputField *_nameField = nullptr;
    Ui::EmojiInputField *_topicField = nullptr;
    QLineEdit *_aliasField = nullptr;
    QWidget *_aliasSlot = nullptr;
    QWidget *_aliasContainer = nullptr;
    QLabel *_aliasSuffixLabel = nullptr;
    QCheckBox *_publicCheck = nullptr;
    QCheckBox *_encryptedCheck = nullptr;
    QCheckBox *_federationCheck = nullptr;
    QWidget *_guestAccessSlot = nullptr;
    QWidget *_guestAccessButton = nullptr;
    QWidget *_historyVisibilityButton = nullptr;
    ::Ui::TextButton *_cancel = nullptr;
    ::Ui::TextButton *_create = nullptr;
    QLabel *_errorLabel = nullptr;

    QString _serverName;
    int _guestAccess = 0;
    int _historyVisibility = 2;

    // Close (X) button painted widget.
    QWidget *_closeButton = nullptr;

    // _a_shown fades the background (easeOutCirc); _a_layerShown fades the box shadow (linear).
    qreal _bgOpacity = 0.0;
    qreal _layerOpacity = 0.0;
    QVariantAnimation *_a_shown = nullptr;
    QVariantAnimation *_a_layerShown = nullptr;

    QEventLoop *_loop = nullptr;
    int _result = Rejected;
    bool _controlsEnabled = true;
};

} // namespace TeleMatrix

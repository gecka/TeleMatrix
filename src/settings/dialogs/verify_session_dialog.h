// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QString>
#include <QWidget>

class QEventLoop;
class QStackedWidget;
class QLineEdit;
class QLabel;
class QAbstractButton;
class QVariantAnimation;
class QPaintEvent;
class QKeyEvent;

namespace Ui {
class TextButton;
} // namespace Ui

namespace TeleMatrix {

class ProtocolBridge;

/// Modal overlay dialog for verifying the current session.
/// Replicates the intro verification flows (emoji SAS + recovery key)
/// in a settings-accessible dialog. Uses a QStackedWidget with 4 pages:
///   0 = Choice (emoji / recovery key)
///   1 = Emoji SAS comparison
///   2 = Recovery key entry
///   3 = Success
class VerifySessionDialog final : public QWidget {
    Q_OBJECT

public:
    enum DialogCode { Rejected = 0, Accepted = 1 };
    enum class StartMode {
        Choice,
        Emoji,
    };

    VerifySessionDialog(
        ProtocolBridge *bridge,
        QWidget *parent = nullptr,
        StartMode startMode = StartMode::Choice,
        const QString &transactionId = QString(),
        const QString &targetUserId = QString(),
        const QString &targetDisplayName = QString());
    ~VerifySessionDialog() override;

    int exec();

private:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

    void accept();
    void reject();

    void buildChoicePage();
    void buildEmojiPage();
    void buildQrPage();
    void buildRecoveryPage();
    void buildSuccessPage();
    void showPage(int index);

    ProtocolBridge *_bridge = nullptr;
    StartMode _startMode = StartMode::Choice;
    QString _transactionId;
    QString _targetUserId;      // non-empty => verifying ANOTHER user, not our session
    QString _targetDisplayName;
    QWidget *_panel = nullptr;
    QStackedWidget *_stack = nullptr;
    QWidget *_closeButton = nullptr;

    // Page widgets for updating title.
    QLabel *_titleLabel = nullptr;

    // QR page widgets.
    QWidget *_qrPage = nullptr;
    QWidget *_qrDisplay = nullptr;
    QLabel *_qrWaitLabel = nullptr;
    ::Ui::TextButton *_qrConfirmButton = nullptr;
    QLabel *_qrErrorLabel = nullptr;

    // Emoji page widgets.
    QWidget *_emojiPage = nullptr;
    QWidget *_emojiContainer = nullptr;
    QLabel *_emojiWaitLabel = nullptr;
    ::Ui::TextButton *_emojiMatchButton = nullptr;
    QAbstractButton *_emojiNoMatchButton = nullptr;
    QLabel *_emojiErrorLabel = nullptr;

    // Recovery page widgets.
    QWidget *_recoveryPage = nullptr;
    QLineEdit *_recoveryInput = nullptr;
    ::Ui::TextButton *_recoverySubmitButton = nullptr;
    QLabel *_recoveryErrorLabel = nullptr;

    // Animations.
    qreal _bgOpacity = 0.0;
    qreal _layerOpacity = 0.0;
    QVariantAnimation *_a_shown = nullptr;
    QVariantAnimation *_a_layerShown = nullptr;

    QEventLoop *_loop = nullptr;
    int _result = Rejected;
};

} // namespace TeleMatrix

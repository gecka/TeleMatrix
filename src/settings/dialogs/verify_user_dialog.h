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

/// Modal overlay dialog for verifying ANOTHER user's identity by comparing
/// emoji — outgoing (from a profile or the trust warning bar) or incoming (a
/// request that arrived in a room). Two pages: emoji comparison and success.
///
/// Verifying our OWN session is a different flow with its own screens, shared
/// with first-run — see ShowVerifySessionPopup / VerificationFlow.
class VerifyUserDialog final : public QWidget {
    Q_OBJECT

public:
    enum DialogCode { Rejected = 0, Accepted = 1 };

    /// `targetUserId` non-empty starts an outgoing verification of that user;
    /// otherwise `flowId` must name the incoming request to attach to.
    /// `targetDisplayName` titles the dialog.
    VerifyUserDialog(
        ProtocolBridge *bridge,
        QWidget *parent,
        const QString &targetUserId,
        const QString &targetDisplayName,
        const QString &flowId = QString());
    ~VerifyUserDialog() override;

    int exec();

private:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

    void accept();
    void reject();

    void buildEmojiPage();
    void buildSuccessPage();
    void showPage(int index);

    ProtocolBridge *_bridge = nullptr;
    QString _transactionId;
    // SDK cancel code for this dialog's flow, latched from
    // verificationCancelInfo just before the Cancelled it explains arrives.
    QString _lastCancelCode;
    // Correlation id of our in-flight start call, 0 when none. A bool is not
    // enough: the session-verification popup's emoji page is another
    // main-window surface with no modality between us, so both can have a
    // start in flight and both receive every (broadcast) reply.
    quint64 _startRequestId = 0;
    // Set once this dialog has shown emojis for its own flow. Gates the success
    // page: _transactionId can hold a foreign flow's id, and claiming "verified"
    // for a flow we did not run is a security lie.
    bool _emojisShown = false;
    QString _targetUserId;  // non-empty => outgoing; empty => incoming request
    QString _targetDisplayName;
    QWidget *_panel = nullptr;
    QStackedWidget *_stack = nullptr;
    QWidget *_closeButton = nullptr;

    // Page widgets for updating title.
    QLabel *_titleLabel = nullptr;

    // Emoji page widgets.
    QWidget *_emojiPage = nullptr;
    QWidget *_emojiContainer = nullptr;
    QLabel *_emojiWaitLabel = nullptr;
    ::Ui::TextButton *_emojiMatchButton = nullptr;
    QAbstractButton *_emojiNoMatchButton = nullptr;
    QLabel *_emojiErrorLabel = nullptr;

    // Animations.
    qreal _bgOpacity = 0.0;
    qreal _layerOpacity = 0.0;
    QVariantAnimation *_a_shown = nullptr;
    QVariantAnimation *_a_layerShown = nullptr;

    QEventLoop *_loop = nullptr;
    int _result = Rejected;
};

} // namespace TeleMatrix

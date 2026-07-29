// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "intro_step.h"

#include <QStringList>

class QPushButton;

namespace TeleMatrix {

class ProtocolBridge;

class IntroVerifyEmoji : public IntroStep {
    Q_OBJECT

public:
    explicit IntroVerifyEmoji(QWidget *parent, ProtocolBridge *bridge);

    void activate() override;
    void submit() override;
    QString nextButtonText() const override;

    // Ignore a Cancelled state belonging to this flow id — used when switching
    // here from a QR flow we deliberately tore down ("compare emoji instead"),
    // so that flow's cancellation does not surface as a failure on this page.
    void ignoreFlow(const QString &flowId) { _ignoredFlowId = flowId; }

    // Attach to an existing verification request rather than starting a fresh
    // outgoing one — set when this page is opened from an incoming request.
    // Consumed by the first start: once that request is cancelled or times out
    // there is nothing left to attach to, so Retry starts our own flow instead.
    void setRequestFlowId(const QString &flowId) { _requestFlowId = flowId; }

    // Show or hide the "use another method" links. Off for an incoming request:
    // it is one specific flow the other session is waiting on, and switching
    // method cancels it out from under them.
    void setShowsAlternativeMethods(bool shows);

signals:
    void mismatch();
    void verified();
    void useRecoveryKeyVerification();
    void useQrVerification();
    void skipVerification();

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void updateSkipVisibility() override;

private:
    void startVerification();
    void onSasStarted(bool success, const QStringList &emojis, const QStringList &labels);
    void onSasConfirmed(bool success);
    void setWaitingState(bool waiting);
    void showFailure(const QString &message);
    void updateEmojiLayout();
    void paintEmojiContainer(QPainter &p);

    ProtocolBridge *_bridge = nullptr;
    QPushButton *_mismatchLink = nullptr;
    QPushButton *_retryLink = nullptr;
    QPushButton *_recoveryKeyLink = nullptr;
    QPushButton *_qrLink = nullptr;
    QPushButton *_skipLink = nullptr;

    QStringList _emojis;
    QStringList _labels;
    bool _waiting = false;
    QString _ignoredFlowId;
    QString _requestFlowId;
};

} // namespace TeleMatrix

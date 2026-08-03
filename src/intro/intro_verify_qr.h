// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "intro_step.h"

#include <QByteArray>

class QPushButton;

namespace TeleMatrix {

class ProtocolBridge;

class IntroVerifyQr : public IntroStep {
    Q_OBJECT

public:
    explicit IntroVerifyQr(QWidget *parent, ProtocolBridge *bridge);

    void activate() override;
    void submit() override;
    QString nextButtonText() const override;

    // Flow id of the verification request this page is showing, latched from
    // verification-state updates; empty until known. Used so the caller can tell
    // a following page which flow to ignore when switching away.
    QString currentFlowId() const { return _flowId; }

    // Correlation id of a start call still on the wire, 0 when none. See
    // IntroVerifyEmoji::pendingStartRequestId.
    [[nodiscard]] quint64 pendingStartRequestId() const { return _startRequestId; }

signals:
    void verified();
    void useEmojiVerification();
    void useRecoveryKeyVerification();
    void skipVerification();

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void updateSkipVisibility() override;

private:
    void startVerification();
    void onQrCodeReady(quint64 requestId, bool success, const QString &flowId);
    void onQrScanConfirmed(bool success);
    void setScannedState();
    void setWaitingState(bool waiting);
    void showFailure(const QString &message);
    void updateLayout();
    void paintQrContainer(QPainter &p);

    ProtocolBridge *_bridge = nullptr;
    QPushButton *_emojiLink = nullptr;
    QPushButton *_recoveryLink = nullptr;
    QPushButton *_skipLink = nullptr;
    QPushButton *_retryLink = nullptr;

    QByteArray _modules;
    int _qrSize = 0;
    bool _scanned = false;
    bool _waiting = false;
    // Correlation id of our in-flight start call, 0 when none. Only one
    // surface consumes qrCodeReady today; this mirrors the emoji page so the
    // ownership rule is the same everywhere.
    quint64 _startRequestId = 0;
    QString _flowId;
    // SDK cancel code for the current attempt's flow, latched from
    // verificationCancelInfo just before the Cancelled it explains arrives.
    QString _lastCancelCode;
};

} // namespace TeleMatrix

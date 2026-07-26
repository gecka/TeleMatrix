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
    explicit IntroVerifyEmoji(IntroWidget *parent, ProtocolBridge *bridge);

    void activate() override;
    void submit() override;
    QString nextButtonText() const override;

    // Ignore a Cancelled state belonging to this flow id — used when switching
    // here from a QR flow we deliberately tore down ("compare emoji instead"),
    // so that flow's cancellation does not surface as a failure on this page.
    void ignoreFlow(const QString &flowId) { _ignoredFlowId = flowId; }

signals:
    void mismatch();
    void verified();
    void useRecoveryKeyVerification();
    void useQrVerification();
    void skipVerification();

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

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
};

} // namespace TeleMatrix

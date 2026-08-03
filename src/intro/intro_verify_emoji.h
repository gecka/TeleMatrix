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

    // Flow id of the verification request this page is showing: the latched SAS
    // flow if emojis have arrived, else the incoming request being answered —
    // so a caller can identify what to cancel/ignore even before emojis land.
    // Empty when neither is known.
    QString currentFlowId() const { return _flowId.isEmpty() ? _requestFlowId : _flowId; }

    // Correlation id of a start call still on the wire, 0 when none. Non-zero
    // together with an empty currentFlowId() means this page owns a flow it
    // cannot yet name, so a cancel has to wait for the reply.
    [[nodiscard]] quint64 pendingStartRequestId() const { return _startRequestId; }

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

    // Adopt a SAS the other device started (it picked emoji while we were on
    // another page). Rendered on the next activate() instead of starting a
    // fresh flow.
    void presentAdoptedSas(
        const QString &flowId,
        const QStringList &emojis,
        const QStringList &labels);

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
    void resetForAttempt();
    void presentEmojis(const QStringList &emojis, const QStringList &labels);
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
    // Correlation id of our in-flight start call, 0 when none. A bool is not
    // enough: this page and VerifyUserDialog are both main-window surfaces
    // with no modality between them, so both can have a start in flight and
    // both receive every (broadcast) reply.
    quint64 _startRequestId = 0;
    // The flow this page currently renders, latched from our own start call's
    // reply and re-confirmed by the emojis that follow.
    QString _flowId;
    // SDK cancel code for the current attempt's flow, latched from
    // verificationCancelInfo just before the Cancelled it explains arrives.
    QString _lastCancelCode;
    bool _presentPending = false;
    QStringList _pendingEmojis;
    QStringList _pendingLabels;
};

} // namespace TeleMatrix

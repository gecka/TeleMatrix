// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QWidget>

class QStackedWidget;

namespace TeleMatrix {

class ProtocolBridge;
class IntroVerifyChoice;
class IntroVerifyEmoji;
class IntroVerifyQr;
class IntroVerifyRecoveryKey;
class IntroVerifySuccess;
class IntroSetupEncryption;

/// The session-verification screens (choose a method, compare emoji, scan a QR
/// code, enter a recovery key, set encryption up) as a standalone widget, so the
/// in-app "Verify this session" popup shows the same screens as first-run rather
/// than a second implementation of them.
///
/// Self-session only. Verifying ANOTHER user is a different flow with different
/// screens — see VerifyUserDialog.
///
/// Sized like the intro stage: the steps center a fixed-width column in whatever
/// they are given, so the host should be roughly intro-window sized.
class VerificationFlow : public QWidget {
    Q_OBJECT

public:
    enum class Entry {
        /// Offer QR / emoji / recovery key.
        Choice,
        /// Straight to emoji comparison, for an incoming request.
        Emoji,
    };

    explicit VerificationFlow(ProtocolBridge *bridge, QWidget *parent = nullptr);

    /// Show the first screen and start whatever it needs. `flowId` attaches the
    /// Emoji entry to an existing incoming request; ignored for Choice.
    void start(Entry entry, const QString &flowId = QString());

    /// Tear down any verification still in flight. Call when the host is
    /// dismissed without finishing; a no-op once a method has succeeded.
    void cancel();

    /// Flow id owned by whichever method page is currently shown (Emoji or
    /// QR), or empty for any other page. Used to scope a cancel to the flow
    /// the user is actually leaving instead of cancelling everything.
    QString activeFlowId() const;

    /// Whether a method has succeeded, whether or not the success screen has
    /// been dismissed. Lets a host that is closed early still report success.
    [[nodiscard]] bool isVerified() const { return _verified; }

signals:
    /// A method succeeded. Emitted before the success screen shows.
    void verified();
    /// The success screen was dismissed — nothing left to do.
    void done();
    /// The user chose "Skip for now".
    void skipped();

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    /// Correlation id of a start call the current method page still has on the
    /// wire, 0 for any other page or when none is pending.
    [[nodiscard]] quint64 pendingStartRequestId() const;
    void showStep(int index);
    /// Latch success and show the success screen.
    void onVerified();
    /// "Skip for now" from any screen.
    void onSkipped();

    ProtocolBridge *_bridge = nullptr;
    bool _verified = false;
    // Whether the backend flow has been settled (verified, skipped or
    // cancelled), so cancel() doesn't tear down a second time.
    bool _finished = false;

    QStackedWidget *_stack = nullptr;
    IntroVerifyChoice *_choiceStep = nullptr;          // 0
    IntroVerifyEmoji *_emojiStep = nullptr;            // 1
    IntroVerifyQr *_qrStep = nullptr;                  // 2
    IntroVerifyRecoveryKey *_recoveryKeyStep = nullptr; // 3
    IntroVerifySuccess *_successStep = nullptr;        // 4
    IntroSetupEncryption *_setupEncryptionStep = nullptr; // 5
};

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QElapsedTimer>
#include <QRect>
#include <QWidget>

class QTimer;
class QVariantAnimation;

namespace Ui {

/// Connection indicator: a `windowBg`
/// capsule (40px tall, semicircular caps) flush to the bottom-left of the chat
/// list, with a radial spinner. Two-phase logic: while a
/// connection attempt is in flight it shows just "Connecting…"; once it's waiting
/// out the retry backoff it shows "Reconnect in N s…" with a clickable "Try now"
/// link that skips the wait. Slides up from the bottom (no fade, 150ms easeOutCirc).
class ConnectingWidget : public QWidget {
    Q_OBJECT

public:
    explicit ConnectingWidget(QWidget *parent = nullptr);

    /// Drive the indicator: connected hides it; disconnected runs the
    /// Connecting → Reconnect-countdown progression.
    void setConnected(bool connected);

    /// X offset of the chat list within the parent (past the filters sidebar), so
    /// the pill sits at the chat list's left edge rather than the window's.
    void setLeftOffset(int x) { _leftOffset = x; }

    /// Extra gap above the parent's bottom edge, so the pill clears anything
    /// pinned there (the chat-list update bar).
    void setBottomSkip(int skip);

    /// Recompute geometry against the parent's bottom-left. Call from the
    /// parent's resizeEvent so it tracks window resizes.
    void reposition();

signals:
    void retryRequested(); // "Try now" clicked, or the countdown elapsed

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    enum class Phase { Hidden, Suppressing, Connecting, Reconnecting };

    void enterConnecting();
    void enterReconnecting();
    void onCountdownTick();
    void slideTo(bool visible);
    [[nodiscard]] bool showLink() const { return _phase == Phase::Reconnecting; }
    [[nodiscard]] QString labelText() const;
    [[nodiscard]] int contentWidthFor() const; // body width for the current text
    void retargetWidth();
    void applyGeometry();
    void setSpinnerRunning(bool running);

    bool _connected = true;
    Phase _phase = Phase::Hidden;
    int _retrySeconds = 0;
    int _backoffSeconds = 5;     // grows per failed cycle, reset on connect
    int _leftOffset = 0;         // chat list's x within the parent (past sidebar)
    int _bottomSkip = 0;         // height of whatever sits at the parent's bottom
    qreal _visibility = 0.0;     // 0 = slid below the bottom edge, 1 = resting
    qreal _contentWidth = 0.0;   // animated body width
    QRect _linkRect;             // "Try now" hit rect (widget coords), set in paint
    bool _linkHovered = false;
    QTimer *_phaseTimer = nullptr;     // suppress→connecting, connecting→reconnecting
    QTimer *_countdownTimer = nullptr; // 1s "Reconnect in N s…" countdown
    QVariantAnimation *_visibilityAnim = nullptr;
    QVariantAnimation *_widthAnim = nullptr;
    QTimer *_spinnerTimer = nullptr;
    QElapsedTimer _spinnerClock;
};

} // namespace Ui

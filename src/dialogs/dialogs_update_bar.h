// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QWidget>

class QVariantAnimation;

namespace TeleMatrix {

/// The update bar pinned to the bottom of the rooms list.
///
/// Its one-line Ready state is a port of tdesktop's
/// `Dialogs::Widget::BottomButton` in its `hasTextIcon` form: 46px, a horizontal
/// groupCallLive1→2 gradient, semibold centred label after the punched-out
/// circular-arrows glyph, and glyph-only below `columnMinimalWidthLeft`.
///
/// Prompt and Downloading are TeleMatrix additions with no upstream counterpart
/// — tdesktop has no notify-only mode to prompt for. They are taller, with a
/// message row over an action row; the action row is the compose area's height,
/// the same 46 as the compose field, and its buttons follow HistoryWidget's
/// invitation DECLINE/ACCEPT bar: flush halves, square, transparent until hovered.
/// The actions are drawn, not child widgets, following
/// DialogsVerificationBanner's hover-rect convention.
class DialogsUpdateBar final : public QWidget {
    Q_OBJECT

public:
    enum class Mode {
        Ready,       // verified payload waiting — whole surface applies
        Prompt,      // a version exists but is not downloaded — Skip / Update
        Downloading, // progress track + Cancel
    };

    explicit DialogsUpdateBar(QWidget *parent = nullptr);
    ~DialogsUpdateBar() override;

    /// `label` is the whole-surface action's text ("Update TeleMatrix", or
    /// "Updating…" while staging, in which case pass enabled = false).
    void setReadyMode(const QString &label, bool enabled);
    void setPromptMode(const QString &message);
    /// `percent` < 0 means indeterminate (no total known yet).
    void setDownloadingMode(const QString &message, int percent);

    [[nodiscard]] Mode mode() const { return _mode; }
    /// Height this bar wants for its current mode. Drives the parent's layout.
    [[nodiscard]] int barHeight() const;

Q_SIGNALS:
    void applyRequested();
    void skipRequested();
    void updateRequested();
    void cancelRequested();

protected:
    void paintEvent(QPaintEvent *e) override;
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

private:
    enum class Action { None, Primary, Secondary };

    void paintReady(QPainter &p);
    void paintTwoLine(QPainter &p);
    void paintAction(
        QPainter &p, const QRect &rect, const QString &text, bool hovered);
    void paintActionRowTopLine(QPainter &p);
    void paintRipple(QPainter &p);
    void refreshCursor();
    void startRipple(QPoint origin);
    void stopRipple();

    /// Everything above the action row. The action row is fixed at the compose
    /// area's height, so this absorbs the remainder.
    [[nodiscard]] int firstRowHeight() const;
    [[nodiscard]] QRect actionRowRect() const;
    /// Secondary = Skip (Prompt only); Primary = Update / Cancel. Empty rects in
    /// modes that do not use them. Together they tile the whole action row.
    [[nodiscard]] QRect primaryRect() const;
    [[nodiscard]] QRect secondaryRect() const;
    [[nodiscard]] QString primaryText() const;
    [[nodiscard]] QString secondaryText() const;
    [[nodiscard]] Action actionAt(QPoint pos) const;
    /// Whole-surface click target, i.e. Ready and enabled.
    [[nodiscard]] bool surfaceClickable() const;

    Mode _mode = Mode::Ready;
    QString _label;   // Ready
    QString _message; // Prompt / Downloading first line
    bool _enabled = true;
    int _percent = -1;
    QImage _textIcon;
    bool _over = false;
    Action _hovered = Action::None;

    // lib_ui's RippleAnimation reduced to the one case the Ready surface needs: a
    // circle grown from the press point over the whole rect, faded out on
    // release. Same 650ms/200ms and `shadowFg` as universalRippleAnimation.
    QPoint _rippleOrigin;
    qreal _rippleRadius = 0.;
    qreal _rippleOpacity = 0.;
    QVariantAnimation *_rippleGrow = nullptr;
    QVariantAnimation *_rippleFade = nullptr;
};

} // namespace TeleMatrix

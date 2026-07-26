// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QLabel>
#include <QTimer>
#include <QPropertyAnimation>

namespace Ui {

/// Temporary toast notification shown centered at the bottom of the parent
/// widget, fading in/out.
///
/// Style values:
///   bg: #2c3033e5, fg: #ffffff, radius: 6px
///   padding: 19px left, 13px top, 19px right, 12px bottom
///   fadeIn: 200ms, visible: 1500ms, fadeOut: 1000ms
class ToastWidget : public QWidget {
    Q_OBJECT
    Q_PROPERTY(qreal shownLevel READ shownLevel WRITE setShownLevel)

public:
    explicit ToastWidget(QWidget *parent);

    void showToast(const QString &text);

    // Like showToast(), but the widget deletes itself once it has faded out.
    // Used by the free Ui::ShowToast() helper for one-shot confirmations.
    void showTransient(const QString &text);

    qreal shownLevel() const { return _shownLevel; }
    void setShownLevel(qreal level);

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    void startFadeOut();
    void positionInParent();

    QString _text;
    qreal _shownLevel = 0.0;
    QTimer _hideTimer;
    QPropertyAnimation _fadeIn;
    QPropertyAnimation _fadeOut;
};

// Show a one-shot toast (e.g. a "copied" confirmation) centered on the active
// top-level window. Creates a self-deleting ToastWidget; safe to call from any
// popup/dialog without owning a toast. No-op if there is no visible window.
void ShowToast(const QString &text);

} // namespace Ui

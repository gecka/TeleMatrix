// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "intro_step.h"

#include <QPixmap>

class QPushButton;

namespace TeleMatrix {

/// First-run screen: the app icon, the wordmark, and the two ways in.
///
/// `goNext()` means "Sign in"; `createAccountRequested()` jumps straight to the
/// register form. Key storage is NOT asked here — it has a default and is
/// reachable from the stage's "Keys: … Change" line.
class IntroStart : public IntroStep {
    Q_OBJECT

public:
    explicit IntroStart(IntroWidget *parent);

    void activate() override;
    void submit() override;
    QString nextButtonText() const override;

signals:
    void createAccountRequested();

protected:
    void resizeEvent(QResizeEvent *e) override;
    void paintEvent(QPaintEvent *e) override;

private:
    [[nodiscard]] int contentHeight() const;
    [[nodiscard]] QFont titleFont() const;

    QPixmap _icon;
    QRect _iconRect;
    QRect _titleRect;
    QPushButton *_signIn = nullptr;
    QPushButton *_createAccount = nullptr;
};

} // namespace TeleMatrix

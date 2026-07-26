// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "intro_step.h"

namespace TeleMatrix {

class IntroVerifySuccess : public IntroStep {
    Q_OBJECT

public:
    explicit IntroVerifySuccess(IntroWidget *parent);

    void activate() override;
    void submit() override;
    QString nextButtonText() const override;

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    void updateSuccessLayout();
};

} // namespace TeleMatrix

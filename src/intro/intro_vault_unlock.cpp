// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "intro_vault_unlock.h"

#include "../protocol/protocol_bridge.h"
#include "../styles/style_constants.h"
#include "intro_colors.h"
#include "intro_widgets.h"

#include "ui/painter.h"

#include <QEventLoop>
#include <QFontMetrics>
#include <QFutureWatcher>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimer>
#include <QtConcurrent>

namespace TeleMatrix {

namespace {

using IntroLineEdit = intro::Field;
using IntroLinkButton = intro::LinkButton;


} // namespace

IntroVaultUnlock::IntroVaultUnlock(QWidget *parent)
    : IntroStep(parent, false /* hasCover */) {
    setTitleText(tr("Unlock TeleMatrix"));
    setDescriptionText(tr("Enter your master password to open your local data"));

    _password = new IntroLineEdit(this);
    _password->setPlaceholderText(tr("Master password"));
    _password->setEchoMode(QLineEdit::Password);
    _password->setFixedSize(st::introCountryWidth, st::introCountryHeight);
    _password->setFont(st::baseFont(16));
    _password->setTextMargins(12, 0, 12, 0);

    _passwordToggle = new IntroLinkButton(tr("Show"), this);
    _passwordToggle->setFont(st::baseFont(13));
    _passwordToggle->adjustSize();
    connect(_passwordToggle, &QPushButton::clicked,
            this, &IntroVaultUnlock::togglePasswordVisibility);

    _resetLink = new IntroLinkButton(tr("Reset local data"), this);
    _resetLink->setFont(st::baseFont(13));
    _resetLink->adjustSize();
    connect(_resetLink, &QPushButton::clicked,
            this, &IntroVaultUnlock::resetRequested);

    connect(_password, &QLineEdit::textChanged, this, [this] {
        hideError();
        updateButtonEnabled();
        updateFieldLayout();
    });
    connect(_password, &QLineEdit::returnPressed, this, &IntroVaultUnlock::submit);

    setTabOrder(_password, nextButton());
}

void IntroVaultUnlock::activate() {
    IntroStep::activate();
    updateButtonEnabled();
    _password->setFocus();
}

QString IntroVaultUnlock::nextButtonText() const {
    return tr("Unlock");
}

void IntroVaultUnlock::submit() {
    const auto pass = _password->text();
    if (pass.isEmpty()) {
        return; // The button is disabled here anyway.
    }
    // Show the busy, disabled state.
    hideError();
    nextButton()->setText(tr("Unlocking..."));
    nextButton()->setEnabled(false);
    _password->setEnabled(false);
    _passwordToggle->setEnabled(false);

    // The unlock derives the vault key with Argon2id (64 MiB x 3) -- ~1s. Run it
    // off the UI thread and pump a local loop, so the "Unlocking..." state actually
    // paints (a blocking call on the UI thread never flushes the frame to screen).
    QFutureWatcher<int> watcher;
    QEventLoop loop;
    connect(&watcher, &QFutureWatcher<int>::finished, &loop, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([pass] {
        return ProtocolBridge::secretStoreUnlock(pass);
    }));
    loop.exec();

    const int code = watcher.result();
    if (code == 0) {
        Q_EMIT unlocked();
        return;
    }
    // Failed: restore the form and report. Select (don't clear) the field --
    // clearing would fire textChanged, whose handler hides the error we just set.
    nextButton()->setText(nextButtonText());
    _password->setEnabled(true);
    _passwordToggle->setEnabled(true);
    updateButtonEnabled();
    // Code 1 is a wrong password; anything else means the vault itself can't be
    // opened, where retyping the password can never help.
    showError(code == 1
        ? tr("Incorrect master password")
        : tr("The vault file can't be opened — it may be damaged. "
             "You can reset local data and sign in again"));
    _password->selectAll();
    _password->setFocus();
    updateFieldLayout();
}

void IntroVaultUnlock::togglePasswordVisibility() {
    if (_password->echoMode() == QLineEdit::Password) {
        _password->setEchoMode(QLineEdit::Normal);
        _passwordToggle->setText(tr("Hide"));
    } else {
        _password->setEchoMode(QLineEdit::Password);
        _passwordToggle->setText(tr("Show"));
    }
    _passwordToggle->adjustSize();
    updateFieldLayout();
    _password->setFocus();
}

void IntroVaultUnlock::updateButtonEnabled() {
    nextButton()->setEnabled(!_password->text().isEmpty());
}

void IntroVaultUnlock::resizeEvent(QResizeEvent *e) {
    IntroStep::resizeEvent(e);
    updateFieldLayout();
}

void IntroVaultUnlock::updateFieldLayout() {
    const auto left = contentLeft();
    const auto top = contentTop();
    const auto fieldLeft = left + (st::introStepWidth - st::introCountryWidth) / 2;

    const auto y = top + st::introStepFieldTop;
    _password->move(fieldLeft, y);

    // Show/hide toggle inside the password field, reserving right text-margin so
    // the password text never runs under it.
    const auto toggleX =
        fieldLeft + st::introCountryWidth - _passwordToggle->width() - 8;
    const auto toggleY = y + (st::introCountryHeight - _passwordToggle->height()) / 2;
    _passwordToggle->move(toggleX, toggleY);
    _password->setTextMargins(12, 0, _passwordToggle->width() + 12, 0);

    const auto fieldsBottom = y + st::introCountryHeight;
    const auto nextLeft = (width() - st::introNextButtonWidth) / 2;
    nextButton()->move(nextLeft, fieldsBottom + 16);

    const auto linkY =
        fieldsBottom + 16 + st::introNextButtonHeight + st::introLinkTop;
    const auto linkLeft = (width() - _resetLink->width()) / 2;
    _resetLink->move(linkLeft, linkY);

    const auto errorY = linkY + _resetLink->height() + 8;
    errorLabel()->move(left, errorY);
    errorLabel()->setFixedWidth(st::introStepWidth);

    centerContentVertically();
}

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "intro_create_password.h"

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
#include <QtConcurrent>

namespace TeleMatrix {

namespace {

using IntroLineEdit = intro::Field;
using IntroLinkButton = intro::LinkButton;


QLineEdit *makeField(QWidget *parent, const QString &placeholder) {
    auto *field = new IntroLineEdit(parent);
    field->setPlaceholderText(placeholder);
    field->setEchoMode(QLineEdit::Password);
    field->setFixedSize(st::introCountryWidth, st::introCountryHeight);
    field->setFont(st::baseFont(16));
    field->setTextMargins(12, 0, 12, 0);
    return field;
}

} // namespace

IntroCreatePassword::IntroCreatePassword(QWidget *parent)
    : IntroStep(parent, false /* hasCover */) {
    setTitleText(tr("Create a master password"));

    _password = makeField(this, tr("Master password"));
    _confirm = makeField(this, tr("Confirm master password"));

    _passwordToggle = new IntroLinkButton(tr("Show"), this);
    _passwordToggle->setFont(st::baseFont(13));
    _passwordToggle->adjustSize();
    connect(_passwordToggle, &QPushButton::clicked,
            this, &IntroCreatePassword::togglePasswordVisibility);

    // "Skip and use system keychain" -- only when a keychain is actually available
    // (on Linux without a Secret Service the vault is the only option).
    if (ProtocolBridge::secretServiceAvailable()) {
        _skipLink = new IntroLinkButton(tr("Or skip and use system keychain"), this);
        _skipLink->setFont(st::baseFont(13));
        _skipLink->adjustSize();
        connect(_skipLink, &QPushButton::clicked,
                this, &IntroCreatePassword::skipToKeychain);
    }

    const auto onEdited = [this] {
        hideError();
        updateButtonEnabled();
    };
    connect(_password, &QLineEdit::textChanged, this, onEdited);
    connect(_confirm, &QLineEdit::textChanged, this, onEdited);
    connect(_password, &QLineEdit::returnPressed, this, [this] { _confirm->setFocus(); });
    connect(_confirm, &QLineEdit::returnPressed, this, &IntroCreatePassword::submit);

    setTabOrder(_password, _confirm);
    setTabOrder(_confirm, nextButton());
}

void IntroCreatePassword::activate() {
    IntroStep::activate();
    updateButtonEnabled();
    _password->setFocus();
}

QString IntroCreatePassword::nextButtonText() const {
    return tr("Continue");
}

void IntroCreatePassword::submit() {
    const auto pass = _password->text();
    if (pass.isEmpty()) {
        return; // The button is disabled here anyway.
    }
    if (pass != _confirm->text()) {
        showError(tr("The passwords don't match"));
        _confirm->selectAll();
        _confirm->setFocus();
        updateFieldLayout();
        return;
    }

    // Show the busy, disabled state.
    hideError();
    nextButton()->setText(tr("Creating..."));
    nextButton()->setEnabled(false);
    _password->setEnabled(false);
    _confirm->setEnabled(false);
    _passwordToggle->setEnabled(false);

    // Setting the passphrase derives the vault key with Argon2id (64 MiB x 3) --
    // ~1s. Run it off the UI thread and pump a local loop so the busy state paints
    // (a blocking call on the UI thread never flushes the frame to screen).
    QFutureWatcher<bool> watcher;
    QEventLoop loop;
    connect(&watcher, &QFutureWatcher<bool>::finished, &loop, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([pass] {
        return ProtocolBridge::secretStoreSetPassphrase(pass);
    }));
    loop.exec();

    if (watcher.result()) {
        Q_EMIT created();
        return;
    }
    // Failed: restore the form and report.
    nextButton()->setText(nextButtonText());
    _password->setEnabled(true);
    _confirm->setEnabled(true);
    _passwordToggle->setEnabled(true);
    updateButtonEnabled();
    showError(tr("Couldn't set up the vault. Please try again"));
    _password->setFocus();
    updateFieldLayout();
}

void IntroCreatePassword::togglePasswordVisibility() {
    const auto mode = (_password->echoMode() == QLineEdit::Password)
        ? QLineEdit::Normal
        : QLineEdit::Password;
    _password->setEchoMode(mode);
    _confirm->setEchoMode(mode);
    _passwordToggle->setText(
        mode == QLineEdit::Normal ? tr("Hide") : tr("Show"));
    _passwordToggle->adjustSize();
    updateFieldLayout();
    _password->setFocus();
}

void IntroCreatePassword::updateButtonEnabled() {
    nextButton()->setEnabled(!_password->text().isEmpty());
}

void IntroCreatePassword::resizeEvent(QResizeEvent *e) {
    IntroStep::resizeEvent(e);
    updateFieldLayout();
}

void IntroCreatePassword::updateFieldLayout() {
    const auto left = contentLeft();
    const auto top = contentTop();
    const auto fieldLeft = left + (st::introStepWidth - st::introCountryWidth) / 2;

    int y = top + st::introStepFieldTop;
    _password->move(fieldLeft, y);

    // Show/hide toggle inside the password field, reserving right text-margin so
    // the password text never runs under it.
    const auto toggleX =
        fieldLeft + st::introCountryWidth - _passwordToggle->width() - 8;
    const auto toggleY = y + (st::introCountryHeight - _passwordToggle->height()) / 2;
    _passwordToggle->move(toggleX, toggleY);
    _password->setTextMargins(12, 0, _passwordToggle->width() + 12, 0);

    y += st::introCountryHeight + st::introFieldSpacing;
    _confirm->move(fieldLeft, y);

    const auto fieldsBottom = y + st::introCountryHeight;
    const auto nextLeft = (width() - st::introNextButtonWidth) / 2;
    const auto nextTop = fieldsBottom + 16;
    nextButton()->move(nextLeft, nextTop);

    int below = nextTop + st::introNextButtonHeight;
    if (_skipLink) {
        const auto linkY = below + st::introLinkTop;
        _skipLink->move((width() - _skipLink->width()) / 2, linkY);
        below = linkY + _skipLink->height();
    }

    const auto errorY = below + 12;
    errorLabel()->move(left, errorY);
    errorLabel()->setFixedWidth(st::introStepWidth);

    centerContentVertically();
}

} // namespace TeleMatrix

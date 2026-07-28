// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "intro_login.h"
#include "intro_widget.h"

#include "../app/app_controller.h"
#include "../protocol/protocol_bridge.h"
#include "../styles/style_constants.h"
#include "intro_colors.h"
#include "intro_widgets.h"
#include "intro_constants.h"

#include "ui/painter.h"

#include <QFontMetrics>
#include <QLineEdit>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimer>

namespace TeleMatrix {

namespace {

// Gap between the two links on the sign-in row (redesign: 22px).
constexpr int kSignInLinkGap = 22;

// Delay before triggering homeserver discovery after typing stops.
constexpr int kDiscoveryDelayMs = 600;


using IntroLineEdit = intro::Field;

} // namespace

IntroLogin::IntroLogin(QWidget *parent, ProtocolBridge *bridge)
    : IntroStep(parent, false /* hasCover */)
    , _bridge(bridge)
{
    setTitleText(tr("Sign in"));
    setDescriptionText(QString());

    // Create input fields. The homeserver starts empty on purpose — no
    // matrix.org prefill.
    _homeserver = createField(tr("Homeserver"));

    _username = createField(tr("Username"));

    _password = createField(tr("Password"));
    _password->setEchoMode(QLineEdit::Password);

    // Password visibility toggle button.
    _passwordToggle = new intro::LinkButton(tr("Show"), this);
    _passwordToggle->setCursor(Qt::PointingHandCursor);
    {
        auto toggleFont = st::baseFont(13);
        _passwordToggle->setFont(toggleFont);
    }
    _passwordToggle->adjustSize();
    connect(_passwordToggle, &QPushButton::clicked,
            this, &IntroLogin::togglePasswordVisibility);

    // "Create account" link below the Sign In button.
    _createAccountLink = new intro::LinkButton(tr("Create account"), this);
    _createAccountLink->setCursor(Qt::PointingHandCursor);
    _createAccountLink->setFont(st::baseFont(13));
    _createAccountLink->adjustSize();
    connect(_createAccountLink, &QPushButton::clicked, this, [this]() {
        Q_EMIT goRegister();
    });

    // "Forgot password?" link below "Create account".
    _forgotPasswordLink = new intro::LinkButton(tr("Forgot password?"), this);
    _forgotPasswordLink->setCursor(Qt::PointingHandCursor);
    _forgotPasswordLink->setFont(st::baseFont(13));
    _forgotPasswordLink->adjustSize();
    connect(_forgotPasswordLink, &QPushButton::clicked, this, [this]() {
        Q_EMIT goForgotPassword();
    });

    // Tab order and Enter key navigation.
    setTabOrder(_homeserver, _username);
    setTabOrder(_username, _password);
    setTabOrder(_password, nextButton());

    // Enter signs in as soon as every field has something in it, whichever field
    // the cursor is in — walking to the next field is only what happens while the
    // form is still incomplete, and then it jumps to what is actually missing
    // rather than blindly to the field below.
    const auto onReturn = [this] {
        const auto homeserverMissing = _homeserver->text().trimmed().isEmpty();
        const auto usernameMissing = _username->text().trimmed().isEmpty();
        // Not trimmed: a password may legitimately be spaces.
        const auto passwordMissing = _password->text().isEmpty();
        if (!homeserverMissing && !usernameMissing && !passwordMissing) {
            submit();
        } else if (homeserverMissing) {
            _homeserver->setFocus();
        } else if (usernameMissing) {
            _username->setFocus();
        } else {
            _password->setFocus();
        }
    };
    connect(_homeserver, &QLineEdit::returnPressed, this, onReturn);
    connect(_username, &QLineEdit::returnPressed, this, onReturn);
    connect(_password, &QLineEdit::returnPressed, this, onReturn);

    // Auto-discover homeserver when user stops typing.
    auto *discoveryTimer = new QTimer(this);
    discoveryTimer->setSingleShot(true);
    connect(_homeserver, &QLineEdit::textChanged, this, [this, discoveryTimer]() {
        _discoveredUrl.clear();
        _pendingDiscoveryRequestId = 0;
        discoveryTimer->start(kDiscoveryDelayMs);
    });
    connect(discoveryTimer, &QTimer::timeout, this, &IntroLogin::startDiscovery);

    // Listen for login and discovery results from the protocol bridge.
    connect(_bridge, &ProtocolBridge::loginResult,
            this, &IntroLogin::onLoginResult);
    connect(_bridge, &ProtocolBridge::homeserverDiscovered,
            this, &IntroLogin::onHomeserverDiscovered);

    // Trigger initial discovery for the default homeserver.
    QTimer::singleShot(100, this, &IntroLogin::startDiscovery);

    // Sign in stays disabled until the form is complete.
    connect(_homeserver, &QLineEdit::textChanged,
            this, &IntroLogin::updateSubmitEnabled);
    connect(_username, &QLineEdit::textChanged,
            this, &IntroLogin::updateSubmitEnabled);
    connect(_password, &QLineEdit::textChanged,
            this, &IntroLogin::updateSubmitEnabled);
    updateSubmitEnabled();
}

bool IntroLogin::allFieldsFilled() const {
    // Password not trimmed: it may legitimately be spaces.
    return !_homeserver->text().trimmed().isEmpty()
        && !_username->text().trimmed().isEmpty()
        && !_password->text().isEmpty();
}

void IntroLogin::updateSubmitEnabled() {
    nextButton()->setEnabled(!_submitting && allFieldsFilled());
}

QLineEdit *IntroLogin::createField(const QString &placeholder) {
    auto *field = new IntroLineEdit(this);
    field->setPlaceholderText(placeholder);
    field->setFixedSize(st::introCountryWidth, st::introCountryHeight);
    return field;
}

void IntroLogin::activate() {
    IntroStep::activate();
    updateSubmitEnabled();
    _homeserver->setFocus();
}

void IntroLogin::startDiscovery() {
    const auto domain = _homeserver->text().trimmed();
    if (domain.isEmpty()) {
        return;
    }

    // Strip protocol prefix if the user typed a full URL.
    auto cleanDomain = domain;
    if (cleanDomain.startsWith(QStringLiteral("https://"))) {
        cleanDomain = cleanDomain.mid(8);
    } else if (cleanDomain.startsWith(QStringLiteral("http://"))) {
        cleanDomain = cleanDomain.mid(7);
    }
    // Strip trailing slash.
    while (cleanDomain.endsWith(QLatin1Char('/'))) {
        cleanDomain.chop(1);
    }

    _discovering = true;
    const auto requestId = _nextDiscoveryRequestId++;
    _pendingDiscoveryRequestId = requestId;
    _bridge->discoverHomeserver(cleanDomain, requestId);
}

void IntroLogin::onHomeserverDiscovered(
        quint64 requestId,
        bool success,
        const QString &url) {
    if (requestId == 0 || requestId != _pendingDiscoveryRequestId) {
        return;
    }

    _pendingDiscoveryRequestId = 0;
    _discovering = false;

    if (success && !url.isEmpty()) {
        _discoveredUrl = url;
    } else {
        _discoveredUrl.clear();
    }
}

void IntroLogin::togglePasswordVisibility() {
    if (_password->echoMode() == QLineEdit::Password) {
        _password->setEchoMode(QLineEdit::Normal);
        _passwordToggle->setText(tr("Hide"));
    } else {
        _password->setEchoMode(QLineEdit::Password);
        _passwordToggle->setText(tr("Show"));
    }
    _passwordToggle->adjustSize();
    updateFieldLayout(); // "Show"/"Hide" widths differ — re-place + re-margin.
    _password->setFocus();
}

void IntroLogin::submit() {
    if (_submitting) {
        return;
    }

    auto homeserver = _homeserver->text().trimmed();
    const auto user = _username->text().trimmed();
    const auto pass = _password->text();

    if (homeserver.isEmpty()) {
        showError(tr("Please enter a homeserver"));
        updateFieldLayout();
        _homeserver->setFocus();
        return;
    }
    if (user.isEmpty()) {
        showError(tr("Please enter your username"));
        updateFieldLayout();
        _username->setFocus();
        return;
    }
    if (pass.isEmpty()) {
        showError(tr("Please enter your password"));
        updateFieldLayout();
        _password->setFocus();
        return;
    }

    // The secret backend must be ready before login writes any secrets.
    switch (AppController::checkSecretBackendForNewSession()) {
    case AppController::SecretSetup::Ready:
        break;
    case AppController::SecretSetup::NeedsMasterPassword:
        Q_EMIT needMasterPassword(); // in-window create-password step
        return;
    case AppController::SecretSetup::KeychainError:
        showError(tr("Couldn't access your keychain. Please try again"));
        updateFieldLayout();
        return;
    }

    hideError();
    _submitting = true;
    setFieldsEnabled(false);
    nextButton()->setText(tr("Signing in..."));

    // Use discovered URL if available, otherwise construct from domain.
    if (!_discoveredUrl.isEmpty()) {
        homeserver = _discoveredUrl;
    } else if (!homeserver.startsWith(QStringLiteral("http"))) {
        homeserver = QStringLiteral("https://") + homeserver;
    }

    _bridge->login(homeserver, user, pass);
}

void IntroLogin::onLoginResult(
    bool success,
    const QString &userId,
    const QString & /*displayName*/,
    const QString & /*avatarUrl*/) {
    _submitting = false;
    setFieldsEnabled(true);
    nextButton()->setText(nextButtonText());

    if (success) {
        hideError();
        Q_EMIT goNext();
    } else {
        showError(tr("Sign in failed. Please check your credentials and homeserver"));
        updateFieldLayout();
    }
}

void IntroLogin::setFieldsEnabled(bool enabled) {
    _homeserver->setEnabled(enabled);
    _username->setEnabled(enabled);
    _password->setEnabled(enabled);
    _passwordToggle->setEnabled(enabled);
    nextButton()->setEnabled(enabled && allFieldsFilled());
    // Fix #11: disable navigation links while a login request is in flight
    // so the user cannot leave the step (and cause a background login completion).
    _createAccountLink->setEnabled(enabled);
    _forgotPasswordLink->setEnabled(enabled);
}

QString IntroLogin::nextButtonText() const {
    return tr("Sign in");
}

void IntroLogin::resizeEvent(QResizeEvent *e) {
    IntroStep::resizeEvent(e);
    updateFieldLayout();
}

void IntroLogin::updateFieldLayout() {
    const auto left = contentLeft();
    const auto top = contentTop();
    const auto fieldLeft = left + (st::introStepWidth - st::introCountryWidth) / 2;

    int y = contentStartTop();
    placeErrorAbove(y);

    _homeserver->move(fieldLeft, y);
    y += st::introCountryHeight + st::introFieldSpacing;
    _username->move(fieldLeft, y);
    y += st::introCountryHeight + st::introFieldSpacing;
    _password->move(fieldLeft, y);

    // Position the show/hide toggle inside the password field, reserving right
    // text-margin so the password text never runs under it.
    const auto toggleX = fieldLeft + st::introCountryWidth - _passwordToggle->width() - 8;
    const auto toggleY = y + (st::introCountryHeight - _passwordToggle->height()) / 2;
    _passwordToggle->move(toggleX, toggleY);
    _password->setTextMargins(12, 0, _passwordToggle->width() + 12, 0);

    // Position next button below the password field (override base layout
    // which uses introNextTop — insufficient for 3-field login form).
    const auto fieldsBottom = y + st::introCountryHeight;
    const auto nextLeft = (width() - st::introNextButtonWidth) / 2;
    nextButton()->move(nextLeft, fieldsBottom + st::introFieldsToButton);

    // "Create account" and "Forgot password?" sit on ONE centred row, 22px
    // apart, per the redesign — the verify screens stack their links, but the
    // sign-in pair is a row.
    const auto linkY = fieldsBottom + st::introFieldsToButton + st::introNextButtonHeight + st::introLinkTop;
    const auto rowWidth = _createAccountLink->width()
        + kSignInLinkGap
        + _forgotPasswordLink->width();
    const auto rowLeft = (width() - rowWidth) / 2;
    _createAccountLink->move(rowLeft, linkY);
    _forgotPasswordLink->move(
        rowLeft + _createAccountLink->width() + kSignInLinkGap, linkY);

    centerContentVertically();
}

} // namespace TeleMatrix

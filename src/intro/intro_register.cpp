// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "intro_register.h"
#include "intro_widget.h"

#include "../app/app_controller.h"
#include "../protocol/protocol_bridge.h"
#include "../styles/style_constants.h"
#include "intro_colors.h"
#include "intro_widgets.h"
#include "intro_constants.h"

#include "ui/painter.h"
#include "ui/safe_url.h"

#include <QFontMetrics>
#include <QLineEdit>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDesktopServices>
#include <QUrl>
#include <QVBoxLayout>

namespace TeleMatrix {

namespace {

constexpr int kUsernameCheckDelayMs = 400;


// Filled action button painted with live intro:: colors. Preserves the exact
// normal / hover / disabled fills and the radius from the former stylesheet.
class IntroFilledButton : public QPushButton {
public:
    IntroFilledButton(
        const QColor *bg,
        const QColor *bgOver,
        const QColor *fg,
        const QColor *disabledBg,
        int radius,
        QWidget *parent)
        : QPushButton(parent)
        , _bg(bg)
        , _bgOver(bgOver)
        , _fg(fg)
        , _disabledBg(disabledBg)
        , _radius(radius) {
        setMouseTracking(true);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        const QColor *fill = _bg;
        if (!isEnabled() && _disabledBg) {
            fill = _disabledBg;
        } else if (_hovered && _bgOver) {
            fill = _bgOver;
        }
        p.setPen(Qt::NoPen);
        if (fill) {
            p.setBrush(*fill);
            if (_radius > 0) {
                p.drawRoundedRect(rect(), _radius, _radius);
            } else {
                p.fillRect(rect(), *fill);
            }
        }
        if (_fg) {
            p.setPen(*_fg);
            p.drawText(rect(), Qt::AlignCenter, text());
        }
    }
    void enterEvent(QEnterEvent *) override { _hovered = true; update(); }
    void leaveEvent(QEvent *) override { _hovered = false; update(); }

private:
    const QColor *_bg = nullptr;
    const QColor *_bgOver = nullptr;
    const QColor *_fg = nullptr;
    const QColor *_disabledBg = nullptr;
    int _radius = 0;
    bool _hovered = false;
};

using IntroLineEdit = intro::Field;

QPushButton *createIntroLink(QWidget *parent, const QString &text) {
    auto *link = new intro::LinkButton(text, parent);
    link->setCursor(Qt::PointingHandCursor);
    link->setFont(st::baseFont(13));
    link->adjustSize();
    return link;
}

// Position a link button inside a field's right edge and reserve enough right
// text-margin that the field text never runs under the button.
void placeFieldButton(QLineEdit *field, QPushButton *button) {
    button->adjustSize();
    const auto x = field->x() + field->width() - button->width() - 8;
    const auto y = field->y() + (field->height() - button->height()) / 2;
    button->move(x, y);
    field->setTextMargins(12, 0, button->width() + 12, 0);
}

} // namespace

IntroRegister::IntroRegister(QWidget *parent, ProtocolBridge *bridge)
    : IntroStep(parent, false /* hasCover */)
    , _bridge(bridge)
{
    setTitleText(tr("Create account"));
    setDescriptionText(QString());

    // Create input fields (same style as IntroLogin). Unlike sign-in, the
    // homeserver starts empty: registration on matrix.org is web-only, so a
    // prefill would just steer users into the delegated-auth notice.
    _homeserver = createField(tr("Homeserver"));

    _username = createField(tr("Username"));

    _password = createField(tr("Password"));
    _password->setEchoMode(QLineEdit::Password);

    _confirmPassword = createField(tr("Confirm Password"));
    _confirmPassword->setEchoMode(QLineEdit::Password);

    // Password visibility toggles.
    _passwordToggle = new intro::LinkButton(tr("Show"), this);
    _passwordToggle->setCursor(Qt::PointingHandCursor);
    _passwordToggle->setFont(st::baseFont(13));
    _passwordToggle->adjustSize();
    connect(_passwordToggle, &QPushButton::clicked,
            this, &IntroRegister::togglePasswordVisibility);

    _confirmPasswordToggle = new intro::LinkButton(tr("Show"), this);
    _confirmPasswordToggle->setCursor(Qt::PointingHandCursor);
    _confirmPasswordToggle->setFont(st::baseFont(13));
    _confirmPasswordToggle->adjustSize();
    connect(_confirmPasswordToggle, &QPushButton::clicked,
            this, &IntroRegister::toggleConfirmPasswordVisibility);

    _signInLink = createIntroLink(this, tr("Back to sign in"));
    connect(_signInLink, &QPushButton::clicked, this, [this]() {
        Q_EMIT goSignIn();
    });


    // "Change" action inside the homeserver field (past phase 1) — returns to
    // the homeserver step so the disabled homeserver can be edited again.
    _changeHomeserver = createIntroLink(this, tr("Change"));
    connect(_changeHomeserver, &QPushButton::clicked, this, [this]() {
        hideError();
        setPhase(Phase::Homeserver);
        _homeserver->setEnabled(true);
        _homeserver->setFocus();
    });
    _changeHomeserver->hide();

    // The delegated-auth notice embeds a clickable "website" link in the
    // error label (plain error strings stay plain — QLabel auto-detects).
    connect(errorLabel(), &QLabel::linkActivated, this, [](const QString &link) {
        OpenSafeExternalUrl(link);
    });

    // Username availability status label.
    _usernameStatus = new QLabel(this);
    _usernameStatus->setFont(st::baseFont(st::introDiscoveryFontSize));
    _usernameStatus->setFixedWidth(st::introCountryWidth);
    _usernameStatus->hide();

    // Delegated-auth notice (OIDC/MAS): text with a link to the server's signup
    // page. Shown only in the Delegated phase.
    _delegatedInfo = new QLabel(this);
    _delegatedInfo->setWordWrap(true);
    _delegatedInfo->setAlignment(Qt::AlignHCenter);
    _delegatedInfo->setTextFormat(Qt::RichText);
    _delegatedInfo->setFont(st::baseFont(14));
    _delegatedInfo->setFixedWidth(st::introStepWidth);
    _delegatedInfo->setOpenExternalLinks(false);
    {
        QPalette pal = _delegatedInfo->palette();
        pal.setColor(QPalette::WindowText, intro::titleFg);
        _delegatedInfo->setPalette(pal);
    }
    connect(_delegatedInfo, &QLabel::linkActivated, this, [](const QString &link) {
        OpenSafeExternalUrl(link);
    });
    _delegatedInfo->hide();

    // UIA container (hidden until challenge received).
    _uiaContainer = new QWidget(this);
    _uiaContainer->hide();

    auto *uiaLayout = new QVBoxLayout(_uiaContainer);
    uiaLayout->setContentsMargins(0, 0, 0, 0);
    uiaLayout->setSpacing(8);

    _uiaLabel = new QLabel(_uiaContainer);
    _uiaLabel->setWordWrap(true);
    _uiaLabel->setFont(st::baseFont(14));
    {
        QPalette pal = _uiaLabel->palette();
        pal.setColor(QPalette::WindowText, intro::titleFg);
        _uiaLabel->setPalette(pal);
    }
    uiaLayout->addWidget(_uiaLabel);

    _tokenField = new IntroLineEdit(_uiaContainer);
    _tokenField->setPlaceholderText(tr("Registration token"));
    _tokenField->setFixedSize(st::introCountryWidth, st::introCountryHeight);
    _tokenField->setFont(st::baseFont(16));
    _tokenField->setTextMargins(12, 0, 12, 0);
    _tokenField->hide();
    uiaLayout->addWidget(_tokenField);

    _uiaSubmitButton = new IntroFilledButton(
        &intro::bgActive,            // background
        &intro::buttonBgOver,        // hover background
        &intro::buttonFg,            // text
        &intro::buttonDisabledBg,    // disabled background
        st::introNextButtonRadius,
        _uiaContainer);
    _uiaSubmitButton->setText(tr("Continue"));
    _uiaSubmitButton->setFixedSize(st::introNextButtonWidth, st::introNextButtonHeight);
    _uiaSubmitButton->setCursor(Qt::PointingHandCursor);
    _uiaSubmitButton->setFont(st::baseFont(16));
    uiaLayout->addWidget(_uiaSubmitButton);

    // Tab order.
    setTabOrder(_homeserver, _username);
    setTabOrder(_username, _password);
    setTabOrder(_password, _confirmPassword);
    setTabOrder(_confirmPassword, nextButton());
    setTabOrder(nextButton(), _signInLink);

    // Enter registers as soon as every field has something in it, whichever field
    // the cursor is in; while the form is still incomplete it moves to the field
    // that is actually missing rather than blindly to the one below.
    const auto onReturn = [this] {
        // Homeserver phase: Enter is "Continue" (submit runs the classification).
        if (_phase != Phase::Credentials) {
            submit();
            return;
        }
        const auto homeserverMissing = _homeserver->text().trimmed().isEmpty();
        const auto usernameMissing = _username->text().trimmed().isEmpty();
        // Not trimmed: a password may legitimately be spaces.
        const auto passwordMissing = _password->text().isEmpty();
        const auto confirmMissing = _confirmPassword->text().isEmpty();
        if (!homeserverMissing && !usernameMissing
                && !passwordMissing && !confirmMissing) {
            submit();
        } else if (homeserverMissing) {
            _homeserver->setFocus();
        } else if (usernameMissing) {
            _username->setFocus();
        } else if (passwordMissing) {
            _password->setFocus();
        } else {
            _confirmPassword->setFocus();
        }
    };
    connect(_homeserver, &QLineEdit::returnPressed, this, onReturn);
    connect(_username, &QLineEdit::returnPressed, this, onReturn);
    connect(_password, &QLineEdit::returnPressed, this, onReturn);
    connect(_confirmPassword, &QLineEdit::returnPressed, this, onReturn);
    connect(_password, &QLineEdit::textChanged, this, &IntroRegister::updateSubmitEnabled);
    connect(_confirmPassword, &QLineEdit::textChanged, this, &IntroRegister::updateSubmitEnabled);

    // Editing the homeserver invalidates a completed classification: drop back
    // to the homeserver phase so the user must Continue again (the check runs on
    // submit, not while typing).
    connect(_homeserver, &QLineEdit::textChanged, this, [this]() {
        _discoveredUrl.clear();
        _delegatedAuthUrl.clear();
        _pendingClassifyRequestId = 0;
        if (_phase != Phase::Homeserver) {
            hideError();
            setPhase(Phase::Homeserver);
        } else {
            updateSubmitEnabled();
        }
    });

    // Username availability check (debounced).
    auto *usernameTimer = new QTimer(this);
    usernameTimer->setSingleShot(true);
    connect(_username, &QLineEdit::textChanged, this, [this, usernameTimer]() {
        _usernameStatus->hide();
        _lastCheckedUsername.clear(); // Fix #19: invalidate previous check scope.
        if (_usernameAvailabilityErrorVisible) {
            _usernameAvailabilityErrorVisible = false;
            hideError();
            updateFieldLayout();
        }
        updateSubmitEnabled();
        usernameTimer->start(kUsernameCheckDelayMs);
    });
    connect(usernameTimer, &QTimer::timeout, this, &IntroRegister::startUsernameCheck);

    // Bridge signal connections.
    connect(_bridge, &ProtocolBridge::registrationSuccess,
            this, &IntroRegister::onRegistrationSuccess);
    connect(_bridge, &ProtocolBridge::registrationChallenge,
            this, &IntroRegister::onRegistrationChallenge);
    connect(_bridge, &ProtocolBridge::registrationFailed,
            this, &IntroRegister::onRegistrationFailed);
    connect(_bridge, &ProtocolBridge::usernameAvailabilityChecked,
            this, &IntroRegister::onUsernameAvailability);
    connect(_bridge, &ProtocolBridge::registrationClassified,
            this, &IntroRegister::onRegistrationClassified);

    // Start on the homeserver-only phase.
    setPhase(Phase::Homeserver);
    updateSubmitEnabled();
}

QLineEdit *IntroRegister::createField(const QString &placeholder) {
    auto *field = new IntroLineEdit(this);
    field->setPlaceholderText(placeholder);
    field->setFixedSize(st::introCountryWidth, st::introCountryHeight);
    return field;
}


void IntroRegister::deactivate() {
    _active = false; // Fix #17: prevent stale completions from forcing navigation.
}

void IntroRegister::activate() {
    IntroStep::activate();
    _active = true; // Fix #17: mark step as visible.
    // IntroStep::activate() force-enables the shared Next button; re-gate it to
    // the current phase (e.g. disabled while the homeserver field is empty).
    updateSubmitEnabled();
    if (_uiaContainer->isVisible()) {
        if (_tokenField->isVisible()) {
            _tokenField->setFocus();
        }
    } else if (_phase == Phase::Credentials) {
        _username->setFocus();
    } else {
        _homeserver->setFocus();
    }
}

void IntroRegister::classifyServer() {
    const auto input = _homeserver->text().trimmed();
    if (input.isEmpty()) {
        return;
    }
    _checking = true;
    hideError();
    _homeserver->setEnabled(false);
    nextButton()->setEnabled(false);
    nextButton()->setText(tr("Checking…"));
    const auto requestId = _nextClassifyRequestId++;
    _pendingClassifyRequestId = requestId;
    _bridge->classifyRegistration(input, requestId);
}

void IntroRegister::onRegistrationClassified(
        quint64 requestId,
        int status,
        const QString &url) {
    if (requestId == 0 || requestId != _pendingClassifyRequestId) {
        return;
    }
    _pendingClassifyRequestId = 0;
    _checking = false;
    nextButton()->setText(nextButtonText());

    switch (status) {
    case 1: // Password registration available.
        _discoveredUrl = url;
        _delegatedAuthUrl.clear();
        hideError();
        setPhase(Phase::Credentials);
        _username->setFocus();
        break;
    case 2: // OIDC/MAS — accounts created on the server's website.
        _discoveredUrl.clear();
        _delegatedAuthUrl = url;
        setPhase(Phase::Delegated);
        break;
    default: // 0: not a Matrix server.
        _discoveredUrl.clear();
        _delegatedAuthUrl.clear();
        setPhase(Phase::Homeserver);
        showError(tr("This doesn't look like a Matrix server. Check the address and try again"));
        updateFieldLayout();
        _homeserver->setFocus();
        break;
    }
}

void IntroRegister::setPhase(Phase phase) {
    _phase = phase;

    const bool credentials = (phase == Phase::Credentials);
    const bool delegated = (phase == Phase::Delegated);

    // The homeserver is only editable while entering it; once classified it is
    // disabled (greyed) with a "Change" action inside it to come back and edit.
    _homeserver->setEnabled(phase == Phase::Homeserver);
    _changeHomeserver->setVisible(phase != Phase::Homeserver);

    // Credential fields only in the Credentials phase.
    _username->setVisible(credentials);
    _password->setVisible(credentials);
    _confirmPassword->setVisible(credentials);
    _passwordToggle->setVisible(credentials);
    _confirmPasswordToggle->setVisible(credentials);
    if (!credentials) {
        _usernameStatus->hide();
    }

    // The delegated notice + its own text.
    _delegatedInfo->setVisible(delegated);
    if (delegated) {
        if (_delegatedAuthUrl.isEmpty()) {
            _delegatedInfo->setText(
                tr("This homeserver creates accounts on its own website"));
        } else {
            _delegatedInfo->setText(
                tr("This homeserver creates accounts on its own website<br>"
                   "<a href=\"%1\" style=\"color:%2;text-decoration:none;\">Continue registration there</a>")
                    .arg(_delegatedAuthUrl.toHtmlEscaped(), intro::accentText.name()));
        }
    }

    // The next button is hidden in the Delegated phase — the link is the action.
    nextButton()->setVisible(!delegated);
    nextButton()->setText(nextButtonText());

    // "Forgot password" only makes sense once you're entering credentials.

    updateSubmitEnabled();
    updateFieldLayout();
}

void IntroRegister::updateSubmitEnabled() {
    if (_submitting || _checking) {
        return; // The button is intentionally disabled while busy.
    }
    const bool hasHomeserver = !_homeserver->text().trimmed().isEmpty();
    if (_phase == Phase::Homeserver) {
        nextButton()->setEnabled(hasHomeserver);
        return;
    }
    if (_phase == Phase::Delegated) {
        return; // No next button in this phase.
    }
    nextButton()->setEnabled(hasHomeserver
        && !_username->text().trimmed().isEmpty()
        && !_password->text().isEmpty()
        && !_confirmPassword->text().isEmpty());
}

void IntroRegister::startUsernameCheck() {
    const auto user = _username->text().trimmed();
    if (user.isEmpty()) {
        _usernameStatus->hide();
        _lastCheckedUsername.clear(); // Fix #19: reset scope.
        return;
    }

    auto homeserver = _homeserver->text().trimmed();
    if (!_discoveredUrl.isEmpty()) {
        homeserver = _discoveredUrl;
    } else if (!homeserver.startsWith(QStringLiteral("http"))) {
        homeserver = QStringLiteral("https://") + homeserver;
    }

    // Fix #19: record the username we are about to check so the result
    // callback can discard stale replies if the field changed in the meantime.
    _lastCheckedUsername = user;
    _bridge->checkUsernameAvailable(homeserver, user);
}

void IntroRegister::onUsernameAvailability(int status, [[maybe_unused]] const QString &message) {

    if (_submitting) {
        return;
    }

    // Fix #19: discard stale results — only update the label when the callback
    // still corresponds to the username currently in the field.
    const auto currentUser = _username->text().trimmed();
    if (currentUser != _lastCheckedUsername) {
        return;
    }

    switch (status) {
    case 0:
        _usernameStatus->hide();
        if (_usernameAvailabilityErrorVisible) {
            _usernameAvailabilityErrorVisible = false;
            hideError();
        }
        break;
    case 1:
        _usernameStatus->hide();
        _usernameAvailabilityErrorVisible = true;
        showError(tr("Username already taken"));
        break;
    case 2:
        _usernameStatus->hide();
        _usernameAvailabilityErrorVisible = true;
        showError(tr("Invalid username format"));
        break;
    default:
        // Error or unknown — don't block registration.
        _usernameStatus->hide();
        if (_usernameAvailabilityErrorVisible) {
            _usernameAvailabilityErrorVisible = false;
            hideError();
        }
        break;
    }
    updateFieldLayout();
}

void IntroRegister::togglePasswordVisibility() {
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

void IntroRegister::toggleConfirmPasswordVisibility() {
    if (_confirmPassword->echoMode() == QLineEdit::Password) {
        _confirmPassword->setEchoMode(QLineEdit::Normal);
        _confirmPasswordToggle->setText(tr("Hide"));
    } else {
        _confirmPassword->setEchoMode(QLineEdit::Password);
        _confirmPasswordToggle->setText(tr("Show"));
    }
    _confirmPasswordToggle->adjustSize();
    updateFieldLayout(); // "Show"/"Hide" widths differ — re-place + re-margin.
    _confirmPassword->setFocus();
}

void IntroRegister::submit() {
    if (_submitting || _checking) {
        return;
    }

    // Phase 1: "Continue" classifies the server, then advances to credentials,
    // shows the delegated notice, or errors out.
    if (_phase == Phase::Homeserver) {
        if (_homeserver->text().trimmed().isEmpty()) {
            showError(tr("Please enter a homeserver"));
            updateFieldLayout();
            _homeserver->setFocus();
            return;
        }
        classifyServer();
        return;
    }

    // Delegated (MAS/OIDC): no local registration — open the server's website.
    if (_phase == Phase::Delegated) {
        if (!_delegatedAuthUrl.isEmpty()) {
            OpenSafeExternalUrl(_delegatedAuthUrl);
        }
        return;
    }

    auto homeserver = _homeserver->text().trimmed();
    const auto user = _username->text().trimmed();
    const auto pass = _password->text();
    const auto confirmPass = _confirmPassword->text();

    if (homeserver.isEmpty()) {
        showError(tr("Please enter a homeserver"));
        updateFieldLayout();
        _homeserver->setFocus();
        return;
    }
    if (user.isEmpty()) {
        showError(tr("Please enter a username"));
        updateFieldLayout();
        _username->setFocus();
        return;
    }
    if (pass.isEmpty()) {
        showError(tr("Please enter a password"));
        updateFieldLayout();
        _password->setFocus();
        return;
    }
    if (pass != confirmPass) {
        showError(tr("Passwords do not match"));
        updateFieldLayout();
        _confirmPassword->setFocus();
        return;
    }

    // The secret backend must be ready before registration writes secrets.
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
    nextButton()->setText(tr("Creating account..."));

    if (!_discoveredUrl.isEmpty()) {
        homeserver = _discoveredUrl;
    } else if (!homeserver.startsWith(QStringLiteral("http"))) {
        homeserver = QStringLiteral("https://") + homeserver;
    }
    _currentHomeserver = homeserver;

    // Cache credentials for UIA continuation (don't re-read from UI later).
    _cachedUsername = user;
    _cachedPassword = pass;

    _bridge->registerAccount(homeserver, user, pass);
}

void IntroRegister::onRegistrationSuccess(
    const QString &userId,
    const QString &displayName,
    const QString &avatarUrl) {
    _submitting = false;
    setFieldsEnabled(true);
    nextButton()->setText(nextButtonText());
    hideError();
    hideUiaWidgets();

    // Clear sensitive fields.
    _password->clear();
    _confirmPassword->clear();
    _cachedUsername.clear();
    _cachedPassword.clear();

    // Fix #17: ignore late completions that arrive after the user backed out.
    if (!_active) {
        return;
    }

    Q_EMIT registerSuccess(userId, displayName, avatarUrl);
}

void IntroRegister::onRegistrationChallenge(const QString &challengeJson) {
    _submitting = false;
    setFieldsEnabled(true);
    if (_uiaContainer->isVisible()) {
        setUiaControlsEnabled(true);
    }
    nextButton()->setText(nextButtonText());

    // Fix #17: ignore late challenges that arrive after the user left this step.
    if (!_active) {
        return;
    }

    auto doc = QJsonDocument::fromJson(challengeJson.toUtf8());
    auto obj = doc.object();

    // Fix #20: surface embedded auth errors (e.g. invalid registration token).
    // The server can return a challenge that also contains errcode/error when a
    // previous UIA stage submission was rejected.  Show the error inline so the
    // user knows why the form was redisplayed.
    const auto errcode = obj.value(QStringLiteral("errcode")).toString();
    const auto errorMsg = obj.value(QStringLiteral("error")).toString();
    if (!errcode.isEmpty() || !errorMsg.isEmpty()) {
        const auto displayMsg = errorMsg.isEmpty() ? errcode : errorMsg;
        showError(displayMsg);
        updateFieldLayout();
        // Fall through — still process the challenge so the user can correct
        // their input and retry without losing the UIA session.
    }

    _uiaSession = obj.value(QStringLiteral("session")).toString();
    if (_uiaSession.isEmpty()) {
        showError(tr("Server returned invalid authentication challenge"));
        return;
    }
    auto flows = obj.value(QStringLiteral("flows")).toArray();
    auto completed = obj.value(QStringLiteral("completed")).toArray();
    auto params = obj.value(QStringLiteral("params")).toObject();

    // Build set of completed stages.
    QSet<QString> completedSet;
    for (const auto &s : completed) {
        completedSet.insert(s.toString());
    }

    // Stage selection: find best flow (fewest unsupported stages).
    static const QSet<QString> kSupportedStages = {
        QStringLiteral("m.login.terms"),
        QStringLiteral("m.login.registration_token"),
        QStringLiteral("org.matrix.msc3231.login.registration_token"),
        QStringLiteral("m.login.dummy"),
    };

    int bestScore = INT_MAX;
    QJsonArray bestFlow;
    for (const auto &flowVal : flows) {
        auto stages = flowVal.toArray();
        int unsupported = 0;
        int remaining = 0;
        for (const auto &stage : stages) {
            auto s = stage.toString();
            if (!completedSet.contains(s)) {
                remaining++;
                if (!kSupportedStages.contains(s)) {
                    unsupported++;
                }
            }
        }
        int score = unsupported * 1000 + remaining;
        if (score < bestScore) {
            bestScore = score;
            bestFlow = stages;
        }
    }

    // Find the next incomplete stage in the best flow.
    QString nextStage;
    for (const auto &stage : bestFlow) {
        auto s = stage.toString();
        if (!completedSet.contains(s)) {
            nextStage = s;
            break;
        }
    }

    if (nextStage.isEmpty()) {
        showError(tr("No more stages to complete, but registration did not succeed"));
        updateFieldLayout();
        return;
    }

    // Get params for this stage.
    QJsonObject stageParams;
    if (params.contains(nextStage)) {
        auto val = params.value(nextStage);
        if (val.isObject()) {
            stageParams = val.toObject();
        } else if (val.isString()) {
            auto parsed = QJsonDocument::fromJson(val.toString().toUtf8());
            stageParams = parsed.object();
        }
    }

    showUiaStage(nextStage, stageParams);
}

void IntroRegister::showUiaStage(const QString &stageType, const QJsonObject &params) {
    if (stageType == QStringLiteral("m.login.dummy")) {
        _uiaContainer->hide();
        setFieldsEnabled(false);
        nextButton()->setText(tr("Creating account..."));
        QJsonObject authObj;
        authObj.insert(QStringLiteral("type"), QStringLiteral("m.login.dummy"));
        submitUiaStage(QStringLiteral("m.login.dummy"),
            QString::fromUtf8(QJsonDocument(authObj).toJson(QJsonDocument::Compact)));
        updateFieldLayout();
        return;
    }

    hideUiaWidgets();

    // Hide form fields during UIA.
    _homeserver->hide();
    _username->hide();
    _password->hide();
    _confirmPassword->hide();
    _passwordToggle->hide();
    _confirmPasswordToggle->hide();
    _usernameStatus->hide();
    nextButton()->hide();

    _uiaContainer->show();

    if (stageType == QStringLiteral("m.login.terms")) {
        // Terms of service — show policy links and accept button.
        QString labelText = tr("Please accept the terms of service to continue");
        auto policies = params.value(QStringLiteral("policies")).toObject();
        if (!policies.isEmpty()) {
            QStringList policyLinks;
            for (auto it = policies.begin(); it != policies.end(); ++it) {
                auto policy = it.value().toObject();
                auto en = policy.value(QStringLiteral("en")).toObject();
                auto name = en.value(QStringLiteral("name")).toString();
                auto url = en.value(QStringLiteral("url")).toString();
                if (!name.isEmpty() && !url.isEmpty()) {
                    policyLinks.append(QStringLiteral("<a href=\"%1\">%2</a>").arg(url, name));
                }
            }
            if (!policyLinks.isEmpty()) {
                labelText = tr("By continuing, you agree to: %1")
                    .arg(policyLinks.join(QStringLiteral(", ")));
            }
        }
        _uiaLabel->setText(labelText);
        _uiaLabel->setTextFormat(Qt::RichText);
        _uiaLabel->setOpenExternalLinks(true);
        _uiaLabel->show();
        _tokenField->hide();
        _uiaSubmitButton->setText(tr("Accept"));
        _uiaSubmitButton->show();

        disconnect(_uiaSubmitButton, nullptr, nullptr, nullptr);
        connect(_uiaSubmitButton, &QPushButton::clicked, this, [this]() {
            QJsonObject authObj;
            authObj.insert(QStringLiteral("type"), QStringLiteral("m.login.terms"));
            submitUiaStage(QStringLiteral("m.login.terms"),
                QString::fromUtf8(QJsonDocument(authObj).toJson(QJsonDocument::Compact)));
        });
    } else if (stageType == QStringLiteral("m.login.registration_token")
               || stageType == QStringLiteral("org.matrix.msc3231.login.registration_token")) {
        // Registration token — show token input.
        _uiaLabel->setText(tr("Enter your registration token:"));
        _uiaLabel->setTextFormat(Qt::PlainText);
        _uiaLabel->show();
        _tokenField->clear();
        _tokenField->show();
        _uiaSubmitButton->setText(tr("Continue"));
        _uiaSubmitButton->show();

        disconnect(_uiaSubmitButton, nullptr, nullptr, nullptr);
        connect(_uiaSubmitButton, &QPushButton::clicked, this, [this, stageType]() {
            auto token = _tokenField->text().trimmed();
            if (token.isEmpty()) {
                showError(tr("Please enter a registration token"));
                updateFieldLayout();
                return;
            }
            QJsonObject authObj;
            authObj.insert(QStringLiteral("type"), stageType);
            authObj.insert(QStringLiteral("token"), token);
            submitUiaStage(stageType,
                QString::fromUtf8(QJsonDocument(authObj).toJson(QJsonDocument::Compact)));
        });
    } else {
        // Unsupported stage.
        _uiaLabel->setText(tr("This server requires an unsupported verification step: %1").arg(stageType));
        _uiaLabel->setTextFormat(Qt::PlainText);
        _uiaLabel->show();
        _tokenField->hide();
        _uiaSubmitButton->hide();

        showError(tr("Registration cannot be completed — unsupported UIA stage"));
        updateFieldLayout();
    }

    setUiaControlsEnabled(true);
    updateFieldLayout();
}

void IntroRegister::submitUiaStage(
    [[maybe_unused]] const QString &stageType,
    const QString &authJson) {
    _submitting = true;
    setUiaControlsEnabled(false);

    _bridge->registerAccount(
        _currentHomeserver,
        _cachedUsername,
        _cachedPassword,
        _uiaSession,
        authJson);
}

void IntroRegister::setUiaControlsEnabled(bool enabled) {
    if (_tokenField) {
        _tokenField->setEnabled(enabled);
    }
    if (_uiaSubmitButton) {
        _uiaSubmitButton->setEnabled(enabled);
    }
    if (_uiaLabel) {
        _uiaLabel->setOpenExternalLinks(enabled);
        _uiaLabel->setTextInteractionFlags(enabled
            ? (Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard)
            : Qt::NoTextInteraction);
    }
    _signInLink->setEnabled(enabled);
}

void IntroRegister::hideUiaWidgets() {
    setUiaControlsEnabled(true);
    _uiaContainer->hide();
    _homeserver->show();
    _username->show();
    _password->show();
    _confirmPassword->show();
    _passwordToggle->show();
    _confirmPasswordToggle->show();
    nextButton()->show();
}

void IntroRegister::onRegistrationFailed(const QString &error) {
    _submitting = false;
    setFieldsEnabled(true);
    if (_uiaContainer->isVisible()) {
        setUiaControlsEnabled(true);
    }
    nextButton()->setText(nextButtonText());

    // Map known Matrix error codes.
    QString msg = error;
    if (error.contains(QStringLiteral("M_USER_IN_USE"))) {
        msg = tr("That username is already taken");
    } else if (error.contains(QStringLiteral("M_INVALID_USERNAME"))) {
        msg = tr("Invalid username format");
    } else if (error.contains(QStringLiteral("M_FORBIDDEN"))) {
        // Classification said password registration was possible, but the server
        // refused it — surface a clear message (it may have registration off).
        msg = tr("Registration is disabled on this homeserver. It may require creating the account on its website");
    } else if (error.contains(QStringLiteral("M_THREEPID_IN_USE"))) {
        msg = tr("That email or phone number is already in use");
    } else {
        msg = friendlyError(msg, _homeserver->text().trimmed());
    }

    showError(msg.isEmpty() ? tr("Couldn't create your account.") : msg);
    updateFieldLayout();
}

void IntroRegister::setFieldsEnabled(bool enabled) {
    const auto readOnly = !enabled;
    _homeserver->setReadOnly(readOnly);
    _username->setReadOnly(readOnly);
    _password->setReadOnly(readOnly);
    _confirmPassword->setReadOnly(readOnly);
    _passwordToggle->setEnabled(enabled);
    _confirmPasswordToggle->setEnabled(enabled);
    _signInLink->setEnabled(enabled);
    if (enabled) {
        updateSubmitEnabled();
    } else {
        nextButton()->setEnabled(false);
    }
}

QString IntroRegister::nextButtonText() const {
    return (_phase == Phase::Homeserver)
        ? tr("Continue")
        : tr("Create Account");
}

void IntroRegister::resizeEvent(QResizeEvent *e) {
    IntroStep::resizeEvent(e);
    updateFieldLayout();
}

void IntroRegister::updateFieldLayout() {
    const auto left = contentLeft();
    const auto top = contentTop();
    const auto fieldLeft = left + (st::introStepWidth - st::introCountryWidth) / 2;

    // Position the title/description here too (not only in the base layout): a
    // phase switch re-lays-out via this method WITHOUT a resize, so relying on
    // the base pass would leave the title stranded at its previous position.
    titleLabel()->move(left, top + st::introTitleTop);
    titleLabel()->setFixedWidth(st::introStepWidth);
    descriptionLabel()->setFixedWidth(st::introStepWidth);
    descriptionLabel()->move(left, top + st::introDescriptionTop);

    if (_uiaContainer->isVisible()) {
        // UIA mode — center the UIA container.
        int y = contentStartTop();
        _uiaContainer->setFixedWidth(st::introCountryWidth);
        _uiaContainer->move(fieldLeft, y);
        _uiaContainer->adjustSize();

        const auto signInY = y + _uiaContainer->height() + st::introLinkTop;
        const auto signInLeft = (width() - _signInLink->width()) / 2;
        _signInLink->move(signInLeft, signInY);


        placeErrorAbove(y);
        return;
    }

    int y = contentStartTop();
    placeErrorAbove(y);
    _homeserver->move(fieldLeft, y);
    if (_changeHomeserver->isVisible()) {
        placeFieldButton(_homeserver, _changeHomeserver);
    } else {
        _homeserver->setTextMargins(12, 0, 12, 0);
    }
    y += st::introCountryHeight;

    if (_phase == Phase::Homeserver) {
        // Only the homeserver field + "Continue"; then the "Sign in" link.
        const auto nextLeft = (width() - st::introNextButtonWidth) / 2;
        const auto nextY = y + st::introFieldsToButton;
        nextButton()->move(nextLeft, nextY);

        const auto signInY = nextY + st::introNextButtonHeight + st::introLinkTop;
        _signInLink->move((width() - _signInLink->width()) / 2, signInY);

        centerContentVertically();
        return;
    }

    if (_phase == Phase::Delegated) {
        // The notice goes ABOVE the field, in the slot the error would use. It
        // is a verdict on the homeserver just entered, so it belongs next to
        // that field rather than as a footnote under the form — and it is what
        // every other message on these screens does. The two never collide: a
        // classification failure returns to the Homeserver phase instead.
        const auto infoHeight = qMax(
            _delegatedInfo->heightForWidth(st::introStepWidth),
            _delegatedInfo->sizeHint().height());
        _delegatedInfo->setFixedHeight(infoHeight);
        const auto fieldTop = y - st::introCountryHeight;
        _delegatedInfo->move(
            left, fieldTop - infoHeight - st::introFieldSpacing);

        const auto signInY = y + st::introFieldsToButton;
        _signInLink->move((width() - _signInLink->width()) / 2, signInY);

        centerContentVertically();
        return;
    }

    // Phase::Credentials — the full form.
    y += st::introFieldSpacing;
    _username->move(fieldLeft, y);
    y += st::introCountryHeight;

    // Username status below username field.
    if (_usernameStatus->isVisible()) {
        _usernameStatus->move(fieldLeft, y + 2);
        y += _usernameStatus->sizeHint().height() + 4;
    } else {
        y += st::introFieldSpacing;
    }

    _password->move(fieldLeft, y);
    placeFieldButton(_password, _passwordToggle);

    y += st::introCountryHeight + st::introFieldSpacing;
    _confirmPassword->move(fieldLeft, y);
    placeFieldButton(_confirmPassword, _confirmPasswordToggle);

    // Next button below confirm password.
    const auto fieldsBottom = y + st::introCountryHeight;
    const auto nextLeft = (width() - st::introNextButtonWidth) / 2;
    const auto nextY = fieldsBottom + st::introFieldsToButton;
    nextButton()->move(nextLeft, nextY);

    const auto signInY = nextY + st::introNextButtonHeight + st::introLinkTop;
    const auto signInLeft = (width() - _signInLink->width()) / 2;
    _signInLink->move(signInLeft, signInY);


    centerContentVertically();
}

} // namespace TeleMatrix

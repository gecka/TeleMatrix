// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "intro_forgot_password.h"
#include "intro_widget.h"

#include "../protocol/protocol_bridge.h"
#include "../styles/style_constants.h"
#include "intro_colors.h"
#include "intro_widgets.h"
#include "intro_constants.h"

#include "ui/painter.h"
#include "ui/safe_url.h"

#include <QCoreApplication>
#include <QFontMetrics>
#include <QLineEdit>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QResizeEvent>

namespace TeleMatrix {

namespace {

constexpr int kStripHeight = 46;
constexpr int kStripPadH = 16;
constexpr int kDotSize = 9;
constexpr int kDotGap = 12;
// Gap between the last field and the "sent" strip (redesign: 24px).
constexpr int kFieldsToStrip = 24;


using IntroLineEdit = intro::Field;

QPushButton *createIntroLink(QWidget *parent, const QString &text) {
    auto *link = new intro::LinkButton(text, parent);
    link->setCursor(Qt::PointingHandCursor);
    link->setFont(st::baseFont(13));
    link->adjustSize();
    return link;
}


// The redesign's "sent" strip: a bordered translucent row with a status dot,
// "Sent to <address>", and a Resend link. Replaces the button once the reset
// mail is on its way.
class SentStrip : public QWidget {
public:
    explicit SentStrip(QWidget *parent) : QWidget(parent) {
        setFixedHeight(kStripHeight);
        _resend = new intro::LinkButton(
            QCoreApplication::translate("TeleMatrix::IntroForgotPassword", "Resend"),
            this);
        _resend->adjustSize();
    }

    void setAddress(const QString &address) {
        _address = address;
        update();
    }

    [[nodiscard]] QPushButton *resendButton() const { return _resend; }

protected:
    void resizeEvent(QResizeEvent *) override {
        _resend->move(
            width() - kStripPadH - _resend->width(),
            (height() - _resend->height()) / 2);
    }

    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(intro::surfaceFill);
        p.drawRoundedRect(QRectF(rect()), intro::metrics::cardRadius,
            intro::metrics::cardRadius);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(intro::surfaceBorder, 1.0));
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
            intro::metrics::cardRadius, intro::metrics::cardRadius);

        const auto dotY = (height() - kDotSize) / 2;
        p.setPen(Qt::NoPen);
        p.setBrush(intro::accentFill);
        p.drawEllipse(QRect(kStripPadH, dotY, kDotSize, kDotSize));

        const auto textLeft = kStripPadH + kDotSize + kDotGap;
        const auto textRight = _resend->x() - kDotGap;
        p.setFont(st::baseFont(intro::metrics::smallSize + 1));
        p.setPen(intro::inkHeading);
        const QFontMetrics fm(p.font());
        p.drawText(
            QRect(textLeft, 0, qMax(0, textRight - textLeft), height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            fm.elidedText(QCoreApplication::translate(
                    "TeleMatrix::IntroForgotPassword", "Sent to %1").arg(_address),
                Qt::ElideMiddle,
                qMax(0, textRight - textLeft)));
    }

private:
    QString _address;
    QPushButton *_resend = nullptr;
};

} // namespace

IntroForgotPassword::IntroForgotPassword(IntroWidget *parent, ProtocolBridge *bridge)
    : IntroStep(parent, false /* hasCover */)
    , _bridge(bridge)
{
    setTitleText(tr("Reset password"));
    setDescriptionText(QString());

    // Create input fields (same style as IntroLogin).
    _homeserverField = createField(tr("Homeserver"));

    _emailField = createField(tr("Email on your account"));

    _newPasswordField = createField(tr("New password"));
    _newPasswordField->setEchoMode(QLineEdit::Password);
    _newPasswordField->hide();

    _confirmPasswordField = createField(tr("Confirm new password"));
    _confirmPasswordField->setEchoMode(QLineEdit::Password);
    _confirmPasswordField->hide();

    // Password visibility toggles (hidden until password state).
    _passwordToggle = new intro::LinkButton(tr("Show"), this);
    _passwordToggle->setCursor(Qt::PointingHandCursor);
    _passwordToggle->setFont(st::baseFont(13));
    _passwordToggle->adjustSize();
    _passwordToggle->hide();
    connect(_passwordToggle, &QPushButton::clicked,
            this, &IntroForgotPassword::togglePasswordVisibility);

    _confirmPasswordToggle = new intro::LinkButton(tr("Show"), this);
    _confirmPasswordToggle->setCursor(Qt::PointingHandCursor);
    _confirmPasswordToggle->setFont(st::baseFont(13));
    _confirmPasswordToggle->adjustSize();
    _confirmPasswordToggle->hide();
    connect(_confirmPasswordToggle, &QPushButton::clicked,
            this, &IntroForgotPassword::toggleConfirmPasswordVisibility);

    // Instruction label (shown in WaitingForEmail state).
    _instructionLabel = new QLabel(this);
    _instructionLabel->setWordWrap(true);
    _instructionLabel->setFont(st::baseFont(14));
    {
        QPalette pal = _instructionLabel->palette();
        pal.setColor(QPalette::WindowText, intro::titleFg);
        _instructionLabel->setPalette(pal);
    }
    _instructionLabel->setFixedWidth(st::introCountryWidth);
    _instructionLabel->setAlignment(Qt::AlignCenter);
    _instructionLabel->hide();

    // "I've verified my email" button (shown in WaitingForEmail state).
    _emailConfirmedButton = new intro::FilledButton(
        tr("I've verified my email"), this);
    _emailConfirmedButton->setFixedSize(st::introNextButtonWidth, st::introNextButtonHeight);
    _emailConfirmedButton->setFont(st::baseFont(16));
    _emailConfirmedButton->hide();
    connect(_emailConfirmedButton, &QPushButton::clicked, this, [this]() {
        setState(State::EnterNewPassword);
    });

    _sentStrip = new SentStrip(this);
    _sentStrip->hide();
    connect(static_cast<SentStrip *>(_sentStrip)->resendButton(),
            &QPushButton::clicked, this, [this] {
        // Re-send to the same address: the fields are still on screen and
        // unchanged, so this is the same request the button made.
        submit();
    });

    _signInLink = createIntroLink(this, tr("Back to sign in"));
    connect(_signInLink, &QPushButton::clicked, this, [this]() {
        Q_EMIT goSignIn();
    });


    // Tab order for email state.
    setTabOrder(_homeserverField, _emailField);
    setTabOrder(_emailField, nextButton());
    setTabOrder(nextButton(), _signInLink);

    connect(_homeserverField, &QLineEdit::returnPressed, [this]() {
        _emailField->setFocus();
    });
    connect(_emailField, &QLineEdit::returnPressed, this, &IntroForgotPassword::submit);
    connect(_newPasswordField, &QLineEdit::returnPressed, [this]() {
        _confirmPasswordField->setFocus();
    });
    connect(_confirmPasswordField, &QLineEdit::returnPressed, this, &IntroForgotPassword::submit);

    // Bridge signal connections.
    connect(_bridge, &ProtocolBridge::passwordResetTokenSent,
            this, &IntroForgotPassword::onPasswordResetTokenSent);
    connect(_bridge, &ProtocolBridge::passwordResetComplete,
            this, &IntroForgotPassword::onPasswordResetComplete);
    connect(_bridge, &ProtocolBridge::passwordResetPageProbed,
            this, &IntroForgotPassword::onPasswordResetPageProbed);

    // The delegated-reset notice embeds a link in the error label; plain error
    // strings stay plain, since QLabel auto-detects rich text.
    connect(errorLabel(), &QLabel::linkActivated, this, [](const QString &link) {
        OpenSafeExternalUrl(link);
    });

    connect(_homeserverField, &QLineEdit::textChanged,
            this, &IntroForgotPassword::updateSubmitEnabled);

    connect(_emailField, &QLineEdit::textChanged,
            this, &IntroForgotPassword::updateSubmitEnabled);
    connect(_newPasswordField, &QLineEdit::textChanged,
            this, &IntroForgotPassword::updateSubmitEnabled);
    connect(_confirmPasswordField, &QLineEdit::textChanged,
            this, &IntroForgotPassword::updateSubmitEnabled);
    updateSubmitEnabled();
}

QLineEdit *IntroForgotPassword::createField(const QString &placeholder) {
    auto *field = new IntroLineEdit(this);
    field->setPlaceholderText(placeholder);
    field->setFixedSize(st::introCountryWidth, st::introCountryHeight);
    return field;
}


void IntroForgotPassword::activate() {
    IntroStep::activate();
    // IntroStep::activate() force-enables the shared Next button; re-gate it to
    // the current state so it stays disabled until the fields are filled.
    updateSubmitEnabled();
    switch (_state) {
    case State::EnterEmail:
        _emailField->setFocus();
        break;
    case State::WaitingForEmail:
        _emailConfirmedButton->setFocus();
        break;
    case State::EnterNewPassword:
        _newPasswordField->setFocus();
        break;
    }
}

void IntroForgotPassword::setState(State state) {
    _state = state;

    switch (state) {
    case State::EnterEmail:
        _sentStrip->hide();
        setTitleText(tr("Reset password"));
        setDescriptionText(QString());
        _homeserverField->show();
        _emailField->show();
        _newPasswordField->hide();
        _confirmPasswordField->hide();
        _passwordToggle->hide();
        _confirmPasswordToggle->hide();
        _instructionLabel->hide();
        _emailConfirmedButton->hide();
        nextButton()->show();
        nextButton()->setText(nextButtonText());
        _emailField->setFocus();
        break;

    case State::WaitingForEmail:
        // The redesign keeps the form on screen and swaps the button for a
        // bordered "Sent to <address>" strip; the confirm button stays because
        // this flow still needs a way to move on to setting the new password.
        setTitleText(tr("Reset password"));
        setDescriptionText(QString());
        _homeserverField->show();
        _emailField->show();
        _newPasswordField->hide();
        _confirmPasswordField->hide();
        _passwordToggle->hide();
        _confirmPasswordToggle->hide();
        _instructionLabel->hide();
        static_cast<SentStrip *>(_sentStrip)->setAddress(_emailField->text().trimmed());
        _sentStrip->show();
        _emailConfirmedButton->show();
        nextButton()->hide();
        _emailConfirmedButton->setFocus();
        break;

    case State::EnterNewPassword:
        _sentStrip->hide();
        setTitleText(tr("Set new password"));
        setDescriptionText(tr("Choose a new password for your account"));
        _homeserverField->hide();
        _emailField->hide();
        _newPasswordField->show();
        _newPasswordField->clear();
        _confirmPasswordField->show();
        _confirmPasswordField->clear();
        _passwordToggle->show();
        _confirmPasswordToggle->show();
        _instructionLabel->hide();
        _emailConfirmedButton->hide();
        nextButton()->show();
        nextButton()->setText(tr("Reset Password"));
        // Update tab order for password fields.
        setTabOrder(_newPasswordField, _confirmPasswordField);
        setTabOrder(_confirmPasswordField, nextButton());
        _newPasswordField->setFocus();
        break;
    }

    hideError();
    updateSubmitEnabled();
    updateFieldLayout();
}

void IntroForgotPassword::submit() {
    if (_submitting) {
        return;
    }

    if (_state == State::EnterEmail) {
        auto homeserver = _homeserverField->text().trimmed();
        const auto email = _emailField->text().trimmed();

        if (homeserver.isEmpty()) {
            showError(tr("Please enter a homeserver"));
            updateFieldLayout();
            _homeserverField->setFocus();
            return;
        }
        if (email.isEmpty()) {
            showError(tr("Please enter your email address"));
            updateFieldLayout();
            _emailField->setFocus();
            return;
        }

        hideError();
        _submitting = true;
        setFieldsEnabled(false);
        nextButton()->setText(tr("Sending..."));

        if (!homeserver.startsWith(QStringLiteral("http"))) {
            homeserver = QStringLiteral("https://") + homeserver;
        }
        _homeserver = homeserver;

        _bridge->requestPasswordReset(homeserver, email);

    } else if (_state == State::EnterNewPassword) {
        const auto newPass = _newPasswordField->text();
        const auto confirmPass = _confirmPasswordField->text();

        if (newPass.isEmpty()) {
            showError(tr("Please enter a new password"));
            updateFieldLayout();
            _newPasswordField->setFocus();
            return;
        }
        if (newPass != confirmPass) {
            showError(tr("Passwords do not match"));
            updateFieldLayout();
            _confirmPasswordField->setFocus();
            return;
        }

        hideError();
        _submitting = true;
        setFieldsEnabled(false);
        nextButton()->setText(tr("Resetting..."));

        _bridge->resetPassword(_homeserver, newPass, _sid, _clientSecret);
    }
}

void IntroForgotPassword::onPasswordResetTokenSent(
    bool success,
    const QString &sid,
    const QString &clientSecret,
    const QString &error)
{
    _submitting = false;
    setFieldsEnabled(true);
    nextButton()->setText(nextButtonText());

    if (success) {
        _sid = sid;
        _clientSecret = clientSecret;
        setState(State::WaitingForEmail);
    } else {
        QString msg = error;
        if (msg.contains(QStringLiteral("M_THREEPID_NOT_FOUND"))) {
            msg = tr("No account is linked to that email address");
        } else if (msg.contains(QStringLiteral("M_SERVER_NOT_TRUSTED"))) {
            msg = tr("The identity server is not trusted by the homeserver");
        } else if (msg.contains(QStringLiteral("M_UNRECOGNIZED"))
                || msg.contains(QStringLiteral("M_FORBIDDEN"))) {
            // OIDC/MAS homeservers (e.g. matrix.org) disable the legacy
            // reset endpoint; the server literally answers "Unrecognized
            // request".
            showDelegatedResetNotice();
            updateFieldLayout();
            return;
        } else {
            msg = friendlyError(msg, _homeserverField->text().trimmed());
        }
        showError(msg.isEmpty() ? tr("Couldn't send the reset email.") : msg);
        updateFieldLayout();
    }
}

void IntroForgotPassword::showDelegatedResetNotice() {
    // Shown immediately, without a link: the page has to be discovered over the
    // network and the user should not wait to learn what happened. The probe
    // below upgrades this same message in place once it knows where to point.
    showError(tr("This homeserver doesn't support password reset from the app. "
                 "Reset your password on its website"));

    if (!_bridge || _homeserver.isEmpty()) {
        return;
    }
    _bridge->probePasswordResetPage(_homeserver, ++_resetPageRequestId);
}

void IntroForgotPassword::onPasswordResetPageProbed(
        quint64 requestId,
        bool available,
        const QString &url) {
    // Ignore a stale answer: the user may have edited the homeserver and asked
    // again while this was in flight.
    if (requestId != _resetPageRequestId || !available || url.isEmpty()) {
        return;
    }
    // Only upgrade the notice if it is still the thing on screen — an error from
    // a later attempt must not be silently replaced.
    if (!errorLabel() || !errorLabel()->isVisible()) {
        return;
    }
    showError(tr("This homeserver doesn't support password reset from the app.<br>"
                 "<a href=\"%1\" style=\"color:%2;text-decoration:none;\">Reset your password on its website</a>")
                  .arg(url.toHtmlEscaped(), intro::accentText.name()));
    updateFieldLayout();
}

void IntroForgotPassword::onPasswordResetComplete(bool success, const QString &error) {
    _submitting = false;
    setFieldsEnabled(true);

    if (success) {
        hideError();
        // Clear sensitive fields.
        _newPasswordField->clear();
        _confirmPasswordField->clear();
        _sid.clear();
        _clientSecret.clear();
        Q_EMIT passwordResetSuccess();
    } else {
        nextButton()->setText(tr("Reset Password"));
        QString msg = error;
        if (msg.contains(QStringLiteral("M_UNAUTHORIZED"))) {
            msg = tr("Email verification not completed. Please check your email and try again");
        } else if (msg.contains(QStringLiteral("M_UNRECOGNIZED"))
                || msg.contains(QStringLiteral("M_FORBIDDEN"))) {
            showDelegatedResetNotice();
            updateFieldLayout();
            return;
        } else {
            msg = friendlyError(msg, _homeserver);
        }
        showError(msg.isEmpty() ? tr("Couldn't reset your password.") : msg);
        updateFieldLayout();
    }
}

void IntroForgotPassword::setFieldsEnabled(bool enabled) {
    _homeserverField->setEnabled(enabled);
    _emailField->setEnabled(enabled);
    _newPasswordField->setEnabled(enabled);
    _confirmPasswordField->setEnabled(enabled);
    _passwordToggle->setEnabled(enabled);
    _confirmPasswordToggle->setEnabled(enabled);
    _emailConfirmedButton->setEnabled(enabled);
    _signInLink->setEnabled(enabled);
    if (enabled) {
        updateSubmitEnabled();
    } else {
        nextButton()->setEnabled(false);
    }
}

void IntroForgotPassword::updateSubmitEnabled() {
    if (_submitting) {
        return; // setFieldsEnabled(false) already disabled the button.
    }
    if (_state == State::EnterEmail) {
        nextButton()->setEnabled(!_homeserverField->text().trimmed().isEmpty()
            && !_emailField->text().trimmed().isEmpty());
    } else if (_state == State::EnterNewPassword) {
        nextButton()->setEnabled(!_newPasswordField->text().isEmpty()
            && !_confirmPasswordField->text().isEmpty());
    } else {
        nextButton()->setEnabled(true);
    }
}

void IntroForgotPassword::togglePasswordVisibility() {
    if (_newPasswordField->echoMode() == QLineEdit::Password) {
        _newPasswordField->setEchoMode(QLineEdit::Normal);
        _passwordToggle->setText(tr("Hide"));
    } else {
        _newPasswordField->setEchoMode(QLineEdit::Password);
        _passwordToggle->setText(tr("Show"));
    }
    _passwordToggle->adjustSize();
    _newPasswordField->setFocus();
}

void IntroForgotPassword::toggleConfirmPasswordVisibility() {
    if (_confirmPasswordField->echoMode() == QLineEdit::Password) {
        _confirmPasswordField->setEchoMode(QLineEdit::Normal);
        _confirmPasswordToggle->setText(tr("Hide"));
    } else {
        _confirmPasswordField->setEchoMode(QLineEdit::Password);
        _confirmPasswordToggle->setText(tr("Show"));
    }
    _confirmPasswordToggle->adjustSize();
    _confirmPasswordField->setFocus();
}

QString IntroForgotPassword::nextButtonText() const {
    if (_state == State::EnterNewPassword) {
        return tr("Reset password");
    }
    return tr("Send reset link");
}

void IntroForgotPassword::resizeEvent(QResizeEvent *e) {
    IntroStep::resizeEvent(e);
    updateFieldLayout();
}

void IntroForgotPassword::updateFieldLayout() {
    const auto left = contentLeft();
    const auto top = contentTop();
    const auto fieldLeft = left + (st::introStepWidth - st::introCountryWidth) / 2;

    // All states start their first control at the same y; errors go on top.
    placeErrorAbove(top + st::introStepFieldTop);

    if (_state == State::WaitingForEmail) {
        // Same form as the entry state, with the strip standing where the
        // button was and the confirm button under it.
        int y = contentStartTop();
        _homeserverField->move(fieldLeft, y);
        y += st::introCountryHeight + st::introFieldSpacing;
        _emailField->move(fieldLeft, y);
        y += st::introCountryHeight + kFieldsToStrip;

        _sentStrip->setGeometry(
            fieldLeft, y, st::introCountryWidth, _sentStrip->height());
        y += _sentStrip->height() + st::introFieldSpacing;

        const auto btnLeft = (width() - st::introNextButtonWidth) / 2;
        _emailConfirmedButton->move(btnLeft, y);

        const auto signInY = y + st::introNextButtonHeight + st::introLinkTop;
        const auto signInLeft = (width() - _signInLink->width()) / 2;
        _signInLink->move(signInLeft, signInY);

        centerContentVertically();
        return;
    }

    if (_state == State::EnterNewPassword) {
        // Password fields.
        int y = contentStartTop();
        _newPasswordField->move(fieldLeft, y);
        const auto pwToggleX = fieldLeft + st::introCountryWidth - _passwordToggle->width() - 8;
        const auto pwToggleY = y + (st::introCountryHeight - _passwordToggle->height()) / 2;
        _passwordToggle->move(pwToggleX, pwToggleY);
        _newPasswordField->setTextMargins(12, 0, _passwordToggle->width() + 12, 0);

        y += st::introCountryHeight + st::introFieldSpacing;
        _confirmPasswordField->move(fieldLeft, y);
        const auto cpToggleX = fieldLeft + st::introCountryWidth - _confirmPasswordToggle->width() - 8;
        const auto cpToggleY = y + (st::introCountryHeight - _confirmPasswordToggle->height()) / 2;
        _confirmPasswordToggle->move(cpToggleX, cpToggleY);
        _confirmPasswordField->setTextMargins(12, 0, _confirmPasswordToggle->width() + 12, 0);

        const auto fieldsBottom = y + st::introCountryHeight;
        const auto nextLeft = (width() - st::introNextButtonWidth) / 2;
        const auto nextY = fieldsBottom + st::introFieldsToButton;
        nextButton()->move(nextLeft, nextY);

        const auto signInY = nextY + st::introNextButtonHeight + st::introLinkTop;
        const auto signInLeft = (width() - _signInLink->width()) / 2;
        _signInLink->move(signInLeft, signInY);

        return;
    }

    // State::EnterEmail — homeserver + email fields.
    int y = contentStartTop();
    _homeserverField->move(fieldLeft, y);
    y += st::introCountryHeight + st::introFieldSpacing;
    _emailField->move(fieldLeft, y);

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

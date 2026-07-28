// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "intro_verify_choice.h"
#include "intro_widget.h"

#include "../protocol/protocol_bridge.h"
#include "../styles/style_constants.h"
#include "intro_colors.h"
#include "intro_widgets.h"

#include "ui/painter.h"

#include <QDebug>
#include <QFontMetrics>
#include <QLabel>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimer>

namespace TeleMatrix {

namespace {

// Gap between cards.
constexpr int kCardGap = 8;

// Selectable verification card painted with live intro:: colors instead of a
// frozen stylesheet, so it tracks theme changes. Draws its rounded background
// + border (hover-highlighted), then the title / subtitle text on top. The
// disabled state dims the whole card to 50% as the former QSS "opacity: 0.5".
class IntroCard : public QPushButton {
public:
    IntroCard(const QString &title, const QString &subtitle, QWidget *parent)
        : QPushButton(parent)
        , _title(title)
        , _subtitle(subtitle) {
        setMouseTracking(true);
        // Keep an accessible text label even though painting is custom.
        setText(title + QStringLiteral("\n") + subtitle);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);

        if (!isEnabled()) {
            p.setOpacity(0.5);
        }

        // Rounded background + border.
        p.setPen(QPen(intro::inputBorder, 1));
        const bool hovered = isEnabled() && underMouse();
        p.setBrush(hovered ? intro::bgOver : intro::bg);
        const auto r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.drawRoundedRect(r, st::introVerifyCardRadius, st::introVerifyCardRadius);

        // Title + subtitle.
        const auto titleFont = st::baseFont(st::introVerifyCardTitleSize, true);
        const auto subtitleFont = st::baseFont(st::introVerifyCardSubSize);
        const auto titleHeight = QFontMetrics(titleFont).height();
        const auto subtitleHeight = QFontMetrics(subtitleFont).height();
        const auto blockHeight =
            titleHeight + st::introVerifyCardTextGap + subtitleHeight;
        const auto blockTop = (height() - blockHeight) / 2;

        p.setFont(titleFont);
        p.setPen(intro::titleFg);
        p.drawText(
            QRect(0, blockTop, width(), titleHeight),
            Qt::AlignHCenter | Qt::AlignVCenter,
            _title);

        p.setFont(subtitleFont);
        p.setPen(intro::subtextFg);
        p.drawText(
            QRect(0, blockTop + titleHeight + st::introVerifyCardTextGap,
                  width(), subtitleHeight),
            Qt::AlignHCenter | Qt::AlignVCenter,
            _subtitle);
    }
    void enterEvent(QEnterEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }

private:
    QString _title;
    QString _subtitle;
};


} // namespace

IntroVerifyChoice::IntroVerifyChoice(QWidget *parent, ProtocolBridge *bridge)
    : IntroStep(parent, false /* hasCover */)
    , _bridge(bridge)
{
    setTitleText(tr("Verify this session"));
    setDescriptionText(QString());

    // Helper to create a card button.
    auto createCard = [this](const QString &title, const QString &subtitle) {
        auto *card = new IntroCard(title, subtitle, this);
        card->setFixedSize(st::introVerifyCardWidth, st::introVerifyCardHeight);
        card->setCursor(Qt::PointingHandCursor);
        return card;
    };

    _qrCard = createCard(
        tr("Scan QR code"),
        tr("Point another signed-in session at this one"));
    connect(_qrCard, &QPushButton::clicked,
            this, &IntroVerifyChoice::qrChosen);

    _emojiCard = createCard(
        tr("Compare emoji"),
        tr("Check the same seven emoji on both"));
    connect(_emojiCard, &QPushButton::clicked,
            this, &IntroVerifyChoice::emojiChosen);

    _recoveryCard = createCard(
        tr("Enter recovery key"),
        tr("Use your security key or phrase"));
    connect(_recoveryCard, &QPushButton::clicked,
            this, &IntroVerifyChoice::recoveryKeyChosen);

    // Skip verification link. Muted, not accent: the design keeps it visibly
    // secondary so it doesn't read as the recommended way out.
    _skipButton = new intro::LinkButton(tr("Skip for now"), this, true);
    _skipButton->setCursor(Qt::PointingHandCursor);
    {
        auto skipFont = st::baseFont(13);
        _skipButton->setFont(skipFont);
    }
    _skipButton->adjustSize();
    connect(_skipButton, &QPushButton::clicked,
            this, &IntroVerifyChoice::skipVerification);

    // Hide the next button — this screen navigates via cards.
    nextButton()->hide();

    _checkingLabel = new QLabel(tr("Checking your account…"), this);
    _checkingLabel->setAlignment(Qt::AlignHCenter);
    _checkingLabel->setFont(st::baseFont(13));
    {
        QPalette pal = _checkingLabel->palette();
        pal.setColor(QPalette::WindowText, intro::subtextFg);
        _checkingLabel->setPalette(pal);
    }
    _checkingLabel->hide();

    // Only announce the wait if it is long enough to notice — the probe usually answers in well
    // under this, and a label that blinks in and out is worse than no label.
    _checkingLabelTimer = new QTimer(this);
    _checkingLabelTimer->setSingleShot(true);
    _checkingLabelTimer->setInterval(250);
    connect(_checkingLabelTimer, &QTimer::timeout, this, [this] {
        if (_checking) {
            _checkingLabel->show();
        }
    });

    // If the probe never answers, fall back to offering every method — the behaviour before the
    // probe existed. A flaky network must not strand anyone on a blank screen.
    _checkingTimeout = new QTimer(this);
    _checkingTimeout->setSingleShot(true);
    _checkingTimeout->setInterval(8000);
    connect(_checkingTimeout, &QTimer::timeout, this, [this] {
        if (_checking) {
            qWarning() << "Verification capabilities timed out; offering all methods";
            revealChoices();
        }
    });

    // Listen for capabilities result.
    connect(_bridge, &ProtocolBridge::verificationCapabilitiesReady,
            this, &IntroVerifyChoice::onCapabilitiesReady);
}

void IntroVerifyChoice::activate() {
    IntroStep::activate();
    nextButton()->hide();

    // The answer decides whether this screen is even the right one, so hold the cards back until
    // it lands (see the header).
    _checking = true;
    _qrCard->hide();
    _emojiCard->hide();
    _recoveryCard->hide();
    _skipButton->hide();
    _checkingLabel->hide();
    _checkingLabelTimer->start();
    _checkingTimeout->start();

    _bridge->getVerificationCapabilities();
}

void IntroVerifyChoice::onCapabilitiesReady(
    bool success,
    bool canDevice,
    bool canRecovery,
    [[maybe_unused]] bool sasOk,
    [[maybe_unused]] bool qrSupported)
{
    if (!_checking) {
        return;
    }

    if (!success) {
        // The probe failed, so we know nothing. Offer everything, as before.
        revealChoices();
        return;
    }

    if (!canDevice && !canRecovery) {
        // The only session on the account, and no recovery key to enter: there is nothing here to
        // verify against. Rust counts the devices from the server's own list precisely so this
        // branch cannot fire for someone who does have other sessions.
        _checking = false;
        _checkingLabelTimer->stop();
        _checkingTimeout->stop();
        Q_EMIT setupEncryptionNeeded();
        return;
    }

    // Device verification stays offered even when the crypto cache is still cold: starting the
    // flow can fetch the identity it is missing, and QR falls back to emoji when the other
    // session can't scan. Only recovery is a hard yes/no.
    _recoveryEnabled = canRecovery;
    revealChoices();
}

void IntroVerifyChoice::revealChoices() {
    _checking = false;
    _checkingLabelTimer->stop();
    _checkingTimeout->stop();
    _checkingLabel->hide();

    setCardEnabled(_qrCard, _qrEnabled);
    setCardEnabled(_emojiCard, _emojiEnabled);
    setCardEnabled(_recoveryCard, _recoveryEnabled);

    _qrCard->show();
    _emojiCard->show();
    _recoveryCard->show();
    _skipButton->setVisible(allowsSkip());
}

void IntroVerifyChoice::updateSkipVisibility() {
    // Nothing appears while the probe is still out — the cards and this link are
    // revealed together (see the header).
    _skipButton->setVisible(allowsSkip() && !_checking);
}

void IntroVerifyChoice::setCardEnabled(QPushButton *card, bool enabled) {
    // IntroCard paints its enabled/disabled (dimmed) and hover states itself
    // from live intro:: colors; here we only toggle the interactive state.
    card->setEnabled(enabled);
    card->setCursor(enabled ? Qt::PointingHandCursor : Qt::ArrowCursor);
    card->update();
}

void IntroVerifyChoice::submit() {
    // No-op — navigation is handled by card clicks.
}

QString IntroVerifyChoice::nextButtonText() const {
    return QString();
}

void IntroVerifyChoice::resizeEvent(QResizeEvent *e) {
    IntroStep::resizeEvent(e);
    updateChoiceLayout();
}

void IntroVerifyChoice::updateChoiceLayout() {
    const auto left = contentLeft();
    const auto top = contentTop();
    const auto cardLeft = left + (st::introStepWidth - st::introVerifyCardWidth) / 2;

    int y = top + st::introStepFieldTop;

    // Sits where the first card would, so the wait reads as part of the same block.
    _checkingLabel->setFixedWidth(st::introStepWidth);
    _checkingLabel->move(left, y);

    _qrCard->move(cardLeft, y);
    y += st::introVerifyCardHeight + kCardGap;

    _emojiCard->move(cardLeft, y);
    y += st::introVerifyCardHeight + kCardGap;

    _recoveryCard->move(cardLeft, y);
    y += st::introVerifyCardHeight + st::introVerifySkipTop;

    // Center the skip button below cards.
    _skipButton->move(
        left + (st::introStepWidth - _skipButton->width()) / 2,
        y);
}

} // namespace TeleMatrix

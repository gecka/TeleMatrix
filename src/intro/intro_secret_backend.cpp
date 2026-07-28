// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "intro_secret_backend.h"
#include "intro_widget.h"

#include "../history/history_confirm_dialog.h"
#include "../protocol/protocol_bridge.h"
#include "../styles/style_constants.h"
#include "intro_colors.h"
#include "intro_widgets.h"

#include "ui/painter.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QResizeEvent>

#include <functional>

namespace TeleMatrix {

namespace {

// Gap between the two backend cards.
constexpr int kCardGap = 10;

// Spacings from the redesign: heading -> copy -> cards -> button.
constexpr int kHeadingToBody = 7;
constexpr int kBodyToCards = 24;
constexpr int kCardsToButton = 24;

constexpr int kColumnWidth = 392;

} // namespace

IntroSecretBackend::IntroSecretBackend(QWidget *parent)
    : IntroStep(parent, false /* hasCover */)
{
    // This screen lays its heading out itself, left-aligned in a wider column.
    setManagesHeadings(true);
    setTitleText(tr("Protect your data"));
    setDescriptionText(tr(
        "Where TeleMatrix keeps your encryption keys on this device."));

    auto createCard = [this](const QString &title, const QString &subtitle) {
        // Selectable: a radio dot on the left and a 2px accent ring when chosen.
        auto *card = new intro::OptionCard(title, subtitle, true, this);
        card->setFixedWidth(kColumnWidth);
        card->setFixedHeight(card->heightForWidth(kColumnWidth));
        return card;
    };

    _keychainCard = createCard(
        tr("System keychain"),
        tr("Unlocks with your device login. Nothing extra to remember."));
    connect(_keychainCard, &QPushButton::clicked, this, [this] { select(false); });

    _vaultCard = createCard(
        tr("Private vault"),
        tr("Asks for a master password each launch. Can't be reset if you forget it."));
    connect(_vaultCard, &QPushButton::clicked, this, [this] { select(true); });
}

void IntroSecretBackend::activate() {
    IntroStep::activate();

    // Live probe: on a system with no keychain (e.g. Linux without a Secret
    // Service) the vault is the only option, so disable + force it.
    _keychainAvailable = ProtocolBridge::secretServiceAvailable();
    _keychainCard->setEnabled(_keychainAvailable);
    _keychainCard->setCursor(_keychainAvailable ? Qt::PointingHandCursor
                                                : Qt::ArrowCursor);
    // The redesign has no "?" badges, so when the keychain cannot be used the
    // card says why itself rather than hiding it behind a popup — on Linux this
    // is the difference between "disabled for no reason" and an explanation.
    auto *keychain = static_cast<intro::OptionCard *>(_keychainCard);
    if (_keychainAvailable) {
        keychain->setDescription(
            tr("Unlocks with your device login. Nothing extra to remember."));
    } else {
        keychain->setDescription(ProtocolBridge::secretServiceStatus() == 1
            ? tr("Unavailable: no D-Bus session bus is running on this system.")
            : tr("Unavailable: no secret service (GNOME Keyring, KWallet) was "
                 "found on this system."));
    }
    updateChoiceLayout();

    // Pre-select from the currently-configured backend (2/3/4 = a vault backend).
    const int state = ProtocolBridge::secretStoreState();
    const bool preferVault = (state == 2 || state == 3 || state == 4);
    select(!_keychainAvailable ? true : preferVault);
}

void IntroSecretBackend::select(bool vault) {
    _vaultSelected = vault;
    static_cast<intro::OptionCard *>(_vaultCard)->setSelected(vault);
    static_cast<intro::OptionCard *>(_keychainCard)->setSelected(!vault);
}

void IntroSecretBackend::submit() {
    hideError();
    if (_vaultSelected) {
        // Set the master password on the next in-window form (not a popup), unless
        // the vault is already unlocked (3) from an earlier pass through this step.
        if (ProtocolBridge::secretStoreState() != 3) {
            emit createMasterPassword();
            return;
        }
        emit secretBackendChosen(true);
    } else {
        // Keychain chosen: migrate back if a vault was set up (this run or a prior
        // one). The pre-login secret cache is empty, so this is a pure backend flip.
        const int state = ProtocolBridge::secretStoreState();
        if (state == 2 || state == 3 || state == 4) {
            if (!ProtocolBridge::secretStoreSwitchBackend(0, QString())) {
                showError(tr("Couldn't switch to the system keychain."));
                return;
            }
        }
        emit secretBackendChosen(false);
    }
    emit goNext();
}

QString IntroSecretBackend::choiceLabel() const {
    return _vaultSelected ? tr("Private vault") : tr("System keychain");
}

QString IntroSecretBackend::nextButtonText() const {
    return tr("Continue");
}

void IntroSecretBackend::resizeEvent(QResizeEvent *e) {
    IntroStep::resizeEvent(e);
    updateChoiceLayout();
}

void IntroSecretBackend::updateChoiceLayout() {
    // A 392px column, left-aligned heading and copy, per the redesign — wider
    // than the 340px form column because the card descriptions are full
    // sentences.
    const auto left = (width() - kColumnWidth) / 2;
    auto y = contentTop();

    const QFontMetrics headingMetrics(intro::headingFont());
    titleLabel()->setFont(intro::headingFont());
    titleLabel()->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    titleLabel()->setGeometry(left, y, kColumnWidth, headingMetrics.height());
    y += headingMetrics.height() + kHeadingToBody;

    const auto bodyFont = st::baseFont(intro::metrics::bodySize);
    descriptionLabel()->setFont(bodyFont);
    descriptionLabel()->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    const auto bodyHeight =
        descriptionLabel()->heightForWidth(kColumnWidth);
    descriptionLabel()->setGeometry(left, y, kColumnWidth, bodyHeight);
    y += bodyHeight + kBodyToCards;

    placeErrorAbove(y);

    for (auto *card : { _keychainCard, _vaultCard }) {
        const auto height =
            static_cast<intro::OptionCard *>(card)->heightForWidth(kColumnWidth);
        card->setGeometry(left, y, kColumnWidth, height);
        y += height + kCardGap;
    }

    y += kCardsToButton - kCardGap;
    nextButton()->setFixedSize(kColumnWidth, intro::metrics::buttonHeight);
    nextButton()->move(left, y);

    centerContentVertically();
}

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "intro_setup_encryption.h"
#include "intro_widget.h"

#include "../protocol/protocol_bridge.h"
#include "../styles/style_constants.h"
#include "intro_colors.h"
#include "intro_widgets.h"

#include "ui/painter.h"
#include "ui/recovery_key_format.h"
#include "ui/toast_widget.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QFontMetrics>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimer>

namespace TeleMatrix {

namespace {

// How long the copied key is left on the clipboard before it is taken back, matching the
// recovery-key dialog in settings.
constexpr int kClipboardClearMs = 30000;

constexpr int kKeyBoxWidth = 340;
constexpr int kKeyBoxHeight = 56;
constexpr int kKeyBoxRadius = 6;
constexpr int kKeyBoxPadding = 12;
constexpr int kRowGap = 12;

// The homeserver error code for "an unusable key backup is already on the account".
constexpr int kErrorBackupExists = 1;


// The recovery key on its rounded, bordered plate.
class KeyPlate : public QLabel {
public:
    explicit KeyPlate(QWidget *parent) : QLabel(parent) {
        setWordWrap(true);
        setAlignment(Qt::AlignCenter);
        setTextInteractionFlags(Qt::TextSelectableByMouse);
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        p.setPen(QPen(intro::inputBorder, 1));
        p.setBrush(intro::bg);
        const auto r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.drawRoundedRect(r, kKeyBoxRadius, kKeyBoxRadius);
        QLabel::paintEvent(event);
    }
};

// Checkbox painted with the intro palette — the app-wide one is themed for the main window.
class IntroCheckBox : public QCheckBox {
public:
    IntroCheckBox(const QString &text, QWidget *parent) : QCheckBox(text, parent) {
        setCursor(Qt::PointingHandCursor);
        setFont(st::baseFont(13));
    }

    [[nodiscard]] QSize sizeHint() const override {
        const QFontMetrics fm(font());
        return QSize(
            kIndicator + kSkip + fm.horizontalAdvance(text()),
            qMax(24, fm.lineSpacing() + 6));
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);

        const auto top = (height() - kIndicator) / 2;
        const QRect box(0, top, kIndicator, kIndicator);
        const auto checked = isChecked();

        p.setPen(QPen(checked ? intro::bgActive : intro::inputBorder, 2));
        p.setBrush(checked ? intro::bgActive : intro::bg);
        p.drawRoundedRect(box.adjusted(1, 1, -1, -1), 3, 3);

        if (checked) {
            p.setPen(QPen(intro::buttonFg, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const auto left = box.left();
            const auto y = box.top();
            p.drawLine(QPointF(left + 4, y + 9), QPointF(left + 8, y + 13));
            p.drawLine(QPointF(left + 8, y + 13), QPointF(left + 14, y + 5));
        }

        p.setPen(intro::titleFg);
        p.setFont(font());
        p.drawText(
            QRect(kIndicator + kSkip, 0, width() - kIndicator - kSkip, height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            text());
    }

private:
    static constexpr int kIndicator = 18;
    static constexpr int kSkip = 8;
};

} // namespace

IntroSetupEncryption::IntroSetupEncryption(IntroWidget *parent, ProtocolBridge *bridge)
    : IntroStep(parent, false /* hasCover */)
    , _bridge(bridge)
{
    titleLabel()->setAlignment(Qt::AlignHCenter);
    descriptionLabel()->setAlignment(Qt::AlignHCenter);

    _statusLabel = new QLabel(this);
    _statusLabel->setAlignment(Qt::AlignHCenter);
    _statusLabel->setWordWrap(true);
    _statusLabel->setFont(st::baseFont(13));
    {
        QPalette pal = _statusLabel->palette();
        pal.setColor(QPalette::WindowText, intro::subtextFg);
        _statusLabel->setPalette(pal);
    }

    _keyLabel = new KeyPlate(this);
    _keyLabel->setFixedSize(kKeyBoxWidth, kKeyBoxHeight);
    _keyLabel->setFont(st::monospaceFont(st::introRecoveryKeyFontSize));
    _keyLabel->setContentsMargins(
        kKeyBoxPadding, kKeyBoxPadding, kKeyBoxPadding, kKeyBoxPadding);
    {
        QPalette pal = _keyLabel->palette();
        pal.setColor(QPalette::WindowText, intro::titleFg);
        _keyLabel->setPalette(pal);
    }

    _copyButton = new intro::LinkButton(tr("Copy"), this);
    _copyButton->setCursor(Qt::PointingHandCursor);
    _copyButton->setFont(st::baseFont(13));
    _copyButton->adjustSize();
    connect(_copyButton, &QPushButton::clicked, this, &IntroSetupEncryption::copyKey);

    _savedCheckbox = new IntroCheckBox(tr("I have saved my recovery key"), this);
    _savedCheckbox->adjustSize();
    connect(_savedCheckbox, &QCheckBox::toggled,
            this, &IntroSetupEncryption::updateContinueState);

    _skipLink = new intro::LinkButton(tr("Skip for now"), this);
    _skipLink->setCursor(Qt::PointingHandCursor);
    _skipLink->setFont(st::baseFont(13));
    _skipLink->adjustSize();
    connect(_skipLink, &QPushButton::clicked, this, [this] {
        if (_active) {
            Q_EMIT skipped();
        }
    });

    connect(_bridge, &ProtocolBridge::recoverySetupResult,
            this, &IntroSetupEncryption::onSetupResult);

    setState(State::Working);
}

void IntroSetupEncryption::activate() {
    IntroStep::activate();
    _active = true;
    if (!_setupRequested) {
        startSetup();
    }
}

void IntroSetupEncryption::deactivate() {
    _active = false;
    _setupRequested = false;
}

void IntroSetupEncryption::startSetup() {
    _setupRequested = true;
    _key.clear();
    _savedCheckbox->setChecked(false);
    setState(State::Working);
    _bridge->setupRecovery();
}

void IntroSetupEncryption::onSetupResult(
    bool success,
    const QString &recoveryKey,
    int errorCode,
    const QString &error)
{
    if (!_active) {
        return;
    }

    if (success) {
        _key = recoveryKey;
        _keyLabel->setText(FormatRecoveryKey(recoveryKey, 0).text);
        setState(State::Ready);
        return;
    }

    if (errorCode == kErrorBackupExists) {
        setState(State::BackupExists);
        return;
    }

    _statusLabel->setText(error.isEmpty()
        ? tr("Encryption could not be set up.")
        : error);
    setState(State::Failed);
}

void IntroSetupEncryption::setState(State state) {
    _state = state;

    const auto ready = (state == State::Ready);
    _keyLabel->setVisible(ready);
    _copyButton->setVisible(ready);
    _savedCheckbox->setVisible(ready);
    _statusLabel->setVisible(!ready);

    // Skipping is always allowed, including while the homeserver is still being waited on — a
    // request that never comes back must not trap anyone in onboarding. It leaves any key that was
    // already provisioned in place; it only means the user chose not to write it down now, and
    // Settings can show it again later.
    _skipLink->setVisible(true);
    nextButton()->setVisible(state != State::Working);

    switch (state) {
    case State::Working:
        setTitleText(tr("Setting up encryption"));
        setDescriptionText(tr("Just a moment while we secure your messages."));
        _statusLabel->setText(QString());
        break;
    case State::Ready:
        setTitleText(tr("Set up encryption"));
        setDescriptionText(tr(
            "Save your recovery key. It restores your encrypted messages on a new device, "
            "and it is the only way back in if you lose this one."));
        break;
    case State::BackupExists:
        setTitleText(tr("Encryption needs a reset"));
        setDescriptionText(tr(
            "This account already has a key backup, but it cannot be unlocked without its "
            "recovery key. Resetting replaces it with a new backup and a new recovery key. "
            "Messages that only exist in the old backup will stay unreadable."));
        _statusLabel->setText(QString());
        break;
    case State::Failed:
        setTitleText(tr("Encryption setup failed"));
        setDescriptionText(tr(
            "Your account is signed in and works, but your encrypted messages are not backed "
            "up yet. You can set this up later in Settings."));
        break;
    }

    nextButton()->setText(nextButtonText());
    updateContinueState();
    updateSetupLayout();
}

void IntroSetupEncryption::updateContinueState() {
    // The key is on screen exactly once, so Continue waits for the user to say they wrote it down.
    nextButton()->setEnabled(
        _state != State::Ready || _savedCheckbox->isChecked());
}

QString IntroSetupEncryption::nextButtonText() const {
    switch (_state) {
    case State::BackupExists:
        return tr("Reset key backup");
    case State::Failed:
        return tr("Try again");
    default:
        return tr("Continue");
    }
}

void IntroSetupEncryption::submit() {
    if (!_active) {
        return;
    }
    switch (_state) {
    case State::Working:
        break;
    case State::Ready:
        if (_savedCheckbox->isChecked()) {
            Q_EMIT done();
        }
        break;
    case State::BackupExists:
        setState(State::Working);
        _bridge->resetRecovery();
        break;
    case State::Failed:
        startSetup();
        break;
    }
}

void IntroSetupEncryption::copyKey() {
    if (_key.isEmpty()) {
        return;
    }
    QApplication::clipboard()->setText(_key);
    ::Ui::ShowToast(tr("Recovery key copied to clipboard"));

    const auto copied = _key;
    QTimer::singleShot(kClipboardClearMs, qApp, [copied] {
        auto *clipboard = QApplication::clipboard();
        if (clipboard && clipboard->text() == copied) {
            clipboard->clear();
        }
    });
}

void IntroSetupEncryption::resizeEvent(QResizeEvent *e) {
    IntroStep::resizeEvent(e);
    updateSetupLayout();
}

void IntroSetupEncryption::updateSetupLayout() {
    _copyButton->adjustSize();
    _skipLink->adjustSize();
    _savedCheckbox->adjustSize();

    const auto left = contentLeft();
    const auto width = st::introStepWidth;

    titleLabel()->setFixedWidth(width);
    descriptionLabel()->setFixedWidth(width);
    const auto descriptionHeight = qMax(
        descriptionLabel()->heightForWidth(width),
        descriptionLabel()->sizeHint().height());
    descriptionLabel()->setFixedHeight(descriptionHeight);

    // The block is measured before it is placed so it can be centred as a whole; the description
    // wraps to a different height in each state, so this cannot be a fixed table of offsets.
    const auto bodyHeight = (_state == State::Ready)
        ? (kKeyBoxHeight + kRowGap + _savedCheckbox->height())
        : _statusLabel->heightForWidth(width);
    // Continue is hidden while we wait; the skip link then moves up into its place. Keyed off the
    // state, not isVisible(): the step lays itself out while it is still off-screen in the stack.
    const auto continueBlock = (_state != State::Working)
        ? (st::introNextButtonHeight + st::introLinkTop)
        : 0;
    const auto groupHeight = st::introStepFieldTop
        + bodyHeight
        + kRowGap * 2
        + continueBlock
        + _skipLink->height();
    const auto top = qMax(st::introStepTopMin, (this->height() - groupHeight) / 2);

    titleLabel()->move(left, top + st::introTitleTop);
    descriptionLabel()->move(left, top + st::introDescriptionTop);

    const auto bodyY = top + st::introStepFieldTop;

    // The Copy link sits to the right of the plate, so the pair is centred together.
    const auto plateBlockWidth = kKeyBoxWidth + kRowGap + _copyButton->width();
    const auto plateLeft = (this->width() - plateBlockWidth) / 2;
    _keyLabel->move(plateLeft, bodyY);
    _copyButton->move(
        plateLeft + kKeyBoxWidth + kRowGap,
        bodyY + (kKeyBoxHeight - _copyButton->height()) / 2);

    _savedCheckbox->move(
        (this->width() - _savedCheckbox->width()) / 2,
        bodyY + kKeyBoxHeight + kRowGap);

    _statusLabel->setFixedWidth(width);
    _statusLabel->setFixedHeight(qMax(0, _statusLabel->heightForWidth(width)));
    _statusLabel->move(left, bodyY);

    const auto buttonY = bodyY + bodyHeight + kRowGap * 2;
    nextButton()->move((this->width() - st::introNextButtonWidth) / 2, buttonY);

    _skipLink->move(
        (this->width() - _skipLink->width()) / 2,
        buttonY + continueBlock);
}

} // namespace TeleMatrix

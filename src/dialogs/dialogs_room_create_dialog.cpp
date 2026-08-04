// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "dialogs_room_create_dialog.h"
#include "ui/widgets/emoji_input_field.h"
#include "ui/widgets/input_fields.h"

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QFont>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QResizeEvent>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

#include "styles/style_constants.h"
#include "ui/focus_restore.h"
#include "ui/internal_choice_dialog.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/close_button.h"
#include "ui/widgets/input_fields.h"

namespace TeleMatrix {

namespace {

// Box round shadow: 8px corner radius, extended 10px on every side.
constexpr int kShadowExtend = 10;
constexpr int kCheckIndicatorSize = 18;
constexpr int kCheckIndicatorRadius = 4;
constexpr int kCheckTextSkip = 10;
constexpr int kRoomCreateExtraWidth = 80;
constexpr int kOptionLabelWidthExtra = 18;

// Maximum room name length.
constexpr int kMaxRoomNameLength = 255;

int roomCreateWidth() {
    return st::boxWideWidth + kRoomCreateExtraWidth;
}

void paintBoxShadow(QPainter &p, const QRect &boxRect) {
    PainterHighQualityEnabler hq(p);
    p.setPen(Qt::NoPen);
    for (int i = kShadowExtend; i >= 1; --i) {
        const auto progress = qreal(kShadowExtend - i) / kShadowExtend;
        const auto alpha = int(18.0 * progress * progress);
        p.setBrush(st::withAlpha(st::windowShadowFg, alpha));
        const auto r = st::boxRadius + i;
        p.drawRoundedRect(boxRect.adjusted(-i, -i, i, i), r, r);
    }
}

// Panel surface painted with live st:: colors (so it tracks theme changes)
// instead of a frozen stylesheet background.
class RoundedPanel : public QWidget {
public:
    explicit RoundedPanel(QWidget *parent) : QWidget(parent) {}

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(st::boxBg);
        p.drawRoundedRect(rect(), st::boxRadius, st::boxRadius);
    }
};

// Primary "Create" button style. When disabled, it renders muted
// (windowBgOver / windowSubTextFg) — modelled here by swapping the TextButton
// style from updateCreateButton().
::Ui::TextButton::Style createButtonStyle(bool enabled) {
    ::Ui::TextButton::Style s;
    s.radius = st::boxRadius;
    s.height = st::boxButtonHeight;
    s.paddingH = 15;
    if (enabled) {
        s.bgOver = &st::lightButtonBgOver;  // transparent until hovered
        s.fg = &st::lightButtonFg;
    } else {
        s.fg = &st::windowSubTextFg;  // flat, muted, no hover
    }
    return s;
}

class CreateRoomCheckBox final : public QCheckBox {
public:
    explicit CreateRoomCheckBox(const QString &text, QWidget *parent = nullptr)
        : QCheckBox(text, parent) {
        setCursor(Qt::PointingHandCursor);
        setFont(st::baseFont(14));
        setAttribute(Qt::WA_Hover);
    }

    QSize sizeHint() const override {
        const auto available = std::max(
            160,
            width() > 0
                ? width()
                : roomCreateWidth() - st::boxPadding.left() - st::boxPadding.right());
        const auto textWidth = std::max(80, available - kCheckIndicatorSize - kCheckTextSkip);
        const auto textRect = QFontMetrics(font()).boundingRect(
            QRect(0, 0, textWidth, 1000),
            Qt::TextWordWrap,
            text());
        return QSize(
            available,
            std::max(st::settingsButtonHeight, textRect.height() + 6));
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);

        const auto content = rect().adjusted(0, 3, 0, -3);
        const auto indicator = QRect(
            0,
            content.top() + (content.height() - kCheckIndicatorSize) / 2,
            kCheckIndicatorSize,
            kCheckIndicatorSize);
        auto border = st::inputBorderFg;
        auto textColor = st::windowFg;
        auto fill = st::boxBg;
        if (!isEnabled()) {
            border.setAlpha(120);
            textColor.setAlpha(120);
            fill.setAlpha(180);
        }

        p.setPen(QPen(isChecked() ? st::activeButtonBg : border, 1.5));
        p.setBrush(isChecked() ? st::activeButtonBg : fill);
        p.drawRoundedRect(
            indicator.adjusted(1, 1, -1, -1),
            kCheckIndicatorRadius,
            kCheckIndicatorRadius);

        if (isChecked()) {
            p.setPen(QPen(st::activeButtonFg, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const auto left = indicator.left();
            const auto top = indicator.top();
            p.drawLine(
                QPointF(left + 4.0, top + 9.5),
                QPointF(left + 7.5, top + 13.0));
            p.drawLine(
                QPointF(left + 7.5, top + 13.0),
                QPointF(left + 14.0, top + 5.5));
        }

        const auto textRect = QRect(
            kCheckIndicatorSize + kCheckTextSkip,
            rect().top(),
            rect().width() - kCheckIndicatorSize - kCheckTextSkip,
            rect().height());
        p.setPen(textColor);
        p.drawText(
            textRect,
            Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap,
            text());
    }
};

class CreateRoomValueButton final : public QWidget {
public:
    CreateRoomValueButton(
        const QString &text,
        const QString &value,
        int labelWidth,
        QWidget *parent = nullptr)
        : QWidget(parent)
        , _text(text)
        , _value(value)
        , _labelWidth(labelWidth) {
        setFixedHeight(st::settingsButtonHeight);
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
    }

    void setValue(const QString &value) {
        if (_value == value) {
            return;
        }
        _value = value;
        update();
    }

    void setClickedCallback(std::function<void()> callback) {
        _clicked = std::move(callback);
        setCursor(_clicked ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        if (isEnabled() && _clicked && _hovered) {
            p.fillRect(rect(), st::windowBgOver);
        }

        const auto textFont = st::baseFont(14);
        const auto valueFont = st::baseFont(14);
        const QFontMetrics textMetrics(textFont);
        const QFontMetrics valueMetrics(valueFont);
        const int left = 0;
        const int valueRight = 0;
        const int labelWidth = _labelWidth > 0
            ? _labelWidth
            : textMetrics.horizontalAdvance(_text);
        const int valueLeft = left + labelWidth + 12;
        const int valueMaxWidth = qMax(
            0,
            width()
                - valueLeft
                - valueRight);
        const auto value = valueMetrics.elidedText(_value, Qt::ElideRight, valueMaxWidth);
        const int valueWidth = value.isEmpty() ? 0 : valueMetrics.horizontalAdvance(value);
        const int valueDrawLeft = width() - valueRight - valueWidth;
        const QRect titleRect(left, 0, qMax(0, labelWidth), height());
        const auto titleColor = isEnabled()
            ? st::settingsCheckboxTextFg
            : st::windowSubTextFg;
        const auto valueColor = isEnabled()
            ? st::windowActiveTextFg
            : st::windowSubTextFg;

        p.setFont(textFont);
        p.setPen(titleColor);
        p.drawText(
            titleRect,
            Qt::AlignLeft | Qt::AlignVCenter,
            textMetrics.elidedText(_text, Qt::ElideRight, titleRect.width()));

        if (!value.isEmpty()) {
            p.setFont(valueFont);
            p.setPen(valueColor);
            p.drawText(
                QRect(valueDrawLeft, 0, valueWidth, height()),
                Qt::AlignRight | Qt::AlignVCenter,
                value);
        }
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (isEnabled() && e->button() == Qt::LeftButton && _clicked) {
            _clicked();
            return;
        }
        QWidget::mousePressEvent(e);
    }

    void enterEvent(QEnterEvent *) override {
        if (!isEnabled()) {
            return;
        }
        _hovered = true;
        update();
    }

    void leaveEvent(QEvent *) override {
        _hovered = false;
        update();
    }

private:
    QString _text;
    QString _value;
    int _labelWidth = 0;
    bool _hovered = false;
    std::function<void()> _clicked;
};

QWidget *reservedSlotFor(QWidget *child, QWidget *parent) {
    auto *slot = new QWidget(parent);
    slot->setFixedHeight(qMax(st::settingsButtonHeight, qMax(child->sizeHint().height(), child->height())));
    auto *layout = new QVBoxLayout(slot);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(child, 0, Qt::AlignVCenter);
    return slot;
}

void setReservedSlotEnabled(QWidget *slot, bool enabled) {
    if (!slot) {
        return;
    }
    slot->setVisible(true);
    slot->setAttribute(Qt::WA_TransparentForMouseEvents, !enabled);
    const auto children = slot->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
    for (auto *child : children) {
        child->setVisible(true);
        child->setEnabled(enabled);
    }
}

QString guestAccessText(int value) {
    return value == 1
        ? QCoreApplication::translate("DialogsRoomCreateDialog", "Guests can join")
        : QCoreApplication::translate("DialogsRoomCreateDialog", "Guests cannot join");
}

QString historyVisibilityText(int value) {
    switch (value) {
    case 0:
        return QCoreApplication::translate(
            "DialogsRoomCreateDialog",
            "Members only (from join)");
    case 1:
        return QCoreApplication::translate(
            "DialogsRoomCreateDialog",
            "Members only (from invite)");
    case 3:
        return QCoreApplication::translate(
            "DialogsRoomCreateDialog",
            "Anyone");
    case 2:
    default:
        return QCoreApplication::translate(
            "DialogsRoomCreateDialog",
            "Members only (full history)");
    }
}

} // namespace

// -----------------------------------------------
// Constructor
// -----------------------------------------------

DialogsRoomCreateDialog::DialogsRoomCreateDialog(const QString &serverName, QWidget *parent)
    : QWidget(parent ? parent->window() : nullptr)
    , _serverName(serverName.trimmed()) {
    init();
}

// -----------------------------------------------
// Common initialization
// -----------------------------------------------

void DialogsRoomCreateDialog::init() {
    if (parentWidget()) {
        setGeometry(parentWidget()->rect());
        parentWidget()->installEventFilter(this);
    }

    setFocusPolicy(Qt::StrongFocus);

    _bgOpacity = 0.0;
    _layerOpacity = 0.0;

    // _a_shown: background overlay, easeOutCirc, 200ms (st::boxDuration).
    _a_shown = new QVariantAnimation(this);
    _a_shown->setDuration(200);
    _a_shown->setEasingCurve(QEasingCurve::OutCirc);
    _a_shown->setStartValue(0.0);
    _a_shown->setEndValue(1.0);
    connect(_a_shown, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) {
        _bgOpacity = value.toReal();
        update();
    });

    // _a_layerShown: box shadow, linear, 200ms.
    _a_layerShown = new QVariantAnimation(this);
    _a_layerShown->setDuration(200);
    _a_layerShown->setEasingCurve(QEasingCurve::Linear);
    _a_layerShown->setStartValue(0.0);
    _a_layerShown->setEndValue(1.0);
    connect(_a_layerShown, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) {
        _layerOpacity = value.toReal();
        if (_panel && !_panel->isVisible() && _layerOpacity > 0) {
            _panel->setVisible(true);
        }
        update();
    });

    // --- Root layout: centers the panel in the overlay ---
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addStretch(1);

    // --- Panel: the white box, widened for long option-row labels. ---
    _panel = new RoundedPanel(this);
    _panel->setVisible(false); // shown on first animation tick
    _panel->setFixedWidth(roomCreateWidth());
    root->addWidget(_panel, 0, Qt::AlignHCenter);
    root->addStretch(1);

    auto *panelLayout = new QVBoxLayout(_panel);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(0);

    // --- Title bar: "New Room" + close X ---
    auto *titleBar = new QWidget(_panel);
    titleBar->setFixedHeight(st::boxTitleHeight);
    panelLayout->addWidget(titleBar);

    // Title label.
    auto *titleText = new QLabel(QCoreApplication::translate("DialogsRoomCreateDialog", "New Room"), titleBar);
    titleText->setFont(st::boxTitleFont);
    {
        QPalette pal = titleText->palette();
        pal.setColor(QPalette::WindowText, st::boxTitleFg);
        titleText->setPalette(pal);
    }
    titleText->move(st::boxTitlePosition.x(), st::boxTitlePosition.y());

    // Close button: the shared content-box × affordance. Its clicked() is
    // ignored while disabled, so setControlsEnabled() gates dismissal (see the
    // _closeButton->setEnabled() call there).
    auto *close = new ::Ui::CloseButton(titleBar);
    connect(close, &::Ui::CloseButton::clicked, this,
            [this] { reject(); });
    close->move(roomCreateWidth() - st::settingsCloseButtonSize, 0);
    _closeButton = close;

    // --- Separator below title ---
    // shadowFg is #00000018 (semi-transparent); QPalette::Window keeps the
    // alpha and re-resolves on theme change.
    auto *titleSep = new QWidget(_panel);
    titleSep->setFixedHeight(1);
    titleSep->setAutoFillBackground(true);
    {
        QPalette pal = titleSep->palette();
        pal.setColor(QPalette::Window, st::shadowFg);
        titleSep->setPalette(pal);
    }
    panelLayout->addWidget(titleSep);

    // --- Input fields ---
    auto *inputContainer = new QWidget(_panel);
    auto *inputLayout = new QVBoxLayout(inputContainer);
    inputLayout->setContentsMargins(
        st::boxPadding.left(),   // 24
        st::boxPadding.top(),    // 14
        st::boxPadding.right(),  // 24
        st::boxPadding.bottom()  // 8
    );
    inputLayout->setSpacing(12);

    // Room name input (floating-caption input field).
    auto *nameInput = new ::Ui::EmojiInputField(
        inputContainer,
        st::defaultInputField,
        QCoreApplication::translate("DialogsRoomCreateDialog", "Room name"));
    nameInput->setFloatingPlaceholder(true);
    _nameField = nameInput;
    _nameField->setMaxLength(kMaxRoomNameLength);
    inputLayout->addWidget(nameInput);

    // Topic input (optional; floating-caption input field).
    auto *topicInput = new ::Ui::EmojiInputField(
        inputContainer,
        st::defaultInputField,
        QCoreApplication::translate("DialogsRoomCreateDialog", "Topic (optional)"));
    topicInput->setFloatingPlaceholder(true);
    _topicField = topicInput;
    inputLayout->addWidget(topicInput);

    auto *settingsContainer = new QWidget(inputContainer);
    auto *settingsLayout = new QVBoxLayout(settingsContainer);
    settingsLayout->setContentsMargins(0, 0, 0, 0);
    settingsLayout->setSpacing(inputLayout->spacing());

    // Encryption toggle.
    _encryptedCheck = new CreateRoomCheckBox(
        QCoreApplication::translate("DialogsRoomCreateDialog", "Enable end-to-end encryption"),
        settingsContainer);
    _encryptedCheck->setChecked(true);
    settingsLayout->addWidget(_encryptedCheck);

    const auto guestAccessLabel = QCoreApplication::translate(
        "DialogsRoomCreateDialog",
        "Guest access");
    const auto historyVisibilityLabel = QCoreApplication::translate(
        "DialogsRoomCreateDialog",
        "Who can read history");
    const auto optionLabelWidth = std::max(
        QFontMetrics(st::baseFont(14)).horizontalAdvance(guestAccessLabel),
        QFontMetrics(st::baseFont(14)).horizontalAdvance(historyVisibilityLabel))
        + kOptionLabelWidthExtra;

    _historyVisibilityButton = new CreateRoomValueButton(
        historyVisibilityLabel,
        historyVisibilityText(_historyVisibility),
        optionLabelWidth,
        settingsContainer);
    static_cast<CreateRoomValueButton*>(_historyVisibilityButton)->setClickedCallback([this] {
        showHistoryVisibilityOptions();
    });
    settingsLayout->addWidget(_historyVisibilityButton);

    const auto federatedText = _serverName.isEmpty()
        ? QCoreApplication::translate(
            "DialogsRoomCreateDialog",
            "Block anyone not part of this server from ever joining this room")
        : QCoreApplication::translate(
            "DialogsRoomCreateDialog",
            "Block anyone not part of \"%1\" from ever joining this room").arg(_serverName);
    _federationCheck = new CreateRoomCheckBox(federatedText, settingsContainer);
    _federationCheck->setChecked(false);
    settingsLayout->addWidget(_federationCheck);

    // Public room toggle.
    _publicCheck = new CreateRoomCheckBox(
        QCoreApplication::translate("DialogsRoomCreateDialog", "Public room (anyone can find and join)"),
        settingsContainer);
    _publicCheck->setChecked(false);
    connect(_publicCheck, &QCheckBox::toggled, this,
            &DialogsRoomCreateDialog::updateVisibilityFields);
    settingsLayout->addWidget(_publicCheck);

    // Room alias (shown when public). The server suffix stays visible next to the
    // field so the local-part editor cannot collide with it. Same flat underline
    // and floating caption as the name and topic fields above.
    _aliasContainer = new QWidget(settingsContainer);
    _aliasContainer->setFixedHeight(st::defaultInputField.heightMin);

    auto *aliasLayout = new QHBoxLayout(_aliasContainer);
    aliasLayout->setContentsMargins(0, 0, 0, 0);
    aliasLayout->setSpacing(8);

    auto *aliasInput = new ::Ui::InputField(
        _aliasContainer,
        st::defaultInputField,
        rpl::single<QString>(
            QCoreApplication::translate("DialogsRoomCreateDialog", "Room address")));
    aliasInput->setFloatingPlaceholder(true);
    _aliasField = aliasInput;
    aliasLayout->addWidget(aliasInput, 1);

    _aliasSuffixLabel = new QLabel(_aliasContainer);
    _aliasSuffixLabel->setFont(st::normalFont);
    _aliasSuffixLabel->setText(_serverName.isEmpty()
        ? QString()
        : QStringLiteral(":%1").arg(_serverName));
    {
        QPalette pal = _aliasSuffixLabel->palette();
        pal.setColor(QPalette::WindowText, st::windowSubTextFg);
        _aliasSuffixLabel->setPalette(pal);
    }
    _aliasSuffixLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    _aliasSuffixLabel->setVisible(!_serverName.isEmpty());
    // Bottom-aligned onto the field's text line, lifted clear of its underline.
    _aliasSuffixLabel->setContentsMargins(
        0, 0, 0, st::defaultInputField.textMargins.bottom());
    aliasLayout->addWidget(_aliasSuffixLabel, 0, Qt::AlignBottom);
    _aliasSlot = reservedSlotFor(_aliasContainer, settingsContainer);
    settingsLayout->addWidget(_aliasSlot);

    _guestAccessButton = new CreateRoomValueButton(
        guestAccessLabel,
        guestAccessText(_guestAccess),
        optionLabelWidth,
        settingsContainer);
    static_cast<CreateRoomValueButton*>(_guestAccessButton)->setClickedCallback([this] {
        showGuestAccessOptions();
    });
    _guestAccessSlot = reservedSlotFor(_guestAccessButton, settingsContainer);
    settingsLayout->addWidget(_guestAccessSlot);
    inputLayout->addWidget(settingsContainer);

    // Error label (hidden by default).
    _errorLabel = new QLabel(inputContainer);
    _errorLabel->setFont(st::baseFont(13));
    {
        QPalette pal = _errorLabel->palette();
        pal.setColor(QPalette::WindowText, st::attentionButtonFg);
        _errorLabel->setPalette(pal);
    }
    _errorLabel->setWordWrap(true);
    _errorLabel->setFixedHeight(38);
    inputLayout->addWidget(_errorLabel);

    panelLayout->addWidget(inputContainer);

    // Initial visibility state (alias/guest hidden when private).
    updateVisibilityFields();

    // --- Buttons area ---
    auto *buttonsContainer = new QWidget(_panel);
    buttonsContainer->setFixedHeight(
        st::boxButtonPadding.top()
        + st::boxButtonHeight
        + st::boxButtonPadding.bottom());
    panelLayout->addWidget(buttonsContainer);

    auto *buttonsLayout = new QHBoxLayout(buttonsContainer);
    buttonsLayout->setContentsMargins(
        st::boxButtonPadding.left(),
        st::boxButtonPadding.top(),
        st::boxButtonPadding.right(),
        st::boxButtonPadding.bottom());
    buttonsLayout->setSpacing(8);
    buttonsLayout->addStretch(1);

    // Cancel button (defaultLightButton style).
    ::Ui::TextButton::Style cancelStyle;
    cancelStyle.bgOver = &st::lightButtonBgOver; // transparent until hovered
    cancelStyle.fg = &st::lightButtonFg;
    cancelStyle.radius = st::boxRadius;
    cancelStyle.height = st::boxButtonHeight;
    cancelStyle.paddingH = 15;
    _cancel = new ::Ui::TextButton(
        QCoreApplication::translate("DialogsRoomCreateDialog", "Cancel"),
        cancelStyle,
        buttonsContainer);
    {
        auto f = st::baseFont(14);
        f.setWeight(QFont::DemiBold);
        _cancel->setFont(f);
    }
    connect(_cancel, &QAbstractButton::clicked, this, [this] { reject(); });
    buttonsLayout->addWidget(_cancel);

    // Create button (defaultActiveButton style). The disabled appearance is
    // applied in updateCreateButton() by swapping the TextButton style.
    _create = new ::Ui::TextButton(
        QCoreApplication::translate("DialogsRoomCreateDialog", "Create"),
        createButtonStyle(true),
        buttonsContainer);
    {
        auto f = st::baseFont(14);
        f.setWeight(QFont::DemiBold);
        _create->setFont(f);
    }
    _create->setFixedHeight(st::boxButtonHeight);
    connect(_create, &QAbstractButton::clicked, this, [this] {
        emit createRequested();
    });
    buttonsLayout->addWidget(_create);

    // --- Connections ---
    connect(_nameField, &QTextEdit::textChanged, this,
            [this] { updateCreateButton(); });

    // Enter key triggers Create.
    connect(_nameField, &::Ui::EmojiInputField::submitted, this, [this] {
        if (_create->isEnabled()) {
            emit createRequested();
        }
    });
    connect(_topicField, &::Ui::EmojiInputField::submitted, this, [this] {
        if (_create->isEnabled()) {
            emit createRequested();
        }
    });

    // Initial button state.
    updateCreateButton();

    // Focus name field on open.
    QTimer::singleShot(0, _nameField, [this] {
        _nameField->setFocus();
    });
}

// -----------------------------------------------
// exec / accept / reject
// -----------------------------------------------

int DialogsRoomCreateDialog::exec() {
    const auto restoreFocus = ::TeleMatrix::Focus::saveFocusForPopup();

    raise();
    show();
    setFocus();

    if (_a_shown) {
        _a_shown->start();
    }
    if (_a_layerShown) {
        _a_layerShown->start();
    }

    QEventLoop loop;
    _loop = &loop;
    loop.exec();
    _loop = nullptr;

    hide();
    ::TeleMatrix::Focus::restoreFocusAfterPopup(restoreFocus);
    return _result;
}

void DialogsRoomCreateDialog::accept() {
    _result = Accepted;
    if (_loop) _loop->quit();
}

void DialogsRoomCreateDialog::reject() {
    _result = Rejected;
    if (_loop) _loop->quit();
}

QString DialogsRoomCreateDialog::roomName() const {
    return _nameField ? _nameField->text().trimmed() : QString();
}

QString DialogsRoomCreateDialog::roomTopic() const {
    return _topicField ? _topicField->text().trimmed() : QString();
}

bool DialogsRoomCreateDialog::isPublic() const {
    return _publicCheck && _publicCheck->isChecked();
}

bool DialogsRoomCreateDialog::isEncrypted() const {
    return _encryptedCheck && _encryptedCheck->isChecked();
}

QString DialogsRoomCreateDialog::roomAlias() const {
    return isPublic() && _aliasField
        ? _aliasField->text().trimmed()
        : QString();
}

QString DialogsRoomCreateDialog::avatarPath() const {
    return QString();
}

int DialogsRoomCreateDialog::guestAccess() const {
    return isPublic() ? _guestAccess : 0;
}

int DialogsRoomCreateDialog::historyVisibility() const {
    return _historyVisibility;
}

bool DialogsRoomCreateDialog::blockFederated() const {
    return _federationCheck && _federationCheck->isChecked();
}

void DialogsRoomCreateDialog::updateVisibilityFields() {
    const bool pub = _publicCheck && _publicCheck->isChecked();

    // Alias and guest access are public-room settings: always visible,
    // disabled until Public room is selected.
    setReservedSlotEnabled(_aliasSlot, pub && _controlsEnabled);
    setReservedSlotEnabled(_guestAccessSlot, pub && _controlsEnabled);

    // Encryption is disabled by default for public rooms;
    // uncheck it when switching to public, re-check when going private.
    if (_encryptedCheck) {
        _encryptedCheck->setChecked(!pub);
    }
}

void DialogsRoomCreateDialog::showGuestAccessOptions() {
    if (!_controlsEnabled || !isPublic()) {
        return;
    }

    QVector<Ui::InternalChoiceEntry> entries;
    entries.push_back({
        QStringLiteral("0"),
        guestAccessText(0),
        QString(),
        st::baseFont(14),
        true,
    });
    entries.push_back({
        QStringLiteral("1"),
        guestAccessText(1),
        QString(),
        st::baseFont(14),
        true,
    });

    Ui::InternalChoiceDialog dialog(
        this,
        QCoreApplication::translate("DialogsRoomCreateDialog", "Guest access"),
        entries,
        QString::number(_guestAccess));
    if (dialog.exec() != Ui::InternalChoiceDialog::Accepted) {
        return;
    }
    bool ok = false;
    const auto value = dialog.chosenId().toInt(&ok);
    if (ok) {
        _guestAccess = value;
        updateOptionRows();
    }
}

void DialogsRoomCreateDialog::showHistoryVisibilityOptions() {
    if (!_controlsEnabled) {
        return;
    }

    QVector<Ui::InternalChoiceEntry> entries;
    entries.push_back({
        QStringLiteral("0"),
        historyVisibilityText(0),
        QCoreApplication::translate(
            "DialogsRoomCreateDialog",
            "Only from the point they joined"),
        st::baseFont(14),
        true,
    });
    entries.push_back({
        QStringLiteral("1"),
        historyVisibilityText(1),
        QCoreApplication::translate(
            "DialogsRoomCreateDialog",
            "Only from the point they were invited"),
        st::baseFont(14),
        true,
    });
    entries.push_back({
        QStringLiteral("2"),
        historyVisibilityText(2),
        QCoreApplication::translate(
            "DialogsRoomCreateDialog",
            "All members, including future members"),
        st::baseFont(14),
        true,
    });
    entries.push_back({
        QStringLiteral("3"),
        historyVisibilityText(3),
        QCoreApplication::translate(
            "DialogsRoomCreateDialog",
            "World-readable room history"),
        st::baseFont(14),
        true,
    });

    Ui::InternalChoiceDialog dialog(
        this,
        QCoreApplication::translate("DialogsRoomCreateDialog", "Who can read history"),
        entries,
        QString::number(_historyVisibility));
    if (dialog.exec() != Ui::InternalChoiceDialog::Accepted) {
        return;
    }
    bool ok = false;
    const auto value = dialog.chosenId().toInt(&ok);
    if (ok) {
        _historyVisibility = value;
        updateOptionRows();
    }
}

void DialogsRoomCreateDialog::updateOptionRows() {
    if (_guestAccessButton) {
        static_cast<CreateRoomValueButton*>(_guestAccessButton)->setValue(
            guestAccessText(_guestAccess));
    }
    if (_historyVisibilityButton) {
        static_cast<CreateRoomValueButton*>(_historyVisibilityButton)->setValue(
            historyVisibilityText(_historyVisibility));
    }
}

void DialogsRoomCreateDialog::showError(const QString &message) {
    if (_errorLabel) {
        _errorLabel->setText(message);
    }
}

void DialogsRoomCreateDialog::setControlsEnabled(bool enabled) {
    _controlsEnabled = enabled;
    if (_nameField) _nameField->setEnabled(enabled);
    if (_topicField) _topicField->setEnabled(enabled);
    if (_aliasField) _aliasField->setEnabled(enabled);
    if (_publicCheck) _publicCheck->setEnabled(enabled);
    if (_encryptedCheck) _encryptedCheck->setEnabled(enabled);
    if (_federationCheck) _federationCheck->setEnabled(enabled);
    if (_historyVisibilityButton) _historyVisibilityButton->setEnabled(enabled);
    updateVisibilityFields();
    if (_create) {
        const bool createEnabled = enabled && !roomName().isEmpty();
        _create->setEnabled(createEnabled);
        _create->setButtonStyle(createButtonStyle(createEnabled));
        _create->setFixedHeight(st::boxButtonHeight);
    }
    if (_cancel) _cancel->setEnabled(enabled);
    if (_closeButton) _closeButton->setEnabled(enabled);
}

bool DialogsRoomCreateDialog::controlsEnabled() const {
    return _controlsEnabled;
}

void DialogsRoomCreateDialog::updateCreateButton() {
    const bool hasText = _nameField
        && !_nameField->text().trimmed().isEmpty();
    if (_create) {
        const bool enabled = _controlsEnabled && hasText;
        _create->setEnabled(enabled);
        _create->setButtonStyle(createButtonStyle(enabled));
        _create->setFixedHeight(st::boxButtonHeight);
    }
    // Hide error when user types.
    if (_controlsEnabled && _errorLabel && !_errorLabel->text().isEmpty()) {
        _errorLabel->clear();
    }
}

// -----------------------------------------------
// Paint
// -----------------------------------------------

void DialogsRoomCreateDialog::paintEvent(QPaintEvent *) {
    QPainter p(this);

    // Dim the backdrop behind the box.
    p.setOpacity(_bgOpacity);
    p.fillRect(rect(), st::layerBg);

    // Fade in the box's round shadow independently of the backdrop.
    if (_panel && _layerOpacity > 0) {
        p.setOpacity(_layerOpacity);
        paintBoxShadow(p, _panel->geometry());
    }
}

// -----------------------------------------------
// Input handling
// -----------------------------------------------

void DialogsRoomCreateDialog::mousePressEvent(QMouseEvent *event) {
    if (_panel && !_panel->geometry().contains(event->pos())) {
        if (_controlsEnabled) {
            reject();
        }
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void DialogsRoomCreateDialog::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        if (!_controlsEnabled) {
            return;
        }
        reject();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool DialogsRoomCreateDialog::eventFilter(QObject *obj, QEvent *event) {
    if (obj == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "dialogs_room_info_box.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include "dialogs/saved_messages.h"
#include "protocol/media_cache.h"
#include "protocol/protocol_bridge.h"
#include "styles/style_constants.h"
#include "ui/empty_userpic.h"
#include "ui/style/icon_provider.h"
#include "ui/toast_widget.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/close_button.h"
#include "ui/widgets/scroll_area.h"

namespace TeleMatrix {

namespace {

constexpr int kAvatarSize = 84;
constexpr int kPanelWidth = 380;
constexpr int kTopicMaxHeight = 220;

// lupdate only extracts literal QCoreApplication::translate("Context", "literal") calls, so every
// user-facing string below is written out in full rather than routed through a helper.

/// The one-word summary of how a stranger gets in. Public rooms show nothing (you're browsing the
/// public directory — it's implied).
QString joinRuleNote(const RoomPreviewInfo &info) {
    switch (info.joinRule) {
    case RoomDirectoryJoinRule::Knock:
    case RoomDirectoryJoinRule::KnockRestricted:
        return QCoreApplication::translate("DialogsRoomInfoBox", "Ask to join");
    case RoomDirectoryJoinRule::Invite:
        return QCoreApplication::translate("DialogsRoomInfoBox", "Invite only");
    case RoomDirectoryJoinRule::Restricted:
        return QCoreApplication::translate("DialogsRoomInfoBox", "Restricted");
    default:
        return QString();
    }
}

/// The card panel with rounded corners (matches the other dialog boxes).
class RoundedPanel final : public QWidget {
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

/// Draws the room avatar (resolved async) or a lettered fallback.
class RoomInfoAvatar final : public QWidget {
public:
    RoomInfoAvatar(QString roomId, QString name, QString avatarUrl, QWidget *parent)
    : QWidget(parent)
    , _roomId(std::move(roomId))
    , _name(std::move(name))
    , _avatarUrl(std::move(avatarUrl)) {
        setFixedSize(kAvatarSize, kAvatarSize);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        const QRect r(0, 0, kAvatarSize, kAvatarSize);
        if (!_avatarUrl.isEmpty()) {
            const auto dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
            const auto avatar = MediaCache::loadAvatarPixmapAsync(
                _avatarUrl, kAvatarSize, dpr, this, r);
            if (!avatar.isNull()) {
                p.drawPixmap(r.topLeft(), avatar);
                return;
            }
        }
        Ui::EmptyUserpic::paint(p, _roomId, _name, r.x(), r.y(), r.width());
    }

private:
    QString _roomId;
    QString _name;
    QString _avatarUrl;
};

// The photo-sized disc crowds the title on this shorter card.
constexpr int kSavedAvatarSize = 64;

/// The drawn bookmark userpic — same art as the rooms list and top bar.
class SavedMessagesAvatar final : public QWidget {
public:
    explicit SavedMessagesAvatar(QWidget *parent) : QWidget(parent) {
        setFixedSize(kSavedAvatarSize, kSavedAvatarSize);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        Ui::EmptyUserpic::paintSavedMessages(p, 0, 0, kSavedAvatarSize);
    }
};

QLabel *makeCenteredLabel(const QFont &font, const QColor &color, QWidget *parent) {
    auto *label = new QLabel(parent);
    label->setFont(font);
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    QPalette pal = label->palette();
    pal.setColor(QPalette::WindowText, color);
    label->setPalette(pal);
    return label;
}

// The room address with a copy glyph beside it, painted as one unit. A QLabel +
// QHBoxLayout can't align the two: the label centres its glyphs by layout cell,
// which drifts from the geometric centre by the font's leading and leaves the
// AlignVCenter icon reading high. So text and glyph are drawn here on the same
// fontMetrics().height() band — identical to the user-id row in UserProfilePopup.
// Only the glyph is the hover/click target, matching that row.
class AliasCopyRow final : public QWidget {
public:
    AliasCopyRow(QString alias, QWidget *parent)
    : QWidget(parent)
    , _alias(std::move(alias))
    , _font(st::userProfileStatusFont()) {
        setMouseTracking(true);
        setToolTip(_alias);
        _icon = Style::IconProvider::tintedIcon(
            QStringLiteral(":/telematrix/icons/chat/"),
            QStringLiteral("mini_copy"),
            st::userProfileStatusFg);
        _iconOver = Style::IconProvider::tintedIcon(
            QStringLiteral(":/telematrix/icons/chat/"),
            QStringLiteral("mini_copy"),
            st::windowActiveTextFg);
        const QFontMetrics fm(_font);
        const auto available = kPanelWidth
            - st::userProfileUserIdSideSkip
            - st::userProfileCopyIconSkip
            - st::userProfileCopyIconSize;
        _text = fm.elidedText(_alias, Qt::ElideRight, available);
        _textWidth = fm.horizontalAdvance(_text);
        setFixedSize(
            _textWidth + st::userProfileCopyIconSkip + st::userProfileCopyIconSize,
            fm.height());
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        p.setFont(_font);
        p.setPen(st::userProfileUserIdFg);
        p.drawText(
            QRect(0, 0, _textWidth, height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            _text);
        const auto &icon = _hovered ? _iconOver : _icon;
        if (!icon.isNull()) {
            p.drawImage(iconRect(), icon);
        }
    }
    void mousePressEvent(QMouseEvent *) override {} // accept so release is delivered
    void mouseMoveEvent(QMouseEvent *e) override {
        const auto over = iconRect().contains(e->pos());
        if (over != _hovered) {
            _hovered = over;
            setCursor(over ? Qt::PointingHandCursor : Qt::ArrowCursor);
            update();
        }
    }
    void leaveEvent(QEvent *) override {
        if (_hovered) {
            _hovered = false;
            update();
        }
    }
    void mouseReleaseEvent(QMouseEvent *e) override {
        if (!iconRect().contains(e->pos())) {
            return;
        }
        QGuiApplication::clipboard()->setText(_alias);
        ::Ui::ShowToast(QCoreApplication::translate(
            "DialogsRoomInfoBox", "Address copied to clipboard"));
    }

private:
    [[nodiscard]] QRect iconRect() const {
        const auto side = st::userProfileCopyIconSize;
        return QRect(
            _textWidth + st::userProfileCopyIconSkip,
            (height() - side) / 2,
            side,
            side);
    }

    QString _alias;
    QFont _font;
    QString _text;
    int _textWidth = 0;
    QImage _icon;
    QImage _iconOver;
    bool _hovered = false;
};

} // namespace

QVBoxLayout *DialogsRoomInfoBox::buildChrome() {
    if (parentWidget()) {
        setGeometry(parentWidget()->rect());
        parentWidget()->installEventFilter(this);
    }
    setFocusPolicy(Qt::StrongFocus);

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

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addStretch(1);

    _panel = new RoundedPanel(this);
    _panel->setVisible(false);
    _panel->setFixedWidth(kPanelWidth);
    root->addWidget(_panel, 0, Qt::AlignHCenter);
    root->addStretch(1);

    auto *layout = new QVBoxLayout(_panel);
    layout->setContentsMargins(24, 28, 24, 20);
    layout->setSpacing(0);

    // Shared top-right close (×): same dismissal as Escape / an outside click /
    // the bottom "Close" button. The card's content is centred, leaving the
    // top-right corner clear for it.
    auto *close = new ::Ui::CloseButton(_panel);
    close->move(kPanelWidth - st::settingsCloseButtonSize, 0);
    connect(close, &::Ui::CloseButton::clicked, this,
            [this] { closeAnimated(); });

    return layout;
}

DialogsRoomInfoBox::DialogsRoomInfoBox(
    const RoomPreviewInfo &info,
    ProtocolBridge *bridge,
    QWidget *parent)
: QWidget(parent ? parent->window() : nullptr)
, _info(info)
, _bridge(bridge) {
    auto *layout = buildChrome();

    auto *avatar = new RoomInfoAvatar(
        _info.roomId, _info.name, _info.avatarUrl, _panel);
    layout->addWidget(avatar, 0, Qt::AlignHCenter);
    // Ask the backend to fetch the avatar so the async paint has something to show.
    if (_bridge && !_info.avatarUrl.isEmpty()) {
        _bridge->resolveAvatar(_info.avatarUrl);
        connect(_bridge, &ProtocolBridge::mediaResolved, avatar,
                [avatar](bool ok, const QString &, const QString &) {
            if (ok) {
                avatar->update();
            }
        });
    }

    layout->addSpacing(14);

    auto nameFont = st::boxTitleFont;
    auto *name = makeCenteredLabel(nameFont, st::boxTitleFg, _panel);
    name->setText(_info.name);
    layout->addWidget(name);

    // Meta line: "N members" plus a join-rule note for the non-public cases.
    QStringList metaParts;
    metaParts << QCoreApplication::translate(
        "DialogsRoomInfoBox", "%n member(s)", nullptr, _info.memberCount);
    if (const auto note = joinRuleNote(_info); !note.isEmpty()) {
        metaParts << note;
    }
    layout->addSpacing(6);
    auto *meta = makeCenteredLabel(st::normalFont, st::windowSubTextFg, _panel);
    meta->setText(metaParts.join(QStringLiteral("  ·  ")));
    layout->addWidget(meta);

    if (!_info.canonicalAlias.isEmpty()) {
        layout->addSpacing(4);
        // The room address with a copy glyph beside it, centred as a unit —
        // same treatment as the user id in UserProfilePopup.
        layout->addWidget(
            new AliasCopyRow(_info.canonicalAlias, _panel),
            0,
            Qt::AlignHCenter);
    }

    const auto topic = _info.topic.trimmed();
    if (!topic.isEmpty()) {
        layout->addSpacing(16);

        auto *sep = new QWidget(_panel);
        sep->setFixedHeight(1);
        sep->setAutoFillBackground(true);
        QPalette sepPal = sep->palette();
        sepPal.setColor(QPalette::Window, st::shadowFg);
        sep->setPalette(sepPal);
        layout->addWidget(sep);

        layout->addSpacing(16);

        auto *topicLabel = new QLabel;
        topicLabel->setFont(st::normalFont);
        topicLabel->setWordWrap(true);
        topicLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        topicLabel->setText(topic);
        QPalette topicPal = topicLabel->palette();
        topicPal.setColor(QPalette::WindowText, st::windowFg);
        topicLabel->setPalette(topicPal);
        topicLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);

        // Cap the height; a very long topic scrolls rather than growing the card off-screen.
        auto *scroll = new ::Ui::ScrollArea(_panel);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setWidgetResizable(true);
        scroll->setWidget(topicLabel);
        scroll->setMaximumHeight(kTopicMaxHeight);
        scroll->setAutoFillBackground(false);
        if (auto *vp = scroll->viewport()) {
            vp->setAutoFillBackground(false);
        }
        layout->addWidget(scroll);
    }

    layout->addSpacing(22);

    ::Ui::TextButton::Style closeStyle;
    closeStyle.bgOver = &st::lightButtonBgOver;
    closeStyle.fg = &st::lightButtonFg;
    closeStyle.radius = st::boxRadius;
    closeStyle.height = st::boxButtonHeight;
    closeStyle.paddingH = 12;
    auto *close = new ::Ui::TextButton(
        QCoreApplication::translate("DialogsRoomInfoBox", "Close"), closeStyle, _panel);
    {
        auto f = st::baseFont(14);
        f.setWeight(QFont::DemiBold);
        close->setFont(f);
    }
    connect(close, &QAbstractButton::clicked, this, [this] { closeAnimated(); });
    layout->addWidget(close, 0, Qt::AlignHCenter);
}

DialogsRoomInfoBox *DialogsRoomInfoBox::forSavedMessages(
    const QString &roomId,
    bool encrypted,
    QWidget *parent) {
    return new DialogsRoomInfoBox(SavedMessagesTag{}, roomId, encrypted, parent);
}

DialogsRoomInfoBox::DialogsRoomInfoBox(
    SavedMessagesTag,
    const QString &roomId,
    bool encrypted,
    QWidget *parent)
: QWidget(parent ? parent->window() : nullptr) {
    auto *layout = buildChrome();

    layout->addWidget(new SavedMessagesAvatar(_panel), 0, Qt::AlignHCenter);

    layout->addSpacing(14);

    auto *name = makeCenteredLabel(st::boxTitleFont, st::boxTitleFg, _panel);
    name->setText(SavedMessages::displayName());
    layout->addWidget(name);

    // One property per line in the meta slot: encryption, access, room id
    // (the address row's copy glyph copies the raw id).
    layout->addSpacing(6);
    auto *encryption = makeCenteredLabel(
        st::normalFont, st::windowSubTextFg, _panel);
    encryption->setText(encrypted
        ? QCoreApplication::translate("SavedMessages", "End-to-end encrypted")
        : QCoreApplication::translate("SavedMessages", "Not encrypted"));
    layout->addWidget(encryption);

    layout->addSpacing(4);
    auto *access = makeCenteredLabel(
        st::normalFont, st::windowSubTextFg, _panel);
    access->setText(QCoreApplication::translate(
        "SavedMessages", "Private — visible only to you"));
    layout->addWidget(access);

    layout->addSpacing(4);
    layout->addWidget(new AliasCopyRow(roomId, _panel), 0, Qt::AlignHCenter);

    layout->addSpacing(22);

    ::Ui::TextButton::Style closeStyle;
    closeStyle.bgOver = &st::lightButtonBgOver;
    closeStyle.fg = &st::lightButtonFg;
    closeStyle.radius = st::boxRadius;
    closeStyle.height = st::boxButtonHeight;
    closeStyle.paddingH = 12;
    auto *close = new ::Ui::TextButton(
        QCoreApplication::translate("DialogsRoomInfoBox", "Close"), closeStyle, _panel);
    {
        auto f = st::baseFont(14);
        f.setWeight(QFont::DemiBold);
        close->setFont(f);
    }
    connect(close, &QAbstractButton::clicked, this, [this] { closeAnimated(); });
    layout->addWidget(close, 0, Qt::AlignHCenter);
}

void DialogsRoomInfoBox::showAnimated() {
    raise();
    show();
    setFocus();
    if (_a_shown) {
        _a_shown->start();
    }
    if (_a_layerShown) {
        _a_layerShown->start();
    }
}

void DialogsRoomInfoBox::closeAnimated() {
    if (_closing) {
        return;
    }
    _closing = true;
    if (parentWidget()) {
        parentWidget()->removeEventFilter(this);
    }
    deleteLater();
}

void DialogsRoomInfoBox::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setOpacity(_bgOpacity);
    p.fillRect(rect(), st::layerBg);
}

void DialogsRoomInfoBox::mousePressEvent(QMouseEvent *event) {
    if (_panel && !_panel->geometry().contains(event->pos())) {
        closeAnimated();
        return;
    }
    QWidget::mousePressEvent(event);
}

void DialogsRoomInfoBox::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        closeAnimated();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool DialogsRoomInfoBox::eventFilter(QObject *obj, QEvent *event) {
    if (obj == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace TeleMatrix

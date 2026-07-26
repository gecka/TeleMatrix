// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "room_settings_widget.h"
#include "../dialogs/dialogs_invite_users_box.h"
#include "app/app_controller.h"
#include "history/history_confirm_dialog.h"
#include "protocol/protocol_bridge.h"
#include "trust_shield.h"
#include "settings/account/account_edit_name_dialog.h"
#include "styles/style_constants.h"
#include "ui/style/icon_provider.h"
#include "settings/settings_common_widgets.h"
#include "ui/internal_choice_dialog.h"
#include "ui/painter.h"
#include "ui/empty_userpic.h"
#include "ui/toast_widget.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTextEdit>
#include <QPixmap>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

#include "protocol/media_cache.h"

namespace TeleMatrix {

namespace {

constexpr int kTopBarHeight = 56;
constexpr int kSidebarWidth = 180;
constexpr int kCoverHeight = 100;
constexpr int kCoverAvatarSize = 54;
constexpr int kCloseIconSize = 24;
constexpr int kCloseIconLeft = 4;
constexpr int kSidebarButtonHeight = 40;
constexpr int kSidebarButtonRadius = 8;
constexpr int kSidebarIconLeft = 16;
constexpr int kSidebarTextLeft = 48;
constexpr int kInfoRowHeight = 44;
constexpr int kActionRowHeight = 44;
constexpr int kMemberRowHeight = 48;
constexpr int kMemberListTopPadding = 8;
constexpr int kMemberAvatarSize = 36;
constexpr int kMembersPreloaderHeight = 72;
constexpr int kMembersPreloaderSize = 24;
constexpr int kMembersPreloaderTextSkip = 10;
constexpr int kSectionTitleHeight = 40;
constexpr int kDividerHeight = 8;
constexpr int kContentPadding = 22;
constexpr int kAdminBadgeRadius = 4;
constexpr int kAvatarDeleteButtonSize = 22;
constexpr int kAvatarDeleteButtonRadius = 4;
constexpr int kAvatarDeleteIconPad = 1;
constexpr int kAvatarSpinnerSize = 30;
constexpr int kFullArcLength = 360 * 16;
constexpr int kQuarterArcLength = 90 * 16;

void showWarningBox(QWidget *parent, const QString &title, const QString &text) {
    HistoryConfirmDialog dialog(
        parent,
        title,
        text,
        QCoreApplication::translate("RoomSettingsWidget", "OK"),
        QString(),
        HistoryConfirmDialog::Normal,
        0,
        -1,
        false);
    dialog.exec();
}

constexpr int kGeneralBodyHeight =
    kCoverHeight
    + kSectionTitleHeight
    + 3 * kInfoRowHeight
    + kSectionTitleHeight
    + 3 * kActionRowHeight
    + 16;

QString imageContentTypeForPath(const QString &path) {
    if (path.endsWith(QStringLiteral(".jpg"), Qt::CaseInsensitive)
        || path.endsWith(QStringLiteral(".jpeg"), Qt::CaseInsensitive)) {
        return QStringLiteral("image/jpeg");
    } else if (path.endsWith(QStringLiteral(".gif"), Qt::CaseInsensitive)) {
        return QStringLiteral("image/gif");
    } else if (path.endsWith(QStringLiteral(".webp"), Qt::CaseInsensitive)) {
        return QStringLiteral("image/webp");
    }
    return QStringLiteral("image/png");
}

qreal avatarSpinnerPhase() {
    const auto period = qMax(1, st::radialPeriod);
    return (QDateTime::currentMSecsSinceEpoch() % period) / qreal(period);
}

void paintAvatarPreloader(QPainter &p, const QRect &avatarRect) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addEllipse(avatarRect);
    p.setClipPath(clip);
    auto overlay = st::windowShadowFg;
    overlay.setAlpha(96);
    p.fillRect(avatarRect, overlay);
    p.restore();

    const QRect spinnerRect(
        avatarRect.center().x() - kAvatarSpinnerSize / 2,
        avatarRect.center().y() - kAvatarSpinnerSize / 2,
        kAvatarSpinnerSize,
        kAvatarSpinnerSize);
    const auto arcRect = QRectF(spinnerRect).adjusted(
        st::uploadRadialLine,
        st::uploadRadialLine,
        -st::uploadRadialLine,
        -st::uploadRadialLine);
    QPen pen(st::activeButtonFg);
    pen.setWidthF(st::uploadRadialLine);
    pen.setCapStyle(Qt::RoundCap);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawArc(
        arcRect,
        kQuarterArcLength - qRound(avatarSpinnerPhase() * kFullArcLength),
        -kFullArcLength / 4);
    p.restore();
}

/// Load a template icon and colorize it.
QImage loadColorizedIcon(const QString &name, const QColor &color) {
    return Style::IconProvider::tintedIcon(
        QStringLiteral(":/settings_icons/"), name, color);
}

/// Load menu icon from the history menu icons resource.
QImage loadMenuIcon(const QString &name, const QColor &color) {
    return Style::IconProvider::tintedIcon(
        QStringLiteral(":/telematrix/icons/menu/"), name, color);
}

void paintSendMediaDeleteButton(QPainter &p, const QRect &buttonRect, bool hovered) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(hovered
        ? st::sendMediaDeleteBgOver
        : st::sendMediaDeleteBg);
    p.drawRoundedRect(
        buttonRect,
        kAvatarDeleteButtonRadius,
        kAvatarDeleteButtonRadius);

    // Cross (X) remove icon, replacing the bin.
    QPen crossPen(st::historyIconFgInverted);
    crossPen.setWidthF(1.5);
    crossPen.setCapStyle(Qt::RoundCap);
    p.setPen(crossPen);
    const auto inset = buttonRect.width() * 0.3;
    const QRectF cr = QRectF(buttonRect).adjusted(inset, inset, -inset, -inset);
    p.drawLine(cr.topLeft(), cr.bottomRight());
    p.drawLine(cr.topRight(), cr.bottomLeft());
    p.restore();
}

QSize iconLogicalSize(const QImage &icon, int fallbackSize) {
    if (icon.isNull()) {
        return QSize(fallbackSize, fallbackSize);
    }
    const auto dpr = icon.devicePixelRatio();
    return QSize(
        qRound(icon.width() / dpr),
        qRound(icon.height() / dpr));
}

QString roomAccessText(RoomAccess access) {
    switch (access) {
    case RoomAccess::Public:
        return QCoreApplication::translate("TeleMatrix::RoomSettingsWidget", "Public");
    case RoomAccess::Knock:
        return QCoreApplication::translate("TeleMatrix::RoomSettingsWidget", "Knock");
    case RoomAccess::Restricted:
        return QCoreApplication::translate("TeleMatrix::RoomSettingsWidget", "Restricted");
    case RoomAccess::KnockRestricted:
        return QCoreApplication::translate("TeleMatrix::RoomSettingsWidget", "Restricted, can knock");
    case RoomAccess::Private:
    case RoomAccess::InviteOnly:
        return QCoreApplication::translate("TeleMatrix::RoomSettingsWidget", "Private");
    case RoomAccess::Unknown:
    default:
        return QCoreApplication::translate("TeleMatrix::RoomSettingsWidget", "Unknown");
    }
}

QString roomAccessId(RoomAccess access) {
    switch (access) {
    case RoomAccess::Public:
        return QStringLiteral("public");
    case RoomAccess::Knock:
        return QStringLiteral("knock");
    case RoomAccess::Restricted:
        return QStringLiteral("restricted");
    case RoomAccess::KnockRestricted:
        return QStringLiteral("knock_restricted");
    case RoomAccess::Private:
    case RoomAccess::InviteOnly:
        return QStringLiteral("private");
    case RoomAccess::Unknown:
    default:
        return QStringLiteral("unknown");
    }
}

RoomAccess roomAccessFromId(const QString &id) {
    if (id == QStringLiteral("public")) {
        return RoomAccess::Public;
    } else if (id == QStringLiteral("knock")) {
        return RoomAccess::Knock;
    } else if (id == QStringLiteral("private")) {
        return RoomAccess::InviteOnly;
    }
    return RoomAccess::Unknown;
}

QString historyVisibilityText(HistoryVisibility visibility) {
    switch (visibility) {
    case HistoryVisibility::Joined:
        return QCoreApplication::translate("TeleMatrix::RoomSettingsWidget", "Members only (since joining)");
    case HistoryVisibility::Invited:
        return QCoreApplication::translate("TeleMatrix::RoomSettingsWidget", "Members only (since invited)");
    case HistoryVisibility::Shared:
        return QCoreApplication::translate("TeleMatrix::RoomSettingsWidget", "Members only (full history)");
    case HistoryVisibility::WorldReadable:
        return QCoreApplication::translate("TeleMatrix::RoomSettingsWidget", "Anyone (world-readable)");
    case HistoryVisibility::Unknown:
    default:
        return QCoreApplication::translate("TeleMatrix::RoomSettingsWidget", "Unknown");
    }
}

QString historyVisibilityId(HistoryVisibility visibility) {
    switch (visibility) {
    case HistoryVisibility::Joined:
        return QStringLiteral("joined");
    case HistoryVisibility::Invited:
        return QStringLiteral("invited");
    case HistoryVisibility::Shared:
        return QStringLiteral("shared");
    case HistoryVisibility::WorldReadable:
        return QStringLiteral("world_readable");
    case HistoryVisibility::Unknown:
    default:
        return QStringLiteral("unknown");
    }
}

HistoryVisibility historyVisibilityFromId(const QString &id) {
    if (id == QStringLiteral("joined")) {
        return HistoryVisibility::Joined;
    } else if (id == QStringLiteral("invited")) {
        return HistoryVisibility::Invited;
    } else if (id == QStringLiteral("shared")) {
        return HistoryVisibility::Shared;
    } else if (id == QStringLiteral("world_readable")) {
        return HistoryVisibility::WorldReadable;
    }
    return HistoryVisibility::Unknown;
}

// Transparent icon button with a hover-only rounded background, painted with
// live st:: colors so it tracks theme changes (a stylesheet built from
// `.name()` would freeze the colors).
class CopyIconButton : public QPushButton {
public:
    explicit CopyIconButton(QWidget *parent) : QPushButton(parent) {
        setCursor(Qt::PointingHandCursor);
        setFixedSize(34, 34);
        setMouseTracking(true);
        _icon = loadMenuIcon(QStringLiteral("copy"), st::lightButtonFg);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        if (_hovered) {
            p.setPen(Qt::NoPen);
            p.setBrush(st::lightButtonBgOver);
            p.drawRoundedRect(rect(), st::boxRadius, st::boxRadius);
        }
        if (!_icon.isNull()) {
            const int w = 18, h = 18;
            const QRect target((width() - w) / 2, (height() - h) / 2, w, h);
            p.drawImage(target, _icon);
        }
    }
    void enterEvent(QEnterEvent *) override { _hovered = true; update(); }
    void leaveEvent(QEvent *) override { _hovered = false; update(); }

private:
    QImage _icon;
    bool _hovered = false;
};

QPushButton *createCopyIconButton(QWidget *parent) {
    auto *button = new CopyIconButton(parent);
    button->setToolTip(QCoreApplication::translate(
        "TeleMatrix::RoomSettingsWidget",
        "Copy to clipboard"));
    return button;
}

/// Icon-only "add member" button (menu glyph) for the members search field.
class AddMemberIconButton : public QPushButton {
public:
    explicit AddMemberIconButton(QWidget *parent) : QPushButton(parent) {
        setCursor(Qt::PointingHandCursor);
        setFixedSize(28, 28);
        setMouseTracking(true);
        _icon = loadMenuIcon(QStringLiteral("add_member"), st::windowActiveTextFg);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        if (_hovered) {
            p.setPen(Qt::NoPen);
            p.setBrush(st::windowBgOver);
            p.drawRoundedRect(rect(), st::boxRadius, st::boxRadius);
        }
        if (!_icon.isNull()) {
            const int w = 20, h = 20;
            const QRect target((width() - w) / 2, (height() - h) / 2, w, h);
            p.drawImage(target, _icon);
        }
    }
    void enterEvent(QEnterEvent *) override { _hovered = true; update(); }
    void leaveEvent(QEvent *) override { _hovered = false; update(); }

private:
    QImage _icon;
    bool _hovered = false;
};

/// Custom-painted sidebar button for room settings.
class RoomSettingsSidebarButton : public QWidget {
public:
    RoomSettingsSidebarButton(const QString &iconName, const QString &text, QWidget *parent = nullptr)
        : QWidget(parent)
        , _text(text)
    {
        _icon = loadMenuIcon(iconName, st::settingsMenuIconFg);
        _iconOver = loadMenuIcon(iconName, st::settingsMenuIconFgOver);
        setFixedHeight(kSidebarButtonHeight);
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
    }

    void setSelected(bool selected) {
        if (_selected != selected) {
            _selected = selected;
            update();
        }
    }
    [[nodiscard]] bool isSelected() const { return _selected; }

    std::function<void()> onClick;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        if (_selected) {
            const int margin = 4;
            const QRectF bg(margin, 2, width() - 2 * margin, height() - 4);
            p.setPen(Qt::NoPen);
            p.setBrush(st::settingsSidebarSelectedBg);
            p.drawRoundedRect(bg, kSidebarButtonRadius, kSidebarButtonRadius);
        } else if (_hovered) {
            const int margin = 4;
            const QRectF bg(margin, 2, width() - 2 * margin, height() - 4);
            p.setPen(Qt::NoPen);
            p.setBrush(st::settingsSidebarBgOver);
            p.drawRoundedRect(bg, kSidebarButtonRadius, kSidebarButtonRadius);
        }

        const auto &img = (_selected || _hovered) ? _iconOver : _icon;
        if (!img.isNull()) {
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            const int iconH = qRound(img.height() / img.devicePixelRatio());
            const int iconY = (height() - iconH) / 2;
            p.drawImage(kSidebarIconLeft, iconY, img);
        }

        p.setFont(st::baseFont(14));
        p.setPen(st::settingsCheckboxTextFg);
        const QRect textRect(kSidebarTextLeft, 0, width() - kSidebarTextLeft - 8, height());
        p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, _text);
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton && onClick) {
            onClick();
        }
    }
    void enterEvent(QEnterEvent *) override { _hovered = true; update(); }
    void leaveEvent(QEvent *) override { _hovered = false; update(); }

private:
    QString _text;
    QImage _icon;
    QImage _iconOver;
    bool _hovered = false;
    bool _selected = false;
};

// A QLabel that elides its text with "…" to its own width, re-eliding on resize —
// so a long value in a stretched cell is truncated with an ellipsis rather than
// hard-clipped at the edge.
class ElidingLabel : public QLabel {
public:
    using QLabel::QLabel;

    void setFullText(const QString &text) {
        _full = text;
        applyElide();
    }

protected:
    void resizeEvent(QResizeEvent *e) override {
        QLabel::resizeEvent(e);
        applyElide();
    }

private:
    void applyElide() {
        const QFontMetrics fm(font());
        QLabel::setText(_full.isEmpty()
            ? _full
            : fm.elidedText(_full, Qt::ElideRight, qMax(1, width())));
    }

    QString _full;
};

class InfoValueRowWidget : public QWidget {
public:
    InfoValueRowWidget(const QString &label, bool clickable, QWidget *parent = nullptr)
        : QWidget(parent)
        , _clickable(clickable) {
        setFixedHeight(kInfoRowHeight);
        setMouseTracking(clickable);
        setCursor(clickable ? Qt::PointingHandCursor : Qt::ArrowCursor);

        auto *rowLayout = new QHBoxLayout(this);
        rowLayout->setContentsMargins(kContentPadding, 0, kContentPadding, 0);

        auto *labelWidget = new QLabel(label, this);
        labelWidget->setFont(st::baseFont(14));
        QPalette labelPal = labelWidget->palette();
        labelPal.setColor(QPalette::WindowText, st::windowSubTextFg);
        labelWidget->setPalette(labelPal);
        rowLayout->addWidget(labelWidget);

        _value = new ElidingLabel(this);
        _value->setFont(st::baseFont(14));
        _value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        QPalette valuePal = _value->palette();
        valuePal.setColor(
            QPalette::WindowText,
            clickable ? st::windowActiveTextFg : st::windowBoldFg);
        _value->setPalette(valuePal);
        rowLayout->addWidget(_value, 1);
    }

    QLabel *valueLabel() const {
        return _value;
    }

    // Set a value that elides with "…" to the cell width (re-elides on resize).
    void setElidedValue(const QString &text) {
        _value->setFullText(text);
    }

    void setClickable(bool clickable) {
        _clickable = clickable;
        setMouseTracking(clickable);
        setCursor(clickable ? Qt::PointingHandCursor : Qt::ArrowCursor);
        QPalette valuePal = _value->palette();
        valuePal.setColor(
            QPalette::WindowText,
            clickable ? st::windowActiveTextFg : st::windowBoldFg);
        _value->setPalette(valuePal);
        if (!clickable && _hovered) {
            _hovered = false;
            update();
        }
    }

    std::function<void()> onClick;

protected:
    void paintEvent(QPaintEvent *) override {
        if (!_hovered) {
            return;
        }
        QPainter p(this);
        p.fillRect(rect(), st::windowBgOver);
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (_clickable && e->button() == Qt::LeftButton && onClick) {
            onClick();
            return;
        }
        QWidget::mousePressEvent(e);
    }

    void enterEvent(QEnterEvent *) override {
        if (_clickable) {
            _hovered = true;
            update();
        }
    }

    void leaveEvent(QEvent *) override {
        if (_hovered) {
            _hovered = false;
            update();
        }
    }

private:
    ElidingLabel *_value = nullptr;
    bool _clickable = false;
    bool _hovered = false;
};

// Multi-line topic field, styled like the settings keyword box: a rounded themed
// frame around a frameless, transparent text area, a fixed 3 lines tall (scrolls
// for longer text). Editable INLINE (no popup), committing on focus-out;
// read-only otherwise.
class TopicDisplay final : public QWidget {
public:
    explicit TopicDisplay(QWidget *parent = nullptr) : QWidget(parent) {
        _edit = new QTextEdit(this);
        _edit->setReadOnly(true);
        _edit->setAcceptRichText(false);
        _edit->setFrameShape(QFrame::NoFrame);
        _edit->setLineWrapMode(QTextEdit::WidgetWidth);
        _edit->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        _edit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        _edit->document()->setDocumentMargin(0);
        auto f = _edit->font();
        f.setPixelSize(14);
        _edit->setFont(f);
        QPalette pal = _edit->palette();
        pal.setColor(QPalette::Base, Qt::transparent);
        pal.setColor(QPalette::Text, st::windowFg);
        pal.setColor(QPalette::Highlight, st::windowBgActive);
        _edit->setPalette(pal);
        _edit->viewport()->setAutoFillBackground(false);
        _edit->installEventFilter(this);

        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(8, 6, 8, 6);
        lay->setSpacing(0);
        lay->addWidget(_edit);
        const QFontMetrics fm(_edit->font());
        setFixedHeight(fm.lineSpacing() * 3 + 14);
    }

    // Don't clobber what the user is currently typing.
    void setText(const QString &text) {
        if (!_edit->hasFocus() && _edit->toPlainText() != text) {
            _edit->setPlainText(text);
        }
    }

    void setPlaceholder(const QString &text) {
        _edit->setPlaceholderText(text);
    }

    void setEditable(bool editable) {
        _editable = editable;
        _edit->setReadOnly(!editable);
    }

    // Called with the edited text when the field loses focus (inline commit).
    std::function<void(const QString &)> onCommit;

protected:
    bool eventFilter(QObject *o, QEvent *e) override {
        if (o == _edit) {
            if (e->type() == QEvent::FocusIn) {
                _focused = true;
                update();
            } else if (e->type() == QEvent::FocusOut) {
                _focused = false;
                update();
                if (_editable && onCommit) {
                    onCommit(_edit->toPlainText());
                }
            }
        }
        return QWidget::eventFilter(o, e);
    }

    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        const auto r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(_focused ? st::activeLineFg : st::inputBorderFg);
        p.setBrush(st::boxBg);
        p.drawRoundedRect(r, 4, 4);
    }

private:
    QTextEdit *_edit = nullptr;
    bool _editable = false;
    bool _focused = false;
};

/// Custom-painted member row widget.
// Small magnifier glyph for the members search field (matches the app's other
// search inputs, which draw the icon rather than relying on native styling).
class MembersSearchIcon final : public QWidget {
public:
    explicit MembersSearchIcon(QWidget *parent = nullptr) : QWidget(parent) {
        setFixedSize(20, 20);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(st::windowSubTextFg, 1.5, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(7.5, 7.5), 5.0, 5.0);
        p.drawLine(QPointF(11.0, 11.0), QPointF(15.0, 15.0));
    }
};

// Paint a single member row inside `rowRect`. Shared by the virtualized list so
// only visible rows cost anything. Avatars load lazily per painted row via
// MediaCache (self-caching + repaint-on-ready), so a huge room only ever fetches
// avatars for the rows actually on screen.
void paintMemberRow(
        QPainter &p,
        const QRect &rowRect,
        const RoomMemberInfo &profile,
        bool hovered,
        QWidget *host) {
    if (hovered) {
        p.fillRect(rowRect, st::windowBgOver);
    }

    const int left = rowRect.left() + kContentPadding;
    const int avatarY = rowRect.top() + (rowRect.height() - kMemberAvatarSize) / 2;
    const QRect avatarRect(left, avatarY, kMemberAvatarSize, kMemberAvatarSize);

    bool paintedAvatar = false;
    if (!profile.avatarUrl.isEmpty()) {
        const auto dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
        const auto avatar = MediaCache::loadAvatarPixmapAsync(
            profile.avatarUrl, kMemberAvatarSize, dpr, host, avatarRect);
        if (!avatar.isNull()) {
            p.drawPixmap(avatarRect.topLeft(), avatar);
            paintedAvatar = true;
        }
    }
    if (!paintedAvatar) {
        ::Ui::EmptyUserpic::paint(
            p, profile.userId, profile.displayName, left, avatarY, kMemberAvatarSize);
    }

    const int textLeft = left + kMemberAvatarSize + 12;
    p.setFont(st::baseFont(14, true));
    p.setPen(st::dialogsNameFg);
    const int nameY = rowRect.top() + rowRect.height() / 2 - 2;
    const auto nameText = profile.displayName.isEmpty()
        ? profile.userId
        : profile.displayName;
    // Inline badges after the name: role (invited/admin) then cross-signing
    // trust ("verified" / "unverified sessions" / "identity changed").
    // "invited" takes priority over "admin": a pending invitee hasn't joined
    // yet, so surface that even if they were pre-assigned power.
    struct RowBadge { QString text; QColor bg; };
    QVector<RowBadge> badges;
    if (profile.membership == MembershipState::Invite) {
        badges.push_back({
            QCoreApplication::translate("TeleMatrix::RoomSettingsWidget", "invited"),
            st::windowSubTextFg});
    } else if (profile.role == MemberRole::Administrator) {
        badges.push_back({
            QCoreApplication::translate("TeleMatrix::RoomSettingsWidget", "admin"),
            st::windowActiveTextFg});
    }
    if (const auto trustText = trustBadgeText(profile.trustState); !trustText.isEmpty()) {
        badges.push_back({trustText, trustBadgeColor(profile.trustState)});
    }

    const QFontMetrics badgeFm(st::baseFont(10, true));
    constexpr int kBadgeGap = 8;
    constexpr int kBadgePadX = 10;
    int badgesReserve = 0;
    for (const auto &b : badges) {
        badgesReserve += kBadgeGap + badgeFm.horizontalAdvance(b.text) + kBadgePadX;
    }
    const int maxNameWidth = qMax(
        0,
        rowRect.right() + 1 - textLeft - kContentPadding - badgesReserve);
    const auto elidedName = p.fontMetrics().elidedText(
        nameText,
        Qt::ElideRight,
        maxNameWidth);
    p.drawText(textLeft, nameY, elidedName);

    if (!badges.isEmpty()) {
        const int badgeH = 16;
        // Vertically center on the name text's line box (name font still active).
        const int nameTop = nameY - p.fontMetrics().ascent();
        const int badgeY = nameTop + (p.fontMetrics().height() - badgeH) / 2;
        int badgeX = textLeft + p.fontMetrics().horizontalAdvance(elidedName) + kBadgeGap;
        p.setFont(st::baseFont(10, true));
        for (const auto &b : badges) {
            const int bw = badgeFm.horizontalAdvance(b.text) + kBadgePadX;
            const QRect badgeRect(badgeX, badgeY, bw, badgeH);
            p.setPen(Qt::NoPen);
            p.setBrush(b.bg);
            p.drawRoundedRect(badgeRect, kAdminBadgeRadius, kAdminBadgeRadius);
            p.setPen(st::activeButtonFg);
            p.drawText(badgeRect, Qt::AlignCenter, b.text);
            badgeX += bw + kBadgeGap;
        }
    }

    // User ID below name.
    p.setFont(st::baseFont(12));
    p.setPen(st::windowSubTextFg);
    p.drawText(textLeft, nameY + 16, profile.userId);
}

class MembersPreloaderWidget : public QWidget {
public:
    explicit MembersPreloaderWidget(const QString &text, QWidget *parent = nullptr)
        : QWidget(parent)
        , _text(text) {
        setFixedHeight(kMembersPreloaderHeight);
        _timer.setInterval(33);
        connect(&_timer, &QTimer::timeout, this, [this] {
            update();
        });
        // Timer runs only while visible (start/stop in show/hideEvent). The preloader is
        // created once and toggled with setVisible, so starting it in the ctor left a
        // 30fps timer waking the event loop for the whole lifetime of the Room Settings
        // panel, even after members loaded and it was hidden. See PERF-11.
    }

protected:
    void showEvent(QShowEvent *e) override {
        QWidget::showEvent(e);
        _timer.start();
    }

    void hideEvent(QHideEvent *e) override {
        QWidget::hideEvent(e);
        _timer.stop();
    }

    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        p.setFont(st::baseFont(13));
        const auto metrics = p.fontMetrics();
        const int textWidth = metrics.horizontalAdvance(_text);
        const int totalWidth = kMembersPreloaderSize + kMembersPreloaderTextSkip + textWidth;
        const int left = qMax(0, (width() - totalWidth) / 2);
        const int centerY = height() / 2;
        const QRect spinnerRect(
            left,
            centerY - kMembersPreloaderSize / 2,
            kMembersPreloaderSize,
            kMembersPreloaderSize);
        const auto arcRect = QRectF(spinnerRect).adjusted(
            st::uploadRadialLine,
            st::uploadRadialLine,
            -st::uploadRadialLine,
            -st::uploadRadialLine);

        QPen pen(st::windowActiveTextFg);
        pen.setWidthF(st::uploadRadialLine);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawArc(
            arcRect,
            kQuarterArcLength - qRound(avatarSpinnerPhase() * kFullArcLength),
            -kFullArcLength / 4);

        p.setFont(st::baseFont(13));
        p.setPen(st::windowSubTextFg);
        p.drawText(
            QRect(
                spinnerRect.right() + 1 + kMembersPreloaderTextSkip,
                centerY - metrics.height() / 2,
                textWidth,
                metrics.height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            _text);
    }

private:
    QString _text;
    QTimer _timer;
};

/// Room cover widget showing avatar, name, and member count.
// Inline room-name field embedded in the cover: bold, transparent, frameless,
// with an accent underline while it's being edited (editable + focused).
class CoverNameField final : public QLineEdit {
public:
    explicit CoverNameField(QWidget *parent) : QLineEdit(parent) {
        setFrame(false);
        setAttribute(Qt::WA_MacShowFocusRect, false);
        setFont(st::baseFont(16, true));
        setTextMargins(0, 0, 0, 0);
        setReadOnly(true);
        QPalette pal = palette();
        pal.setColor(QPalette::Base, Qt::transparent);
        pal.setColor(QPalette::Text, st::windowBoldFg);
        pal.setColor(QPalette::Highlight, st::windowBgActive);
        setPalette(pal);
    }

protected:
    void paintEvent(QPaintEvent *e) override {
        QLineEdit::paintEvent(e);
        if (!isReadOnly() && hasFocus()) {
            QPainter p(this);
            p.fillRect(0, height() - 2, width(), 2, st::activeLineFg);
        }
    }
};

class RoomCoverWidget : public QWidget {
public:
    RoomCoverWidget(const RoomSummary &summary, QWidget *parent = nullptr)
        : QWidget(parent)
        , _summary(summary)
    {
        setFixedHeight(kCoverHeight);
        setMouseTracking(true);
        _operationTimer.setInterval(33);
        connect(&_operationTimer, &QTimer::timeout, this, [this] {
            update(avatarRect().adjusted(-4, -4, 4, 4));
        });

        _nameField = new CoverNameField(this);
        _nameField->setText(_summary.displayName);
        // Enter or focus-out commits the (changed) name inline.
        connect(_nameField, &QLineEdit::editingFinished, this, [this] {
            if (_canEditName && onNameCommit) {
                onNameCommit(_nameField->text());
            }
        });
        positionNameField();
    }

    void setAvatarUrl(const QString &avatarUrl) {
        if (_summary.avatarUrl != avatarUrl) {
            _summary.avatarUrl = avatarUrl;
            update();
        }
    }

    void setDisplayName(const QString &name) {
        if (_summary.displayName != name) {
            _summary.displayName = name;
            if (_nameField && !_nameField->hasFocus()) {
                _nameField->setText(name);
            }
            update();
        }
    }

    void setCanEditAvatar(bool canEdit) {
        if (_canEditAvatar == canEdit) {
            return;
        }
        _canEditAvatar = canEdit;
        if (!_canEditAvatar && (_avatarHovered || _deleteHovered)) {
            _avatarHovered = false;
            _deleteHovered = false;
            setCursor(Qt::ArrowCursor);
        }
        update();
    }

    void setCanEditName(bool canEdit) {
        if (_canEditName == canEdit) {
            return;
        }
        _canEditName = canEdit;
        if (_nameField) {
            _nameField->setReadOnly(!canEdit);
        }
        update();
    }

    void setAvatarOperationInFlight(bool inFlight) {
        if (_avatarOperationInFlight == inFlight) {
            return;
        }
        _avatarOperationInFlight = inFlight;
        if (_avatarOperationInFlight) {
            _avatarHovered = false;
            _deleteHovered = false;
            setCursor(Qt::ArrowCursor);
            _operationTimer.start();
        } else {
            _operationTimer.stop();
        }
        update();
    }

    std::function<void()> onAvatarClick;
    std::function<void()> onAvatarDeleteClick;
    std::function<void(const QString &)> onNameCommit;

protected:
    void resizeEvent(QResizeEvent *e) override {
        QWidget::resizeEvent(e);
        positionNameField();
    }

    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), st::windowBg);

        const int coverLeft = kContentPadding;
        const int avatarY = (height() - kCoverAvatarSize) / 2;

	bool paintedAvatar = false;
	if (!_summary.avatarUrl.isEmpty()) {
		const QRect avatarRect(coverLeft, avatarY, kCoverAvatarSize, kCoverAvatarSize);
		const auto dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
		const auto avatar = MediaCache::loadAvatarPixmapAsync(
			_summary.avatarUrl,
			kCoverAvatarSize,
			dpr,
			this,
			avatarRect);
		if (!avatar.isNull()) {
			p.drawPixmap(avatarRect.topLeft(), avatar);
			paintedAvatar = true;
		}
	}
        if (!paintedAvatar) {
            ::Ui::EmptyUserpic::paint(
                p,
                _summary.avatarEntityId.isEmpty() ? _summary.roomId : _summary.avatarEntityId,
                _summary.displayName,
                coverLeft,
                avatarY,
                kCoverAvatarSize);
        }

        const auto currentAvatarRect = avatarRect();
        if (_avatarOperationInFlight) {
            paintAvatarPreloader(p, currentAvatarRect);
        }

        if (_canEditAvatar
            && !_avatarOperationInFlight
            && !_summary.avatarUrl.isEmpty()
            && (_avatarHovered || _deleteHovered)) {
            paintSendMediaDeleteButton(p, deleteButtonRect(), _deleteHovered);
        }

        // The name is an inline editable field (positioned in positionNameField,
        // vertically centred with the avatar). Nothing else in the cover.
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        const auto deleteHovered = _canEditAvatar
            && !_avatarOperationInFlight
            && !_summary.avatarUrl.isEmpty()
            && deleteButtonRect().contains(e->pos());
        const auto editHovered = _canEditAvatar
            && !_avatarOperationInFlight
            && !deleteHovered
            && avatarRect().contains(e->pos());
        if (editHovered == _avatarHovered && deleteHovered == _deleteHovered) {
            return;
        }
        _avatarHovered = editHovered;
        _deleteHovered = deleteHovered;
        setCursor((editHovered || deleteHovered)
            ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }

    void leaveEvent(QEvent *) override {
        if (!_avatarHovered && !_deleteHovered) {
            return;
        }
        _avatarHovered = false;
        _deleteHovered = false;
        setCursor(Qt::ArrowCursor);
        update();
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (_canEditAvatar
            && !_avatarOperationInFlight
            && e->button() == Qt::LeftButton
            && deleteButtonRect().contains(e->pos())
            && !_summary.avatarUrl.isEmpty()) {
            if (onAvatarDeleteClick) {
                onAvatarDeleteClick();
            }
            return;
        }
        if (_canEditAvatar
            && !_avatarOperationInFlight
            && e->button() == Qt::LeftButton
            && avatarRect().contains(e->pos())) {
            if (onAvatarClick) {
                onAvatarClick();
            }
            return;
        }
        QWidget::mousePressEvent(e);
    }

private:
    QRect avatarRect() const {
        return QRect(
            kContentPadding,
            (height() - kCoverAvatarSize) / 2,
            kCoverAvatarSize,
            kCoverAvatarSize);
    }

    QRect deleteButtonRect() const {
        const auto rect = avatarRect();
        return QRect(
            rect.right() - kAvatarDeleteButtonSize + 3,
            rect.top() - 3,
            kAvatarDeleteButtonSize,
            kAvatarDeleteButtonSize);
    }

    // Place the inline name field to the right of the avatar, vertically centred
    // on the avatar (the avatar itself is centred in the cover height).
    void positionNameField() {
        if (!_nameField) {
            return;
        }
        const int textLeft = kContentPadding + kCoverAvatarSize + 14;
        const int nameW = qMax(0, width() - textLeft - kContentPadding);
        constexpr int kNameFieldHeight = 26;
        const int top = (height() - kNameFieldHeight) / 2;
        _nameField->setGeometry(textLeft, top, nameW, kNameFieldHeight);
    }

    RoomSummary _summary;
    QTimer _operationTimer;
    CoverNameField *_nameField = nullptr;
    bool _canEditAvatar = false;
    bool _avatarOperationInFlight = false;
    bool _avatarHovered = false;
    bool _deleteHovered = false;
    bool _canEditName = false;
};

/// Action row widget with icon and text.
class ActionRowWidget : public QWidget {
public:
    ActionRowWidget(const QString &text, const QString &iconName,
                     bool attention, QWidget *parent = nullptr)
        : QWidget(parent)
        , _text(text)
        , _attention(attention)
    {
        _icon = loadMenuIcon(iconName, attention ? st::attentionButtonFg : st::menuIconFg);
        _iconOver = loadMenuIcon(iconName, attention ? st::attentionButtonFgOver : st::menuIconFgOver);
        setFixedHeight(kActionRowHeight);
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
    }

    std::function<void()> onClick;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        if (_hovered) {
            p.fillRect(rect(), st::windowBgOver);
        }

        const int left = kContentPadding;
        const auto &img = _hovered ? _iconOver : _icon;
        if (!img.isNull()) {
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            const int iconH = qRound(img.height() / img.devicePixelRatio());
            const int iconY = (height() - iconH) / 2;
            p.drawImage(left, iconY, img);
        }

        p.setFont(st::baseFont(14));
        p.setPen(_attention ? st::attentionButtonFg : st::windowBoldFg);
        const QRect textRect(left + 36, 0, width() - left - 36 - kContentPadding, height());
        p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, _text);
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton && onClick) {
            onClick();
        }
    }
    void enterEvent(QEnterEvent *) override { _hovered = true; update(); }
    void leaveEvent(QEvent *) override { _hovered = false; update(); }

private:
    QString _text;
    QImage _icon;
    QImage _iconOver;
    bool _attention = false;
    bool _hovered = false;
};

} // namespace

// Virtualized, filterable members list: a single painted widget that draws only
// the rows overlapping the paint clip-rect, so cost is constant regardless of
// member count (Matrix has no server-side member pagination — the whole set is
// loaded, then rendering/filtering is client-side). Lives at
// namespace scope so RoomSettingsWidget can hold a typed pointer (forward-declared
// in the header).
class MembersListInner : public QWidget {
public:
    explicit MembersListInner(QWidget *parent = nullptr) : QWidget(parent) {
        setMouseTracking(true);
    }

    void setMembers(QVector<RoomMemberInfo> members) {
        _all = std::move(members);
        applyFilter();
    }

    void setFilter(const QString &text) {
        const auto trimmed = text.trimmed().toLower();
        if (trimmed == _filter) {
            return;
        }
        _filter = trimmed;
        applyFilter();
    }

    int contentHeight() const {
        return kMemberListTopPadding + int(_filtered.size()) * kMemberRowHeight;
    }

    std::function<void(const QString &userId)> onMemberClicked;

protected:
    void paintEvent(QPaintEvent *e) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        if (_filtered.isEmpty()) {
            if (!_filter.isEmpty()) {
                p.setFont(st::baseFont(13));
                p.setPen(st::windowSubTextFg);
                p.drawText(
                    rect(),
                    Qt::AlignCenter,
                    QCoreApplication::translate(
                        "TeleMatrix::RoomSettingsWidget", "No members found"));
            }
            return;
        }

        const QRect clip = e->rect();
        const int first = qMax(0,
            (clip.top() - kMemberListTopPadding) / kMemberRowHeight);
        const int last = qMin(
            int(_filtered.size()) - 1,
            (clip.bottom() - kMemberListTopPadding) / kMemberRowHeight);
        for (int i = first; i <= last; ++i) {
            const QRect rowRect(
                0,
                kMemberListTopPadding + i * kMemberRowHeight,
                width(),
                kMemberRowHeight);
            paintMemberRow(p, rowRect, _all[_filtered[i]], (i == _hovered), this);
        }
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        const int row = rowAt(e->pos());
        if (row != _hovered) {
            _hovered = row;
            setCursor(row >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
            update();
        }
    }

    void leaveEvent(QEvent *) override {
        if (_hovered != -1) {
            _hovered = -1;
            update();
        }
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton) {
            return;
        }
        const int row = rowAt(e->pos());
        if (row >= 0 && onMemberClicked) {
            onMemberClicked(_all[_filtered[row]].userId);
        }
    }

private:
    void applyFilter() {
        _filtered.clear();
        _filtered.reserve(_all.size());
        for (int i = 0; i < _all.size(); ++i) {
            if (_filter.isEmpty()
                || _all[i].displayName.toLower().contains(_filter)
                || _all[i].userId.toLower().contains(_filter)) {
                _filtered.append(i);
            }
        }
        _hovered = -1;
        // Fixed height drives scrolling inside the widgetResizable scroll area;
        // when a filter matches nothing we keep a small height for the hint.
        const int height = !_filtered.isEmpty()
            ? contentHeight()
            : (!_filter.isEmpty() ? kMembersPreloaderHeight : 0);
        setFixedHeight(height);
        update();
    }

    int rowAt(const QPoint &pos) const {
        if (pos.y() < kMemberListTopPadding) {
            return -1;
        }
        const int row = (pos.y() - kMemberListTopPadding) / kMemberRowHeight;
        return (row >= 0 && row < int(_filtered.size())) ? row : -1;
    }

    QVector<RoomMemberInfo> _all;
    QVector<int> _filtered;
    QString _filter;
    int _hovered = -1;
};

// --- RoomSettingsWidget ---

RoomSettingsWidget::RoomSettingsWidget(
    const QString &roomId,
    AppController *controller,
    ProtocolBridge *bridge,
    QWidget *parent)
    : QWidget(parent)
    , _controller(controller)
    , _bridge(bridge)
    , _roomId(roomId)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    // Rounded corners are painted by LayerStackWidget's CornerOverlay.

    _closeIcon = loadColorizedIcon(QStringLiteral("info_close"), st::settingsCloseIconFg);
    _closeIconOver = loadColorizedIcon(QStringLiteral("info_close"), st::settingsCloseIconFgOver);

    // Fetch room data.
    const auto rooms = _bridge->cachedRooms();
    bool found = false;
    for (const auto &room : rooms) {
        if (room.roomId == roomId) {
            _roomSummary = room;
            found = true;
            break;
        }
    }
    // Spaces are deliberately excluded from the chat list, so they never appear
    // in cachedRooms(). Synthesize the summary from the joined-spaces cache so
    // the cover, member count, topic and Type row render correctly.
    if (!found) {
        for (const auto &space : _bridge->cachedJoinedSpaces()) {
            if (space.roomId == roomId) {
                _isSpace = true;
                _roomSummary.roomId = space.roomId;
                _roomSummary.displayName = space.displayName;
                _roomSummary.avatarUrl = space.avatarUrl;
                _roomSummary.avatarEntityId = space.roomId;
                _roomSummary.canonicalAlias = space.canonicalAlias;
                _roomSummary.memberCount = space.memberCount;
                _roomSummary.roomTopic = space.topic;
                break;
            }
        }
    }

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Top bar spacer (painted manually in paintEvent).
    auto *topBarSpacer = new QWidget(this);
    topBarSpacer->setFixedHeight(kTopBarHeight);
    topBarSpacer->setAttribute(Qt::WA_TransparentForMouseEvents);
    mainLayout->addWidget(topBarSpacer);

    // Body: sidebar + content.
    auto *bodyLayout = new QHBoxLayout;
    // Bottom margin = boxRadius so child widgets don't reach the rounded
    // bottom corners. The parent's paintEvent fills the corner area.
    bodyLayout->setContentsMargins(0, 0, 0, st::boxRadius);
    bodyLayout->setSpacing(0);

    setupSidebar();
    bodyLayout->addWidget(_sidebar);

    _stack = new QStackedWidget(this);
    bodyLayout->addWidget(_stack);

    setupGeneralPage();
    if (!_roomSummary.isDirect) {
        // Private chats have no member list.
        setupMembersPage();
    }
    // Spaces surface their join rule on the General page instead of a Security tab.
    if (!_isSpace) {
        setupSecurityPage();
    }

    mainLayout->addLayout(bodyLayout);

    showSection(Section::General);

    // Request room settings snapshot asynchronously.
    connect(_bridge, &ProtocolBridge::roomSettingsReady,
            this, &RoomSettingsWidget::onRoomSettingsReady);
    connect(_bridge, &ProtocolBridge::roomMembersSnapshotReady,
            this, [this](const QString &roomId, bool success, const RoomMembersSnapshot &snapshot) {
        if (roomId != _roomId) {
            return;
        }
        _membersSnapshotLoaded = true;
        _members = success ? snapshot.members : QVector<RoomMemberInfo>{};
        for (auto &member : _members) {
            member.trustState = static_cast<UserTrustState>(
                _bridge->cachedUserTrust(member.userId));
            _bridge->ensureUserTrust(member.userId);
        }
        rebuildMembersList();
        // Two-phase load: the first response is served instantly from the local
        // state store (possibly partial under sliding sync); request one
        // authoritative server refresh to fill in any missing members.
        if (success && !_membersFullRefreshRequested) {
            _membersFullRefreshRequested = true;
            _bridge->getRoomMembersSnapshotAsync(_roomId, true);
        }
    });
    // Live cross-signing trust -> member shields.
    const auto refreshMemberTrust = [this](const QString &userId, int state) {
        bool changed = false;
        for (auto &member : _members) {
            if (member.userId == userId) {
                const auto ts = static_cast<UserTrustState>(state);
                if (member.trustState != ts) {
                    member.trustState = ts;
                    changed = true;
                }
            }
        }
        if (changed) {
            rebuildMembersList();
        }
    };
    connect(_bridge, &ProtocolBridge::userTrustChanged, this, refreshMemberTrust);
    connect(_bridge, &ProtocolBridge::userTrustStateResult, this, refreshMemberTrust);
    connect(_bridge, &ProtocolBridge::roomEncryptionEnabled,
            this, &RoomSettingsWidget::onRoomEncryptionEnabled);
    connect(_bridge, &ProtocolBridge::roomAccessSet,
            this, &RoomSettingsWidget::onRoomAccessSet);
    connect(_bridge, &ProtocolBridge::roomHistoryVisibilitySet,
            this, &RoomSettingsWidget::onRoomHistoryVisibilitySet);
    connect(_bridge, &ProtocolBridge::roomNameSet,
            this, &RoomSettingsWidget::onRoomNameSet);
    connect(_bridge, &ProtocolBridge::roomTopicSet,
            this, &RoomSettingsWidget::onRoomTopicSet);
    connect(_bridge, &ProtocolBridge::roomAvatarUploaded,
            this, &RoomSettingsWidget::onRoomAvatarUploaded);
    connect(_bridge, &ProtocolBridge::roomAvatarDeleted,
            this, &RoomSettingsWidget::onRoomAvatarDeleted);
    connect(_bridge, &ProtocolBridge::mediaResolved,
            this, [this](bool success, const QString &mxcUrl, const QString &localPath) {
        if (!success || localPath.isEmpty()) {
            if (mxcUrl.startsWith(QStringLiteral("mxc://"))) {
                MediaCache::clearRequested(mxcUrl);
            }
            return;
        }
        MediaCache::insertPath(mxcUrl, localPath);
        if (_roomCover && mxcUrl == _roomSummary.avatarUrl) {
            _roomCover->update();
        }
    });
    if (_roomSummary.avatarUrl.startsWith(QStringLiteral("mxc://"))
        && MediaCache::needsResolution(_roomSummary.avatarUrl)) {
        MediaCache::markRequested(_roomSummary.avatarUrl);
        _bridge->resolveAvatar(_roomSummary.avatarUrl);
    }
    if (!_roomSummary.isDirect) {
        _bridge->getRoomMembersSnapshotAsync(_roomId);
    }
    _bridge->getRoomSettings(_roomId);
}

QSize RoomSettingsWidget::sizeHint() const {
    return {
        st::settingsMaxWidth,
        kTopBarHeight + desiredBodyHeight() + st::boxRadius,
    };
}

void RoomSettingsWidget::showMembersSection() {
    showSection(Section::Members);
}

void RoomSettingsWidget::setupSidebar() {
    _sidebar = new QWidget(this);
    _sidebar->setFixedWidth(kSidebarWidth);

    auto *layout = new QVBoxLayout(_sidebar);
    layout->setContentsMargins(0, 8, 0, 8);
    layout->setSpacing(2);

    const auto addButton = [&](const QString &icon, const QString &text, Section section) {
        auto *btn = new RoomSettingsSidebarButton(icon, text, _sidebar);
        btn->onClick = [this, section] { showSection(section); };
        layout->addWidget(btn);
        _sidebarButtons.append({ btn, section });
    };

    addButton(QStringLiteral("settings"), tr("General"), Section::General);
    if (!_roomSummary.isDirect) {
        addButton(QStringLiteral("groups"), tr("Members"), Section::Members);
    }
    // Spaces have no Security page — their join rule lives on the General page.
    if (!_isSpace) {
        addButton(QStringLiteral("read"), tr("Security"), Section::Security);
    }

    layout->addStretch();
}

namespace {

QString notifModeLabel(RoomNotificationMode mode) {
    switch (mode) {
    case RoomNotificationMode::MentionsOnly:
        return QCoreApplication::translate(
            "TeleMatrix::RoomSettingsWidget", "Mentions & keywords");
    case RoomNotificationMode::Mute:
        return QCoreApplication::translate(
            "TeleMatrix::RoomSettingsWidget", "Mute room");
    case RoomNotificationMode::AllMessages:
    default:
        return QCoreApplication::translate(
            "TeleMatrix::RoomSettingsWidget", "All messages");
    }
}

QString notifModeId(RoomNotificationMode mode) {
    switch (mode) {
    case RoomNotificationMode::MentionsOnly: return QStringLiteral("mentions");
    case RoomNotificationMode::Mute: return QStringLiteral("mute");
    default: return QStringLiteral("all");
    }
}

RoomNotificationMode notifModeFromId(const QString &id) {
    if (id == QStringLiteral("mentions")) return RoomNotificationMode::MentionsOnly;
    if (id == QStringLiteral("mute")) return RoomNotificationMode::Mute;
    return RoomNotificationMode::AllMessages;
}

QVector<Ui::InternalChoiceEntry> notifModeChoices() {
    QVector<Ui::InternalChoiceEntry> result;
    for (const auto mode : { RoomNotificationMode::AllMessages,
                             RoomNotificationMode::MentionsOnly,
                             RoomNotificationMode::Mute }) {
        result.push_back({ notifModeId(mode), notifModeLabel(mode) });
    }
    return result;
}

} // namespace

void RoomSettingsWidget::setupGeneralPage() {
    auto *scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 16);
    layout->setSpacing(0);

    // Room cover: avatar + inline name field.
    auto *cover = new RoomCoverWidget(_roomSummary, page);
    cover->onAvatarClick = [this] { chooseAndUploadRoomAvatar(); };
    cover->onAvatarDeleteClick = [this] { deleteRoomAvatar(); };
    cover->onNameCommit = [this](const QString &name) { commitRoomName(name); };
    _roomCover = cover;
    layout->addWidget(cover);

    // Topic / description as a read-only textarea (full text, like the settings
    // keyword box). Editable on click when permitted; settled in updateTopicRow.
    addSectionTitle(page, layout, _isSpace ? tr("Description") : tr("Topic"));
    {
        auto *topicDisplay = new TopicDisplay(page);
        topicDisplay->onCommit = [this](const QString &text) {
            commitTopic(text);
        };
        _topicRow = topicDisplay;

        auto *wrap = new QWidget(page);
        auto *wrapLayout = new QHBoxLayout(wrap);
        wrapLayout->setContentsMargins(kContentPadding, 0, kContentPadding, 8);
        wrapLayout->setSpacing(0);
        wrapLayout->addWidget(topicDisplay);
        layout->addWidget(wrap);
        updateTopicRow();
    }

    // A space has no Security tab (encryption/history don't apply), so its join
    // rule (Public/Private) lives here, clickable to change when permitted.
    if (_isSpace) {
        auto *row = new InfoValueRowWidget(tr("Access"), false, page);
        row->onClick = [this] { showAccessOptions(); };
        _accessRow = row;
        _accessValue = row->valueLabel();
        _accessValue->setText(tr("Loading..."));
        layout->addWidget(row);
    }

    // Notifications selector (same style as the settings language selector).
    if (!_isSpace) {
        auto *notif = new SettingsValueButton(
            tr("Notifications"),
            notifModeLabel(_roomSummary.notificationMode),
            page,
            QStringLiteral("settings_icons"),
            QStringLiteral("menu_notifications"));
        _notifButton = notif;
        notif->setClickedCallback([this, notif] {
            Ui::InternalChoiceDialog dialog(
                this,
                tr("Notifications"),
                notifModeChoices(),
                notifModeId(_roomSummary.notificationMode));
            if (dialog.exec() != Ui::InternalChoiceDialog::Accepted) {
                return;
            }
            const auto mode = notifModeFromId(dialog.chosenId());
            if (mode == _roomSummary.notificationMode) {
                return;
            }
            _roomSummary.notificationMode = mode; // optimistic
            notif->setValue(notifModeLabel(mode));
            if (_bridge) {
                _bridge->setRoomNotificationMode(_roomId, mode);
            }
        });
        layout->addWidget(notif);
    }

    addActionRow(page, layout,
                 _isSpace ? tr("Copy Space Link") : tr("Copy Room Link"),
                 QStringLiteral("link"), [this] {
        const auto matrixId = _roomSummary.canonicalAlias.isEmpty()
            ? _roomId
            : _roomSummary.canonicalAlias;
        const auto link = QStringLiteral("https://matrix.to/#/")
            + QString::fromLatin1(QUrl::toPercentEncoding(matrixId));
        QGuiApplication::clipboard()->setText(link);
        ::Ui::ShowToast(_isSpace ? tr("Space link copied to clipboard")
                                 : tr("Room link copied to clipboard"));
    });

    // Spaces hold no timeline, so there is nothing to export.
    if (!_isSpace) {
        addActionRow(page, layout, tr("Export History"), QStringLiteral("download"), [this] {
            emit exportHistoryRequested(_roomId);
        });
    }

    // Saved Messages cannot be left from the UI (tdesktop semantics).
    if (!_bridge || _roomId != _bridge->savedMessagesRoomId()) {
        addActionRow(page, layout,
                     _isSpace ? tr("Leave Space") : tr("Leave Room"),
                     QStringLiteral("leave"), [this] {
            emit leaveRoomRequested(_roomId);
        }, true);
    }

    layout->addStretch();

    scrollArea->setWidget(page);
    _stack->addWidget(scrollArea);
}

void RoomSettingsWidget::setupMembersPage() {
    _membersPage = new QWidget;
    auto *layout = new QVBoxLayout(_membersPage);
    layout->setContentsMargins(0, 8, 0, 8);
    layout->setSpacing(0);

    // --- Fixed header (stays put while the list scrolls) ---

    // Search / filter field: frameless + transparent with a magnifier and a
    // bottom separator, matching the app's other search inputs (the native
    // QLineEdit frame renders as an out-of-place dark pill).
    auto *searchContainer = new QWidget(_membersPage);
    searchContainer->setFixedHeight(36);
    auto *searchLayout = new QHBoxLayout(searchContainer);
    searchLayout->setContentsMargins(kContentPadding, 4, kContentPadding, 4);
    searchLayout->setSpacing(8);

    auto *searchIcon = new MembersSearchIcon(searchContainer);
    searchLayout->addWidget(searchIcon);

    _membersSearchField = new QLineEdit(searchContainer);
    _membersSearchField->setPlaceholderText(tr("Search members"));
    _membersSearchField->setFrame(false);
    _membersSearchField->setClearButtonEnabled(true);
    _membersSearchField->setAttribute(Qt::WA_MacShowFocusRect, false);
    _membersSearchField->setFont(st::baseFont(13));
    {
        QPalette pal = _membersSearchField->palette();
        pal.setColor(QPalette::Base, Qt::transparent);
        pal.setColor(QPalette::Text, st::windowFg);
        _membersSearchField->setPalette(pal);
    }
    connect(_membersSearchField, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (_membersListInner) {
            _membersListInner->setFilter(text);
        }
    });
    searchLayout->addWidget(_membersSearchField, 1);

    // "Add member" action icon (menu glyph, no text) at the right of the field.
    auto *addMemberIcon = new AddMemberIconButton(searchContainer);
    addMemberIcon->setToolTip(tr("Add member"));
    addMemberIcon->setVisible(_canInvite);
    connect(addMemberIcon, &QPushButton::clicked, this, [this] {
        if (!_canInvite || !_bridge) {
            return;
        }
        InviteUsersBox dialog(_roomId, _bridge, this, true);
        dialog.exec();
        _membersSnapshotLoaded = false;
        _members.clear();
        rebuildMembersList();
        // Membership just changed on the server: force an authoritative refetch.
        _bridge->getRoomMembersSnapshotAsync(_roomId, true);
    });
    _addMemberButton = addMemberIcon;
    searchLayout->addWidget(addMemberIcon);

    layout->addWidget(searchContainer);

    auto *searchSep = new QWidget(_membersPage);
    searchSep->setFixedHeight(1);
    searchSep->setAutoFillBackground(true);
    {
        QPalette pal = searchSep->palette();
        pal.setColor(QPalette::Window, st::shadowFg);
        searchSep->setPalette(pal);
    }
    layout->addWidget(searchSep);

    // Loading spinner (shown until the first snapshot arrives).
    _membersPreloader = new MembersPreloaderWidget(tr("Loading..."), _membersPage);
    _membersPreloader->setVisible(false);
    layout->addWidget(_membersPreloader);

    // --- Scrolling, virtualized members list ---

    auto *scrollArea = new QScrollArea(_membersPage);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    _membersListInner = new MembersListInner();
    _membersListInner->onMemberClicked = [this](const QString &userId) {
        emit openUserProfileRequested(_roomId, userId);
    };
    scrollArea->setWidget(_membersListInner);
    layout->addWidget(scrollArea, 1);

    rebuildMembersList();

    _stack->addWidget(_membersPage);
}

void RoomSettingsWidget::setupSecurityPage() {
    auto *scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 8, 0, 16);
    layout->setSpacing(0);

    {
        auto *row = new InfoValueRowWidget(tr("Encryption"), false, page);
        row->onClick = [this, row] {
            if (_roomIsEncrypted
                || _encryptionRequestInFlight
                || !_canChangeEncryption
                || !_bridge) {
                return;
            }
            HistoryConfirmDialog dialog(
                this,
                tr("Enable Encryption"),
                tr(
                    "Once enabled, encryption cannot be disabled.\n\n"
                    "Are you sure you want to enable encryption for this room?"),
                tr("Enable"));
            if (dialog.exec() != HistoryConfirmDialog::Accepted) {
                return;
            }
            _encryptionRequestInFlight = true;
            if (_encryptionValue) {
                _encryptionValue->setText(tr("Enabling..."));
            }
            row->setClickable(false);
            _bridge->enableRoomEncryption(_roomId);
        };
        _encryptionRow = row;
        _encryptionValue = row->valueLabel();
        layout->addWidget(row);
    }

    {
        auto *row = new InfoValueRowWidget(tr("Access"), false, page);
        row->onClick = [this] { showAccessOptions(); };
        _accessRow = row;
        _accessValue = row->valueLabel();
        _accessValue->setText(tr("Loading..."));
        layout->addWidget(row);
    }

    {
        auto *row = new InfoValueRowWidget(tr("History visibility"), false, page);
        row->onClick = [this] { showHistoryVisibilityOptions(); };
        _visibilityRow = row;
        _visibilityValue = row->valueLabel();
        _visibilityValue->setText(tr("Loading..."));
        layout->addWidget(row);
    }

    {
        auto *row = new InfoValueRowWidget(tr("New members can see history"), false, page);
        _newMembersValue = row->valueLabel();
        _newMembersValue->setText(tr("Loading..."));
        layout->addWidget(row);
    }

    layout->addStretch();

    scrollArea->setWidget(page);
    _stack->addWidget(scrollArea);
}

void RoomSettingsWidget::showSection(Section section) {
    // Members is absent for private chats; fall back to General if requested.
    if (section == Section::Members && _roomSummary.isDirect) {
        section = Section::General;
    }
    _currentSection = section;

    for (auto &btn : _sidebarButtons) {
        auto *sb = static_cast<RoomSettingsSidebarButton *>(btn.widget);
        if (sb) {
            sb->setSelected(btn.section == section);
        }
    }

    // Section enum → stack index. The Members page is only added for non-DM
    // rooms, so Security shifts down one when it is absent.
    int index = 0;
    switch (section) {
        case Section::General: index = 0; break;
        case Section::Members: index = 1; break;
        case Section::Security: index = _roomSummary.isDirect ? 1 : 2; break;
    }
    _stack->setCurrentIndex(index);
    updateGeometry();
    if (parentWidget()) {
        parentWidget()->update();
    }
}

void RoomSettingsWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Background.
    p.fillRect(rect(), st::windowBg);

    // Top bar.
    paintTopBar(p);

    // Sidebar separator.
    const int sidebarRight = kSidebarWidth;
    p.setPen(QPen(st::shadowFg, 1));
    p.drawLine(sidebarRight, kTopBarHeight, sidebarRight, height());
    // Rounded corners are handled by LayerStackWidget's CornerOverlay.
}

void RoomSettingsWidget::paintTopBar(QPainter &p) {
    const QRect topBar(0, 0, width(), kTopBarHeight);
    p.fillRect(topBar, st::topBarBg);

    // Title.
    p.setFont(st::baseFont(16, true));
    p.setPen(st::windowBoldFg);
    p.drawText(22, kTopBarHeight / 2 + 6,
               _isSpace ? tr("Space Settings") : tr("Room Settings"));

    // Close button (top right).
    const auto &icon = _closeHovered ? _closeIconOver : _closeIcon;
    const auto iconSize = iconLogicalSize(icon, kCloseIconSize);
    const int closeButtonLeft = width() - st::settingsCloseButtonSize;
    const int closeX = closeButtonLeft + kCloseIconLeft;
    const int closeY = (kTopBarHeight - iconSize.height()) / 2;
    _closeButtonRect = QRect(closeButtonLeft, 0, st::settingsCloseButtonSize, kTopBarHeight);

    if (!icon.isNull()) {
        p.drawImage(closeX, closeY, icon);
    }

    // Bottom separator.
    p.setPen(QPen(st::shadowFg, 1));
    p.drawLine(0, kTopBarHeight - 1, width(), kTopBarHeight - 1);
}

void RoomSettingsWidget::addSectionTitle(QWidget *parent, QVBoxLayout *layout, const QString &title) {
    auto *label = new QLabel(parent);
    label->setFixedHeight(kSectionTitleHeight);
    label->setContentsMargins(kContentPadding, 0, kContentPadding, 0);
    label->setText(title);
    label->setFont(st::baseFont(13, true));
    QPalette pal = label->palette();
    pal.setColor(QPalette::WindowText, st::windowActiveTextFg);
    label->setPalette(pal);
    layout->addWidget(label);
}

void RoomSettingsWidget::addDivider(QWidget *parent, QVBoxLayout *layout) {
    auto *divider = new QWidget(parent);
    divider->setFixedHeight(kDividerHeight);
    divider->setAutoFillBackground(true);
    QPalette pal = divider->palette();
    pal.setColor(QPalette::Window, st::boxDividerBg);
    divider->setPalette(pal);
    layout->addWidget(divider);
}

void RoomSettingsWidget::addInfoRow(QWidget *parent, QVBoxLayout *layout,
                                     const QString &label, const QString &value,
                                     bool copyButton) {
    auto *row = new QWidget(parent);
    row->setFixedHeight(kInfoRowHeight);
    auto *rowLayout = new QHBoxLayout(row);
    // QPushButton centers the 18px copy glyph inside a 34px hit target.
    // Pull the hit target 8px right so the visible glyph edge aligns with
    // plain value rows below it.
    rowLayout->setContentsMargins(
        kContentPadding,
        0,
        copyButton ? qMax(0, kContentPadding - 8) : kContentPadding,
        0);

    auto *labelWidget = new QLabel(label, row);
    labelWidget->setFont(st::baseFont(14));
    QPalette labelPal = labelWidget->palette();
    labelPal.setColor(QPalette::WindowText, st::windowSubTextFg);
    labelWidget->setPalette(labelPal);
    rowLayout->addWidget(labelWidget);

    auto *valueWidget = new QLabel(value, row);
    valueWidget->setFont(st::baseFont(14));
    valueWidget->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    QPalette valuePal = valueWidget->palette();
    valuePal.setColor(QPalette::WindowText, st::windowBoldFg);
    valueWidget->setPalette(valuePal);
    valueWidget->setMinimumWidth(0);
    valueWidget->setTextInteractionFlags(copyButton
        ? Qt::NoTextInteraction
        : Qt::TextSelectableByMouse);
    rowLayout->addWidget(valueWidget, 1);

    if (copyButton) {
        auto *copy = createCopyIconButton(row);
        connect(copy, &QPushButton::clicked, this, [copy, value] {
            QGuiApplication::clipboard()->setText(value);
            ::Ui::ShowToast(QCoreApplication::translate(
                "TeleMatrix::RoomSettingsWidget", "Copied to clipboard"));
            copy->setToolTip(QCoreApplication::translate(
                "TeleMatrix::RoomSettingsWidget",
                "Copied!"));
            QTimer::singleShot(2000, copy, [copy] {
                if (copy) {
                    copy->setToolTip(QCoreApplication::translate(
                        "TeleMatrix::RoomSettingsWidget",
                        "Copy to clipboard"));
                }
            });
        });
        rowLayout->addWidget(copy, 0, Qt::AlignVCenter);
    }

    layout->addWidget(row);
}

void RoomSettingsWidget::addActionRow(QWidget *parent, QVBoxLayout *layout,
                                       const QString &text, const QString &iconName,
                                       const std::function<void()> &callback,
                                       bool attention) {
    auto *row = new ActionRowWidget(text, iconName, attention, parent);
    row->onClick = callback;
    layout->addWidget(row);
}

void RoomSettingsWidget::showAccessOptions() {
    if (_accessRequestInFlight || !_canChangeAccess || !_bridge) {
        return;
    }

    QVector<Ui::InternalChoiceEntry> entries;
    entries.push_back({
        QStringLiteral("private"),
        tr("Private"),
        QString(),
        st::baseFont(14),
        true,
    });
    entries.push_back({
        QStringLiteral("public"),
        tr("Public"),
        QString(),
        st::baseFont(14),
        true,
    });
    entries.push_back({
        QStringLiteral("knock"),
        tr("Knock"),
        QString(),
        st::baseFont(14),
        true,
    });
    if (_roomAccess == RoomAccess::Restricted) {
        entries.push_back({
            QStringLiteral("restricted"),
            tr("Restricted"),
            QString(),
            st::baseFont(14),
            false,
        });
    } else if (_roomAccess == RoomAccess::KnockRestricted) {
        entries.push_back({
            QStringLiteral("knock_restricted"),
            tr("Restricted, can knock"),
            QString(),
            st::baseFont(14),
            false,
        });
    }

    Ui::InternalChoiceDialog dialog(this, tr("Access"), entries, roomAccessId(_roomAccess));
    if (dialog.exec() != Ui::InternalChoiceDialog::Accepted) {
        return;
    }

    const auto next = roomAccessFromId(dialog.chosenId());
    if (next == RoomAccess::Unknown || next == _roomAccess) {
        return;
    }

    _roomAccessBeforeRequest = _roomAccess;
    _roomAccessPending = next;
    _accessRequestInFlight = true;
    _roomAccess = _roomAccessPending;
    updateAccessSection();
    _bridge->setRoomAccess(_roomId, _roomAccessPending);
}

void RoomSettingsWidget::showHistoryVisibilityOptions() {
    if (_historyVisibilityRequestInFlight
        || !_canChangeHistoryVisibility
        || !_bridge) {
        return;
    }

    QVector<Ui::InternalChoiceEntry> entries;
    entries.push_back({
        QStringLiteral("shared"),
        tr("Members only (full history)"),
        QString(),
        st::baseFont(14),
        true,
    });
    entries.push_back({
        QStringLiteral("invited"),
        tr("Members only (since invited)"),
        QString(),
        st::baseFont(14),
        true,
    });
    entries.push_back({
        QStringLiteral("joined"),
        tr("Members only (since joining)"),
        QString(),
        st::baseFont(14),
        true,
    });
    entries.push_back({
        QStringLiteral("world_readable"),
        tr("Anyone (world-readable)"),
        QString(),
        st::baseFont(14),
        true,
    });

    Ui::InternalChoiceDialog dialog(
        this,
        tr("History visibility"),
        entries,
        historyVisibilityId(_historyVisibility));
    if (dialog.exec() != Ui::InternalChoiceDialog::Accepted) {
        return;
    }

    const auto next = historyVisibilityFromId(dialog.chosenId());
    if (next == HistoryVisibility::Unknown || next == _historyVisibility) {
        return;
    }

    _historyVisibilityBeforeRequest = _historyVisibility;
    _historyVisibilityPending = next;
    _historyVisibilityRequestInFlight = true;
    _historyVisibility = _historyVisibilityPending;
    updateHistoryVisibilitySection();
    _bridge->setRoomHistoryVisibility(_roomId, _historyVisibilityPending);
}

void RoomSettingsWidget::updateMemberActions() {
    if (_addMemberButton) {
        _addMemberButton->setVisible(_canInvite);
    }
}

void RoomSettingsWidget::updateEncryptionSection() {
    if (_encryptionRow) {
        static_cast<InfoValueRowWidget *>(_encryptionRow)->setClickable(
            !_roomIsEncrypted
            && _canChangeEncryption
            && !_encryptionRequestInFlight);
    }
}

void RoomSettingsWidget::updateAccessSection() {
    if (_accessValue) {
        _accessValue->setText(_accessRequestInFlight
            ? tr("Updating...")
            : roomAccessText(_roomAccess));
    }
    if (_accessRow) {
        static_cast<InfoValueRowWidget *>(_accessRow)->setClickable(
            _canChangeAccess && !_accessRequestInFlight);
    }
}

void RoomSettingsWidget::updateHistoryVisibilitySection() {
    if (_visibilityValue) {
        _visibilityValue->setText(_historyVisibilityRequestInFlight
            ? tr("Updating...")
            : historyVisibilityText(_historyVisibility));
    }
    if (_visibilityRow) {
        static_cast<InfoValueRowWidget *>(_visibilityRow)->setClickable(
            _canChangeHistoryVisibility && !_historyVisibilityRequestInFlight);
    }
}

int RoomSettingsWidget::desiredBodyHeight() const {
    const auto sidebarButtons = _sidebarButtons.size();
    const auto sidebarHeight = 16
        + sidebarButtons * kSidebarButtonHeight
        + qMax(0, sidebarButtons - 1) * 2;

    return qMax(sidebarHeight, kGeneralBodyHeight);
}

void RoomSettingsWidget::rebuildMembersList() {
    const bool loading = !_membersSnapshotLoaded && _members.isEmpty();
    if (_membersPreloader) {
        _membersPreloader->setVisible(loading);
    }
    if (_membersListInner) {
        _membersListInner->setMembers(_members);
    }
}

void RoomSettingsWidget::mousePressEvent(QMouseEvent *e) {
    if (_closeButtonRect.contains(e->pos())) {
        emit closeRequested();
        return;
    }
    QWidget::mousePressEvent(e);
}

void RoomSettingsWidget::mouseMoveEvent(QMouseEvent *e) {
    const bool closeHovered = _closeButtonRect.contains(e->pos());
    if (closeHovered != _closeHovered) {
        _closeHovered = closeHovered;
        setCursor(_closeHovered ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update(QRect(0, 0, width(), kTopBarHeight));
    }
    QWidget::mouseMoveEvent(e);
}

void RoomSettingsWidget::leaveEvent(QEvent *e) {
    if (_closeHovered) {
        _closeHovered = false;
        setCursor(Qt::ArrowCursor);
        update(QRect(0, 0, width(), kTopBarHeight));
    }
    QWidget::leaveEvent(e);
}

void RoomSettingsWidget::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
}

void RoomSettingsWidget::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape) {
        emit closeRequested();
        e->accept();
        return;
    }
    QWidget::keyPressEvent(e);
}

void RoomSettingsWidget::chooseAndUploadRoomAvatar() {
    if (!_bridge || _roomAvatarOperationInFlight || !_canChangeAvatar) {
        return;
    }
    const auto path = QFileDialog::getOpenFileName(
        this,
        tr("Choose Avatar"),
        QString(),
        tr("Images (*.png *.jpg *.jpeg *.gif *.webp)"));
    if (path.isEmpty()) {
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const auto data = file.readAll();
    if (data.isEmpty()) {
        return;
    }

    _roomAvatarOperationInFlight = true;
    if (auto *cover = static_cast<RoomCoverWidget *>(_roomCover)) {
        cover->setAvatarOperationInFlight(true);
    }
    _bridge->uploadRoomAvatar(_roomId, data, imageContentTypeForPath(path));
}

void RoomSettingsWidget::deleteRoomAvatar() {
    if (!_bridge
        || _roomAvatarOperationInFlight
        || !_canChangeAvatar
        || _roomSummary.avatarUrl.isEmpty()) {
        return;
    }
    HistoryConfirmDialog dialog(
        this,
        tr("Delete Avatar"),
        tr("Remove this room's avatar?"),
        tr("Delete"),
        QString(),
        HistoryConfirmDialog::Attention);
    if (dialog.exec() != HistoryConfirmDialog::Accepted) {
        return;
    }

    _roomAvatarOperationInFlight = true;
    if (auto *cover = static_cast<RoomCoverWidget *>(_roomCover)) {
        cover->setAvatarOperationInFlight(true);
    }
    _bridge->deleteRoomAvatar(_roomId);
}

void RoomSettingsWidget::onRoomAvatarUploaded(
    const QString &roomId,
    bool success,
    const QString &newAvatarUrl) {
    if (roomId != _roomId) {
        return;
    }
    _roomAvatarOperationInFlight = false;
    if (auto *cover = static_cast<RoomCoverWidget *>(_roomCover)) {
        cover->setAvatarOperationInFlight(false);
    }
    if (!success || newAvatarUrl.isEmpty()) {
        showWarningBox(
            this,
            tr("Error"),
            tr("Failed to upload room avatar. Please try again."));
        return;
    }

    _roomSummary.avatarUrl = newAvatarUrl;
    if (auto *cover = static_cast<RoomCoverWidget *>(_roomCover)) {
        cover->setAvatarUrl(newAvatarUrl);
    }
    if (newAvatarUrl.startsWith(QStringLiteral("mxc://"))
        && MediaCache::needsResolution(newAvatarUrl)) {
        MediaCache::markRequested(newAvatarUrl);
        _bridge->resolveAvatar(newAvatarUrl);
    }
}

void RoomSettingsWidget::onRoomAvatarDeleted(const QString &roomId, bool success) {
    if (roomId != _roomId) {
        return;
    }
    _roomAvatarOperationInFlight = false;
    if (auto *cover = static_cast<RoomCoverWidget *>(_roomCover)) {
        cover->setAvatarOperationInFlight(false);
    }
    if (!success) {
        showWarningBox(
            this,
            tr("Error"),
            tr("Failed to delete room avatar. Please try again."));
        return;
    }

    if (!_roomSummary.avatarUrl.isEmpty()) {
        MediaCache::clearRequested(_roomSummary.avatarUrl);
    }
    _roomSummary.avatarUrl.clear();
    if (auto *cover = static_cast<RoomCoverWidget *>(_roomCover)) {
        cover->setAvatarUrl(QString());
    }
}

void RoomSettingsWidget::onRoomSettingsReady(bool success, const RoomSettingsSnapshot &snap) {
    if (snap.roomId != _roomId) {
        return;
    }
    if (!success) {
        if (_encryptionValue) _encryptionValue->setText(tr("Unavailable"));
        if (_accessValue) _accessValue->setText(tr("Unavailable"));
        if (_visibilityValue) _visibilityValue->setText(tr("Unavailable"));
        if (_newMembersValue) _newMembersValue->setText(tr("Unavailable"));
        _canInvite = false;
        _canChangeAvatar = false;
        _canChangeName = false;
        _canChangeTopic = false;
        _canChangeEncryption = false;
        _canChangeAccess = false;
        _canChangeHistoryVisibility = false;
        updateMemberActions();
        updateTopicRow();
        if (auto *cover = static_cast<RoomCoverWidget *>(_roomCover)) {
            cover->setCanEditAvatar(false);
            cover->setCanEditName(false);
        }
        updateEncryptionSection();
        if (_accessRow) {
            static_cast<InfoValueRowWidget *>(_accessRow)->setClickable(false);
        }
        if (_visibilityRow) {
            static_cast<InfoValueRowWidget *>(_visibilityRow)->setClickable(false);
        }
        return;
    }

    _roomSummary.canonicalAlias = snap.canonicalAlias;
    _roomSummary.notificationMode = snap.notificationMode;
    if (_notifButton) {
        static_cast<SettingsValueButton *>(_notifButton)
            ->setValue(notifModeLabel(_roomSummary.notificationMode));
    }
    _roomSummary.isMuted = snap.isMuted;
    if (!snap.displayName.isEmpty()) {
        _roomSummary.displayName = snap.displayName;
    }
    _canInvite = snap.canInvite;
    _canChangeAvatar = snap.canChangeAvatar;
    _canChangeName = snap.canChangeName;
    _canChangeTopic = snap.canChangeTopic;
    _canChangeEncryption = snap.canChangeEncryption;
    _canChangeAccess = snap.canChangeAccess;
    _canChangeHistoryVisibility = snap.canChangeHistoryVisibility;
    updateMemberActions();
    updateTopicRow();
    if (auto *cover = static_cast<RoomCoverWidget *>(_roomCover)) {
        cover->setCanEditAvatar(_canChangeAvatar);
        cover->setCanEditName(_canChangeName);
        cover->setDisplayName(_roomSummary.displayName);
    }
    // Encryption status.
    _roomIsEncrypted = snap.isEncrypted;
    if (_encryptionValue) {
        if (_encryptionRequestInFlight) {
            _encryptionValue->setText(tr("Enabling..."));
        } else if (snap.isEncrypted) {
            auto text = tr("Enabled");
            if (!snap.encryptionAlgorithm.isEmpty()) {
                text = tr("Enabled (%1)").arg(snap.encryptionAlgorithm);
            }
            _encryptionValue->setText(text);
        } else {
            _encryptionValue->setText(tr("Not encrypted"));
        }
    }
    if (_encryptionRow) {
        updateEncryptionSection();
    }

    if (!_accessRequestInFlight) {
        _roomAccess = snap.access;
    }
    updateAccessSection();

    // History visibility.
    if (!_historyVisibilityRequestInFlight) {
        _historyVisibility = snap.historyVisibility;
    }
    updateHistoryVisibilitySection();

    // New members can see history.
    if (_newMembersValue) {
        _newMembersValue->setText(snap.newMembersCanSeeHistory
            ? tr("Yes")
            : tr("No"));
    }
    updateGeometry();
    if (parentWidget()) {
        parentWidget()->update();
    }
}

void RoomSettingsWidget::onRoomEncryptionEnabled(bool success) {
    _encryptionRequestInFlight = false;
    if (success) {
        _roomIsEncrypted = true;
        if (_encryptionValue) {
            _encryptionValue->setText(
                tr("Enabled (%1)").arg(QStringLiteral("m.megolm.v1.aes-sha2")));
        }
        if (_encryptionRow) {
            updateEncryptionSection();
        }
    } else {
        if (_encryptionValue) {
            _encryptionValue->setText(tr("Not encrypted"));
        }
        if (_encryptionRow) {
            updateEncryptionSection();
        }
        showWarningBox(
            this,
            tr("Error"),
            tr("Failed to enable encryption. Please try again."));
    }
    updateGeometry();
    if (parentWidget()) {
        parentWidget()->update();
    }
}

void RoomSettingsWidget::onRoomAccessSet(const QString &roomId, bool success) {
    if (roomId != _roomId || !_accessRequestInFlight) {
        return;
    }
    _accessRequestInFlight = false;
    if (success) {
        _roomAccess = _roomAccessPending;
        if (_bridge) {
            _bridge->getRoomSettings(_roomId);
        }
    } else {
        _roomAccess = _roomAccessBeforeRequest;
        showWarningBox(
            this,
            tr("Error"),
            tr("Failed to update room access. Please try again."));
    }
    updateAccessSection();
}

void RoomSettingsWidget::onRoomHistoryVisibilitySet(
    const QString &roomId,
    bool success) {
    if (roomId != _roomId || !_historyVisibilityRequestInFlight) {
        return;
    }
    _historyVisibilityRequestInFlight = false;
    if (success) {
        _historyVisibility = _historyVisibilityPending;
        if (_bridge) {
            _bridge->getRoomSettings(_roomId);
        }
    } else {
        _historyVisibility = _historyVisibilityBeforeRequest;
        showWarningBox(
            this,
            tr("Error"),
            tr("Failed to update history visibility. Please try again."));
    }
    updateHistoryVisibilitySection();
}

void RoomSettingsWidget::commitRoomName(const QString &name) {
    if (!_bridge || !_canChangeName) {
        return;
    }
    const auto newName = name.trimmed();
    const auto currentName = _roomSummary.displayName;
    if (newName.isEmpty() || newName == currentName) {
        return;
    }
    _bridge->setRoomName(_roomId, newName);
    // Reflect the new name immediately; onRoomNameSet reverts it on failure.
    _roomSummary.displayName = newName;
    if (auto *cover = static_cast<RoomCoverWidget *>(_roomCover)) {
        cover->setDisplayName(newName);
    }
}

void RoomSettingsWidget::onRoomNameSet(const QString &roomId, bool success) {
    if (roomId != _roomId) {
        return;
    }
    if (success) {
        ::Ui::ShowToast(tr("Room name updated"));
    } else {
        showWarningBox(
            this,
            tr("Error"),
            tr("Failed to update room name. Please try again."));
        // Revert the optimistic name to the server's current value.
        if (_bridge) {
            _bridge->getRoomSettings(_roomId);
        }
    }
}

void RoomSettingsWidget::updateTopicRow() {
    if (!_topicRow) {
        return;
    }
    const auto topic = _roomSummary.roomTopic.trimmed();
    const auto hasTopic = !topic.isEmpty();
    auto *display = static_cast<TopicDisplay *>(_topicRow);
    // The textarea shows the full topic; when there's none, a muted placeholder
    // invites the (permitted) user to add one, or reads "No topic".
    display->setText(hasTopic ? topic : QString());
    display->setPlaceholder(_canChangeTopic
        ? (_isSpace ? tr("Add a description") : tr("Add a topic"))
        : (_isSpace ? tr("No description") : tr("No topic")));
    display->setEditable(_canChangeTopic);
}

void RoomSettingsWidget::commitTopic(const QString &text) {
    if (!_bridge || !_canChangeTopic || _topicOpInFlight) {
        return;
    }
    const auto next = text.trimmed();
    const auto current = _roomSummary.roomTopic.trimmed();
    if (next == current) {
        return;
    }
    _topicBeforeEdit = _roomSummary.roomTopic;
    _topicOpInFlight = true;
    _roomSummary.roomTopic = next; // optimistic; reverted on failure
    updateTopicRow();
    _bridge->setRoomTopic(_roomId, next);
}

void RoomSettingsWidget::onRoomTopicSet(const QString &roomId, bool success) {
    if (roomId != _roomId) {
        return;
    }
    _topicOpInFlight = false;
    if (success) {
        ::Ui::ShowToast(tr("Topic updated"));
    } else {
        showWarningBox(
            this,
            tr("Error"),
            tr("Failed to update the topic. Please try again."));
        _roomSummary.roomTopic = _topicBeforeEdit;
        updateTopicRow();
    }
}

} // namespace TeleMatrix

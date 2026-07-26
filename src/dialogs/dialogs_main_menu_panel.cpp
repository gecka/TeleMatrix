// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "dialogs_main_menu_panel.h"

#include "app/account.h"
#include "app/app_controller.h"
#include "app/unread_state_store.h"
#include "dialogs/dialogs_layout.h"
#include "protocol/media_cache.h"
#include "protocol/protocol_bridge.h"
#include "styles/style_constants.h"
#include "ui/empty_userpic.h"
#include "ui/painter.h"
#include "ui/style/icon_provider.h"
#include "ui/widgets/buttons.h"

#include <QCoreApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QScrollArea>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include <functional>

namespace TeleMatrix {

namespace {

QString appNameLabel() {
    const auto name = QCoreApplication::applicationName().trimmed();
    return name.isEmpty() ? QStringLiteral("TeleMatrix") : name;
}

QString appVersionLabel() {
    const auto version = QCoreApplication::applicationVersion().trimmed();
    if (version.isEmpty()) {
        return QCoreApplication::translate("DialogsMainMenuPanel", "version unknown");
    }
    return version.startsWith(QLatin1Char('v'))
        ? version
        : (QStringLiteral("v") + version);
}

QString displayNameFor(AppController *controller) {
    if (!controller) {
        return QCoreApplication::translate("DialogsMainMenuPanel", "Unknown User");
    }
    const auto name = controller->displayName().trimmed();
    return name.isEmpty() ? QCoreApplication::translate("DialogsMainMenuPanel", "Unknown User") : name;
}

QString secondaryLineFor(AppController *controller) {
    if (!controller) {
        return QCoreApplication::translate("DialogsMainMenuPanel", "user unknown");
    }
    const auto userId = controller->userId().trimmed();
    return userId.isEmpty() ? QCoreApplication::translate("DialogsMainMenuPanel", "user unknown") : userId;
}

class MainMenuRow final : public QWidget {
    Q_OBJECT

public:
    /// `roundIcon` draws the glyph in white on a filled accent disc, the way
    /// tdesktop renders its "Add Account" entry (IconType::Round), instead of the
    /// flat tinted glyph every other row uses.
    MainMenuRow(
        const QString &label,
        const QString &iconName,
        bool attention,
        bool toggle,
        QWidget *parent = nullptr,
        bool roundIcon = false)
    : QWidget(parent)
    , _label(label)
    , _iconName(iconName)
    , _attention(attention)
    , _toggle(toggle)
    , _roundIcon(roundIcon) {
        setFixedHeight(st::mainMenuRowHeight);
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);

        if (_toggle) {
            _toggleAnimation = new QVariantAnimation(this);
            _toggleAnimation->setDuration(140);
            _toggleAnimation->setEasingCurve(QEasingCurve::OutCubic);
            QObject::connect(_toggleAnimation, &QVariantAnimation::valueChanged, this,
                [this](const QVariant &value) {
                    _togglePosition = value.toReal();
                    update();
                });
        }
    }

    /// Lay this row out like an account row rather than a menu action: the
    /// switcher's icon column and text column, and its taller row height.
    void useAccountRowMetrics() {
        _accountMetrics = true;
        setFixedHeight(st::mainMenuAccountRowHeight);
        update();
    }

    // Set toggle state without emitting signal.
    void setChecked(bool checked) {
        if (!_toggle || _checked == checked) return;
        _checked = checked;
        _togglePosition = checked ? 1.0 : 0.0;
        update();
    }

Q_SIGNALS:
    void clicked();
    void toggledChanged(bool toggled);

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);

        if (_pressed) {
            p.fillRect(rect(), st::mainMenuRowBgDown);
        } else if (_hovered) {
            p.fillRect(rect(), st::mainMenuRowBgOver);
        }

        const auto iconColor = _roundIcon
            ? st::activeButtonFg
            : (_attention ? st::mainMenuRowIconAttention : st::mainMenuRowIcon);
        const auto icon = TeleMatrix::Style::IconProvider::tintedIcon(
            QStringLiteral(":/telematrix/icons/menu/"),
            _iconName,
            iconColor);
        if (!icon.isNull()) {
            const auto iconSize = icon.size() / icon.devicePixelRatio();
            // One centre line for both the disc and the glyph. On switcher rows
            // it is the avatar column's centre, so the disc sits exactly where
            // the account avatars above it do; elsewhere the glyph keeps its own
            // left-aligned icon column.
            const auto centreX = _accountMetrics
                ? (st::mainMenuAccountAvatarLeft
                    + st::mainMenuAccountAvatarSize / 2.0)
                : (st::mainMenuRowIconLeft + iconSize.width() / 2.0);
            const auto iconTop = (height() - iconSize.height()) / 2;
            const auto iconLeft = qRound(centreX - iconSize.width() / 2.0);
            if (_roundIcon) {
                // tdesktop IconType::Round as the folders "create" row renders
                // it: the accent disc is the glyph's own integer rect, so the
                // two can't drift apart by sub-pixel rounding at fractional
                // interface scales.
                p.save();
                p.setRenderHint(QPainter::Antialiasing, true);
                p.setPen(Qt::NoPen);
                p.setBrush(st::activeButtonBg);
                p.drawEllipse(QRect(
                    iconLeft, iconTop, iconSize.width(), iconSize.height()));
                p.restore();
            }
            p.drawImage(QPoint(iconLeft, iconTop), icon);
        }

        p.setFont(st::mainMenuRowFont());
        p.setPen(_attention
            ? st::mainMenuRowTextAttention
            : st::mainMenuRowText);

        auto textRight = st::mainMenuRowRightPadding;
        if (_toggle) {
            textRight += st::settingsToggleWidth + 12;
        }

        const auto textLeft = _accountMetrics
            ? st::mainMenuAccountTextLeft
            : st::mainMenuRowTextLeft;
        const auto textRect = QRect(
            textLeft,
            0,
            qMax(0, width() - textLeft - textRight),
            height());
        const auto elided = QFontMetrics(st::mainMenuRowFont()).elidedText(
            _label,
            Qt::ElideRight,
            textRect.width());
        p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, elided);

        if (_toggle) {
            paintToggle(p);
        }
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton) {
            _pressed = true;
            update();
        }
        QWidget::mousePressEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent *e) override {
        const auto wasPressed = _pressed;
        _pressed = false;
        update();

        if (!wasPressed || e->button() != Qt::LeftButton) {
            QWidget::mouseReleaseEvent(e);
            return;
        }
        if (!rect().contains(e->pos())) {
            QWidget::mouseReleaseEvent(e);
            return;
        }

        if (_toggle) {
            setToggled(!_checked);
        }
        emit clicked();
        QWidget::mouseReleaseEvent(e);
    }

    void enterEvent(QEnterEvent *e) override {
        _hovered = true;
        update();
        QWidget::enterEvent(e);
    }

    void leaveEvent(QEvent *e) override {
        _hovered = false;
        _pressed = false;
        update();
        QWidget::leaveEvent(e);
    }

private:
    void setToggled(bool toggled) {
        if (!_toggle) {
            return;
        }
        if (_checked == toggled
            && (_toggleAnimation->state() == QAbstractAnimation::Stopped)) {
            return;
        }

        _checked = toggled;
        const auto to = _checked ? 1.0 : 0.0;
        if (_toggleAnimation) {
            _toggleAnimation->stop();
            _toggleAnimation->setStartValue(_togglePosition);
            _toggleAnimation->setEndValue(to);
            _toggleAnimation->start();
        } else {
            _togglePosition = to;
            update();
        }
        emit toggledChanged(_checked);
    }

    void paintToggle(QPainter &p) {
        // Toggle switch paint with animation support.
        PainterHighQualityEnabler hq(p);

        const auto toggled = _togglePosition;
        const auto border = st::settingsToggleBorder;
        const auto diameter = st::settingsToggleDiameter;
        const auto extraW = st::settingsToggleExtraWidth;
        const auto shift = st::settingsToggleShift;
        const auto fullWidth = diameter + extraW;
        const auto innerDiameter = diameter - 2 * shift;
        const auto innerRadius = innerDiameter / 2.0;

        const auto baseX = width() - st::mainMenuRowRightPadding - st::settingsToggleWidth;
        const auto baseY = (height() - st::settingsToggleHeight) / 2;
        const auto left = baseX + border;
        const auto top = baseY + border;
        const auto knobLeft = left + qRound((fullWidth - diameter) * toggled);

        // Interpolate colors.
        auto lerpColor = [](const QColor &a, const QColor &b, double t) {
            return QColor(
                qRound(a.red() + (b.red() - a.red()) * t),
                qRound(a.green() + (b.green() - a.green()) * t),
                qRound(a.blue() + (b.blue() - a.blue()) * t),
                qRound(a.alpha() + (b.alpha() - a.alpha()) * t));
        };
        const auto fgColor = lerpColor(st::settingsToggleUntoggledFg, st::settingsToggleToggledFg, toggled);
        const auto bgColor = lerpColor(st::settingsToggleUntoggledBg, st::settingsToggleToggledBg, toggled);

        // Background track.
        const QRectF bgRect(left + shift, top + shift, fullWidth - 2 * shift, innerDiameter);
        p.setPen(Qt::NoPen);
        p.setBrush(fgColor);
        p.drawRoundedRect(bgRect, innerRadius, innerRadius);

        // Knob circle.
        const QRectF fgRect(knobLeft, top, diameter, diameter);
        auto pen = QPen(fgColor);
        pen.setWidth(border);
        p.setPen(pen);
        p.setBrush(bgColor);
        p.drawEllipse(fgRect);
    }

    QString _label;
    QString _iconName;
    bool _attention = false;
    bool _toggle = false;
    bool _roundIcon = false;
    bool _accountMetrics = false;
    bool _hovered = false;
    bool _pressed = false;
    bool _checked = false;
    qreal _togglePosition = 0.0;
    QVariantAnimation *_toggleAnimation = nullptr;
};

class MainMenuHeader final : public QWidget {
    Q_OBJECT
public:
    explicit MainMenuHeader(AppController *controller, QWidget *parent = nullptr)
    : QWidget(parent)
    , _controller(controller)
    , _bridge(controller ? controller->bridge() : nullptr) {
        setFixedHeight(st::mainMenuCoverHeight);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        // The whole cover toggles the account switcher, so it reads as clickable.
        setCursor(Qt::PointingHandCursor);

        // Active/filled verify button painted with live st:: colors (the
        // pointers track theme changes), replacing the former frozen QSS.
        ::Ui::TextButton::Style verifyStyle;
        verifyStyle.bg = &st::activeButtonBg;
        verifyStyle.bgOver = &st::activeButtonBgOver;
        verifyStyle.fg = &st::activeButtonFg;
        verifyStyle.radius = st::mainMenuVerifyButtonRadius;
        verifyStyle.height = st::mainMenuVerifyButtonHeight;
        verifyStyle.paddingH = st::mainMenuVerifyButtonHorizontalPadding;
        _verifyButton = new ::Ui::TextButton(
            tr("Verify session"), verifyStyle, this);
        _verifyButton->hide();
        _verifyButton->setToolTip(
            tr("Verify this session to access encrypted messages"));
        _verifyButton->setFont(st::baseFont(13, true));
        QObject::connect(_verifyButton, &QAbstractButton::clicked, this, [this] {
            if (onVerifySessionClick) {
                onVerifySessionClick();
            }
        });
        updateVerifyButtonGeometry();

        // Repaint verify button on theme change — its colors are read live
        // from st:: via the style pointers, so no stylesheet re-application.
        // The menu can be opened before the session has restored, when there is
        // no identity to show yet; repaint once it arrives instead of sitting on
        // the "unknown user" placeholder until the menu is reopened.
        if (_controller) {
            QObject::connect(
                _controller,
                &AppController::activeAccountProfileChanged,
                this,
                [this] {
                    _requestedAvatarUrl.clear(); // let the new avatar resolve
                    update();
                });
        }

        if (_controller) {
            if (auto *tm = _controller->themeManager()) {
                QObject::connect(tm, &Theme::ThemeManager::themeChanged,
                    this, [this](bool, Theme::ThemeMode) {
                        if (_verifyButton) {
                            _verifyButton->update();
                        }
                        update();
                    });
            }
        }

        if (_bridge) {
            QObject::connect(_bridge, &ProtocolBridge::mediaResolved, this,
                [this](bool success, const QString &mxcUrl, [[maybe_unused]] const QString &localPath) {
                    if (!success || !_controller) {
                        return;
                    }
                    if (mxcUrl == _controller->avatarUrl()) {
                        update();
                    }
                });
            QObject::connect(_bridge, &ProtocolBridge::encryptionOverviewReady, this,
                [this](bool success, const EncryptionOverview &overview) {
                    const bool shouldShow = success
                        && overview.healthState == EncryptionHealthState::VerifyThisSession;
                    _verifyButton->setVisible(shouldShow);
                    updateVerifyButtonGeometry();
                });
            requestEncryptionOverview();
        }
    }

    std::function<void()> onVerifySessionClick;
    std::function<void()> onToggleAccounts;

    /// Point the chevron up (accounts shown) or down (hidden).
    void setAccountsExpanded(bool expanded) {
        if (_accountsExpanded == expanded) {
            return;
        }
        _accountsExpanded = expanded;
        update();
    }

protected:
    void showEvent(QShowEvent *e) override {
        QWidget::showEvent(e);
        requestEncryptionOverview();
    }

    void mouseReleaseEvent(QMouseEvent *e) override {
        // Anywhere on the cover toggles the list, as in tdesktop — the chevron
        // marks the affordance rather than being the only target.
        if (e->button() == Qt::LeftButton && rect().contains(e->pos())
                && onToggleAccounts) {
            onToggleAccounts();
        }
        QWidget::mouseReleaseEvent(e);
    }

    void resizeEvent(QResizeEvent *e) override {
        QWidget::resizeEvent(e);
        updateVerifyButtonGeometry();
    }

    void paintEvent(QPaintEvent *) override {
        maybeResolveAvatar();

        QPainter p(this);
        p.fillRect(rect(), st::windowBg);

        const auto name = displayNameFor(_controller);
        const auto userLine = secondaryLineFor(_controller);
        const auto userId = _controller ? _controller->userId() : QString();
        const auto avatarUrl = _controller ? _controller->avatarUrl() : QString();

        const auto dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
        const auto avatar = MediaCache::loadAvatarPixmap(
            avatarUrl,
            st::mainMenuAvatarSize,
            dpr);
        if (!avatar.isNull()) {
            p.drawPixmap(
                st::mainMenuAvatarLeft,
                st::mainMenuAvatarTop,
                avatar);
        } else {
            Ui::EmptyUserpic::paint(p, userId.isEmpty() ? name : userId, name, st::mainMenuAvatarLeft, st::mainMenuAvatarTop, st::mainMenuAvatarSize);
        }

        // The Matrix ID is the only identity line here — it says exactly which
        // account this is, which a display name shared between accounts does not.
        // (The avatar still takes its initials from the display name.)
        p.setPen(st::mainMenuHeaderNameFg);
        p.setFont(st::mainMenuHeaderNameFont());
        // Room is left for the chevron (and its collapsed-state badge) so a long
        // id elides instead of running underneath them.
        const auto toggleReserve = st::mainMenuToggleRight
            + st::mainMenuToggleSize
            + st::mainMenuToggleBadgeSkip;
        const auto idRect = QRect(
            st::mainMenuNameLeft,
            st::mainMenuNameTop,
            qMax(0, width() - st::mainMenuNameLeft - toggleReserve),
            QFontMetrics(st::mainMenuHeaderNameFont()).height());
        const auto elidedId = QFontMetrics(st::mainMenuHeaderNameFont()).elidedText(
            userLine,
            Qt::ElideRight,
            idRect.width());
        p.drawText(idRect, Qt::AlignLeft | Qt::AlignVCenter, elidedId);

        paintAccountsToggle(p);
    }

private:
    // tdesktop's ToggleAccountsButton: a flip-arrow that points down when the
    // account list is hidden, plus — while it is hidden — a badge carrying the
    // unread of the OTHER accounts, since their rows aren't on screen to show it.
    void paintAccountsToggle(QPainter &p) {
        if (!_controller) {
            return;
        }
        const auto &domain = _controller->domain();
        const auto middle = st::mainMenuNameTop
            + QFontMetrics(st::mainMenuHeaderNameFont()).height() / 2;
        const auto size = st::mainMenuToggleSize;

        // The chevron is anchored to the right edge and does NOT move between the
        // two states: the badge is laid out to its left, so expanding the list
        // can't make the arrow jump sideways.
        const auto chevronRight = width() - st::mainMenuToggleRight;
        const auto centre = QPointF(chevronRight - size / 2.0, middle);

        if (!_accountsExpanded) {
            int others = 0;
            for (int i = 0; i < domain.count(); ++i) {
                if (i == domain.activeIndex()) {
                    continue;
                }
                if (const auto store = domain.account(i)->unreadStateStore()) {
                    others += store->totalUnreadCount(
                        _controller->settings().includeMutedInBadge());
                }
            }
            if (others > 0) {
                DialogsLayout::paintUnreadBadge(
                    p,
                    others,
                    /*muted=*/false,
                    chevronRight - size - st::mainMenuToggleBadgeSkip,
                    middle - st::dialogsUnreadHeight / 2,
                    /*active=*/false,
                    /*selected=*/false);
            }
        }

        const auto rise = size / 2.0;
        QPainterPath path;
        if (_accountsExpanded) {
            path.moveTo(centre.x() - size / 2.0, centre.y() + rise / 2.0);
            path.lineTo(centre.x(), centre.y() - rise / 2.0);
            path.lineTo(centre.x() + size / 2.0, centre.y() + rise / 2.0);
        } else {
            path.moveTo(centre.x() - size / 2.0, centre.y() - rise / 2.0);
            path.lineTo(centre.x(), centre.y() + rise / 2.0);
            path.lineTo(centre.x() + size / 2.0, centre.y() - rise / 2.0);
        }
        auto pen = QPen(st::mainMenuRowIcon);
        pen.setWidthF(1.5);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        p.restore();
    }

    void maybeResolveAvatar() {
        if (!_controller || !_bridge) {
            return;
        }

        const auto avatarUrl = _controller->avatarUrl();
        if (!avatarUrl.startsWith(QStringLiteral("mxc://"))) {
            return;
        }
        if (_requestedAvatarUrl == avatarUrl) {
            return;
        }
        _requestedAvatarUrl = avatarUrl;

        if (MediaCache::needsResolution(avatarUrl)) {
            MediaCache::markRequested(avatarUrl);
            _bridge->resolveAvatar(avatarUrl);
        }
    }

    void requestEncryptionOverview() {
        if (_bridge) {
            _bridge->getEncryptionOverview();
        }
    }

    void updateVerifyButtonGeometry() {
        if (!_verifyButton) {
            return;
        }
        const int avatarRight = st::mainMenuAvatarLeft + st::mainMenuAvatarSize;
        const int availableWidth = qMax(0, width() - avatarRight);
        const int buttonWidth = qMin(
            _verifyButton->sizeHint().width(),
            availableWidth);
        const int buttonX = avatarRight + ((availableWidth - buttonWidth) / 2);
        _verifyButton->setGeometry(
            buttonX,
            st::mainMenuVerifyButtonTop,
            buttonWidth,
            st::mainMenuVerifyButtonHeight);
    }

    AppController *_controller = nullptr;
    ProtocolBridge *_bridge = nullptr;
    QString _requestedAvatarUrl;
    ::Ui::TextButton *_verifyButton = nullptr;
    bool _accountsExpanded = false;
};

// One account in the switcher: avatar, name, and its own unread badge. Ported
// from tdesktop's Settings::MakeAccountButton, which the main menu and the
// settings page share.
class MainMenuAccountRow final : public QWidget {
    Q_OBJECT

public:
    MainMenuAccountRow(
        AppController *controller,
        int accountIndex,
        QWidget *parent = nullptr)
    : QWidget(parent)
    , _controller(controller)
    , _accountIndex(accountIndex) {
        setFixedHeight(st::mainMenuAccountRowHeight);
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);
    }

Q_SIGNALS:
    void clicked(int accountIndex);

protected:
    void paintEvent(QPaintEvent *) override {
        const auto account = _controller
            ? _controller->domain().account(_accountIndex)
            : nullptr;
        if (!account) {
            return;
        }

        QPainter p(this);
        p.fillRect(rect(), st::windowBg);
        if (_pressed) {
            p.fillRect(rect(), st::mainMenuRowBgDown);
        } else if (_hovered) {
            p.fillRect(rect(), st::mainMenuRowBgOver);
        }

        // The full Matrix ID, not the display name: two accounts can carry the
        // same display name, and the MXID is what actually tells them apart. The
        // persisted id is preferred because it is known before the session has
        // finished restoring.
        const auto userId = account->settings().sessionUserId().isEmpty()
            ? account->userId()
            : account->settings().sessionUserId();
        const auto name = userId.isEmpty() ? account->displayName() : userId;

        const auto avatarTop = (height() - st::mainMenuAccountAvatarSize) / 2;
        const auto dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
        const auto avatar = MediaCache::loadAvatarPixmap(
            account->avatarUrl(),
            st::mainMenuAccountAvatarSize,
            dpr);
        if (!avatar.isNull()) {
            p.drawPixmap(st::mainMenuAccountAvatarLeft, avatarTop, avatar);
        } else {
            Ui::EmptyUserpic::paint(
                p,
                userId.isEmpty() ? name : userId,
                name,
                st::mainMenuAccountAvatarLeft,
                avatarTop,
                st::mainMenuAccountAvatarSize);
        }

        // The badge is painted first so the name can be elided around it.
        auto textRight = st::mainMenuRowRightPadding;
        if (const auto store = account->unreadStateStore()) {
            const auto unread = store->totalUnreadCount(
                _controller->settings().includeMutedInBadge());
            if (unread > 0) {
                const auto badgeWidth = DialogsLayout::paintUnreadBadge(
                    p,
                    unread,
                    /*muted=*/false,
                    width() - st::mainMenuRowRightPadding,
                    (height() - st::dialogsUnreadHeight) / 2,
                    /*active=*/false,
                    /*selected=*/false);
                textRight += badgeWidth;
            }
        }

        p.setFont(st::mainMenuAccountRowFont());
        p.setPen(st::mainMenuRowText);
        const auto textRect = QRect(
            st::mainMenuAccountTextLeft,
            0,
            qMax(0, width() - st::mainMenuAccountTextLeft - textRight),
            height());
        p.drawText(
            textRect,
            Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(st::mainMenuAccountRowFont()).elidedText(
                name, Qt::ElideRight, textRect.width()));
    }

    void enterEvent(QEnterEvent *e) override {
        _hovered = true;
        update();
        QWidget::enterEvent(e);
    }

    void leaveEvent(QEvent *e) override {
        _hovered = false;
        _pressed = false;
        update();
        QWidget::leaveEvent(e);
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton) {
            _pressed = true;
            update();
        }
        QWidget::mousePressEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent *e) override {
        const auto wasPressed = _pressed;
        _pressed = false;
        update();
        if (wasPressed && e->button() == Qt::LeftButton && rect().contains(e->pos())) {
            emit clicked(_accountIndex);
        }
        QWidget::mouseReleaseEvent(e);
    }

private:
    AppController *_controller = nullptr;
    int _accountIndex = -1;
    bool _hovered = false;
    bool _pressed = false;
};

class MainMenuFooter final : public QWidget {
public:
    explicit MainMenuFooter(QWidget *parent = nullptr)
    : QWidget(parent) {
        setMinimumHeight(st::mainMenuFooterMinHeight);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), st::windowBg);

        const auto appName = appNameLabel();
        const auto version = appVersionLabel();

        const auto maxWidth = qMax(0, width() - st::mainMenuFooterLeft - st::mainMenuFooterLeft);

        p.setPen(st::mainMenuFooterLabelFg);
        p.setFont(st::mainMenuFooterNameFont());
        const auto product = QFontMetrics(st::mainMenuFooterNameFont()).elidedText(
            appName,
            Qt::ElideRight,
            maxWidth);
        p.drawText(
            st::mainMenuFooterLeft,
            height() - st::mainMenuFooterProductBottom,
            product);

        p.setPen(st::mainMenuFooterVersionFg);
        p.setFont(st::mainMenuFooterVersionFont());
        const auto build = QFontMetrics(st::mainMenuFooterVersionFont()).elidedText(
            version,
            Qt::ElideRight,
            maxWidth);
        p.drawText(
            st::mainMenuFooterLeft,
            height() - st::mainMenuFooterVersionBottom,
            build);
    }
};

class MainMenuDelimiter final : public QWidget {
public:
    explicit MainMenuDelimiter(QWidget *parent = nullptr)
    : QWidget(parent) {
        setFixedHeight(1);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), st::menuSeparatorFg);
    }
};

} // namespace

DialogsMainMenuPanel::DialogsMainMenuPanel(
    AppController *controller,
    QWidget *parent)
: QWidget(parent)
, _controller(controller) {
    setFixedWidth(st::mainMenuWidth);
    setAutoFillBackground(true);
    auto pal = palette();
    pal.setColor(QPalette::Window, st::windowBg);
    setPalette(pal);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *header = new MainMenuHeader(_controller, this);
    header->onVerifySessionClick = [this] {
        emit verifySessionClicked();
    };
    layout->addWidget(header);

    // The account switcher slides out under the cover, as in tdesktop: the other
    // accounts, then "Add Account" while there is room for one more.
    _accountsWrap = new QWidget(this);
    _accountsWrap->setAutoFillBackground(true);
    _accountsWrap->setPalette(palette());
    auto *accountsLayout = new QVBoxLayout(_accountsWrap);
    accountsLayout->setContentsMargins(0, 0, 0, 0);
    accountsLayout->setSpacing(0);
    _accountsLayout = accountsLayout;
    _accountsWrap->setMaximumHeight(0);
    layout->addWidget(_accountsWrap);

    _accountsAnimation = new QVariantAnimation(this);
    _accountsAnimation->setDuration(st::mainMenuAccountsAnimationDuration);
    _accountsAnimation->setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(_accountsAnimation, &QVariantAnimation::valueChanged, this,
        [this](const QVariant &value) {
            _accountsWrap->setMaximumHeight(value.toInt());
        });

    header->onToggleAccounts = [this, header] {
        _accountsExpanded = !_accountsExpanded;
        header->setAccountsExpanded(_accountsExpanded);
        const auto naturalHeight = rebuildAccountsList();
        _accountsAnimation->stop();
        _accountsAnimation->setStartValue(_accountsWrap->maximumHeight());
        _accountsAnimation->setEndValue(_accountsExpanded ? naturalHeight : 0);
        _accountsAnimation->start();
    };
    _header = header;

    auto *rowsScroll = new QScrollArea(this);
    rowsScroll->setFrameShape(QFrame::NoFrame);
    rowsScroll->setWidgetResizable(true);
    rowsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    rowsScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    rowsScroll->setAutoFillBackground(true);
    auto scrollPal = rowsScroll->palette();
    scrollPal.setColor(QPalette::Window, st::windowBg);
    rowsScroll->setPalette(scrollPal);
    rowsScroll->viewport()->setAutoFillBackground(true);
    rowsScroll->viewport()->setPalette(scrollPal);

    auto *rowsContainer = new QWidget(rowsScroll);
    rowsContainer->setAutoFillBackground(true);
    rowsContainer->setPalette(scrollPal);

    auto *rowsLayout = new QVBoxLayout(rowsContainer);
    rowsLayout->setContentsMargins(0, 0, 0, 0);
    rowsLayout->setSpacing(0);

    auto *newRoom = new MainMenuRow(
        tr("New Room"),
        QStringLiteral("groups"),
        false,
        false,
        rowsContainer);
    auto *newChat = new MainMenuRow(
        tr("New Chat"),
        QStringLiteral("chats"),
        false,
        false,
        rowsContainer);
    auto *exploreRooms = new MainMenuRow(
        tr("Explore"),
        QStringLiteral("explore"),
        false,
        false,
        rowsContainer);
    auto *savedMessages = new MainMenuRow(
        tr("Saved Messages"),
        QStringLiteral("saved_messages"),
        false,
        false,
        rowsContainer);
    auto *settings = new MainMenuRow(
        tr("Settings"),
        QStringLiteral("settings"),
        false,
        false,
        rowsContainer);
    auto *colorTheme = new MainMenuRow(
        tr("Color Theme"),
        QStringLiteral("palette"),
        false,
        false,
        rowsContainer);
    auto *nightMode = new MainMenuRow(
        tr("Night Mode"),
        QStringLiteral("night_mode"),
        false,
        true,
        rowsContainer);
    auto *signOut = new MainMenuRow(
        tr("Sign Out"),
        QStringLiteral("leave"),
        true,
        false,
        rowsContainer);
    auto *topDelimiter = new MainMenuDelimiter(rowsContainer);
    auto *bottomDelimiter = new MainMenuDelimiter(rowsContainer);

    rowsLayout->addWidget(topDelimiter);
    rowsLayout->addWidget(newRoom);
    rowsLayout->addWidget(newChat);
    rowsLayout->addWidget(exploreRooms);
    rowsLayout->addWidget(savedMessages);
    rowsLayout->addWidget(settings);
    rowsLayout->addWidget(colorTheme);
    rowsLayout->addWidget(nightMode);
    rowsLayout->addWidget(bottomDelimiter);
    rowsLayout->addWidget(signOut);
    rowsLayout->addStretch(1);

    rowsScroll->setWidget(rowsContainer);
    layout->addWidget(rowsScroll, 1);

    auto *footer = new MainMenuFooter(this);
    layout->addWidget(footer);

    QObject::connect(newRoom, &MainMenuRow::clicked,
        this, &DialogsMainMenuPanel::newRoomClicked);
    QObject::connect(newChat, &MainMenuRow::clicked,
        this, &DialogsMainMenuPanel::newChatClicked);
    QObject::connect(exploreRooms, &MainMenuRow::clicked,
        this, &DialogsMainMenuPanel::exploreRoomsClicked);
    QObject::connect(savedMessages, &MainMenuRow::clicked,
        this, &DialogsMainMenuPanel::savedMessagesClicked);
    QObject::connect(settings, &MainMenuRow::clicked,
        this, &DialogsMainMenuPanel::settingsClicked);
    QObject::connect(colorTheme, &MainMenuRow::clicked,
        this, &DialogsMainMenuPanel::colorThemeClicked);
    QObject::connect(nightMode, &MainMenuRow::toggledChanged,
        this, &DialogsMainMenuPanel::nightModeToggled);
    QObject::connect(signOut, &MainMenuRow::clicked,
        this, &DialogsMainMenuPanel::signOutClicked);

    _nightModeRow = nightMode;

    // Initialize toggle from current theme state and refresh on theme change.
    if (_controller) {
        if (auto *tm = _controller->themeManager()) {
            nightMode->setChecked(tm->isNight());
            QObject::connect(tm, &Theme::ThemeManager::themeChanged,
                this, [this](bool, Theme::ThemeMode) {
                    // Refresh panel background.
                    auto pal = palette();
                    pal.setColor(QPalette::Window, st::windowBg);
                    setPalette(pal);
                    // Repaint all children.
                    const auto children = findChildren<QWidget *>();
                    for (auto *child : children) {
                        child->update();
                    }
                    update();
                });
        }
    }
}

int DialogsMainMenuPanel::rebuildAccountsList() {
    if (!_accountsLayout || !_controller) {
        return 0;
    }
    while (auto *item = _accountsLayout->takeAt(0)) {
        if (auto *widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }

    // Height is summed as rows are added rather than read back from sizeHint():
    // the layout hasn't been activated yet at this point, so its hint would still
    // describe the list we just tore down (zero, the first time).
    auto naturalHeight = 1;
    _accountsLayout->addWidget(new MainMenuDelimiter(_accountsWrap));

    const auto &domain = _controller->domain();
    for (int i = 0; i < domain.count(); ++i) {
        // The active account is already the cover above; listing it again would
        // just be a row that does nothing. A session-less account is one the user
        // started adding and abandoned — it has no name to show yet.
        if (i == domain.activeIndex() || !domain.account(i)->settings().hasSession()) {
            continue;
        }
        auto *row = new MainMenuAccountRow(_controller, i, _accountsWrap);
        QObject::connect(row, &MainMenuAccountRow::clicked,
            this, &DialogsMainMenuPanel::accountSwitchRequested);
        _accountsLayout->addWidget(row);
        naturalHeight += st::mainMenuAccountRowHeight;
    }

    if (domain.canAddAccount()) {
        // Laid out like the account rows above it, not like the action rows in
        // the menu below: it belongs to this list, so its icon and label line up
        // with the avatars and names it sits under.
        auto *add = new MainMenuRow(
            tr("Add Account"),
            QStringLiteral("add_account"),
            false,
            false,
            _accountsWrap,
            /*roundIcon=*/true);
        add->useAccountRowMetrics();
        QObject::connect(add, &MainMenuRow::clicked,
            this, &DialogsMainMenuPanel::addAccountClicked);
        _accountsLayout->addWidget(add);
        naturalHeight += st::mainMenuAccountRowHeight;
    }

    _accountsLayout->activate();
    return naturalHeight;
}

QSize DialogsMainMenuPanel::sizeHint() const {
    return QSize(st::mainMenuWidth, 640);
}

void DialogsMainMenuPanel::setNightModeChecked(bool checked) {
    if (auto *row = qobject_cast<MainMenuRow *>(_nightModeRow)) {
        row->setChecked(checked);
    }
}

void DialogsMainMenuPanel::applyTheme() {
    auto pal = palette();
    pal.setColor(QPalette::Window, st::windowBg);
    setPalette(pal);
    update();
}

} // namespace TeleMatrix

#include "dialogs_main_menu_panel.moc"

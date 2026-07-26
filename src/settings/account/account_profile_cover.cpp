// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "settings/account/account_profile_cover.h"

#include "app/app_controller.h"
#include "core/core_settings.h"
#include "protocol/media_cache.h"
#include "settings/settings_common_widgets.h"
#include "styles/style_constants.h"
#include "ui/empty_userpic.h"
#include "ui/toast_widget.h"
#include "ui/widgets/buttons.h"

#include <QClipboard>
#include <QDateTime>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QTimer>

namespace TeleMatrix {
namespace {

constexpr int kAccountAvatarBadgeSize = 22;
constexpr int kAccountAvatarDeleteButtonRadius = 4;
constexpr int kAccountAvatarDeleteIconPad = 1;
constexpr int kAccountAvatarSpinnerSize = 34;
constexpr int kFullArcLength = 360 * 16;
constexpr int kQuarterArcLength = 90 * 16;

qreal avatarSpinnerPhase() {
    const auto period = qMax(1, st::radialPeriod);
    return (QDateTime::currentMSecsSinceEpoch() % period) / qreal(period);
}

QRect accountAvatarRect() {
    return QRect(
        st::settingsPhotoLeft,
        st::settingsPhotoTop,
        st::settingsPhotoSize,
        st::settingsPhotoSize);
}

QRect accountAvatarDeleteButtonRect(const QRect &avatarRect) {
    return QRect(
        avatarRect.right() - kAccountAvatarBadgeSize + 3,
        avatarRect.top() - 3,
        kAccountAvatarBadgeSize,
        kAccountAvatarBadgeSize);
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
        avatarRect.center().x() - kAccountAvatarSpinnerSize / 2,
        avatarRect.center().y() - kAccountAvatarSpinnerSize / 2,
        kAccountAvatarSpinnerSize,
        kAccountAvatarSpinnerSize);
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

void paintSendMediaDeleteButton(QPainter &p, const QRect &buttonRect, bool hovered) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(hovered
        ? st::sendMediaDeleteBgOver
        : st::sendMediaDeleteBg);
    p.drawRoundedRect(
        buttonRect,
        kAccountAvatarDeleteButtonRadius,
        kAccountAvatarDeleteButtonRadius);

    // Cross (X) remove icon — close-style glyph, replacing the bin.
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

} // namespace

AccountProfileCover::AccountProfileCover(
        AppController *controller,
        QWidget *parent)
    : QWidget(parent)
    , _controller(controller) {
    setFixedHeight(st::settingsCoverHeight);
    setMouseTracking(true);

    // Two avatar-action buttons on one line beside the avatar.
    auto buttonFont = st::baseFont(13);
    buttonFont.setWeight(QFont::DemiBold);

    ::Ui::TextButton::Style updateStyle;
    updateStyle.bg = &st::lightButtonBg;
    updateStyle.bgOver = &st::lightButtonBgOver;
    updateStyle.fg = &st::lightButtonFg;
    updateStyle.radius = st::boxRadius;
    updateStyle.height = 30;
    _updateButton = new ::Ui::TextButton(tr("Update avatar"), updateStyle, this);
    _updateButton->setFont(buttonFont);
    _updateButton->setFixedSize(
        QFontMetrics(buttonFont).horizontalAdvance(tr("Update avatar")) + 24, 30);
    connect(_updateButton, &QAbstractButton::clicked, this,
            [this] { Q_EMIT uploadAvatarRequested(); });

    ::Ui::TextButton::Style deleteStyle = updateStyle;
    deleteStyle.fg = &st::attentionButtonFg;
    _deleteButton = new ::Ui::TextButton(tr("Delete avatar"), deleteStyle, this);
    _deleteButton->setFont(buttonFont);
    _deleteButton->setFixedSize(
        QFontMetrics(buttonFont).horizontalAdvance(tr("Delete avatar")) + 24, 30);
    connect(_deleteButton, &QAbstractButton::clicked, this,
            [this] { Q_EMIT deleteAvatarRequested(); });

    _preloaderTimer = new QTimer(this);
    _preloaderTimer->setInterval(33);
    connect(_preloaderTimer, &QTimer::timeout, this, [this] {
        const auto avatarRect = accountAvatarRect();
        update(avatarRect.adjusted(-4, -4, 4, 4));
    });

    positionAvatarButtons();
}

void AccountProfileCover::positionAvatarButtons() {
    if (!_updateButton || !_deleteButton) {
        return;
    }
    const auto canEdit = canEditAvatar();
    const auto hasAvatar = !avatarUrl().isEmpty();
    _updateButton->setVisible(canEdit);
    _deleteButton->setVisible(canEdit && hasAvatar);

    const int centerY = st::settingsPhotoTop + st::settingsPhotoSize / 2;
    const int top = centerY - _updateButton->height() / 2;
    const int left = st::settingsNameLeft;
    _updateButton->move(left, top);
    _deleteButton->move(left + _updateButton->width() + 8, top);
}

void AccountProfileCover::setAccountSummary(
        const AccountSummary &summary,
        bool loaded) {
    _summary = summary;
    _summaryLoaded = loaded;
    positionAvatarButtons();
    update();
}

void AccountProfileCover::setAvatarOperationInFlight(bool inFlight) {
    if (_avatarOperationInFlight == inFlight) {
        return;
    }
    _avatarOperationInFlight = inFlight;
    if (_avatarOperationInFlight) {
        _avatarHovered = false;
        _avatarDeleteHovered = false;
        setCursor(Qt::ArrowCursor);
        _preloaderTimer->start();
    } else {
        _preloaderTimer->stop();
    }
    update();
}

QString AccountProfileCover::displayName() const {
    return _summaryLoaded
        ? _summary.displayName
        : (_controller ? _controller->displayName() : QString());
}

QString AccountProfileCover::userId() const {
    return _summaryLoaded
        ? _summary.userId
        : (_controller ? _controller->userId() : QString());
}

QString AccountProfileCover::avatarUrl() const {
    return _summaryLoaded
        ? _summary.avatarUrl
        : (_controller ? _controller->avatarUrl() : QString());
}

bool AccountProfileCover::canEditAvatar() const {
    return !_summaryLoaded || _summary.capabilities.canSetAvatarUrl;
}

void AccountProfileCover::updateAvatarHover(const QPoint &pos) {
    const auto url = avatarUrl();
    const auto avatarRect = accountAvatarRect();
    const auto deleteRect = accountAvatarDeleteButtonRect(avatarRect);
    const auto overDelete = canEditAvatar()
        && !_avatarOperationInFlight
        && !url.isEmpty()
        && deleteRect.contains(pos);
    const auto overAvatar = canEditAvatar()
        && !_avatarOperationInFlight
        && !overDelete
        && avatarRect.contains(pos);
    if (_avatarHovered == overAvatar
        && _avatarDeleteHovered == overDelete) {
        return;
    }
    _avatarHovered = overAvatar;
    _avatarDeleteHovered = overDelete;
    setCursor((overAvatar || overDelete)
        ? Qt::PointingHandCursor : Qt::ArrowCursor);
    update();
}

void AccountProfileCover::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), st::windowBg);

    const auto name = displayName();
    const auto id = userId();
    const auto url = avatarUrl();

    const int ax = st::settingsPhotoLeft;
    const int ay = st::settingsPhotoTop;
    const int as = st::settingsPhotoSize;

    bool paintedAvatar = false;
    if (!url.isEmpty()) {
        const auto avatar = MediaCache::loadImage(url);
        if (!avatar.isNull()) {
            const auto dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
            const auto scaled = avatar.scaled(
                QSize(as, as) * dpr,
                Qt::IgnoreAspectRatio,
                Qt::SmoothTransformation);
            QPainterPath clipPath;
            clipPath.addEllipse(ax, ay, as, as);
            p.save();
            p.setClipPath(clipPath);
            p.drawImage(QRect(ax, ay, as, as), scaled);
            p.restore();
            paintedAvatar = true;
        }
    }

    if (!paintedAvatar) {
        ::Ui::EmptyUserpic::paint(p, id, name, ax, ay, as);
    }

    const auto avatarRect = accountAvatarRect();
    if (_avatarOperationInFlight) {
        paintAvatarPreloader(p, avatarRect);
    }

    if (canEditAvatar()
        && !_avatarOperationInFlight
        && !url.isEmpty()
        && (_avatarHovered || _avatarDeleteHovered)) {
        paintSendMediaDeleteButton(
            p,
            accountAvatarDeleteButtonRect(avatarRect),
            _avatarDeleteHovered);
    }
}

void AccountProfileCover::resizeEvent(QResizeEvent *event) {
    positionAvatarButtons();
    QWidget::resizeEvent(event);
}

void AccountProfileCover::mouseMoveEvent(QMouseEvent *event) {
    updateAvatarHover(event->pos());
    QWidget::mouseMoveEvent(event);
}

void AccountProfileCover::leaveEvent(QEvent *event) {
    if (_avatarHovered || _avatarDeleteHovered) {
        _avatarHovered = false;
        _avatarDeleteHovered = false;
        setCursor(Qt::ArrowCursor);
        update();
    }
    QWidget::leaveEvent(event);
}

void AccountProfileCover::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    const auto url = avatarUrl();
    const auto avatarRect = accountAvatarRect();
    const auto deleteRect = accountAvatarDeleteButtonRect(avatarRect);
    if (canEditAvatar()
        && !_avatarOperationInFlight
        && !url.isEmpty()
        && deleteRect.contains(event->pos())) {
        Q_EMIT deleteAvatarRequested();
        return;
    }
    if (canEditAvatar()
        && !_avatarOperationInFlight
        && avatarRect.contains(event->pos())) {
        Q_EMIT uploadAvatarRequested();
        return;
    }
    QWidget::mousePressEvent(event);
}

} // namespace TeleMatrix

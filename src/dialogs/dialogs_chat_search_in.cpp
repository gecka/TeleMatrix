// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "dialogs_chat_search_in.h"

#include <QCoreApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include "protocol/media_cache.h"
#include "styles/style_constants.h"
#include "ui/painter.h"
#include "ui/empty_userpic.h"

namespace TeleMatrix {

namespace {

/// Load an icon stored as white-on-black RGB (no alpha).
/// Returns a premultiplied ARGB image with luminance used as alpha.
[[nodiscard]] QImage loadTdesktopIcon(const QString &basePath, qreal dpr) {
    // Tag the image with the SPRITE's scale, not the screen's. Only 1x/2x/3x
    // assets exist, so at a fractional ratio (Windows at 125%/150%) the @2x file
    // tagged 1.25 would draw 1.6x oversized. Every other icon loader in the app
    // buckets it this way; this one used to pass the raw ratio through.
    const auto bucket = (dpr > 2.0) ? 3 : (dpr > 1.0) ? 2 : 1;
    const auto suffix = (bucket == 3) ? QStringLiteral("@3x")
                      : (bucket == 2) ? QStringLiteral("@2x")
                      : QString();
    auto path = basePath;
    if (!suffix.isEmpty()) {
        path.insert(path.lastIndexOf(QLatin1Char('.')), suffix);
    }
    QImage img(path);
    if (img.isNull()) {
        return {};
    }
    auto out = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    out.setDevicePixelRatio(bucket);
    return out;
}

/// Colorize a white-on-black icon: luminance → alpha, fill with given color.
[[nodiscard]] QImage colorizeIcon(const QImage &base, const QColor &color) {
    QImage tinted = base;
    const auto r = color.red();
    const auto g = color.green();
    const auto b = color.blue();
    for (int y = 0; y < tinted.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(tinted.scanLine(y));
        for (int x = 0; x < tinted.width(); ++x) {
            const auto a = qGray(line[x]);
            line[x] = qRgba(r * a / 255, g * a / 255, b * a / 255, a);
        }
    }
    return tinted;
}

} // namespace

ChatSearchIn::ChatSearchIn(QWidget *parent)
    : Ui::RpWidget(parent)
{
    setMouseTracking(true);

    const auto dpr = devicePixelRatioF();
    _cancelIcon = loadTdesktopIcon(
        QStringLiteral(":/dialogs/cancel_search.png"), dpr);
    _chevronIcon = loadTdesktopIcon(
        QStringLiteral(":/dialogs/country_dropdown.png"), dpr);
    _chatsIcon = loadTdesktopIcon(
        QStringLiteral(":/dialogs/menu_chats.png"), dpr);
}

void ChatSearchIn::setRoom(const QString &roomId, const QString &roomName) {
    _roomId = roomId;
    _roomName = roomName;
    updateHeight();
    update();
}

void ChatSearchIn::setRoomLabel(const QString &label) {
    _roomLabel = label;
    update();
}

void ChatSearchIn::setRoomAvatar(const QString &avatarUrl) {
    _roomAvatarUrl = avatarUrl;
    update();
}

void ChatSearchIn::setRoomUseChatsIcon(bool useChatsIcon) {
    if (_roomUseChatsIcon == useChatsIcon) {
        return;
    }
    _roomUseChatsIcon = useChatsIcon;
    update();
}

void ChatSearchIn::setSender(const QString &senderId, const QString &senderName) {
    _senderId = senderId;
    _senderName = senderName;
    updateHeight();
    update();
}

void ChatSearchIn::setSenderAvatar(const QString &avatarUrl) {
    _senderAvatarUrl = avatarUrl;
    update();
}

void ChatSearchIn::clearSender() {
    _senderId.clear();
    _senderName.clear();
    _senderAvatarUrl.clear();
    _senderCloseHovered = false;
    _senderNameHovered = false;
    updateHeight();
    update();
}

int ChatSearchIn::contentHeight() const {
    if (_roomId.isEmpty()) {
        return 0;
    }
    int h = st::searchedBarHeight;       // "Search in" label bar
    h += st::dialogsSearchInHeight;      // room row
    h += 1;                              // shadow line
    if (!_senderId.isEmpty()) {
        h += st::dialogsSearchInHeight;  // sender row
        h += 1;                          // shadow line
    }
    return h;
}

void ChatSearchIn::updateHeight() {
    const auto h = contentHeight();
    setFixedHeight(h);
}

// Cancel button rect: 40×40, right-aligned, vertically centered in row.
QRect ChatSearchIn::roomCloseRect() const {
    const auto top = st::searchedBarHeight;
    const auto btnW = st::dialogsSearchInCancelWidth;
    const auto btnH = st::dialogsSearchInCancelWidth; // square
    return QRect(
        width() - btnW,
        top + (st::dialogsSearchInHeight - btnH) / 2,
        btnW,
        btnH);
}

QRect ChatSearchIn::senderCloseRect() const {
    if (_senderId.isEmpty()) {
        return {};
    }
    const auto top = st::searchedBarHeight + st::dialogsSearchInHeight + 1;
    const auto btnW = st::dialogsSearchInCancelWidth;
    const auto btnH = st::dialogsSearchInCancelWidth;
    return QRect(
        width() - btnW,
        top + (st::dialogsSearchInHeight - btnH) / 2,
        btnW,
        btnH);
}

// Name clickable area: from left edge to cancel button.
QRect ChatSearchIn::roomNameRect() const {
    const auto top = st::searchedBarHeight;
    const auto right = width() - st::dialogsSearchInCancelWidth;
    return QRect(0, top, right, st::dialogsSearchInHeight);
}

QRect ChatSearchIn::senderNameRect() const {
    if (_senderId.isEmpty()) {
        return {};
    }
    const auto top = st::searchedBarHeight + st::dialogsSearchInHeight + 1;
    const auto right = width() - st::dialogsSearchInCancelWidth;
    return QRect(0, top, right, st::dialogsSearchInHeight);
}

void ChatSearchIn::paintIcon(
    QPainter &p,
    const QImage &baseIcon,
    const QColor &color,
    int x,
    int y) const
{
    if (baseIcon.isNull()) {
        return;
    }
    auto tinted = colorizeIcon(baseIcon, color); // keeps the sprite dpr
    p.drawImage(QPointF(x, y), tinted);
}

void ChatSearchIn::paintAvatar(
    QPainter &p,
    int left,
    int top,
    int size,
    const QString &name,
    const QString &avatarUrl,
    const QString &entityId) const
{
	bool painted = false;
	if (!avatarUrl.isEmpty()) {
		const QRect avatarRect(left, top, size, size);
		const auto dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
		const auto avatar = MediaCache::loadAvatarPixmapAsync(
			avatarUrl,
			size,
			dpr,
			const_cast<ChatSearchIn*>(this),
			avatarRect);
		if (!avatar.isNull()) {
			p.drawPixmap(avatarRect.topLeft(), avatar);
			painted = true;
		}
	}
    if (!painted) {
        Ui::EmptyUserpic::paint(p, entityId.isEmpty() ? name : entityId, name, left, top, size);
    }
}

void ChatSearchIn::paintRow(
    QPainter &p,
    int rowTop,
    const QString &label,
    const QString &avatarName,
    const QString &avatarUrl,
    const QString &entityId,
    const QString &prefix,
    bool closeHovered,
    bool useChatsIcon) const
{
    const auto w = width();

    // Background.
    p.fillRect(0, rowTop, w, st::dialogsSearchInHeight, st::dialogsBg);

    // Avatar.
    const auto photoLeft = st::dialogsSearchInPhotoPadding;
    const auto photoSize = st::dialogsSearchInPhotoSize;
    const auto photoTop = rowTop + (st::dialogsSearchInHeight - photoSize) / 2;
    if (useChatsIcon) {
        if (!_chatsIcon.isNull()) {
            auto tinted = colorizeIcon(_chatsIcon, st::menuIconColor);
            const auto iconW = qRound(tinted.width() / tinted.devicePixelRatio());
            const auto iconH = qRound(tinted.height() / tinted.devicePixelRatio());
            const auto iconX = photoLeft + (photoSize - iconW) / 2;
            const auto iconY = photoTop + (photoSize - iconH) / 2;
            p.drawImage(QPoint(iconX, iconY), tinted);
        }
    } else {
        paintAvatar(p, photoLeft, photoTop, photoSize, avatarName, avatarUrl, entityId);
    }

    // Name text.
    const auto x = photoLeft + photoSize + st::dialogsSearchInSkip;
    const auto cancelW = st::dialogsSearchInCancelWidth;
    const auto chevronW = _chevronIcon.isNull()
        ? 0
        : int(_chevronIcon.width() / _chevronIcon.devicePixelRatio());
    const auto available = w
        - st::dialogsSearchInSkip
        - cancelW
        - 2 * st::dialogsSearchInDownSkip
        - chevronW
        - x;

    p.setPen(st::windowBoldFg);
    p.setFont(st::semiboldFont);
    const auto metrics = QFontMetrics(st::semiboldFont);

    const auto rowLabel = prefix.isEmpty() ? label : (prefix + label);
    const auto elided = metrics.elidedText(rowLabel, Qt::ElideRight, available);
    const auto nameY = rowTop + st::dialogsSearchInNameTop;
    p.drawText(x, nameY + metrics.ascent(), elided);

    // Chevron icon (color: windowBoldFg).
    const auto textW = qMin(metrics.horizontalAdvance(elided), available);
    const auto iconX = x + textW + st::dialogsSearchInDownSkip;
    const auto iconY = rowTop + st::dialogsSearchInDownTop;
    paintIcon(p, _chevronIcon, st::windowBoldFg, iconX, iconY);

    // Cancel button icon (color: dialogsMenuIconFg).
    const auto cancelRect = (rowTop == st::searchedBarHeight)
        ? roomCloseRect()
        : senderCloseRect();
    const auto &cancelColor = closeHovered
        ? st::menuIconFgOver
        : st::menuIconFg;
    const auto iconPos = st::dialogsSearchInCancelIconPos;
    paintIcon(p, _cancelIcon, cancelColor,
        cancelRect.x() + iconPos, cancelRect.y() + iconPos);

    // Shadow line below row.
    p.fillRect(0, rowTop + st::dialogsSearchInHeight, w, 1, st::shadowFg);
}

void ChatSearchIn::paintEvent(QPaintEvent *e) {
    QPainter p(this);

    if (_roomId.isEmpty()) {
        return;
    }

    const auto w = width();

    // 1. "Search messages in" header bar.
    p.fillRect(0, 0, w, st::searchedBarHeight, st::searchedBarBg);
    p.setFont(st::normalFont);
    p.setPen(st::searchedBarFg);
    p.drawText(
        st::searchedBarPosition.x(),
        st::searchedBarPosition.y() + QFontMetrics(st::normalFont).ascent(),
        QCoreApplication::translate("ChatSearchIn", "Search messages in"));

    // 2. Room row.
    const auto roomLabel = _roomLabel.isEmpty()
        ? QCoreApplication::translate("ChatSearchIn", "This Room")
        : _roomLabel;
    paintRow(p,
        st::searchedBarHeight,
        roomLabel,
        _roomName,
        _roomAvatarUrl,
        _roomId,
        QString(),
        _roomCloseHovered,
        _roomUseChatsIcon);

    // 3. Optional sender row.
    if (!_senderId.isEmpty()) {
        const auto senderTop = st::searchedBarHeight
            + st::dialogsSearchInHeight + 1;
        paintRow(p,
            senderTop,
            _senderName,
            _senderName,
            _senderAvatarUrl,
            _senderId,
            QCoreApplication::translate("ChatSearchIn", "From: "),
            _senderCloseHovered,
            false);
    }
}

void ChatSearchIn::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) {
        return;
    }
    const auto pos = e->pos();
    if (roomCloseRect().contains(pos)) {
        emit roomCleared();
        return;
    }
    if (!_senderId.isEmpty() && senderCloseRect().contains(pos)) {
        emit senderCleared();
        return;
    }
    if (roomNameRect().contains(pos)) {
        emit roomFilterClicked();
        return;
    }
    if (!_senderId.isEmpty() && senderNameRect().contains(pos)) {
        emit senderFilterClicked();
        return;
    }
}

void ChatSearchIn::mouseMoveEvent(QMouseEvent *e) {
    const auto pos = e->pos();
    const auto overRoomClose = roomCloseRect().contains(pos);
    const auto overSenderClose = !_senderId.isEmpty()
        && senderCloseRect().contains(pos);
    const auto overRoomName = !overRoomClose && roomNameRect().contains(pos);
    const auto overSenderName = !overSenderClose && !_senderId.isEmpty()
        && senderNameRect().contains(pos);

    bool changed = false;
    if (overRoomClose != _roomCloseHovered) {
        _roomCloseHovered = overRoomClose;
        changed = true;
    }
    if (overSenderClose != _senderCloseHovered) {
        _senderCloseHovered = overSenderClose;
        changed = true;
    }
    if (overRoomName != _roomNameHovered) {
        _roomNameHovered = overRoomName;
        changed = true;
    }
    if (overSenderName != _senderNameHovered) {
        _senderNameHovered = overSenderName;
        changed = true;
    }
    if (changed) {
        const auto anyHand = overRoomClose || overSenderClose
            || overRoomName || overSenderName;
        setCursor(anyHand ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void ChatSearchIn::leaveEvent(QEvent *e) {
    Ui::RpWidget::leaveEvent(e);
    bool changed = false;
    if (_roomCloseHovered) { _roomCloseHovered = false; changed = true; }
    if (_senderCloseHovered) { _senderCloseHovered = false; changed = true; }
    if (_roomNameHovered) { _roomNameHovered = false; changed = true; }
    if (_senderNameHovered) { _senderNameHovered = false; changed = true; }
    if (changed) {
        setCursor(Qt::ArrowCursor);
        update();
    }
}

} // namespace TeleMatrix

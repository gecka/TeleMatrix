// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "dialogs_layout.h"

#include "ui/text/emoji_text.h"
#include "dialogs_row.h"

#include <QCoreApplication>
#include <QImage>
#include <QPainterPath>
#include <QPixmap>

#include "ui/painter.h"
#include "ui/empty_userpic.h"
#include "ui/style/icon_provider.h"
#include "../protocol/media_cache.h"
#include "styles/style_dialogs.h"

namespace TeleMatrix {
namespace DialogsLayout {

namespace {

// Use runtime-scaled st:: constants instead of hardcoded pixel values.
#define kPaddingLeft   st::dialogsPadding.left()
#define kPaddingTop    st::dialogsPadding.top()
#define kPaddingRight  st::dialogsPadding.right()
#define kPhotoSize     st::dialogsPhotoSize
#define kNameLeft      st::dialogsNameLeft
#define kNameTop       st::dialogsNameTop
#define kTextLeft      st::dialogsTextLeft
#define kTextTop       st::dialogsTextTop
#define kRowHeight     st::dialogsRowHeight
#define kUnreadHeight  st::dialogsUnreadHeight
#define kUnreadPadding st::dialogsUnreadPadding
#define kUnreadMarkDiameter st::dialogsUnreadMarkDiameter
#define kDateSkip      st::dialogsDateSkip

/// Paint the date/time text in the top-right corner.
/// Adjusts rectForName width to account for the date width.
void paintRowTopRight(
    QPainter &p,
    const DialogsRow &row,
    QRect &rectForName,
    const DialogsPaintContext &context)
{
    const auto &text = row.dateText();
    const auto width = st::dialogsDateFont->width(text);
    rectForName.setWidth(rectForName.width() - width - kDateSkip);

    const auto dateLeft = rectForName.left() + rectForName.width() + kDateSkip;
    const auto baseline = rectForName.top()
        + st::semiboldFont->height
        - st::normalFont->descent;

    p.setFont(st::dialogsDateFont);
    p.setPen(context.active
        ? st::dialogsDateFgActive
        : context.selected
        ? st::dialogsDateFgOver
        : st::dialogsDateFg);
    p.drawText(dateLeft, baseline, text);
}


/// Paint a circular avatar (image if available, placeholder with first letter otherwise).
/// Uses the same MediaCache::loadAvatarPixmap as the timeline, ensuring
/// identical crop/shape rendering for the same mxc:// URL.
/// @param entityId  Stable identity key for placeholder color (user ID or room ID).
void paintAvatar(
    QPainter &p,
    const QString &name,
    const QString &avatarUrl,
    const QString &entityId,
    int x, int y, int size,
    QWidget *repaintTarget)
{
    if (!avatarUrl.isEmpty()) {
        const auto dpr = p.device()->devicePixelRatioF();
        const auto avatarPix = MediaCache::loadAvatarPixmapAsync(
            avatarUrl,
            size,
            dpr,
            repaintTarget);
        if (!avatarPix.isNull()) {
            p.drawPixmap(x, y, avatarPix);
            return;
        }
    }

    // Fallback: gradient circle with initials.
    const auto &colorKey = entityId.isEmpty() ? name : entityId;
    Ui::EmptyUserpic::paint(p, colorKey, name, x, y, size);
}

/// Paint the online badge dot on the avatar corner.
/// Draw into an offscreen QImage with CompositionMode_Source so the transparent
/// pen cuts through the avatar, creating a white-looking border.
void paintOnlineBadge(
    QPainter &p,
    int avatarX, int avatarY, int avatarSize,
    bool active,
    qreal progress)
{
    if (progress <= 0.01) return;

    const auto size = st::dialogsOnlineBadgeSize;
    const auto stroke = st::dialogsOnlineBadgeStroke;
    const auto skipX = st::dialogsOnlineBadgeSkip.x();
    const auto skipY = st::dialogsOnlineBadgeSkip.y();

    const auto badgeX = avatarSize - skipX - size;
    const auto badgeY = avatarSize - skipY - size;

    const auto frameSize = size + 2 * stroke;
    const auto dpr = p.device()->devicePixelRatioF();
    const auto pixelSize = qRound(frameSize * dpr);

    QImage frame(pixelSize, pixelSize, QImage::Format_ARGB32_Premultiplied);
    frame.setDevicePixelRatio(dpr);
    frame.fill(Qt::transparent);

    {
        QPainter q(&frame);
        q.setRenderHint(QPainter::Antialiasing, true);
        q.translate(stroke, stroke);
        q.setCompositionMode(QPainter::CompositionMode_Source);

        const auto shrink = (size / 2.0) * (1.0 - progress);

        auto pen = QPen(Qt::transparent);
        pen.setWidthF(stroke * progress);
        q.setPen(pen);
        q.setBrush(active
            ? st::dialogsOnlineBadgeFgActive
            : st::dialogsOnlineBadgeFg);
        q.drawEllipse(QRectF(0, 0, size, size)
            .marginsRemoved({shrink, shrink, shrink, shrink}));
    }

    p.drawImage(
        avatarX + badgeX - stroke,
        avatarY + badgeY - stroke,
        frame);
}

const QColor &unreadBadgeBg(
        bool muted,
        bool active,
        bool selected) {
    return muted
        ? (active
            ? st::dialogsUnreadBgMutedActive
            : selected
            ? st::dialogsUnreadBgMutedOver
            : st::dialogsUnreadBgMuted)
        : (active
            ? st::dialogsUnreadBgActive
            : selected
            ? st::dialogsUnreadBgOver
            : st::dialogsUnreadBg);
}

const QColor &unreadBadgeFg(
        bool active,
        bool selected) {
    return active
        ? st::dialogsUnreadFgActive
        : selected
        ? st::dialogsUnreadFgOver
        : st::dialogsUnreadFg;
}

int paintUnreadDot(
        QPainter &p,
        bool muted,
        int right,
        int top,
        bool active,
        bool selected) {
    const auto d = kUnreadMarkDiameter;
    const QRect dotRect(
        right - kUnreadHeight + (kUnreadHeight - d) / 2,
        top + (kUnreadHeight - d) / 2,
        d,
        d);
    {
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(unreadBadgeBg(muted, active, selected));
        p.drawEllipse(dotRect);
    }
    return kUnreadHeight + kUnreadPadding;
}

int paintPinnedIcon(
        QPainter &p,
        int right,
        int top,
        bool active,
        bool selected) {
    const auto iconColor = active
        ? st::dialogsUnreadBgMutedActive
        : selected
        ? st::dialogsUnreadBgMutedOver
        : st::dialogsUnreadBgMuted;
    const auto pinIcon = TeleMatrix::Style::IconProvider::tintedIcon(
        QStringLiteral(":/telematrix/icons/dialogs/"),
        QStringLiteral("dialogs_pinned"),
        iconColor);
    if (pinIcon.isNull()) {
        return 0;
    }
    const auto iconW = int(pinIcon.width() / pinIcon.devicePixelRatio());
    const auto iconH = int(pinIcon.height() / pinIcon.devicePixelRatio());
    const auto iconX = right - iconW;
    const auto iconY = top + (st::dialogsTextFont->height - iconH) / 2;
    p.drawImage(QPoint(iconX, iconY), pinIcon);
    return iconW + kUnreadPadding;
}

int paintMentionBadge(
        QPainter &p,
        bool muted,
        int right,
        int top,
        bool active,
        bool selected) {
    const QRect badgeRect(right - kUnreadHeight, top, kUnreadHeight, kUnreadHeight);
    {
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(unreadBadgeBg(muted, active, selected));
        p.drawEllipse(badgeRect);
    }

    const auto icon = TeleMatrix::Style::IconProvider::tintedIcon(
        QStringLiteral(":/dialogs/"),
        QStringLiteral("mention"),
        unreadBadgeFg(active, selected));
    if (!icon.isNull()) {
        const auto iconW = int(icon.width() / icon.devicePixelRatio());
        const auto iconH = int(icon.height() / icon.devicePixelRatio());
        p.drawImage(
            QPoint(
                badgeRect.left() + (badgeRect.width() - iconW) / 2,
                badgeRect.top() + (badgeRect.height() - iconH) / 2),
            icon);
    }
    return kUnreadHeight + kUnreadPadding;
}

} // namespace

void paintRow(
    QPainter &p,
    DialogsRow &row,
    const DialogsPaintContext &context)
{
    const auto rowHeight = st::dialogsRowHeight;

    // 1. Background.
    if (context.active) {
        p.fillRect(0, 0, context.width, rowHeight, st::dialogsBgActive);
    } else if (context.selected) {
        p.fillRect(0, 0, context.width, rowHeight, st::dialogsBgOver);
    }

    // 2. Avatar (circular, 46px).
    if (context.savedMessages) {
        // The drawn bookmark always wins over any uploaded room avatar.
        Ui::EmptyUserpic::paintSavedMessages(
            p, kPaddingLeft, kPaddingTop, kPhotoSize);
    } else {
        paintAvatar(
            p,
            row.displayName(),
            row.avatarUrl(),
            row.avatarEntityId(),
            kPaddingLeft,
            kPaddingTop,
            kPhotoSize,
            context.repaintTarget);
    }

    // Online badge (green dot on DM avatars).
    if (row.isPeerOnline()) {
        paintOnlineBadge(
            p, kPaddingLeft, kPaddingTop, kPhotoSize,
            context.active,
            row.onlineBadgeProgress());
    }

    // 3. Calculate name rect.
    auto nameWidth = context.width - kNameLeft - kPaddingRight;
    auto rectForName = QRect(kNameLeft, kNameTop, nameWidth, st::semiboldFont->height);

    // 3b. Chat type icon for groups, offset (1px, 4px) from the name rect's
    // top-left. Saved Messages shows none (tdesktop: bare title, like a DM).
    if (!row.isDirect() && !context.savedMessages) {
        const auto &iconColor = context.active
            ? st::dialogsChatIconFgActive
            : context.selected
                ? st::dialogsChatIconFgOver
                : st::dialogsChatIconFg;
        const auto icon = TeleMatrix::Style::IconProvider::tintedIcon(
            QStringLiteral(":/dialogs/"), QStringLiteral("chat_type"), iconColor);
        if (!icon.isNull()) {
            const auto iconW = int(icon.width() / icon.devicePixelRatio());
            // Offset (1px, 4px) from rectForName.topLeft().
            p.drawImage(QPoint(rectForName.left() + 1, rectForName.top() + 4), icon);
            rectForName.setLeft(rectForName.left() + iconW + st::dialogsChatTypeSkip);
        }
    }

    // 4. Date in top-right.
    if (!row.dateText().isEmpty()) {
        paintRowTopRight(p, row, rectForName, context);
    }

    // 5. Message preview at (textLeft, textTop).
    auto textWidth = context.width - kTextLeft - kPaddingRight;

    // 6. Unread badge, unread dot, pinned icon, and mention badge.
    const auto badgeTop = kTextTop
        + st::dialogsTextFont->ascent
        - st::dialogsUnreadFont->ascent
        - (kUnreadHeight - st::dialogsUnreadFont->height) / 2;
    // Regular chat mention badges stay in the active (unmuted) style even when the chat is muted.
    const auto mentionMuted = false;
    if (row.unreadCount() > 0) {
        const auto used = paintUnreadBadge(
            p,
            row.unreadCount(),
            row.isMuted(),
            context.width - kPaddingRight,
            badgeTop,
            context.active,
            context.selected);
        textWidth -= used;
        auto badgeRight = context.width - kPaddingRight - used;
        if (row.hasMentionBadge()) {
            const auto mentionUsed = paintMentionBadge(
                p,
                mentionMuted,
                badgeRight,
                badgeTop,
                context.active,
                context.selected);
            textWidth -= mentionUsed;
        }
    } else if (row.isMarkedUnread()) {
        // Unread dot (no numeric count).
        const auto used = paintUnreadDot(
            p,
            row.isMuted(),
            context.width - kPaddingRight,
            badgeTop,
            context.active,
            context.selected);
        textWidth -= used;
        if (row.hasMentionBadge()) {
            const auto mentionUsed = paintMentionBadge(
                p,
                mentionMuted,
                context.width - kPaddingRight - used,
                badgeTop,
                context.active,
                context.selected);
            textWidth -= mentionUsed;
        }
    } else if (row.isPinned()) {
        const auto used = paintPinnedIcon(
            p,
            context.width - kPaddingRight,
            kTextTop,
            context.active,
            context.selected);
        textWidth -= used;
        if (row.hasMentionBadge()) {
            const auto mentionUsed = paintMentionBadge(
                p,
                mentionMuted,
                context.width - kPaddingRight - used,
                badgeTop,
                context.active,
                context.selected);
            textWidth -= mentionUsed;
        }
    } else if (row.hasMentionBadge()) {
        const auto used = paintMentionBadge(
            p,
            mentionMuted,
            context.width - kPaddingRight,
            badgeTop,
            context.active,
            context.selected);
        textWidth -= used;
    }

    // 7. Draw the name text (ellipsis).
    p.setFont(st::semiboldFont);
    p.setPen(context.active
        ? st::dialogsNameFgActive
        : context.selected
        ? st::dialogsNameFgOver
        : st::dialogsNameFg);
    row.nameText().draw(p, {
        .position = rectForName.topLeft(),
        .availableWidth = rectForName.width(),
        .elisionLines = 1,
    });

    // 8. Draw the message preview text (or Draft: prefix, or typing indicator).
    p.setFont(st::dialogsTextFont);
    const auto previewColor = context.active
        ? st::dialogsTextFgActive
        : context.selected
        ? st::dialogsTextFgOver
        : st::dialogsTextFg;
    const auto draftColor = context.active
        ? st::dialogsDraftFgActive
        : context.selected
        ? st::dialogsDraftFgOver
        : st::dialogsDraftFg;
    const auto senderColor = context.active
        ? st::dialogsTextFgServiceActive
        : context.selected
        ? st::dialogsTextFgServiceOver
        : st::dialogsTextFgService;
    const auto baseline = kTextTop + st::dialogsTextFont->ascent;
    // Previews carry whatever the sender typed, so they need atlas emoji like any
    // other running text — otherwise they are blank on Linux and mismatched elsewhere.
    const auto &emojiMetrics = TeleMatrix::EmojiText::CachedMetricsFor(
        st::dialogsTextFont,
        st::emojiInlineSlot,
        st::emojiInlineGlyph);
    if (row.membership() == MembershipState::Invite) {
        // Invite preview: show "Invited by <name>" in accent blue.
        p.setFont(st::dialogsTextFont);
        p.setPen(context.active
            ? st::dialogsTextFgActive
            : st::dialogsTextFgService);
        const auto inviteText = row.inviterDisplayName().isEmpty()
            ? QCoreApplication::translate("DialogsLayout", "New chat invitation")
            : QCoreApplication::translate("DialogsLayout", "Invited by %1").arg(row.inviterDisplayName());
        TeleMatrix::EmojiText::DrawElided(
            p, kTextLeft, baseline, textWidth, inviteText, emojiMetrics);
    } else if (row.hasDraft()) {
        const auto draftPrefix = QCoreApplication::translate("DialogsLayout", "Draft: ");
        p.setFont(st::dialogsTextFont);
        p.setPen(draftColor);
        const auto prefixWidth = TeleMatrix::EmojiText::DrawLine(
            p, kTextLeft, baseline, draftPrefix, emojiMetrics);

        p.setPen(previewColor);
        TeleMatrix::EmojiText::DrawElided(
            p,
            kTextLeft + prefixWidth,
            baseline,
            qMax(0, textWidth - prefixWidth),
            row.draftText(),
            emojiMetrics);
    } else {
        // Saved Messages is a self-chat: every message is outgoing, so a
        // "You:" prefix would sit on every preview forever — show bare text.
        const auto senderLabel = context.savedMessages
            ? QString()
            : row.isLastMessageOutgoing()
                ? QCoreApplication::translate("DialogsLayout", "You")
                : row.lastSender();
        const auto prefix = (senderLabel.isEmpty() || row.lastMessage().isEmpty())
            ? QString()
            : (senderLabel + QStringLiteral(": "));
        const auto fullPreview = prefix + row.lastMessage();
        const auto elidedPreview = TeleMatrix::EmojiText::Elide(
            fullPreview,
            st::dialogsTextFont,
            emojiMetrics,
            textWidth);
        if (!prefix.isEmpty() && elidedPreview.startsWith(prefix)) {
            p.setPen(senderColor);
            const auto prefixWidth = TeleMatrix::EmojiText::DrawLine(
                p, kTextLeft, baseline, prefix, emojiMetrics);
            p.setPen(previewColor);
            TeleMatrix::EmojiText::DrawLine(
                p,
                kTextLeft + prefixWidth,
                baseline,
                elidedPreview.mid(prefix.size()),
                emojiMetrics);
        } else {
            p.setPen(previewColor);
            TeleMatrix::EmojiText::DrawLine(
                p, kTextLeft, baseline, elidedPreview, emojiMetrics);
        }
    }
}

int paintUnreadBadge(
    QPainter &p,
    int count,
    bool muted,
    int right,
    int top,
    bool active,
    bool selected)
{
    const auto text = QString::number(count);
    const auto textWidth = st::dialogsUnreadFont->width(text);
    const auto badgeWidth = qMax(kUnreadHeight, textWidth + 2 * kUnreadPadding);
    const auto badgeLeft = right - badgeWidth;
    const auto radius = kUnreadHeight / 2;

    // Badge background.
    {
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(unreadBadgeBg(muted, active, selected));
        p.drawRoundedRect(badgeLeft, top, badgeWidth, kUnreadHeight, radius, radius);
    }

    // Badge text.
    p.setFont(st::dialogsUnreadFont);
    p.setPen(unreadBadgeFg(active, selected));
    p.drawText(
        badgeLeft + (badgeWidth - textWidth) / 2,
        top + (kUnreadHeight - st::dialogsUnreadFont->height) / 2 + st::dialogsUnreadFont->ascent,
        text);

    return badgeWidth + kUnreadPadding;
}

} // namespace DialogsLayout
} // namespace TeleMatrix

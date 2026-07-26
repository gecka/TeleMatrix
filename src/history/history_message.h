// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include "../ui/style/runtime_scale.h"
#include <QRect>
#include <QString>
#include <QVector>
#include <QWidget>

#include "../protocol/protocol_types.h"
#include "../styles/style_constants.h"
#include "message_index.h"

namespace TeleMatrix {

class HistoryInlineVideoPlayer;

/// Resolves a reply-parent TimelineItem by event id via the timeline's index +
/// item vector — no per-message copy (this replaced the old full-item by-id map).
/// find() returns nullptr when the id isn't in the loaded window.
struct TimelineItemLookup {
    const MessageIndex *index = nullptr;
    const QVector<TimelineItem> *items = nullptr;
    [[nodiscard]] const TimelineItem *find(const QString &eventId) const {
        if (!index || !items) {
            return nullptr;
        }
        const auto i = index->physicalIndexOf(eventId);
        return (i >= 0 && i < items->size()) ? &(*items)[i] : nullptr;
    }
};

/// Audio playback state passed from HistoryList to paint functions.
struct AudioPlaybackState {
    QString playingEventId;
    qint64 positionMs = 0;
    qint64 durationMs = 0;
    bool isPaused = false;
};

/// Rendering context for a message bubble.
struct MessagePaintContext {
    int width = 0;         // available width for the bubble
    bool isGroup = false;  // show sender name for group chats
    bool showOutgoingPrivateAvatar = false; // show self avatars for outgoing DM messages
    bool sameSenderAbove = false; // collapse sender name if same sender above
    bool sameSenderBelow = false; // next message is same sender (avatar on last)
    bool isHovered = false; // mouse is over this message
    bool hoveredFastReply = false; // mouse is over inline fast-reply action
    int hoveredLinkStart = -1; // char offset of hovered link (-1 = none)
    int selectionStart = -1;   // start of text selection (-1 = none)
    int selectionEnd = -1;     // end of text selection (-1 = none)
    bool selectionMode = false; // message-level selection mode
    bool messageSelected = false; // this message is selected in selection mode
    qreal sendingAnimationProgress = 0.0; // 0..1 phase for sending clock
    const TimelineItemLookup *timelineIndex = nullptr; // reply-parent lookup by event id (index+vector, no copy)
    const AudioPlaybackState *audioState = nullptr; // audio playback state (null if no player)
    double voiceSeekHoverProgress = -1.0; // 0..1 over active voice waveform, -1 if none
    bool largeEmojiEnabled = true; // from settings: render isolated emoji as large
    bool itemGlowActive = false; // this UTD item is in the decrypting/glow state (utdState==0, not safety-expired)
    qreal decryptingGlowProgress = 0.0; // 0..1 within the 2s glow cycle (slide 0..0.5, wait 0.5..1)
    bool urlPreviewFetching = false; // glow this message's URL while its link-preview is being fetched
    QWidget *paintTarget = nullptr; // widget for async repaint scheduling
    QRect repaintTargetRect; // row/widget rect to repaint for async media and animations
    // True while the list is actively scrolling: suppress per-row animation repaint
    // scheduling (glow, sending clock, UTD). The scroll blits moved rows cheaply, so
    // forcing them to repaint every frame defeats that and janks the scroll; the
    // time-based phases simply resume when scrolling settles.
    bool suppressAnimationScheduling = false;
    HistoryInlineVideoPlayer *inlineVideo = nullptr; // active inline video player (null if none)
    // Redaction in flight: the row is dimmed by the caller and inert, so it must
    // not offer affordances (play, seek, fullscreen) it will no longer honour.
    bool deleting = false;
};

/// Static helper class that paints a single message bubble.
/// Reimplements the text-only subset of message-bubble rendering.
namespace HistoryMessage {

/// Pixel-perfect chat-style values.
/// Non-constexpr: scaled by st::initPxValues() at startup.
inline int kMaxBubbleWidth = 430;   // msgMaxWidth
inline int kMinBubbleWidth = 160;   // msgMinWidth
inline int kBubblePaddingH = 11;    // msgPadding left/right
inline int kBubblePaddingV = 8;     // msgPadding top/bottom
inline int kBubbleShadow = 2;       // msgShadow
inline int kBubbleTailWidth = 6;    // historyBubbleTail width
inline int kBubbleTailHeight = 10;  // historyBubbleTail height
inline int kPhotoSize = 33;         // msgPhotoSize
inline int kPhotoSkip = 40;         // msgPhotoSkip
inline int kMarginLeft = 16;        // msgMargin.left
inline int kMarginRight = 56;       // msgMargin.right
inline int kMarginTop = 6;          // msgMargin.top
inline int kMarginTopAttached = 0;  // msgMarginTopAttached
inline int kMarginBottom = 2;       // msgMargin.bottom
/// pill height = padding.top + reactionInlineSize + padding.bottom.
inline int reactionPillHeight() {
    return st::reactionInlinePadding.top()
        + st::reactionInlineSize
        + st::reactionInlinePadding.bottom();
}

/// Apply interface scale to message layout constants.
/// Called from AppController after st::initPxValues().
inline void initMessagePxValues() {
    using TeleMatrix::Style::ConvertScale;
    kMaxBubbleWidth = ConvertScale(430);
    kMinBubbleWidth = ConvertScale(160);
    kBubblePaddingH = ConvertScale(11);
    kBubblePaddingV = ConvertScale(8);
    kBubbleShadow = ConvertScale(2);
    kBubbleTailWidth = ConvertScale(6);
    kBubbleTailHeight = ConvertScale(10);
    kPhotoSize = ConvertScale(33);
    kPhotoSkip = ConvertScale(40);
    kMarginLeft = ConvertScale(16);
    kMarginRight = ConvertScale(56);
    kMarginTop = ConvertScale(6);
    // kMarginTopAttached stays 0
    kMarginBottom = ConvertScale(2);
}
constexpr int kDateSpace = 12;         // msgDateSpace
constexpr int kDateDeltaX = 2;        // msgDateDelta.x()
constexpr int kDateDeltaY = 5;        // msgDateDelta.y()
constexpr int kSendStateSpace = 24;
constexpr int kSenderNameHeight = 20;
// chat-style reply-preview height.
constexpr int kReplyPreviewHeight = 32;
// chat-style reply-preview bottom skip.
constexpr int kReplyPreviewBottomSkip = 2;

/// Calculate the total height of a message bubble (for layout purposes).
int bubbleHeight(
    const TimelineItem &item,
    int maxWidth,
    const MessagePaintContext &context);

/// Returns true when the sender row should be shown above this message.
/// Matches TeleMatrix media rules:
/// pure file/audio/voice bubbles hide the sender row,
/// while captioned file bubbles keep it.
bool showSenderName(
    const TimelineItem &item,
    const MessagePaintContext &context);

/// Paint a single message bubble at the current painter position.
/// The painter should be translated to the message's y-offset.
void paint(
    QPainter &p,
    const TimelineItem &item,
    const MessagePaintContext &context);

/// Convert a point (relative to message origin) to a cursor position
/// in the message text.  Returns -1 if the point is outside the text area.
/// With `clamp=true`, clamps to the nearest line edge (for drag selection).
int cursorAt(
    const TimelineItem &item,
    const MessagePaintContext &context,
    QPoint pos,
    bool clamp = false);

/// Returns true if `pos` is over the body text area (not timestamp/buttons).
bool isOverText(
    const TimelineItem &item,
    const MessagePaintContext &context,
    QPoint pos);

/// Extract plain text between cursor positions `start` and `end`.
QString selectedText(
    const TimelineItem &item,
    int start,
    int end);

/// Return the resolved plain text for a message (for word-boundary detection).
QString plainText(const TimelineItem &item);

/// Hit-test a point (relative to message origin) against links.
/// Returns the link URL if the point is over a link, or empty string.
/// Also sets `outStart` to the link's character offset (for hover painting).
QString linkAt(
    const TimelineItem &item,
    const MessagePaintContext &context,
    QPoint pos,
    int &outStart);

/// Hit-test a point against the timestamp area.
/// Returns true if the point is over the timestamp + checkmarks region.
bool timestampAt(
    const TimelineItem &item,
    const MessagePaintContext &context,
    QPoint pos);

/// Hit-test a point against code block copy buttons.
/// Returns the plain text content of the code block if the point
/// hits a copy button header, or empty string otherwise.
QString codeBlockCopyAt(
    const TimelineItem &item,
    const MessagePaintContext &context,
    QPoint pos);

/// Hit-test a point against reaction pills in the bubble.
/// Returns the reaction key when a pill is hit, or empty otherwise.
QString reactionPillAt(
    const TimelineItem &item,
    const MessagePaintContext &context,
    QPoint pos);

/// Hit-test a point against the link preview card.
/// Returns the preview URL when the card is clicked, or empty otherwise.
QString linkPreviewUrlAt(
    const TimelineItem &item,
    const MessagePaintContext &context,
    QPoint pos);

/// Hit-test a point against the reply preview area.
/// Returns referenced event id when clicked, or empty otherwise.
QString replyTargetAt(
    const TimelineItem &item,
    const MessagePaintContext &context,
    QPoint pos);

/// Returns true if this message should show a floating reply pill button.
bool hasFastReplyAction(const TimelineItem &item, const MessagePaintContext &context);

/// Paint the floating reply pill button.
/// Call from the list paint loop AFTER the bubble is painted.
void paintFastReplyButton(
    QPainter &p,
    const TimelineItem &item,
    const MessagePaintContext &context);

/// Get the reply pill button rect (for animation center).
QRect fastReplyRect(
    const TimelineItem &item,
    const MessagePaintContext &context);

/// Hit-test a point against the reply pill button.
bool fastReplyAt(
    const TimelineItem &item,
    const MessagePaintContext &context,
    QPoint pos);

// --- Hover reaction button + in-place vertical expansion ---
// Stateless paint model (state lives in HistoryList). No animation: the
// resting button and the expanded column are drawn directly.

/// Whether this message shows the hover reaction button (incoming + outgoing).
bool hasReactionButton(
    const TimelineItem &item,
    const MessagePaintContext &context);

/// Resting reaction button outer rect (incl. shadow) at the bubble's bottom
/// corner, in local message coordinates. Empty if not applicable.
QRect reactionButtonRect(
    const TimelineItem &item,
    const MessagePaintContext &context);

/// Paint the resting reaction button (rounded pill + shadow + one emoji).
void paintReactionButton(
    QPainter &p,
    const TimelineItem &item,
    const MessagePaintContext &context);

/// Full (unclamped) inner content height of the column for `count` cells.
int reactionColumnContentInnerHeight(int count);
/// Visible inner height (capped at reactionCornerAddedHeightMax beyond cell 1).
int reactionColumnVisibleInnerHeight(int count);
/// Max scroll offset within the column (0 when it fits without scrolling).
int reactionColumnScrollMax(int count);

/// Expanded column outer rect (incl. shadow) in local message coordinates,
/// anchored at the resting button and growing up or down.
QRect reactionColumnRect(
    const TimelineItem &item,
    const MessagePaintContext &context,
    int count,
    bool expandUp);

/// Paint the expanded vertical reaction column (no animation; wheel-scrolled).
void paintReactionColumn(
    QPainter &p,
    const TimelineItem &item,
    const MessagePaintContext &context,
    const QVector<QString> &emojis,
    int scroll,
    int hoveredIndex,
    bool expandUp);

/// Hit-test a point (local coords) against the expanded column's cells.
/// Returns the cell index, or -1.
int reactionColumnCellAt(
    const TimelineItem &item,
    const MessagePaintContext &context,
    int count,
    int scroll,
    bool expandUp,
    QPoint pos);

/// Bubble rectangle in local message coordinates (without outer row margins).
QRect bubbleRect(
    const TimelineItem &item,
    const MessagePaintContext &context);

/// Bubble shape path in local message coordinates (includes tail where applicable).
QPainterPath bubbleShapePath(
    const TimelineItem &item,
    const MessagePaintContext &context);

/// Hit-test: is pos inside the bubble? Works for all content types
/// including UTD/deleted (which lack full geometry).
bool isInsideBubble(
    const TimelineItem &item,
    const MessagePaintContext &context,
    QPoint pos);

bool isInsideUtdVerifyLink(
    const TimelineItem &item,
    const MessagePaintContext &context,
    QPoint pos);

/// Returns true if this message should render as an audio bubble.
bool isAudioBubble(const TimelineItem &item);

/// Play/pause button rect in local message coordinates.
QRect audioPlayButtonRect(
    const TimelineItem &item,
    const MessagePaintContext &context);

/// Returns the avatar rect in local message coordinates, or an invalid rect
/// if the avatar is not visible for this message.
QRect senderAvatarRect(
    const TimelineItem &item,
    const MessagePaintContext &context);

/// Returns true if `pos` (local message coords) hits the sender avatar.
bool senderAvatarAt(
    const TimelineItem &item,
    const MessagePaintContext &context,
    QPoint pos);

/// Reaction trigger icon rect (grey smile) for a message.
/// Returns a 28x28 rect positioned to the right of the bubble,
/// vertically centered against the bubble body.
QRect reactionTriggerRect(
    const TimelineItem &item,
    const MessagePaintContext &context);

/// Returns the upload cancel button rect (centered on the upload overlay).
/// Empty rect if the item is not currently uploading.
QRect uploadCancelRect(
    const TimelineItem &item,
    const MessagePaintContext &context);

/// Media (poster/frame) rect of a video bubble in local message coordinates.
/// Single source of truth for both paint and hit-test. Empty for non-video.
QRect videoMediaRect(
    const TimelineItem &item,
    const MessagePaintContext &context);

/// Clickable seek-bar strip along the bottom of an inline-video media rect.
QRect videoSeekBarRect(const QRect &mediaRect);

/// Fullscreen-expand button rect at the top-right of an inline-video media rect.
QRect videoFullscreenButtonRect(const QRect &mediaRect);
QRect videoMuteButtonRect(const QRect &mediaRect, qint64 durationMs);

/// Returns the download cancel button rect for in-flight file/audio media.
/// Empty rect if the item is not currently downloading.
QRect downloadCancelRect(
    const TimelineItem &item,
    const MessagePaintContext &context);

/// Quick reaction bar anchor point (above bubble, right-aligned).
QPoint reactionBarAnchor(
    const TimelineItem &item,
    const MessagePaintContext &context);

/// Clear cached text layouts and formatted text.
/// Call when the message set changes (room switch, timeline reload).
void clearPaintCache();

/// Clear the emoji sprite cache. Call on logout only (emoji recur across rooms).
void clearEmojiImageCache();

/// Set whether the OS reports a usable network. When offline, an in-flight
/// upload's status shows "Waiting for network..." instead of stalled progress.
void setUploadsNetworkOnline(bool online);

/// Paint the sender avatar circle at the given position.
/// Used by the list paint loop for sliding-avatar behavior.
void paintSenderAvatar(
    QPainter &p,
    const TimelineItem &item,
    int avatarLeft,
    int avatarTop,
    int avatarSize,
    qreal dpr,
    QWidget *repaintTarget = nullptr,
    const QRect &repaintRect = QRect());

} // namespace HistoryMessage

} // namespace TeleMatrix

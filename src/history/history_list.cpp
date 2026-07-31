// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_list.h"

#include "ui/text/emoji_text.h"
#include "history_message.h"
#include "media/history_view_audio.h"
#include "media/history_view_poll.h"
#include "history_video_thumbnail_probe_state.h"
#include "history_popup_menu_style.h"
#include "unread_bar_placement.h"
#include "unread_read_progress.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QImageReader>
#include <QFileInfo>
#include <QStandardPaths>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QLocale>
#include "history/history_popup_menu_style.h"
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QRegion>
#include <QScreen>
#include <QSet>
#include <QHash>
#include <QTimer>
#include <QElapsedTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVariantAnimation>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <algorithm>
#include <functional>
#include "protocol/media_cache.h"
#include <utility>

using namespace Qt::Literals::StringLiterals;

#include "ui/painter.h"
#include "ui/platform/reveal_in_folder.h"
#include "ui/safe_url.h"
#include "ui/style/icon_provider.h"
#include "styles/style_chat.h"
#include "styles/style_font_metrics.h"

namespace TeleMatrix {

namespace {

constexpr int kBottomSnapTolerance = 8;

// ─── TM_PAINT_STATS: timeline paint instrumentation ──────────────────────────
//
// Zero cost when TM_PAINT_STATS is unset (one bool test per paint; the phase
// timers take a null sink and never start a clock).
//
// Everything is reported PER BLOCK — v1 reported a cumulative average, which
// reads as "improving" while per-block cost is flat or worse, and cost us a
// wrong diagnosis once already.
//
// The clip classes are the point of the probe. An aggregate rows/paint cannot
// distinguish a full-viewport invalidation (a landed pagination page, a resize)
// from an honest scroll strip or a single-row animation repaint, yet those have
// completely different fixes. Splitting by damage geometry separates them, and
// the `scrolling` counter separates active scroll from the post-settle glow.
[[nodiscard]] bool paintStatsEnabled() {
    static const bool on = qEnvironmentVariableIsSet("TM_PAINT_STATS");
    return on;
}
// Note (2026-07-21): a TM_FLAT_SCROLL_BG experiment flat-filled this background
// while scrolling, on the theory that a translation-invariant fill would let
// Qt's scroll-blit repaint only the exposed strip. Measured on macOS/retina it
// did nothing: the paint clip stayed full-viewport every scroll tick (the blit
// is disabled by macOS layer-backing, independent of the background), and a
// full-viewport fillRect costs the same ~2.3ms as the pixmap. Reverted. See
// docs/perf-backscroll-2026-07-20.md.

// What invalidated the widget since the last dump. Counted at the call sites
// rather than inferred, so the clip classes can be attributed to a cause.
// Deliberately process-wide, like the block accumulators: the only second
// HistoryList is the pinned section, which neither scrolls nor paginates.
struct PaintCauses {
    int prepend = 0;
    int sliceReplace = 0;
    int append = 0;
    int mediaInvalidate = 0;
    int resize = 0;
};
PaintCauses g_paintCauses;

void notePaintCause(int PaintCauses::*field) {
    if (paintStatsEnabled()) {
        ++(g_paintCauses.*field);
    }
}

// Accumulates one phase of a paint into `sink`. A null sink means disabled.
struct PaintPhase {
    qint64 *sink;
    QElapsedTimer timer;
    explicit PaintPhase(qint64 *target) : sink(target) {
        if (sink) {
            timer.start();
        }
    }
    ~PaintPhase() {
        if (sink) {
            *sink += timer.nsecsElapsed() / 1000;
        }
    }
};

struct PaintStatsScope {
    enum Class { Full = 0, Strip, Row, ClassCount };
    static constexpr const char *kClassName[ClassCount] = { "full", "strip", "row" };

    bool enabled;
    const int &rows;
    QElapsedTimer timer;

    // Filled in by paintEvent while it runs.
    qint64 bgUs = 0;
    qint64 rowsUs = 0;
    qint64 overlayUs = 0;
    int clipHeight = 0;
    int viewportW = 0;
    int viewportH = 0;
    int typicalRow = 0;
    Class cls = Strip;
    bool scrolling = false;

    PaintStatsScope(bool on, const int &rowCounter) : enabled(on), rows(rowCounter) {
        if (enabled) {
            timer.start();
        }
    }

    // Null when disabled, so PaintPhase compiles out to nothing measurable.
    [[nodiscard]] qint64 *bg() { return enabled ? &bgUs : nullptr; }
    [[nodiscard]] qint64 *rowsPhase() { return enabled ? &rowsUs : nullptr; }
    [[nodiscard]] qint64 *overlay() { return enabled ? &overlayUs : nullptr; }

    void classify(const QRect &clip, int vpW, int vpH, int typical, bool isScrolling) {
        if (!enabled) {
            return;
        }
        viewportW = vpW;
        viewportH = vpH;
        typicalRow = typical;
        clipHeight = clip.height();
        scrolling = isScrolling;
        const auto vpArea = qint64(vpW) * qint64(vpH);
        const auto clipArea = qint64(clip.width()) * qint64(clip.height());
        if (vpArea > 0 && clipArea * 10 >= vpArea * 9) {
            cls = Full;
        } else if (typical > 0 && clip.height() * 2 <= typical * 3) {
            cls = Row;
        } else {
            cls = Strip;
        }
    }

    ~PaintStatsScope() {
        if (!enabled) {
            return;
        }
        struct Block {
            qint64 paints = 0;
            qint64 rowsPainted = 0;
            qint64 scrolls = 0;
            qint64 bg = 0, rows = 0, overlay = 0;
            qint64 n[ClassCount] = {};
            qint64 us[ClassCount] = {};
            qint64 maxUs[ClassCount] = {};
            qint64 clipH[ClassCount] = {};
        };
        static Block b;
        static qint64 total = 0;
        static QElapsedTimer since;      // wall clock of the current block
        static QElapsedTimer sinceStart; // wall clock since the first paint
        if (!sinceStart.isValid()) {
            sinceStart.start();
            since.start();
        }

        const auto us = timer.nsecsElapsed() / 1000;
        ++b.paints;
        ++total;
        b.rowsPainted += rows;
        b.scrolls += scrolling ? 1 : 0;
        b.bg += bgUs;
        b.rows += rowsUs;
        b.overlay += overlayUs;
        ++b.n[cls];
        b.us[cls] += us;
        b.maxUs[cls] = qMax(b.maxUs[cls], us);
        b.clipH[cls] += clipHeight;

        if (b.paints < 120) {
            return;
        }

        const auto blockMs = since.restart();
        const auto sumUs = b.us[Full] + b.us[Strip] + b.us[Row];
        const auto maxUs = qMax(b.maxUs[Full], qMax(b.maxUs[Strip], b.maxUs[Row]));
        qInfo("paint[n=%lld] %lld paints in %lldms (t=+%.1fs) vp=%dx%d typRow=%d",
              total, b.paints, blockMs, sinceStart.elapsed() / 1000.0,
              viewportW, viewportH, typicalRow);
        qInfo("  total  avg=%.2fms max=%.2fms rows/paint=%.1f scrolling=%lld/%lld",
              double(sumUs) / b.paints / 1000.0, maxUs / 1000.0,
              double(b.rowsPainted) / b.paints, b.scrolls, b.paints);
        for (int c = 0; c < ClassCount; ++c) {
            if (!b.n[c]) {
                continue;
            }
            qInfo("  %-6s n=%-4lld avg=%.2fms max=%.2fms clipH=%lld",
                  kClassName[c], b.n[c], double(b.us[c]) / b.n[c] / 1000.0,
                  b.maxUs[c] / 1000.0, b.clipH[c] / b.n[c]);
        }
        qInfo("  phase  bg=%.2fms rows=%.2fms overlay=%.2fms (avg/paint)",
              double(b.bg) / b.paints / 1000.0,
              double(b.rows) / b.paints / 1000.0,
              double(b.overlay) / b.paints / 1000.0);
        qInfo("  cause  prepend=%d slice=%d append=%d media=%d resize=%d",
              g_paintCauses.prepend, g_paintCauses.sliceReplace,
              g_paintCauses.append, g_paintCauses.mediaInvalidate,
              g_paintCauses.resize);

        b = Block{};
        g_paintCauses = PaintCauses{};
    }
};

// Quick (default) reactions for the hover reaction column, in canonical order.
// fromUtf8 is required — raw bytes would be mis-decoded as Latin-1.
const QVector<QString> &quickReactionEmojis() {
    static const QVector<QString> value = {
        QString::fromUtf8("\xF0\x9F\x91\x8D"),           // 👍 thumbs up
        QString::fromUtf8("\xF0\x9F\x91\x8E"),           // 👎 thumbs down
        QString::fromUtf8("\xE2\x9D\xA4\xEF\xB8\x8F"),   // ❤️ red heart
        QString::fromUtf8("\xF0\x9F\x94\xA5"),           // 🔥 fire
        QString::fromUtf8("\xF0\x9F\xA5\xB0"),           // 🥰 smiling face with hearts
        QString::fromUtf8("\xF0\x9F\x91\x8F"),           // 👏 clapping hands
        QString::fromUtf8("\xF0\x9F\x98\x81"),           // 😁 beaming face
    };
    return value;
}

// Max emoji shown in the column (quick + recents), keeps the list bounded.
constexpr int kReactionColumnMax = 24;

// Hover timing for the reaction affordance (button show delay / expand delay).
// The resting pill appears a little after the reply pill (show delay); resting on
// it briefly expands the column. A 25ms expand delay would normally be paired with
// a 300ms unfold animation we don't have, so we use a longer, perceptible delay
// instead of instant expansion.
constexpr int kReactionShowDelayMs = 300;
constexpr int kReactionExpandDelayMs = 200;

// Painter opacity for a message whose redaction is in flight. Applied once,
// around every paint path (bubble, media, avatar), so the whole row fades.
constexpr qreal kDeletingOpacity = 0.4;

// Pinned-mode "go to message" jump button: a msgServiceBg circle holding the
// filled_go_to_message icon (msgServiceFg), anchored to the bubble's
// bottom-right corner.
constexpr int kJumpButtonDiameter = 31; // px, fast-action button diameter
constexpr int kJumpButtonLeft = 13;     // px, gap from bubble to button
constexpr int kJumpButtonBottom = 5;    // px, button offset above bubble bottom
// Right gutter reserved so a max-width bubble still leaves room for the button.
constexpr int kJumpButtonReserve =
    kJumpButtonDiameter + 2 * kJumpButtonLeft;

[[nodiscard]] bool isContextMenuMouseGesture(const QMouseEvent *e) {
#ifdef Q_OS_MAC
    return e->button() == Qt::LeftButton
        && e->modifiers().testFlag(Qt::ControlModifier);
#else
    return false;
#endif
}

[[nodiscard]] bool timelineItemVisualChanged(
    const TimelineItem &a,
    const TimelineItem &b) {
	return a.transactionId != b.transactionId
		|| a.sender != b.sender
        || a.content != b.content
		|| a.reply != b.reply
        || a.forwardedFrom != b.forwardedFrom
        || a.isEdited != b.isEdited
        || a.isPinned != b.isPinned
        || a.reactions != b.reactions
        || a.delivery != b.delivery
        || a.timestamp != b.timestamp
        || a.urlPreview != b.urlPreview
        || a.encryption != b.encryption;
}

[[nodiscard]] bool timelineItemHeightAffectingChanged(
    const TimelineItem &a,
    const TimelineItem &b) {
    return contentType(a) != contentType(b)
        || a.sender.id != b.sender.id
        || a.timestamp != b.timestamp
        || a.content != b.content
        || a.reply != b.reply
        || a.forwardedFrom != b.forwardedFrom
        || a.reactions != b.reactions
        || a.delivery.deleted != b.delivery.deleted
        || a.urlPreview != b.urlPreview;
}

// ---------- matrix.to link parsing ----------

struct MatrixToLink {
    enum Type { None, Room, RoomEvent, User };
    Type type = None;
    QString id;        // room ID / user ID
    QString eventId;   // event ID (only for RoomEvent)
    QString server;    // server part of the room/user ID
    QStringList via;   // ?via= routing hints (federated peek/join)
};

MatrixToLink parseMatrixToUrl(const QString &url) {
    MatrixToLink result;
    const QUrl parsed(url);
    if (parsed.host().compare(u"matrix.to"_s, Qt::CaseInsensitive) != 0) {
        return result;
    }
    auto fragment = parsed.fragment(QUrl::FullyDecoded);
    if (fragment.startsWith(QLatin1Char('/'))) {
        fragment = fragment.mid(1);
    }
    // The ?via= hints live in the fragment's own query part — without them a
    // bare !room-id from another server is unroutable for the homeserver.
    const auto queryIdx = fragment.indexOf(QLatin1Char('?'));
    if (queryIdx >= 0) {
        const QUrlQuery query(fragment.mid(queryIdx + 1));
        for (const auto &item : query.queryItems(QUrl::FullyDecoded)) {
            if (item.first == QLatin1String("via") && !item.second.isEmpty()) {
                result.via.push_back(item.second);
            }
        }
        fragment = fragment.left(queryIdx);
    }
    if (fragment.isEmpty()) {
        return result;
    }
    const auto parts = fragment.split(QLatin1Char('/'));
    const auto first = QUrl::fromPercentEncoding(parts[0].toUtf8());

    if (first.startsWith(QLatin1Char('@'))) {
        result.type = MatrixToLink::User;
        result.id = first;
        const auto colonIdx = first.indexOf(QLatin1Char(':'));
        if (colonIdx > 0) result.server = first.mid(colonIdx + 1);
    } else if (first.startsWith(QLatin1Char('!')) || first.startsWith(QLatin1Char('#'))) {
        result.id = first;
        const auto colonIdx = first.indexOf(QLatin1Char(':'));
        if (colonIdx > 0) result.server = first.mid(colonIdx + 1);
        if (parts.size() >= 2 && !parts[1].isEmpty()) {
            result.type = MatrixToLink::RoomEvent;
            result.eventId = QUrl::fromPercentEncoding(parts[1].toUtf8());
        } else {
            result.type = MatrixToLink::Room;
        }
    }
    return result;
}

// ---------- end matrix.to link parsing ----------

[[nodiscard]] bool applyReactionChange(
    QVector<ReactionInfo> &reactions,
    const QString &key,
    bool active) {
    if (key.isEmpty()) {
        return false;
    }

    for (auto i = 0; i < reactions.size(); ++i) {
        auto &reaction = reactions[i];
        if (reaction.key != key) {
            continue;
        }

        if (active) {
            if (reaction.isSelf) {
                return false;
            }
            reaction.isSelf = true;
            reaction.count = qMax(1, reaction.count + 1);
            return true;
        }

        if (!reaction.isSelf) {
            return false;
        }
        reaction.isSelf = false;
        if (reaction.count > 1) {
            --reaction.count;
        } else {
            reactions.removeAt(i);
        }
        return true;
    }

    if (!active) {
        return false;
    }

    reactions.push_back(ReactionInfo{
        .key = key,
        .count = 1,
        .isSelf = true,
    });
    return true;
}

[[nodiscard]] QStringList selectedPollOptionIds(const TimelineItem &item) {
    QStringList result;
    const auto poll = pollContent(item);
    if (!poll) {
        return result;
    }
    result.reserve(poll->options.size());
    for (const auto &option : poll->options) {
        if (option.isChosen) {
            result.push_back(option.id);
        }
    }
    return result;
}

[[nodiscard]] bool isMediaBubbleHit(
    const TimelineItem &item,
    const MessagePaintContext &context,
    QPoint pos) {
    const auto isMedia = (isImageMessage(item) || isVideoMessage(item))
        && !mediaUrl(item).isEmpty();
    if (!isMedia) {
        return false;
    }
    return HistoryMessage::bubbleShapePath(item, context).contains(QPointF(pos));
}

struct PollContentGeometry {
    int left = 0;
    int top = 0;
    int width = 0;
    bool valid = false;
};

[[nodiscard]] QString forwardedText(const TimelineItem &item) {
    return QCoreApplication::translate("HistoryList", "Forwarded from %1")
        .arg(forwardedSenderName(item));
}

[[nodiscard]] int forwardedHeaderHeight(const TimelineItem &item, int innerWidth) {
    if (forwardedSenderName(item).isEmpty()) {
        return 0;
    }
    const auto &fm = st::fontMetrics(st::msgServiceFont);
    const auto lines = (fm.horizontalAdvance(forwardedText(item)) > innerWidth) ? 2 : 1;
    return lines * st::msgServiceFont->height;
}

[[nodiscard]] PollContentGeometry pollContentGeometry(
    const TimelineItem &item,
    const MessagePaintContext &context) {
    PollContentGeometry result;
    const auto bubble = HistoryMessage::bubbleRect(item, context);
    if (!bubble.isValid()) {
        return result;
    }

    const auto showSender = context.isGroup
        && !context.sameSenderAbove
        && !item.delivery.outgoing;
    result.left = bubble.left() + HistoryMessage::kBubblePaddingH;
    result.top = HistoryMessage::kBubblePaddingV;
    result.width = bubble.width() - 2 * HistoryMessage::kBubblePaddingH;
    if (showSender) {
        result.top += HistoryMessage::kSenderNameHeight;
    }
    result.top += forwardedHeaderHeight(item, result.width);
    if (hasReply(item)) {
        result.top += HistoryMessage::kReplyPreviewHeight
            + HistoryMessage::kReplyPreviewBottomSkip;
    }
    result.valid = result.width > 0;
    return result;
}


} // namespace

class TimestampTooltip final : public QWidget {
public:
    TimestampTooltip()
        : QWidget(nullptr) {
        setWindowFlags(
            Qt::WindowFlags(Qt::FramelessWindowHint)
            | Qt::BypassWindowManagerHint
            | Qt::NoDropShadowWindowHint
            | Qt::ToolTip);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        setAttribute(Qt::WA_TransparentForMouseEvents, true);

        _fade.setDuration(kFadeDuration);
        _fade.setEasingCurve(QEasingCurve::OutCubic);
        QObject::connect(
            &_fade,
            &QVariantAnimation::valueChanged,
            this,
            [this](const QVariant &value) {
                _opacity = value.toReal();
                update();
            });
        QObject::connect(
            &_fade,
            &QVariantAnimation::finished,
            this,
            [this] {
                if (_hiding) {
                    QWidget::hide();
                    _hiding = false;
                }
            });

        // A Qt::ToolTip is a top-level window, so it floats above the app's
        // child-widget popups and right-click menus. Real tooltips vanish on any
        // input; this one must too — dismiss it on the first press/wheel/key so it
        // never lingers over a popup the user just opened.
        if (qApp) {
            qApp->installEventFilter(this);
        }
    }

    void popup(const QPoint &globalPos, const QString &text) {
        if (text.isEmpty()) {
            hideAnimated();
            return;
        }

        _text = text;
        const auto font = static_cast<const QFont &>(st::normalFont);
        const auto &metrics = st::fontMetrics(font);

        const auto addWidth = 2 * kLineWidth + kTextPadding.left() + kTextPadding.right();
        const auto addHeight = 2 * kLineWidth + kTextPadding.top() + kTextPadding.bottom();
        const auto maxTextWidth = qMax(1, kWidthMax - addWidth);
        const auto measured = metrics.boundingRect(
            QRect(0, 0, maxTextWidth, 20000),
            Qt::TextWordWrap | Qt::TextExpandTabs,
            _text);
        const auto textHeight = qMin(measured.height(), kLinesMax * metrics.height());
        const QSize size(
            addWidth + qMax(1, measured.width()),
            addHeight + qMax(metrics.height(), textHeight));

        setFixedSize(size);
        move(countPosition(globalPos, size));

        if (isHidden()) {
            _fade.stop();
            _hiding = false;
            _opacity = 0.0;
            QWidget::show();
            _fade.setStartValue(_opacity);
            _fade.setEndValue(1.0);
            _fade.start();
        } else if (_hiding) {
            _fade.stop();
            _hiding = false;
            _fade.setStartValue(_opacity);
            _fade.setEndValue(1.0);
            _fade.start();
        } else {
            _opacity = 1.0;
            update();
        }
    }

    void hideAnimated() {
        if (isHidden() && !_hiding) {
            return;
        }
        _fade.stop();
        _hiding = true;
        _fade.setStartValue(_opacity);
        _fade.setEndValue(0.0);
        _fade.start();
    }

private:
    bool eventFilter(QObject *o, QEvent *e) override {
        switch (e->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::NonClientAreaMouseButtonPress:
        case QEvent::Wheel:
        case QEvent::KeyPress:
            if (!isHidden() && !_hiding) {
                hideAnimated();
            }
            break;
        default:
            break;
        }
        return QWidget::eventFilter(o, e);
    }

    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        p.setOpacity(_opacity);
        p.setPen(st::tooltipBorderFg);
        p.setBrush(st::tooltipBg);
        p.drawRoundedRect(
            QRectF(0.5, 0.5, width() - 1.0, height() - 1.0),
            kRadius,
            kRadius);

        p.setFont(st::normalFont);
        p.setPen(st::tooltipFg);
        const QRect textRect(
            kLineWidth + kTextPadding.left(),
            kLineWidth + kTextPadding.top(),
            width() - 2 * kLineWidth - kTextPadding.left() - kTextPadding.right(),
            height() - 2 * kLineWidth - kTextPadding.top() - kTextPadding.bottom());
        p.drawText(
            textRect,
            Qt::TextWordWrap | Qt::TextExpandTabs,
            _text);
    }

    [[nodiscard]] QPoint countPosition(const QPoint &globalPos, const QSize &size) const {
        auto p = globalPos + kShift;
        auto *screen = QGuiApplication::screenAt(globalPos);
        if (!screen) {
            screen = QGuiApplication::primaryScreen();
        }
        if (!screen) {
            return p;
        }
        const auto area = screen->availableGeometry();

        if (area.x() + area.width() - kSkip < p.x() + size.width()
            && p.x() + size.width() > globalPos.x()) {
            p.setX(qMax(
                area.x() + area.width() - kSkip - size.width(),
                globalPos.x() - size.width()));
        }
        if (area.x() + kSkip > p.x() && p.x() < globalPos.x()) {
            p.setX(qMin(globalPos.x(), area.x() + kSkip));
        }
        if (area.y() + area.height() - kSkip < p.y() + size.height()) {
            p.setY(globalPos.y() - size.height() - kSkip);
        }
        if (area.y() > p.y()) {
            p.setY(qMin(
                globalPos.y() + kShift.y(),
                area.y() + area.height() - size.height()));
        }
        return p;
    }

    static constexpr int kLineWidth = 1;
    static constexpr int kRadius = 3;
    static constexpr int kWidthMax = 800;
    static constexpr int kLinesMax = 12;
    static constexpr int kSkip = 10;
    static constexpr int kFadeDuration = 120;
    static constexpr QPoint kShift = QPoint(-20, 20);
    inline static const QMargins kTextPadding = QMargins(5, 2, 5, 2);

    QString _text;
    qreal _opacity = 1.0;
    bool _hiding = false;
    QVariantAnimation _fade;
};

namespace {
// Space below the last message.
constexpr int kBottomPadding = 8;
constexpr qint64 kHighlightFadeInMs = 120;
constexpr qint64 kHighlightFadeOutMs = 1380;
// Selection checkbox constants (chat style).
constexpr int kSelectionDiameter = 20;        // msgSelectionCheck.size
constexpr int kSelectionOffset = 30;           // msgSelectionOffset
constexpr int kSelectionBottomSkip = 5;        // msgSelectionBottomSkip
constexpr int kSelectionCheckIconLeft = 3;
constexpr int kSelectionCheckIconTop = 6;
constexpr int kScrollBarDelta = 3;             // historyScroll.deltax
inline const QColor kSelectionUncheckedBg(0x00, 0x00, 0x00, 0x40);

[[nodiscard]] QString selectionCheckIconResourcePath(qreal dpr) {
    const auto base = QStringLiteral(":/telematrix/icons/chat/default_checkbox_check");
    if (dpr >= 2.5) {
        return base + QStringLiteral("@3x.png");
    }
    if (dpr >= 1.5) {
        return base + QStringLiteral("@2x.png");
    }
    return base + QStringLiteral(".png");
}

[[nodiscard]] QImage selectionCheckIcon(qreal dpr) {
    static QHash<int, QImage> cache;
    const auto bucket = (dpr >= 2.5) ? 3 : (dpr >= 1.5) ? 2 : 1;
    if (const auto it = cache.constFind(bucket); it != cache.cend()) {
        return it.value();
    }

    auto icon = QImage(selectionCheckIconResourcePath(dpr));
    if (icon.isNull()) {
        icon = QImage(QStringLiteral(":/telematrix/icons/chat/default_checkbox_check.png"));
    }
    if (!icon.isNull()) {
        icon.setDevicePixelRatio(bucket);
        const auto tinted = TeleMatrix::Style::IconProvider::colorizeMask(icon, st::windowBg);
        if (!tinted.isNull()) {
            cache.insert(bucket, tinted);
            return tinted;
        }
        cache.insert(bucket, icon);
    }
    return icon;
}

[[nodiscard]] QString extensionForPlaybackHint(
        const QString &filename,
        const QString &mime) {
    auto extension = QFileInfo(filename).suffix().toLower();
    if (!extension.isEmpty()) {
        return extension;
    }
    const auto normalized = mime.toLower();
    if (normalized.contains(QStringLiteral("ogg"))
        || normalized.contains(QStringLiteral("opus"))) {
        return QStringLiteral("ogg");
    } else if (normalized.contains(QStringLiteral("mpeg"))
        || normalized.contains(QStringLiteral("mp3"))) {
        return QStringLiteral("mp3");
    } else if (normalized.contains(QStringLiteral("mp4"))
        || normalized.contains(QStringLiteral("aac"))) {
        return QStringLiteral("m4a");
    } else if (normalized.contains(QStringLiteral("wav"))) {
        return QStringLiteral("wav");
    } else if (normalized.contains(QStringLiteral("flac"))) {
        return QStringLiteral("flac");
    }
    return QStringLiteral("ogg");
}

[[nodiscard]] int dateBadgeInnerHeight() {
    return st::msgServicePadding.top()
        + st::msgServiceFont->height
        + st::msgServicePadding.bottom();
}

[[nodiscard]] int dateBadgeTotalHeight() {
    return st::msgServiceMargin.top()
        + dateBadgeInnerHeight()
        + st::msgServiceMargin.bottom();
}

[[nodiscard]] int unreadBarTotalHeight() {
    // Unread-bar total height = bar height + margin.
    return st::historyUnreadBarHeight + st::historyUnreadBarMargin;
}

[[nodiscard]] int topTimelineIndicatorHeight(
        bool hasMessages,
        bool canPaginateBack,
        bool hitTimelineStart) {
    return (hasMessages && (canPaginateBack || hitTimelineStart)) ? 40 : 0;
}

[[nodiscard]] int preMessageHeight(const HistoryList::LayoutItem &item) {
    auto result = 0;
    if (item.showDate) {
        result += dateBadgeTotalHeight();
    }
    if (item.showUnreadBar) {
        result += unreadBarTotalHeight();
    }
    return result;
}
} // namespace

HistoryList::HistoryList(QWidget *parent)
    : Ui::RpWidget(parent)
{
    // Reply-parent resolver: points at this list's own index + item vector (both
    // stable members), so the paint context resolves parents without a by-id copy.
    _timelineLookup.index = &_messageIndex;
    _timelineLookup.items = &_messages;
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setFocusPolicy(Qt::ClickFocus);
    _timestampTooltip = new TimestampTooltip();

    // Delayed tooltip (~1s before showing).
    _tooltipTimer.setSingleShot(true);
    _tooltipTimer.setInterval(1000);
    connect(&_tooltipTimer, &QTimer::timeout, this, [this] {
        if (!_hoveredTimestamp || _tooltipText.isEmpty() || !_timestampTooltip) {
            return;
        }
        _timestampTooltip->popup(_tooltipGlobalPos, _tooltipText);
        _tooltipShownAt = _tooltipGlobalPos;
    });

    // Single shared inline video player; repaint the timeline as frames arrive
    // and on play/pause/seek so the active bubble updates.
    _inlineVideo = new HistoryInlineVideoPlayer(this);
    connect(_inlineVideo, &HistoryInlineVideoPlayer::frameChanged, this,
        [this](const QString &eventId) { updateRowByEventId(eventId); });
    connect(_inlineVideo, &HistoryInlineVideoPlayer::stateChanged, this,
        [this](const QString &eventId) { updateRowByEventId(eventId); });

    // A short moment after scrolling stops, clear the scrolling flag and repaint
    // once so the (time-based) glow / animation repaints resume — during scroll they
    // are suppressed so they don't defeat the scroll-blit optimization.
    _scrollSettleTimer.setSingleShot(true);
    _scrollSettleTimer.setInterval(120);
    connect(&_scrollSettleTimer, &QTimer::timeout, this, [this] {
        _scrolling = false;
        // Apply any full-window replace that landed mid-scroll. setSlice
        // re-diffs against the current _messages and repaints, so no separate
        // update() is needed on this branch.
        if (_deferredReplaceSlice) {
            const auto slice = *_deferredReplaceSlice;
            _deferredReplaceSlice.reset();
            setSlice(slice);
        } else {
            update();
        }
    });

    _sendStateTimer.setInterval(100);
    connect(&_sendStateTimer, &QTimer::timeout, this, [this] {
        auto hasSending = false;
        QRegion repaintRegion;
        for (int i = 0; i < _messages.size(); ++i) {
            if (_messages[i].delivery.sendState == SendState::Sending) {
                hasSending = true;
                if (i < _layout.size()) {
                    repaintRegion += QRect(
                        0,
                        _layout[i].y + _contentOffset,
                        width(),
                        _layout[i].height);
                }
            }
        }
        if (!hasSending) {
            _sendStateTimer.stop();
            return;
        }
        _sendStateTick = (_sendStateTick + 1) % 20;
        if (repaintRegion.isEmpty()) {
            update();
        } else {
            update(repaintRegion);
        }
    });

    _decryptingAnimTimer.setInterval(33); // ~30fps glow sweep
    connect(&_decryptingAnimTimer, &QTimer::timeout, this, [this] {
        constexpr qint64 kGlowCycleMs = 2000; // 1s slide + 1s wait (PathShiftGradient)
        const auto elapsed = QDateTime::currentMSecsSinceEpoch()
            - _decryptingGlowStartMs;
        _decryptingGlowProgress =
            qreal(elapsed % kGlowCycleMs) / qreal(kGlowCycleMs);
        // Iterate only the maintained glowing set (not every loaded message) so
        // the per-frame cost stays O(glowing) during long E2EE back-scroll. Drop
        // ids whose row decrypted or safety-expired.
        QRegion repaintRegion;
        QStringList expired;
        for (const auto &eventId : _glowingEventIds) {
            const auto i = _messageIndex.physicalIndexOf(eventId);
            if (i >= 0 && i < _layout.size() && i < _messages.size()
                && itemGlowActive(_messages[i])) {
                repaintRegion += QRect(
                    0,
                    _layout[i].y + _contentOffset,
                    width(),
                    _layout[i].height);
            } else {
                expired.append(eventId);
            }
        }
        for (const auto &eventId : expired) {
            _glowingEventIds.remove(eventId);
        }
        const auto glowingCount = _glowingEventIds.size();
        if (glowingCount == 0) {
            // All glowing UTDs replaced by plaintext or safety-expired: stop the
            // timer and reflow any skeleton rows to their terminal card height.
            _decryptingAnimTimer.stop();
            _lastGlowingCount = 0;
            _utdGlowFirstSeenMs.clear();
            relayoutUtdRows();
            return;
        }
        if (glowingCount < _lastGlowingCount) {
            // A SUBSET of glowing UTDs safety-expired while others still glow:
            // reflow so the now-terminal rows get their card height instead of
            // painting the card into a skeleton-sized row (clipping/overlap).
            // recalculateLayout re-reads per-item glow state.
            _lastGlowingCount = glowingCount;
            relayoutUtdRows();
            return;
        }
        _lastGlowingCount = glowingCount;
        if (repaintRegion.isEmpty()) {
            update();
        } else {
            update(repaintRegion);
        }
    });

    _highlightTimer.setInterval(16);
    connect(&_highlightTimer, &QTimer::timeout, this, [this] {
        if (_highlightedEventId.isEmpty()) {
            _highlightTimer.stop();
            _highlightOpacity = 0.0f;
            return;
        }
        const auto repaintEventId = _highlightedEventId;
        const auto elapsed = QDateTime::currentMSecsSinceEpoch() - _highlightStartTime;
        const auto duration = kHighlightFadeInMs + kHighlightFadeOutMs;
        if (elapsed >= duration) {
            _highlightOpacity = 0.0f;
            _highlightedEventId.clear();
            _highlightTimer.stop();
        } else if (elapsed < kHighlightFadeInMs) {
            _highlightOpacity = float(elapsed) / float(kHighlightFadeInMs);
        } else {
            const auto fadeOutElapsed = elapsed - kHighlightFadeInMs;
            const auto t = qBound(
                0.0f,
                float(fadeOutElapsed) / float(kHighlightFadeOutMs),
                1.0f);
            _highlightOpacity = 1.0f - (t * t * t);
        }
        const auto i = _messageIndex.physicalIndexOf(repaintEventId);
        if (i >= 0 && i < _layout.size()) {
            update(QRect(0, _layout[i].y + _contentOffset, width(), _layout[i].height));
        } else {
            update();
        }
    });

    // Reply pill animation (120ms sineInOut scale+opacity).
    _replyPillAnim = new QVariantAnimation(this);
    _replyPillAnim->setDuration(120);
    _replyPillAnim->setEasingCurve(QEasingCurve::InOutSine);
    connect(_replyPillAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        _replyPillOpacity = v.toReal();
        // Capture the pill's row + painted extent before the fully-hidden reset.
        // The pill hangs ABOVE the bubble top (outside the row rect), so repaint
        // its overhang too — a bare row rect leaves a ghost under region culling.
        const auto pillRow = _replyPillIndex;
        const auto pillRect = _replyPillWidgetRect;
        // Reset pill index when fully hidden so it can re-appear on re-enter.
        if (_replyPillOpacity < 0.01) {
            _replyPillIndex = -1;
            _replyPillWidgetRect = QRect();
        }
        QRegion dirty;
        if (pillRow >= 0 && pillRow < _layout.size()) {
            dirty += affordanceRowRect(pillRow);
        }
        if (!pillRect.isEmpty()) {
            dirty += pillRect;
        }
        if (dirty.isEmpty()) {
            update();
        } else {
            update(dirty);
        }
    });
    _replyPillHideTimer.setSingleShot(true);
    _replyPillHideTimer.setInterval(300);
    connect(&_replyPillHideTimer, &QTimer::timeout, this, [this] {
        // Animate out.
        _replyPillAnim->setStartValue(_replyPillOpacity);
        _replyPillAnim->setEndValue(0.0);
        _replyPillAnim->start();
    });

    _scrollDateHideTimer.setSingleShot(true);
    _scrollDateHideTimer.setInterval(st::historyScrollDateHideTimeout);
    connect(&_scrollDateHideTimer, &QTimer::timeout, this, [this] {
        toggleScrollDateShown(false);
    });

    // Re-tile the wallpaper once a resize settles; paintEvent stretches the
    // stale composite in the meantime.
    _bgRebuildTimer.setSingleShot(true);
    _bgRebuildTimer.setInterval(150);
    connect(&_bgRebuildTimer, &QTimer::timeout, this, [this] {
        _bgCache.rebuild(backgroundArea(), devicePixelRatioF());
        update();
    });

    _scrollDateOpacityAnimation.setDuration(st::historyDateFadeDuration);
    _scrollDateOpacityAnimation.setEasingCurve(QEasingCurve::OutCubic);
    connect(&_scrollDateOpacityAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        _scrollDateOpacity = value.toReal();
        update(QRect(0, _visibleTop, width(), dateBadgeTotalHeight() + 4));
    });

    // Audio playback engine.
    _mediaPlayer = new QMediaPlayer(this);
    _audioOutput = new QAudioOutput(this);
    _mediaPlayer->setAudioOutput(_audioOutput);

    connect(_mediaPlayer, &QMediaPlayer::positionChanged, this, [this](qint64 pos) {
        _audioPlayback.setPosition(pos);
    });
    connect(_mediaPlayer, &QMediaPlayer::durationChanged, this, [this](qint64 dur) {
        _audioPlayback.setDuration(dur);
        const auto playingEventId = _audioPlayback.eventId();
        if (dur > 0 && !playingEventId.isEmpty()) {
            _cachedAudioDurations.insert(playingEventId, dur);
            QString mxc;
            const auto playIdx = _messageIndex.physicalIndexOf(playingEventId);
            if (playIdx >= 0 && playIdx < _messages.size()) {
                setMediaDurationMs(_messages[playIdx], dur);
                mxc = mediaUrl(_messages[playIdx]);
            }
            // Persist so the length shows on later loads without replaying.
            if (!mxc.isEmpty()) {
                emit audioDurationLearned(mxc, static_cast<quint64>(dur));
            }
        }
    });
    connect(_mediaPlayer, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
        if (state == QMediaPlayer::StoppedState) {
            if (_audioPlaybackBuffer) {
                _audioPlaybackBuffer->deleteLater();
                _audioPlaybackBuffer = nullptr;
            }
            _audioPlayback.stop();
            _hoveredVoiceSeekEventId.clear();
            _hoveredVoiceSeekProgress = -1.0;
            _playbackRepaintTimer.stop();
            update();
        }
    });
    connect(_mediaPlayer, &QMediaPlayer::errorOccurred,
        this, [this](QMediaPlayer::Error error, const QString &msg) {
        qWarning() << "Audio playback error:" << error << msg;
        const auto fallbackUrl = _audioPlayback.memoryPlaybackActive()
            ? _audioPlayback.mediaUrl()
            : QString();
        const auto fallbackEventId = _audioPlayback.eventId();
        stopAudio();
        if (!fallbackUrl.isEmpty() && !fallbackEventId.isEmpty()) {
            emit audioDownloadRequested(fallbackUrl, fallbackEventId);
        }
    });

    _playbackRepaintTimer.setInterval(33); // ~30fps
    connect(&_playbackRepaintTimer, &QTimer::timeout, this, [this] {
        // Only repaint the row containing the playing message.
        const auto i = _messageIndex.physicalIndexOf(_audioPlayback.eventId());
        if (i >= 0 && i < _layout.size()) {
            update(QRect(0, _layout[i].y + _contentOffset, width(), _layout[i].height));
        }
    });

    // Duration probe player — extracts duration from local files when server
    // metadata is missing (no audio output needed, metadata only).
    _probePlayer = new QMediaPlayer(this);
    connect(_probePlayer, &QMediaPlayer::durationChanged, this, [this](qint64 dur) {
        if (dur <= 0 || _probeEventId.isEmpty()) {
            return;
        }
        // Mark as successfully probed now (not before).
        _probedEventIds.insert(_probeEventId);
        // Persist in the cross-setMessages cache.
        _cachedAudioDurations.insert(_probeEventId, dur);
        // Write duration back onto the matching TimelineItem.
        QString mxc;
        const auto probeIdx = _messageIndex.physicalIndexOf(_probeEventId);
        if (probeIdx >= 0 && probeIdx < _messages.size()) {
            setMediaDurationMs(_messages[probeIdx], dur);
            mxc = mediaUrl(_messages[probeIdx]);
        }
        // Persist so the length shows on later loads without re-probing.
        if (!mxc.isEmpty()) {
            emit audioDurationLearned(mxc, static_cast<quint64>(dur));
        }
        _probePlayer->stop();
        _probeEventId.clear();
        update();
    });
    // If the probe fails (format error, missing file), keep the event in
    // _probedEventIds so the paint path skips it and won't retry on every repaint.
    connect(_probePlayer, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &) {
        if (!_probeEventId.isEmpty()) {
            _probedEventIds.insert(_probeEventId);
        }
        _probePlayer->stop();
        _probeEventId.clear();
    });

    _videoThumbnailProbe = new HistoryVideoThumbnailProbeState(
        this,
        _messages,
        [this](const QString &eventId, const QString &mxcUrl) {
            emit videoLocalThumbnailRequested(eventId, mxcUrl);
        });

    // Hover reaction button: collapse/hide after a short debounce so moving
    // the cursor between bubble, button and column doesn't drop it (no anim).
    _reactionHideTimer.setSingleShot(true);
    _reactionHideTimer.setInterval(300);
    connect(&_reactionHideTimer, &QTimer::timeout, this, [this] {
        hideReactionAffordance();
    });

    // Show the resting reaction pill only after a short hover, so it appears a
    // little after the reply pill (show delay).
    _reactionShowTimer.setSingleShot(true);
    _reactionShowTimer.setInterval(kReactionShowDelayMs);
    connect(&_reactionShowTimer, &QTimer::timeout, this, [this] {
        const auto idx = _reactionPendingIndex;
        _reactionPendingIndex = -1;
        if (idx < 0 || idx >= _messages.size()) {
            return;
        }
        _reactionPillIndex = idx;
        _reactionExpanded = false;
        _reactionHovered = -1;
        update();
    });

    // Expand the pill into the emoji column only after resting on it briefly,
    // rather than the instant the cursor arrives (expand delay).
    _reactionExpandTimer.setSingleShot(true);
    _reactionExpandTimer.setInterval(kReactionExpandDelayMs);
    connect(&_reactionExpandTimer, &QTimer::timeout, this, [this] {
        const auto idx = _reactionExpandPendingIndex;
        _reactionExpandPendingIndex = -1;
        if (idx >= 0 && idx == _reactionPillIndex && !_reactionExpanded) {
            expandReactionColumn(idx);
        }
    });
}

HistoryList::~HistoryList() {
    delete _timestampTooltip;
    _timestampTooltip = nullptr;
}

void HistoryList::setPreviewInfo(const QString &name, const QString &topic) {
    if (_previewName == name && _previewTopic == topic) {
        return;
    }
    _previewName = name;
    _previewTopic = topic;
    update();
}

void HistoryList::setSyncing(bool syncing) {
    if (_syncing != syncing) {
        _syncing = syncing;
        update();
    }
}

void HistoryList::clearProbedState() {
    _probedEventIds.clear();
    _videoThumbnailProbe->clear();
}

void HistoryList::enrichMessages(
        const std::function<void(TimelineItem &)> &mutator) {
    for (auto &message : _messages) {
        mutator(message);
    }
    update();
}

bool HistoryList::hasSender(const QString &userId) const {
    for (const auto &message : _messages) {
        if (message.sender.id == userId) {
            return true;
        }
    }
    return false;
}

QString HistoryList::senderName(const QString &userId) const {
    for (const auto &message : _messages) {
        if (message.sender.id == userId && !message.sender.name.isEmpty()) {
            return message.sender.name;
        }
    }
    return QString();
}

void HistoryList::setLoadingTimeline(bool loading) {
    if (_loadingTimeline == loading) {
        return;
    }
    _loadingTimeline = loading;
    // Show the preloader immediately whenever a fetch is needed — no grace delay.
    _loadingTimelineVisible = loading;
    update();
}

void HistoryList::setJumpLoadingCover(bool on) {
    if (_jumpLoadingCover == on) {
        return;
    }
    _jumpLoadingCover = on;
    update();
}

void HistoryList::setShowOutgoingPrivateAvatars(bool show) {
    if (_showOutgoingPrivateAvatars == show) {
        return;
    }
    _showOutgoingPrivateAvatars = show;
    update();
}

void HistoryList::setSavedMessagesMode(bool saved) {
    if (_savedMessagesMode == saved) {
        return;
    }
    _savedMessagesMode = saved;
    if (saved) {
        hideReactionAffordance();
    }
    update();
}

void HistoryList::setUnreadBar(const QString &firstUnreadEventId, int unreadCount) {
    if (_firstUnreadEventId == firstUnreadEventId && _unreadBarCount == unreadCount) {
        return;
    }
    _firstUnreadEventId = firstUnreadEventId;
    _unreadBarCount = unreadCount;
    recalculateLayout();
    const auto visibleHeight = parentWidget() ? parentWidget()->height() : height();
    resize(width(), qMax(_totalHeight, visibleHeight));
    update();
}

void HistoryList::clearUnreadBar() {
    if (_firstUnreadEventId.isEmpty()) {
        return;
    }
    _firstUnreadEventId.clear();
    _unreadBarCount = 0;
    recalculateLayout();
    const auto visibleHeight = parentWidget() ? parentWidget()->height() : height();
    resize(width(), qMax(_totalHeight, visibleHeight));
    update();
}

void HistoryList::relayout() {
    _lastLayoutWidth = -1; // force recalc even if width is unchanged
    recalculateLayout();
    const auto visibleHeight = parentWidget() ? parentWidget()->height() : height();
    resize(width(), qMax(_totalHeight, visibleHeight));
    update();
}

void HistoryList::setMarkingMessagesRead(bool marking) {
    if (_markingMessagesRead == marking) {
        return;
    }
    _markingMessagesRead = marking;
    if (marking) {
        // Window/room became active: run detection now (no scroll needed).
        checkReadProgress();
    }
}

void HistoryList::setReadFrontier(const QString &eventId) {
    if (_readFrontierEventId == eventId) {
        return;
    }
    _readFrontierEventId = eventId;
    if (eventId.isEmpty()) {
        // Boundary / fully-read: forget the last emit so the next room (or the
        // next unread run in this room) is detected from scratch.
        _lastReadTillEmitted.clear();
    }
    checkReadProgress(); // a frontier push may arm detection with no scroll
}

void HistoryList::checkReadProgress() {
    if (!_markingMessagesRead
        || _pinnedMode
        || _layout.size() != _messages.size()) {
        return;
    }
    // Anchor at the advancing frontier; before the first frontier push (initial
    // entry) fall back to the frozen visual bar. Never the other way round.
    const auto anchor = UnreadBar::detectorAnchor(
        _readFrontierEventId,
        _firstUnreadEventId);
    if (anchor.isEmpty()) {
        return;
    }
    const auto first = _messageIndex.physicalIndexOf(anchor);
    if (first < 0) {
        return; // frontier not in the loaded window
    }
    // Measure from the widget's actual offset (child sits at y = -scrollTop),
    // the same self-heal paintEvent uses — _visibleTop can lag a resize →
    // rangeChanged cascade and read a stale viewport.
    const auto viewportBottom = qMax(0, -y())
        + (parentWidget() ? parentWidget()->height() : height());
    const auto readTillIdx = UnreadRead::readTillIndex(
        _layout.size(),
        [this](int i) { return _layout[i].y + _contentOffset + _layout[i].height; },
        first,
        viewportBottom,
        _readDetectionHold);
    if (readTillIdx < 0) {
        return;
    }
    const auto readTill = _messages[readTillIdx].eventId;
    if (readTill.isEmpty() || readTill == _lastReadTillEmitted) {
        return;
    }
    _lastReadTillEmitted = readTill;
    QMetaObject::invokeMethod(this, [this, readTill] {
        emit messagesReadTill(readTill);
    }, Qt::QueuedConnection); // deferred, matching the former paint-driven emit
}

qint64 HistoryList::eventTimestamp(const QString &eventId) const {
    if (eventId.isEmpty()) {
        return 0;
    }
    const auto i = _messageIndex.physicalIndexOf(eventId);
    return (i >= 0 && i < _messages.size()) ? _messages[i].timestamp : 0;
}

void HistoryList::invalidateLayoutForMedia(const QString &mxcUrl) {
    notePaintCause(&PaintCauses::mediaInvalidate);
    if (mxcUrl.isEmpty()) {
        return;
    }
    QSet<QString> mediaUrls;
    mediaUrls.insert(mxcUrl);
    invalidateLayoutForMedia(mediaUrls);
}

void HistoryList::invalidateLayoutForMedia(const QSet<QString> &mediaUrls) {
    notePaintCause(&PaintCauses::mediaInvalidate);
    if (mediaUrls.isEmpty() || _layout.size() != _messages.size()) {
        return;
    }
    bool invalidated = false;
    int firstDirty = _messages.size();
    int lastDirty = -1;
    for (int i = 0; i < _messages.size(); ++i) {
        const auto &msg = _messages[i];
        const auto url = mediaUrl(msg);
        const auto thumbUrl = mediaThumbUrl(msg);
        const auto preview = urlPreviewInfo(msg);
        const auto srvThumbKey = QStringLiteral("srvthumb:") + url;
        const auto srvThumbMatch = !url.isEmpty()
            && mediaUrls.contains(srvThumbKey);
        const auto mediaUrlMatch = !url.isEmpty()
            && mediaUrls.contains(url);
        if (mediaUrlMatch
            || (!thumbUrl.isEmpty() && mediaUrls.contains(thumbUrl))
            || (!msg.sender.avatarUrl.isEmpty() && mediaUrls.contains(msg.sender.avatarUrl))
            || (preview && !preview->imageUrl.isEmpty()
                && mediaUrls.contains(MediaCache::previewImageKey(preview->imageUrl)))
            || srvThumbMatch) {
            _layout[i].cachedBubbleHeight = -1;
            _layout[i].cachedWidth = -1;
            invalidated = true;
            firstDirty = qMin(firstDirty, i);
            lastDirty = i;
            // Probe audio duration for newly-resolved audio files.
            if (mediaUrlMatch
                && HistoryMessage::isAudioBubble(msg)
                && mediaDurationMs(msg) == 0
                && !_probedEventIds.contains(msg.eventId)) {
                const auto localPath = MediaCache::localPath(url);
                if (!localPath.isEmpty()) {
                    probeAudioDuration(msg.eventId, localPath);
                }
            }
            // Probe video thumbnail from resolved media or srvthumb files.
            if ((mediaUrlMatch || srvThumbMatch)
                && isVideoMessage(msg)
                && thumbUrl.isEmpty()) {
                _videoThumbnailProbe->queueMessage(i);
            }
        }
    }
    if (invalidated) {
        // Only rows from the first invalidated one down changed height; reuse the
        // prefix above it instead of recomputing the whole timeline.
        recalculateLayoutFrom(firstDirty);
        const auto visibleHeight = parentWidget() ? parentWidget()->height() : height();
        resize(width(), qMax(_totalHeight, visibleHeight));
        // Repaint only if a height-changed row is actually in view; rows below
        // that merely shifted are handled by the caller's scroll-anchor
        // correction, and a fully off-screen resolve needs no repaint at all.
        if (lastDirty >= 0) {
            const auto dirtyTop = _layout[firstDirty].y + _contentOffset;
            const auto dirtyBottom = _layout[lastDirty].y
                + _layout[lastDirty].height + _contentOffset;
            const auto viewTop = qMax(0, _visibleTop);
            const auto viewBottom = viewTop + visibleHeight;
            if (dirtyBottom > viewTop && dirtyTop < viewBottom) {
                update();
            }
        }
    }
}

HistoryList::ReactionResult HistoryList::applyReactionLocally(
    const QString &eventId,
    const QString &key,
    bool active) {
    if (eventId.isEmpty() || key.isEmpty()) {
        return {};
    }

    const auto targetIndex = _messageIndex.physicalIndexOf(eventId);
    if (targetIndex < 0 || targetIndex >= _messages.size()) {
        return {};
    }

    if (!applyReactionChange(_messages[targetIndex].reactions, key, active)) {
        return {};
    }

    // Mark as pending so in-place slice updates don't overwrite with stale server data.
    _pendingReactionEventIds.insert(eventId);

    // Instead of recalculateLayout() (which recomputes ALL heights and can
    // drift due to text-wrapping differences), only recompute the single
    // changed message and shift subsequent items.
    const auto oldY = (targetIndex < _layout.size())
        ? (_layout[targetIndex].messageY + _contentOffset)
        : 0;
    const auto oldHeight = (targetIndex < _layout.size()) ? _layout[targetIndex].messageHeight : 0;

    if (targetIndex < _layout.size()) {
        MessagePaintContext ctx;
        ctx.width = messageContextWidth();
        ctx.isGroup = _isGroup;
        ctx.showOutgoingPrivateAvatar = _showOutgoingPrivateAvatars;
        ctx.sameSenderAbove = _layout[targetIndex].sameSenderAbove;
        ctx.timelineIndex = &_timelineLookup;
        ctx.largeEmojiEnabled = _largeEmojiEnabled;
        ctx.itemGlowActive = itemGlowActive(_messages[targetIndex]);
        ctx.urlPreviewFetching = _previewGlowEventIds.contains(_messages[targetIndex].eventId);

        const auto marginTop = _layout[targetIndex].sameSenderAbove
            ? HistoryMessage::kMarginTopAttached
            : HistoryMessage::kMarginTop;
        const auto bubbleH = HistoryMessage::bubbleHeight(
                _messages[targetIndex], width(), ctx);
        _layout[targetIndex].cachedBubbleHeight = bubbleH;
        _layout[targetIndex].cachedWidth = width();
        _layout[targetIndex].cachedSameSenderAbove = _layout[targetIndex].sameSenderAbove;
        const auto newHeight = bubbleH + marginTop + HistoryMessage::kMarginBottom;
        const auto delta = newHeight - oldHeight;

        _layout[targetIndex].messageHeight = newHeight;
        _layout[targetIndex].height += delta;
        // Shift all subsequent items' y positions.
        for (int i = targetIndex + 1; i < _layout.size(); ++i) {
            _layout[i].y += delta;
            _layout[i].messageY += delta;
        }
        _totalHeight += delta;
        updateSenderGroupBounds();
    }

    const auto visibleHeight = parentWidget() ? parentWidget()->height() : height();
    // Keep bottom-alignment offset in sync with the changed total height
    // (mirrors recalculateLayout); otherwise short timelines paint at stale y.
    _contentOffset = qMax(0, visibleHeight - _totalHeight);
    resize(width(), qMax(_totalHeight, visibleHeight));
    update();

    const auto newHeight = (targetIndex < _layout.size()) ? _layout[targetIndex].height : 0;
    return { true, newHeight - oldHeight, oldY };
}

int HistoryList::applyReactionUpdates(
    const QVector<TimelineItem> &incoming,
    int scrollTop) {
    // Find which messages have reaction changes and update them in place.
    // Only recompute heights for changed messages (avoids full recalculate
    // which can drift other messages' heights).
    int scrollDelta = 0;
    bool anyChanged = false;
    const auto count = qMin(_messages.size(), incoming.size());
    bool boundsChanged = false;

    for (int i = 0; i < count; ++i) {
        if (_messages[i].reactions != incoming[i].reactions) {
            _messages[i].reactions = incoming[i].reactions;

            if (i < _layout.size()) {
                const auto oldHeight = _layout[i].messageHeight;
                MessagePaintContext ctx;
                ctx.width = messageContextWidth();
                ctx.isGroup = _isGroup;
                ctx.showOutgoingPrivateAvatar = _showOutgoingPrivateAvatars;
                ctx.sameSenderAbove = _layout[i].sameSenderAbove;
                ctx.timelineIndex = &_timelineLookup;
                ctx.largeEmojiEnabled = _largeEmojiEnabled;
                ctx.itemGlowActive = itemGlowActive(_messages[i]);
                ctx.urlPreviewFetching = _previewGlowEventIds.contains(_messages[i].eventId);
                const auto marginTop = _layout[i].sameSenderAbove
                    ? HistoryMessage::kMarginTopAttached
                    : HistoryMessage::kMarginTop;
                const auto bubbleH = HistoryMessage::bubbleHeight(
                        _messages[i], width(), ctx);
                _layout[i].cachedBubbleHeight = bubbleH;
                _layout[i].cachedWidth = width();
                _layout[i].cachedSameSenderAbove = _layout[i].sameSenderAbove;
                const auto newHeight = bubbleH + marginTop + HistoryMessage::kMarginBottom;
                const auto delta = newHeight - oldHeight;
                if (delta != 0) {
                    _layout[i].messageHeight = newHeight;
                    _layout[i].height += delta;
                    for (int j = i + 1; j < _layout.size(); ++j) {
                        _layout[j].y += delta;
                        _layout[j].messageY += delta;
                    }
                    _totalHeight += delta;
                    // Defer the O(N) group-bounds rebuild to a single call after
                    // the loop — it recomputes from the final layout, so per-item
                    // rebuilds were redundant.
                    boundsChanged = true;
                    if (_layout[i].messageY + _contentOffset < scrollTop) {
                        scrollDelta += delta;
                    }
                }
            }
            anyChanged = true;
        }
    }

    if (!anyChanged) {
        return 0;
    }
    if (boundsChanged) {
        updateSenderGroupBounds();
    }

    const auto visibleHeight = parentWidget() ? parentWidget()->height() : height();
    resize(width(), qMax(_totalHeight, visibleHeight));
    update();
    return scrollDelta;
}

void HistoryList::setMessages(const QVector<TimelineItem> &messages) {
    notePaintCause(&PaintCauses::sliceReplace);
    // A real replace (room switch, mode change, settle flush) supersedes any
    // slice stashed while scrolling — drop it so it can't clobber the new room.
    _deferredReplaceSlice.reset();
    stopAudio();
    // Wholesale replacement of one non-empty window with another (a gappy-sync
    // reset or live↔focused switch) moves the viewport without user action —
    // hold read detection until the user scrolls. Initial fill (old empty) and
    // the room-switch clear (setMessages({})) are excluded, so neither arms it.
    if (!_messages.isEmpty() && !messages.isEmpty()) {
        _readDetectionHold = true;
    }
    _pendingReactionEventIds.clear();
    if (_selection.inSelectionMode()) {
        exitSelectionMode();
    }

    // Accumulate locally-probed audio durations into the persistent cache
    // so they survive across setMessages({}) + setMessages(fresh) sequences
    // (e.g. room switching calls setMessages({}) first, destroying _messages).
    for (const auto &msg : _messages) {
        const auto duration = mediaDurationMs(msg);
        if (duration > 0 && !msg.eventId.isEmpty()) {
            _cachedAudioDurations.insert(msg.eventId, duration);
        }
    }

    _messages = messages;

    // Re-apply probed durations when the server doesn't provide them.
    _sendingCount = 0;
    for (auto &msg : _messages) {
        if (mediaDurationMs(msg) == 0 && !msg.eventId.isEmpty()) {
            const auto it = _cachedAudioDurations.constFind(msg.eventId);
            if (it != _cachedAudioDurations.cend()) {
                setMediaDurationMs(msg, it.value());
            }
        }
        applyPendingPollSelection(msg);
        // Pinned section is a read-only view: hide the per-bubble pin badge
        // (the section itself is the pin indicator) and drop reactions (no
        // strip, no reacting). The reply *quote* is kept for context; the
        // floating "Reply" action pill is suppressed at hover/paint time.
        if (_pinnedMode) {
            msg.isPinned = false;
            msg.reactions.clear();
        }
        if (msg.delivery.sendState == SendState::Sending) {
            ++_sendingCount;
        }
    }

    _layout.clear(); // Force cache invalidation when messages are replaced.
    _lastLayoutWidth = -1; // Force recalculateLayout on next resize.
    _probedEventIds.clear();
    // Transient per-message visuals (delete dim, URL-preview glow) must survive
    // live content slices — setMessages(slice.items) runs on every timeline
    // update — so only a room switch/reset (setMessages({})) clears them.
    if (messages.isEmpty()) {
        _deletingEventIds.clear();
        _previewGlowEventIds.clear();
    }
    _probeEventId.clear();
    if (_probePlayer) _probePlayer->stop();
    _videoThumbnailProbe->clear();
    HistoryMessage::clearPaintCache();
    _scrollDateHideTimer.stop();
    _scrollDateOpacityAnimation.stop();
    _scrollDateShown = false;
    _scrollDateOpacity = 0.0;
    _scrollDateTopTimestamp = 0;
    _visibleTop = 0;
    _lastScrollVisibleTop = -1;
    _lastScrollDateIndex = -1;
    updateSendStateAnimationTimer();
    _highlightedEventId.clear();
    _highlightOpacity = 0.0f;
    _highlightTimer.stop();
    rebuildMessageIndex();
    clearSelection();
    _utdGlowFirstSeenMs.clear();
    recalculateLayout();
    refreshDecryptingGlowState();
    // Inner widget is always at least viewport height.
    // This ensures short chats show messages at the bottom, not the top.
    const auto visibleHeight = parentWidget() ? parentWidget()->height() : height();
    resize(width(), qMax(_totalHeight, visibleHeight));
    _videoThumbnailProbe->queueResolved();
    update();
}

void HistoryList::appendMessage(const TimelineItem &message) {
    notePaintCause(&PaintCauses::append);
    _deferredReplaceSlice.reset();
    const auto firstNewIndex = _messages.size();
    auto pendingAware = message;
    applyPendingPollSelection(pendingAware);
    _messages.append(pendingAware);
    if (pendingAware.delivery.sendState == SendState::Sending) {
        ++_sendingCount;
    }
    updateSendStateAnimationTimer();
    if (!pendingAware.eventId.isEmpty()) {
        _messageIndex.setAt(pendingAware.eventId, _messages.size() - 1);
    }
    const auto prevContentOffset = _contentOffset;
    recalculateLayoutAppend(firstNewIndex);
    refreshDecryptingGlowState();
    const auto visibleHeight = parentWidget() ? parentWidget()->height() : height();
    resize(width(), qMax(_totalHeight, visibleHeight));
    _videoThumbnailProbe->queueMessage(firstNewIndex);
    if (_contentOffset == prevContentOffset
        && firstNewIndex > 0
        && firstNewIndex - 1 < _layout.size()) {
        // Existing rows didn't shift: repaint only the appended region plus the
        // previous last row (whose grouping tail can change), not the whole view.
        const auto top = _layout[firstNewIndex - 1].y + _contentOffset;
        update(0, top, width(), height() - top);
    } else {
        update();
    }
}

void HistoryList::appendMessages(const QVector<TimelineItem> &messages) {
    notePaintCause(&PaintCauses::append);
    if (messages.isEmpty()) {
        return;
    }
    _deferredReplaceSlice.reset();
    const auto firstNewIndex = _messages.size();
    for (const auto &msg : messages) {
        auto pendingAware = msg;
        applyPendingPollSelection(pendingAware);
        _messages.append(pendingAware);
        if (pendingAware.delivery.sendState == SendState::Sending) {
            ++_sendingCount;
        }
        if (!pendingAware.eventId.isEmpty()) {
            _messageIndex.setAt(pendingAware.eventId, _messages.size() - 1);
        }
    }
    updateSendStateAnimationTimer();
    const auto prevContentOffset = _contentOffset;
    recalculateLayoutAppend(firstNewIndex);
    refreshDecryptingGlowState();
    const auto visibleHeight = parentWidget() ? parentWidget()->height() : height();
    resize(width(), qMax(_totalHeight, visibleHeight));
    _videoThumbnailProbe->queueResolved(firstNewIndex);
    if (_contentOffset == prevContentOffset
        && firstNewIndex > 0
        && firstNewIndex - 1 < _layout.size()) {
        // Existing rows didn't shift: repaint only the appended region plus the
        // previous last row (whose grouping tail can change), not the whole view.
        const auto top = _layout[firstNewIndex - 1].y + _contentOffset;
        update(0, top, width(), height() - top);
    } else {
        update();
    }
}

void HistoryList::setSlice(const TimelineSlice &slice) {
    setSliceImpl(slice);
    // Content/layout may now expose more read messages at/after the frontier
    // without a scroll (in-place edit height change, prepend, full replace).
    checkReadProgress();
}

void HistoryList::setSliceImpl(const TimelineSlice &slice) {
    _canPaginateBack = slice.canPaginateBack;
    _canPaginateForward = slice.canPaginateForward;
    _hitTimelineStart = slice.hitTimelineStart;
    const bool modeChanged = (slice.isLive != _isLive)
        || (slice.focusEventId != _focusEventId);
	_isLive = slice.isLive;
	_focusEventId = slice.focusEventId;

	if (!modeChanged && !_messages.isEmpty()) {
		if (slice.updateKind == TimelineUpdateKind::Append) {
			if (slice.updateIndex != _messages.size()) {
				return;
			}
			appendMessages(slice.items);
			return;
		} else if (slice.updateKind == TimelineUpdateKind::Prepend) {
			if (slice.updateIndex != 0) {
				return;
			}
			prependMessages(slice.items);
			return;
		} else if (slice.updateKind == TimelineUpdateKind::MetadataOnly) {
			update();
			return;
		}
	}

	const auto applyOverlapUpdates = [&](const QVector<TimelineItem> &items) {
        if (items.isEmpty() || _messages.isEmpty()
            || items.size() != _messages.size()
            || items.first().eventId != _messages.first().eventId
            || items.last().eventId != _messages.last().eventId) {
            return false;
        }

        bool anyVisualChanged = false;
        bool heightChanged = false;
        // Rows whose height changes in THIS update. The scroll anchor must not be
        // one of them: pinning the top of a row that is itself growing pushes
        // everything below it down by that row's delta, which is exactly the drift
        // seen while a decryption backlog resolves.
        QSet<int> resizedRows;
        for (int i = 0; i < items.size() && i < _messages.size(); ++i) {
            const auto &newItem = items[i];
            auto &oldItem = _messages[i];
            if (oldItem.eventId != newItem.eventId) {
                return false;
            }
            const auto heightAffecting = timelineItemHeightAffectingChanged(
                oldItem,
                newItem);
            const auto visualChanged = timelineItemVisualChanged(
                oldItem,
                newItem);
            if (visualChanged) {
                anyVisualChanged = true;
                if (oldItem.delivery.sendState == SendState::Sending && newItem.delivery.sendState != SendState::Sending) {
                    --_sendingCount;
                } else if (oldItem.delivery.sendState != SendState::Sending && newItem.delivery.sendState == SendState::Sending) {
                    ++_sendingCount;
                }
                const auto localReactions = oldItem.reactions;
                const auto isPending = _pendingReactionEventIds.contains(newItem.eventId);
                oldItem = newItem;
                if (isPending) {
                    int localTotal = 0, serverTotal = 0;
                    for (const auto &r : localReactions) localTotal += r.count;
                    for (const auto &r : newItem.reactions) serverTotal += r.count;
                    if (localTotal != serverTotal) {
                        oldItem.reactions = localReactions;
                    } else {
                        _pendingReactionEventIds.remove(newItem.eventId);
                    }
                }
                _videoThumbnailProbe->queueMessage(i);
                if (heightAffecting && i < _layout.size()) {
                    _layout[i].cachedBubbleHeight = -1;
                    heightChanged = true;
                    resizedRows.insert(i);
                }
            }
        }
        if (anyVisualChanged) {
            updateSendStateAnimationTimer();
        }
        // A UTD may have been replaced by plaintext (R2D2 Set diff) or vice
        // versa; re-arm/stop the shimmer timer accordingly.
        refreshDecryptingGlowState();
        if (heightChanged) {
            const auto visibleHeight = parentWidget() ? parentWidget()->height() : height();
            const auto currentVisibleTop = qMax(0, -y());
            const auto oldScrollMax = qMax(0, height() - visibleHeight);
            const auto wasAtBottom = (currentVisibleTop >= oldScrollMax - kBottomSnapTolerance);
            saveScrollAnchor(&resizedRows);
            recalculateLayout();
            const auto targetHeight = qMax(_totalHeight, visibleHeight);
            resize(width(), targetHeight);
            if (wasAtBottom) {
                emit scrollToRequested(qMax(0, targetHeight - visibleHeight));
            } else {
                restoreScrollAnchor();
            }
        }
        update();
        return true;
    };

    // Step 1: Mode change or empty list -> full replace.
    if (modeChanged || _messages.isEmpty()) {
        setMessages(slice.items);
        return;
    }

    // Step 2: Edge growth detection — the incoming slice may prepend older
    // messages, append newer ones, or do both while keeping our current
    // messages as one contiguous overlap.
    if (!slice.items.isEmpty() && !_messages.isEmpty()) {
        const auto &oldFirstId = _messages.first().eventId;
        const auto &oldLastId = _messages.last().eventId;
        int firstMatch = -1;
        int lastMatch = -1;
        for (int i = 0; i < slice.items.size(); ++i) {
            if (slice.items[i].eventId == oldFirstId) {
                firstMatch = i;
            }
            if (slice.items[i].eventId == oldLastId) {
                lastMatch = i;
            }
            if (firstMatch >= 0 && lastMatch >= 0) break;
        }
        if (firstMatch >= 0
            && lastMatch >= firstMatch
            && (lastMatch - firstMatch + 1) == _messages.size()) {
            const auto prependCount = firstMatch;
            const auto appendCount = slice.items.size() - lastMatch - 1;

            if (prependCount > 0) {
                QVector<TimelineItem> olderItems;
                olderItems.reserve(prependCount);
                for (int i = 0; i < prependCount; ++i) {
                    olderItems.append(slice.items[i]);
                }
                prependMessages(olderItems);
            }

            if (appendCount > 0) {
                QVector<TimelineItem> newerItems;
                newerItems.reserve(appendCount);
                for (int i = lastMatch + 1; i < slice.items.size(); ++i) {
                    newerItems.append(slice.items[i]);
                }
                appendMessages(newerItems);
            }

            if (!applyOverlapUpdates(slice.items)
                && !deferReplaceWhileScrolling(slice)) {
                replaceSliceKeepingViewport(slice.items);
            }
            return;
        }
    }

    // Step 3: In-place updates (edits, reactions) — same first/last IDs AND same count.
    // If count differs, a message was added or removed — fall through to full replace.
    if (applyOverlapUpdates(slice.items)) {
        return;
    }

    // Step 4: Fallback — full replace.
    if (!deferReplaceWhileScrolling(slice)) {
        replaceSliceKeepingViewport(slice.items);
    }
}

// setMessages() rebuilds the list from scratch and leaves the scrollbar value
// untouched, so on a live update it silently teleports the reader. It is the
// right behaviour for a room switch (the caller places the view) but not for the
// full-window slices that a decryption backlog produces: any concurrent window
// change — an arriving event, a redaction, a local echo settling — knocks the
// diff off the in-place path and lands here, mid-read. Anchor around it: pin the
// bottom if the reader was at the bottom, otherwise re-pin the message they were
// looking at. The anchor is keyed by event id and setMessages rebuilds the index,
// so the id survives the rebuild.
void HistoryList::replaceSliceKeepingViewport(const QVector<TimelineItem> &items) {
    const auto visibleHeight = parentWidget() ? parentWidget()->height() : height();
    const auto currentVisibleTop = qMax(0, -y());
    const auto oldScrollMax = qMax(0, height() - visibleHeight);
    const auto wasAtBottom = (currentVisibleTop >= oldScrollMax - kBottomSnapTolerance);

    saveScrollAnchor();
    setMessages(items);
    if (wasAtBottom) {
        emit scrollToRequested(qMax(0, height() - visibleHeight));
    } else {
        restoreScrollAnchor();
    }
}

bool HistoryList::deferReplaceWhileScrolling(const TimelineSlice &slice) {
    // Live slices only. A focused/jump slice carries scroll anchoring applied by
    // HistoryWidget::applyTimelineSlice *around* setSlice; the settle flush goes
    // straight through setSlice and would skip it. Live back-scroll — the case
    // that produces the spikes — has no such dependency.
    if (!_scrolling || !slice.isLive) {
        return false;
    }
    // Overwrite: only the freshest window matters, and the settle flush
    // re-diffs it against whatever _messages is by then.
    _deferredReplaceSlice = slice;
    return true;
}

void HistoryList::prependMessages(const QVector<TimelineItem> &items) {
    notePaintCause(&PaintCauses::prepend);
    _deferredReplaceSlice.reset(); // _messages moves forward; stale stash out
    if (items.isEmpty()) {
        return;
    }

    // Save the total height before prepend so we can compute the exact
    // scroll adjustment needed (simpler and more accurate than anchor-based).
    const auto prevTotalHeight = _totalHeight;
    const auto prevContentOffset = _contentOffset;

    QVector<TimelineItem> newMessages;
    newMessages.reserve(items.size() + _messages.size());
    newMessages.append(items);
    newMessages.append(_messages);
    _messages = newMessages;

    // Shift existing entries via the index base offset and insert only the new
    // front ids — O(prepended), not a full O(n) rebuild.
    QVector<QString> frontIds;
    frontIds.reserve(items.size());
    for (const auto &item : items) {
        frontIds.append(item.eventId);
    }
    _messageIndex.prependFront(frontIds);

    recalculateLayoutPrepend(items.size());
    refreshDecryptingGlowState();

    const auto heightAdded = (_totalHeight - _contentOffset)
        - (prevTotalHeight - prevContentOffset);
    const auto savedVisibleTop = qMax(0, -y());
    const auto targetScroll = qMax(0, savedVisibleTop + heightAdded);

    // Set scroll BEFORE resize to prevent a paint frame at the wrong
    // position. resize() can trigger an immediate repaint on macOS;
    // if the scroll value hasn't been adjusted yet, the user sees the
    // chat jump to the top before snapping back.
    if (heightAdded > 0) {
        emit scrollToRequested(targetScroll);
    }

    const auto visibleHeight = parentWidget() ? parentWidget()->height() : height();
    resize(width(), qMax(_totalHeight, visibleHeight));

    // Re-apply scroll after resize in case the scroll area clamped/reset
    // the value during the geometry change.
    if (heightAdded > 0) {
        emit scrollToRequested(targetScroll);
    }

    _videoThumbnailProbe->queueResolved(0);
    update();
}

void HistoryList::saveScrollAnchor(const QSet<int> *avoidRows) {
    _scrollAnchorEventId.clear();
    _scrollAnchorPixelOffset = 0;
    const auto visibleTop = qMax(0, -y());

    auto lo = 0;
    auto hi = _layout.size();
    while (lo < hi) {
        const auto mid = lo + (hi - lo) / 2;
        const auto rowBottom = _layout[mid].y + _contentOffset + _layout[mid].height;
        if (rowBottom <= visibleTop) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    // The binary search lands on the row straddling the viewport top — which is
    // also the row most likely to be changing height right now (a UTD resolving,
    // a media bubble growing). Anchoring on it pins its TOP while its body grows,
    // shoving the whole viewport down. Walk forward to the first row that is NOT
    // changing, so everything from the anchor down holds still. If every visible
    // row is changing there is nothing stable to hold, and the original row is as
    // good a choice as any.
    if (avoidRows && !avoidRows->isEmpty()) {
        const auto limit = qMin(_layout.size(), _messages.size());
        auto stable = lo;
        while (stable < limit && avoidRows->contains(stable)) {
            ++stable;
        }
        if (stable < limit) {
            lo = stable;
        }
    }
    if (lo < _layout.size() && lo < _messages.size()) {
        _scrollAnchorEventId = _messages[lo].eventId;
        const auto widgetY = _layout[lo].messageY + _contentOffset;
        // How far the message top is from the viewport top.
        // Positive = message top is above viewport (partially visible).
        // Negative = message top is below viewport top.
        _scrollAnchorPixelOffset = widgetY - visibleTop;
    }
}

void HistoryList::restoreScrollAnchor() {
    if (_scrollAnchorEventId.isEmpty()) {
        return;
    }
    const auto i = _messageIndex.physicalIndexOf(_scrollAnchorEventId);
    if (i >= 0 && i < _layout.size()) {
        // The message's new widget Y after layout change.
        const auto newWidgetY = _layout[i].messageY + _contentOffset;
        // Set scroll so the message appears at the same viewport offset.
        const int newScrollTop = newWidgetY - _scrollAnchorPixelOffset;
        emit scrollToRequested(qMax(0, newScrollTop));
    } else if (!_scrollAnchorEventId.isEmpty()) {
        qWarning() << "Scroll anchor message not found:"
            << _scrollAnchorEventId
            << "(may have been deleted)";
    }
    _scrollAnchorEventId.clear();
}

void HistoryList::setRoomId(const QString &roomId) {
    if (_roomId != roomId) {
        _pendingPollSelections.clear();
        if (_inlineVideo) {
            _inlineVideo->stop(); // leaving the room — stop any inline video
        }
    }
    _roomId = roomId;
}

QStringList HistoryList::currentPollSelection(const TimelineItem &item) const {
    if (!item.eventId.isEmpty()) {
        const auto it = _pendingPollSelections.constFind(item.eventId);
        if (it != _pendingPollSelections.cend()) {
            return it.value();
        }
    }
    return selectedPollOptionIds(item);
}

QStringList HistoryList::normalizePollSelection(
    const TimelineItem &item,
    const QStringList &optionIds) const {
    const auto poll = pollContent(item);
    if (!poll) {
        return {};
    }

    const auto maxSelections = qMax(1, poll->maxSelections);
    QSet<QString> wanted;
    for (const auto &optionId : optionIds) {
        wanted.insert(optionId);
    }

    QStringList result;
    result.reserve(qMin(poll->options.size(), maxSelections));
    for (const auto &option : poll->options) {
        if (!wanted.contains(option.id)) {
            continue;
        }
        result.push_back(option.id);
        if (result.size() >= maxSelections) {
            break;
        }
    }
    return result;
}

void HistoryList::applyPendingPollSelection(TimelineItem &item) {
    auto poll = mutablePollContent(item);
    if (!poll || item.eventId.isEmpty()) {
        return;
    }
    if (poll->isClosed) {
        _pendingPollSelections.remove(item.eventId);
        return;
    }

    const auto it = _pendingPollSelections.find(item.eventId);
    if (it == _pendingPollSelections.end()) {
        return;
    }

    const auto serverSelection = selectedPollOptionIds(item);
    const auto normalized = normalizePollSelection(item, it.value());
    if (normalized == serverSelection) {
        _pendingPollSelections.erase(it);
        return;
    }

    for (auto &option : poll->options) {
        option.isChosen = normalized.contains(option.id);
    }
}

bool HistoryList::setPollSelectionLocally(int messageIndex, const QStringList &optionIds) {
    if (messageIndex < 0 || messageIndex >= _messages.size()) {
        return false;
    }

    auto &item = _messages[messageIndex];
    auto poll = mutablePollContent(item);
    if (!poll) {
        return false;
    }

    const auto normalized = normalizePollSelection(item, optionIds);
    if (normalized == currentPollSelection(item)) {
        return false;
    }

    if (!item.eventId.isEmpty()) {
        _pendingPollSelections.insert(item.eventId, normalized);
    }

    for (auto &option : poll->options) {
        option.isChosen = normalized.contains(option.id);
    }
    if (!normalized.isEmpty()) {
        poll->hasVoted = true;
    }
    // `item` is a reference into _messages, already mutated above — no separate
    // by-id copy to sync.
    return true;
}

void HistoryList::setMessagePinState(const QString &eventId, bool pinned) {
    if (eventId.isEmpty()) {
        return;
    }
    const auto idx = _messageIndex.physicalIndexOf(eventId);
    if (idx < 0 || idx >= _messages.size() || _messages[idx].isPinned == pinned) {
        return;
    }
    _messages[idx].isPinned = pinned;
    // The pin badge widens the info line (infoWidth → the skip block reserved in
    // the layout pass), so the bubble must be RE-LAID-OUT, not just repainted —
    // otherwise the badge has no reserved space and isn't drawn until the next
    // full slice rebuild (i.e. reopening the room). This is the optimistic local
    // path; the slice echo relayouts anyway. Pinning is a rare user action, so a
    // full relayout here is fine (mirrors setUnreadBar/clearUnreadBar).
    relayout();
}

bool HistoryList::updateMessageSendState(
    const QString &eventId,
    SendState sendState,
    const QString &newEventId) {
    if (eventId.isEmpty()) {
        return false;
    }
    const auto idx = _messageIndex.physicalIndexOf(eventId);
    if (idx < 0 || idx >= _messages.size()) {
        return false;
    }
    const auto oldState = _messages[idx].delivery.sendState;
    _messages[idx].delivery.sendState = sendState;
    if (oldState == SendState::Sending && sendState != SendState::Sending) {
        --_sendingCount;
    } else if (oldState != SendState::Sending && sendState == SendState::Sending) {
        ++_sendingCount;
    }
    _messageIndex.remove(eventId);
    if (!newEventId.isEmpty()) {
        _messages[idx].eventId = newEventId;
    }
    if (!_messages[idx].eventId.isEmpty()) {
        _messageIndex.setAt(_messages[idx].eventId, idx);
    }
    updateSendStateAnimationTimer();
    update();
    return true;
}

bool HistoryList::updateMessageUploadProgress(
    const QString &eventId,
    double progress) {
    if (eventId.isEmpty()) {
        return false;
    }
    const auto idx = _messageIndex.physicalIndexOf(eventId);
    if (idx < 0 || idx >= _messages.size()) {
        return false;
    }
    _messages[idx].delivery.uploadProgress = progress;
    update();
    return true;
}

bool HistoryList::isDeletingIndex(int index) const {
    return index >= 0
        && index < _messages.size()
        && _deletingEventIds.contains(_messages[index].eventId);
}

void HistoryList::quiesceForDeleting(const QString &eventId) {
    // Playback: no position is stashed — unlike a fullscreen handoff, this
    // message is not coming back.
    if (_inlineVideo && _inlineVideo->activeEventId() == eventId) {
        _inlineVideo->stop();
        _videoSeekDragging = false;
    }
    if (_audioPlayback.eventId() == eventId) {
        stopAudio();
    }

    const auto i = _messageIndex.physicalIndexOf(eventId);
    if (i < 0 || i >= _messages.size()) {
        return;
    }

    // An in-flight download of a message about to vanish is wasted bandwidth.
    const auto url = mediaUrl(_messages[i]);
    if (!url.isEmpty()
        && url.startsWith(QStringLiteral("mxc://"))
        && !MediaCache::isResolved(url)
        && MediaCache::isRequested(url)) {
        emit mediaDownloadCancelRequested(url);
    }

    // Drop affordances already aimed at this row: the cursor may never move
    // again, so mouseMoveEvent's guard would not get a chance to retract them.
    if (_hoveredIndex == i) {
        _hoveredIndex = -1;
        _hoveredLinkStart = -1;
        _hoveredLinkUrl.clear();
        _hoveredFastReply = false;
        _hoveredCopyButton = false;
        _hoveredTimestamp = false;
        _hoveredAvatar = false;
        setCursor(Qt::ArrowCursor);
    }
    if (_replyPillIndex == i) {
        _replyPillIndex = -1;
    }
    if (_reactionPillIndex == i) {
        _reactionPillIndex = -1;
        _reactionExpanded = false;
    }
    if (_hoveredVoiceSeekEventId == eventId) {
        _hoveredVoiceSeekEventId.clear();
        _hoveredVoiceSeekProgress = -1.0;
    }
}

void HistoryList::markDeleting(const QString &eventId) {
    if (eventId.isEmpty() || _deletingEventIds.contains(eventId)) {
        return;
    }
    _deletingEventIds.insert(eventId);
    quiesceForDeleting(eventId);
    const auto i = _messageIndex.physicalIndexOf(eventId);
    if (i >= 0 && i < _layout.size()) {
        update(QRect(0, _layout[i].y + _contentOffset, width(), _layout[i].height));
    } else {
        update();
    }
}

void HistoryList::clearDeleting(const QString &eventId) {
    if (!_deletingEventIds.remove(eventId)) {
        return;
    }
    const auto i = _messageIndex.physicalIndexOf(eventId);
    if (i >= 0 && i < _layout.size()) {
        update(QRect(0, _layout[i].y + _contentOffset, width(), _layout[i].height));
    } else {
        update();
    }
}

void HistoryList::setPreviewFetching(const QString &eventId, bool fetching) {
    if (eventId.isEmpty()) {
        return;
    }
    bool changed = false;
    if (fetching) {
        if (!_previewGlowEventIds.contains(eventId)) {
            _previewGlowEventIds.insert(eventId);
            changed = true;
        }
    } else {
        changed = _previewGlowEventIds.remove(eventId);
    }
    if (!changed) {
        return;
    }
    const auto i = _messageIndex.physicalIndexOf(eventId);
    if (i >= 0 && i < _layout.size()) {
        update(QRect(0, _layout[i].y + _contentOffset, width(), _layout[i].height));
    } else {
        update();
    }
}

bool HistoryList::removeMessage(const QString &eventId) {
    if (eventId.isEmpty()) {
        return false;
    }
    const auto idx = _messageIndex.physicalIndexOf(eventId);
    if (idx < 0 || idx >= _messages.size()) {
        return false;
    }

    if (_messages[idx].delivery.sendState == SendState::Sending) {
        --_sendingCount;
    }
    _messages.removeAt(idx);
    rebuildMessageIndex();
    _layout.clear();
    _lastLayoutWidth = -1;
    recalculateLayout();
    const auto visibleHeight = parentWidget() ? parentWidget()->height() : height();
    resize(width(), qMax(_totalHeight, visibleHeight));
    updateSendStateAnimationTimer();
    update();
    return true;
}

QString HistoryList::eventIdNearDate(qint64 timestamp) const {
    if (_messages.isEmpty()) {
        return {};
    }
    // Messages are ordered oldest-first.  Binary search for the first
    // message with timestamp >= the target date.
    int lo = 0, hi = _messages.size() - 1;
    int result = 0; // fallback to first
    while (lo <= hi) {
        const auto mid = lo + (hi - lo) / 2;
        if (_messages[mid].timestamp >= timestamp) {
            result = mid;
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    // If no message is on or after the target, return the last (closest).
    if (_messages[result].timestamp < timestamp) {
        result = _messages.size() - 1;
    }
    return _messages[result].eventId;
}

int HistoryList::yForEventId(const QString &eventId) const {
    if (eventId.isEmpty()) {
        return -1;
    }
    const auto i = _messageIndex.physicalIndexOf(eventId);
    if (i >= 0 && i < _layout.size()) {
        return _layout[i].messageY + _contentOffset;
    }
    return -1;
}

int HistoryList::rowHeightForEventId(const QString &eventId) const {
    if (eventId.isEmpty()) {
        return -1;
    }
    const auto i = _messageIndex.physicalIndexOf(eventId);
    if (i >= 0 && i < _layout.size()) {
        return _layout[i].messageHeight;
    }
    return -1;
}

void HistoryList::highlightMessage(const QString &eventId) {
    if (eventId.isEmpty()) {
        return;
    }
    _highlightedEventId = eventId;
    _highlightOpacity = 0.0f;
    _highlightStartTime = QDateTime::currentMSecsSinceEpoch();
    if (!_highlightTimer.isActive()) {
        _highlightTimer.start();
    }
    update();
}

void HistoryList::enterSelectionMode(int messageIndex) {
    QSet<QString> selectedIds;
    if (messageIndex >= 0 && messageIndex < _messages.size()) {
        const auto &item = _messages[messageIndex];
        if (contentType(item) != ContentType::Service && !item.eventId.isEmpty() && !item.delivery.deleted) {
            selectedIds.insert(item.eventId);
        }
    }
    if (selectedIds.isEmpty()) {
        return;
    }
    _selection.enterSelectionMode(std::move(selectedIds));
    clearSelection();
    setFocus();
    emit selectionModeChanged(true);
    emit selectedCountChanged(_selection.selectedCount());
    update();
}

void HistoryList::exitSelectionMode() {
    if (!_selection.exitSelectionMode()) {
        return;
    }
    _hoveredLinkStart = -1;
    _hoveredLinkUrl.clear();
    _hoveredCopyButton = false;
    setCursor(Qt::ArrowCursor);
    emit selectionModeChanged(false);
    emit selectedCountChanged(0);
    update();
}

void HistoryList::requestForwardSelected() {
    if (!_selection.inSelectionMode()) {
        return;
    }
    const auto ids = selectedEventIdsInOrder();
    if (ids.isEmpty()) {
        return;
    }
    emit forwardSelectedRequested(ids);
}

bool HistoryList::isAtBottom() const {
    // Will be checked by the scroll area's parent.
    return true;
}

void HistoryList::scrollToBottom() {
    // Handled by the parent ScrollArea.
}

void HistoryList::resizeEvent(QResizeEvent *e) {
    notePaintCause(&PaintCauses::resize);
    if (_inResize) {
        return;
    }
    _inResize = true;
    Ui::RpWidget::resizeEvent(e);

    // Use the effective bubble width as the layout cache key.
    // bubbleMaxWidth caps at kMaxBubbleWidth (430), so once the window
    // is wider than ~542px, horizontal resize produces zero relayout.
    const auto reserved = HistoryMessage::kMarginLeft
        + HistoryMessage::kMarginRight
        + HistoryMessage::kPhotoSkip;
    const auto effectiveWidth = qMax(
        HistoryMessage::kMinBubbleWidth,
        qMin(HistoryMessage::kMaxBubbleWidth, messageContextWidth() - reserved));
    const auto layoutChanged = (effectiveWidth != _lastLayoutWidth);

    if (layoutChanged) {
        _lastScrollVisibleTop = -1;
        _lastScrollDateIndex = -1;
        if (!_messages.isEmpty()) {
            saveScrollAnchor();
            recalculateLayout();
            restoreScrollAnchor();
        }
        _lastLayoutWidth = effectiveWidth;
    }

    // Widget must be at least viewport height.
    // This applies even when messages are empty so that the
    // "Syncing..." loading pill has room to paint.
    const auto visibleHeight = parentWidget() ? parentWidget()->height() : height();
    const auto targetHeight = qMax(_totalHeight, visibleHeight);
    if (height() != targetHeight) {
        resize(width(), targetHeight);
    }

    // Repaint only on a WIDTH change. Bubble x-positions depend on width, and
    // backgroundArea() is sized from the viewport, not from this widget — so a
    // height-only resize (every media resolution grows _totalHeight, ~36 per room
    // open) changes nothing visible, and the path that grew the height already
    // repaints its own dirty rows.
    //
    // Caveat: without WA_StaticContents Qt invalidates the whole widget on any
    // real resize, so this drops a redundant explicit update() but need not drop
    // the paint itself. The measurable open-path win is fewer resizes (debounce
    // the 50ms media-invalidation batch), not this gate.
    if (layoutChanged) {
        update();
    }
    _inResize = false;
}

bool HistoryList::event(QEvent *e) {
    if (e->type() == QEvent::Enter
        || e->type() == QEvent::WindowActivate
        || e->type() == QEvent::ActivationChange
        || e->type() == QEvent::FocusIn) {
        QMetaObject::invokeMethod(this, [this] {
            refreshHoverFromCursor();
        }, Qt::QueuedConnection);
    }

    // Override ShortcutOverride so Cmd+C reaches keyPressEvent
    // instead of being consumed by the Edit > Copy menu action.
    if (e->type() == QEvent::ShortcutOverride) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->matches(QKeySequence::Copy)) {
            TextCursor from, to;
            normalizedSelection(from, to);
            if (from.isValid() && to.isValid()
                && (from.messageIndex != to.messageIndex
                    || from.textPosition != to.textPosition)) {
                e->accept();
                return true;
            }
        }
    }
    return Ui::RpWidget::event(e);
}

void HistoryList::refreshHoverFromPosition(const QPoint &localPos, Qt::MouseButtons buttons) {
    if (!rect().contains(localPos)) {
        return;
    }
    QMouseEvent moveEvent(
        QEvent::MouseMove,
        QPointF(localPos),
        QPointF(mapToGlobal(localPos)),
        Qt::NoButton,
        buttons,
        Qt::NoModifier);
    mouseMoveEvent(&moveEvent);
}

void HistoryList::refreshHoverFromCursor() {
    refreshHoverFromPosition(mapFromGlobal(QCursor::pos()));
}

void HistoryList::recalculateLayout() {
    if (_layout.size() != _messages.size()) {
        _layout.clear();
        _layout.resize(_messages.size());
    }
    _totalHeight = 0;

    // Reserve space for the top-of-timeline indicator (spinner or
    // "beginning of conversation" pill) so it doesn't overlap messages.
    _totalHeight += topTimelineIndicatorHeight(
        !_messages.isEmpty(),
        _canPaginateBack,
        _hitTimelineStart);

    QDate prevDate;
    QString prevSenderId;

    for (int i = 0; i < _messages.size(); ++i) {
        const auto &msg = _messages[i];
        auto &item = _layout[i];
        const auto isService = (contentType(msg) == ContentType::Service);

        // Check if we need a date separator.
        const auto msgDate = QDateTime::fromSecsSinceEpoch(msg.timestamp).date();
        item.showDate = (msgDate != prevDate);
        prevDate = msgDate;

        // Check if this message shows the unread bar.
        item.showUnreadBar = (!_firstUnreadEventId.isEmpty()
            && msg.eventId == _firstUnreadEventId);

        // Check if same sender as previous (for collapsing).
        // Unread bar breaks sender grouping (like date separator).
        item.sameSenderAbove = (!isService
            && !item.showDate
            && !item.showUnreadBar
            && msg.sender.id == prevSenderId);
        prevSenderId = isService ? QString() : msg.sender.id;

        // Check if next message is same sender (for avatar placement).
        // Avatar shows on the last message of a group.
        if (i + 1 < _messages.size()) {
            const auto &next = _messages[i + 1];
            const auto nextDate = QDateTime::fromSecsSinceEpoch(next.timestamp).date();
            const auto nextIsUnreadBar = (!_firstUnreadEventId.isEmpty()
                && next.eventId == _firstUnreadEventId);
            item.sameSenderBelow = (!isService
                && contentType(next) != ContentType::Service
                && nextDate == msgDate
                && !nextIsUnreadBar
                && next.sender.id == msg.sender.id);
        } else {
            item.sameSenderBelow = false;
        }

        const auto beforeMessageHeight = preMessageHeight(item);
        item.y = _totalHeight;
        item.messageY = item.y + beforeMessageHeight;

        const auto marginTop = item.sameSenderAbove
            ? HistoryMessage::kMarginTopAttached
            : HistoryMessage::kMarginTop;

        // Cache key: effective bubble width (caps at kMaxBubbleWidth).
        const auto w = messageContextWidth();
        const auto reserved = HistoryMessage::kMarginLeft
            + HistoryMessage::kMarginRight
            + HistoryMessage::kPhotoSkip;
        const auto ew = qMax(
            HistoryMessage::kMinBubbleWidth,
            qMin(HistoryMessage::kMaxBubbleWidth, w - reserved));
        if (item.cachedBubbleHeight >= 0
            && item.cachedWidth == ew
            && item.cachedSameSenderAbove == item.sameSenderAbove) {
            item.messageHeight = item.cachedBubbleHeight
                + marginTop
                + HistoryMessage::kMarginBottom;
        } else {
            MessagePaintContext ctx;
            ctx.width = w;
            ctx.isGroup = _isGroup;
            ctx.showOutgoingPrivateAvatar = _showOutgoingPrivateAvatars;
            ctx.sameSenderAbove = item.sameSenderAbove;
            ctx.timelineIndex = &_timelineLookup;
            ctx.largeEmojiEnabled = _largeEmojiEnabled;
            ctx.itemGlowActive = itemGlowActive(msg);
            item.cachedBubbleHeight = HistoryMessage::bubbleHeight(msg, w, ctx);
            item.cachedWidth = ew;
            item.cachedSameSenderAbove = item.sameSenderAbove;
            item.messageHeight = item.cachedBubbleHeight
                + marginTop
                + HistoryMessage::kMarginBottom;
        }
        item.height = beforeMessageHeight + item.messageHeight;

        _totalHeight += item.height;
    }

    // Padding below the last message.
    if (!_messages.isEmpty()) {
        _totalHeight += kBottomPadding;
    }

    // Build date-index vector for O(log N) sticky-date lookup.
    _dateIndices.clear();
    for (int i = 0; i < _layout.size(); ++i) {
        if (_layout[i].showDate) _dateIndices.push_back(i);
    }

    updateSenderGroupBounds();

    // When content is shorter than the visible area,
    // offset all items so messages appear at the bottom (not the top).
    const auto visibleHeight = parentWidget() ? parentWidget()->height() : height();
    _contentOffset = qMax(0, visibleHeight - _totalHeight);
}

void HistoryList::recalculateLayoutFrom(int firstDirtyIndex) {
    // Fall back to the full pass when there is no usable prefix or the layout is
    // out of sync (the full version handles clear/resize and the top indicator).
    if (firstDirtyIndex <= 0
        || _layout.size() != _messages.size()
        || firstDirtyIndex >= _messages.size()) {
        recalculateLayout();
        return;
    }

    // The prefix [0, firstDirtyIndex) keeps its y/height/flags; continue the
    // running height from the last prefix row and seed grouping state from it.
    _totalHeight = _layout[firstDirtyIndex - 1].y
        + _layout[firstDirtyIndex - 1].height;
    const auto &prev = _messages[firstDirtyIndex - 1];
    auto prevDate = QDateTime::fromSecsSinceEpoch(prev.timestamp).date();
    auto prevSenderId = (contentType(prev) == ContentType::Service)
        ? QString()
        : prev.sender.id;

    for (int i = firstDirtyIndex; i < _messages.size(); ++i) {
        const auto &msg = _messages[i];
        auto &item = _layout[i];
        const auto isService = (contentType(msg) == ContentType::Service);

        const auto msgDate = QDateTime::fromSecsSinceEpoch(msg.timestamp).date();
        item.showDate = (msgDate != prevDate);
        prevDate = msgDate;

        item.showUnreadBar = (!_firstUnreadEventId.isEmpty()
            && msg.eventId == _firstUnreadEventId);

        item.sameSenderAbove = (!isService
            && !item.showDate
            && !item.showUnreadBar
            && msg.sender.id == prevSenderId);
        prevSenderId = isService ? QString() : msg.sender.id;

        if (i + 1 < _messages.size()) {
            const auto &next = _messages[i + 1];
            const auto nextDate = QDateTime::fromSecsSinceEpoch(next.timestamp).date();
            const auto nextIsUnreadBar = (!_firstUnreadEventId.isEmpty()
                && next.eventId == _firstUnreadEventId);
            item.sameSenderBelow = (!isService
                && contentType(next) != ContentType::Service
                && nextDate == msgDate
                && !nextIsUnreadBar
                && next.sender.id == msg.sender.id);
        } else {
            item.sameSenderBelow = false;
        }

        const auto beforeMessageHeight = preMessageHeight(item);
        item.y = _totalHeight;
        item.messageY = item.y + beforeMessageHeight;

        const auto marginTop = item.sameSenderAbove
            ? HistoryMessage::kMarginTopAttached
            : HistoryMessage::kMarginTop;

        const auto w = messageContextWidth();
        const auto reserved = HistoryMessage::kMarginLeft
            + HistoryMessage::kMarginRight
            + HistoryMessage::kPhotoSkip;
        const auto ew = qMax(
            HistoryMessage::kMinBubbleWidth,
            qMin(HistoryMessage::kMaxBubbleWidth, w - reserved));
        if (item.cachedBubbleHeight >= 0
            && item.cachedWidth == ew
            && item.cachedSameSenderAbove == item.sameSenderAbove) {
            item.messageHeight = item.cachedBubbleHeight
                + marginTop
                + HistoryMessage::kMarginBottom;
        } else {
            MessagePaintContext ctx;
            ctx.width = w;
            ctx.isGroup = _isGroup;
            ctx.showOutgoingPrivateAvatar = _showOutgoingPrivateAvatars;
            ctx.sameSenderAbove = item.sameSenderAbove;
            ctx.timelineIndex = &_timelineLookup;
            ctx.largeEmojiEnabled = _largeEmojiEnabled;
            ctx.itemGlowActive = itemGlowActive(msg);
            item.cachedBubbleHeight = HistoryMessage::bubbleHeight(msg, w, ctx);
            item.cachedWidth = ew;
            item.cachedSameSenderAbove = item.sameSenderAbove;
            item.messageHeight = item.cachedBubbleHeight
                + marginTop
                + HistoryMessage::kMarginBottom;
        }
        item.height = beforeMessageHeight + item.messageHeight;

        _totalHeight += item.height;
    }

    if (!_messages.isEmpty()) {
        _totalHeight += kBottomPadding;
    }

    _dateIndices.clear();
    for (int i = 0; i < _layout.size(); ++i) {
        if (_layout[i].showDate) _dateIndices.push_back(i);
    }

    updateSenderGroupBounds();

    const auto visibleHeight = parentWidget() ? parentWidget()->height() : height();
    _contentOffset = qMax(0, visibleHeight - _totalHeight);
}

void HistoryList::recalculateLayoutAppend(int firstNewIndex) {
    // Sanity: if layout doesn't match the pre-append state, fall back.
    if (firstNewIndex <= 0
        || firstNewIndex > _messages.size()
        || _layout.size() != firstNewIndex) {
        recalculateLayout();
        return;
    }

    // Extend layout vector for the newly appended messages.
    _layout.resize(_messages.size());

    // Resume _totalHeight: subtract the kBottomPadding that was added at
    // the end of the previous layout pass so we can continue accumulating.
    if (_totalHeight > 0) {
        _totalHeight -= kBottomPadding;
    }

    const auto w = messageContextWidth();
    const auto reserved = HistoryMessage::kMarginLeft
        + HistoryMessage::kMarginRight
        + HistoryMessage::kPhotoSkip;
    const auto ew = qMax(
        HistoryMessage::kMinBubbleWidth,
        qMin(HistoryMessage::kMaxBubbleWidth, w - reserved));

    for (int i = firstNewIndex; i < _messages.size(); ++i) {
        const auto &msg = _messages[i];
        auto &item = _layout[i];
        const auto isService = (contentType(msg) == ContentType::Service);

        // Date separator.
        const auto msgDate = QDateTime::fromSecsSinceEpoch(msg.timestamp).date();
        QDate prevDate;
        if (i > 0) {
            prevDate = QDateTime::fromSecsSinceEpoch(
                _messages[i - 1].timestamp).date();
        }
        item.showDate = (msgDate != prevDate);

        // Unread bar.
        item.showUnreadBar = (!_firstUnreadEventId.isEmpty()
            && msg.eventId == _firstUnreadEventId);

        // Same sender as previous.
        const auto prevIsService = (i > 0)
            ? (contentType(_messages[i - 1]) == ContentType::Service)
            : true;
        const auto prevSenderId = (i > 0 && !prevIsService)
            ? _messages[i - 1].sender.id
            : QString();
        item.sameSenderAbove = (!isService
            && !item.showDate
            && !item.showUnreadBar
            && msg.sender.id == prevSenderId);

        // Update the previous item's sameSenderBelow — it was false (last
        // message) and may now need to be true.
        if (i > 0) {
            auto &prev = _layout[i - 1];
            prev.sameSenderBelow = (!isService
                && !prevIsService
                && msgDate == prevDate
                && !item.showUnreadBar
                && msg.sender.id == _messages[i - 1].sender.id);
        }
        // Last message in the batch has no sender below.
        item.sameSenderBelow = false;

        const auto beforeMessageHeight = preMessageHeight(item);
        item.y = _totalHeight;
        item.messageY = item.y + beforeMessageHeight;

        const auto marginTop = item.sameSenderAbove
            ? HistoryMessage::kMarginTopAttached
            : HistoryMessage::kMarginTop;

        MessagePaintContext ctx;
        ctx.width = w;
        ctx.isGroup = _isGroup;
        ctx.showOutgoingPrivateAvatar = _showOutgoingPrivateAvatars;
        ctx.sameSenderAbove = item.sameSenderAbove;
        ctx.timelineIndex = &_timelineLookup;
        ctx.largeEmojiEnabled = _largeEmojiEnabled;
        ctx.itemGlowActive = itemGlowActive(msg);
        item.cachedBubbleHeight = HistoryMessage::bubbleHeight(msg, w, ctx);
        item.cachedWidth = ew;
        item.cachedSameSenderAbove = item.sameSenderAbove;
        item.messageHeight = item.cachedBubbleHeight + marginTop
            + HistoryMessage::kMarginBottom;
        item.height = beforeMessageHeight + item.messageHeight;

        _totalHeight += item.height;
    }

    // Bottom padding.
    if (!_messages.isEmpty()) {
        _totalHeight += kBottomPadding;
    }

    // Update contentOffset.
    const auto visibleHeight = parentWidget() ? parentWidget()->height() : height();
    _contentOffset = qMax(0, visibleHeight - _totalHeight);

    // Rebuild _dateIndices (must stay consistent).
    // Only need to append new date indices from firstNewIndex onward,
    // but rebuilding is simpler and _dateIndices is small.
    _dateIndices.clear();
    for (int i = 0; i < _layout.size(); ++i) {
        if (_layout[i].showDate) _dateIndices.push_back(i);
    }

    updateSenderGroupBounds();
}

void HistoryList::recalculateLayoutPrepend(int prependedCount) {
    const auto oldCount = _messages.size() - prependedCount;
    if (prependedCount <= 0
        || oldCount <= 0
        || _layout.size() != oldCount) {
        recalculateLayout();
        return;
    }

    auto oldLayout = std::move(_layout);
    _layout.clear();
    _layout.resize(_messages.size());

    const auto w = messageContextWidth();
    const auto reserved = HistoryMessage::kMarginLeft
        + HistoryMessage::kMarginRight
        + HistoryMessage::kPhotoSkip;
    const auto ew = qMax(
        HistoryMessage::kMinBubbleWidth,
        qMin(HistoryMessage::kMaxBubbleWidth, w - reserved));

    _totalHeight = topTimelineIndicatorHeight(
        !_messages.isEmpty(),
        _canPaginateBack,
        _hitTimelineStart);

    QDate prevDate;
    QString prevSenderId;

    const auto buildRow = [&](int index) {
        const auto &msg = _messages[index];
        auto &item = _layout[index];
        const auto isService = (contentType(msg) == ContentType::Service);
        const auto msgDate = QDateTime::fromSecsSinceEpoch(msg.timestamp).date();

        item.showDate = (msgDate != prevDate);
        item.showUnreadBar = (!_firstUnreadEventId.isEmpty()
            && msg.eventId == _firstUnreadEventId);
        item.sameSenderAbove = (!isService
            && !item.showDate
            && !item.showUnreadBar
            && msg.sender.id == prevSenderId);

        if (index + 1 < _messages.size()) {
            const auto &next = _messages[index + 1];
            const auto nextDate = QDateTime::fromSecsSinceEpoch(next.timestamp).date();
            const auto nextIsUnreadBar = (!_firstUnreadEventId.isEmpty()
                && next.eventId == _firstUnreadEventId);
            item.sameSenderBelow = (!isService
                && contentType(next) != ContentType::Service
                && nextDate == msgDate
                && !nextIsUnreadBar
                && next.sender.id == msg.sender.id);
        } else {
            item.sameSenderBelow = false;
        }

        const auto beforeMessageHeight = preMessageHeight(item);
        item.y = _totalHeight;
        item.messageY = item.y + beforeMessageHeight;

        const auto marginTop = item.sameSenderAbove
            ? HistoryMessage::kMarginTopAttached
            : HistoryMessage::kMarginTop;

        MessagePaintContext ctx;
        ctx.width = w;
        ctx.isGroup = _isGroup;
        ctx.showOutgoingPrivateAvatar = _showOutgoingPrivateAvatars;
        ctx.sameSenderAbove = item.sameSenderAbove;
        ctx.timelineIndex = &_timelineLookup;
        ctx.largeEmojiEnabled = _largeEmojiEnabled;
        ctx.itemGlowActive = itemGlowActive(msg);
        item.cachedBubbleHeight = HistoryMessage::bubbleHeight(msg, w, ctx);
        item.cachedWidth = ew;
        item.cachedSameSenderAbove = item.sameSenderAbove;
        item.messageHeight = item.cachedBubbleHeight
            + marginTop
            + HistoryMessage::kMarginBottom;
        item.height = beforeMessageHeight + item.messageHeight;

        _totalHeight += item.height;
        prevDate = msgDate;
        prevSenderId = isService ? QString() : msg.sender.id;
    };

    for (int i = 0; i < prependedCount; ++i) {
        buildRow(i);
    }

    // The previous first row may lose its date separator or sender header when
    // older messages are prepended before it, so rebuild only that boundary row.
    buildRow(prependedCount);

    // The old second row's sameSenderAbove (and thus its margins/height/y)
    // depends on the boundary row's date/sender state, which may have changed
    // above. Rebuild it too so the seam matches the full-layout path, then copy
    // the remaining untouched rows verbatim (their grouping depends only on the
    // old second row's intrinsic sender/date, which prepending does not change).
    if (oldCount > 1) {
        buildRow(prependedCount + 1);
    }

    if (oldCount > 2) {
        const auto firstUntouchedOldIndex = 2;
        const auto shiftY = _totalHeight - oldLayout[firstUntouchedOldIndex].y;
        for (int oldIndex = firstUntouchedOldIndex; oldIndex < oldCount; ++oldIndex) {
            auto item = oldLayout[oldIndex];
            item.y += shiftY;
            item.messageY += shiftY;
            const auto newIndex = prependedCount + oldIndex;
            _layout[newIndex] = item;
            _totalHeight = item.y + item.height;
        }
    }

    if (!_messages.isEmpty()) {
        _totalHeight += kBottomPadding;
    }

    const auto visibleHeight = parentWidget() ? parentWidget()->height() : height();
    _contentOffset = qMax(0, visibleHeight - _totalHeight);

    _dateIndices.clear();
    for (int i = 0; i < _layout.size(); ++i) {
        if (_layout[i].showDate) {
            _dateIndices.push_back(i);
        }
    }

    updateSenderGroupBounds();
}

const TimelineItem *HistoryList::messageById(const QString &eventId) const {
    const auto idx = _messageIndex.physicalIndexOf(eventId);
    return (idx >= 0 && idx < _messages.size()) ? &_messages[idx] : nullptr;
}

TimelineItem *HistoryList::messageById(const QString &eventId) {
    const auto idx = _messageIndex.physicalIndexOf(eventId);
    return (idx >= 0 && idx < _messages.size()) ? &_messages[idx] : nullptr;
}

void HistoryList::updateRowByEventId(const QString &eventId) {
    const auto i = _messageIndex.physicalIndexOf(eventId);
    if (i >= 0 && i < _layout.size()) {
        update(QRect(0, _layout[i].y + _contentOffset, width(), _layout[i].height));
    } else {
        update();
    }
}

void HistoryList::updateSenderGroupBounds() {
    auto i = 0;
    while (i < _layout.size()) {
        auto j = i;
        while (j + 1 < _layout.size() && _layout[j].sameSenderBelow) {
            ++j;
        }

        const auto groupTopY = _layout[i].messageY;
        const auto groupBottomY = _layout[j].messageY + _layout[j].messageHeight;
        for (auto k = i; k <= j; ++k) {
            _layout[k].senderGroupFirstIndex = i;
            _layout[k].senderGroupLastIndex = j;
            _layout[k].senderGroupTopY = groupTopY;
            _layout[k].senderGroupBottomY = groupBottomY;
        }
        i = j + 1;
    }
}

void HistoryList::paintEvent(QPaintEvent *e) {
    const bool kPaintStats = paintStatsEnabled();
    int paintedRowCount = 0;
    PaintStatsScope paintStats(kPaintStats, paintedRowCount);
    if (kPaintStats) {
        // Row height varies, so derive "typical" from the live layout rather
        // than guessing a constant: it is what separates a one-row animation
        // repaint from a real scroll strip.
        const auto typicalRow = (!_layout.isEmpty() && _totalHeight > 0)
            ? qMax(1, int(_totalHeight / _layout.size()))
            : 64;
        paintStats.classify(
            e->rect(),
            width(),
            parentWidget() ? parentWidget()->height() : height(),
            typicalRow,
            _scrolling);
    }

    // Sync _visibleTop with the actual widget position at paint time.
    // In a QScrollArea the child is at y = -scrollTop, so -y() gives
    // the true scroll offset.  This prevents gradient misalignment when
    // a paint event fires before updateVisibleTop() has processed the
    // latest scroll-bar change (e.g. after invalidateLayoutForMedia →
    // resize → rangeChanged).
    const auto actualVisibleTop = qMax(0, -y());
    _visibleTop = actualVisibleTop;

    QPainter p(this);

    // Paint chat background: gradient with the soft-light doodle tiled over it,
    // composited once for the whole chat column and slid up so the viewport
    // shows a slice of it. Anchored to the column (not the scroll viewport) so
    // the top bar and composer never rescale it, and to the viewport (not this
    // widget) so it stays put while messages scroll past.
    {
        PaintPhase phase(paintStats.bg());
        const auto bgClip = e->rect();
        const auto area = backgroundArea();
        const auto columnTop = _visibleTop - backgroundOriginY();
        const auto dpr = devicePixelRatioF();
        if (_bgCache.isNull()) {
            _bgCache.rebuild(area, dpr);
        } else if (!_bgCache.matches(area, dpr)) {
            _bgRebuildTimer.start();
        }
        if (_bgCache.isNull()) {
            p.fillRect(bgClip, st::historyBg);
        } else {
            p.save();
            p.setClipRect(bgClip);
            p.translate(0, columnTop);
            if (_bgCache.matches(area, dpr)) {
                p.drawPixmap(0, 0, _bgCache.pixmap());
            } else {
                // Mid-resize: stretch the stale composite until the timer fires.
                p.drawPixmap(QRect(QPoint(), area), _bgCache.pixmap());
            }
            p.restore();
        }
    }

    // Opaque jump "Loading…" cover: the chat background is already painted above;
    // draw a centered pill and skip messages/date-separators/overlays so a jump's
    // focused slice can populate underneath while staying hidden until the reveal.
    if (_jumpLoadingCover) {
        PainterHighQualityEnabler hq(p);
        const auto text = tr("Loading...");
        const auto f = static_cast<const QFont &>(st::msgServiceFont);
        const auto &fm = st::fontMetrics(f);
        const auto pillW = fm.horizontalAdvance(text)
            + st::msgPadding.left() + st::msgPadding.right();
        const auto pillH = fm.height()
            + st::msgServicePadding.top() + st::msgServicePadding.bottom();
        const auto pillR = pillH / 2.0;
        const auto pillX = (width() - pillW) / 2;
        const auto viewportH = parentWidget() ? parentWidget()->height() : height();
        const auto pillY = qMax(0, -y()) + (viewportH - pillH) / 2;
        p.setPen(Qt::NoPen);
        p.setBrush(st::msgServiceBg);
        p.drawRoundedRect(pillX, pillY, pillW, pillH, pillR, pillR);
        p.setFont(f);
        p.setPen(st::msgServiceFg);
        p.drawText(
            pillX + st::msgPadding.left(),
            pillY + st::msgServicePadding.top() + fm.ascent(),
            text);
        return;
    }

    const auto clip = e->rect();
    // Cull rows against the actual dirty region, not just its bounding box: a
    // scroll dirties a top date band + a bottom avatar band whose bounding box
    // still spans the viewport, so per-row region tests skip the untouched
    // middle rows entirely (QPainter is already system-clipped to the region).
    const auto region = e->region();
    const auto dateHeight = dateBadgeTotalHeight();
    const auto visibleTop = qMax(0, _visibleTop);
    auto stickyDateIndex = stickyDateIndexAtVisibleTop(visibleTop);
    int stickyDateY = visibleTop;
    if (stickyDateIndex >= 0) {
        const auto next = std::upper_bound(
            _dateIndices.cbegin(),
            _dateIndices.cend(),
            stickyDateIndex);
        if (next != _dateIndices.cend()) {
            const auto nextDateY = _layout[*next].y + _contentOffset;
            stickyDateY = qMin(stickyDateY, nextDateY - dateHeight);
        }
    }

    // Paint top-of-timeline indicator: spinner or beginning-of-conversation.
    if (!_messages.isEmpty()) {
        if (_canPaginateBack && !_hitTimelineStart) {
            // "Loading..." pill at top of content area.
            PainterHighQualityEnabler hq(p);
            const auto text = tr("Loading...");
            const auto f = static_cast<const QFont &>(st::msgServiceFont);
            const auto &fm = st::fontMetrics(f);
            const auto pillW = fm.horizontalAdvance(text)
                + st::msgPadding.left() + st::msgPadding.right();
            const auto pillH = fm.height()
                + st::msgServicePadding.top() + st::msgServicePadding.bottom();
            const auto pillR = pillH / 2.0;
            const auto pillX = (width() - pillW) / 2;
            const auto pillY = _contentOffset + 8;
            p.setPen(Qt::NoPen);
            p.setBrush(st::msgServiceBg);
            p.drawRoundedRect(pillX, pillY, pillW, pillH, pillR, pillR);
            p.setFont(f);
            p.setPen(st::msgServiceFg);
            p.drawText(
                pillX + st::msgPadding.left(),
                pillY + st::msgServicePadding.top() + fm.ascent(),
                text);
        } else if (_hitTimelineStart && !_savedMessagesMode) {
            // "Beginning of conversation" pill at top of content area (not in
            // Saved Messages — a notepad has no conversation to begin).
            PainterHighQualityEnabler hq(p);
            const auto text = tr("Beginning of conversation");
            const auto f = static_cast<const QFont &>(st::msgServiceFont);
            const auto &fm = st::fontMetrics(f);
            const auto pillW = fm.horizontalAdvance(text)
                + st::msgPadding.left() + st::msgPadding.right();
            const auto pillH = fm.height()
                + st::msgServicePadding.top() + st::msgServicePadding.bottom();
            const auto pillR = pillH / 2.0;
            const auto pillX = (width() - pillW) / 2;
            const auto pillY = _contentOffset + 8;
            p.setPen(Qt::NoPen);
            p.setBrush(st::msgServiceBg);
            p.drawRoundedRect(pillX, pillY, pillW, pillH, pillR, pillR);
            p.setFont(f);
            p.setPen(st::msgServiceFg);
            p.drawText(
                pillX + st::msgPadding.left(),
                pillY + st::msgServicePadding.top() + fm.ascent(),
                text);
        }
    }

    // Explicit timer rather than a scoped PaintPhase: the rows pass ends at the
    // loop's closing brace, and re-bracing that span would change the lifetime
    // of locals the overlay pass below still reads.
    QElapsedTimer rowsPhaseTimer;
    if (kPaintStats) {
        rowsPhaseTimer.start();
    }

    // Binary search for first visible item (std::lower_bound).
    const auto clipTop = clip.top() - _contentOffset;
    int firstVisible = 0;
    if (!_layout.isEmpty()) {
        int lo = 0, hi = _layout.size();
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            if (_layout[mid].y + _layout[mid].height <= clipTop) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        firstVisible = lo;
    }
    for (int i = firstVisible; i < _messages.size(); ++i) {
        const auto &layoutItem = _layout[i];
        const auto rowY = layoutItem.y + _contentOffset;
        const auto messageY = layoutItem.messageY + _contentOffset;

        if (rowY > clip.bottom()) break;
        const QRect rowRect(0, rowY, width(), layoutItem.height);
        if (!region.intersects(rowRect)) {
            continue;
        }
        ++paintedRowCount;

        // Paint date separator if needed.
        if (layoutItem.showDate) {
            if (!(i == stickyDateIndex && rowY < visibleTop)) {
                paintDateSeparator(
                    p,
                    _messages[i].timestamp,
                    rowY,
                    width());
            }
        }

        // Paint unread bar separator if needed.
        if (layoutItem.showUnreadBar) {
            const auto barRegionTop = rowY
                + (layoutItem.showDate ? dateHeight : 0);
            const auto barBgY = barRegionTop + st::historyUnreadBarMargin;
            const auto barRect = QRect(0, barBgY, width(), st::historyUnreadBarHeight);
            if (barRect.intersects(clip)) {
                // Draw 1px border at top, then fill background.
                p.fillRect(QRect(0, barBgY, width(), 1), st::historyUnreadBarBorder);
                p.fillRect(QRect(0, barBgY + 1, width(), st::historyUnreadBarHeight - 1), st::historyUnreadBarBg);
                p.setPen(st::historyUnreadBarFg);
                p.setFont(static_cast<const QFont &>(st::historyUnreadBarFont));
                p.drawText(barRect, Qt::AlignCenter, tr("Unread messages"));
            }
        }

        // Paint the message bubble.
        const auto marginTop = layoutItem.sameSenderAbove
            ? HistoryMessage::kMarginTopAttached
            : HistoryMessage::kMarginTop;

        p.save();
        p.translate(0, messageY + marginTop);

        MessagePaintContext ctx;
        ctx.width = messageContextWidth();
        ctx.isGroup = _isGroup;
        ctx.showOutgoingPrivateAvatar = _showOutgoingPrivateAvatars;
        ctx.sameSenderAbove = layoutItem.sameSenderAbove;
        ctx.sameSenderBelow = layoutItem.sameSenderBelow;
        ctx.isHovered = (i == _hoveredIndex);
        ctx.hoveredFastReply = (i == _hoveredIndex) && _hoveredFastReply;
        ctx.hoveredLinkStart = (i == _hoveredIndex) ? _hoveredLinkStart : -1;
        ctx.sendingAnimationProgress = qreal(_sendStateTick) / 20.0;
        const auto selectable = contentType(_messages[i]) != ContentType::Service
            && !_messages[i].eventId.isEmpty()
            && !_messages[i].delivery.deleted;
        const auto selected = selectable
            && _selection.selectedContains(_messages[i].eventId);
        ctx.selectionMode = _selection.inSelectionMode();
        ctx.messageSelected = _selection.inSelectionMode() && selected;
        ctx.timelineIndex = &_timelineLookup;
        ctx.largeEmojiEnabled = _largeEmojiEnabled;
        ctx.itemGlowActive = itemGlowActive(_messages[i]);
        ctx.urlPreviewFetching = _previewGlowEventIds.contains(_messages[i].eventId);
        ctx.decryptingGlowProgress = _decryptingGlowProgress;
        ctx.paintTarget = this;
        ctx.repaintTargetRect = QRect(0, rowY, width(), layoutItem.height);
        ctx.suppressAnimationScheduling = _scrolling;
        ctx.inlineVideo = _inlineVideo;
        if (_hoveredVoiceSeekEventId == _messages[i].eventId) {
            ctx.voiceSeekHoverProgress = _hoveredVoiceSeekProgress;
        }

        // Pass audio playback state for this message.
        const auto audioState = _audioPlayback.paintState();
        ctx.audioState = &audioState;

        // Pass text selection range for this message.
        int selStart = -1, selEnd = -1;
        if (selectionForMessage(i, selStart, selEnd)) {
            ctx.selectionStart = selStart;
            ctx.selectionEnd = selEnd;
        }

        // Dim a message the user has just deleted until the redaction lands (the
        // bubble flips to the "Deleted message" placeholder). The opacity is set
        // once here and inherited by every paint path; it is reverted by the
        // p.restore() below, which restores the saved opacity. `ctx.deleting`
        // additionally suppresses affordances the row will no longer honour.
        ctx.deleting = !_messages[i].delivery.deleted
            && _deletingEventIds.contains(_messages[i].eventId);
        if (ctx.deleting) {
            p.setOpacity(kDeletingOpacity);
        }
        HistoryMessage::paint(p, _messages[i], ctx);
        p.restore();

        // Pinned mode: per-row "go to message" jump button — a msgServiceBg
        // circle with the filled_go_to_message icon (msgServiceFg) centered.
        if (_pinnedMode) {
            const auto jumpRect = jumpButtonRect(i);
            if (!jumpRect.isEmpty()) {
                PainterHighQualityEnabler hq(p);
                p.setPen(Qt::NoPen);
                p.setBrush(st::msgServiceBg);
                p.drawEllipse(jumpRect);
                const auto icon = TeleMatrix::Style::IconProvider::tintedIcon(
                    QStringLiteral(":/telematrix/icons/chat/"),
                    QStringLiteral("filled_go_to_message"),
                    st::msgServiceFg);
                if (!icon.isNull()) {
                    // Sub-pixel-precise centering.
                    const auto iw = icon.width() / icon.devicePixelRatio();
                    const auto ih = icon.height() / icon.devicePixelRatio();
                    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
                    p.drawImage(
                        QPointF(
                            jumpRect.x() + (jumpRect.width() - iw) / 2.0,
                            jumpRect.y() + (jumpRect.height() - ih) / 2.0),
                        icon);
                }
            }
        }

        // Floating reply pill button — painted outside
        // the bubble with 120ms scale+opacity animation.
        if (i == _replyPillIndex && _replyPillOpacity > 0.01
            && !_pinnedMode
            && !ctx.deleting
            && !ctx.selectionMode
            && HistoryMessage::hasFastReplyAction(_messages[i], ctx)) {
            const auto pillLocalRect = HistoryMessage::fastReplyRect(_messages[i], ctx);
            // Store in widget coordinates for hover/click detection.
            _replyPillWidgetRect = pillLocalRect.translated(
                0, messageY + marginTop);
            p.save();
            p.translate(0, messageY + marginTop);
            p.setOpacity(_replyPillOpacity);
            // Scale from center of the pill.
            if (_replyPillOpacity < 0.99) {
                const auto cx = pillLocalRect.center().x();
                const auto cy = pillLocalRect.center().y();
                p.translate(cx, cy);
                p.scale(_replyPillOpacity, _replyPillOpacity);
                p.translate(-cx, -cy);
            }
            HistoryMessage::paintFastReplyButton(p, _messages[i], ctx);
            p.restore();
        }

        if (_messages[i].eventId == _highlightedEventId && _highlightOpacity > 0.0f) {
            auto overlay = st::msgSelectOverlay;
            overlay.setAlpha(qRound(_highlightOpacity * st::msgSelectOverlay.alpha()));
            if (overlay.alpha() > 0) {
                const QRect highlightRect(0, rowY, width(), layoutItem.height);
                p.fillRect(highlightRect, overlay);
            }
        }

        if (_selection.inSelectionMode() && selectable) {
            const auto dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
            const auto checkIcon = selectionCheckIcon(dpr);
            // Fixed X based on bubbleWidthLimit, not window width.
            const auto right = qMin(
                HistoryMessage::kMaxBubbleWidth
                    + HistoryMessage::kPhotoSkip
                    + kSelectionOffset
                    + HistoryMessage::kBubblePaddingH
                    + kSelectionDiameter,
                width());
            const auto checkLeft = right
                - (kSelectionOffset - kSelectionDiameter) / 2
                - HistoryMessage::kBubblePaddingH / 2
                - kSelectionDiameter
                - kScrollBarDelta;
            const auto checkTop = messageY
                + layoutItem.messageHeight
                - kSelectionDiameter
                - kSelectionBottomSkip;
            const QRect circle(
                checkLeft,
                checkTop,
                kSelectionDiameter,
                kSelectionDiameter);

            PainterHighQualityEnabler hq(p);
            if (selected) {
                p.setPen(QPen(st::windowBg, 2.0));
                p.setBrush(st::windowBgActive);
                p.drawEllipse(circle);
                if (!checkIcon.isNull()) {
                    p.drawImage(
                        QPoint(
                            circle.left() + kSelectionCheckIconLeft,
                            circle.top() + kSelectionCheckIconTop),
                        checkIcon);
                }
            } else {
                p.setPen(QPen(st::windowBg, 2.0));
                p.setBrush(kSelectionUncheckedBg);
                p.drawEllipse(circle);
            }
        }
    }

    if (kPaintStats) {
        paintStats.rowsUs += rowsPhaseTimer.nsecsElapsed() / 1000;
    }
    // Runs to the end of paintEvent; destroyed before PaintStatsScope (reverse
    // construction order), so the total is complete when the block is dumped.
    PaintPhase overlayPhase(paintStats.overlay());

    // --- Hover reaction button + in-place expanded column (overlay) ---
    // Painted after all bubbles so the (possibly tall) expanded column draws on
    // top. State lives here; geometry/paint live in HistoryMessage (no anim).
    if (_reactionPillIndex >= 0
        && _reactionPillIndex < _messages.size()
        && _reactionPillIndex < _layout.size()
        && !_pinnedMode
        && !isDeletingIndex(_reactionPillIndex)
        && !_selection.inSelectionMode()) {
        const auto i = _reactionPillIndex;
        const auto &li = _layout[i];
        const auto messageY = li.messageY + _contentOffset;
        const auto marginTop = li.sameSenderAbove
            ? HistoryMessage::kMarginTopAttached
            : HistoryMessage::kMarginTop;
        const auto ctx = reactionPaintCtx(i);
        if (HistoryMessage::hasReactionButton(_messages[i], ctx)) {
            p.save();
            p.translate(0, messageY + marginTop);
            if (_reactionExpanded && !_reactionColumnEmojis.isEmpty()) {
                const auto count = int(_reactionColumnEmojis.size());
                const auto colLocal = HistoryMessage::reactionColumnRect(
                    _messages[i], ctx, count, _reactionExpandUp);
                _reactionColumnWidgetRect = colLocal.translated(
                    0, messageY + marginTop);
                _reactionButtonWidgetRect = HistoryMessage::reactionButtonRect(
                    _messages[i], ctx).translated(0, messageY + marginTop);
                HistoryMessage::paintReactionColumn(
                    p, _messages[i], ctx, _reactionColumnEmojis,
                    _reactionScroll, _reactionHovered, _reactionExpandUp);
            } else {
                _reactionButtonWidgetRect = HistoryMessage::reactionButtonRect(
                    _messages[i], ctx).translated(0, messageY + marginTop);
                _reactionColumnWidgetRect = QRect();
                HistoryMessage::paintReactionButton(p, _messages[i], ctx);
            }
            p.restore();
        } else {
            _reactionButtonWidgetRect = QRect();
            _reactionColumnWidgetRect = QRect();
        }
    }

    // --- Sliding avatars (one per sender group) ---
    // One avatar per sender group: drawn on the group end when it is visible,
    // or clamped to the viewport bottom while the group continues below it.
    if (_isGroup || _showOutgoingPrivateAvatars) {
        const auto visibleBottom = _visibleTop
            + (parentWidget() ? parentWidget()->height() : height());
        const auto avatarSize = HistoryMessage::kPhotoSize;
        const auto avatarLeft = st::historyPhotoLeft;
        const auto minBottomSkip = HistoryMessage::kMarginBottom;
        const auto dpr = p.device()
            ? p.device()->devicePixelRatioF() : 1.0;

        for (int i = firstVisible; i < _messages.size(); ++i) {
            const auto &msg = _messages[i];
            const auto &li = _layout[i];
            const auto rowY = li.y + _contentOffset;
            const auto rowBottom = rowY + li.height;

            // Skip if completely above visible area.
            if (rowBottom < clip.top()) continue;
            // Stop if completely below visible area (but see group check below).
            if (rowY > clip.bottom()) break;

            const auto showAvatarForMessage = _isGroup
                || _showOutgoingPrivateAvatars;
            if (!showAvatarForMessage) continue;
            if (contentType(msg) == ContentType::Service) continue;

            // Paint avatar if this is the last message in its sender group,
            // or if this is the bottom-most visible message while the group
            // continues below the viewport.
            const bool isGroupEnd = !li.sameSenderBelow;
            const bool nextBelowViewport = li.sameSenderBelow
                && (i + 1 < _messages.size())
                && (_layout[i + 1].messageY + _contentOffset > visibleBottom);
            if (!isGroupEnd && !nextBelowViewport) continue;

            const auto groupFirst = qBound(0, li.senderGroupFirstIndex, _layout.size() - 1);
            const auto groupLast = qBound(0, li.senderGroupLastIndex, _layout.size() - 1);
            const auto groupTopY = li.senderGroupTopY + _contentOffset;
            const auto groupBottomY = li.senderGroupBottomY + _contentOffset;

            // Natural position is the group bottom, clamped to the visible
            // bottom until the group end becomes visible.
            const auto groupMarginTop = _layout[groupFirst].sameSenderAbove
                ? HistoryMessage::kMarginTopAttached
                : HistoryMessage::kMarginTop;
            auto avatarBottom = groupBottomY - minBottomSkip;
            avatarBottom = qMin(avatarBottom, visibleBottom - minBottomSkip);

            // Clamp: never above the group's first message.
            avatarBottom = qMax(
                avatarBottom,
                groupTopY + groupMarginTop + avatarSize);

            // Skip avatars outside the dirty region — a narrow scroll only
            // dirties the floating avatar's band, so this avoids re-decoding
            // every content-anchored avatar the blit already kept valid.
            const QRect avatarRect(
                avatarLeft,
                avatarBottom - avatarSize,
                avatarSize,
                avatarSize);
            if (!region.intersects(avatarRect)) {
                if (!isGroupEnd) {
                    i = qMax(i, groupLast);
                }
                continue;
            }

            // Avatars are painted in this separate pass, outside the per-message
            // opacity wrapper, so they need the deleting dim applied here too.
            const auto deleting = !msg.delivery.deleted
                && _deletingEventIds.contains(msg.eventId);
            if (deleting) {
                p.setOpacity(kDeletingOpacity);
            }
            HistoryMessage::paintSenderAvatar(
                p,
                msg,
                avatarLeft,
                avatarBottom - avatarSize,
                avatarSize,
                dpr,
                this,
                QRect(
                    avatarLeft - 2,
                    avatarBottom - avatarSize - 2,
                    avatarSize + 4,
                    avatarSize + 4));
            if (deleting) {
                p.setOpacity(1.0);
            }

            // Skip remaining messages in this group (avatar already drawn).
            if (!isGroupEnd) {
                i = qMax(i, groupLast);
            }
        }
    }

    if (_scrollDateOpacity > 0.0
        && stickyDateIndex >= 0
        && stickyDateIndex < _messages.size()) {
        const auto naturalDateY = _layout[stickyDateIndex].y + _contentOffset;
        if ((naturalDateY < visibleTop || stickyDateY < naturalDateY)
            && stickyDateY < clip.bottom()) {
            p.save();
            p.setOpacity(_scrollDateOpacity);
            paintDateSeparator(
                p,
                _messages[stickyDateIndex].timestamp,
                stickyDateY,
                width());
            p.restore();
        }
    }

    // A room we have not joined has no timeline to wait for — its name and description are the
    // whole content, so they take the place of the syncing pill rather than sitting under it.
    const auto showPreview = !_previewName.isEmpty() && _messages.isEmpty();
    if (showPreview) {
        PainterHighQualityEnabler hq(p);

        const auto nameFont = static_cast<const QFont &>(st::msgServiceFont);
        const auto &nameFm = st::fontMetrics(nameFont);
        const auto topicFont = st::normalFont;
        const auto &topicFm = st::fontMetrics(topicFont);

        const auto viewportH = parentWidget() ? parentWidget()->height() : height();
        const auto maxTextW = qMax(120, int(width() * 0.62));

        const auto namePillW = qMin(
            nameFm.horizontalAdvance(_previewName)
                + st::msgPadding.left() + st::msgPadding.right(),
            width() - 2 * st::msgPadding.left());
        const auto namePillH = nameFm.height()
            + st::msgServicePadding.top() + st::msgServicePadding.bottom();

        QRect topicRect;
        if (!_previewTopic.isEmpty()) {
            const auto bounds = topicFm.boundingRect(
                QRect(0, 0, maxTextW, viewportH),
                int(Qt::AlignHCenter | Qt::TextWordWrap),
                _previewTopic);
            topicRect = QRect(0, 0,
                bounds.width() + st::msgPadding.left() + st::msgPadding.right(),
                bounds.height() + st::msgServicePadding.top()
                    + st::msgServicePadding.bottom());
        }

        const auto gap = topicRect.isNull() ? 0 : 6;
        const auto blockH = namePillH + gap + topicRect.height();
        auto y = visibleTop + (viewportH - blockH) / 2;

        p.setPen(Qt::NoPen);
        p.setBrush(st::msgServiceBg);
        const auto namePillX = (width() - namePillW) / 2;
        const auto nameR = namePillH / 2.0;
        p.drawRoundedRect(namePillX, y, namePillW, namePillH, nameR, nameR);
        p.setFont(nameFont);
        p.setPen(st::msgServiceFg);
        TeleMatrix::EmojiText::DrawWrapped(
            p,
            QRect(namePillX, y, namePillW, namePillH),
            Qt::AlignCenter,
            TeleMatrix::EmojiText::Elide(
                _previewName,
                p.font(),
                TeleMatrix::EmojiText::CachedMetricsFor(
                    p.font(), st::emojiInlineSlot, st::emojiInlineGlyph),
                namePillW - st::msgPadding.left() - st::msgPadding.right()),
            TeleMatrix::EmojiText::CachedMetricsFor(
                p.font(), st::emojiInlineSlot, st::emojiInlineGlyph));

        if (!topicRect.isNull()) {
            y += namePillH + gap;
            const auto topicX = (width() - topicRect.width()) / 2;
            p.setPen(Qt::NoPen);
            p.setBrush(st::msgServiceBg);
            p.drawRoundedRect(
                topicX, y, topicRect.width(), topicRect.height(),
                st::msgServicePadding.top() + 4, st::msgServicePadding.top() + 4);
            p.setFont(topicFont);
            p.setPen(st::msgServiceFg);
            TeleMatrix::EmojiText::DrawWrapped(
                p,
                QRect(
                    topicX + st::msgPadding.left(),
                    y + st::msgServicePadding.top(),
                    topicRect.width() - st::msgPadding.left() - st::msgPadding.right(),
                    topicRect.height() - st::msgServicePadding.top()
                        - st::msgServicePadding.bottom()),
                Qt::AlignHCenter,
                _previewTopic,
                TeleMatrix::EmojiText::CachedMetricsFor(
                    topicFont, st::emojiInlineSlot, st::emojiInlineGlyph));
        }
    }

    // Paint status pill centered in the viewport while syncing or loading —
    // but ONLY when there are no cached messages to show. If the local event
    // cache already gave us messages, render them instantly (like the rooms
    // list) instead of covering them with a sync-gated "Waiting for network…"
    // overlay for the several seconds until the first sync completes.
    const auto showSyncingPill = !showPreview
        && (_syncing || _loadingTimelineVisible) && _messages.isEmpty();
    if (showSyncingPill) {
        PainterHighQualityEnabler hq(p);
        // A jump fetch with no cached slice yet (e.g. a cross-room link/search
        // result) shows this empty-timeline pill; keep its text "Loading..." so it
        // matches the centered overlay below and the same-room jump look.
        const auto text = _syncing
            ? tr("Waiting for network...")
            : tr("Loading...");
        const auto f = static_cast<const QFont &>(st::msgServiceFont);
        const auto &fm = st::fontMetrics(f);
        const auto pillW = fm.horizontalAdvance(text)
            + st::msgPadding.left() + st::msgPadding.right();
        const auto pillH = fm.height()
            + st::msgServicePadding.top() + st::msgServicePadding.bottom();
        const auto pillR = pillH / 2.0;
        const auto pillX = (width() - pillW) / 2;
        // Center vertically when timeline is empty; near top otherwise.
        const auto viewportH = parentWidget() ? parentWidget()->height() : height();
        const auto pillY = _messages.isEmpty()
            ? visibleTop + (viewportH - pillH) / 2
            : visibleTop + 12;
        p.setPen(Qt::NoPen);
        p.setBrush(st::msgServiceBg);
        p.drawRoundedRect(pillX, pillY, pillW, pillH, pillR, pillR);
        p.setFont(f);
        p.setPen(st::msgServiceFg);
        p.drawText(
            pillX + st::msgPadding.left(),
            pillY + st::msgServicePadding.top() + fm.ascent(),
            text);
    }

    // (The former in-place "Loading…" overlay is gone: jumps now use the opaque
    // jump-loading cover painted at the top of paintEvent.)

}

void HistoryList::paintDateSeparator(
    QPainter &p, qint64 timestamp, int y, int width)
{
    const auto date = QDateTime::fromSecsSinceEpoch(timestamp).date();
    const auto today = QDate::currentDate();

    QString text;
    if (date == today) {
        text = tr("Today");
    } else if (date.addDays(1) == today) {
        text = tr("Yesterday");
    } else {
        text = QLocale().toString(date, u"MMMM d"_s);
    }

    const auto dateTextWidth = st::msgServiceFont->width(text);
    const auto maxWidth = width;
    auto availableWidth = maxWidth
        - st::msgServiceMargin.left()
        - st::msgServiceMargin.right();
    if (availableWidth < 1) {
        availableWidth = 1;
    }
    auto left = st::msgServiceMargin.left();
    left += (availableWidth
        - dateTextWidth
        - st::msgServicePadding.left()
        - st::msgServicePadding.right()) / 2;

    const auto bubbleTop = y + st::msgServiceMargin.top();
    const auto bubbleWidth = dateTextWidth
        + st::msgServicePadding.left()
        + st::msgServicePadding.right();
    const auto bubbleHeight = dateBadgeInnerHeight();

    // Rounded background (service-bubble style).
    {
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(st::msgServiceBg);
        p.drawRoundedRect(
            left,
            bubbleTop,
            bubbleWidth,
            bubbleHeight,
            st::bubbleRadiusSmall,
            st::bubbleRadiusSmall);
    }

    // Text baseline from the service date painter.
    p.setFont(st::msgServiceFont);
    p.setPen(st::msgServiceFg);
    p.drawText(
        left + st::msgServicePadding.left(),
        bubbleTop + st::msgServicePadding.top() + st::msgServiceFont->ascent,
        text);
}

void HistoryList::toggleScrollDateShown(bool shown) {
    const auto target = shown ? 1.0 : 0.0;
    if (_scrollDateShown == shown
        && qAbs(_scrollDateOpacity - target) < 0.001) {
        return;
    }
    _scrollDateShown = shown;
    _scrollDateOpacityAnimation.stop();
    _scrollDateOpacityAnimation.setStartValue(_scrollDateOpacity);
    _scrollDateOpacityAnimation.setEndValue(target);
    _scrollDateOpacityAnimation.start();
}

void HistoryList::touchScrollDate(qint64 visibleTopTimestamp) {
    if (visibleTopTimestamp <= 0) {
        return;
    }
    if (_scrollDateTopTimestamp != visibleTopTimestamp || !_scrollDateShown) {
        _scrollDateTopTimestamp = visibleTopTimestamp;
        toggleScrollDateShown(true);
    }
    _scrollDateHideTimer.start();
}

int HistoryList::stickyDateIndexAtVisibleTop(int visibleTop) const {
    if (_dateIndices.isEmpty()) return -1;
    // Binary search: find last date index whose dateY <= visibleTop.
    int lo = 0, hi = _dateIndices.size() - 1;
    int result = -1;
    while (lo <= hi) {
        const auto mid = lo + (hi - lo) / 2;
        const auto idx = _dateIndices[mid];
        const auto dateY = _layout[idx].y + _contentOffset;
        if (dateY <= visibleTop) {
            result = idx;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return result;
}

void HistoryList::updateVisibleTop(int visibleTop) {
    const auto prevTop = _visibleTop;
    _visibleTop = qMax(0, visibleTop);
    if (_visibleTop != prevTop) {
        // Mark actively-scrolling so paint suppresses per-row animation repaints;
        // the settle timer clears it shortly after motion stops.
        _scrolling = true;
        _scrollSettleTimer.start();

        // The scroll-area blits the widget backing store (widget coords) and Qt
        // auto-invalidates only the newly-exposed strip. Every bubble and every
        // content-anchored avatar moves with that blit and stays valid; the ONLY
        // element painted at a viewport-relative position is the sender avatar
        // that floats at the viewport bottom because its group continues below
        // the fold. Invalidate just the avatar-column band spanning that avatar's
        // old and new positions (erase old, draw new) instead of the whole column
        // — this lets paintEvent's region culling skip every middle row.
        if (_isGroup || _showOutgoingPrivateAvatars) {
            const auto viewportH = parentWidget()
                ? parentWidget()->height() : height();
            const auto avatarColumnRight = st::historyPhotoLeft
                + HistoryMessage::kPhotoSize
                + 4;
            const auto avatarSize = HistoryMessage::kPhotoSize;
            const auto minBottomSkip = HistoryMessage::kMarginBottom;
            const auto prevBottom = prevTop + viewportH;
            const auto newBottom = _visibleTop + viewportH;
            const auto bandTop = qMin(prevBottom, newBottom)
                - minBottomSkip - avatarSize;
            const auto bandHeight = qAbs(_visibleTop - prevTop) + avatarSize;
            update(QRect(0, bandTop, avatarColumnRight, bandHeight));
        }
        // Scrolled: messages may have passed the viewport bottom — detect reads.
        checkReadProgress();

        // Pause the active inline video once it scrolls a full viewport past the
        // fold (and resume on return), so offscreen decoding stops. Never
        // overrides an explicit user pause — see pauseForViewport.
        if (_inlineVideo && !_inlineVideo->activeEventId().isEmpty()) {
            const auto viewportH = parentWidget()
                ? parentWidget()->height() : height();
            const auto i = _messageIndex.physicalIndexOf(
                _inlineVideo->activeEventId());
            const auto visible = (i >= 0) && (i < _layout.size())
                && QRect(0, _layout[i].y + _contentOffset,
                        width(), _layout[i].height)
                    .intersects(QRect(0, _visibleTop - viewportH,
                        width(), 3 * viewportH));
            _inlineVideo->pauseForViewport(visible);
        }
    }

    // Invalidate hover state on scroll — the message under the cursor
    // changes when the viewport scrolls, making _hoveredIndex stale.
    // Don't call update() here to avoid feedback loops during layout.
    if (_visibleTop != prevTop && _hoveredIndex >= 0) {
        _hoveredIndex = -1;
        _hoveredLinkStart = -1;
        _hoveredLinkUrl.clear();
        _hoveredFastReply = false;
        _hoveredCopyButton = false;
        _hoveredAvatar = false;
        _hoveredVoiceSeekEventId.clear();
        _hoveredVoiceSeekProgress = -1.0;
        _overLinkPreview = false;
        // Start reply pill hide animation on scroll.
        if (_replyPillIndex >= 0 && !_replyPillHideTimer.isActive()) {
            _replyPillHideTimer.start();
        }
        // Cancel a pending reaction show/expand — the hovered message scrolls out
        // from under the cursor, so committing later would target the wrong row.
        _reactionShowTimer.stop();
        _reactionExpandTimer.stop();
        _reactionPendingIndex = -1;
        _reactionExpandPendingIndex = -1;
        // Collapse the reaction affordance on scroll. Its button/column hang
        // outside the row rect, and the narrow scroll invalidation above no
        // longer repaints the middle (region culling), so erase their painted
        // extent explicitly before clearing the cached rects — otherwise the
        // button ghosts at its old widget-Y (update() is async, no feedback loop).
        if (_reactionPillIndex >= 0) {
            update(affordanceDirtyRegion());
            _reactionPillIndex = -1;
            _reactionExpanded = false;
            _reactionHovered = -1;
            _reactionScroll = 0;
            _reactionButtonWidgetRect = QRect();
            _reactionColumnWidgetRect = QRect();
        }
        setCursor(Qt::ArrowCursor);
    }

    if (_messages.isEmpty() || _layout.isEmpty()) {
        _scrollDateHideTimer.stop();
        toggleScrollDateShown(false);
        _lastScrollVisibleTop = -1;
        _lastScrollDateIndex = -1;
        return;
    }

    const auto stickyDateIndex = stickyDateIndexAtVisibleTop(_visibleTop);
    if (stickyDateIndex < 0 || stickyDateIndex >= _messages.size()) {
        _scrollDateHideTimer.stop();
        toggleScrollDateShown(false);
        _lastScrollVisibleTop = _visibleTop;
        _lastScrollDateIndex = -1;
        return;
    }

    if (_lastScrollVisibleTop < 0) {
        _scrollDateTopTimestamp = _messages[stickyDateIndex].timestamp;
        _lastScrollVisibleTop = _visibleTop;
        _lastScrollDateIndex = stickyDateIndex;
        update();
        return;
    }

    const auto scrolledUp = (_visibleTop < _lastScrollVisibleTop);
    if (_visibleTop != _lastScrollVisibleTop
        || stickyDateIndex != _lastScrollDateIndex) {
        _scrollDateTopTimestamp = _messages[stickyDateIndex].timestamp;
        if (scrolledUp) {
            // Display scroll date only after initial visible area is known.
            if (_lastScrollDateIndex >= 0) {
                touchScrollDate(_scrollDateTopTimestamp);
            }
        } else {
            _scrollDateHideTimer.stop();
            toggleScrollDateShown(false);
        }
    }

    _lastScrollVisibleTop = _visibleTop;
    _lastScrollDateIndex = stickyDateIndex;
    // Repaint only the sticky date badge region — the scroll area
    // already handles repainting newly-exposed message regions.
    const auto dateH = dateBadgeTotalHeight();
    update(QRect(0, _visibleTop, width(), dateH + 4));
}

int HistoryList::messageContextWidth() const {
    if (!_pinnedMode) {
        return width();
    }
    // Reserve the jump-button gutter on the right. Clamp so the bubble area
    // never collapses on a very narrow widget.
    return qMax(HistoryMessage::kMinBubbleWidth, width() - kJumpButtonReserve);
}

QRect HistoryList::jumpButtonRect(int index) const {
    if (!_pinnedMode
        || index < 0
        || index >= _messages.size()
        || index >= _layout.size()) {
        return QRect();
    }
    const auto &msg = _messages[index];
    if (msg.eventId.isEmpty()
        || contentType(msg) == ContentType::Service) {
        return QRect();
    }
    const auto &layoutItem = _layout[index];
    // Lay the bubble out with the same context the paint loop uses, then anchor
    // the button to the bubble's bottom-right corner:
    // left = bubbleRight + buttonLeft, top = bubbleBottom - buttonBottom - size.
    MessagePaintContext ctx;
    ctx.width = messageContextWidth();
    ctx.isGroup = _isGroup;
    ctx.showOutgoingPrivateAvatar = _showOutgoingPrivateAvatars;
    ctx.sameSenderAbove = layoutItem.sameSenderAbove;
    ctx.sameSenderBelow = layoutItem.sameSenderBelow;
    ctx.timelineIndex = &_timelineLookup;
    const auto bubble = HistoryMessage::bubbleRect(msg, ctx);
    if (!bubble.isValid()) {
        return QRect();
    }
    const auto marginTop = layoutItem.sameSenderAbove
        ? HistoryMessage::kMarginTopAttached
        : HistoryMessage::kMarginTop;
    const auto originY = layoutItem.messageY + _contentOffset + marginTop;
    const auto skip = qBound(
        0,
        (bubble.height() - kJumpButtonDiameter) / 2,
        kJumpButtonBottom);
    const auto left = bubble.left() + bubble.width() + kJumpButtonLeft;
    const auto top = originY + bubble.top() + bubble.height()
        - skip - kJumpButtonDiameter;
    return QRect(left, top, kJumpButtonDiameter, kJumpButtonDiameter);
}

int HistoryList::messageIndexAt(int y) const {
    if (_layout.isEmpty()) return -1;
    // Binary search: find the last item whose adjusted Y <= y.
    int lo = 0, hi = _layout.size() - 1;
    while (lo < hi) {
        const auto mid = lo + (hi - lo + 1) / 2;
        if (_layout[mid].y + _contentOffset <= y) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    const auto &item = _layout[lo];
    const auto rowY = item.y + _contentOffset;
    const auto messageY = item.messageY + _contentOffset;
    if (y >= rowY
        && y < rowY + item.height
        && y >= messageY
        && y < messageY + item.messageHeight) {
        return lo;
    }
    return -1;
}

void HistoryList::updateSendStateAnimationTimer() {
    if (_sendingCount > 0) {
        if (!_sendStateTimer.isActive()) {
            _sendStateTimer.start();
        }
    } else {
        _sendStateTimer.stop();
        _sendStateTick = 0;
    }
}

void HistoryList::mouseMoveEvent(QMouseEvent *e) {
    // Active inline-video scrub: live-seek and swallow the move so it doesn't
    // drive hover/selection.
    if (_videoSeekDragging) {
        const auto bar = activeVideoSeekBarRect();
        if (!bar.isNull()) {
            seekActiveVideoToX(e->pos().x(), bar);
        }
        setCursor(Qt::PointingHandCursor);
        return;
    }

    auto idx = messageIndexAt(e->pos().y());
    // A message whose redaction is in flight is inert: treat its row as empty so
    // no hover affordance, link or cursor change ever targets it, and so any
    // affordance still showing there is retracted by the logic below.
    if (isDeletingIndex(idx)) {
        idx = -1;
    }
    // Any hover-state change touches at most the row the cursor left, the row it
    // entered, and the floating reply/reaction affordances — all row-bounded.
    // Capture the outgoing hovered row so the final repaint can be row-scoped.
    const auto prevHovered = _hoveredIndex;
    bool needsUpdate = false;

    // Pinned mode: the go-to-message button uses a pointing-hand cursor.
    if (_pinnedMode && idx >= 0 && jumpButtonRect(idx).contains(e->pos())) {
        if (_hoveredIndex != idx) {
            _hoveredIndex = idx;
            update();
        }
        setCursor(Qt::PointingHandCursor);
        return;
    }

    if (_selection.inSelectionMode()) {
        if (idx != _hoveredIndex) {
            _hoveredIndex = idx;
            needsUpdate = true;
        }
        if (_hoveredTimestamp) {
            _hoveredTimestamp = false;
            _tooltipTimer.stop();
            if (_timestampTooltip) {
                _timestampTooltip->hideAnimated();
            }
        }
        _hoveredLinkStart = -1;
        _hoveredLinkUrl.clear();
        _hoveredFastReply = false;
        _hoveredCopyButton = false;
        if (!_hoveredVoiceSeekEventId.isEmpty()
            || _hoveredVoiceSeekProgress >= 0.0) {
            _hoveredVoiceSeekEventId.clear();
            _hoveredVoiceSeekProgress = -1.0;
            needsUpdate = true;
        }

        if (idx >= 0 && idx < _messages.size()
            && contentType(_messages[idx]) != ContentType::Service
            && !_messages[idx].eventId.isEmpty()
            && !_messages[idx].delivery.deleted) {
            setCursor(Qt::PointingHandCursor);
        } else {
            setCursor(Qt::ArrowCursor);
        }
        if (needsUpdate) {
            update();
        }
        return;
    }

    if (idx != _hoveredIndex) {
        _hoveredIndex = idx;
        _hoveredLinkStart = -1;
        _hoveredLinkUrl.clear();
        _hoveredFastReply = false;
        _hoveredCopyButton = false;
        _hoveredAvatar = false;
        _hoveredVoiceSeekEventId.clear();
        _hoveredVoiceSeekProgress = -1.0;
        _overLinkPreview = false;
        setCursor(Qt::ArrowCursor);
        needsUpdate = true;

        // Reply pill: trigger hide when leaving an eligible message,
        // but NOT if the cursor is still on the painted pill rect
        // (pill can extend into the previous message's layout area).
        if (_replyPillIndex >= 0 && idx != _replyPillIndex) {
            const auto stillOnPill = !_replyPillWidgetRect.isEmpty()
                && _replyPillWidgetRect.contains(
                    mapFromGlobal(QCursor::pos()));
            if (!stillOnPill && !_replyPillHideTimer.isActive()) {
                _replyPillHideTimer.start();
            }
        }
    }

    QPoint localPos;
    MessagePaintContext localCtx;
    auto localAudioState = _audioPlayback.paintState();
    auto haveLocalContext = false;
    auto overReplyPreview = false;
    auto overMediaBubble = false;
    auto overFileBubble = false;
    auto overVoiceSeek = false;

    // Hit-test links within the hovered message.
    if (idx >= 0 && idx < _messages.size()) {
        const auto &layoutItem = _layout[idx];
        const auto messageY = layoutItem.messageY + _contentOffset;
        const auto marginTop = layoutItem.sameSenderAbove
            ? HistoryMessage::kMarginTopAttached
            : HistoryMessage::kMarginTop;

        // Convert mouse pos to message-local coordinates (matching paint translate).
        localPos = QPoint(e->pos().x(), e->pos().y() - messageY - marginTop);

        localCtx.width = width();
        localCtx.isGroup = _isGroup;
        localCtx.showOutgoingPrivateAvatar = _showOutgoingPrivateAvatars;
        localCtx.sameSenderAbove = layoutItem.sameSenderAbove;
        localCtx.sameSenderBelow = layoutItem.sameSenderBelow;
        localCtx.isHovered = true;
        localCtx.hoveredFastReply = _hoveredFastReply;
        localCtx.selectionMode = _selection.inSelectionMode();
        localCtx.timelineIndex = &_timelineLookup;
        localCtx.largeEmojiEnabled = _largeEmojiEnabled;
        localCtx.audioState = &localAudioState;
        haveLocalContext = true;

        int linkStart = -1;
        const auto url = HistoryMessage::linkAt(
            _messages[idx], localCtx, localPos, linkStart);

        if (linkStart != _hoveredLinkStart) {
            _hoveredLinkStart = linkStart;
            _hoveredLinkUrl = url;
            needsUpdate = true;
        }

        // Hit-test code block copy buttons (only if not over a link).
        bool overCopy = false;
        if (_hoveredLinkStart < 0) {
            const auto codeText = HistoryMessage::codeBlockCopyAt(
                _messages[idx], localCtx, localPos);
            overCopy = !codeText.isEmpty();
        }
        if (overCopy != _hoveredCopyButton) {
            _hoveredCopyButton = overCopy;
            needsUpdate = true;
        }

        const auto overFastReply = (_hoveredLinkStart < 0 && !_hoveredCopyButton)
            && (HistoryMessage::fastReplyAt(_messages[idx], localCtx, localPos)
                || (_replyPillIndex >= 0
                    && !_replyPillWidgetRect.isEmpty()
                    && _replyPillWidgetRect.contains(e->pos())));
        if (overFastReply != _hoveredFastReply) {
            _hoveredFastReply = overFastReply;
            localCtx.hoveredFastReply = overFastReply;
            needsUpdate = true;
        }

        overReplyPreview = (_hoveredLinkStart < 0 && !_hoveredCopyButton && !_hoveredFastReply)
            && !HistoryMessage::replyTargetAt(_messages[idx], localCtx, localPos).isEmpty();

        overMediaBubble = isMediaBubbleHit(_messages[idx], localCtx, localPos);

        overFileBubble = (HistoryMessage::isAudioBubble(_messages[idx])
                || isFileMessage(_messages[idx]))
            && !mediaUrl(_messages[idx]).isEmpty()
            && HistoryMessage::bubbleShapePath(_messages[idx], localCtx).contains(QPointF(localPos));

        QString nextVoiceSeekEventId;
        auto nextVoiceSeekProgress = -1.0;
        if (_hoveredLinkStart < 0
            && !_hoveredCopyButton
            && !_hoveredFastReply
            && isAudioMessage(_messages[idx])
            && isVoiceMessage(_messages[idx])
            && !_audioPlayback.eventId().isEmpty()
            && _messages[idx].eventId == _audioPlayback.eventId()) {
            const auto progress = HistoryViewAudio::waveformSeekAt(
                _messages[idx],
                localCtx,
                localPos,
                0,
                0,
                localCtx.width);
            if (progress >= 0.0) {
                nextVoiceSeekEventId = _messages[idx].eventId;
                nextVoiceSeekProgress = progress;
                overVoiceSeek = true;
            }
        }
        const auto voiceSeekChanged =
            nextVoiceSeekEventId != _hoveredVoiceSeekEventId
            || ((_hoveredVoiceSeekProgress < 0.0) != (nextVoiceSeekProgress < 0.0))
            || (nextVoiceSeekProgress >= 0.0
                && qAbs(_hoveredVoiceSeekProgress - nextVoiceSeekProgress) > 0.001);
        if (voiceSeekChanged) {
            _hoveredVoiceSeekEventId = nextVoiceSeekEventId;
            _hoveredVoiceSeekProgress = nextVoiceSeekProgress;
            needsUpdate = true;
        }

        _overLinkPreview = (_hoveredLinkStart < 0 && !_hoveredCopyButton && !_hoveredFastReply)
            && !HistoryMessage::linkPreviewUrlAt(_messages[idx], localCtx, localPos).isEmpty();

        // Hit-test sender avatar.
        const bool overAvatar = (_hoveredLinkStart < 0 && !_hoveredCopyButton && !_hoveredFastReply)
            && HistoryMessage::senderAvatarAt(_messages[idx], localCtx, localPos);
        if (overAvatar != _hoveredAvatar) {
            _hoveredAvatar = overAvatar;
            needsUpdate = true;
        }

        // Hit-test timestamp area for tooltip.
        const bool overTimestamp = HistoryMessage::timestampAt(
            _messages[idx], localCtx, localPos);
        if (overTimestamp) {
            _hoveredTimestamp = true;
            const auto dt = QDateTime::fromSecsSinceEpoch(_messages[idx].timestamp);
            _tooltipText = QLocale().toString(dt, QLocale::LongFormat);
            _tooltipGlobalPos = e->globalPosition().toPoint();

            // Tooltip delay is reset while the cursor moves.
            _tooltipTimer.start();

            if (_timestampTooltip
                && _timestampTooltip->isVisible()
                && (_tooltipGlobalPos - _tooltipShownAt).manhattanLength()
                    > QApplication::startDragDistance()) {
                _timestampTooltip->hideAnimated();
            }
        } else if (_hoveredTimestamp) {
            _hoveredTimestamp = false;
            _tooltipTimer.stop();
            if (_timestampTooltip) {
                _timestampTooltip->hideAnimated();
            }
        }
    } else if (_hoveredTimestamp) {
        _hoveredTimestamp = false;
        _tooltipTimer.stop();
        if (_timestampTooltip) {
            _timestampTooltip->hideAnimated();
        }
    }
    if (idx < 0 && _hoveredFastReply) {
        _hoveredFastReply = false;
        needsUpdate = true;
    }
    if (idx < 0 && _hoveredAvatar) {
        _hoveredAvatar = false;
        needsUpdate = true;
    }

    // Reply pill: show when mouse is inside the bubble, keep visible
    // when on the pill itself. Use the widget-coordinate rect stored
    // during the last paint for pixel-perfect pill detection.
    {
        const auto onPaintedPill = (_replyPillIndex >= 0)
            && !_replyPillWidgetRect.isEmpty()
            && _replyPillWidgetRect.contains(e->pos());

        if (_replyButtonEnabled && !_activeMenu
            && haveLocalContext && !readOnlyView() && idx >= 0 && idx < _messages.size()
            && !_messages[idx].delivery.outgoing
            && !isServiceMessage(_messages[idx])
            && !_messages[idx].eventId.isEmpty()
            && !_messages[idx].delivery.deleted) {
            const auto insideBubble =
                HistoryMessage::bubbleShapePath(_messages[idx], localCtx)
                    .contains(QPointF(localPos));
            if (insideBubble || onPaintedPill) {
                if (_replyPillIndex != idx && !onPaintedPill) {
                    _replyPillIndex = idx;
                    _replyPillHideTimer.stop();
                    _replyPillAnim->stop();
                    _replyPillAnim->setStartValue(_replyPillOpacity);
                    _replyPillAnim->setEndValue(1.0);
                    _replyPillAnim->start();
                } else if (_replyPillHideTimer.isActive()) {
                    // Mouse re-entered bubble/pill — cancel pending hide.
                    _replyPillHideTimer.stop();
                    if (_replyPillOpacity < 0.99) {
                        _replyPillAnim->stop();
                        _replyPillAnim->setStartValue(_replyPillOpacity);
                        _replyPillAnim->setEndValue(1.0);
                        _replyPillAnim->start();
                    }
                }
            } else if (_replyPillIndex == idx) {
                if (!_replyPillHideTimer.isActive()) {
                    _replyPillHideTimer.start();
                }
            }
        } else if (onPaintedPill) {
            // Cursor is on the pill but idx doesn't match the pill's message
            // (pill extends above the message row). Cancel any pending hide.
            if (_replyPillHideTimer.isActive()) {
                _replyPillHideTimer.stop();
                if (_replyPillOpacity < 0.99) {
                    _replyPillAnim->stop();
                    _replyPillAnim->setStartValue(_replyPillOpacity);
                    _replyPillAnim->setEndValue(1.0);
                    _replyPillAnim->start();
                }
            }
        }
    }

    // Handle text selection drag.
    if (_selection.mousePressed() && e->buttons() & Qt::LeftButton) {
        const auto delta = e->pos() - _selection.mousePressPos();
        if (!_selection.dragStarted() && delta.manhattanLength() > 3) {
            _selection.setDragStarted(true);
        }
        if (_selection.dragStarted()) {
            _selection.setSelectionEnd(cursorFromPoint(e->pos()));
            needsUpdate = true;
        }
    }

    // Hover reaction button + in-place column (bottom-corner button, no anim).
    // The resting button shows while the cursor is in the bubble; moving onto it
    // expands the vertical emoji column in place; moving onto the column keeps it
    // open and tracks the hovered cell.
    bool overReactionAffordance = false;
    // _activeMenu: our popup menus are plain widgets (no input grab), so mouse
    // moves still reach the list while one is open — don't re-arm pills then.
    // Saved Messages has no reaction affordances at all.
    if (_reactionButtonEnabled && !_activeMenu && !_savedMessagesMode
        && !readOnlyView() && !_selection.inSelectionMode()) {
        const auto overButton = !_reactionButtonWidgetRect.isEmpty()
            && _reactionButtonWidgetRect.contains(e->pos());
        const auto overColumn = !_reactionColumnWidgetRect.isEmpty()
            && _reactionColumnWidgetRect.contains(e->pos());
        const auto eligible = haveLocalContext
            && idx >= 0 && idx < _messages.size()
            && contentType(_messages[idx]) != ContentType::Service
            && !_messages[idx].eventId.isEmpty()
            && !_messages[idx].delivery.deleted
            && !_messages[idx].delivery.outgoing; // no reaction pill on our own messages
        const auto insideBubble = eligible
            && HistoryMessage::bubbleShapePath(_messages[idx], localCtx)
                .contains(QPointF(localPos));

        if (_reactionExpanded && overColumn) {
            _reactionHideTimer.stop();
            _reactionExpandTimer.stop();
            _reactionExpandPendingIndex = -1;
            const auto hit = reactionColumnCellHit(e->pos());
            if (hit != _reactionHovered) {
                _reactionHovered = hit;
                needsUpdate = true;
            }
            overReactionAffordance = (hit >= 0);
        } else if (overButton) {
            _reactionHideTimer.stop();
            _reactionShowTimer.stop();
            _reactionPendingIndex = -1;
            // Task 2: expand after resting on the pill briefly, not the instant
            // the cursor arrives (expand delay). The click-to-expand
            // path stays immediate.
            if (!_reactionExpanded
                && _reactionExpandPendingIndex != _reactionPillIndex) {
                _reactionExpandPendingIndex = _reactionPillIndex;
                _reactionExpandTimer.start();
            }
            overReactionAffordance = true;
        } else if (insideBubble) {
            _reactionHideTimer.stop();
            // In the bubble but not on the pill: cancel a pending expand.
            _reactionExpandTimer.stop();
            _reactionExpandPendingIndex = -1;
            if (_reactionPillIndex == idx) {
                // Already resting on this message; collapse back to the pill if it
                // was expanded (cursor moved off the button into the bubble).
                if (_reactionExpanded) {
                    _reactionExpanded = false;
                    _reactionHovered = -1;
                    needsUpdate = true;
                }
            } else {
                // Task 1: appear after kButtonShowDelay — a little after the reply
                // pill — instead of the instant the cursor enters the bubble.
                if (_reactionPendingIndex != idx) {
                    _reactionPendingIndex = idx;
                    _reactionShowTimer.start();
                }
                // Drop a pill already shown for a different message right away.
                if (_reactionPillIndex >= 0) {
                    _reactionPillIndex = -1;
                    _reactionExpanded = false;
                    _reactionHovered = -1;
                    needsUpdate = true;
                }
            }
        } else if (_reactionPillIndex >= 0 || _reactionPendingIndex >= 0
                   || _reactionShowTimer.isActive()) {
            // Cursor left the bubble/pill/column: cancel a pending show/expand and
            // debounce-hide any visible pill.
            _reactionShowTimer.stop();
            _reactionExpandTimer.stop();
            _reactionPendingIndex = -1;
            _reactionExpandPendingIndex = -1;
            if (_reactionPillIndex >= 0 && !_reactionHideTimer.isActive()) {
                _reactionHideTimer.start();
            }
        }
    }

    // Update cursor shape.
    // UTD messages are clickable when session is not yet verified
    // → opens session verification dialog.
    const bool overUtd = (idx >= 0 && idx < _messages.size()
            && isUnableToDecryptMessage(_messages[idx]))
        && haveLocalContext
        && HistoryMessage::isInsideUtdVerifyLink(_messages[idx], localCtx, localPos);
    if (_hoveredLinkStart >= 0
        || _hoveredCopyButton
        || _hoveredFastReply
        || _hoveredAvatar
        || overReplyPreview
        || overMediaBubble
        || overFileBubble
        || overVoiceSeek
        || _overLinkPreview
        || overUtd
        || overReactionAffordance) {
        setCursor(Qt::PointingHandCursor);
    } else if (idx >= 0 && idx < _messages.size() && !_selection.dragStarted()) {
        if (haveLocalContext
            && !HistoryMessage::reactionPillAt(_messages[idx], localCtx, localPos).isEmpty()) {
            setCursor(Qt::PointingHandCursor);
        } else if (haveLocalContext
            && pollContent(_messages[idx])) {
            const auto geometry = pollContentGeometry(_messages[idx], localCtx);
            const auto optionId = geometry.valid
                ? HistoryViewPoll::optionAt(
                    _messages[idx],
                    localCtx,
                    geometry.left,
                    geometry.top,
                    geometry.width,
                    localPos)
                : QString();
            const auto overVoteButton = geometry.valid
                && HistoryViewPoll::voteButtonAt(
                    _messages[idx],
                    localCtx,
                    geometry.left,
                    geometry.top,
                    geometry.width,
                    localPos);
            if (!optionId.isEmpty() || overVoteButton) {
                setCursor(Qt::PointingHandCursor);
            } else {
                setCursor(Qt::ArrowCursor);
            }
        } else if (haveLocalContext && HistoryMessage::isOverText(_messages[idx], localCtx, localPos)) {
            setCursor(Qt::IBeamCursor);
        } else {
            setCursor(Qt::ArrowCursor);
        }
    } else if (_selection.dragStarted()) {
        setCursor(Qt::IBeamCursor);
    } else {
        setCursor(Qt::ArrowCursor);
    }

    if (needsUpdate) {
        // The expanded reaction column can extend well beyond a single row, so
        // fall back to a full repaint only in that (rare) case; otherwise repaint
        // just the affected rows.
        if (_reactionExpanded) {
            update();
        } else {
            // Erase the affordances last painted (their overhang may sit on a
            // row no longer in the set below, e.g. the reaction pill just moved).
            QRegion dirty = affordanceDirtyRegion();
            // Draw the newly-hovered affordances unclipped: inflate the row rect
            // so the reply pill (above) / reaction button (below) aren't cut off.
            for (const int i : {prevHovered, _hoveredIndex,
                    _replyPillIndex, _reactionPillIndex}) {
                if (i >= 0 && i < _layout.size()) {
                    dirty += affordanceRowRect(i);
                }
            }
            if (dirty.isEmpty()) {
                update();
            } else {
                update(dirty);
            }
        }
    }
}

void HistoryList::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton || isContextMenuMouseGesture(e)) {
        Ui::RpWidget::mousePressEvent(e);
        return;
    }

    // Pinned mode: the "go to message" jump button is hit-tested first and
    // consumes the press so it never starts a text selection or other action.
    _pressedJumpButtonIndex = -1;
    if (_pinnedMode) {
        const auto idx = messageIndexAt(e->pos().y());
        if (idx >= 0 && jumpButtonRect(idx).contains(e->pos())) {
            _pressedJumpButtonIndex = idx;
            setFocus();
            e->accept();
            return;
        }
    }

    refreshHoverFromPosition(e->pos(), e->buttons());

    // See mouseReleaseEvent: a message being deleted takes no input at all, so it
    // must not start a scrub, a text selection or a copy-button press either.
    if (isDeletingIndex(messageIndexAt(e->pos().y()))) {
        setFocus();
        _selection.resetDrag();
        e->accept();
        return;
    }

    if (_selection.inSelectionMode()) {
        setFocus();
        _selection.resetDrag();
        return;
    }

    // Inline-video seek bar: a press starts a scrub and seeks to the press point;
    // moves scrub live, release ends it. Takes priority so it never starts a text
    // selection or a play/pause toggle. Grab area is widened above/below the strip.
    if (_inlineVideo && !_videoSeekDragging) {
        const auto bar = activeVideoSeekBarRect();
        if (!bar.isNull()) {
            const auto pad = bar.height() / 2;
            if (bar.adjusted(0, -pad, 0, pad).contains(e->pos())) {
                _videoSeekDragging = true;
                seekActiveVideoToX(e->pos().x(), bar);
                setFocus();
                _selection.resetDrag();
                setCursor(Qt::PointingHandCursor);
                e->accept();
                update();
                return;
            }
        }
    }

    // Handle code block copy button clicks immediately.
    if (_hoveredCopyButton) {
        _selection.setDragStarted(false);
        const auto idx = _hoveredIndex;
        if (idx >= 0 && idx < _messages.size()) {
            const auto &layoutItem = _layout[idx];
            const auto messageY = layoutItem.messageY + _contentOffset;
            const auto marginTop = layoutItem.sameSenderAbove
                ? HistoryMessage::kMarginTopAttached
                : HistoryMessage::kMarginTop;
            const QPoint localPos(e->pos().x(), e->pos().y() - messageY - marginTop);

            MessagePaintContext ctx;
            ctx.width = messageContextWidth();
            ctx.isGroup = _isGroup;
            ctx.showOutgoingPrivateAvatar = _showOutgoingPrivateAvatars;
            ctx.sameSenderAbove = layoutItem.sameSenderAbove;
            ctx.timelineIndex = &_timelineLookup;
            ctx.largeEmojiEnabled = _largeEmojiEnabled;

            const auto codeText = HistoryMessage::codeBlockCopyAt(
                _messages[idx], ctx, localPos);
            if (!codeText.isEmpty()) {
                QApplication::clipboard()->setText(codeText);
                emit codeCopied();
            }
        }
        return;
    }

    // Suppress mouse press (prevents drag-to-select) when on the reply pill.
    // Use both the computed hover flag and the painted rect as fallback.
    if (_hoveredFastReply
        || (_replyPillIndex >= 0
            && !_replyPillWidgetRect.isEmpty()
            && _replyPillWidgetRect.contains(e->pos()))) {
        setFocus();
        _selection.resetDrag();
        return;
    }

    // Suppress press on the reaction button/column (handled on release).
    if ((!_reactionButtonWidgetRect.isEmpty()
            && _reactionButtonWidgetRect.contains(e->pos()))
        || (!_reactionColumnWidgetRect.isEmpty()
            && _reactionColumnWidgetRect.contains(e->pos()))) {
        setFocus();
        _selection.resetDrag();
        return;
    }

    // Grab keyboard focus so Cmd+C works.
    setFocus();

    // Track consecutive clicks for triple-click detection.
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto interval = QApplication::doubleClickInterval();
    const auto clickCount = _selection.updateClickCount(e->pos(), now, interval);

    // Triple-click: select all text in the message.
    if (clickCount >= 3) {
        const auto cursor = cursorFromPoint(e->pos());
        if (cursor.isValid() && cursor.messageIndex < _messages.size()) {
            const auto text = HistoryMessage::plainText(
                _messages[cursor.messageIndex]);
            _selection.selectWholeMessage(cursor.messageIndex, int(text.length()));
        }
        _selection.resetDrag();
        update();
        return;
    }

    // Double-click press: just record state, mouseDoubleClickEvent handles selection.
    _selection.setMousePressed(true);
    _selection.setMousePressPos(e->pos());
    _selection.setDragStarted(false);

    if (clickCount == 2) {
        return;
    }

    // Single click: set the anchor cursor — replaces any previous selection.
    const auto cursor = cursorFromPoint(e->pos());
    _selection.setTextSelection(cursor, cursor);
    update();
}

void HistoryList::mouseDoubleClickEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton || isContextMenuMouseGesture(e)) {
        Ui::RpWidget::mouseDoubleClickEvent(e);
        return;
    }

    if (_selection.inSelectionMode()) {
        return;
    }

    setFocus();

    // Qt sends mouseDoubleClickEvent INSTEAD OF mousePressEvent for the
    // 2nd click, so update click tracking here for triple-click detection.
    _selection.setDoubleClick(e->pos(), QDateTime::currentMSecsSinceEpoch());

    const auto cursor = cursorFromPoint(e->pos());
    if (!cursor.isValid()) {
        return;
    }

    // Find word boundaries in the plain text.
    const auto text = HistoryMessage::plainText(
        _messages[cursor.messageIndex]);
    if (text.isEmpty()) {
        return;
    }

    const int pos = qMin(cursor.textPosition, text.length() - 1);

    // Scan left to find word start.
    int wordStart = pos;
    while (wordStart > 0 && !text[wordStart - 1].isSpace()
           && text[wordStart - 1] != QChar::LineSeparator) {
        --wordStart;
    }

    // Scan right to find word end.
    int wordEnd = pos;
    while (wordEnd < text.length() && !text[wordEnd].isSpace()
           && text[wordEnd] != QChar::LineSeparator) {
        ++wordEnd;
    }

    if (wordStart < wordEnd) {
        _selection.setTextSelection(
            { cursor.messageIndex, wordStart },
            { cursor.messageIndex, wordEnd });
        _selection.resetDrag();
        update();
    }
}

void HistoryList::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton || isContextMenuMouseGesture(e)) {
        Ui::RpWidget::mouseReleaseEvent(e);
        return;
    }

    // End an inline-video seek scrub — swallow the release so it isn't taken as a
    // play/pause toggle on the frame.
    if (_videoSeekDragging) {
        _videoSeekDragging = false;
        _selection.resetDrag();
        e->accept();
        return;
    }

    // Pinned mode: complete a "go to message" jump click — the press and the
    // release must land on the same row's button. Consumes the event so it
    // never falls through to normal click routing.
    if (_pinnedMode) {
        const auto pressed = _pressedJumpButtonIndex;
        _pressedJumpButtonIndex = -1;
        if (pressed >= 0
            && jumpButtonRect(pressed).contains(e->pos())
            && pressed < _messages.size()) {
            const auto eventId = _messages[pressed].eventId;
            if (!eventId.isEmpty()) {
                emit jumpToMessageRequested(eventId);
            }
            e->accept();
            return;
        }
    }

    refreshHoverFromPosition(e->pos(), e->buttons());

    // Swallow every click on a message whose redaction is in flight: open, play,
    // vote, select and link handlers would all act on a message that is going away.
    if (isDeletingIndex(messageIndexAt(e->pos().y()))) {
        _selection.resetDrag();
        e->accept();
        return;
    }

    if (_selection.inSelectionMode()) {
        _selection.resetDrag();
        const auto idx = _hoveredIndex;
        if (idx >= 0 && idx < _messages.size()) {
            const auto &item = _messages[idx];
            if (!isServiceMessage(item)
                && !isUnableToDecryptMessage(item)
                && !item.eventId.isEmpty() && !item.delivery.deleted) {
                _selection.toggleSelected(item.eventId);
                if (_selection.selectedEmpty()) {
                    exitSelectionMode();
                } else {
                    emit selectedCountChanged(_selection.selectedCount());
                    update();
                }
            }
        }
        return;
    }

    const bool wasDragging = _selection.dragStarted();
    _selection.resetDrag();

    if (!wasDragging) {
        // Reply pill click: the pill can extend outside the hovered row,
        // so check the painted rect directly as a first-class click target.
        if (_replyPillIndex >= 0
            && _replyPillIndex < _messages.size()
            && !_replyPillWidgetRect.isEmpty()
            && _replyPillWidgetRect.contains(e->pos())
            && !_messages[_replyPillIndex].eventId.isEmpty()) {
            const auto &pillMsg = _messages[_replyPillIndex];
            emit replyRequested(pillMsg.eventId, pillMsg.sender.name, bodyText(pillMsg), QString());
            clearSelection();
            update();
            return;
        }
        // Reaction column click: toggle the emoji under the cursor.
        if (_reactionExpanded
            && _reactionPillIndex >= 0
            && _reactionPillIndex < _messages.size()) {
            const auto cell = reactionColumnCellHit(e->pos());
            if (cell >= 0 && cell < _reactionColumnEmojis.size()) {
                const auto &msg = _messages[_reactionPillIndex];
                const auto emoji = _reactionColumnEmojis[cell];
                if (!msg.eventId.isEmpty()) {
                    auto isSelf = false;
                    for (const auto &r : msg.reactions) {
                        if (r.key == emoji) {
                            isSelf = r.isSelf;
                            break;
                        }
                    }
                    emit reactionRequested(msg.eventId, emoji, !isSelf);
                }
                hideReactionAffordance();
                clearSelection();
                update();
                return;
            }
        }
        // Reaction button click (no prior hover-expand): expand in place.
        if (!_reactionExpanded
            && _reactionPillIndex >= 0
            && !_reactionButtonWidgetRect.isEmpty()
            && _reactionButtonWidgetRect.contains(e->pos())) {
            expandReactionColumn(_reactionPillIndex);
            clearSelection();
            update();
            return;
        }
        if (!_hoveredLinkUrl.isEmpty()) {
            // Every parseable matrix.to permalink is handled in-app (Element
            // does the same): rooms are federated, so the id's server part
            // says nothing about reachability — and the old same-server gate
            // also broke in previews, where _roomId is deliberately empty.
            const auto matrixLink = parseMatrixToUrl(_hoveredLinkUrl);
            if (matrixLink.type != MatrixToLink::None) {
                if (matrixLink.type == MatrixToLink::User) {
                    emit matrixUserLinkActivated(matrixLink.id);
                } else {
                    emit matrixLinkActivated(
                        matrixLink.id, matrixLink.eventId, matrixLink.via);
                }
                clearSelection();
                update();
                return;
            }
            OpenSafeExternalUrl(_hoveredLinkUrl);
            clearSelection();
            update();
            return;
        }

        const auto idx = _hoveredIndex;
        if (idx >= 0 && idx < _messages.size()) {
            auto &item = _messages[idx];
            const auto &layoutItem = _layout[idx];
            const auto messageY = layoutItem.messageY + _contentOffset;
            const auto marginTop = layoutItem.sameSenderAbove
                ? HistoryMessage::kMarginTopAttached
                : HistoryMessage::kMarginTop;
            const QPoint localPos(e->pos().x(), e->pos().y() - messageY - marginTop);

            MessagePaintContext ctx;
            ctx.width = messageContextWidth();
            ctx.isGroup = _isGroup;
            ctx.showOutgoingPrivateAvatar = _showOutgoingPrivateAvatars;
            ctx.sameSenderAbove = layoutItem.sameSenderAbove;
            ctx.sameSenderBelow = layoutItem.sameSenderBelow;
            ctx.isHovered = true;
            ctx.hoveredFastReply = _hoveredFastReply;
            ctx.selectionMode = _selection.inSelectionMode();
            ctx.timelineIndex = &_timelineLookup;
            ctx.largeEmojiEnabled = _largeEmojiEnabled;

            if (isUnableToDecryptMessage(item)
                && HistoryMessage::isInsideUtdVerifyLink(item, ctx, localPos)) {
                emit verifySessionRequested();
                clearSelection();
                update();
                return;
            }

            // Avatar click → open user profile.
            if (_hoveredAvatar
                && HistoryMessage::senderAvatarAt(item, ctx, localPos)
                && !item.sender.id.isEmpty()) {
                emit userAvatarClicked(item.sender.id, item.eventId);
                clearSelection();
                update();
                return;
            }

            if ((HistoryMessage::fastReplyAt(item, ctx, localPos)
                || (_replyPillIndex >= 0
                     && !_replyPillWidgetRect.isEmpty()
                     && _replyPillWidgetRect.contains(e->pos())))
                && !item.eventId.isEmpty()) {
                emit replyRequested(item.eventId, item.sender.name, bodyText(item), QString());
                clearSelection();
                update();
                return;
            }

            const auto replyTarget = HistoryMessage::replyTargetAt(item, ctx, localPos);
            if (!replyTarget.isEmpty()) {
                emit replyToMessageRequested(replyTarget, item.eventId);
                clearSelection();
                update();
                return;
            }

            const auto lpUrl = HistoryMessage::linkPreviewUrlAt(item, ctx, localPos);
            if (!lpUrl.isEmpty()) {
                // Intercept matrix.to links on link preview cards too — all
                // of them, same as inline links above.
                const auto lpMatrix = parseMatrixToUrl(lpUrl);
                if (lpMatrix.type != MatrixToLink::None) {
                    if (lpMatrix.type == MatrixToLink::User) {
                        emit matrixUserLinkActivated(lpMatrix.id);
                    } else {
                        emit matrixLinkActivated(
                            lpMatrix.id, lpMatrix.eventId, lpMatrix.via);
                    }
                    clearSelection();
                    update();
                    return;
                }
                OpenSafeExternalUrl(lpUrl);
                clearSelection();
                update();
                return;
            }

            const auto reactionKey = HistoryMessage::reactionPillAt(item, ctx, localPos);
            if (!reactionKey.isEmpty() && !item.eventId.isEmpty()) {
                auto isSelf = false;
                for (const auto &reaction : item.reactions) {
                    if (reaction.key == reactionKey) {
                        isSelf = reaction.isSelf;
                        break;
                    }
                }
                emit reactionRequested(item.eventId, reactionKey, !isSelf);
                clearSelection();
                update();
                return;
            }

            if (const auto poll = pollContent(item); poll && !_roomId.isEmpty() && !item.eventId.isEmpty()) {
                const auto geometry = pollContentGeometry(item, ctx);
                if (geometry.valid) {
                    const auto optionId = HistoryViewPoll::optionAt(
                        item,
                        ctx,
                        geometry.left,
                        geometry.top,
                        geometry.width,
                        localPos);
                    if (!optionId.isEmpty()) {
                        if (poll->isMultiChoice) {
                            auto selection = currentPollSelection(item);
                            if (selection.contains(optionId)) {
                                selection.removeAll(optionId);
                            } else if (selection.size() < qMax(1, poll->maxSelections)) {
                                selection.push_back(optionId);
                            }
                            if (setPollSelectionLocally(idx, selection)) {
                                clearSelection();
                                update();
                            }
                            return;
                        }

                        if (setPollSelectionLocally(idx, QStringList{optionId})) {
                            emit pollVoteRequested(_roomId, item.eventId, QStringList{optionId});
                        }
                        clearSelection();
                        update();
                        return;
                    }

                    if (HistoryViewPoll::voteButtonAt(
                            item,
                            ctx,
                            geometry.left,
                            geometry.top,
                            geometry.width,
                            localPos)) {
                        const auto selection = currentPollSelection(item);
                        if (!selection.isEmpty()) {
                            emit pollVoteRequested(_roomId, item.eventId, selection);
                        }
                        clearSelection();
                        update();
                        return;
                    }
                }
            }

            // Disable all media open/play actions for uploading or failed items.
            const auto blockMediaClick = item.delivery.outgoing
                && (item.delivery.sendState == SendState::Sending
                    || item.delivery.sendState == SendState::Failed);

            // Upload cancel button — must come before file/audio/media open handlers.
            if (item.delivery.outgoing && item.delivery.sendState == SendState::Sending) {
                const auto cancelRect = HistoryMessage::uploadCancelRect(item, ctx);
                if (!cancelRect.isEmpty() && cancelRect.contains(localPos)) {
                    emit cancelUploadRequested(item.eventId);
                    clearSelection();
                    update();
                    return;
                }
            }

            const auto insideBubble = HistoryMessage::bubbleShapePath(item, ctx)
                .contains(QPointF(localPos));
            const auto url = mediaUrl(item);
            const auto downloadActive = !url.isEmpty()
                && url.startsWith(QStringLiteral("mxc://"))
                && !MediaCache::isResolved(url)
                && MediaCache::isRequested(url);
            if (!blockMediaClick && downloadActive) {
                const auto cancelRect = HistoryMessage::downloadCancelRect(item, ctx);
                if (!cancelRect.isEmpty() && cancelRect.contains(localPos)) {
                    emit mediaDownloadCancelRequested(url);
                    clearSelection();
                    update();
                    return;
                }
                if (insideBubble
                    && (HistoryMessage::isAudioBubble(item)
                        || isFileMessage(item)
                        || isVideoMessage(item))) {
                    return;
                }
            }

            // Audio/file bubble interaction: only inside the bubble shape.
            if (!blockMediaClick && HistoryMessage::isAudioBubble(item) && insideBubble) {
                if (isAudioMessage(item) && isVoiceMessage(item)) {
                    const auto seekRatio = HistoryViewAudio::waveformSeekAt(
                        item,
                        ctx,
                        localPos,
                        0,
                        0,
                        ctx.width);
                    if (seekRatio >= 0.0) {
                        const auto activeDurationMs = (_audioPlayback.durationMs() > 0)
                            ? _audioPlayback.durationMs()
                            : mediaDurationMs(item);
                        if (item.eventId == _audioPlayback.eventId() && activeDurationMs > 0) {
                            const auto seekMs = static_cast<qint64>(seekRatio * activeDurationMs);
                            seekAudio(seekMs);
                            if (_audioPlayback.paused()) {
                                resumeAudio();
                            }
                            return;
                        }
                    }
                }

                if (item.eventId == _audioPlayback.eventId()) {
                    if (_audioPlayback.paused()) {
                        resumeAudio();
                    } else {
                        pauseAudio();
                    }
                } else if (!url.isEmpty()) {
                    auto filePath = url;
                    if (filePath.startsWith(QStringLiteral("mxc://"))) {
                        filePath = MediaCache::localPath(filePath);
                    }
                    if (!filePath.isEmpty()) {
                        playAudio(item.eventId, filePath);
                    } else if (MediaCache::hasMemoryBlob(url)) {
                        playAudioBytes(item.eventId, url);
                    } else {
                        emit audioDownloadRequested(url, item.eventId);
                    }
                }
                return;
            } else if (!blockMediaClick && isMediaBubbleHit(item, ctx, localPos)) {
                if (isVideoMessage(item) && _inlineVideo
                    && !mediaUrl(item).isEmpty()) {
                    const auto toggleInline = [&] {
                        _inlineVideo->toggle(
                            item.eventId,
                            mediaUrl(item),
                            mediaFilename(item),
                            mediaMime(item),
                            qint64(mediaDurationMs(item)));
                    };
                    if (_inlineVideo->activeEventId() != item.eventId) {
                        // Still preview: route its fullscreen/mute controls the same
                        // as during playback (they're drawn identically). Fullscreen
                        // opens the viewer; mute toggles the session preference; a
                        // click anywhere else starts inline playback.
                        const auto mediaRect =
                            HistoryMessage::videoMediaRect(item, ctx);
                        const auto fsRect =
                            HistoryMessage::videoFullscreenButtonRect(mediaRect);
                        const auto muteRect =
                            HistoryMessage::videoMuteButtonRect(
                                mediaRect, qint64(mediaDurationMs(item)));
                        if (fsRect.contains(localPos)) {
                            // Stop any OTHER inline video first so it doesn't keep
                            // playing (with audio) behind the overlay; stash its
                            // position so it resumes wherever it's next opened.
                            if (!_inlineVideo->activeEventId().isEmpty()) {
                                MediaCache::setPlaybackPosition(
                                    _inlineVideo->currentMxc(),
                                    _inlineVideo->positionMs());
                                _inlineVideo->stop();
                            }
                            emit openMediaViewRequested(
                                QVector<TimelineItem>{ item }, 0);
                        } else if (muteRect.contains(localPos)) {
                            _inlineVideo->toggleMute();
                        } else {
                            toggleInline(); // start
                        }
                    } else if (_inlineVideo->failed()) {
                        // Errored out — a click retries from scratch.
                        toggleInline();
                    } else if (_inlineVideo->currentFrame().isNull()) {
                        // Still loading/downloading/buffering (no frame yet) — a
                        // click cancels (stops) it. The proxy stream isn't an
                        // mxc download, so the generic download-cancel handler
                        // above doesn't fire for it.
                        _inlineVideo->stop();
                    } else {
                        // Active with a frame: route control clicks.
                        const auto mediaRect =
                            HistoryMessage::videoMediaRect(item, ctx);
                        const auto fsRect =
                            HistoryMessage::videoFullscreenButtonRect(mediaRect);
                        const auto muteRect =
                            HistoryMessage::videoMuteButtonRect(
                                mediaRect, _inlineVideo->durationMs());
                        const auto seekRect =
                            HistoryMessage::videoSeekBarRect(mediaRect);
                        if (muteRect.contains(localPos)) {
                            _inlineVideo->toggleMute();
                        } else if (fsRect.contains(localPos)) {
                            // Hand off to the fullscreen viewer, preserving the
                            // current playback position. Keep the inline player
                            // alive but PAUSED (not stopped) so returning from
                            // fullscreen lands on a paused frame with controls; it
                            // is re-synced to the fullscreen position when the
                            // viewer closes (HistoryWidget::onFullscreenVideoClosed).
                            MediaCache::setPlaybackPosition(
                                mediaUrl(item), _inlineVideo->positionMs());
                            _inlineVideo->pauseForFullscreen();
                            emit openMediaViewRequested(
                                QVector<TimelineItem>{ item }, 0);
                        } else if (seekRect.contains(localPos)
                                   && seekRect.width() > 0) {
                            const auto ratio = std::clamp(
                                qreal(localPos.x() - seekRect.left())
                                    / qreal(seekRect.width()),
                                qreal(0),
                                qreal(1));
                            _inlineVideo->seekToFraction(ratio);
                        } else {
                            // Frame click toggles play/pause — allowed while
                            // buffering too, so the user can pause the rebuffering
                            // wait (pausing cancels the pending auto-resume).
                            toggleInline();
                        }
                    }
                    clearSelection();
                    update();
                    return;
                } else {
                    // Images open the fullscreen viewer (videos in the list stay
                    // reachable there by swiping). Stop any inline video first so it
                    // doesn't keep playing (with audio) behind the overlay; stash its
                    // position so it resumes wherever it's next opened.
                    if (_inlineVideo && !_inlineVideo->activeEventId().isEmpty()) {
                        MediaCache::setPlaybackPosition(
                            _inlineVideo->currentMxc(), _inlineVideo->positionMs());
                        _inlineVideo->stop();
                    }
                    QVector<TimelineItem> mediaItems;
                    mediaItems.reserve(_messages.size());
                    auto clickedIndex = -1;
                    for (auto i = 0; i < _messages.size(); ++i) {
                        const auto &message = _messages[i];
                        if ((!isImageMessage(message) && !isVideoMessage(message))
                            || mediaUrl(message).isEmpty()) {
                            continue;
                        }
                        if (i == idx) {
                            clickedIndex = mediaItems.size();
                        }
                        mediaItems.push_back(message);
                    }
                    if (clickedIndex >= 0) {
                        emit openMediaViewRequested(mediaItems, clickedIndex);
                    }
                }
            } else if (!blockMediaClick && isFileMessage(item)
                       && !url.isEmpty() && insideBubble) {
                auto filePath = url;
                if (filePath.startsWith(QStringLiteral("mxc://"))) {
                    filePath = MediaCache::localPath(filePath);
                }
                if (filePath.isEmpty()) {
                    // Not yet downloaded — request download.
                    emit fileDownloadRequested(url);
                } else {
                    // Already downloaded — open it.
                    emit fileOpenRequested(
                        url, mediaFilename(item), mediaMime(item));
                }
            }
        }
    }
    // Drag and double-click selections are preserved; the next single click
    // resets the text range.
}

void HistoryList::keyPressEvent(QKeyEvent *e) {
    if (_selection.inSelectionMode() && e->key() == Qt::Key_Escape) {
        exitSelectionMode();
        e->accept();
        return;
    }
    if (e->matches(QKeySequence::Copy)) {
        const auto text = collectSelectedText();
        if (!text.isEmpty()) {
            QApplication::clipboard()->setText(text);
        }
        return;
    }
    Ui::RpWidget::keyPressEvent(e);
}

void HistoryList::leaveEvent(QEvent *e) {
    auto needsUpdate = false;
    if (_hoveredIndex != -1 || _hoveredLinkStart != -1
        || _hoveredFastReply || _hoveredCopyButton || _hoveredTimestamp
        || !_hoveredVoiceSeekEventId.isEmpty()
        || _hoveredVoiceSeekProgress >= 0.0) {
        _hoveredIndex = -1;
        _hoveredLinkStart = -1;
        _hoveredLinkUrl.clear();
        _hoveredFastReply = false;
        _hoveredCopyButton = false;
        _hoveredVoiceSeekEventId.clear();
        _hoveredVoiceSeekProgress = -1.0;
        _hoveredTimestamp = false;
        _tooltipTimer.stop();
        if (_timestampTooltip) {
            _timestampTooltip->hideAnimated();
        }
        setCursor(Qt::ArrowCursor);
        needsUpdate = true;
    }
    // Cancel a pending reaction show/expand, and hide any visible pill after the
    // debounce, when the cursor leaves the list.
    _reactionShowTimer.stop();
    _reactionExpandTimer.stop();
    _reactionPendingIndex = -1;
    _reactionExpandPendingIndex = -1;
    if (_reactionPillIndex >= 0 && !_reactionHideTimer.isActive()) {
        _reactionHideTimer.start();
    }
    if (needsUpdate) {
        update();
    }
    Ui::RpWidget::leaveEvent(e);
}

void HistoryList::wheelEvent(QWheelEvent *e) {
    // A user wheel is genuine scroll intent — re-arm read detection suppressed
    // by a prior programmatic viewport move.
    _readDetectionHold = false;
    // Scroll the expanded reaction column when the cursor is over it; otherwise
    // ignore the event so the chat scroll area receives it.
    if (_reactionExpanded
        && _reactionPillIndex >= 0
        && !_reactionColumnEmojis.isEmpty()
        && !_reactionColumnWidgetRect.isEmpty()
        && _reactionColumnWidgetRect.contains(e->position().toPoint())) {
        const auto scrollMax = HistoryMessage::reactionColumnScrollMax(
            int(_reactionColumnEmojis.size()));
        if (scrollMax > 0) {
            const auto pd = e->pixelDelta();
            const auto deltaY = !pd.isNull() ? pd.y() : e->angleDelta().y();
            if (deltaY != 0) {
                const auto maxStep = 2
                    * (st::reactionCornerSize.height() + st::reactionCornerSkip);
                const auto magnitude = qMin(qAbs(deltaY), maxStep);
                const auto dir = _reactionExpandUp ? 1 : -1;
                const auto delta = (deltaY > 0 ? 1 : -1) * magnitude * dir;
                _reactionScroll = qBound(0, _reactionScroll + delta, scrollMax);
                _reactionHovered = reactionColumnCellHit(e->position().toPoint());
                update();
            }
            e->accept();
            return;
        }
    }
    e->ignore();
}

void HistoryList::setReactionRecentEmojis(const QVector<QString> &emojis) {
    _reactionRecentEmojis = emojis;
}

MessagePaintContext HistoryList::reactionPaintCtx(int index) const {
    MessagePaintContext ctx;
    if (index < 0 || index >= _messages.size() || index >= _layout.size()) {
        return ctx;
    }
    const auto &li = _layout[index];
    ctx.width = messageContextWidth();
    ctx.isGroup = _isGroup;
    ctx.showOutgoingPrivateAvatar = _showOutgoingPrivateAvatars;
    ctx.sameSenderAbove = li.sameSenderAbove;
    ctx.sameSenderBelow = li.sameSenderBelow;
    ctx.selectionMode = _selection.inSelectionMode();
    ctx.timelineIndex = &_timelineLookup;
    ctx.largeEmojiEnabled = _largeEmojiEnabled;
    return ctx;
}

void HistoryList::buildReactionColumnEmojis() {
    _reactionColumnEmojis.clear();
    QSet<QString> seen;
    for (const auto &emoji : quickReactionEmojis()) {
        if (!seen.contains(emoji)) {
            seen.insert(emoji);
            _reactionColumnEmojis.push_back(emoji);
        }
    }
    for (const auto &emoji : _reactionRecentEmojis) {
        if (_reactionColumnEmojis.size() >= kReactionColumnMax) {
            break;
        }
        if (!emoji.isEmpty() && !seen.contains(emoji)) {
            seen.insert(emoji);
            _reactionColumnEmojis.push_back(emoji);
        }
    }
}

void HistoryList::expandReactionColumn(int index) {
    if (index < 0 || index >= _messages.size() || index >= _layout.size()) {
        return;
    }
    buildReactionColumnEmojis();
    const auto count = int(_reactionColumnEmojis.size());
    if (count <= 0) {
        return;
    }
    const auto ctx = reactionPaintCtx(index);
    const auto &li = _layout[index];
    const auto messageY = li.messageY + _contentOffset;
    const auto marginTop = li.sameSenderAbove
        ? HistoryMessage::kMarginTopAttached
        : HistoryMessage::kMarginTop;
    const auto buttonWidget = HistoryMessage::reactionButtonRect(
        _messages[index], ctx).translated(0, messageY + marginTop);
    const auto viewportTop = _visibleTop;
    const auto viewportBottom = _visibleTop
        + (parentWidget() ? parentWidget()->height() : height());
    const auto columnH = HistoryMessage::reactionColumnVisibleInnerHeight(count)
        + st::reactionCornerShadow.top() + st::reactionCornerShadow.bottom();
    const auto spaceBelow = viewportBottom - buttonWidget.bottom();
    const auto spaceAbove = buttonWidget.top() - viewportTop;
    // Expand upward by default; only grow downward when there isn't enough
    // room above and there's more space below.
    _reactionExpandUp = !((spaceAbove < columnH) && (spaceBelow > spaceAbove));
    _reactionScroll = 0;
    _reactionHovered = -1;
    _reactionExpanded = true;
    _reactionPillIndex = index;
    const auto columnLocal = HistoryMessage::reactionColumnRect(
        _messages[index], ctx, count, _reactionExpandUp);
    _reactionColumnWidgetRect = columnLocal.translated(0, messageY + marginTop);
    update();
}

int HistoryList::reactionColumnCellHit(QPoint widgetPos) const {
    const auto i = _reactionPillIndex;
    if (i < 0 || i >= _messages.size() || i >= _layout.size()
        || _reactionColumnEmojis.isEmpty()) {
        return -1;
    }
    const auto ctx = reactionPaintCtx(i);
    const auto &li = _layout[i];
    const auto messageY = li.messageY + _contentOffset;
    const auto marginTop = li.sameSenderAbove
        ? HistoryMessage::kMarginTopAttached
        : HistoryMessage::kMarginTop;
    const QPoint localPos(
        widgetPos.x(),
        widgetPos.y() - messageY - marginTop);
    return HistoryMessage::reactionColumnCellAt(
        _messages[i], ctx, int(_reactionColumnEmojis.size()),
        _reactionScroll, _reactionExpandUp, localPos);
}

void HistoryList::hideReactionAffordance() {
    // Cancel any pending show/expand so a delayed pill can't pop up after hide.
    _reactionShowTimer.stop();
    _reactionExpandTimer.stop();
    _reactionPendingIndex = -1;
    _reactionExpandPendingIndex = -1;
    if (_reactionPillIndex < 0 && !_reactionExpanded) {
        return;
    }
    _reactionPillIndex = -1;
    _reactionExpanded = false;
    _reactionHovered = -1;
    _reactionScroll = 0;
    _reactionButtonWidgetRect = QRect();
    _reactionColumnWidgetRect = QRect();
    update();
}

QRegion HistoryList::affordanceDirtyRegion() const {
    QRegion r;
    if (!_replyPillWidgetRect.isEmpty()) {
        r += _replyPillWidgetRect;
    }
    if (!_reactionButtonWidgetRect.isEmpty()) {
        r += _reactionButtonWidgetRect;
    }
    if (!_reactionColumnWidgetRect.isEmpty()) {
        r += _reactionColumnWidgetRect;
    }
    return r;
}

QRect HistoryList::affordanceRowRect(int index) const {
    if (index < 0 || index >= _layout.size()) {
        return {};
    }
    // Reaction corner button is the tallest resting overhang; it dwarfs the reply
    // pill's upward reach, so inflating symmetrically by it covers both sides.
    const auto overhang = st::reactionCornerSize.height()
        + st::reactionCornerShadow.top()
        + st::reactionCornerShadow.bottom();
    const auto &li = _layout[index];
    return QRect(0, li.y + _contentOffset - overhang,
        width(), li.height + 2 * overhang);
}

void HistoryList::contextMenuEvent(QContextMenuEvent *e) {
    if (e->reason() == QContextMenuEvent::Mouse) {
        refreshHoverFromPosition(e->pos());
    }
    tryShowContextMenuAt(e->globalPos(), e->pos());
    e->accept();
}

bool HistoryList::tryShowContextMenuAt(const QPoint &globalPos, const QPoint &localPos) {
    if (_selection.inSelectionMode()) {
        return false;
    }
    const auto idx = messageIndexAt(localPos.y());
    if (idx < 0 || idx >= _messages.size()) {
        return false;
    }
    if (isServiceMessage(_messages[idx])
        || isUnableToDecryptMessage(_messages[idx])
        || _messages[idx].delivery.deleted
        || isDeletingIndex(idx)) {
        return false;
    }

    const auto &layoutItem = _layout[idx];
    const auto messageY = layoutItem.messageY + _contentOffset;
    const auto marginTop = layoutItem.sameSenderAbove
        ? HistoryMessage::kMarginTopAttached
        : HistoryMessage::kMarginTop;
    const QPoint messageLocalPos(
        localPos.x(),
        localPos.y() - messageY - marginTop);

    MessagePaintContext ctx;
    ctx.width = messageContextWidth();
    ctx.isGroup = _isGroup;
    ctx.showOutgoingPrivateAvatar = _showOutgoingPrivateAvatars;
    ctx.sameSenderAbove = layoutItem.sameSenderAbove;
    ctx.sameSenderBelow = layoutItem.sameSenderBelow;
    ctx.timelineIndex = &_timelineLookup;
    ctx.largeEmojiEnabled = _largeEmojiEnabled;

    const auto insideBubble = HistoryMessage::bubbleShapePath(
        _messages[idx], ctx).contains(QPointF(messageLocalPos));
    if (!insideBubble) {
        return false;
    }

    setFocus();

    // Suppress the hover reply/reaction pills while the context menu is open. The
    // right-click just re-armed hover (refreshHoverFromPosition), and the open menu
    // grabs input so they can't reappear until it closes.
    hideReactionAffordance();
    _replyPillHideTimer.stop();
    _replyPillAnim->stop();
    _replyPillOpacity = 0.0;
    _replyPillIndex = -1;
    _replyPillWidgetRect = QRect();
    update();

    if (_messages[idx].delivery.sendState == SendState::Failed) {
        const auto item = _messages[idx];
        auto *menu = HistoryPopupMenuStyle::createStyledMenu(
            this, HistoryPopupMenuStyle::Variant::WithIcons);
        _activeMenu = menu;
        auto *resendAction = menu->addAction(tr("Resend"));
        // Send paper-plane (the composer's input_send mask), tinted
        // to the menu color. ("send" had no menu/ asset, so no icon showed.)
        HistoryPopupMenuStyle::setActionIconName(
            resendAction, QStringLiteral("chat/input_send"));
        connect(resendAction, &QAction::triggered, this, [this, item] {
            emit resendRequested(item.eventId);
        });
        auto *deleteAction = menu->addAction(tr("Delete"));
        HistoryPopupMenuStyle::setActionIconName(deleteAction, QStringLiteral("delete"));
        connect(deleteAction, &QAction::triggered, this, [this, item] {
            emit deleteMessageRequested(item.eventId);
        });
        // Dispose on hide like showMessageContextMenu does — a lingering hidden
        // menu would keep _activeMenu set and the hover pills suppressed.
        QObject::connect(menu, &HistoryPopupMenuStyle::PopupMenu::aboutToHide,
            menu, &QObject::deleteLater);
        menu->popup(globalPos);
        return true;
    }

    showMessageContextMenu(globalPos, localPos, idx);
    return true;
}

void HistoryList::showMessageContextMenu(const QPoint &globalPos, const QPoint &localPos, int msgIndex) {
    if (msgIndex < 0 || msgIndex >= _messages.size()) {
        return;
    }

    const auto item = _messages[msgIndex];
    if (isServiceMessage(item)) {
        return;
    }
    QString selectedText;
    const bool hasSelectedText = selectedTextForMessage(msgIndex, selectedText);

    auto clickedLinkUrl = QString();
    {
        const auto &layoutItem = _layout[msgIndex];
        const auto messageY = layoutItem.messageY + _contentOffset;
        const auto marginTop = layoutItem.sameSenderAbove
            ? HistoryMessage::kMarginTopAttached
            : HistoryMessage::kMarginTop;
        const QPoint messageLocalPos(
            localPos.x(),
            localPos.y() - messageY - marginTop);

        MessagePaintContext ctx;
        ctx.width = messageContextWidth();
        ctx.isGroup = _isGroup;
        ctx.showOutgoingPrivateAvatar = _showOutgoingPrivateAvatars;
        ctx.sameSenderAbove = layoutItem.sameSenderAbove;
        ctx.sameSenderBelow = layoutItem.sameSenderBelow;
        ctx.timelineIndex = &_timelineLookup;
        ctx.largeEmojiEnabled = _largeEmojiEnabled;

        auto linkStart = -1;
        clickedLinkUrl = HistoryMessage::linkAt(
            item,
            ctx,
            messageLocalPos,
            linkStart);
        if (clickedLinkUrl.isEmpty()) {
            clickedLinkUrl = HistoryMessage::linkPreviewUrlAt(
                item,
                ctx,
                messageLocalPos);
        }
        if (clickedLinkUrl.isEmpty()) {
            clickedLinkUrl = _hoveredLinkUrl;
        }
    }

    auto *menu = HistoryPopupMenuStyle::createStyledMenu(
        this,
        HistoryPopupMenuStyle::Variant::WithIcons);

    const auto addAction = [menu](
            const QString &label,
            const QString &icon,
            const std::function<void()> &slot) {
        auto *action = menu->addAction(label);
        HistoryPopupMenuStyle::setActionIconName(action, icon);
        if (slot) {
            QObject::connect(action, &QAction::triggered, menu, slot);
        }
    };

    // "Copy Link" — shown when right-clicking a URL.
    const auto addCopyLinkAction = [this, &addAction, clickedLinkUrl] {
        if (clickedLinkUrl.isEmpty()) {
            return;
        }
        addAction(tr("Copy Link"), QStringLiteral("copy"), [this, clickedLinkUrl] {
            QApplication::clipboard()->setText(clickedLinkUrl);
            emit contextActionFeedback(tr("Link copied to clipboard"));
        });
    };

    const auto addCopySelectedTextAction = [this, &addAction, selectedText] {
        addAction(tr("Copy Selected Text"), QStringLiteral("copy"), [this, selectedText] {
            if (selectedText.isEmpty()) {
                return;
            }
            QApplication::clipboard()->setText(selectedText);
            emit contextActionFeedback(tr("Selected text copied"));
        });
    };


    const auto addSelectAction = [this, &addAction, msgIndex] {
        addAction(tr("Select"), QStringLiteral("select"), [this, msgIndex] {
            enterSelectionMode(msgIndex);
        });
    };

    // "Show in Finder" + "Save as..." for file attachments.
    const bool mediaMessage = isMediaMessage(item);
    const auto addDocumentActions = [this, &addAction, menu, item, mediaMessage] {
        if (!mediaMessage) {
            return;
        }
        const auto url = mediaUrl(item);
        const auto filename = mediaFilename(item);
        const auto localPath = MediaCache::localPath(url);
        // Save action — label varies by content type.
        const auto saveLabel = isVideoMessage(item)
            ? tr("Save Video As...")
            : isAudioMessage(item)
                ? tr("Save Audio As...")
            : isImageMessage(item)
                ? tr("Save Image As...")
                : tr("Save File As...");
        addAction(saveLabel, QStringLiteral("download"), [this, item, localPath, saveLabel, url, filename] {
            auto sourcePath = localPath;
            if (sourcePath.isEmpty()) {
                sourcePath = MediaCache::localPath(url);
            }

            auto suggestedName = filename;
            if (suggestedName.isEmpty() && !sourcePath.isEmpty()) {
                suggestedName = QFileInfo(sourcePath).fileName();
            }
            if (suggestedName.isEmpty()) {
                suggestedName = QStringLiteral("file-%1").arg(QDateTime::currentSecsSinceEpoch());
            }

            // Pick default save directory based on content type.
            const auto stdLoc = isImageMessage(item)
                ? QStandardPaths::PicturesLocation
                : isAudioMessage(item)
                    ? QStandardPaths::MusicLocation
                : isVideoMessage(item)
                    ? QStandardPaths::MoviesLocation
                    : QStandardPaths::DownloadLocation;
            const auto saveDir = QStandardPaths::writableLocation(stdLoc);
            const auto initialPath = saveDir.isEmpty()
                ? suggestedName
                : QDir(saveDir).filePath(suggestedName);

            const auto filter = isImageMessage(item)
                ? tr("Images (*.png *.jpg *.jpeg *.webp *.bmp);;All Files (*)")
                : isAudioMessage(item)
                    ? tr("Audio (*.mp3 *.m4a *.ogg *.opus *.wav *.flac *.aac);;All Files (*)")
                : isVideoMessage(item)
                    ? tr("Videos (*.mp4 *.mov *.mkv *.webm);;All Files (*)")
                    : tr("All Files (*)");

            const auto targetPath = QFileDialog::getSaveFileName(
                window(),
                saveLabel,
                initialPath,
                filter);
            if (targetPath.isEmpty()) {
                return;
            }
            if (sourcePath.isEmpty()) {
                if (url.startsWith(QStringLiteral("mxc://"))) {
                    emit mediaExportRequested(url, targetPath);
                    return;
                }
                emit contextActionFeedback(tr("File not yet downloaded."));
                return;
            }
            if (sourcePath != targetPath) {
                QFile::remove(targetPath);
                QFile::copy(sourcePath, targetPath);
            }
            emit contextActionFeedback(tr("File saved."));
        });
        // Reveal in the file manager — only for file/audio that have been
        // downloaded and copied to the Downloads folder.
        if ((isFileMessage(item) || isAudioMessage(item))
            && !localPath.isEmpty()) {
            const auto downloadsDir = QStandardPaths::writableLocation(
                QStandardPaths::DownloadLocation);
            auto targetName = filename;
            if (targetName.isEmpty()) {
                targetName = QFileInfo(localPath).fileName();
            }
            const auto targetPath = downloadsDir + QStringLiteral("/") + targetName;
            if (QFileInfo::exists(targetPath)) {
#ifdef Q_OS_MACOS
                const auto revealLabel = tr("Show in Finder");
#else // Q_OS_MACOS
                const auto revealLabel = tr("Show in Folder");
#endif // Q_OS_MACOS
                addAction(revealLabel,
                    QStringLiteral("show_in_folder"), [targetPath] {
                    Platform::RevealInFolder(targetPath);
                });
            }
        }
    };

    // "Copy Text" becomes "Copy Link" when right-clicking a URL.
    const auto addCopyTextOrLinkAction = [this, &addAction, item, clickedLinkUrl] {
        if (!clickedLinkUrl.isEmpty()) {
            addAction(tr("Copy Link"), QStringLiteral("copy"), [this, clickedLinkUrl] {
                QApplication::clipboard()->setText(clickedLinkUrl);
                emit contextActionFeedback(tr("Link copied to clipboard"));
            });
        } else if (!bodyText(item).isEmpty()) {
            addAction(tr("Copy Text"), QStringLiteral("copy"), [this, item] {
                QApplication::clipboard()->setText(bodyText(item));
                emit contextActionFeedback(tr("Text copied to clipboard"));
            });
        }
    };

    const auto addCopyMessageLinkAction = [this, &addAction, item] {
        // Links into the private Saved Messages room are unusable by anyone.
        if (_savedMessagesMode) return;
        const auto link = messageLink(item);
        if (link.isEmpty()) return;
        addAction(tr("Copy Message Link"), QStringLiteral("link"), [this, link] {
            QApplication::clipboard()->setText(link);
            emit contextActionFeedback(tr("Message link copied"));
        });
    };

    // Pinned mode: every message in the section is pinned, so the action is
    // always "Unpin" with the `unpin` asset. Otherwise base it on isPinned.
    const auto addPinUnpinAction = [this, &addAction, item] {
        // No membership in an unjoined-room preview, so pinning is impossible.
        if (!_canPinMessages || _readOnly) {
            return;
        }
        if (_pinnedMode) {
            addAction(tr("Unpin"), QStringLiteral("unpin"), [this, item] {
                emit pinMessageRequested(item.eventId, false);
            });
            return;
        }
        addAction(
            item.isPinned ? tr("Unpin") : tr("Pin"),
            item.isPinned ? QStringLiteral("unpin") : QStringLiteral("pin"),
            [this, item] {
                emit pinMessageRequested(item.eventId, !item.isPinned);
            });
    };

    if (item.delivery.outgoing) {
        // Pinned mode omits the "Reply" action (the section is a read view).
        if (hasSelectedText) {
            if (!readOnlyView()) {
                addAction(tr("Reply"), QStringLiteral("reply"), [this, item] {
                    emit replyRequested(item.eventId, item.sender.name, bodyText(item), QString());
                });
            }
            addCopySelectedTextAction();
        } else {
            if (!readOnlyView()) {
                addAction(tr("Reply"), QStringLiteral("reply"), [this, item] {
                    emit replyRequested(item.eventId, item.sender.name, bodyText(item), QString());
                });
            }
            addCopyTextOrLinkAction();
        }
    addDocumentActions();
    // Read-only views (pinned section / unjoined-room preview) omit Edit / Delete.
    if (!readOnlyView()) {
        addAction(tr("Edit"), QStringLiteral("edit"), [this, item] {
            emit editMessageRequested(item.eventId, item.sender.name, bodyText(item), formattedText(item));
        });
    }
    addPinUnpinAction();
    addCopyMessageLinkAction();
    // Forwarding needs a source room we belong to — omit it in a preview.
    if (!_readOnly) {
        addAction(tr("Forward"), QStringLiteral("forward"), [this, item] {
            emit forwardRequested(item.eventId);
        });
    }

    if (!readOnlyView()) {
        addAction(tr("Delete"), QStringLiteral("delete"), [this, item] {
            emit deleteMessageRequested(item.eventId);
        });
        addSelectAction();
    }
    } else {
        if (hasSelectedText) {
            if (!readOnlyView()) {
                addAction(tr("Reply"), QStringLiteral("reply"), [this, item] {
                    emit replyRequested(item.eventId, item.sender.name, bodyText(item), QString());
                });
            }
            addCopySelectedTextAction();
        } else {
            if (!readOnlyView()) {
                addAction(tr("Reply"), QStringLiteral("reply"), [this, item] {
                    emit replyRequested(item.eventId, item.sender.name, bodyText(item), QString());
                });
            }
            addCopyTextOrLinkAction();
        }
    addDocumentActions();
    addPinUnpinAction();
    addCopyMessageLinkAction();
    // Forwarding needs a source room we belong to — omit it in a preview.
    if (!_readOnly) {
        addAction(tr("Forward"), QStringLiteral("forward"), [this, item] {
            emit forwardRequested(item.eventId);
        });
    }

    if (!readOnlyView()) {
        if (_savedMessagesMode) {
            // Substituted forwards render as incoming but are the user's own
            // events — they must stay deletable.
            addAction(tr("Delete"), QStringLiteral("delete"), [this, item] {
                emit deleteMessageRequested(item.eventId);
            });
        }
        addSelectAction();
    }
}

    // Enable reaction strip for non-service, non-deleted messages.
    // Hidden in read-only views (pinned section / unjoined-room preview) and
    // in Saved Messages (no reactions on a private notepad).
    if (!readOnlyView() && !_savedMessagesMode
        && !item.eventId.isEmpty() && !item.delivery.deleted) {
        menu->setReactionStrip(item.eventId);

        connect(menu, &HistoryPopupMenuStyle::PopupMenu::reactionChosen,
            this, [this](const QString &eventId, const QString &key) {
            bool alreadyReacted = false;
            if (const auto *msg = messageById(eventId)) {
                for (const auto &r : msg->reactions) {
                    if (r.key == key && r.isSelf) {
                        alreadyReacted = true;
                        break;
                    }
                }
            }
            emit reactionRequested(eventId, key, !alreadyReacted);
        });

        connect(menu, &HistoryPopupMenuStyle::PopupMenu::reactionExpandRequested,
            this, [this, menu](const QString &eventId, const QPoint &) {
            // Pass the menu's top-left position so the picker appears
            // at the same horizontal position as the popup menu.
            emit reactionPanelRequested(eventId, menu->mapToGlobal(QPoint(0, 0)));
        });
    }

    _activeMenu = menu;
    menu->popup(globalPos);
    QObject::connect(menu, &HistoryPopupMenuStyle::PopupMenu::aboutToHide, menu, &QObject::deleteLater);
}

QString HistoryList::messageLink(const TimelineItem &item) const {
    if (item.eventId.isEmpty()) {
        return QString();
    }
    if (!_roomId.isEmpty()) {
        const auto encodedRoom = QString::fromUtf8(
            QUrl::toPercentEncoding(_roomId));
        const auto encodedEvent = QString::fromUtf8(
            QUrl::toPercentEncoding(item.eventId));
        return QStringLiteral("https://matrix.to/#/")
            + encodedRoom
            + QStringLiteral("/")
            + encodedEvent;
    }
    return item.eventId;
}

void HistoryList::selectWholeMessage(int msgIndex) {
    if (msgIndex < 0 || msgIndex >= _messages.size()) {
        return;
    }
    const auto text = HistoryMessage::plainText(_messages[msgIndex]);
    if (text.isEmpty()) {
        return;
    }
    _selection.selectWholeMessage(msgIndex, int(text.size()));
    update();
}

bool HistoryList::selectedTextForMessage(int msgIndex, QString &text) const {
    int start = -1, end = -1;
    if (!selectionForMessage(msgIndex, start, end)) {
        return false;
    }

    text = collectSelectedText();
    if (text.isEmpty() && msgIndex >= 0 && msgIndex < _messages.size()) {
        text = HistoryMessage::selectedText(_messages[msgIndex], start, end);
    }
    return !text.isEmpty();
}

TextCursor HistoryList::cursorFromPoint(QPoint pos) const {
    const auto idx = messageIndexAt(pos.y());
    if (idx < 0 || idx >= _messages.size()) {
        return {};
    }

    const auto &layoutItem = _layout[idx];
    const auto messageY = layoutItem.messageY + _contentOffset;
    const auto marginTop = layoutItem.sameSenderAbove
        ? HistoryMessage::kMarginTopAttached
        : HistoryMessage::kMarginTop;
    const QPoint localPos(pos.x(), pos.y() - messageY - marginTop);

    MessagePaintContext ctx;
    ctx.width = messageContextWidth();
    ctx.isGroup = _isGroup;
    ctx.showOutgoingPrivateAvatar = _showOutgoingPrivateAvatars;
    ctx.sameSenderAbove = layoutItem.sameSenderAbove;
    ctx.timelineIndex = &_timelineLookup;
    ctx.largeEmojiEnabled = _largeEmojiEnabled;

    const int textPos = HistoryMessage::cursorAt(
        _messages[idx], ctx, localPos, true);
    if (textPos < 0) {
        return {};
    }
    return { idx, textPos };
}

void HistoryList::clearSelection() {
    _selection.clearTextSelection();
}

void HistoryList::normalizedSelection(TextCursor &from, TextCursor &to) const {
    _selection.normalizedSelection(from, to);
}

bool HistoryList::selectionForMessage(int msgIndex, int &start, int &end) const {
    return _selection.selectionForMessage(msgIndex, start, end);
}

QString HistoryList::collectSelectedText() const {
    TextCursor from, to;
    normalizedSelection(from, to);
    if (!from.isValid() || !to.isValid()) {
        return {};
    }

    QStringList parts;
    for (int i = from.messageIndex; i <= to.messageIndex && i < _messages.size(); ++i) {
        int start, end;
        if (!selectionForMessage(i, start, end)) {
            continue;
        }
        const auto text = HistoryMessage::selectedText(_messages[i], start, end);
        if (!text.isEmpty()) {
            parts.append(text);
        }
    }
    return parts.join(u'\n');
}

QStringList HistoryList::selectedEventIdsInOrder() const {
    QStringList ids;
    ids.reserve(_selection.selectedCount());
    for (const auto &item : _messages) {
        if (!item.eventId.isEmpty() && _selection.selectedContains(item.eventId)) {
            ids.push_back(item.eventId);
        }
    }
    return ids;
}

void HistoryList::rebuildMessageIndex() {
    QVector<QString> ids;
    ids.reserve(_messages.size());
    for (const auto &item : _messages) {
        ids.append(item.eventId);
    }
    _messageIndex.rebuild(ids);
}

void HistoryList::setBackgroundDoodlesEnabled(bool enabled) {
    _bgCache.setDoodlesEnabled(enabled);
    update();
}

void HistoryList::setBackgroundAnchor(QWidget *anchor) {
    _bgAnchor = anchor;
    update();
}

void HistoryList::invalidateBackground() {
    // Themes describe their wallpaper as four st:: corner colours, read late by
    // DefaultCornerColors(); rebuilding the source is how a theme switch (new
    // corners, new doodle) reaches the composite.
    _bgCache.setSource(QPixmap(), Theme::DefaultCornerColors());
    update();
}

QSize HistoryList::backgroundArea() const {
    const auto viewport = parentWidget();
    const auto fillHeight = _bgAnchor
        ? _bgAnchor->height()
        : (viewport ? viewport->height() : height());
    return QSize(width(), qMax(fillHeight, 1));
}

int HistoryList::backgroundOriginY() const {
    // How far the scroll viewport sits below the chat column's top; the
    // wallpaper is drawn from the column's top, so the viewport shows the
    // slice starting here. Zero without an anchor (background == viewport).
    const auto viewport = parentWidget();
    if (!_bgAnchor || !viewport || !_bgAnchor->isAncestorOf(viewport)) {
        return 0;
    }
    return viewport->mapTo(_bgAnchor, QPoint(0, 0)).y();
}

void HistoryList::setLargeEmojiEnabled(bool enabled) {
    if (_largeEmojiEnabled == enabled) {
        return;
    }
    _largeEmojiEnabled = enabled;
    // Invalidate all cached heights so large emoji messages get re-laid out.
    for (auto &item : _layout) {
        item.cachedBubbleHeight = -1;
        item.cachedWidth = -1;
    }
    recalculateLayout();
    update();
}

void HistoryList::setReplyButtonEnabled(bool enabled) {
    if (_replyButtonEnabled == enabled) {
        return;
    }
    _replyButtonEnabled = enabled;
    if (!enabled) {
        // Hide any reply pill currently shown.
        _replyPillHideTimer.stop();
        _replyPillAnim->stop();
        _replyPillOpacity = 0.0;
        _replyPillIndex = -1;
        _replyPillWidgetRect = QRect();
        update();
    }
}

void HistoryList::setReactionButtonEnabled(bool enabled) {
    if (_reactionButtonEnabled == enabled) {
        return;
    }
    _reactionButtonEnabled = enabled;
    if (!enabled) {
        hideReactionAffordance();
    }
}

// Per-item glow: only utdState==0 UTDs glow, downgraded after the safety window
// so a stuck UTD eventually stops shimmering (guard against waiting forever).
constexpr qint64 kUtdGlowSafetyMs = 55000;

bool HistoryList::itemGlowActive(const TimelineItem &item) {
    if (!isUtdGlowing(item)) {
        return false;
    }
    // An unverified session never glows, whatever the cause. Nothing is in
    // flight for it: historical keys need verification before the backup opens,
    // and key forwarding only targets verified devices. A shimmer would promise
    // progress that cannot happen, so show the static card — it carries the
    // "Verify this device" action, which is the only thing that changes anything.
    if (!_sessionVerified) {
        return false;
    }
    const auto now = QDateTime::currentMSecsSinceEpoch();
    auto it = _utdGlowFirstSeenMs.find(item.eventId);
    if (it == _utdGlowFirstSeenMs.end()) {
        _utdGlowFirstSeenMs.insert(item.eventId, now);
        return true;
    }
    return (now - it.value()) < kUtdGlowSafetyMs;
}

void HistoryList::refreshDecryptingGlowState() {
    // Recompute the set of currently-glowing UTDs. The 30fps shimmer timer
    // iterates this set instead of scanning every loaded message each tick. This
    // O(n) scan runs only on message-change events (prepend/append/slice), not
    // per frame.
    _glowingEventIds.clear();
    bool anyGlowing = false;
    for (const auto &msg : _messages) {
        if (itemGlowActive(msg)) {
            anyGlowing = true;
            _glowingEventIds.insert(msg.eventId);
        }
    }
    for (auto it = _utdGlowFirstSeenMs.begin(); it != _utdGlowFirstSeenMs.end();) {
        it = _glowingEventIds.contains(it.key())
            ? std::next(it)
            : _utdGlowFirstSeenMs.erase(it);
    }
    if (anyGlowing && !_decryptingAnimTimer.isActive()) {
        _decryptingGlowStartMs = QDateTime::currentMSecsSinceEpoch();
        _decryptingGlowProgress = 0.0;
        _lastGlowingCount = _glowingEventIds.size();
        _decryptingAnimTimer.start();
    } else if (!anyGlowing && _decryptingAnimTimer.isActive()) {
        _decryptingAnimTimer.stop();
    }
}

void HistoryList::relayoutUtdRows() {
    // UTD rows switch between the small decrypting skeleton and the full
    // "Unable to decrypt" card, so their heights change — relayout them.
    if (_layout.size() != _messages.size()) {
        return;
    }
    bool invalidated = false;
    for (int i = 0; i < _messages.size(); ++i) {
        if (isUnableToDecryptMessage(_messages[i])) {
            _layout[i].cachedBubbleHeight = -1;
            _layout[i].cachedWidth = -1;
            invalidated = true;
        }
    }
    if (invalidated) {
        recalculateLayout();
        const auto visibleHeight = parentWidget()
            ? parentWidget()->height()
            : height();
        resize(width(), qMax(_totalHeight, visibleHeight));
    }
    update();
}

void HistoryList::setSessionVerified(bool verified) {
    if (_sessionVerified == verified) {
        return;
    }
    _sessionVerified = verified;
    // Verification flips whole classes of UTDs between "shimmering" and "static
    // card", so the cached glow set is stale until it is rebuilt — without this
    // the change only lands on the next slice update.
    refreshDecryptingGlowState();
    update();
}

// --- Audio playback methods ---

void HistoryList::playAudio(const QString &eventId, const QString &filePath) {
    stopAudio();

    // Find the matching message for MIME/filename context.
    QString filename, mime;
    qint64 knownDurationMs = 0;
    if (const auto *it = messageById(eventId)) {
        filename = mediaFilename(*it);
        mime = mediaMime(*it);
        knownDurationMs = mediaDurationMs(*it);
    }
    if (knownDurationMs <= 0) {
        knownDurationMs = _cachedAudioDurations.value(eventId, 0);
    }
    _audioPlayback.startFile(eventId, knownDurationMs);
    const auto resolved = MediaCache::resolvedPathForPlayback(filePath, filename, mime);
    _mediaPlayer->setSource(QUrl::fromLocalFile(resolved));
    _mediaPlayer->play();
    _playbackRepaintTimer.start();
    update();
}

void HistoryList::playAudioBytes(const QString &eventId, const QString &mxcUrl) {
    const auto blob = MediaCache::memoryBlob(mxcUrl);
    if (blob.bytes.isEmpty()) {
        return;
    }

    stopAudio();

    QString filename = blob.filename;
    QString mime = blob.mime;
    qint64 knownDurationMs = 0;
    if (const auto *it = messageById(eventId)) {
        if (filename.isEmpty()) {
            filename = mediaFilename(*it);
        }
        if (mime.isEmpty()) {
            mime = mediaMime(*it);
        }
        knownDurationMs = mediaDurationMs(*it);
    }
    if (knownDurationMs <= 0) {
        knownDurationMs = _cachedAudioDurations.value(eventId, 0);
    }
    _audioPlayback.startMemory(eventId, mxcUrl, knownDurationMs);

    _audioPlaybackBuffer = new QBuffer(this);
    _audioPlaybackBuffer->setData(blob.bytes);
    _audioPlaybackBuffer->open(QIODevice::ReadOnly);

    const auto extension = extensionForPlaybackHint(filename, mime);
    const auto sourceHint = QUrl(QStringLiteral("telematrix://media/%1.%2")
        .arg(eventId, extension));
    _mediaPlayer->setSourceDevice(_audioPlaybackBuffer, sourceHint);
    _mediaPlayer->play();
    _playbackRepaintTimer.start();
    update();
}

void HistoryList::pauseAudio() {
    _mediaPlayer->pause();
    _audioPlayback.pause();
    _playbackRepaintTimer.stop();
    update();
}

void HistoryList::resumeAudio() {
    _mediaPlayer->play();
    _audioPlayback.resume();
    _playbackRepaintTimer.start();
}

void HistoryList::stopAudio() {
    _mediaPlayer->stop();
    if (_audioPlaybackBuffer) {
        _audioPlaybackBuffer->deleteLater();
        _audioPlaybackBuffer = nullptr;
    }
    _audioPlayback.stop();
    _hoveredVoiceSeekEventId.clear();
    _hoveredVoiceSeekProgress = -1.0;
    _playbackRepaintTimer.stop();
}

void HistoryList::seekAudio(qint64 positionMs) {
    _mediaPlayer->setPosition(positionMs);
    _audioPlayback.setPosition(positionMs);
}

QRect HistoryList::activeVideoSeekBarRect() const {
    if (!_inlineVideo) {
        return {};
    }
    const auto eventId = _inlineVideo->activeEventId();
    if (eventId.isEmpty() || _inlineVideo->currentFrame().isNull()) {
        return {};
    }
    for (auto i = 0; i < _messages.size(); ++i) {
        if (_messages[i].eventId != eventId) {
            continue;
        }
        const auto &layoutItem = _layout[i];
        const auto messageY = layoutItem.messageY + _contentOffset;
        const auto marginTop = layoutItem.sameSenderAbove
            ? HistoryMessage::kMarginTopAttached
            : HistoryMessage::kMarginTop;
        MessagePaintContext ctx;
        ctx.width = messageContextWidth();
        ctx.isGroup = _isGroup;
        ctx.showOutgoingPrivateAvatar = _showOutgoingPrivateAvatars;
        ctx.sameSenderAbove = layoutItem.sameSenderAbove;
        ctx.sameSenderBelow = layoutItem.sameSenderBelow;
        ctx.timelineIndex = &_timelineLookup;
        ctx.largeEmojiEnabled = _largeEmojiEnabled;
        const auto mediaRect = HistoryMessage::videoMediaRect(_messages[i], ctx);
        if (mediaRect.isNull()) {
            return {};
        }
        const auto seekLocal = HistoryMessage::videoSeekBarRect(mediaRect);
        if (seekLocal.isNull()) {
            return {};
        }
        // Row-local space -> widget space: x is shared, y offsets by the row top.
        return seekLocal.translated(0, messageY + marginTop);
    }
    return {};
}

void HistoryList::seekActiveVideoToX(int x, const QRect &bar) {
    if (!_inlineVideo || bar.width() <= 0) {
        return;
    }
    const auto ratio = std::clamp(
        qreal(x - bar.left()) / qreal(bar.width()), qreal(0), qreal(1));
    _inlineVideo->seekToFraction(ratio);
}

void HistoryList::probeAudioDuration(const QString &eventId, const QString &filePath) {
    // One probe at a time; durationChanged/errorOccurred clears the active event.
    if (!_probeEventId.isEmpty()) {
        return;
    }

    // Don't insert into _probedEventIds here — wait for success in
    // the durationChanged handler.  This ensures failed probes can be
    // retried on the next paint cycle.

    // Find the matching message for MIME/filename context.
    QString filename, mime;
    if (const auto *it = messageById(eventId)) {
        filename = mediaFilename(*it);
        mime = mediaMime(*it);
    }

    const auto resolved = MediaCache::resolvedPathForPlayback(filePath, filename, mime);
    _probeEventId = eventId;
    _probePlayer->setSource(QUrl::fromLocalFile(resolved));
}

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "dialogs_inner.h"

#include "ui/text/emoji_text.h"
#include "dialogs_layout.h"
#include "../history/history_popup_menu_style.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <utility>
#include <QAction>
#include <QCoreApplication>
#include <QLocale>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QImage>
#include <QMetaObject>
#include <QPainter>
#include <QScrollBar>
#include <QPainterPath>
#include <QMouseEvent>
#include <QTextLayout>
#include <QDateTime>
#include <QTimer>
#include "history/history_popup_menu_style.h"
#include <QResizeEvent>
#include "../protocol/media_cache.h"
#include "ui/painter.h"
#include "ui/empty_userpic.h"
#include "ui/style/icon_provider.h"
#include "styles/style_constants.h"

namespace TeleMatrix {
namespace {
constexpr int kSearchResultChatTypeSkip = 3;
constexpr int kSearchEmptyMinHeight = 220;
constexpr int kSearchLoadingRows = 2;
constexpr int kSearchEmptyTextWidth = 240;
constexpr int kDialogsInitialLoadingMinHeight = 220;

[[nodiscard]] QString folderMenuIconName(const FolderInfo &folder) {
    if (folder.filterId == 0) {
        return QStringLiteral("folders/folders_all");
    }
    if (folder.filterId == 1) {
        return QStringLiteral("folders/folders_private");
    }
    if (folder.filterId == 2) {
        return QStringLiteral("folders/folders_unread");
    }

    const auto text = folder.displayName.trimmed().toLower();
    if (text.contains(QStringLiteral("unread"))) {
        return QStringLiteral("folders/folders_unread");
    }
    if (text.contains(QStringLiteral("private"))
        || text.contains(QStringLiteral("direct"))) {
        return QStringLiteral("folders/folders_private");
    }
    if (text.contains(QStringLiteral("group"))) {
        return QStringLiteral("folders/folders_group");
    }
    if (text.contains(QStringLiteral("channel"))) {
        return QStringLiteral("folders/folders_channels");
    }
    if (text.contains(QStringLiteral("bot"))) {
        return QStringLiteral("folders/folders_bots");
    }
    if (text.contains(QStringLiteral("unmuted"))
        || text.contains(QStringLiteral("notify"))) {
        return QStringLiteral("folders/folders_unmuted");
    }
    if (text.contains(QStringLiteral("setup"))) {
        return QStringLiteral("folders/folders_setup");
    }
    return QStringLiteral("folders/folders_custom");
}

[[nodiscard]] int searchResultsHeaderHeight(bool hasResults) {
    return hasResults ? st::searchedBarHeight : 0;
}

[[nodiscard]] QString formatSearchResultDate(qint64 timestamp) {
    if (timestamp <= 0) {
        return QString();
    }

    const auto now = QDateTime::currentDateTime();
    const auto dt = QDateTime::fromSecsSinceEpoch(timestamp);
    const auto today = now.date();
    const auto date = dt.date();
    if (date == today) {
        return QLocale().toString(dt.time(), QLocale::ShortFormat);
    } else if (date.addDays(1) == today) {
        return QCoreApplication::translate("DialogsInner", "Yesterday");
    } else if (date.addDays(7) > today) {
        return QLocale().toString(dt, QStringLiteral("ddd"));
    } else if (date.year() == today.year()) {
        return QLocale().toString(dt, QStringLiteral("MMM d"));
    }
    return QLocale().toString(dt, QLocale::ShortFormat);
}

[[nodiscard]] QString searchResultsCountText(int totalApprox, int visibleCount) {
    const auto total = (totalApprox > 0) ? totalApprox : visibleCount;
    return QCoreApplication::translate(
        "DialogsInner", "%n message(s) found", nullptr, total);
}

void paintSearchAvatar(
    QPainter &p,
    const QString &name,
    const QString &avatarUrl,
    const QString &entityId,
    int x,
    int y,
    int size,
    QWidget *repaintTarget)
{
    if (!avatarUrl.isEmpty()) {
        const auto dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
        const auto image = MediaCache::loadScaledImageAsync(
            avatarUrl,
            QSize(size, size),
            dpr,
            repaintTarget);
        if (!image.isNull()) {
            QPainterPath clip;
            clip.addEllipse(x, y, size, size);
            p.save();
            p.setClipPath(clip);
            p.drawImage(QRect(x, y, size, size), image);
            p.restore();
            return;
        }
    }

    Ui::EmptyUserpic::paint(
        p, entityId.isEmpty() ? name : entityId, name,
        x, y, size);
}

[[nodiscard]] QImage loadDialogsMaskIcon(
    const QString &baseName,
    qreal dpr,
    const QColor &color)
{
    QString path = QStringLiteral(":/dialogs/%1").arg(baseName);
    if (dpr >= 2.5) {
        path += QStringLiteral("@3x.png");
    } else if (dpr >= 1.5) {
        path += QStringLiteral("@2x.png");
    } else {
        path += QStringLiteral(".png");
    }
    auto mask = QImage(path);
    if (mask.isNull()) {
        return {};
    }
    if (dpr >= 2.5) {
        mask.setDevicePixelRatio(3.0);
    } else if (dpr >= 1.5) {
        mask.setDevicePixelRatio(2.0);
    } else {
        mask.setDevicePixelRatio(1.0);
    }
    return TeleMatrix::Style::IconProvider::colorizeMask(mask, color);
}

void drawHighlightedSnippet(
    QPainter &p,
    int x,
    int baseline,
    int availableWidth,
    const QString &text,
    const QString &query,
    const QColor &baseColor,
    const QColor &highlightColor)
{
    const auto font = static_cast<const QFont &>(st::normalFont);
    const auto &emojiMetrics = TeleMatrix::EmojiText::CachedMetricsFor(
        font, st::emojiInlineSlot, st::emojiInlineGlyph);
    const auto elided = TeleMatrix::EmojiText::Elide(
        text, font, emojiMetrics, availableWidth);
    if (elided.isEmpty()) {
        return;
    }

    p.setFont(font);
    if (query.isEmpty()) {
        p.setPen(baseColor);
        TeleMatrix::EmojiText::DrawLine(p, x, baseline, elided, emojiMetrics);
        return;
    }

    auto left = x;
    auto from = 0;
    while (from < elided.size()) {
        const auto match = elided.indexOf(query, from, Qt::CaseInsensitive);
        const auto till = (match < 0) ? elided.size() : match;
        if (till > from) {
            p.setPen(baseColor);
            left += TeleMatrix::EmojiText::DrawLine(
                p, left, baseline, elided.mid(from, till - from), emojiMetrics);
        }
        if (match < 0) {
            break;
        }
        p.setPen(highlightColor);
        left += TeleMatrix::EmojiText::DrawLine(
            p, left, baseline, elided.mid(match, query.size()), emojiMetrics);
        from = match + query.size();
    }
}

void paintCenteredSearchMessage(
    QPainter &p,
    const QRect &rect,
    const QString &title,
    const QString &subtitle = QString(),
    const QString &note = QString())
{
    if (rect.isEmpty()) {
        return;
    }

    const auto hasSubtitle = !subtitle.isEmpty();
    const auto hasNote = !note.isEmpty();
    const auto titleFont = (hasSubtitle || hasNote)
        ? static_cast<const QFont &>(st::semiboldFont)
        : static_cast<const QFont &>(st::normalFont);
    const auto bodyFont = static_cast<const QFont &>(st::normalFont);
    const auto titleMetrics = QFontMetrics(titleFont);
    const auto bodyMetrics = QFontMetrics(bodyFont);
    const auto blockWidth = qMin(rect.width(), kSearchEmptyTextWidth);
    const auto wrapFlags = Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap;
    // Measure each block's actual wrapped height instead of assuming a fixed line
    // count: at kSearchEmptyTextWidth these strings can wrap to 3 lines, so a
    // hardcoded two-line box clipped the last line.
    const auto measure = [&](const QString &text) {
        return bodyMetrics.boundingRect(
            QRect(0, 0, blockWidth, INT_MAX),
            wrapFlags,
            text).height();
    };
    constexpr auto kSubtitleSpacing = 8;
    constexpr auto kNoteSpacing = 12;

    const auto titleHeight = titleMetrics.height();
    const auto subtitleHeight = hasSubtitle ? measure(subtitle) : 0;
    const auto noteHeight = hasNote ? measure(note) : 0;
    auto totalHeight = titleHeight;
    if (hasSubtitle) {
        totalHeight += kSubtitleSpacing + subtitleHeight;
    }
    if (hasNote) {
        totalHeight += kNoteSpacing + noteHeight;
    }

    const auto x = rect.x() + (rect.width() - blockWidth) / 2;
    auto y = rect.y() + qMax(0, (rect.height() - totalHeight) / 2);

    p.setPen(st::windowSubTextFgOver);
    p.setFont(titleFont);
    p.drawText(
        QRect(x, y, blockWidth, titleHeight),
        Qt::AlignHCenter | Qt::AlignTop,
        title);
    y += titleHeight;
    if (hasSubtitle) {
        y += kSubtitleSpacing;
        p.setFont(bodyFont);
        p.drawText(QRect(x, y, blockWidth, subtitleHeight), wrapFlags, subtitle);
        y += subtitleHeight;
    }
    if (hasNote) {
        y += kNoteSpacing;
        p.setFont(bodyFont);
        p.setPen(st::windowSubTextFg);
        p.drawText(QRect(x, y, blockWidth, noteHeight), wrapFlags, note);
    }
}

QColor searchPlaceholderColor() {
    constexpr auto kTwoPi = 6.28318530717958647692;
    const auto ms = QDateTime::currentMSecsSinceEpoch();
    const auto phase = 0.5 + 0.5 * std::sin((double(ms % 1200) / 1200.0) * kTwoPi);
    // Derive base from the current theme's dialog background.
    const auto bg = st::dialogsBg;
    const auto bgLightness = bg.lightnessF();
    // In dark themes, use a lighter shade; in light themes, a darker shade.
    const auto base = (bgLightness < 0.5)
        ? qBound(0, bg.red() + 20, 255)   // dark theme: slightly lighter
        : st::dialogsSearchPlaceholderFg.red();
    const auto delta = int(std::round(phase * 10.0));
    const auto v = qBound(0, base - delta, 255);
    return QColor(v, v, v);
}

void paintLoadingPlaceholderRow(QPainter &p, int rowY, int width) {
    const auto c = searchPlaceholderColor();
    p.fillRect(0, rowY, width, st::dialogsRowHeight, st::dialogsBg);

    PainterHighQualityEnabler hq(p);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawEllipse(
        st::dialogsPadding.left(),
        rowY + st::dialogsPadding.top(),
        st::dialogsPhotoSize,
        st::dialogsPhotoSize);
    constexpr auto kNameWidth = 60;
    constexpr auto kStatusWidth = 100;
    const auto titleHeight = st::semiboldFont->ascent;
    const auto bodyHeight = st::normalFont->ascent;
    const auto titleTop = rowY
        + st::dialogsNameTop
        + (st::semiboldFont->height - titleHeight) / 2;
    p.drawRoundedRect(
        QRectF(st::dialogsNameLeft, titleTop, kNameWidth, titleHeight),
        titleHeight / 2.0,
        titleHeight / 2.0);
    const auto bodyTop = rowY
        + st::dialogsTextTop
        + (st::normalFont->height - bodyHeight) / 2;
    p.drawRoundedRect(
        QRectF(st::dialogsTextLeft, bodyTop, kStatusWidth, bodyHeight),
        bodyHeight / 2.0,
        bodyHeight / 2.0);
}

} // namespace

DialogsInner::DialogsInner(QWidget *parent)
    : Ui::RpWidget(parent)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    _searchLoadingTimer = new QTimer(this);
    _searchLoadingTimer->setInterval(33);
    QObject::connect(_searchLoadingTimer, &QTimer::timeout, this, [this] {
        if (_searchLoading) {
            update();
        }
    });
}

void DialogsInner::setRooms(const QVector<RoomSummary> &rooms) {
    _allRows.clear();
    _allRows.reserve(rooms.size());
    for (const auto &room : rooms) {
        DialogsRow row;
        row.updateFrom(room);
        row.setDraft(_drafts.value(room.roomId));
        _allRows.append(row);
    }
    std::stable_sort(_allRows.begin(), _allRows.end(), [](const DialogsRow &a, const DialogsRow &b) {
        if (a.isPinned() != b.isPinned()) {
            return a.isPinned();
        }
        if (a.isPinned() && b.isPinned()) {
            return a.pinnedIndex() < b.pinnedIndex();
        }
        // Invites above regular rooms (but below pinned).
        const auto aInvite = (a.membership() == MembershipState::Invite);
        const auto bInvite = (b.membership() == MembershipState::Invite);
        if (aInvite != bInvite) return aInvite;
        return a.timestamp() > b.timestamp();
    });

    rebuildAllRowsIndex();
    applyFilters();
}

void DialogsInner::setDraft(const QString &roomId, const QString &text) {
    if (roomId.isEmpty()) {
        return;
    }
    const auto draft = text.simplified();
    if (draft.isEmpty()) {
        _drafts.remove(roomId);
    } else {
        _drafts.insert(roomId, draft);
    }
    for (auto i = 0; i < _allRows.size(); ++i) {
        if (_allRows[i].roomId() != roomId) {
            continue;
        }
        _allRows[i].setDraft(draft);
        break;
    }
    for (auto i = 0; i < _rows.size(); ++i) {
        if (_rows[i].roomId() != roomId) {
            continue;
        }
        _rows[i].setDraft(draft);
        updateRowRect(i);
        break;
    }
}

void DialogsInner::setRoomUnreadCount(const QString &roomId, int count) {
    if (roomId.isEmpty()) {
        return;
    }
    for (auto i = 0; i < _allRows.size(); ++i) {
        if (_allRows[i].roomId() != roomId) {
            continue;
        }
        _allRows[i].setUnreadCount(count);
        break;
    }
    if (reapplyUnreadFilterIfNeeded()) {
        return;
    }
    for (auto i = 0; i < _rows.size(); ++i) {
        if (_rows[i].roomId() != roomId) {
            continue;
        }
        _rows[i].setUnreadCount(count);
        updateRowRect(i);
        break;
    }
}

void DialogsInner::setRoomHighlightCount(const QString &roomId, int count) {
    if (roomId.isEmpty()) {
        return;
    }
    for (auto i = 0; i < _allRows.size(); ++i) {
        if (_allRows[i].roomId() != roomId) {
            continue;
        }
        _allRows[i].setHighlightCount(count);
        break;
    }
    if (reapplyUnreadFilterIfNeeded()) {
        return;
    }
    for (auto i = 0; i < _rows.size(); ++i) {
        if (_rows[i].roomId() != roomId) {
            continue;
        }
        _rows[i].setHighlightCount(count);
        updateRowRect(i);
        break;
    }
}

void DialogsInner::setRoomMarkedUnread(const QString &roomId, bool marked) {
    if (roomId.isEmpty()) {
        return;
    }
    for (auto i = 0; i < _allRows.size(); ++i) {
        if (_allRows[i].roomId() != roomId) {
            continue;
        }
        _allRows[i].setMarkedUnread(marked);
        break;
    }
    if (reapplyUnreadFilterIfNeeded()) {
        return;
    }
    for (auto i = 0; i < _rows.size(); ++i) {
        if (_rows[i].roomId() != roomId) {
            continue;
        }
        _rows[i].setMarkedUnread(marked);
        updateRowRect(i);
        break;
    }
}

void DialogsInner::setActiveFilter(int filterId) {
    // Selecting a folder/built-in clears any active space (the two are mutually
    // exclusive; the rail shows one selection).
    if (_activeFilterId == filterId && _activeSpaceId.isEmpty()) {
        return;
    }
    _activeFilterId = filterId;
    _activeSpaceId.clear();
    applyFilters();
}

void DialogsInner::setActiveSpace(const QString &spaceId) {
    if (_activeSpaceId == spaceId) {
        return;
    }
    _activeSpaceId = spaceId;
    // A space selection supersedes the built-in/custom filter (back to "All" for
    // the folder axis so leaving the space returns to all chats).
    _activeFilterId = 0;
    applyFilters();
}

void DialogsInner::setSearchFilter(const QString &query) {
    const auto normalized = query.trimmed();
    if (_searchQuery == normalized) {
        return;
    }
    _searchQuery = normalized;
    applyFilters();
}

void DialogsInner::setFolders(const QVector<FolderInfo> &folders) {
    _folders = folders;
}

void DialogsInner::toggleRoomFolder(const QString &roomId, int folderId) {
    if (roomId.isEmpty() || folderId <= 0) {
        return;
    }

    auto toggle = [roomId, folderId](DialogsRow &row) {
        if (row.roomId() != roomId) {
            return false;
        }
        auto ids = row.filterIds();
        const auto existing = std::find(ids.begin(), ids.end(), folderId);
        if (existing == ids.end()) {
            ids.push_back(folderId);
        } else {
            ids.erase(existing);
        }
        std::sort(ids.begin(), ids.end());
        row.setFilterIds(ids);
        return true;
    };

    for (auto &row : _allRows) {
        toggle(row);
    }
    for (auto &row : _rows) {
        toggle(row);
    }
    applyFilters();
}

void DialogsInner::setRoomInFolder(const QString &roomId, int folderId, bool member) {
    if (roomId.isEmpty() || folderId <= 0) {
        return;
    }
    auto apply = [roomId, folderId, member](DialogsRow &row) {
        if (row.roomId() != roomId) {
            return;
        }
        auto ids = row.filterIds();
        const auto existing = std::find(ids.begin(), ids.end(), folderId);
        const bool has = (existing != ids.end());
        if (member && !has) {
            ids.push_back(folderId);
            std::sort(ids.begin(), ids.end());
            row.setFilterIds(ids);
        } else if (!member && has) {
            ids.erase(existing);
            row.setFilterIds(ids);
        }
    };
    for (auto &row : _allRows) {
        apply(row);
    }
    for (auto &row : _rows) {
        apply(row);
    }
    applyFilters();
}

bool DialogsInner::roomInFolder(const QString &roomId, int folderId) const {
    for (const auto &row : _allRows) {
        if (row.roomId() == roomId) {
            return row.filterIds().contains(folderId);
        }
    }
    return false;
}

bool DialogsInner::setRoomPinned(const QString &roomId, bool pinned) {
    if (roomId.isEmpty()) {
        return false;
    }

    auto changed = false;
    for (auto &row : _allRows) {
        if (row.roomId() != roomId) {
            continue;
        }
        if (row.isPinned() == pinned) {
            return false;
        }
        row.setPinned(pinned);
        if (pinned) {
            int maxIndex = 0;
            for (const auto &r : _allRows) {
                if (r.isPinned() && r.pinnedIndex() > maxIndex) {
                    maxIndex = r.pinnedIndex();
                }
            }
            row.setPinnedIndex(maxIndex + 1);
        } else {
            row.setPinnedIndex(0);
        }
        changed = true;
        break;
    }
    if (!changed) {
        return false;
    }

    std::stable_sort(_allRows.begin(), _allRows.end(), [](const DialogsRow &a, const DialogsRow &b) {
        if (a.isPinned() != b.isPinned()) {
            return a.isPinned();
        }
        if (a.isPinned() && b.isPinned()) {
            return a.pinnedIndex() < b.pinnedIndex();
        }
        // Invites above regular rooms (but below pinned).
        const auto aInvite = (a.membership() == MembershipState::Invite);
        const auto bInvite = (b.membership() == MembershipState::Invite);
        if (aInvite != bInvite) return aInvite;
        return a.timestamp() > b.timestamp();
    });
    rebuildAllRowsIndex();
    applyFilters();
    return true;
}

void DialogsInner::setRoomNotificationMode(const QString &roomId, RoomNotificationMode mode) {
    for (auto &row : _allRows) {
        if (row.roomId() == roomId) {
            row.setNotificationMode(mode);
            break;
        }
    }
    for (auto &row : _rows) {
        if (row.roomId() == roomId) {
            row.setNotificationMode(mode);
            break;
        }
    }
    update();
}

void DialogsInner::applyFilters() {
    rebuildAllRowsIndex();
    _rows.clear();
    _rows.reserve(_allRows.size());

    const auto normalizedQuery = _searchQuery.toLower();
    const bool searching = !normalizedQuery.isEmpty();

    // When searching, collect (row, score) pairs for ranking.
    struct ScoredRow {
        DialogsRow row;
        int score = 0;
    };
    QVector<ScoredRow> scoredRows;

    for (const auto &row : _allRows) {
        // Apply folder/space filter only when not searching (search is cross-folder
        // and cross-space).
        if (!searching) {
            if (!_activeSpaceId.isEmpty()) {
                // A space is selected: keep only its (recursive) member rooms.
                if (!row.spaceIds().contains(_activeSpaceId)) {
                    continue;
                }
            } else if (_activeFilterId == 1) {
                // Saved Messages is a chat with yourself: Personal includes it
                // even though it is not an m.direct room (tdesktop parity).
                if (!row.isDirect() && row.roomId() != _savedMessagesRoomId) {
                    continue;
                }
            } else if (_activeFilterId == 2) {
                if (!row.hasUnreadIndicator()) {
                    continue;
                }
            } else if (_activeFilterId > 2 && !row.filterIds().contains(_activeFilterId)) {
                continue;
            }
        }

        if (searching) {
            const auto score = row.searchScore(normalizedQuery);
            if (score < 0) {
                continue; // no match
            }
            scoredRows.append({row, score});
        } else {
            _rows.append(row);
        }
    }

    if (searching) {
        // Sort by score (lower = better), then pinned as tie-breaker, then timestamp.
        std::stable_sort(scoredRows.begin(), scoredRows.end(),
            [](const ScoredRow &a, const ScoredRow &b) {
                if (a.score != b.score) {
                    return a.score < b.score;
                }
                if (a.row.isPinned() != b.row.isPinned()) {
                    return a.row.isPinned();
                }
                return a.row.timestamp() > b.row.timestamp();
            });
        _rows.reserve(scoredRows.size());
        for (auto &sr : scoredRows) {
            _rows.append(std::move(sr.row));
        }
    }

    // Re-resolve the selected row to its new index. Keep the desired room id even when it isn't in
    // this snapshot (index -1 = no highlight for now): a just-joined or previewed room isn't in the
    // list yet, so clearing here would drop the pending selection and the room would never light up
    // once it finally appears. When it does, a later setRooms finds it and highlights it.
    if (!_selectedRoomId.isEmpty()) {
        _selectedIndex = -1;
        for (int i = 0; i < _rows.size(); ++i) {
            if (_rows[i].roomId() == _selectedRoomId) {
                _selectedIndex = i;
                break;
            }
        }
    }

    if (_menuRowIndex >= _rows.size()) {
        _menuRowIndex = -1;
    }
    if (_hoveredIndex >= _rows.size()) {
        _hoveredIndex = -1;
    }

    // The rows were rebuilt, so any in-flight drag/click index now points at a
    // different (or missing) room. Re-resolve the pressed room to its new index
    // via its captured id so a pending click still targets the right room; if it
    // is gone, drop the drag state entirely.
    if (_draggedIndex >= 0) {
        int reresolved = -1;
        if (!_draggedRoomId.isEmpty()) {
            for (int i = 0; i < _rows.size(); ++i) {
                if (_rows[i].roomId() == _draggedRoomId) {
                    reresolved = i;
                    break;
                }
            }
        }
        if (reresolved < 0) {
            _draggedRoomId.clear();
            _dragStarted = false;
        }
        _draggedIndex = reresolved;
        _dropTargetIndex = -1;
    }

    updateContentSize();
    update();
}

bool DialogsInner::reapplyUnreadFilterIfNeeded() {
    if (_activeFilterId != 2 || !_searchQuery.trimmed().isEmpty()) {
        return false;
    }
    applyFilters();
    return true;
}

void DialogsInner::setRoomSelectedCallback(RoomSelectedCallback callback) {
    _roomSelectedCallback = std::move(callback);
}

void DialogsInner::setMessageSearchMode(bool active, int topInset) {
    topInset = qMax(0, topInset);
    if (_messageSearchMode == active
        && _messageSearchTopInset == (active ? topInset : 0)) {
        return;
    }

    _messageSearchMode = active;
    _messageSearchTopInset = active ? topInset : 0;
    if (!active) {
        _searchLoading = false;
        _searchResolved = false;
        if (_searchLoadingTimer) {
            _searchLoadingTimer->stop();
        }
        _searchHoveredIndex = -1;
        _searchSelectedIndex = -1;
        setCursor(Qt::ArrowCursor);
    }

    updateContentSize();
    update();
}

void DialogsInner::setInitialLoading(bool loading) {
    if (_initialLoading == loading) {
        return;
    }
    _initialLoading = loading;
    if (_initialLoading) {
        _initialLoadingPaintedOnce = false;
        _hoveredIndex = -1;
        _searchHoveredIndex = -1;
        _menuRowIndex = -1;
        setCursor(Qt::ArrowCursor);
    }
    updateContentSize();
    update();
}

void DialogsInner::setSearchLoading(bool loading) {
    if (_searchLoading == loading) {
        return;
    }
    _searchLoading = loading;
    if (_searchLoading) {
        _searchResolved = false;
        if (_searchLoadingTimer && !_searchLoadingTimer->isActive()) {
            _searchLoadingTimer->start();
        }
    } else if (_searchLoadingTimer) {
        _searchLoadingTimer->stop();
    }
    updateContentSize();
    update();
}

void DialogsInner::setMessageSearchQuery(const QString &query) {
    const auto normalized = query.trimmed();
    if (_messageSearchQuery == normalized) {
        return;
    }
    _messageSearchQuery = normalized;
    update();
}

void DialogsInner::setMessageSearchGlobalScope(bool globalScope) {
    if (_messageSearchGlobalScope == globalScope) {
        return;
    }
    _messageSearchGlobalScope = globalScope;
    update();
}

void DialogsInner::setViewportHeight(int height) {
    height = qMax(0, height);
    if (_viewportHeight == height) {
        return;
    }
    _viewportHeight = height;
    if (_initialLoading) {
        updateContentSize();
    }
}

bool DialogsInner::isDefaultSearchVisible() const {
    return !_messageSearchMode && !_searchQuery.isEmpty();
}

int DialogsInner::filteredRowsHeight() const {
    return _rows.size() * DialogsRow::rowHeight();
}

int DialogsInner::defaultSearchResultsTop() const {
    return filteredRowsHeight() + searchResultsHeaderHeight(true);
}

void DialogsInner::paintDialogRows(QPainter &p, const QRect &clip) {
    if (_rows.isEmpty()) {
        return;
    }

    const auto firstVisible = qMax(0, clip.top() / DialogsRow::rowHeight());
    if (firstVisible >= _rows.size()) {
        return;
    }
    const auto lastVisible = qMin(
        _rows.size() - 1,
        qMax(0, clip.bottom() / DialogsRow::rowHeight()));

    for (int i = firstVisible; i <= lastVisible; ++i) {
        if (_dragStarted && i == _draggedIndex) {
            const auto rowY = i * DialogsRow::rowHeight();
            p.fillRect(0, rowY, width(), DialogsRow::rowHeight(), st::dialogsBgOver);
            continue;
        }
        const auto rowY = i * DialogsRow::rowHeight();
        p.save();
        p.translate(0, rowY);

        DialogsPaintContext ctx;
        ctx.width = width();
        ctx.active = (i == _selectedIndex);
        ctx.selected = ((i == _hoveredIndex || i == _menuRowIndex) && i != _selectedIndex);
        ctx.savedMessages = !_savedMessagesRoomId.isEmpty()
            && _rows[i].roomId() == _savedMessagesRoomId;
        ctx.repaintTarget = this;

        DialogsLayout::paintRow(p, _rows[i], ctx);
        p.restore();
    }

    if (_dragStarted && _draggedIndex >= 0 && _draggedIndex < _rows.size()) {
        if (_dropTargetIndex >= 0 && _dropTargetIndex != _draggedIndex) {
            const auto lineY = (_dropTargetIndex < _draggedIndex)
                ? _dropTargetIndex * DialogsRow::rowHeight()
                : (_dropTargetIndex + 1) * DialogsRow::rowHeight();
            p.setPen(QPen(st::dialogsBgActive, 2));
            p.drawLine(10, lineY, width() - 10, lineY);
        }

        const auto dragOffset = _dragCurrentY - _dragStartY;
        const auto drawY = _draggedIndex * DialogsRow::rowHeight() + dragOffset;
        p.save();
        p.setOpacity(0.7);
        p.translate(0, drawY);
        DialogsPaintContext ctx;
        ctx.width = width();
        ctx.active = false;
        ctx.selected = true;
        ctx.savedMessages = !_savedMessagesRoomId.isEmpty()
            && _rows[_draggedIndex].roomId() == _savedMessagesRoomId;
        ctx.repaintTarget = this;
        DialogsLayout::paintRow(p, _rows[_draggedIndex], ctx);
        p.restore();
    }
}

void DialogsInner::setSavedMessagesRoomId(const QString &roomId) {
    if (_savedMessagesRoomId == roomId) {
        return;
    }
    _savedMessagesRoomId = roomId;
    update();
}

void DialogsInner::updateContentSize() {
    const auto widthHint = (width() > 0) ? width() : 300;
    int totalHeight = 0;

    if (_initialLoading) {
        totalHeight = qMax(
            kDialogsInitialLoadingMinHeight,
            _viewportHeight > 0 ? _viewportHeight : height());
    } else if (_messageSearchMode) {
        totalHeight = _messageSearchTopInset;
        if (_searchLoading) {
            totalHeight += st::searchedBarHeight + kSearchLoadingRows * st::dialogsRowHeight;
        } else if (_searchResults.isEmpty()) {
            totalHeight += kSearchEmptyMinHeight;
        } else {
            totalHeight += searchResultsHeaderHeight(true);
            totalHeight += _searchResults.size() * st::dialogsRowHeight;
        }
    } else if (isDefaultSearchVisible()) {
        totalHeight = filteredRowsHeight();
        if (_searchLoading) {
            totalHeight += st::searchedBarHeight
                + kSearchLoadingRows * st::dialogsRowHeight;
        } else if (!_searchResults.isEmpty()) {
            totalHeight += st::searchedBarHeight
                + _searchResults.size() * st::dialogsRowHeight;
        } else if (_searchResolved && _rows.isEmpty()) {
            totalHeight += kSearchEmptyMinHeight;
        }
    } else if (_searchResults.isEmpty()) {
        totalHeight = _rows.size() * DialogsRow::rowHeight();
    } else {
        totalHeight = searchResultsHeaderHeight(true)
            + _searchResults.size() * st::dialogsRowHeight;
    }

    setMinimumHeight(totalHeight);
    resize(widthHint, totalHeight);
}

void DialogsInner::resizeEvent(QResizeEvent *e) {
    Ui::RpWidget::resizeEvent(e);
}

QRect DialogsInner::activeRowRect() const {
    // Derived from the selection, NOT from what the last paint happened to cover:
    // paintDialogRows only walks the rows intersecting the repaint clip, so a
    // partial repaint (a hover, an avatar arriving, a badge changing) would report
    // "no active row" and clear the seam cover a frame after it was set.
    if (_initialLoading
            || _messageSearchMode
            || _selectedIndex < 0
            || _selectedIndex >= _rows.size()) {
        return QRect();
    }
    return QRect(
        0,
        _selectedIndex * DialogsRow::rowHeight(),
        width(),
        DialogsRow::rowHeight());
}

void DialogsInner::paintEvent(QPaintEvent *e) {
    paintContents(e);

    // Reported from the paint pass because that is the one place guaranteed to run
    // for every reason the row can move — selection, scroll, resize, reorder, a
    // banner appearing above the list.
    const auto active = activeRowRect();
    if (active != _activeRowRect) {
        _activeRowRect = active;
        Q_EMIT activeRowPainted(active);
    }
}

void DialogsInner::paintContents(QPaintEvent *e) {
    QPainter p(this);

    // Fill background (white) to avoid dark-mode bleed-through.
    p.fillRect(e->rect(), st::windowBg);

    if (_initialLoading) {
        paintInitialLoading(p, e->rect());
        return;
    }

    if (_messageSearchMode) {
        paintSearchSection(p, e->rect(), _messageSearchTopInset, _messageSearchGlobalScope);
        return;
    }

    if (isDefaultSearchVisible()) {
        paintDialogRows(p, e->rect());
        paintSearchSection(p, e->rect(), filteredRowsHeight(), false);
        return;
    }

    paintDialogRows(p, e->rect());
}

void DialogsInner::mousePressEvent(QMouseEvent *e) {
    if (_initialLoading) {
        Ui::RpWidget::mousePressEvent(e);
        return;
    }
    if (e->button() == Qt::LeftButton) {
        if (_messageSearchMode) {
            const auto index = searchResultRowAtY(e->pos().y());
            if (index >= 0 && index < _searchResults.size()) {
                const auto &hit = _searchResults[index];
                // Keep the clicked result highlighted (active row).
                _searchSelectedIndex = index;
                update();
                emit searchResultClicked(hit.roomId, hit.eventId);
            }
            return;
        }
        if (isDefaultSearchVisible()) {
            const auto searchIndex = searchResultRowAtY(e->pos().y());
            if (searchIndex >= 0 && searchIndex < _searchResults.size()) {
                const auto &hit = _searchResults[searchIndex];
                // Keep the clicked result highlighted (active row).
                _searchSelectedIndex = searchIndex;
                update();
                emit searchResultClicked(hit.roomId, hit.eventId);
                return;
            }
        }
        const auto index = rowAtY(e->pos().y());
        if (index >= 0 && index < _rows.size()) {
            if (_rows[index].isPinned()) {
                _draggedIndex = index;
                _draggedRoomId = _rows[index].roomId();
                _dragStartY = e->pos().y();
                _dragCurrentY = e->pos().y();
                _dragStarted = false;
            } else {
                setSelectedRow(index);
            }
        }
    }
}

void DialogsInner::mouseMoveEvent(QMouseEvent *e) {
    if (_initialLoading) {
        setCursor(Qt::ArrowCursor);
        Ui::RpWidget::mouseMoveEvent(e);
        return;
    }
    if (_messageSearchMode || isDefaultSearchVisible()) {
        const auto searchIndex = searchResultRowAtY(e->pos().y());
        if (searchIndex != _searchHoveredIndex) {
            _searchHoveredIndex = searchIndex;
            update();
        }
        if (searchIndex >= 0 && searchIndex < _searchResults.size()) {
            if (_hoveredIndex >= 0) {
                setHoveredRow(-1);
            }
            setCursor(Qt::PointingHandCursor);
            return;
        }
        // Message-search paints no dialog rows (only results or the empty
        // prompt), so don't let the dialog-row hover below claim a phantom
        // row and show a pointing-hand cursor over non-interactive space.
        if (_messageSearchMode) {
            if (_hoveredIndex >= 0) {
                setHoveredRow(-1);
            }
            setCursor(Qt::ArrowCursor);
            return;
        }
    }
    if (_draggedIndex >= 0 && (e->buttons() & Qt::LeftButton)) {
        _dragCurrentY = e->pos().y();
        if (!_dragStarted) {
            if (qAbs(_dragCurrentY - _dragStartY) >= kDragThreshold) {
                _dragStarted = true;
                setCursor(Qt::ClosedHandCursor);
            } else {
                return;
            }
        }
        // Count pinned rows in filtered view.
        int pinnedCount = 0;
        for (const auto &row : _rows) {
            if (row.isPinned()) ++pinnedCount;
        }
        const auto targetRow = std::clamp(
            rowAtY(_dragCurrentY),
            0,
            pinnedCount - 1);
        if (targetRow < 0) {
            return;
        }
        _dropTargetIndex = targetRow;
        update();
        return;
    }
    const auto index = rowAtY(e->pos().y());
    if (index < 0 && _hoveredIndex < 0) {
        setCursor(Qt::ArrowCursor);
    }
    setHoveredRow(index);
}

void DialogsInner::mouseReleaseEvent(QMouseEvent *e) {
    if (_initialLoading) {
        Ui::RpWidget::mouseReleaseEvent(e);
        return;
    }
    if (_messageSearchMode) {
        Ui::RpWidget::mouseReleaseEvent(e);
        return;
    }
    if (e->button() == Qt::LeftButton && _draggedIndex >= 0) {
        if (_dragStarted && _dropTargetIndex >= 0
            && _dropTargetIndex != _draggedIndex) {
            // Collect pinned rows from _allRows.
            QVector<int> pinnedAllIndices;
            for (int i = 0; i < _allRows.size(); ++i) {
                if (_allRows[i].isPinned()) {
                    pinnedAllIndices.append(i);
                }
            }

            // Map filtered drag/drop indices to _allRows pinned indices.
            // Build a mapping: _rows pinned index -> _allRows pinned index.
            QVector<int> filteredToPinned;
            for (int i = 0; i < _rows.size(); ++i) {
                if (_rows[i].isPinned()) {
                    // Find this row in pinnedAllIndices.
                    for (int j = 0; j < pinnedAllIndices.size(); ++j) {
                        if (_allRows[pinnedAllIndices[j]].roomId() == _rows[i].roomId()) {
                            filteredToPinned.append(j);
                            break;
                        }
                    }
                }
            }

            if (_draggedIndex < filteredToPinned.size()
                && _dropTargetIndex < filteredToPinned.size()) {
                // Collect pinned rows.
                QVector<DialogsRow> pinnedRows;
                for (auto idx : pinnedAllIndices) {
                    pinnedRows.append(_allRows[idx]);
                }
                // Reorder: remove from old position, insert at new.
                const auto srcPinIdx = filteredToPinned[_draggedIndex];
                const auto dstPinIdx = filteredToPinned[_dropTargetIndex];
                auto movedRow = pinnedRows[srcPinIdx];
                pinnedRows.remove(srcPinIdx);
                pinnedRows.insert(dstPinIdx, movedRow);
                // Reassign pinnedIndex.
                for (int i = 0; i < pinnedRows.size(); ++i) {
                    pinnedRows[i].setPinnedIndex(i + 1);
                }
                // Write back to _allRows.
                for (int i = 0; i < pinnedAllIndices.size(); ++i) {
                    _allRows[pinnedAllIndices[i]] = pinnedRows[i];
                }
                // Re-sort and refilter.
                std::stable_sort(_allRows.begin(), _allRows.end(),
                    [](const DialogsRow &a, const DialogsRow &b) {
                        if (a.isPinned() != b.isPinned()) return a.isPinned();
                        if (a.isPinned() && b.isPinned()) return a.pinnedIndex() < b.pinnedIndex();
                        const auto aInvite = (a.membership() == MembershipState::Invite);
                        const auto bInvite = (b.membership() == MembershipState::Invite);
                        if (aInvite != bInvite) return aInvite;
                        return a.timestamp() > b.timestamp();
                    });
                rebuildAllRowsIndex();
                applyFilters();
                emit pinnedOrderChanged(pinnedRoomIds());
            }
        } else if (!_dragStarted) {
            // It was a click, not a drag — select the row.
            setSelectedRow(_draggedIndex);
        }
        _draggedIndex = -1;
        _dropTargetIndex = -1;
        _dragStarted = false;
        _draggedRoomId.clear();
        setCursor(Qt::PointingHandCursor);
        update();
    }
    Ui::RpWidget::mouseReleaseEvent(e);
}

void DialogsInner::leaveEvent(QEvent * /*e*/) {
    if (_searchHoveredIndex >= 0) {
        _searchHoveredIndex = -1;
        update();
    }
    if (_initialLoading) {
        setCursor(Qt::ArrowCursor);
        return;
    }
    if (_messageSearchMode) {
        setCursor(Qt::ArrowCursor);
        return;
    }
    setHoveredRow(-1);
}

void DialogsInner::contextMenuEvent(QContextMenuEvent *e) {
    if (_initialLoading) {
        e->accept();
        return;
    }
    if (_messageSearchMode) {
        e->accept();
        return;
    }
    // e->pos() can be wrong inside a scroll area (Qt maps to full widget
    // coordinates including the scrolled-off portion). Compute the correct
    // Y relative to our visible content by using the global position and
    // the position of our first visible pixel on screen.
    // Use the already-tracked hovered row index from mouseMoveEvent,
    // which correctly handles scroll area coordinate mapping.
    const auto index = _hoveredIndex;
    if (index < 0 || index >= _rows.size()) {
        Ui::RpWidget::contextMenuEvent(e);
        return;
    }

    const auto previous = std::exchange(_menuRowIndex, index);
    if (previous >= 0 && previous != index) {
        updateRowRect(previous);
    }
    updateRowRect(index);
    showContextMenu(index, e->globalPos());
    e->accept();
}

int DialogsInner::rowAtY(int y) const {
    if (y < 0 || _rows.isEmpty()) {
        return -1;
    }
    // Dialog rows occupy Y: 0 to filteredRowsHeight().
    // Beyond that is the search results section — not a dialog row.
    if (y >= filteredRowsHeight()) {
        return -1;
    }
    const auto index = y / DialogsRow::rowHeight();
    return (index < _rows.size()) ? index : -1;
}

void DialogsInner::setHoveredRow(int index) {
    if (_hoveredIndex == index) {
        return;
    }
    const auto prev = _hoveredIndex;
    _hoveredIndex = index;
    // PointingHandCursor on interactive dialog rows.
    setCursor(index >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    if (prev >= 0) {
        updateRowRect(prev);
    }
    if (index >= 0) {
        updateRowRect(index);
    }
}

void DialogsInner::rebuildAllRowsIndex() {
    _allRowsByRoomId.clear();
    _allRowsByRoomId.reserve(_allRows.size());
    for (int i = 0; i != _allRows.size(); ++i) {
        const auto roomId = _allRows[i].roomId();
        if (!roomId.isEmpty()) {
            _allRowsByRoomId.insert(roomId, i);
        }
    }
}

void DialogsInner::setSelectedRow(int index) {
    const auto sameRow = (_selectedIndex == index);
    if (!sameRow) {
        const auto prev = _selectedIndex;
        _selectedIndex = index;
        _selectedRoomId = (index >= 0 && index < _rows.size())
            ? _rows[index].roomId()
            : QString();

        if (prev >= 0) {
            updateRowRect(prev);
        }
        if (index >= 0) {
            updateRowRect(index);
        }
    }

    // Fire callback even for the already-selected row
    // so clicking the current room scrolls to unread/bottom.
    if (_roomSelectedCallback && !_selectedRoomId.isEmpty()) {
        _roomSelectedCallback(_selectedRoomId);
    }
}

void DialogsInner::selectRoomById(const QString &roomId) {
    // Clear active folder filter if the room isn't in the current filtered view.
    bool found = false;
    for (int i = 0; i < _rows.size(); ++i) {
        if (_rows[i].roomId() == roomId) {
            found = true;
            const auto prev = _selectedIndex;
            _selectedIndex = i;
            _selectedRoomId = roomId;
            if (prev >= 0) {
                updateRowRect(prev);
            }
            updateRowRect(i);
            break;
        }
    }
    if (!found) {
        // Room not in current filter — switch to "All Chats" and retry.
        if (_activeFilterId != 0) {
            setActiveFilter(0);
            for (int i = 0; i < _rows.size(); ++i) {
                if (_rows[i].roomId() == roomId) {
                    _selectedIndex = i;
                    _selectedRoomId = roomId;
                    updateRowRect(i);
                    break;
                }
            }
        } else {
            // Room not found at all — just update the selected ID.
            const auto prev = _selectedIndex;
            _selectedIndex = -1;
            _selectedRoomId = roomId;
            if (prev >= 0) {
                updateRowRect(prev);
            }
        }
    }
}

void DialogsInner::clearSelection() {
    if (_selectedIndex < 0) {
        return;
    }
    const auto prev = _selectedIndex;
    _selectedIndex = -1;
    _selectedRoomId.clear();
    updateRowRect(prev);
}

void DialogsInner::updateRowRect(int index) {
    if (index >= 0 && index < _rows.size()) {
        update(0, index * DialogsRow::rowHeight(), width(), DialogsRow::rowHeight());
    }
}

void DialogsInner::showContextMenu(int rowIndex, const QPoint &globalPos) {
    if (rowIndex < 0 || rowIndex >= _rows.size()) {
        return;
    }

    if (_contextMenu) {
        _contextMenu->close();
        _contextMenu->deleteLater();
        _contextMenu = nullptr;
    }

    const auto &row = _rows[rowIndex];
    const auto roomId = row.roomId();
    if (roomId.isEmpty()) {
        return;
    }

    auto *menu = HistoryPopupMenuStyle::createStyledMenu(
        this,
        HistoryPopupMenuStyle::Variant::WithIcons);
    const auto addAction = [menu](
            const QString &label,
            const QString &icon,
            const std::function<void()> &slot,
            bool attention = false) {
        auto *action = menu->addAction(label);
        HistoryPopupMenuStyle::setActionIconName(action, icon);
        if (attention) {
            action->setProperty("_tm_attention", true);
        }
        if (slot) {
            QObject::connect(action, &QAction::triggered, menu, slot);
        }
        return action;
    };

    const auto isUnread = row.hasUnreadIndicator();
    addAction(
        row.isPinned() ? tr("Unpin from Top") : tr("Pin to Top"),
        row.isPinned() ? QStringLiteral("unpin") : QStringLiteral("pin"),
        [this, roomId, pinned = !row.isPinned()] {
            setRoomPinned(roomId, pinned);
            emit pinRoomRequested(roomId, pinned);
        });
    if (roomId == _savedMessagesRoomId) {
        // Saved Messages is muted by design; no mute controls.
    } else if (row.notificationMode() != RoomNotificationMode::AllMessages) {
        addAction(
            tr("Unmute"),
            QStringLiteral("unmute"),
            [this, roomId] {
                setRoomNotificationMode(roomId, RoomNotificationMode::AllMessages);
                emit roomNotificationModeRequested(roomId, RoomNotificationMode::AllMessages);
            });
    } else {
        // Not muted: "Mute" opens a submenu to pick the mute mode (nested submenu,
        // mirroring "Add to Folder" below). Both are "muting" — the
        // muted branch above collapses back to a single "Unmute".
        auto *muteMenu = HistoryPopupMenuStyle::createStyledMenu(
            menu,
            HistoryPopupMenuStyle::Variant::WithIcons);
        auto *mentionsAction = muteMenu->addAction(
            tr("Except mentions and keywords"), [this, roomId] {
                setRoomNotificationMode(roomId, RoomNotificationMode::MentionsOnly);
                emit roomNotificationModeRequested(roomId, RoomNotificationMode::MentionsOnly);
            });
        HistoryPopupMenuStyle::setActionIconName(
            mentionsAction, QStringLiteral("mute_mentions"));
        auto *allMessagesAction = muteMenu->addAction(tr("All messages"), [this, roomId] {
            setRoomNotificationMode(roomId, RoomNotificationMode::Mute);
            emit roomNotificationModeRequested(roomId, RoomNotificationMode::Mute);
        });
        HistoryPopupMenuStyle::setActionIconName(
            allMessagesAction, QStringLiteral("mute"));
        allMessagesAction->setProperty("_tm_attention", true);
        auto *muteAction = addAction(
            tr("Mute"),
            QStringLiteral("mute"),
            std::function<void()>());
        menu->setSubmenu(muteAction, muteMenu);
    }
    addAction(
        isUnread ? tr("Mark as Read") : tr("Mark as Unread"),
        isUnread ? QStringLiteral("read") : QStringLiteral("unread"),
        [this, roomId, read = isUnread] {
            emit markReadRequested(roomId, read);
        });

    auto *foldersMenu = HistoryPopupMenuStyle::createStyledMenu(
        menu,
        HistoryPopupMenuStyle::Variant::Folders);
    for (const auto &folder : _folders) {
        // Spaces share the _folders vector but are not tag-folders — never offer
        // them in the room's "Add to folder" submenu.
        if (folder.isSpace || folder.filterId <= 2 || folder.displayName.isEmpty()) {
            continue;
        }
        auto *folderAction = foldersMenu->addAction(folder.displayName, [this, roomId, id = folder.filterId] {
            emit addToFolderRequested(roomId, id);
        });
        folderAction->setCheckable(true);
        folderAction->setChecked(row.filterIds().contains(folder.filterId));
        HistoryPopupMenuStyle::setActionRightIconName(
            folderAction,
            folderMenuIconName(folder));
    }
    if (foldersMenu->actions().size() > 0) {
        foldersMenu->addSeparator();
    }
    auto *createFolderAction = foldersMenu->addAction(
        tr("Create new folder"), [this, roomId] {
        emit createFolderRequested(roomId);
    });
    HistoryPopupMenuStyle::setActionIconName(
        createFolderAction,
        QStringLiteral("menu/show_in_folder"));
    {
        auto *addToFolder = addAction(
            tr("Add to Folder"),
            QStringLiteral("add_to_folder"),
            std::function<void()>());
        menu->setSubmenu(addToFolder, foldersMenu);
    }

    // Saved Messages cannot be left from the UI (tdesktop semantics); its
    // destructive action is "delete" — clear history and hide until reopened.
    if (roomId != _savedMessagesRoomId) {
        menu->addSeparator();
        addAction(
            tr("Leave Room"),
            QStringLiteral("leave"),
            [this, roomId] {
                emit leaveRoomRequested(roomId);
            },
            true);
    } else {
        menu->addSeparator();
        addAction(
            tr("Delete chat"),
            QStringLiteral("delete"),
            [this] {
                emit deleteSavedMessagesRequested();
            },
            true);
    }

    _contextMenu = menu;
    QObject::connect(menu, &HistoryPopupMenuStyle::PopupMenu::aboutToHide, this, [this, menu] {
        if (_contextMenu != menu) {
            return;
        }
        _contextMenu = nullptr;
        const auto previous = std::exchange(_menuRowIndex, -1);
        if (previous >= 0) {
            updateRowRect(previous);
        }
    });
    menu->popup(globalPos);
    QObject::connect(menu, &HistoryPopupMenuStyle::PopupMenu::aboutToHide, menu, &QObject::deleteLater);
}

QVector<QString> DialogsInner::pinnedRoomIds() const {
    QVector<std::pair<int, QString>> pinned;
    for (const auto &row : _allRows) {
        if (row.isPinned()) {
            pinned.append({row.pinnedIndex(), row.roomId()});
        }
    }
    std::sort(pinned.begin(), pinned.end(), [](const auto &a, const auto &b) {
        return a.first < b.first;
    });
    QVector<QString> ids;
    ids.reserve(pinned.size());
    for (const auto &[idx, id] : pinned) {
        ids.append(id);
    }
    return ids;
}

bool DialogsInner::hasPinnedRoomsWithoutOrder() const {
    return std::any_of(_allRows.begin(), _allRows.end(), [](const DialogsRow &row) {
        return row.isPinned() && row.pinnedOrder() < 0.0;
    });
}

void DialogsInner::applyPinnedOrder(const QVector<QString> &savedIds) {
    // The arrangement the user chose lives in each room's m.favourite tag, on the
    // server — that is what has to win, or the order dies with the local settings
    // file at the next logout. `savedIds` is only a fallback for rooms pinned before
    // the app recorded an order; those get published to the server once (see
    // DialogsWidget), after which they have one and this branch stops mattering.
    QVector<const DialogsRow *> pinned;
    for (const auto &row : _allRows) {
        if (row.isPinned()) {
            pinned.append(&row);
        }
    }
    std::sort(pinned.begin(), pinned.end(), [&savedIds](const DialogsRow *a, const DialogsRow *b) {
        const auto rank = [&savedIds](const DialogsRow *row) {
            if (row->pinnedOrder() >= 0.0) {
                return 0; // the server knows where this one goes
            }
            return savedIds.contains(row->roomId()) ? 1 : 2;
        };
        const auto aRank = rank(a);
        const auto bRank = rank(b);
        if (aRank != bRank) {
            return aRank < bRank;
        }
        if (aRank == 0) {
            return a->pinnedOrder() < b->pinnedOrder();
        }
        if (aRank == 1) {
            return savedIds.indexOf(a->roomId()) < savedIds.indexOf(b->roomId());
        }
        return a->timestamp() > b->timestamp();
    });

    QHash<QString, int> indexByRoom;
    indexByRoom.reserve(pinned.size());
    for (int i = 0; i < pinned.size(); ++i) {
        indexByRoom.insert(pinned[i]->roomId(), i + 1);
    }
    for (auto &row : _allRows) {
        row.setPinnedIndex(row.isPinned() ? indexByRoom.value(row.roomId(), 0) : 0);
    }

    std::stable_sort(_allRows.begin(), _allRows.end(), [](const DialogsRow &a, const DialogsRow &b) {
        if (a.isPinned() != b.isPinned()) {
            return a.isPinned();
        }
        if (a.isPinned() && b.isPinned()) {
            return a.pinnedIndex() < b.pinnedIndex();
        }
        // Invites above regular rooms (but below pinned).
        const auto aInvite = (a.membership() == MembershipState::Invite);
        const auto bInvite = (b.membership() == MembershipState::Invite);
        if (aInvite != bInvite) return aInvite;
        return a.timestamp() > b.timestamp();
    });
    rebuildAllRowsIndex();
    applyFilters();
}

void DialogsInner::applySavedFolderAssignments(const QMap<QString, QVector<int>> &assignments) {
    for (auto &row : _allRows) {
        auto it = assignments.constFind(row.roomId());
        if (it != assignments.constEnd()) {
            auto ids = row.filterIds();
            for (const auto fid : it.value()) {
                if (fid > 2 && !ids.contains(fid)) {
                    ids.append(fid);
                }
            }
            std::sort(ids.begin(), ids.end());
            row.setFilterIds(ids);
        }
    }
}

void DialogsInner::setSearchResults(const SearchPage &page) {
    _searchResults = page.hits;
    _searchHoveredIndex = -1;
    _searchSelectedIndex = -1;
    _searchTotalApprox = page.totalApprox;
    _searchDone = page.done || page.nextToken.isEmpty();
    _searchResolved = true;
    _searchE2eeDisabled = page.e2eeDisabled;
    _searchIndexing = page.indexing;
    updateSearchResultsSize();
}

void DialogsInner::appendSearchResults(const SearchPage &page) {
    _searchResults.append(page.hits);
    _searchTotalApprox = page.totalApprox;
    _searchDone = page.done || page.nextToken.isEmpty();
    _searchResolved = true;
    _searchE2eeDisabled = page.e2eeDisabled;
    _searchIndexing = page.indexing;
    updateSearchResultsSize();
}

void DialogsInner::clearSearchResults() {
    _searchResults.clear();
    _searchHoveredIndex = -1;
    _searchSelectedIndex = -1;
    _searchTotalApprox = 0;
    _searchDone = true;
    _searchResolved = false;
    _searchE2eeDisabled = false;
    _searchIndexing = false;
    updateSearchResultsSize();
}

void DialogsInner::updateSearchResultsSize() {
    updateContentSize();
    update();
}

int DialogsInner::selectNextSearchResult() {
    if (_searchResults.isEmpty()) {
        return -1;
    }
    if (_searchSelectedIndex < 0) {
        _searchSelectedIndex = 0;
    } else if (_searchSelectedIndex < _searchResults.size() - 1) {
        ++_searchSelectedIndex;
    } else if (!_searchDone) {
        emit loadMoreSearchResults();
        return _searchSelectedIndex;
    }
    update();

    // Auto-click the selected result.
    if (_searchSelectedIndex >= 0 && _searchSelectedIndex < _searchResults.size()) {
        const auto &hit = _searchResults[_searchSelectedIndex];
        emit searchResultClicked(hit.roomId, hit.eventId);
    }
    return _searchSelectedIndex;
}

int DialogsInner::selectPreviousSearchResult() {
    if (_searchResults.isEmpty()) {
        return -1;
    }
    if (_searchSelectedIndex > 0) {
        --_searchSelectedIndex;
    } else {
        _searchSelectedIndex = 0;
    }
    update();

    if (_searchSelectedIndex >= 0 && _searchSelectedIndex < _searchResults.size()) {
        const auto &hit = _searchResults[_searchSelectedIndex];
        emit searchResultClicked(hit.roomId, hit.eventId);
    }
    return _searchSelectedIndex;
}

int DialogsInner::searchResultRowAtY(int y) const {
    if (_searchLoading || _searchResults.isEmpty()) {
        return -1;
    }
    const auto top = _messageSearchMode
        ? (_messageSearchTopInset + searchResultsHeaderHeight(true))
        : isDefaultSearchVisible()
        ? defaultSearchResultsTop()
        : searchResultsHeaderHeight(true);
    if (y < top) {
        return -1;
    }
    const auto index = (y - top) / st::dialogsRowHeight;
    return (index < _searchResults.size()) ? index : -1;
}

void DialogsInner::paintInitialLoading(QPainter &p, const QRect &clip) {
    if (clip.isEmpty()) {
        return;
    }
    if (!_initialLoadingPaintedOnce) {
        _initialLoadingPaintedOnce = true;
        QMetaObject::invokeMethod(this, [this] {
            emit initialLoadingPainted();
        }, Qt::QueuedConnection);
    }
    paintCenteredSearchMessage(
        p,
        QRect(0, 0, width(), height()),
        tr("Loading..."));
}

void DialogsInner::paintSearchSection(
    QPainter &p,
    const QRect &clip,
    int contentTop,
    bool showAllChatsLabel) {
    const auto w = width();
    const auto query = (_messageSearchMode
        ? _messageSearchQuery
        : _searchQuery).trimmed();
    if (_searchLoading) {
        if (clip.bottom() < contentTop) {
            return;
        }
        p.fillRect(0, contentTop, w, st::searchedBarHeight, st::searchedBarBg);
        p.setFont(st::normalFont);
        p.setPen(st::searchedBarFg);
        const auto metrics = QFontMetrics(static_cast<const QFont &>(st::normalFont));
        p.drawText(
            st::searchedBarPosition.x(),
            contentTop + st::searchedBarPosition.y() + metrics.ascent(),
            tr("Loading..."));
        for (auto i = 0; i != kSearchLoadingRows; ++i) {
            paintLoadingPlaceholderRow(
                p,
                contentTop + st::searchedBarHeight + i * st::dialogsRowHeight,
                w);
        }
        return;
    }
    if (_searchResults.isEmpty()) {
        const auto canShowEmpty = _messageSearchMode || _rows.isEmpty();
        if (canShowEmpty && clip.bottom() >= contentTop) {
            const auto rect = QRect(
                0,
                contentTop,
                w,
                qMax(kSearchEmptyMinHeight, height() - contentTop));
            // When the E2EE room's local index is still backfilling, an empty
            // result can be a "not indexed yet" artifact rather than a true "no
            // results", so append it as a hint under the No Results block. (The
            // flag is only known after a search runs, so the empty-field prompt
            // can't show it.)
            const auto indexingNote = _searchIndexing
                ? tr("This encrypted room is still being indexed. "
                     "Some messages may not appear yet.")
                : QString();
            if (query.isEmpty()) {
                paintCenteredSearchMessage(
                    p,
                    rect,
                    tr("Search for messages"));
            } else if (_searchE2eeDisabled) {
                paintCenteredSearchMessage(
                    p,
                    rect,
                    tr("Search for E2EE rooms is disabled"));
            } else if (_searchResolved) {
                paintCenteredSearchMessage(
                    p,
                    rect,
                    tr("No Results"),
                    tr("There were no results for \"%1\".").arg(query),
                    indexingNote);
            }
        }
        return;
    }

    if (clip.bottom() < contentTop) {
        return;
    }

    const auto headerH = searchResultsHeaderHeight(true);
    const auto resultsTop = contentTop + headerH;
    if (clip.bottom() >= contentTop && clip.top() < resultsTop) {
        p.fillRect(0, contentTop, w, headerH, st::searchedBarBg);
        p.setFont(st::normalFont);
        p.setPen(st::searchedBarFg);
        const auto metrics = QFontMetrics(static_cast<const QFont &>(st::normalFont));
        const auto baseline = contentTop + st::searchedBarPosition.y() + metrics.ascent();
        const auto rightText = showAllChatsLabel
            ? tr("All chats")
            : QString();
        auto leftWidth = w - 2 * st::searchedBarPosition.x();
        if (!rightText.isEmpty()) {
            const auto rightWidth = metrics.horizontalAdvance(rightText);
            p.drawText(
                w - st::searchedBarPosition.x() - rightWidth,
                baseline,
                rightText);
            leftWidth -= rightWidth + st::dialogsDateSkip;
        }
        const auto leftText = metrics.elidedText(
            searchResultsCountText(_searchTotalApprox, _searchResults.size()),
            Qt::ElideRight,
            qMax(0, leftWidth));
        p.drawText(st::searchedBarPosition.x(), baseline, leftText);
    }

    const auto rowH = st::dialogsRowHeight;
    const auto firstVisible = qMax(0, (clip.top() - resultsTop) / rowH);
    const auto lastVisible = (clip.bottom() - resultsTop) / rowH;

    for (int i = firstVisible; i <= lastVisible && i < _searchResults.size(); ++i) {
        const auto &hit = _searchResults[i];
        const auto roomIndex = _allRowsByRoomId.value(hit.roomId, -1);
        const auto roomRow = (roomIndex >= 0 && roomIndex < _allRows.size())
            ? &_allRows[roomIndex]
            : nullptr;
        const auto roomName = (roomRow && !roomRow->displayName().isEmpty())
            ? roomRow->displayName()
            : hit.roomId;
        const auto roomAvatarUrl = roomRow ? roomRow->avatarUrl() : QString();
        const auto rowY = resultsTop + i * rowH;
        const auto isHovered = (i == _searchHoveredIndex);
        const auto isSelected = (i == _searchSelectedIndex);
        const auto nameColor = isSelected
            ? st::dialogsNameFgActive
            : isHovered
            ? st::dialogsNameFgOver
            : st::dialogsNameFg;
        const auto dateColor = isSelected
            ? st::dialogsDateFgActive
            : isHovered
            ? st::dialogsDateFgOver
            : st::dialogsDateFg;
        const auto baseSnippetColor = isSelected
            ? st::dialogsTextFgActive
            : isHovered
            ? st::dialogsTextFgOver
            : st::dialogsTextFg;
        const auto highlightColor = isSelected
            ? st::dialogsTextFgServiceActive
            : isHovered
            ? st::dialogsTextFgServiceOver
            : st::dialogsTextFgService;

        if (isSelected) {
            p.fillRect(0, rowY, w, rowH, st::dialogsBgActive);
        } else if (isHovered) {
            p.fillRect(0, rowY, w, rowH, st::dialogsBgOver);
        }

        const auto photoLeft = st::dialogsPadding.left();
        const auto photoTop = rowY + st::dialogsPadding.top();
        paintSearchAvatar(
            p,
            roomName,
            roomAvatarUrl,
            hit.roomId,
            photoLeft,
            photoTop,
            st::dialogsPhotoSize,
            this);

        const auto textLeft = st::dialogsTextLeft;
        const auto nameTop = rowY + st::dialogsNameTop;
        auto rectForName = QRect(
            st::dialogsNameLeft,
            nameTop,
            w - st::dialogsNameLeft - st::dialogsPadding.right(),
            st::semiboldFont->height);

        const auto dateText = formatSearchResultDate(hit.timestamp);
        if (!dateText.isEmpty()) {
            const auto dateMetrics = QFontMetrics(static_cast<const QFont &>(st::dialogsDateFont));
            const auto dateWidth = dateMetrics.horizontalAdvance(dateText);
            rectForName.setWidth(rectForName.width() - dateWidth - st::dialogsDateSkip);
            p.setPen(dateColor);
            p.setFont(st::dialogsDateFont);
            p.drawText(
                rectForName.left() + rectForName.width() + st::dialogsDateSkip,
                nameTop + st::semiboldFont->height - st::normalFont->descent,
                dateText);
        }

        p.setFont(st::semiboldFont);
        const auto metrics = QFontMetrics(st::semiboldFont);
        const auto dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
        if (roomRow && !roomRow->isDirect()) {
            const auto useChannelIcon = roomRow->lastSender().isEmpty();
            const auto icon = loadDialogsMaskIcon(
                useChannelIcon ? QStringLiteral("channel_type") : QStringLiteral("chat_type"),
                dpr,
                isSelected
                    ? st::dialogsChatIconFgActive
                    : isHovered
                    ? st::dialogsChatIconFgOver
                    : st::dialogsChatIconFg);
            if (!icon.isNull()) {
                const auto iconW = qRound(icon.width() / icon.devicePixelRatio());
                const auto iconH = qRound(icon.height() / icon.devicePixelRatio());
                const auto iconY = rectForName.top() + (st::semiboldFont->height - iconH) / 2;
                p.drawImage(QPoint(rectForName.left(), iconY), icon);
                rectForName.setLeft(rectForName.left() + iconW + kSearchResultChatTypeSkip);
            }
        }
        p.setPen(nameColor);
        TeleMatrix::EmojiText::DrawElided(
            p,
            rectForName.left(),
            nameTop + metrics.ascent(),
            rectForName.width(),
            roomName,
            TeleMatrix::EmojiText::CachedMetricsFor(
                st::semiboldFont,
                st::emojiInlineSlot,
                st::emojiInlineGlyph));

        const auto textTop = rowY + st::dialogsTextTop;
        const auto snippetMetrics = QFontMetrics(st::normalFont);
        const auto snippetWidth = w - textLeft - st::dialogsPadding.right();
        drawHighlightedSnippet(
            p,
            textLeft,
            textTop + snippetMetrics.ascent(),
            snippetWidth,
            hit.snippet,
            query,
            baseSnippetColor,
            highlightColor);
    }
}

void DialogsInner::updateOnlineStatus(const QString &userId, int state) {
    // Update _allRows so the presence survives filter rebuilds.
    for (auto &allRow : _allRows) {
        if (allRow.isDirect() && allRow.avatarEntityId() == userId) {
            allRow.setPeerPresence(state);
        }
    }

    for (int i = 0; i < _rows.size(); ++i) {
        auto &row = _rows[i];
        if (!row.isDirect()) continue;
        if (row.avatarEntityId() != userId) continue;

        const auto wasOnline = row.isPeerOnline();
        row.setPeerPresence(state);
        const auto isOnline = row.isPeerOnline();

        if (wasOnline == isOnline) return;

        // Animate the badge (150ms scale).
        const auto roomId = row.roomId();
        auto *anim = _onlineBadgeAnims.value(roomId);
        if (!anim) {
            anim = new QVariantAnimation(this);
            anim->setDuration(150);
            anim->setEasingCurve(QEasingCurve::InOutSine);
            _onlineBadgeAnims.insert(roomId, anim);
            connect(anim, &QVariantAnimation::valueChanged, this, [this, roomId](const QVariant &v) {
                // Find the row by roomId and update progress.
                for (int j = 0; j < _rows.size(); ++j) {
                    if (_rows[j].roomId() == roomId) {
                        _rows[j].setOnlineBadgeProgress(v.toReal());
                        // Minimal repaint: badge corner area of this row.
                        const int rowY = j * DialogsRow::rowHeight();
                        update(QRect(
                            10 + 46 - 10 - 2,   // paddingLeft + photoSize - badgeSize - stroke
                            rowY + 8 + 46 - 10 - 2 - 2, // rowY + paddingTop + photoSize - badgeSize - stroke - skipY
                            14, 14));  // badgeSize + 2*stroke
                        break;
                    }
                }
            });
        }
        anim->stop();
        anim->setStartValue(row.onlineBadgeProgress());
        anim->setEndValue(isOnline ? 1.0 : 0.0);
        anim->start();
        return;
    }
}

} // namespace TeleMatrix

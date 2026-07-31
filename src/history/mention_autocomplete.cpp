// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "mention_autocomplete.h"

#include "ui/text/emoji_text.h"

#include <functional>

#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>

#include "protocol/media_cache.h"
#include "styles/style_constants.h"
#include "ui/empty_userpic.h"
#include "ui/painter.h"
#include "ui/widgets/scroll_area.h"

namespace TeleMatrix {

namespace {

// Max visible rows = 4.5 before scrolling.
constexpr int kMaxVisibleRows = 4;

// Gap between name text end and username start.
constexpr int kNameUsernameGap = 8;
constexpr auto kRoomMentionText = "@room";

// st::mentionHeight is a runtime-scaled value, so this can't be constexpr.
[[nodiscard]] int MaxPopupHeight() {
    return kMaxVisibleRows * st::mentionHeight + st::mentionHeight / 2;
}

} // namespace

// ─── Inner: paint-based row list ─────────────────────────────────────────────

class MentionAutocomplete::Inner final : public QWidget {
public:
    using ChosenCallback = std::function<void(
        const QString &userId,
        const QString &displayName,
        bool roomMention)>;

    explicit Inner(QWidget *parent);

    void setChosenCallback(ChosenCallback callback) {
        _chosen = std::move(callback);
    }

    void setMembers(const QVector<UserProfile> &members);
    void updateFilter(const QString &query);
    [[nodiscard]] int rowCount() const { return int(_filtered.size()); }
    [[nodiscard]] int selectedIndex() const { return _selected; }
    bool moveSelection(int delta);
    bool chooseSelected();

protected:
    void paintEvent(QPaintEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *) override;

private:
    enum class MentionKind {
        User,
        Room,
    };

    int rowAtY(int y) const;

    struct MentionRow {
        MentionKind kind = MentionKind::User;
        QString userId;
        QString displayName;
        QString avatarUrl;
        // Lowercased match keys, precomputed in setMembers so updateFilter does
        // no per-keystroke string work per member. (Unused for Room rows.)
        QString nameLower;
        QString localLower;
    };
    QVector<MentionRow> _allMembers;
    QVector<MentionRow> _filtered;
    QString _query;
    int _selected = -1;
    int _hovered = -1;
    ChosenCallback _chosen;
};

MentionAutocomplete::Inner::Inner(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
}

void MentionAutocomplete::Inner::setMembers(
        const QVector<UserProfile> &members) {
    _allMembers.clear();
    _allMembers.reserve(members.size());
    for (const auto &m : members) {
        // Extract local part once: @localpart:server → localpart.
        auto localPart = m.userId;
        if (localPart.startsWith(QLatin1Char('@'))) {
            localPart = localPart.mid(1);
        }
        const auto colonIdx = localPart.indexOf(QLatin1Char(':'));
        if (colonIdx > 0) {
            localPart = localPart.left(colonIdx);
        }
        _allMembers.append({
            .kind = MentionKind::User,
            .userId = m.userId,
            .displayName = m.displayName,
            .avatarUrl = m.avatarUrl,
            .nameLower = m.displayName.toLower(),
            .localLower = localPart.toLower(),
        });
    }
    // A new member set invalidates any incremental-narrow basis.
    _query.clear();
}

void MentionAutocomplete::Inner::updateFilter(const QString &query) {
    const auto needle = query.toLower();
    const auto prevNeedle = _query.toLower();
    _query = query;
    _hovered = -1;

    const auto matchesRoom = needle.isEmpty()
        || QStringLiteral("room").startsWith(needle);
    const auto matchesMember = [&needle](const MentionRow &m) {
        return needle.isEmpty()
            || m.nameLower.contains(needle)
            || m.localLower.startsWith(needle);
    };

    // Typing forward (the new query extends the previous one) can only shrink
    // the match set — `contains`/`startsWith` are monotonic under prefix
    // extension — so rescan the previous results instead of every member.
    const bool narrow = !prevNeedle.isEmpty()
        && needle.startsWith(prevNeedle)
        && !_filtered.isEmpty();
    if (narrow) {
        const auto previous = _filtered;
        _filtered.clear();
        for (const auto &m : previous) {
            if (m.kind == MentionKind::Room) {
                if (matchesRoom) {
                    _filtered.append(m);
                }
            } else if (matchesMember(m)) {
                _filtered.append(m);
            }
        }
    } else {
        _filtered.clear();
        if (matchesRoom) {
            _filtered.append({
                .kind = MentionKind::Room,
                .userId = QString(),
                .displayName = QString::fromLatin1(kRoomMentionText),
                .avatarUrl = QString(),
            });
        }
        for (const auto &m : _allMembers) {
            if (matchesMember(m)) {
                _filtered.append(m);
            }
        }
    }

    _selected = _filtered.isEmpty() ? -1 : 0;
    update();
}

bool MentionAutocomplete::Inner::moveSelection(int delta) {
    if (_filtered.isEmpty()) return false;
    int next = _selected + delta;
    if (next < 0) next = 0;
    if (next >= _filtered.size()) next = _filtered.size() - 1;
    if (next != _selected) {
        _selected = next;
        update();
    }
    return true;
}

bool MentionAutocomplete::Inner::chooseSelected() {
    if (_selected < 0 || _selected >= _filtered.size()) return false;
    const auto &row = _filtered[_selected];
    if (_chosen) {
        _chosen(row.userId, row.displayName, row.kind == MentionKind::Room);
    }
    return true;
}

int MentionAutocomplete::Inner::rowAtY(int y) const {
    if (y < 0 || _filtered.isEmpty()) return -1;
    const int row = y / st::mentionHeight;
    return (row < _filtered.size()) ? row : -1;
}

void MentionAutocomplete::Inner::paintEvent(QPaintEvent *e) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const auto clip = e->rect();
    p.fillRect(clip, st::mentionBg);

    const auto &pad = st::mentionPadding;
    const auto nameFont = st::semiboldFont;
    const auto usernameFont = st::baseFont(st::fsize, false);

    for (int i = 0; i < _filtered.size(); ++i) {
        const auto &row = _filtered[i];
        const int rowY = i * st::mentionHeight;

        // Cull rows outside the exposed region. Avatars are loaded lazily in the
        // paint, so skipping offscreen rows matters for large member lists.
        if (rowY + st::mentionHeight <= clip.top() || rowY > clip.bottom()) {
            continue;
        }

        const bool isOver = (i == _hovered || i == _selected);
        if (isOver) {
            p.fillRect(0, rowY, width(), st::mentionHeight, st::mentionBgOver);
        }

        if (row.kind == MentionKind::Room) {
            const auto mentionFont = st::baseFont(st::fsize, false);
            const auto mentionMetrics = QFontMetrics(mentionFont);
            const int mentionLeft = st::mentionPadding.left();
            const int textY = rowY + st::mentionTop + mentionMetrics.ascent();
            const auto needle = _query.toLower();

            QString first;
            QString second = QString::fromLatin1(kRoomMentionText);
            if (!needle.isEmpty() && QStringLiteral("room").startsWith(needle)) {
                const auto prefixLen = qBound(1, needle.size() + 1, second.size());
                first = second.left(prefixLen);
                second = second.mid(prefixLen);
            }

            p.setFont(mentionFont);
            int x = mentionLeft;
            if (!first.isEmpty()) {
                p.setPen(isOver ? st::mentionFgOverActive : st::mentionFgActive);
                p.drawText(x, textY, first);
                x += mentionMetrics.horizontalAdvance(first);
            }
            if (!second.isEmpty()) {
                p.setPen(isOver ? st::mentionFgOver : st::mentionFg);
                p.drawText(x, textY, second);
            }
            continue;
        }

        // Avatar: x=pad.left, y=rowY+pad.top, size=mentionPhotoSize.
        const int avatarX = pad.left();
        const int avatarY = rowY + pad.top();
        bool paintedAvatar = false;
        if (!row.avatarUrl.isEmpty()) {
            const auto dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
            const auto avatarPix = MediaCache::loadAvatarPixmap(
                row.avatarUrl, st::mentionPhotoSize, dpr);
            if (!avatarPix.isNull()) {
                p.drawPixmap(avatarX, avatarY, avatarPix);
                paintedAvatar = true;
            }
        }
        if (!paintedAvatar) {
            const auto name = row.displayName.isEmpty()
                ? row.userId : row.displayName;
            Ui::EmptyUserpic::paint(
                p, row.userId, name,
                avatarX, avatarY, st::mentionPhotoSize);
        }

        // Name: x = 2*pad.left + photoSize = 49, y = rowY + mentionTop.
        const int nameX = 2 * pad.left() + st::mentionPhotoSize;
        const int textY = rowY + st::mentionTop;

        p.setFont(nameFont);
        p.setPen(isOver ? st::mentionNameFgOver : st::mentionNameFg);

        const auto nameMetrics = QFontMetrics(nameFont);
        const auto &nameEmoji = TeleMatrix::EmojiText::CachedMetricsFor(
            nameFont, st::emojiInlineSlot, st::emojiInlineGlyph);
        const int nameWidth = TeleMatrix::EmojiText::DrawLine(
            p, nameX, textY + nameMetrics.ascent(), row.displayName, nameEmoji);

        // Username after name + gap.
        auto localPart = row.userId;
        if (localPart.startsWith(QLatin1Char('@'))) {
            localPart = localPart.mid(1);
        }
        const auto colonIdx = localPart.indexOf(QLatin1Char(':'));
        if (colonIdx > 0) {
            localPart = localPart.left(colonIdx);
        }

        const int usernameX = nameX + nameWidth + kNameUsernameGap;
        const int availableWidth = width() - usernameX - pad.right();
        if (availableWidth > 0 && !localPart.isEmpty()) {
            p.setFont(usernameFont);
            const auto usernameMetrics = QFontMetrics(usernameFont);

            // Matched prefix in active color, rest in muted color.
            const auto needle = _query.toLower();
            const auto localLower = localPart.toLower();
            int matchLen = 0;
            if (!needle.isEmpty() && localLower.startsWith(needle)) {
                matchLen = needle.length();
            }

            const auto usernameDisplay = QStringLiteral("@") + localPart;
            int curX = usernameX;

            if (matchLen > 0) {
                // Draw "@" + matched prefix in active color.
                const auto matchedText = QStringLiteral("@") + localPart.left(matchLen);
                p.setPen(isOver ? st::mentionFgOverActive : st::mentionFgActive);
                p.drawText(curX, textY + usernameMetrics.ascent(), matchedText);
                curX += usernameMetrics.horizontalAdvance(matchedText);

                // Rest in muted color.
                const auto restText = localPart.mid(matchLen);
                if (!restText.isEmpty()) {
                    p.setPen(isOver ? st::mentionFgOver : st::mentionFg);
                    p.drawText(curX, textY + usernameMetrics.ascent(), restText);
                }
            } else {
                // No prefix match — all muted.
                p.setPen(isOver ? st::mentionFgOver : st::mentionFg);
                p.drawText(curX, textY + usernameMetrics.ascent(), usernameDisplay);
            }
        }
    }
}

void MentionAutocomplete::Inner::mouseMoveEvent(QMouseEvent *e) {
    const int row = rowAtY(e->position().toPoint().y());
    if (row != _hovered) {
        _hovered = row;
        update();
    }
}

void MentionAutocomplete::Inner::mousePressEvent(QMouseEvent *e) {
    const int row = rowAtY(e->position().toPoint().y());
    if (row >= 0 && row < _filtered.size()) {
        _selected = row;
        chooseSelected();
    }
}

void MentionAutocomplete::Inner::leaveEvent(QEvent *) {
    if (_hovered >= 0) {
        _hovered = -1;
        update();
    }
}

// ─── MentionAutocomplete: scroll-area popup shell ────────────────────────────

MentionAutocomplete::MentionAutocomplete(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::NoFocus);

    _scroll = new ::Ui::ScrollArea(this);
    _scroll->setFocusPolicy(Qt::NoFocus);
    _scroll->setFrameShape(QFrame::NoFrame);

    _inner = new Inner(_scroll);
    _inner->setChosenCallback([this](
            const QString &userId,
            const QString &displayName,
            bool roomMention) {
        emit mentionChosen(userId, displayName, roomMention);
        hide();
    });
    _scroll->setWidget(_inner);

    hide();
}

void MentionAutocomplete::setMembers(const QVector<UserProfile> &members) {
    _inner->setMembers(members);
}

void MentionAutocomplete::updateFilter(const QString &query) {
    _inner->updateFilter(query);
    const int count = _inner->rowCount();
    if (count == 0) {
        hide();
        return;
    }
    setFixedHeight(qMin(count * st::mentionHeight, MaxPopupHeight()));
    relayout();
    _scroll->scrollToY(0);
    show();
    raise();
}

bool MentionAutocomplete::moveSelection(int delta) {
    if (!_inner->moveSelection(delta)) {
        return false;
    }
    ensureSelectedVisible();
    return true;
}

bool MentionAutocomplete::chooseSelected() {
    return _inner->chooseSelected();
}

void MentionAutocomplete::refresh() {
    if (_inner) {
        _inner->update();
    }
}

void MentionAutocomplete::resizeEvent(QResizeEvent *) {
    relayout();
}

void MentionAutocomplete::relayout() {
    _scroll->setGeometry(0, 0, width(), height());
    const int contentHeight = _inner->rowCount() * st::mentionHeight;
    _inner->resize(_scroll->viewport()->width(), contentHeight);
    _scroll->updateBars();
}

void MentionAutocomplete::ensureSelectedVisible() {
    const int sel = _inner->selectedIndex();
    if (sel < 0) {
        return;
    }
    const int rowTop = sel * st::mentionHeight;
    const int rowBottom = rowTop + st::mentionHeight;
    const int top = _scroll->scrollTop();
    const int viewHeight = _scroll->viewport()->height();
    if (rowTop < top) {
        _scroll->scrollToY(rowTop);
    } else if (rowBottom > top + viewHeight) {
        _scroll->scrollToY(rowBottom - viewHeight);
    }
}

} // namespace TeleMatrix

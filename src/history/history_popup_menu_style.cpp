// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_popup_menu_style.h"

#include <QAction>
#include <QApplication>
#include <QFont>
#include <QFontMetrics>
#include <QHash>
#include <QImage>
#include <QKeyEvent>
#include <QMargins>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QScreen>

#include "styles/style_constants.h"
#include "ui/painter.h"
#include "ui/platform/ui_utility_mac.h"
#include "ui/style/icon_provider.h"

namespace TeleMatrix::HistoryPopupMenuStyle {

namespace {

constexpr auto kActionIconNameProperty = "_telematrix_menu_icon_name";
constexpr auto kActionRightIconNameProperty = "_telematrix_menu_icon_right_name";
constexpr auto kActionAttentionProperty = "_tm_attention";
constexpr auto kPopupRadius = 8;
constexpr auto kMenuItemRightSkip = 6;
constexpr auto kSeparatorLineWidth = 1;
constexpr auto kFoldersRightIconSkip = 17; // st::menuWithIcons.itemPadding.right()
constexpr auto kFoldersRightIconScale = 0.75;
const auto kDefaultItemPadding = QMargins(17, 8, 17, 7); // defaultMenu.itemPadding
const auto kIconsItemPadding = QMargins(54, 8, 17, 8);   // menuWithIcons.itemPadding
const auto kFoldersItemPadding = QMargins(54, 8, 44, 8); // foldersMenu.menu.itemPadding
const auto kMenuIconPosition = QPoint(15, 5); // menuWithIcons.itemIconPosition
const auto kSeparatorPadding = QMargins(0, 5, 0, 5); // defaultMenuSeparator.padding
constexpr auto kFoldersMenuMaxHeight = 320; // foldersMenu.maxHeight

// Rounded 8px shadow: extend = margins(10px, 10px, 10px, 10px).
constexpr auto kShadowExtend = 10;

// Default popup scrollPadding: margins(0px, 8px, 0px, 8px).
// Popup-with-icons scrollPadding: margins(0px, 5px, 0px, 5px).
constexpr auto kScrollPaddingTopDefault = 8;
constexpr auto kScrollPaddingBottomDefault = 8;
constexpr auto kScrollPaddingTopIcons = 5;
constexpr auto kScrollPaddingBottomIcons = 5;

// Default menu widthMin/widthMax.
constexpr auto kWidthMin = 156;
constexpr auto kWidthMax = 300;

[[nodiscard]] int stripHeight() {
    return st::reactStripHeight;
}

[[nodiscard]] int stripCellSize() {
    return st::reactStripSize;
}

[[nodiscard]] int stripEmojiSize() {
    return TeleMatrix::Style::ConvertScale(21);
}

[[nodiscard]] int stripSkipX() {
    return st::reactStripSkip;
}

[[nodiscard]] int stripRadius() {
    return stripHeight() / 2;
}

[[nodiscard]] int stripBubbleTriangleHeight() {
    return TeleMatrix::Style::ConvertScale(6);
}

[[nodiscard]] int stripTotalHeight() {
    return stripHeight() + stripBubbleTriangleHeight();
}

[[nodiscard]] int stripMenuGap() {
    return TeleMatrix::Style::ConvertScale(4);
}

[[nodiscard]] qreal stripExpandCircleSize() {
    return TeleMatrix::Style::ConvertScale(24);
}

[[nodiscard]] qreal stripChevronWidth() {
    return TeleMatrix::Style::ConvertScale(8);
}

[[nodiscard]] qreal stripChevronHeight() {
    return TeleMatrix::Style::ConvertScale(5);
}

[[nodiscard]] qreal stripChevronPenWidth() {
    return qMax(1, TeleMatrix::Style::ConvertScale(2));
}

// --------------------------------------------------------
// Shadow tile loading and painting.
// --------------------------------------------------------

struct ShadowTiles {
    QImage left;
    QImage topLeft;
    QImage top;
    QImage topRight;
    QImage right;
    QImage bottomLeft;
    QImage bottom;
    QImage bottomRight;
};

[[nodiscard]] ShadowTiles loadShadowTiles(qreal dpr) {
    static QHash<int, ShadowTiles> cache;
    const auto key = dpr >= 2.5 ? 3 : (dpr >= 1.5 ? 2 : 1);
    if (const auto i = cache.constFind(key); i != cache.cend()) {
        return i.value();
    }

    const auto base = QStringLiteral(":/telematrix/icons/shadow/");
    auto load = [&](const QString &name) -> QImage {
        const auto mask = TeleMatrix::Style::IconProvider::loadScaledMask(base, name);
        return TeleMatrix::Style::IconProvider::colorizeMask(mask, st::windowShadowFg);
    };

    ShadowTiles tiles;
    tiles.left = load(QStringLiteral("round_shadow_box_left"));
    tiles.topLeft = load(QStringLiteral("round_shadow_box_top_left"));
    tiles.top = load(QStringLiteral("round_shadow_box_top"));
    tiles.bottomLeft = load(QStringLiteral("round_shadow_box_bottom_left"));
    tiles.bottom = load(QStringLiteral("round_shadow_box_bottom"));
    tiles.topRight = tiles.topLeft.flipped(Qt::Horizontal);
    tiles.topRight.setDevicePixelRatio(tiles.topLeft.devicePixelRatio());
    tiles.right = tiles.left.flipped(Qt::Horizontal);
    tiles.right.setDevicePixelRatio(tiles.left.devicePixelRatio());
    tiles.bottomRight = tiles.bottomLeft.flipped(Qt::Horizontal);
    tiles.bottomRight.setDevicePixelRatio(tiles.bottomLeft.devicePixelRatio());

    cache.insert(key, tiles);
    return tiles;
}

void paintShadow(QPainter &p, const QRect &box, int outerWidth, const ShadowTiles &st) {
    const auto ratio = st.topLeft.devicePixelRatio();
    auto logicalW = [ratio](const QImage &img) {
        return qRound(img.width() / ratio);
    };
    auto logicalH = [ratio](const QImage &img) {
        return qRound(img.height() / ratio);
    };

    const auto tlW = logicalW(st.topLeft);
    const auto tlH = logicalH(st.topLeft);
    const auto trW = logicalW(st.topRight);
    const auto trH = logicalH(st.topRight);
    const auto blW = logicalW(st.bottomLeft);
    const auto blH = logicalH(st.bottomLeft);
    const auto brW = logicalW(st.bottomRight);
    const auto brH = logicalH(st.bottomRight);
    const auto leftW = logicalW(st.left);
    const auto rightW = logicalW(st.right);
    const auto topH = logicalH(st.top);
    const auto bottomH = logicalH(st.bottom);

    // Left edge.
    {
        auto from = box.y();
        auto to = box.y() + box.height();
        p.drawImage(
            box.x() - kShadowExtend,
            box.y() - kShadowExtend,
            st.topLeft);
        from += tlH - kShadowExtend;
        p.drawImage(
            box.x() - kShadowExtend,
            box.y() + box.height() + kShadowExtend - blH,
            st.bottomLeft);
        to -= blH - kShadowExtend;
        if (to > from) {
            p.drawImage(
                QRect(box.x() - kShadowExtend, from, leftW, to - from),
                st.left,
                QRect(0, 0, st.left.width(), st.left.height()));
        }
    }

    // Right edge.
    {
        auto from = box.y();
        auto to = box.y() + box.height();
        p.drawImage(
            box.x() + box.width() + kShadowExtend - trW,
            box.y() - kShadowExtend,
            st.topRight);
        from += trH - kShadowExtend;
        p.drawImage(
            box.x() + box.width() + kShadowExtend - brW,
            box.y() + box.height() + kShadowExtend - brH,
            st.bottomRight);
        to -= brH - kShadowExtend;
        if (to > from) {
            p.drawImage(
                QRect(
                    box.x() + box.width() + kShadowExtend - rightW,
                    from,
                    rightW,
                    to - from),
                st.right,
                QRect(0, 0, st.right.width(), st.right.height()));
        }
    }

    // Top edge.
    {
        auto from = box.x();
        auto to = box.x() + box.width();
        from += tlW - kShadowExtend;
        to -= trW - kShadowExtend;
        if (to > from) {
            p.drawImage(
                QRect(from, box.y() - kShadowExtend, to - from, topH),
                st.top,
                QRect(0, 0, st.top.width(), st.top.height()));
        }
    }

    // Bottom edge.
    {
        auto from = box.x();
        auto to = box.x() + box.width();
        from += blW - kShadowExtend;
        to -= brW - kShadowExtend;
        if (to > from) {
            p.drawImage(
                QRect(
                    from,
                    box.y() + box.height() + kShadowExtend - bottomH,
                    to - from,
                    bottomH),
                st.bottom,
                QRect(0, 0, st.bottom.width(), st.bottom.height()));
        }
    }
}

} // namespace

// ============================================================
// PopupMenu implementation.
// ============================================================

PopupMenu::PopupMenu(Variant variant, QWidget *parent)
    : QWidget(parent)
    , _variant(variant) {
    setWindowFlags(
        Qt::Tool
        | Qt::FramelessWindowHint
        | Qt::NoDropShadowWindowHint
        | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
}

const QStringList &PopupMenu::stripEmoji() {
    // The fast (default) reactions, in Telegram's canonical order.
    static const QStringList list = {
        QString::fromUtf8("\xF0\x9F\x91\x8D"),         // 👍 thumbs up
        QString::fromUtf8("\xF0\x9F\x91\x8E"),         // 👎 thumbs down
        QString::fromUtf8("\xE2\x9D\xA4\xEF\xB8\x8F"), // ❤️ red heart
        QString::fromUtf8("\xF0\x9F\x94\xA5"),         // 🔥 fire
        QString::fromUtf8("\xF0\x9F\xA5\xB0"),         // 🥰 smiling face with hearts
        QString::fromUtf8("\xF0\x9F\x91\x8F"),         // 👏 clapping hands
        QString::fromUtf8("\xF0\x9F\x98\x81"),         // 😁 beaming face
    };
    return list;
}

void PopupMenu::setReactionStrip(const QString &eventId) {
    _hasReactionStrip = true;
    _reactionEventId = eventId;
    recalculateLayout();
}

PopupMenu::~PopupMenu() {
    if (!_parentMenu) {
        qApp->removeEventFilter(this);
    }
}

// --------------------------------------------------------
// Action management.
// --------------------------------------------------------

QAction *PopupMenu::addAction(const QString &text) {
    auto *action = new QAction(text, this);
    _items.append({ action, false, nullptr, {}, 0, {} });
    return action;
}

QAction *PopupMenu::addAction(const QString &text, std::function<void()> callback) {
    auto *action = new QAction(text, this);
    _items.append({ action, false, nullptr, {}, 0, std::move(callback) });
    return action;
}

void PopupMenu::addSeparator() {
    auto *action = new QAction(this);
    action->setSeparator(true);
    _items.append({ action, true, nullptr, {}, 0, {} });
}

void PopupMenu::setSubmenu(QAction *action, PopupMenu *submenu) {
    for (auto &item : _items) {
        if (item.action == action) {
            item.submenu = submenu;
            submenu->_parentMenu = this;
            break;
        }
    }
}

QAction *PopupMenu::addSubmenu(PopupMenu *submenu) {
    auto *action = addAction(submenu->title());
    setSubmenu(action, submenu);
    return action;
}

void PopupMenu::setTitle(const QString &title) {
    _title = title;
}

QString PopupMenu::title() const {
    return _title;
}

QList<QAction *> PopupMenu::actions() const {
    QList<QAction *> result;
    result.reserve(_items.size());
    for (const auto &item : _items) {
        result.append(item.action);
    }
    return result;
}

Variant PopupMenu::variant() const {
    return _variant;
}

// --------------------------------------------------------
// Layout calculation (menu recountWidth formula).
// --------------------------------------------------------

const QMargins &PopupMenu::itemPadding() const {
    if (_variant == Variant::WithIcons) return kIconsItemPadding;
    if (_variant == Variant::Folders) return kFoldersItemPadding;
    return kDefaultItemPadding;
}

int PopupMenu::scrollPaddingTop() const {
    return (_variant == Variant::Default)
        ? kScrollPaddingTopDefault
        : kScrollPaddingTopIcons;
}

int PopupMenu::scrollPaddingBottom() const {
    return (_variant == Variant::Default)
        ? kScrollPaddingBottomDefault
        : kScrollPaddingBottomIcons;
}

int PopupMenu::calculateBodyWidth() const {
    const auto f = st::baseFont(13);
    const QFontMetricsF fmf(f);
    const auto &pad = itemPadding();

    int maxItemWidth = 0;
    for (const auto &item : _items) {
        if (item.isSeparator) {
            continue;
        }
        const auto text = item.action->text();
        const auto tabIdx = text.indexOf(QLatin1Char('\t'));
        const auto label = tabIdx < 0 ? text : text.left(tabIdx);
        const auto shortcut = tabIdx < 0 ? QString() : text.mid(tabIdx + 1);

        // Use ceil of float advance to prevent sub-pixel truncation.
        const auto textWidth = qCeil(fmf.horizontalAdvance(label));
        auto additionalWidth = 0;
        if (item.submenu) {
            additionalWidth = kMenuItemRightSkip + 8;
        } else if (!shortcut.isEmpty()) {
            additionalWidth = kMenuItemRightSkip
                + qCeil(fmf.horizontalAdvance(shortcut));
        }
        const auto itemWidth = pad.left()
            + textWidth
            + additionalWidth
            + pad.right();
        maxItemWidth = qMax(maxItemWidth, itemWidth);
    }

    return qBound(kWidthMin, maxItemWidth + 18, kWidthMax);
}

void PopupMenu::recalculateLayout() {
    _bodyWidth = calculateBodyWidth();

    // Strip and menu have independent widths — don't force match.

    const auto f = st::baseFont(13);
    const QFontMetrics fm(f);
    const auto fontHeight = fm.height();
    const auto &pad = itemPadding();
    const auto actionHeight = pad.top() + fontHeight + pad.bottom();
    const auto separatorHeight
        = kSeparatorPadding.top() + kSeparatorLineWidth + kSeparatorPadding.bottom();

    const auto scrollTop = scrollPaddingTop();
    const auto scrollBottom = scrollPaddingBottom();

    int y = kShadowExtend + scrollTop;
    if (_hasReactionStrip) {
        y += stripTotalHeight() + stripMenuGap();
    }
    // Compute the menu body X offset when strip is wider than menu.
    const auto stripW = _hasReactionStrip
        ? (2 * stripSkipX() + kStripColumns * stripCellSize())
        : 0;
    const auto innerWidth = qMax(_bodyWidth, stripW);
    const auto menuBodyX = kShadowExtend + (innerWidth - _bodyWidth) / 2;

    for (auto &item : _items) {
        item.height = item.isSeparator ? separatorHeight : actionHeight;
        item.rect = QRect(menuBodyX, y, _bodyWidth, item.height);
        y += item.height;
    }

    auto contentHeight = y - kShadowExtend - scrollTop;
    if (_hasReactionStrip) {
        contentHeight -= stripTotalHeight() + stripMenuGap();
    }

    // Clamp content height for the Folders variant.
    if (_variant == Variant::Folders) {
        const auto maxContent
            = kFoldersMenuMaxHeight - scrollTop - scrollBottom;
        contentHeight = qMin(contentHeight, maxContent);
    }

    const auto bodyHeight = scrollTop + contentHeight + scrollBottom;
    const auto widgetWidth = innerWidth + 2 * kShadowExtend;
    auto totalInnerHeight = bodyHeight;
    if (_hasReactionStrip) {
        totalInnerHeight += stripTotalHeight() + stripMenuGap();
    }
    const auto widgetHeight = totalInnerHeight + 2 * kShadowExtend;
    setFixedSize(widgetWidth, widgetHeight);
}

// --------------------------------------------------------
// Popup / hide.
// --------------------------------------------------------

namespace {

// A never-shown top-level widget's screen() is null, which makes edge-clamping
// inconsistent between the first and later opens (the "submenu top jumps on the
// second open" bug — first open skips the clamp, second applies it). Fall back to
// the screen under the target point.
[[nodiscard]] const QScreen *screenForPopup(const QWidget *w, QPoint globalPos) {
    if (const auto *s = w->screen()) {
        return s;
    }
    if (const auto *s = QGuiApplication::screenAt(globalPos)) {
        return s;
    }
    return QGuiApplication::primaryScreen();
}

} // namespace

void PopupMenu::popup(const QPoint &globalPos) {
    recalculateLayout();

    // Position widget so the body top-left is at globalPos.
    auto widgetPos = globalPos - QPoint(kShadowExtend, kShadowExtend);

    // Clamp to screen bounds.
    if (const auto *scr = screenForPopup(this, globalPos)) {
        const auto avail = scr->availableGeometry();
        if (widgetPos.x() + width() > avail.right() + 1) {
            widgetPos.setX(avail.right() + 1 - width());
        }
        if (widgetPos.y() + height() > avail.bottom() + 1) {
            widgetPos.setY(avail.bottom() + 1 - height());
        }
        widgetPos.setX(qMax(widgetPos.x(), avail.left()));
        widgetPos.setY(qMax(widgetPos.y(), avail.top()));
    }

    move(widgetPos);
    show();
    raise();

    // Only activate the root menu — activating a submenu would
    // deactivate the parent and reset its cursor to arrow.
    if (!_parentMenu) {
        activateWindow();
        qApp->installEventFilter(this);
    }

    // macOS: ensure the window receives all mouse events even on
    // transparent pixels (shadow area).
    Platform::AcceptAllMouseInput(this);

    // Immediately update selection and cursor for the current
    // cursor position (cursor is forced via Cocoa in setActiveIndex).
    const auto localPos = mapFromGlobal(QCursor::pos());
    const auto idx = itemIndexAt(localPos);
    setActiveIndex(idx);
}

void PopupMenu::hideMenu() {
    hideActiveSubmenu();
    emit aboutToHide();
    hide();
    _activeIndex = -1;

    if (!_parentMenu) {
        qApp->removeEventFilter(this);
    }
}

void PopupMenu::hideEvent(QHideEvent *e) {
    QWidget::hideEvent(e);
    hideActiveSubmenu();
    _activeIndex = -1;

    if (!_parentMenu) {
        qApp->removeEventFilter(this);
    }
}

// --------------------------------------------------------
// Global event filter (root menu): close on outside click / deactivate.
// --------------------------------------------------------

bool PopupMenu::eventFilter(QObject *obj [[maybe_unused]], QEvent *e) {
    if (e->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(e);
        const auto globalPos = me->globalPosition().toPoint();
        const auto inside = containsGlobal(globalPos);
        if (!inside) {
            hideMenu();
        }
    } else if (e->type() == QEvent::ApplicationDeactivate) {
        hideMenu();
    }
    return false;
}

bool PopupMenu::containsGlobal(const QPoint &globalPos) const {
    if (isVisible() && geometry().contains(globalPos)) {
        return true;
    }
    if (_visibleSubmenu) {
        return _visibleSubmenu->containsGlobal(globalPos);
    }
    return false;
}

// --------------------------------------------------------
// Hit testing.
// --------------------------------------------------------

int PopupMenu::itemIndexAt(const QPoint &localPos) const {
    for (int i = 0; i < _items.size(); ++i) {
        if (!_items[i].isSeparator && _items[i].rect.contains(localPos)) {
            return i;
        }
    }
    return -1;
}

void PopupMenu::setActiveIndex(int index) {
    if (_activeIndex == index) {
        return;
    }
    _activeIndex = index;

    // Show/hide submenu based on the active item.
    if (index >= 0
        && index < _items.size()
        && _items[index].submenu) {
        showSubmenuForItem(index);
    } else {
        hideActiveSubmenu();
    }

    // Force hand cursor via Cocoa AFTER submenu operations —
    // showing a submenu triggers a cursor update cycle on macOS
    // that would reset an earlier cursor change.
    // Skip for disabled items — they keep the default arrow cursor.
    if (index >= 0 && _items[index].action->isEnabled()) {
        Platform::ForcePointingHandCursor();
    }

    update();
}

// --------------------------------------------------------
// Submenu management (popupSubmenu).
// --------------------------------------------------------

void PopupMenu::showSubmenuForItem(int index) {
    auto &item = _items[index];
    if (!item.submenu) {
        return;
    }
    if (item.submenu == _visibleSubmenu && _visibleSubmenu->isVisible()) {
        return;
    }

    hideActiveSubmenu();

    // Size the child now so its body width is current for the left/right decision.
    item.submenu->recalculateLayout();
    const auto childBodyWidth = item.submenu->_bodyWidth;

    // Force the child's native window to exist BEFORE popup() positions it.
    // A submenu's parent is another Tool window, and its NSWindow is otherwise
    // created lazily during the first show() — so the move() popup() issues before
    // that show is dropped on the first open (and only sticks on later opens),
    // which is the "submenu top jumps on the second open" bug. Realizing the
    // window here makes the first open behave like every later one. (The root menu
    // doesn't need this — its parent is an ordinary widget — so it's left alone.)
    (void)item.submenu->winId();

    // Default: child body left = parent body right (shadows overlap).
    const auto parentBodyLeft = pos().x() + kShadowExtend;
    auto bodyLeft = parentBodyLeft + _bodyWidth;
    const auto bodyTop = pos().y() + item.rect.y();

    // Open on whichever side has room: flip to the left of the parent when the
    // child would overflow off the right edge. The vertical clamp is left to
    // popup(), which now fits consistently across opens (screenForPopup).
    if (const auto *scr = screenForPopup(this, QPoint(bodyLeft, bodyTop))) {
        if (bodyLeft + childBodyWidth > scr->availableGeometry().right() + 1) {
            bodyLeft = parentBodyLeft - childBodyWidth;
        }
    }

    item.submenu->popup(QPoint(bodyLeft, bodyTop));
    _visibleSubmenu = item.submenu;
}

void PopupMenu::hideActiveSubmenu() {
    if (_visibleSubmenu) {
        _visibleSubmenu->hide();
        _visibleSubmenu = nullptr;
    }
}

// --------------------------------------------------------
// Action triggering.
// --------------------------------------------------------

void PopupMenu::triggerAction(int index) {
    if (index < 0 || index >= _items.size()) {
        return;
    }
    auto &item = _items[index];
    if (item.isSeparator || !item.action->isEnabled()) {
        return;
    }
    if (item.submenu) {
        showSubmenuForItem(index);
        return;
    }

    // Save callback before hiding — hideMenu may start cleanup.
    auto callback = item.callback;

    // Close the entire hierarchy, then trigger.
    auto *root = this;
    while (root->_parentMenu) {
        root = root->_parentMenu;
    }
    root->hideMenu();

    // Prefer direct callback (bypasses Qt signal delivery issues
    // with hidden widget contexts), fall back to action trigger.
    if (callback) {
        callback();
    } else {
        item.action->trigger();
    }
}

// --------------------------------------------------------
// Mouse handling.
// --------------------------------------------------------

void PopupMenu::enterEvent(QEnterEvent *e) {
    QWidget::enterEvent(e);
    // If this is a submenu and the parent had us hidden, we're back.
    if (_parentMenu) {
        _parentMenu->_visibleSubmenu = this;
    }
}

void PopupMenu::mouseMoveEvent(QMouseEvent *e) {
    // Reaction strip hover detection.
    if (_hasReactionStrip) {
        const auto localX = e->pos().x();
        const auto localY = e->pos().y();
        const auto innerW = width() - 2 * kShadowExtend;
        const auto pillW = 2 * stripSkipX() + kStripColumns * stripCellSize();
        const auto pillLeft = kShadowExtend + (innerW - pillW) / 2;
        const auto stripTop = kShadowExtend;
        const auto stripBottom = stripTop + stripHeight();
        const auto cellAreaLeft = pillLeft + stripSkipX();
        const auto cellAreaRight = cellAreaLeft + kStripColumns * stripCellSize();

        if (localY >= stripTop
            && localY < stripBottom
            && localX >= cellAreaLeft
            && localX < cellAreaRight) {
            const auto newIdx = (localX - cellAreaLeft) / stripCellSize();
            if (newIdx != _hoveredEmojiIndex) {
                _hoveredEmojiIndex = newIdx;
                setActiveIndex(-1);
                update();
            }
            Platform::ForcePointingHandCursor();
            return;
        }

        if (_hoveredEmojiIndex >= 0) {
            _hoveredEmojiIndex = -1;
            update();
        }
    }

    const auto idx = itemIndexAt(e->pos());
    setActiveIndex(idx);
    // Force cursor on every move — setActiveIndex only forces on
    // index change, but submenu show events can reset the cursor
    // between moves while the index stays the same.
    if (idx >= 0 && _items[idx].action->isEnabled()) {
        Platform::ForcePointingHandCursor();
    }
}

void PopupMenu::mousePressEvent(QMouseEvent *e) {
    // Reaction strip click handling.
    if (_hasReactionStrip && _hoveredEmojiIndex >= 0) {
        const auto &emojis = stripEmoji();
        if (_hoveredEmojiIndex < emojis.size()) {
            emit reactionChosen(_reactionEventId, emojis[_hoveredEmojiIndex]);
        } else {
            emit reactionExpandRequested(
                _reactionEventId,
                mapToGlobal(e->pos()));
        }
        hideMenu();
        return;
    }

    const auto idx = itemIndexAt(e->pos());
    if (idx >= 0) {
        triggerAction(idx);
    }
}

void PopupMenu::leaveEvent(QEvent *e) {
    QWidget::leaveEvent(e);

    if (_hoveredEmojiIndex >= 0) {
        _hoveredEmojiIndex = -1;
        update();
    }

    if (_visibleSubmenu && _visibleSubmenu->isVisible()) {
        // Mouse left parent while submenu is open — keep submenu
        // trigger highlighted, submenu will close itself if mouse
        // doesn't enter it.
        return;
    }

    // If this is a submenu, check where the mouse went.
    if (_parentMenu) {
        const auto globalPos = QCursor::pos();
        if (_parentMenu->geometry().contains(globalPos)) {
            // Mouse moved to parent — let parent's mouseMoveEvent
            // decide whether to keep or close this submenu.
            return;
        }
        // Mouse went elsewhere — hide entire chain.
        auto *root = _parentMenu;
        while (root->_parentMenu) {
            root = root->_parentMenu;
        }
        root->hideMenu();
        return;
    }

    _activeIndex = -1;
    update();
}

void PopupMenu::forwardMouseMove(const QPoint &globalPos) {
    const auto localPos = mapFromGlobal(globalPos);
    const auto idx = itemIndexAt(localPos);
    setActiveIndex(idx);
}

void PopupMenu::forwardMousePress(const QPoint &globalPos) {
    const auto localPos = mapFromGlobal(globalPos);
    const auto idx = itemIndexAt(localPos);
    if (idx >= 0) {
        triggerAction(idx);
    }
}

// --------------------------------------------------------
// Keyboard navigation.
// --------------------------------------------------------

void PopupMenu::keyPressEvent(QKeyEvent *e) {
    switch (e->key()) {
    case Qt::Key_Escape:
        if (_parentMenu) {
            hide();
        } else {
            hideMenu();
        }
        break;
    case Qt::Key_Up:
        navigateItems(-1);
        break;
    case Qt::Key_Down:
        navigateItems(1);
        break;
    case Qt::Key_Right:
        if (_activeIndex >= 0
            && _activeIndex < _items.size()
            && _items[_activeIndex].submenu) {
            showSubmenuForItem(_activeIndex);
        }
        break;
    case Qt::Key_Left:
        if (_parentMenu) {
            hide();
        }
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (_activeIndex >= 0) {
            triggerAction(_activeIndex);
        }
        break;
    default:
        QWidget::keyPressEvent(e);
    }
}

void PopupMenu::navigateItems(int delta) {
    if (_items.isEmpty()) {
        return;
    }

    auto idx = _activeIndex;
    const auto count = _items.size();

    for (int tries = 0; tries < count; ++tries) {
        idx += delta;
        if (idx < 0) idx = count - 1;
        if (idx >= count) idx = 0;

        if (!_items[idx].isSeparator && _items[idx].action->isEnabled()) {
            setActiveIndex(idx);
            return;
        }
    }
}

// --------------------------------------------------------
// Painting — menu action paint.
// --------------------------------------------------------

void PopupMenu::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Inner rect: the full area inset by shadow extend.
    const auto innerRect = rect().adjusted(
        kShadowExtend, kShadowExtend,
        -kShadowExtend, -kShadowExtend);

    // Compute strip and menu body rects independently.
    const auto stripW = _hasReactionStrip
        ? (2 * stripSkipX() + kStripColumns * stripCellSize()) : 0;
    const auto menuBodyTop = _hasReactionStrip
        ? (innerRect.top() + stripTotalHeight() + stripMenuGap()) : innerRect.top();
    const auto menuBodyRect = QRect(
        innerRect.left() + (innerRect.width() - _bodyWidth) / 2,
        menuBodyTop,
        _bodyWidth,
        innerRect.bottom() - menuBodyTop + 1);

    // Paint shadow behind menu body.
    const auto tiles = loadShadowTiles(devicePixelRatioF());
    paintShadow(p, menuBodyRect, width(), tiles);

    // Menu body background.
    {
        QPainterPath menuPath;
        menuPath.addRoundedRect(
            QRectF(menuBodyRect).adjusted(0.5, 0.5, -0.5, -0.5),
            kPopupRadius, kPopupRadius);
        p.fillPath(menuPath, st::menuBg);
        p.setClipPath(menuPath);
    }

    // ---- Reaction emoji strip (visually separate from menu) ----
    if (_hasReactionStrip) {
        p.setClipping(false); // strip paints outside the menu clip

        const auto pillW = stripW;
        const auto pillLeft = innerRect.left() + (innerRect.width() - pillW) / 2;
        const auto pillTop = innerRect.top();
        const auto pillRect = QRectF(pillLeft, pillTop, pillW, stripHeight());

        // Strip pill background with border.
        {
            PainterHighQualityEnabler hq(p);
            p.setPen(QPen(st::shadowFg, 1.0));
            p.setBrush(st::menuBg);
            p.drawRoundedRect(pillRect, stripRadius(), stripRadius());
        }

        // Emoji cells.
        const auto cellsLeft = pillLeft + stripSkipX();
        const auto cellsTop = pillTop + (stripHeight() - stripCellSize()) / 2;
        const auto &emojis = stripEmoji();
        QFont emojiFont;
        emojiFont.setPixelSize(stripEmojiSize());
        for (int i = 0; i < kStripColumns; ++i) {
            const auto cellLeft = cellsLeft + i * stripCellSize();
            const auto cellRect = QRect(cellLeft, cellsTop, stripCellSize(), stripCellSize());
            // No hover resize on the quick-reaction strip — draw at fixed size.
            if (i < emojis.size()) {
                // Hovered-emoji background, matching the vertical reaction column.
                if (i == _hoveredEmojiIndex) {
                    PainterHighQualityEnabler hq(p);
                    p.setPen(Qt::NoPen);
                    p.setBrush(st::emojiPanHover);
                    p.drawRoundedRect(cellRect.adjusted(2, 2, -2, -2), 8, 8);
                }
                p.setFont(emojiFont);
                p.setPen(st::windowFg);
                p.drawText(cellRect, Qt::AlignCenter, emojis[i]);
            } else {
                // Expand button: 24px circle with downward chevron icon.
                PainterHighQualityEnabler hq(p);
                const auto circleSize = stripExpandCircleSize();
                const auto cx = cellRect.center().x();
                const auto cy = cellRect.center().y();
                const auto circleRect = QRectF(
                    cx - circleSize / 2, cy - circleSize / 2,
                    circleSize, circleSize);

                // Circle background (windowBgRipple equivalent).
                p.setPen(Qt::NoPen);
                p.setBrush(st::windowBgOver);
                p.drawEllipse(circleRect);

                // Downward chevron (˅) — matching reactions_expand_panel icon.
                // The icon is a 2px stroke chevron centered in 24px.
                const auto chevW = stripChevronWidth();  // half-width of chevron
                const auto chevH = stripChevronHeight();  // height of chevron
                const auto chevY = cy - chevH / 2 + 0.5;
                QPen chevPen(
                    st::windowSubTextFg,
                    stripChevronPenWidth(),
                    Qt::SolidLine,
                    Qt::RoundCap,
                    Qt::RoundJoin);
                p.setPen(chevPen);
                p.setBrush(Qt::NoBrush);
                QPainterPath chevron;
                chevron.moveTo(cx - chevW / 2, chevY);
                chevron.lineTo(cx, chevY + chevH);
                chevron.lineTo(cx + chevW / 2, chevY);
                p.drawPath(chevron);
            }
        }

        // Restore menu body clip for item painting.
        QPainterPath menuClip;
        menuClip.addRoundedRect(
            QRectF(menuBodyRect).adjusted(0.5, 0.5, -0.5, -0.5),
            kPopupRadius, kPopupRadius);
        p.setClipPath(menuClip);
    }

    const auto f = st::baseFont(13);
    p.setFont(f);
    const QFontMetrics fm(f);
    const auto &pad = itemPadding();

    for (int i = 0; i < _items.size(); ++i) {
        const auto &item = _items[i];
        const auto &rect = item.rect;
        if (rect.isEmpty()) {
            continue;
        }

        // Separators.
        if (item.isSeparator) {
            p.fillRect(rect, st::menuBg);
            p.fillRect(
                rect.left() + kSeparatorPadding.left(),
                rect.top() + kSeparatorPadding.top(),
                rect.width() - kSeparatorPadding.left() - kSeparatorPadding.right(),
                kSeparatorLineWidth,
                st::menuSeparatorFg);
            continue;
        }

        const auto isEnabled = item.action->isEnabled();
        const auto isActive = isEnabled && (i == _activeIndex);
        const auto isAttention
            = item.action->property(kActionAttentionProperty).toBool();
        const auto bg = isActive ? st::menuBgOver : st::menuBg;

        p.fillRect(rect, bg);

        // Left icon.
        auto arrowWidth = 0;
        if (_variant == Variant::WithIcons || _variant == Variant::Folders) {
            auto iconName = item.action->property(kActionIconNameProperty).toString();
            auto isCheckIcon = false;
            if (iconName.isEmpty()
                && item.action->isCheckable()
                && item.action->isChecked()) {
                iconName = QStringLiteral("player/player_check");
                isCheckIcon = true;
            }
            if (!iconName.isEmpty()) {
                const auto iconColor = isEnabled
                    ? (isCheckIcon
                        ? st::windowBgActive
                        : isAttention
                        ? (isActive ? st::attentionButtonFgOver : st::attentionButtonFg)
                        : (isActive ? st::menuIconFgOver : st::menuIconFg))
                    : st::menuFgDisabled;
                const auto icon = iconName.contains(QLatin1Char('/'))
                    ? TeleMatrix::Style::IconProvider::tintedIcon(QStringLiteral(":/telematrix/icons/"), iconName, iconColor)
                    : TeleMatrix::Style::IconProvider::tintedIcon(QStringLiteral(":/telematrix/icons/menu/"), iconName, iconColor);
                if (!icon.isNull()) {
                    p.drawImage(
                        QPoint(
                            rect.left() + kMenuIconPosition.x(),
                            rect.top() + kMenuIconPosition.y()),
                        icon);
                }
            }
        }

        // Text parsing.
        const auto text = item.action->text();
        const auto tabIdx = text.indexOf(QLatin1Char('\t'));
        const auto label = tabIdx < 0 ? text : text.left(tabIdx);
        const auto shortcut = tabIdx < 0 ? QString() : text.mid(tabIdx + 1);

        const auto textLeft = rect.left() + pad.left();
        auto textRight = rect.left() + rect.width() - pad.right();
        const auto textY = rect.top() + pad.top() + fm.ascent();

        // Submenu arrow (right side).
        if (item.submenu) {
            const auto arrowColor = isEnabled
                ? st::menuIconColor
                : st::menuFgDisabled;
            const auto arrow = TeleMatrix::Style::IconProvider::tintedIcon(
                QStringLiteral(":/telematrix/icons/menu/"),
                QStringLiteral("submenu_arrow"),
                arrowColor);
            if (!arrow.isNull()) {
                const auto arrowH = qRound(arrow.height() / arrow.devicePixelRatio());
                arrowWidth = qRound(arrow.width() / arrow.devicePixelRatio());
                const auto arrowLeft = rect.left()
                    + rect.width()
                    - kMenuItemRightSkip
                    - arrowWidth;
                const auto arrowTop = rect.top() + (rect.height() - arrowH) / 2;
                p.drawImage(QPoint(arrowLeft, arrowTop), arrow);
            }
        } else if (!shortcut.isEmpty()) {
            // Shortcut text (right side).
            const auto shortcutWidth = fm.horizontalAdvance(shortcut);
            const auto shortcutX = rect.left()
                + rect.width()
                - pad.right()
                - shortcutWidth;
            p.setPen(!isEnabled
                ? st::menuFgDisabled
                : (isActive ? st::windowSubTextFgOver : st::windowSubTextFg));
            p.drawText(shortcutX, textY, shortcut);
            textRight -= (kMenuItemRightSkip + shortcutWidth);
        }

        if (item.submenu) {
            textRight -= (kMenuItemRightSkip + arrowWidth);
        }

        // Right icon (Folders variant).
        if (_variant == Variant::Folders) {
            const auto rightIconName
                = item.action->property(kActionRightIconNameProperty).toString();
            if (!rightIconName.isEmpty()) {
                auto rightIcon = TeleMatrix::Style::IconProvider::tintedIcon(
                    QStringLiteral(":/telematrix/icons/"),
                    rightIconName,
                    st::dialogsUnreadBgMuted);
                if (!rightIcon.isNull()) {
                    const auto ratio = rightIcon.devicePixelRatio();
                    const auto iconW = qMax(1, qRound(
                        rightIcon.width() / ratio * kFoldersRightIconScale));
                    const auto iconH = qMax(1, qRound(
                        rightIcon.height() / ratio * kFoldersRightIconScale));
                    auto scaled = rightIcon.scaled(
                        qMax(1, qRound(iconW * ratio)),
                        qMax(1, qRound(iconH * ratio)),
                        Qt::IgnoreAspectRatio,
                        Qt::SmoothTransformation);
                    scaled.setDevicePixelRatio(ratio);

                    const auto left
                        = rect.left() + rect.width() - kFoldersRightIconSkip - iconW;
                    const auto top = rect.top() + (rect.height() - iconH) / 2;
                    p.drawImage(QPoint(left, top), scaled);
                }
            }
        }

        // Label text.
        p.setPen(!isEnabled
            ? st::menuFgDisabled
            : (isAttention
                ? (isActive ? st::attentionButtonFgOver : st::attentionButtonFg)
                : (isActive ? st::windowFgOver : st::windowFg)));
        p.drawText(
            textLeft,
            textY,
            fm.elidedText(label, Qt::ElideRight, qMax(0, textRight - textLeft)));

        // Fallback submenu arrow if icon resource is missing.
        if (item.submenu && arrowWidth <= 0) {
            PainterHighQualityEnabler hq(p);
            const auto cx = rect.right() - 20.0;
            const auto cy = rect.center().y();
            p.setPen(QPen(
                !isEnabled ? st::menuFgDisabled : st::menuIconColor,
                1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            QPainterPath arrow;
            arrow.moveTo(cx - 2.5, cy - 4.5);
            arrow.lineTo(cx + 2.5, cy);
            arrow.lineTo(cx - 2.5, cy + 4.5);
            p.drawPath(arrow);
        }
    }
}

// ============================================================
// Factory functions.
// ============================================================

PopupMenu *createStyledMenu(QWidget *parent, Variant variant) {
    return new PopupMenu(variant, parent);
}

void setActionIconName(QAction *action, const QString &iconName) {
    if (!action) {
        return;
    }
    action->setProperty(kActionIconNameProperty, iconName);
}

void setActionRightIconName(QAction *action, const QString &iconName) {
    if (!action) {
        return;
    }
    action->setProperty(kActionRightIconNameProperty, iconName);
}

} // namespace TeleMatrix::HistoryPopupMenuStyle

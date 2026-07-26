// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_emoji_picker.h"
#include "emoji_data.h"
#include "../app/app_controller.h"
#include "ui/widgets/input_fields.h"

#include <QCoreApplication>
#include <QCursor>
#include <QEvent>
#include <QApplication>
#include <QGuiApplication>
#include <QHash>
#include <QKeyEvent>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPaintEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QShowEvent>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>

#include "styles/style_constants.h"
#include "ui/painter.h"
#include "ui/platform/ui_utility_mac.h"
#include "ui/style/icon_provider.h"
#include "ui/style/runtime_scale.h"

namespace TeleMatrix {

namespace {

// --- Emoji panel constants (runtime-scaled) ---
inline int kPanelWidth = 345;
inline int kShadowExtend = 10;
inline int kPanelRadius = 8;
inline int kFooterHeight = 36;
inline int kIconWidth = 30;
inline int kIconArea = 28;
inline int kHeaderHeight = 33;
inline int kHeaderLeft = 14;
inline int kHeaderTop = 10;
inline int kDesiredSize = 37;
inline int kVerticalSizeSub = 1;
inline int kHoverWidth = 34;
inline int kHoverHeight = 32;
inline int kHoverRadius = 8;
inline int kScrollBarWidth = 7;
inline int kMinBodyHeight = 278;
inline int kMaxBodyHeight = 640;
inline QMargins kSearchMargins(8, 11, 9, 5);

// Thin capsule scrollbar painted with live st:: colors (tracks theme changes)
// instead of a frozen QSS stylesheet. Mirrors the old QSS exactly:
//   width = kScrollBarWidth, margins 3px top / 2px right / 3px bottom / 0 left,
//   handle radius = kScrollBarWidth/2, min handle height 20px,
//   scrollBarBg normally, scrollBarBgOver on hover; no arrow buttons/page bg.
class CapsuleScrollBar final : public QScrollBar {
public:
    explicit CapsuleScrollBar(QWidget *parent)
        : QScrollBar(Qt::Vertical, parent) {
        setMouseTracking(true);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        if (minimum() >= maximum()) {
            return; // Nothing to scroll: leave fully transparent.
        }
        // Margins from the original QSS: 3px top, 2px right, 3px bottom.
        constexpr int kMarginTop = 3;
        constexpr int kMarginRight = 2;
        constexpr int kMarginBottom = 3;
        constexpr int kMinHandle = 20;

        const int trackTop = kMarginTop;
        const int trackHeight = height() - kMarginTop - kMarginBottom;
        if (trackHeight <= 0) {
            return;
        }
        const int handleWidth = width() - kMarginRight;
        if (handleWidth <= 0) {
            return;
        }

        const int range = maximum() - minimum();
        const int span = range + pageStep();
        int handleHeight = (span > 0)
            ? int(qint64(trackHeight) * pageStep() / span)
            : trackHeight;
        handleHeight = std::max(handleHeight, kMinHandle);
        handleHeight = std::min(handleHeight, trackHeight);

        const int travel = trackHeight - handleHeight;
        const int handleTop = trackTop
            + ((range > 0) ? int(qint64(travel) * (value() - minimum()) / range) : 0);

        const auto &fill = _hovered ? st::scrollBarBgOver : st::scrollBarBg;
        const qreal radius = handleWidth / 2.0;

        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawRoundedRect(
            QRectF(0, handleTop, handleWidth, handleHeight), radius, radius);
    }

    void enterEvent(QEnterEvent *e) override {
        _hovered = true;
        update();
        QScrollBar::enterEvent(e);
    }
    void leaveEvent(QEvent *e) override {
        _hovered = false;
        update();
        QScrollBar::leaveEvent(e);
    }

private:
    bool _hovered = false;
};

void applyEmojiPanelScale() {
    using TeleMatrix::Style::ConvertScale;
    kPanelWidth = ConvertScale(345);
    kShadowExtend = ConvertScale(10);
    kPanelRadius = ConvertScale(8);
    kFooterHeight = ConvertScale(36);
    kIconWidth = ConvertScale(30);
    kIconArea = ConvertScale(28);
    kHeaderHeight = ConvertScale(33);
    kHeaderLeft = ConvertScale(14);
    kHeaderTop = ConvertScale(10);
    kDesiredSize = ConvertScale(37);
    kVerticalSizeSub = ConvertScale(1);
    kHoverWidth = ConvertScale(34);
    kHoverHeight = ConvertScale(32);
    kHoverRadius = ConvertScale(8);
    kScrollBarWidth = ConvertScale(7);
    kMinBodyHeight = ConvertScale(278);
    kMaxBodyHeight = ConvertScale(640);
    kSearchMargins = QMargins(
        ConvertScale(8), ConvertScale(11),
        ConvertScale(9), ConvertScale(5));
}

// Colors aliased to st:: tokens so they
// follow the current theme (day/night).
#define emojiPanBg       st::emojiPanBg
#define emojiPanHeaderFg st::emojiPanHeaderFg
#define emojiPanHover    st::emojiPanHover
#define categoriesBg     st::emojiPanCategories
#define categoriesBgOver st::windowBgRipple
#define emojiIconFg      st::emojiIconFg
#define emojiIconFgActive st::emojiIconFgActive

// Sections: Recent + 7 categories (Symbols & Flags combined).
struct SectionInfo {
    QString title;
    QVector<EmojiCategory> dataCategories; // categories from emoji_data to merge
    QString icon;
    bool showHeader = true;
    bool recent = false;
};

QVector<SectionInfo> sectionInfos() {
    const QVector<SectionInfo> infos = {
        { QString(),
          {},
          QStringLiteral("emoji_recent"),
          false,
          true },
        { QCoreApplication::translate("EmojiPicker", "Smileys & People"),
          { EmojiCategory::People },
          QStringLiteral("emoji_smile"),
          true,
          false },
        { QCoreApplication::translate("EmojiPicker", "Animals & Nature"),
          { EmojiCategory::Nature },
          QStringLiteral("emoji_nature"),
          true,
          false },
        { QCoreApplication::translate("EmojiPicker", "Food & Drink"),
          { EmojiCategory::FoodDrink },
          QStringLiteral("emoji_food"),
          true,
          false },
        { QCoreApplication::translate("EmojiPicker", "Activity"),
          { EmojiCategory::Activity },
          QStringLiteral("emoji_activity"),
          true,
          false },
        { QCoreApplication::translate("EmojiPicker", "Travel & Places"),
          { EmojiCategory::TravelPlaces },
          QStringLiteral("emoji_travel"),
          true,
          false },
        { QCoreApplication::translate("EmojiPicker", "Objects"),
          { EmojiCategory::Objects },
          QStringLiteral("emoji_objects"),
          true,
          false },
        { QCoreApplication::translate("EmojiPicker", "Symbols & Flags"),
          { EmojiCategory::Symbols, EmojiCategory::Flags },
          QStringLiteral("emoji_symbols"),
          true,
          false },
    };
    return infos;
}

EmojiEntry LookupRecentEntry(const QString &emoji) {
    static QHash<QString, EmojiEntry> map;
    if (map.isEmpty()) {
        const auto &all = allEmojiEntries();
        map.reserve(all.size());
        for (const auto &entry : all) {
            map.insert(entry.emoji, entry);
        }
    }
    const auto it = map.constFind(emoji);
    if (it != map.cend()) {
        return it.value();
    }
    return {
        emoji,
        emoji,
        EmojiCategory::People,
    };
}

// Load emoji tab icon via centralized IconProvider (scale-aware).
QPixmap loadTintedIcon(const QString &name, const QColor &color) {
    const auto img = TeleMatrix::Style::IconProvider::tintedIcon(
        QStringLiteral(":/telematrix/icons/emoji/"), name, color);
    return QPixmap::fromImage(img);
}

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

ShadowTiles loadShadowTiles(qreal dpr) {
    static QHash<quint64, ShadowTiles> cache;
    const auto key = dpr >= 2.5 ? 3 : (dpr >= 1.5 ? 2 : 1);
    const auto scaleKey = (TeleMatrix::Style::Scale() * 10) + key;
    // Fold the shadow color into the cache key: a theme switch can reassign
    // st::windowShadowFg, and keying on scale/dpr alone would keep serving
    // stale-colored tiles until restart.
    const quint64 cacheKey =
        (quint64(st::windowShadowFg.rgba()) << 32) | quint64(uint(scaleKey));
    if (const auto i = cache.constFind(cacheKey); i != cache.cend()) {
        return i.value();
    }

    const auto base = QStringLiteral(":/telematrix/icons/shadow/");
    auto load = [&](const QString &name) {
        const auto mask = TeleMatrix::Style::IconProvider::loadScaledMask(base, name);
        auto colorized = TeleMatrix::Style::IconProvider::colorizeMask(mask, st::windowShadowFg);
        if (key == 3) {
            colorized.setDevicePixelRatio(3.0);
        } else if (key == 2) {
            colorized.setDevicePixelRatio(2.0);
        } else {
            colorized.setDevicePixelRatio(1.0);
        }
        return colorized;
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

    cache.insert(cacheKey, tiles);
    return tiles;
}

void paintShadow(QPainter &p, const QRect &box, const ShadowTiles &tiles) {
    const auto ratio = tiles.topLeft.devicePixelRatio();
    auto logicalW = [ratio](const QImage &image) {
        return qRound(image.width() / ratio);
    };
    auto logicalH = [ratio](const QImage &image) {
        return qRound(image.height() / ratio);
    };

    const auto tlW = logicalW(tiles.topLeft);
    const auto tlH = logicalH(tiles.topLeft);
    const auto trW = logicalW(tiles.topRight);
    const auto trH = logicalH(tiles.topRight);
    const auto blW = logicalW(tiles.bottomLeft);
    const auto blH = logicalH(tiles.bottomLeft);
    const auto brW = logicalW(tiles.bottomRight);
    const auto brH = logicalH(tiles.bottomRight);
    const auto leftW = logicalW(tiles.left);
    const auto rightW = logicalW(tiles.right);
    const auto topH = logicalH(tiles.top);
    const auto bottomH = logicalH(tiles.bottom);

    {
        auto from = box.y();
        auto to = box.y() + box.height();
        p.drawImage(box.x() - kShadowExtend, box.y() - kShadowExtend, tiles.topLeft);
        from += tlH - kShadowExtend;
        p.drawImage(
            box.x() - kShadowExtend,
            box.y() + box.height() + kShadowExtend - blH,
            tiles.bottomLeft);
        to -= blH - kShadowExtend;
        if (to > from) {
            p.drawImage(
                QRect(box.x() - kShadowExtend, from, leftW, to - from),
                tiles.left,
                QRect(0, 0, tiles.left.width(), tiles.left.height()));
        }
    }

    {
        auto from = box.y();
        auto to = box.y() + box.height();
        p.drawImage(
            box.x() + box.width() + kShadowExtend - trW,
            box.y() - kShadowExtend,
            tiles.topRight);
        from += trH - kShadowExtend;
        p.drawImage(
            box.x() + box.width() + kShadowExtend - brW,
            box.y() + box.height() + kShadowExtend - brH,
            tiles.bottomRight);
        to -= brH - kShadowExtend;
        if (to > from) {
            p.drawImage(
                QRect(
                    box.x() + box.width() + kShadowExtend - rightW,
                    from,
                    rightW,
                    to - from),
                tiles.right,
                QRect(0, 0, tiles.right.width(), tiles.right.height()));
        }
    }

    {
        auto from = box.x() + tlW - kShadowExtend;
        auto to = box.x() + box.width() - (trW - kShadowExtend);
        if (to > from) {
            p.drawImage(
                QRect(from, box.y() - kShadowExtend, to - from, topH),
                tiles.top,
                QRect(0, 0, tiles.top.width(), tiles.top.height()));
        }
    }

    {
        auto from = box.x() + blW - kShadowExtend;
        auto to = box.x() + box.width() - (brW - kShadowExtend);
        if (to > from) {
            p.drawImage(
                QRect(
                    from,
                    box.y() + box.height() + kShadowExtend - bottomH,
                    to - from,
                    bottomH),
                tiles.bottom,
                QRect(0, 0, tiles.bottom.width(), tiles.bottom.height()));
        }
    }
}

st::InputFieldStyle emojiSearchFieldStyle() {
    auto style = st::dialogsFilter;
    // Only widen the left text inset to clear the search icon. Keep
    // dialogsFilter's placeholderMargins (5px left gap): zeroing it put the
    // placeholder at the caret's x, so "Search" sat under the blinking caret.
    style.textMargins.setLeft(TeleMatrix::Style::ConvertScale(35));
    return style;
}

} // namespace

void HistoryEmojiPicker::initEmojiPanelPxValues() {
    applyEmojiPanelScale();
}

// ============================================================
// EmojiTabBar — bottom category bar
// ============================================================

class EmojiTabBar final : public QWidget {
public:
    using TabCallback = std::function<void(int index)>;

    explicit EmojiTabBar(QWidget *parent = nullptr)
        : QWidget(parent) {
        setMouseTracking(true);
        refreshIcons();
        refreshMetrics();
    }

    void setTabCallback(TabCallback cb) { _callback = std::move(cb); }
    void refreshIcons() {
        const auto &sections = sectionInfos();
        _icons.resize(sections.size());
        _iconsActive.resize(sections.size());
        for (int i = 0; i < sections.size(); ++i) {
            _icons[i] = loadTintedIcon(sections[i].icon, emojiIconFg);
            _iconsActive[i] = loadTintedIcon(sections[i].icon, emojiIconFgActive);
        }
        update();
    }

    void refreshMetrics() {
        setFixedHeight(kFooterHeight);
        rebuildBgPath();
        update();
    }

    void setActiveTab(int index) {
        if (_active == index) return;
        _active = index;
        update();
    }

    int activeTab() const { return _active; }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        if (_bgPath.isEmpty()) {
            rebuildBgPath();
        }
        p.fillPath(_bgPath, categoriesBg);

        const int count = _icons.size();
        if (count == 0) return;
        const int singleW = width() / count;

        for (int i = 0; i < count; ++i) {
            const int x = i * singleW;
            const int centerX = x + singleW / 2;
            const int centerY = height() / 2;

            // Selection background.
            if (i == _active || i == _hovered) {
                p.setPen(Qt::NoPen);
                p.setBrush(i == _active ? categoriesBgOver : emojiPanHover);
                const QRect selRect(centerX - kIconArea / 2,
                                    centerY - kIconArea / 2,
                                    kIconArea, kIconArea);
                p.drawRoundedRect(selRect, kHoverRadius, kHoverRadius);
            }

            // Icon.
            const auto &px = (i == _active) ? _iconsActive[i] : _icons[i];
            if (!px.isNull()) {
                const int iconW = px.width() / px.devicePixelRatio();
                const int iconH = px.height() / px.devicePixelRatio();
                p.drawPixmap(centerX - iconW / 2, centerY - iconH / 2, px);
            }
        }
    }

    void enterEvent(QEnterEvent *event) override {
        updateHoverAndCursor(event->position().toPoint());
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        const int idx = updateHoverAndCursor(event->pos());
        if (idx != _hovered) {
            _hovered = idx;
            update();
        }
    }

    void leaveEvent(QEvent *) override {
        unsetCursor();
        Platform::ForceArrowCursor();
        if (_hovered >= 0) {
            _hovered = -1;
            update();
        }
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton) return;
        const int idx = iconIndexAt(event->pos());
        if (idx >= 0 && _callback) {
            _callback(idx);
        }
    }

private:
    int updateHoverAndCursor(const QPoint &pos) {
        const auto idx = iconIndexAt(pos);
        if (idx >= 0) {
            setCursor(Qt::PointingHandCursor);
            Platform::ForcePointingHandCursor();
        } else {
            unsetCursor();
            Platform::ForceArrowCursor();
        }
        return idx;
    }

    int iconIndexAt(const QPoint &pos) const {
        const int count = _icons.size();
        if (count == 0) return -1;
        const int singleW = width() / count;
        const int idx = pos.x() / singleW;
        if (idx < 0 || idx >= count) {
            return -1;
        }
        const int centerX = idx * singleW + singleW / 2;
        const int centerY = height() / 2;
        const QRect hitRect(
            centerX - kIconArea / 2,
            centerY - kIconArea / 2,
            kIconArea,
            kIconArea);
        return hitRect.contains(pos) ? idx : -1;
    }

    QVector<QPixmap> _icons;
    QVector<QPixmap> _iconsActive;
    TabCallback _callback;
    int _active = 0;
    int _hovered = -1;
    QPainterPath _bgPath;

    void resizeEvent(QResizeEvent *) override {
        rebuildBgPath();
    }

    void rebuildBgPath() {
        _bgPath = QPainterPath();
        _bgPath.moveTo(0, 0);
        _bgPath.lineTo(width(), 0);
        _bgPath.lineTo(width(), height() - kPanelRadius);
        _bgPath.arcTo(
            QRectF(width() - 2 * kPanelRadius, height() - 2 * kPanelRadius,
                2 * kPanelRadius, 2 * kPanelRadius),
            0,
            -90);
        _bgPath.lineTo(kPanelRadius, height());
        _bgPath.arcTo(
            QRectF(0, height() - 2 * kPanelRadius, 2 * kPanelRadius, 2 * kPanelRadius),
            270,
            -90);
        _bgPath.closeSubpath();
    }
};

class EmojiSearchField final : public Ui::InputField {
public:
    explicit EmojiSearchField(QWidget *parent = nullptr)
        : Ui::InputField(
            parent,
            emojiSearchFieldStyle(),
            rpl::single(QCoreApplication::translate("EmojiPicker", "Search")))
        , _searchIcon(loadTintedIcon(
            QStringLiteral("emoji_search_input"),
            emojiIconFg)) {
        connect(this, &QLineEdit::textChanged, this, [this](const QString &text) {
            setCancelVisible(!text.isEmpty());
        });
        setCancelVisible(false);
    }

    void refreshIcon() {
        _searchIcon = loadTintedIcon(
            QStringLiteral("emoji_search_input"),
            emojiIconFg);
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        Ui::InputField::paintEvent(event);

        if (_searchIcon.isNull()) {
            return;
        }

        QPainter painter(this);
        const auto dpr = qMax(1.0, _searchIcon.devicePixelRatio());
        const auto iconH = qRound(_searchIcon.height() / dpr);
        painter.drawPixmap(
            st::dialogsFilterPadding.x(),
            (height() - iconH) / 2,
            _searchIcon);
    }

private:
    QPixmap _searchIcon;
};

// ============================================================
// EmojiSectionedGrid — scrollable emoji content with sections
// ============================================================

class EmojiSectionedGrid final : public QWidget {
public:
    using SelectCallback = std::function<void(const QString &emoji)>;

    explicit EmojiSectionedGrid(int contentWidth, QWidget *parent = nullptr)
        : QWidget(parent)
        , _contentWidth(contentWidth) {
        setMouseTracking(true);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        recomputeMetrics();
    }

    void setSelectCallback(SelectCallback cb) { _select = std::move(cb); }

    void setTooltipsEnabled(bool enabled) { _tooltipsEnabled = enabled; }

    void refreshMetrics(int contentWidth) {
        _contentWidth = contentWidth;
        recomputeMetrics();
        recomputeLayout();
        update();
    }

    void setSections(QVector<QString> titles,
                     QVector<QVector<EmojiEntry>> entries) {
        // Skip the rebuild only when nothing the grid renders changed. Compare the
        // actual emoji sequence per section, not just the counts — otherwise a
        // reordered section of the same length (e.g. Recent after a pick) would be
        // silently ignored and never shown until the widget is recreated (restart).
        if (titles == _titles && entries.size() == _entries.size()) {
            bool same = true;
            for (int i = 0; same && i < entries.size(); ++i) {
                if (entries[i].size() != _entries[i].size()) {
                    same = false;
                    break;
                }
                for (int j = 0; j < entries[i].size(); ++j) {
                    if (entries[i][j].emoji != _entries[i][j].emoji) {
                        same = false;
                        break;
                    }
                }
            }
            if (same) return;
        }
        _titles = std::move(titles);
        _entries = std::move(entries);
        _sectionYOffsets.clear();
        _hovered = -1;
        _hoveredSection = -1;
        _pressed = -1;
        _pressedSection = -1;
        recomputeLayout();
        update();
    }

    void setPlaceholderText(const QString &text) {
        _placeholder = text;
        update();
    }

    int sectionYOffset(int idx) const {
        return (idx >= 0 && idx < _sectionYOffsets.size())
            ? _sectionYOffsets[idx] : 0;
    }

    int sectionCount() const { return _titles.size(); }

    int sectionAtY(int y) const {
        for (int i = _sectionYOffsets.size() - 1; i >= 0; --i) {
            if (y >= _sectionYOffsets[i]) return i;
        }
        return 0;
    }

protected:
    void enterEvent(QEnterEvent *event) override {
        int section = -1;
        int index = -1;
        hitTest(event->position().toPoint(), section, index);
        updateHoverState(section, index);
    }

    void paintEvent(QPaintEvent *e) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // Fill background white.
        const auto clip = e ? e->rect() : rect();
        p.fillRect(clip, emojiPanBg);

        if (_titles.isEmpty()) {
            const auto placeholderFont = st::baseFont(13);
            p.setFont(placeholderFont);
            p.setPen(emojiPanHeaderFg);
            p.drawText(rect(), Qt::AlignCenter, _placeholder);
            return;
        }

        for (int s = 0; s < _titles.size(); ++s) {
            const auto yOff = _sectionYOffsets[s];
            const auto headerHeight = sectionHeaderHeight(s);
            const auto rows = (_entries[s].size() + _columnCount - 1) / _columnCount;
            const auto sectionEnd = yOff + headerHeight + rows * _cellH;

            // Skip entire section if it doesn't intersect the clip rect.
            if (sectionEnd <= clip.top() || yOff >= clip.bottom()) {
                continue;
            }

            if (headerHeight > 0) {
                // Section header (semibold 13px, #999999).
                auto hdrFont = st::baseFont(13);
                hdrFont.setBold(true);
                const auto hdrAscent = QFontMetrics(hdrFont).ascent();
                p.setFont(hdrFont);
                p.setPen(emojiPanHeaderFg);
                p.drawText(
                    QPointF(
                        kHeaderLeft,
                        yOff + kHeaderTop + hdrAscent),
                    _titles[s]);
            }

            // Emoji cells.
            const auto emojiCellFont = st::baseFont(20);
            p.setFont(emojiCellFont);
            p.setPen(st::windowFg);
            const auto &items = _entries[s];
            const auto gridY = yOff + headerHeight;
            for (int i = 0; i < items.size(); ++i) {
                const auto row = i / _columnCount;
                const auto col = i % _columnCount;
                const auto cellX = _leftPad + col * _cellW;
                const auto cellY = gridY + row * _cellH;

                // Skip rows outside clip rect.
                if (cellY + _cellH <= clip.top() || cellY >= clip.bottom()) {
                    continue;
                }

                // Hover highlight.
                if (s == _hoveredSection && i == _hovered) {
                    p.setPen(Qt::NoPen);
                    p.setBrush(emojiPanHover);
                    const auto hx = cellX + (_cellW - kHoverWidth) / 2;
                    const auto hy = cellY + (_cellH - kHoverHeight) / 2;
                    p.drawRoundedRect(QRect(hx, hy, kHoverWidth, kHoverHeight),
                                      kHoverRadius, kHoverRadius);
                    p.setPen(st::windowFg);
                }
                p.drawText(QRect(cellX, cellY, _cellW, _cellH),
                           Qt::AlignCenter, items[i].emoji);
            }
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        int section = -1, index = -1;
        hitTest(event->pos(), section, index);
        updateHoverState(section, index);
        QToolTip::hideText();
    }

    void leaveEvent(QEvent *) override {
        QToolTip::hideText();
        unsetCursor();
        Platform::ForceArrowCursor();
        if (_hovered >= 0) {
            const auto oldRect = cellRect(_hoveredSection, _hovered);
            _hovered = -1;
            _hoveredSection = -1;
            if (!oldRect.isEmpty()) {
                update(oldRect);
            }
        }
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton) return;
        int section = -1, index = -1;
        hitTest(event->pos(), section, index);
        _pressedSection = section;
        _pressed = (section >= 0
            && index >= 0
            && index < _entries[section].size())
            ? index
            : -1;
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton) {
            return;
        }
        int section = -1, index = -1;
        hitTest(event->pos(), section, index);
        const auto activate = (_pressedSection >= 0
            && _pressed >= 0
            && section == _pressedSection
            && index == _pressed
            && section < _entries.size()
            && index < _entries[section].size()
            && _select);
        _pressedSection = -1;
        _pressed = -1;
        if (activate) {
            unsetCursor();
            _select(_entries[section][index].emoji);
        }
        event->accept();
    }

private:
    void recomputeMetrics() {
        _columnCount = qMax(_contentWidth / kDesiredSize, 1);
        _cellW = _contentWidth / _columnCount;
        _cellH = _cellW - 2 * kVerticalSizeSub;
        _leftPad = (_contentWidth - _columnCount * _cellW) / 2;
    }

    void updateHoverState(int section, int index) {
        if (section != _hoveredSection || index != _hovered) {
            const auto oldRect = cellRect(_hoveredSection, _hovered);
            _hoveredSection = section;
            _hovered = index;
            const auto newRect = cellRect(section, index);
            if (!oldRect.isEmpty() && !newRect.isEmpty()) {
                update(oldRect.united(newRect));
            } else if (!oldRect.isEmpty()) {
                update(oldRect);
            } else if (!newRect.isEmpty()) {
                update(newRect);
            }
        }
        if (section >= 0 && index >= 0) {
            setCursor(Qt::PointingHandCursor);
            Platform::ForcePointingHandCursor();
        } else {
            unsetCursor();
            Platform::ForceArrowCursor();
        }
    }

    void hitTest(const QPoint &pos, int &outS, int &outI) const {
        outS = -1; outI = -1;
        for (int s = 0; s < _titles.size(); ++s) {
            const auto gridY = _sectionYOffsets[s] + sectionHeaderHeight(s);
            const auto rows = (_entries[s].size() + _columnCount - 1) / _columnCount;
            const auto gridEnd = gridY + rows * _cellH;
            if (pos.y() >= gridY && pos.y() < gridEnd) {
                const auto col = (pos.x() - _leftPad) / _cellW;
                const auto row = (pos.y() - gridY) / _cellH;
                if (col >= 0 && col < _columnCount) {
                    const auto idx = row * _columnCount + col;
                    if (idx >= 0 && idx < _entries[s].size()) {
                        outS = s; outI = idx;
                    }
                }
                return;
            }
        }
    }

    void recomputeLayout() {
        _sectionYOffsets.resize(_titles.size());
        int y = 0;
        for (int s = 0; s < _titles.size(); ++s) {
            _sectionYOffsets[s] = y;
            const auto rows = qMax(1, (_entries[s].size() + _columnCount - 1) / _columnCount);
            y += sectionHeaderHeight(s) + rows * _cellH;
        }
        if (_titles.isEmpty()) {
            // No sections (e.g. empty search results): a zero-height widget
            // receives no paint events, so the centered placeholder would
            // never draw. Fill the scroll viewport so it has an area to paint.
            const auto viewportH = parentWidget() ? parentWidget()->height() : 0;
            y = (viewportH > 0) ? viewportH : kMinBodyHeight;
        }
        setFixedSize(_contentWidth, y);
    }

    int sectionHeaderHeight(int section) const {
        return (_titles.value(section).isEmpty() ? 0 : kHeaderHeight);
    }

    QRect cellRect(int section, int index) const {
        if (section < 0 || index < 0
            || section >= _entries.size()
            || index >= _entries[section].size()) {
            return {};
        }
        const auto gridY = _sectionYOffsets[section] + sectionHeaderHeight(section);
        const auto row = index / _columnCount;
        const auto col = index % _columnCount;
        return QRect(
            _leftPad + col * _cellW,
            gridY + row * _cellH,
            _cellW,
            _cellH);
    }

    int _contentWidth;
    int _columnCount;
    int _cellW;
    int _cellH;
    int _leftPad = 0;
    QVector<QString> _titles;
    QVector<QVector<EmojiEntry>> _entries;
    QVector<int> _sectionYOffsets;
    SelectCallback _select;
    int _hoveredSection = -1;
    int _hovered = -1;
    int _pressedSection = -1;
    int _pressed = -1;
    bool _tooltipsEnabled = true;
    QString _placeholder;
};

// ============================================================
// HistoryEmojiPicker — main popup
// ============================================================

HistoryEmojiPicker::HistoryEmojiPicker(
    AppController *controller,
    QWidget *parent)
    : QWidget(parent)
    , _controller(controller) {
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setAttribute(Qt::WA_TranslucentBackground, true);

    _rootLayout = new QVBoxLayout(this);
    _rootLayout->setContentsMargins(
        kShadowExtend,
        kShadowExtend,
        kShadowExtend,
        kShadowExtend);
    _rootLayout->setSpacing(0);

    // --- Search (33px + margins 11/5 top/bottom) ---
    _searchWrap = new QWidget(this);
    _searchWrap->setAttribute(Qt::WA_TranslucentBackground, true);
    _searchLayout = new QVBoxLayout(_searchWrap);
    _searchLayout->setContentsMargins(kSearchMargins);
    _search = new EmojiSearchField(_searchWrap);
    _search->setFixedHeight(st::dialogsFilterHeight);
    _search->setFocusPolicy(Qt::ClickFocus);
    _searchLayout->addWidget(_search);
    _rootLayout->addWidget(_searchWrap);

    // --- Scrollable emoji grid ---
    _scroll = new QScrollArea(this);
    _scroll->setFrameShape(QFrame::NoFrame);
    _scroll->setWidgetResizable(false);
    _scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    // Scrollbar: 7px wide, semi-transparent black capsule.
    applyScrollStyle();

    // Grid content width: panel - left padding - scrollbar.
    const int gridWidth = kPanelWidth - kScrollBarWidth;
    _grid = new EmojiSectionedGrid(gridWidth, _scroll);
    _grid->setSelectCallback([this](const QString &emoji) {
        QToolTip::hideText();
        _grid->setTooltipsEnabled(false);
        if (_pickerMode == PickerMode::Reaction && !_reactionTargetEventId.isEmpty()) {
            _hideAfterEmojiSelection = true;
            emit reactionSelected(_reactionTargetEventId, emoji);
            _pickerMode = PickerMode::Compose;
            _reactionTargetEventId.clear();
            // Keep the compose picker open, but still close
            // the reaction picker after a reaction is chosen.
            QTimer::singleShot(0, this, &QWidget::hide);
        } else {
            _hideAfterEmojiSelection = false;
            emit emojiSelected(emoji);
            _gridDirty = true; // Recent list may have changed.
        }
    });
    _scroll->setWidget(_grid);
    _rootLayout->addWidget(_scroll, 1);

    // --- Tab bar at bottom (footer) ---
    _tabBar = new EmojiTabBar(this);
    _tabBar->setTabCallback([this](int idx) {
        scrollToSection(idx);
    });
    _rootLayout->addWidget(_tabBar);

    _search->installEventFilter(this);

    connect(_search, &QLineEdit::textChanged, this, [this] {
        rebuildGrid();
    });

    connect(_scroll->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &HistoryEmojiPicker::onScrollChanged);

    rebuildGrid();
}

void HistoryEmojiPicker::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const auto bodyRect = rect().marginsRemoved(QMargins(
        kShadowExtend,
        kShadowExtend,
        kShadowExtend,
        kShadowExtend));
    paintShadow(p, bodyRect, loadShadowTiles(devicePixelRatioF()));

    // Main background — white rounded rect.
    p.setPen(Qt::NoPen);
    p.setBrush(emojiPanBg);
    p.drawRoundedRect(bodyRect, kPanelRadius, kPanelRadius);

    // Footer background — covers bottom portion with categoriesBg.
    // Draw a rect for the footer area, then redraw bottom rounded corners.
    const int footerY = bodyRect.bottom() - kFooterHeight + 1;
    p.setBrush(categoriesBg);
    QPainterPath path;
    path.moveTo(bodyRect.left(), footerY);
    path.lineTo(bodyRect.right() + 1, footerY);
    path.lineTo(bodyRect.right() + 1, bodyRect.bottom() - kPanelRadius + 1);
    path.arcTo(QRectF(bodyRect.right() + 1 - 2 * kPanelRadius, bodyRect.bottom() + 1 - 2 * kPanelRadius,
                       2 * kPanelRadius, 2 * kPanelRadius), 0, -90);
    path.lineTo(bodyRect.left() + kPanelRadius, bodyRect.bottom() + 1);
    path.arcTo(QRectF(bodyRect.left(), bodyRect.bottom() + 1 - 2 * kPanelRadius,
                       2 * kPanelRadius, 2 * kPanelRadius), 270, -90);
    path.closeSubpath();
    p.drawPath(path);
}

int HistoryEmojiPicker::shadowExtend() {
    return kShadowExtend;
}

int HistoryEmojiPicker::panelWidth() {
    return kPanelWidth;
}

int HistoryEmojiPicker::minBodyHeight() {
    return kMinBodyHeight;
}

int HistoryEmojiPicker::maxBodyHeight() {
    return kMaxBodyHeight;
}

QSize HistoryEmojiPicker::sizeHint() const {
    auto target = 380;
    if (auto *screen = QGuiApplication::screenAt(QCursor::pos())) {
        target = int(screen->availableGeometry().height() * 0.75);
    } else if (auto *primary = QGuiApplication::primaryScreen()) {
        target = int(primary->availableGeometry().height() * 0.75);
    }
    const auto bodyHeight = qBound(kMinBodyHeight, target, kMaxBodyHeight);
    return QSize(
        kPanelWidth + 2 * kShadowExtend,
        bodyHeight + 2 * kShadowExtend);
}

void HistoryEmojiPicker::showEvent(QShowEvent *e) {
    refreshScaleDependentUi();
    QWidget::showEvent(e);
    if (_gridDirty) {
        rebuildGrid();
        _gridDirty = false;
    }
    if (_arrowOverrideActive) {
        _arrowOverrideActive = false;
        QApplication::restoreOverrideCursor();
        qApp->removeEventFilter(this);
    }
    unsetCursor();
    _search->unsetCursor();
    _grid->unsetCursor();
    _tabBar->unsetCursor();
    Platform::AcceptAllMouseInput(this);
    _grid->setTooltipsEnabled(true);
}

bool HistoryEmojiPicker::event(QEvent *event) {
    if (event->type() == QEvent::Hide) {
        _grid->setTooltipsEnabled(false);
        QToolTip::hideText();
        unsetCursor();
        _search->unsetCursor();
        _grid->unsetCursor();
        _tabBar->unsetCursor();
        Platform::ForceArrowCursor();
        if (_hideAfterEmojiSelection && !_arrowOverrideActive) {
            QApplication::setOverrideCursor(Qt::ArrowCursor);
            qApp->installEventFilter(this);
            _arrowOverrideActive = true;
        }
        _hideAfterEmojiSelection = false;
    } else if (event->type() == QEvent::WindowDeactivate) {
        _hideAfterEmojiSelection = false;
        hide();
    } else if (event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape) {
            _hideAfterEmojiSelection = false;
            hide();
            return true;
        }
    }
    return QWidget::event(event);
}

bool HistoryEmojiPicker::eventFilter(QObject *obj, QEvent *event) {
    if (_arrowOverrideActive
        && obj != _search
        && event->type() == QEvent::MouseMove) {
        _arrowOverrideActive = false;
        QApplication::restoreOverrideCursor();
        qApp->removeEventFilter(this);
    }
    if (obj == _search) {
        if (event->type() == QEvent::KeyPress) {
            const auto *key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Escape && _search->text().isEmpty()) {
                _hideAfterEmojiSelection = false;
                hide();
                return true;
            }
        } else if (event->type() == QEvent::Enter
            || event->type() == QEvent::MouseMove) {
            _search->setCursor(Qt::IBeamCursor);
            Platform::ForceIBeamCursor();
        } else if (event->type() == QEvent::Leave) {
            _search->unsetCursor();
            Platform::ForceArrowCursor();
        }
    }
    return QWidget::eventFilter(obj, event);
}

void HistoryEmojiPicker::scrollToSection(int tabIndex) {
    if (_grid->sectionCount() <= 0 || tabIndex < 0) {
        return;
    }
    // The footer always shows every category tab, but the grid can omit
    // sections (Recent is dropped when empty), so tab index != grid section
    // index. Resolve the clicked tab to its grid section, or the nearest
    // section after it when that tab has none of its own.
    int gridSection = -1;
    for (int i = 0; i < _sectionTabIndex.size(); ++i) {
        if (_sectionTabIndex[i] >= tabIndex) {
            gridSection = i;
            break;
        }
    }
    if (gridSection < 0) {
        gridSection = _grid->sectionCount() - 1;
    }
    _tabBar->setActiveTab(_sectionTabIndex.value(gridSection, tabIndex));
    const auto y = _grid->sectionYOffset(gridSection);
    _scrollingToSection = true;
    _scroll->verticalScrollBar()->setValue(y);
    _scrollingToSection = false;
}

void HistoryEmojiPicker::onScrollChanged() {
    if (_scrollingToSection) return;
    const auto y = _scroll->verticalScrollBar()->value();
    const auto gridSection = _grid->sectionAtY(y);
    _tabBar->setActiveTab(_sectionTabIndex.value(gridSection, 0));
}

void HistoryEmojiPicker::applyScrollStyle() {
    // Custom capsule scrollbar (7px wide, semi-transparent rounded handle)
    // painted with live st:: colors. The QScrollArea
    // itself is already frameless/transparent via setFrameShape(NoFrame).
    auto *bar = dynamic_cast<CapsuleScrollBar *>(_scroll->verticalScrollBar());
    if (!bar) {
        bar = new CapsuleScrollBar(_scroll);
        _scroll->setVerticalScrollBar(bar);
    }
    bar->setFixedWidth(kScrollBarWidth);

    // Keep the scroll area + viewport transparent (was `background: transparent`
    // in the QSS) so the panel background shows through next to the scrollbar.
    if (auto *vp = _scroll->viewport()) {
        vp->setAutoFillBackground(false);
        QPalette pal = vp->palette();
        pal.setColor(QPalette::Window, Qt::transparent);
        pal.setColor(QPalette::Base, Qt::transparent);
        vp->setPalette(pal);
    }
}

void HistoryEmojiPicker::refreshScaleDependentUi() {
    applyEmojiPanelScale();
    if (_rootLayout) {
        _rootLayout->setContentsMargins(
            kShadowExtend,
            kShadowExtend,
            kShadowExtend,
            kShadowExtend);
    }
    if (_searchLayout) {
        _searchLayout->setContentsMargins(kSearchMargins);
    }
    if (auto *searchField = static_cast<EmojiSearchField*>(_search)) {
        searchField->refreshStyle(emojiSearchFieldStyle());
        searchField->refreshIcon();
    }
    applyScrollStyle();
    if (_grid) {
        _grid->refreshMetrics(kPanelWidth - kScrollBarWidth);
    }
    if (_tabBar) {
        _tabBar->refreshIcons();
        _tabBar->refreshMetrics();
    }
    updateGeometry();
    update();
}

void HistoryEmojiPicker::rebuildGrid() {
    const auto query = _search->text().trimmed().toLower();
    const auto &sections = sectionInfos();

    QVector<QString> titles;
    QVector<QVector<EmojiEntry>> groups;
    QVector<int> tabIndices; // grid section -> footer tab (sectionInfos) index

    if (!query.isEmpty()) {
        QVector<EmojiEntry> group;
        for (int i = 1; i < sections.size(); ++i) {
            for (const auto &cat : sections[i].dataCategories) {
                for (const auto &entry : categoryEntries(cat)) {
                    if (entry.name.contains(query) || entry.emoji.contains(query)) {
                        group.push_back(entry);
                    }
                }
            }
        }
        if (!group.isEmpty()) {
            titles.push_back(QString());
            groups.push_back(std::move(group));
            tabIndices.push_back(0);
        }
    } else {
        QVector<EmojiEntry> recent;
        if (_controller) {
            const auto &saved = _controller->accountSettings().recentEmoji();
            recent.reserve(saved.size());
            for (const auto &item : saved) {
                if (!item.emoji.isEmpty()) {
                    recent.push_back(LookupRecentEntry(item.emoji));
                }
            }
        }
        if (!recent.isEmpty()) {
            titles.push_back(QString());
            groups.push_back(std::move(recent));
            tabIndices.push_back(0); // Recent tab.
        }

        for (int i = 0; i < sections.size(); ++i) {
            if (sections[i].recent) {
                continue;
            }
            QVector<EmojiEntry> group;
            for (const auto &cat : sections[i].dataCategories) {
                const auto &entries = categoryEntries(cat);
                group.append(entries);
            }
            titles.push_back(sections[i].title);
            groups.push_back(std::move(group));
            tabIndices.push_back(i);
        }
    }

    _sectionTabIndex = std::move(tabIndices);

    if (titles.isEmpty() && !query.isEmpty()) {
        _grid->setSections({}, {});
        _grid->setPlaceholderText(tr("No emoji found"));
    } else {
        _grid->setSections(std::move(titles), std::move(groups));
        _grid->setPlaceholderText(QString());
    }
    onScrollChanged();
    _gridDirty = false;
}

void HistoryEmojiPicker::setMode(PickerMode mode) {
    _pickerMode = mode;
}

void HistoryEmojiPicker::setReactionTarget(const QString &eventId) {
    _reactionTargetEventId = eventId;
}

} // namespace TeleMatrix

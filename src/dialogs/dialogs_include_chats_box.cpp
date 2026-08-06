// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "dialogs_include_chats_box.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QScrollArea>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include "protocol/media_cache.h"
#include "protocol/protocol_bridge.h"
#include "styles/style_constants.h"
#include "ui/empty_userpic.h"
#include "ui/focus_restore.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/close_button.h"
#include "ui/widgets/scroll_area.h"

namespace TeleMatrix {

namespace {

constexpr int kShadowExtend = 10;

constexpr int kRowHeight = 56;
constexpr int kPhotoSize = 42;
constexpr int kPhotoX = 16;
constexpr int kPhotoY = 7;
constexpr int kNameX = 74;
constexpr int kNameY = 9;              // name top offset (two-line row)
constexpr int kStatusY = 30;          // folders subtitle top offset
constexpr int kPhotoSmall = 36;       // imageSmallRadius*2 (avatar shrinks when selected)
constexpr int kListPaddingTop = 8;
constexpr int kListPaddingBottom = 10;

// MultiSelect chip bar (defaultMultiSelectItem: height 32).
constexpr int kChipHeight = 32;
constexpr int kChipRadius = 16;
constexpr int kChipTextGap = 6;
constexpr int kChipRightPad = 12;
constexpr int kChipSpacing = 6;
constexpr int kBarPadding = 8;
constexpr int kBarMinHeight = 48;
constexpr int kBarMaxHeight = 104;
constexpr int kChipMaxWidth = 128;    // defaultMultiSelectItem.maxWidth (whole chip)

void paintBoxShadow(QPainter &p, const QRect &boxRect) {
    PainterHighQualityEnabler hq(p);
    p.setPen(Qt::NoPen);
    for (int i = kShadowExtend; i >= 1; --i) {
        const auto progress = qreal(kShadowExtend - i) / kShadowExtend;
        const auto alpha = int(18.0 * progress * progress);
        p.setBrush(st::withAlpha(st::windowShadowFg, alpha));
        const auto r = st::boxRadius + i;
        p.drawRoundedRect(boxRect.adjusted(-i, -i, i, i), r, r);
    }
}

class RoundedPanel : public QWidget {
public:
    explicit RoundedPanel(QWidget *parent) : QWidget(parent) {}

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(st::boxBg);
        p.drawRoundedRect(rect(), st::boxRadius, st::boxRadius);
    }
};

void makeScrollTransparent(QScrollArea *scroll) {
    scroll->setAutoFillBackground(false);
    if (auto *viewport = scroll->viewport()) {
        viewport->setAutoFillBackground(false);
        viewport->setAttribute(Qt::WA_TranslucentBackground);
    }
}

// Delete-cross geometry (size 32, skip 10, stroke 1.5), fully shown — a
// filled × path drawn as a vector (no PNG).
void drawChipCross(QPainter &p, int ox, int oy, const QColor &color) {
    PainterHighQualityEnabler hq(p);
    const qreal size = kChipHeight;
    const qreal skip = 10.0;
    const qreal stroke = 1.5;
    const qreal s = stroke / 1.4142135623730951; // deleteStroke = stroke / sqrt(2)
    const qreal L = ox + skip;
    const qreal T = oy + skip;
    const qreal W = size - 2 * skip;
    const qreal H = size - 2 * skip;
    const QPointF pts[12] = {
        { L, T + s },
        { L + s, T },
        { L + W / 2, T + H / 2 - s },
        { L + W - s, T },
        { L + W, T + s },
        { L + W / 2 + s, T + H / 2 },
        { L + W, T + H - s },
        { L + W - s, T + H },
        { L + W / 2, T + H / 2 + s },
        { L + s, T + H },
        { L, T + H - s },
        { L + W / 2 - s, T + H / 2 },
    };
    QPainterPath path;
    path.moveTo(pts[0]);
    for (int i = 1; i < 12; ++i) {
        path.lineTo(pts[i]);
    }
    path.lineTo(pts[0]);
    p.fillPath(path, color);
}

} // namespace

// ─────────────────────────────────────────────
// ChatChipBar
// ─────────────────────────────────────────────

ChatChipBar::ChatChipBar(QWidget *parent) : QWidget(parent) {
    setMouseTracking(true);
    _input = new QLineEdit(this);
    _input->setFrame(false);
    _input->setAttribute(Qt::WA_MacShowFocusRect, false);
    _input->setFixedHeight(kChipHeight);
    _input->setFont(st::baseFont(13));
    {
        QPalette pal = _input->palette();
        pal.setColor(QPalette::Base, Qt::transparent);
        pal.setColor(QPalette::Text, st::windowFg);
        _input->setPalette(pal);
    }
    _input->setTextMargins(4, 0, 4, 0);
    setMinimumHeight(kBarMinHeight);
    setMaximumHeight(kBarMaxHeight);
}

void ChatChipBar::addChip(const RoomPickEntry &room) {
    if (hasChip(room.id)) {
        return;
    }
    ChatChip chip;
    chip.id = room.id;
    chip.name = room.name.isEmpty() ? room.id : room.name;
    chip.avatarUrl = room.avatarUrl;
    chip.avatarEntityId = room.avatarEntityId;
    _chips.push_back(chip);
    relayout();
    emit chipAdded(room.id);
}

void ChatChipBar::removeChip(const QString &id) {
    for (int i = 0; i < _chips.size(); ++i) {
        if (_chips[i].id == id) {
            _chips.remove(i);
            _hoveredChip = -1;
            relayout();
            emit chipRemoved(id);
            return;
        }
    }
}

bool ChatChipBar::hasChip(const QString &id) const {
    for (const auto &c : _chips) {
        if (c.id == id) return true;
    }
    return false;
}

QSet<QString> ChatChipBar::currentIds() const {
    QSet<QString> ids;
    for (const auto &c : _chips) {
        ids.insert(c.id);
    }
    return ids;
}

QLineEdit *ChatChipBar::inputField() const {
    return _input;
}

QSize ChatChipBar::sizeHint() const {
    return { width(), qMax(kBarMinHeight, minimumHeight()) };
}

QSize ChatChipBar::minimumSizeHint() const {
    return { 100, kBarMinHeight };
}

int ChatChipBar::chipAt(const QPoint &pos) const {
    for (int i = 0; i < _chips.size(); ++i) {
        if (_chips[i].rect.contains(pos)) return i;
    }
    return -1;
}

void ChatChipBar::relayout() {
    const QFontMetrics fm(st::normalFont);
    int x = kBarPadding;
    int y = kBarPadding;
    for (auto &chip : _chips) {
        const int fixed = kChipHeight + kChipTextGap + kChipRightPad;
        const int textW = qMin(fm.horizontalAdvance(chip.name), kChipMaxWidth - fixed);
        const int chipW = fixed + textW;
        if (x + chipW > width() - kBarPadding && x > kBarPadding) {
            x = kBarPadding;
            y += kChipHeight + kChipSpacing;
        }
        chip.rect = QRect(x, y, chipW, kChipHeight);
        x += chipW + kChipSpacing;
    }
    const int inputMinW = 100;
    if (x + inputMinW > width() - kBarPadding && x > kBarPadding) {
        x = kBarPadding;
        y += kChipHeight + kChipSpacing;
    }
    _input->setGeometry(x, y, qMax(width() - kBarPadding - x, inputMinW), kChipHeight);
    const int totalH = y + kChipHeight + kBarPadding;
    const int clampedH = qBound(kBarMinHeight, totalH, kBarMaxHeight);
    if (height() != clampedH) {
        setFixedHeight(clampedH);
        emit heightChanged();
    }
    update();
}

void ChatChipBar::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), st::boxBg);
    const QFontMetrics fm(st::normalFont);
    for (int i = 0; i < _chips.size(); ++i) {
        const auto &chip = _chips[i];
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(st::windowBgOver);
        p.drawRoundedRect(chip.rect, kChipRadius, kChipRadius);

        // Avatar fills the pill's rounded left end.
        const QRect av(chip.rect.x(), chip.rect.y(), kChipHeight, kChipHeight);
        bool painted = false;
        if (!chip.avatarUrl.isEmpty()) {
            const auto dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
            const auto pm = MediaCache::loadAvatarPixmapAsync(
                chip.avatarUrl, kChipHeight, dpr, this, av);
            if (!pm.isNull()) {
                p.drawPixmap(av.topLeft(), pm);
                painted = true;
            }
        }
        if (!painted) {
            const auto &seed = chip.avatarEntityId.isEmpty() ? chip.id : chip.avatarEntityId;
            Ui::EmptyUserpic::paint(p, seed, chip.name, av.x(), av.y(), av.width());
        }
        if (i == _hoveredChip) {
            // Hover → accent circle + delete-cross over the avatar.
            p.setPen(Qt::NoPen);
            p.setBrush(st::activeButtonBg);
            p.drawEllipse(av);
            drawChipCross(p, av.x(), av.y(), st::activeButtonFg);
        }

        p.setPen(st::windowFg);
        p.setFont(st::normalFont);
        const int textX = chip.rect.x() + kChipHeight + kChipTextGap;
        // Same text width as relayout (width - height - paddings),
        // so a name that fits is shown in full and isn't force-elided.
        const int fixed = kChipHeight + kChipTextGap + kChipRightPad;
        const int textW = qMin(fm.horizontalAdvance(chip.name), kChipMaxWidth - fixed);
        p.drawText(textX,
            chip.rect.y() + (kChipHeight - fm.height()) / 2 + fm.ascent(),
            fm.elidedText(chip.name, Qt::ElideRight, qMax(0, textW)));
    }
}

void ChatChipBar::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) return;
    const int idx = chipAt(e->pos());
    // Remove only when the "×" (the avatar area) is clicked.
    if (idx >= 0
        && QRect(_chips[idx].rect.x(), _chips[idx].rect.y(), kChipHeight, kChipHeight)
               .contains(e->pos())) {
        removeChip(_chips[idx].id);
        return;
    }
    _input->setFocus();
}

void ChatChipBar::mouseMoveEvent(QMouseEvent *e) {
    const int idx = chipAt(e->pos());
    // Hand cursor only over the "×" (avatar area); the rest of the chip is inert.
    const bool overCross = (idx >= 0)
        && QRect(_chips[idx].rect.x(), _chips[idx].rect.y(), kChipHeight, kChipHeight)
               .contains(e->pos());
    setCursor(overCross ? Qt::PointingHandCursor : Qt::ArrowCursor);
    if (idx != _hoveredChip) {
        _hoveredChip = idx;
        update();
    }
}

void ChatChipBar::leaveEvent(QEvent *) {
    if (_hoveredChip >= 0) {
        _hoveredChip = -1;
        setCursor(Qt::ArrowCursor);
        update();
    }
}

void ChatChipBar::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    relayout();
}

// ─────────────────────────────────────────────
// ChatPickInner
// ─────────────────────────────────────────────

ChatPickInner::ChatPickInner(QWidget *parent)
    : QWidget(parent) {
    setMouseTracking(true);
}

void ChatPickInner::setRooms(const QVector<RoomPickEntry> &rooms) {
    _all = rooms;
    _hovered = -1;
    _filtered.clear();
    for (int i = 0; i < _all.size(); ++i) {
        _filtered.push_back(i);
    }
    setFixedHeight(contentHeight());
    update();
}

void ChatPickInner::setSelected(const QSet<QString> &selected) {
    _selected = selected;
    update();
}

void ChatPickInner::setFilter(const QString &query) {
    const auto needle = query.trimmed().toLower();
    _filtered.clear();
    for (int i = 0; i < _all.size(); ++i) {
        if (needle.isEmpty() || _all[i].name.toLower().contains(needle)) {
            _filtered.push_back(i);
        }
    }
    _hovered = -1;
    setFixedHeight(contentHeight());
    update();
}

int ChatPickInner::contentHeight() const {
    return kListPaddingTop + _filtered.size() * kRowHeight + kListPaddingBottom;
}

QSize ChatPickInner::sizeHint() const {
    // Width 0 so the scroll area sizes us to the viewport; otherwise the
    // right-side checkmarks land off-screen.
    return { 0, contentHeight() };
}

QSize ChatPickInner::minimumSizeHint() const {
    return { 0, contentHeight() };
}

void ChatPickInner::paintEvent(QPaintEvent *e) {
    QPainter p(this);
    const auto r = e->rect();
    p.setClipRect(r);
    p.fillRect(r, st::boxBg);

    if (_filtered.isEmpty()) {
        p.setFont(st::normalFont);
        p.setPen(st::windowSubTextFg);
        p.drawText(QRect(0, 0, width(), kRowHeight + kListPaddingTop), int(Qt::AlignCenter),
                   QCoreApplication::translate("DialogsIncludeChatsBox", "No chats found"));
        return;
    }

    const int yFrom = r.y();
    const int yTo = r.y() + r.height();
    const int firstRow = qMax(0, (yFrom - kListPaddingTop) / kRowHeight);
    const int lastRow = qMin(
        (int)_filtered.size() - 1,
        (yTo - kListPaddingTop + kRowHeight - 1) / kRowHeight);

    for (int i = firstRow; i <= lastRow; ++i) {
        paintRow(p, i, i == _hovered);
    }
}

void ChatPickInner::paintRow(QPainter &p, int filteredIndex, bool hovered) {
    const auto &room = _all[_filtered[filteredIndex]];
    const int y = kListPaddingTop + filteredIndex * kRowHeight;
    const int w = width();

    p.fillRect(0, y, w, kRowHeight, hovered ? st::windowBgOver : st::boxBg);

    const bool selected = _selected.contains(room.id);

    // Avatar (round checkbox): shrinks + gains an accent ring and
    // a check badge when selected.
    const QRect slot(kPhotoX, y + kPhotoY, kPhotoSize, kPhotoSize);
    const int av = selected ? Style::ConvertScale(kPhotoSmall) : kPhotoSize;
    const int off = (kPhotoSize - av) / 2;
    const QRect avRect(slot.x() + off, slot.y() + off, av, av);
    bool paintedAvatar = false;
    if (!room.avatarUrl.isEmpty()) {
        const auto dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
        const auto avatar = MediaCache::loadAvatarPixmapAsync(
            room.avatarUrl, av, dpr, this, avRect);
        if (!avatar.isNull()) {
            p.drawPixmap(avRect.topLeft(), avatar);
            paintedAvatar = true;
        }
    }
    if (!paintedAvatar) {
        const auto &seed = room.avatarEntityId.isEmpty() ? room.id : room.avatarEntityId;
        Ui::EmptyUserpic::paint(p, seed, room.name, avRect.x(), avRect.y(), avRect.width());
    }
    if (selected) {
        PainterHighQualityEnabler hq(p);
        // Accent ring (selectWidth 2, selectFg windowBgActive).
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(st::windowBgActive, Style::ConvertScale(2)));
        p.drawEllipse(QRectF(slot).adjusted(1, 1, -1, -1));
        // Check badge (bottom-right): accent circle, boxBg border, white check.
        const int badge = Style::ConvertScale(16);
        const QRect bRect(slot.right() - badge + 2, slot.bottom() - badge + 2, badge, badge);
        p.setPen(QPen(st::boxBg, Style::ConvertScale(2)));
        p.setBrush(st::windowBgActive);
        p.drawEllipse(bRect);
        p.setPen(QPen(st::activeButtonFg, Style::ConvertScale(1.5),
            Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPainterPath check;
        check.moveTo(bRect.left() + badge * 0.28, bRect.top() + badge * 0.52);
        check.lineTo(bRect.left() + badge * 0.44, bRect.top() + badge * 0.68);
        check.lineTo(bRect.left() + badge * 0.74, bRect.top() + badge * 0.34);
        p.drawPath(check);
    }

    // Name (accent when selected) + folders subtitle (two-line peer row).
    const int textW = w - kNameX - Style::ConvertScale(16);
    const QFontMetrics nameFm(st::semiboldFont);
    p.setFont(st::semiboldFont);
    p.setPen(selected ? st::windowActiveTextFg : st::windowFg);
    if (room.status.isEmpty()) {
        p.drawText(kNameX,
            y + (kRowHeight + nameFm.ascent() - nameFm.descent()) / 2,
            nameFm.elidedText(room.name, Qt::ElideRight, qMax(0, textW)));
    } else {
        p.drawText(kNameX, y + kNameY + nameFm.ascent(),
            nameFm.elidedText(room.name, Qt::ElideRight, qMax(0, textW)));
        const QFontMetrics stFm(st::normalFont);
        p.setFont(st::normalFont);
        p.setPen(st::windowSubTextFg);
        p.drawText(kNameX, y + kStatusY + stFm.ascent(),
            stFm.elidedText(room.status, Qt::ElideRight, qMax(0, textW)));
    }
}

int ChatPickInner::indexAt(const QPoint &pos) const {
    if (_filtered.isEmpty()) return -1;
    const int row = (pos.y() - kListPaddingTop) / kRowHeight;
    if (row < 0 || row >= _filtered.size()) return -1;
    return row;
}

void ChatPickInner::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) return;
    const int idx = indexAt(e->pos());
    if (idx < 0) return;
    emit roomClicked(_all[_filtered[idx]].id);
}

void ChatPickInner::mouseMoveEvent(QMouseEvent *e) {
    const int idx = indexAt(e->pos());
    if (idx != _hovered) {
        _hovered = idx;
        setCursor(idx >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void ChatPickInner::leaveEvent(QEvent *) {
    if (_hovered >= 0) {
        _hovered = -1;
        setCursor(Qt::ArrowCursor);
        update();
    }
}

// ─────────────────────────────────────────────
// DialogsIncludeChatsBox
// ─────────────────────────────────────────────

DialogsIncludeChatsBox::DialogsIncludeChatsBox(
    const QVector<RoomPickEntry> &rooms,
    const QSet<QString> &selected,
    ProtocolBridge *bridge,
    QWidget *parent)
    : QWidget(parent ? parent->window() : nullptr) {
    if (parentWidget()) {
        setGeometry(parentWidget()->rect());
        parentWidget()->installEventFilter(this);
    }
    setFocusPolicy(Qt::StrongFocus);

    for (const auto &room : rooms) {
        _entryById.insert(room.id, room);
    }

    _a_shown = new QVariantAnimation(this);
    _a_shown->setDuration(200);
    _a_shown->setEasingCurve(QEasingCurve::OutCirc);
    _a_shown->setStartValue(0.0);
    _a_shown->setEndValue(1.0);
    connect(_a_shown, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) {
        _bgOpacity = value.toReal();
        update();
    });

    _a_layerShown = new QVariantAnimation(this);
    _a_layerShown->setDuration(200);
    _a_layerShown->setEasingCurve(QEasingCurve::Linear);
    _a_layerShown->setStartValue(0.0);
    _a_layerShown->setEndValue(1.0);
    connect(_a_layerShown, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) {
        _layerOpacity = value.toReal();
        if (_panel && !_panel->isVisible() && _layerOpacity > 0) {
            _panel->setVisible(true);
        }
        update();
    });

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addStretch(1);

    _panel = new RoundedPanel(this);
    _panel->setVisible(false);
    _panel->setFixedWidth(st::boxWideWidth);
    root->addWidget(_panel, 0, Qt::AlignHCenter);
    root->addStretch(1);

    auto *panelLayout = new QVBoxLayout(_panel);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(0);

    // Title.
    auto *titleBar = new QWidget(_panel);
    titleBar->setFixedHeight(st::boxTitleHeight);
    panelLayout->addWidget(titleBar);
    auto *titleText = new QLabel(
        QCoreApplication::translate("DialogsIncludeChatsBox", "Include Chats"), titleBar);
    titleText->setFont(st::boxTitleFont);
    {
        QPalette pal = titleText->palette();
        pal.setColor(QPalette::WindowText, st::boxTitleFg);
        titleText->setPalette(pal);
    }
    titleText->move(st::boxTitlePosition.x(), st::boxTitlePosition.y());

    auto *close = new ::Ui::CloseButton(titleBar);
    close->move(st::boxWideWidth - st::settingsCloseButtonSize, 0);
    connect(close, &::Ui::CloseButton::clicked, this, [this] {
        reject();
    });

    // MultiSelect chip bar (selected chats + inline search).
    _chipBar = new ChatChipBar(_panel);
    _chipBar->setFixedWidth(st::boxWideWidth);
    _chipBar->inputField()->setPlaceholderText(
        QCoreApplication::translate("DialogsIncludeChatsBox", "Search"));
    panelLayout->addWidget(_chipBar);
    for (const auto &room : rooms) {
        if (selected.contains(room.id)) {
            _chipBar->addChip(room);
        }
    }

    auto *sep = new QWidget(_panel);
    sep->setFixedHeight(1);
    sep->setAutoFillBackground(true);
    {
        QPalette pal = sep->palette();
        pal.setColor(QPalette::Window, st::shadowFg);
        sep->setPalette(pal);
    }
    panelLayout->addWidget(sep);

    // Fixed-max-height list.
    _inner = new ChatPickInner(nullptr);
    _inner->setRooms(rooms);
    _inner->setSelected(selected);

    _scroll = new ::Ui::ScrollArea(_panel);
    _scroll->setFrameShape(QFrame::NoFrame);
    _scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _scroll->setWidgetResizable(true);
    _scroll->setFixedHeight(st::boxMaxListHeight);
    _scroll->setWidget(_inner);
    makeScrollTransparent(_scroll);
    panelLayout->addWidget(_scroll);

    // Top border for the buttons block (matches the title separator).
    auto *buttonsSep = new QWidget(_panel);
    buttonsSep->setFixedHeight(1);
    buttonsSep->setAutoFillBackground(true);
    {
        QPalette pal = buttonsSep->palette();
        pal.setColor(QPalette::Window, st::shadowFg);
        buttonsSep->setPalette(pal);
    }
    panelLayout->addWidget(buttonsSep);

    // Buttons.
    auto *buttonsContainer = new QWidget(_panel);
    buttonsContainer->setFixedHeight(
        st::boxButtonPadding.top()
        + st::boxButtonHeight
        + st::boxButtonPadding.bottom());
    panelLayout->addWidget(buttonsContainer);

    auto *buttonsLayout = new QHBoxLayout(buttonsContainer);
    buttonsLayout->setContentsMargins(
        st::boxButtonPadding.left(),
        st::boxButtonPadding.top(),
        st::boxButtonPadding.right(),
        st::boxButtonPadding.bottom());
    buttonsLayout->setSpacing(Style::ConvertScale(8));
    buttonsLayout->addStretch(1);

    ::Ui::TextButton::Style cancelStyle;
    cancelStyle.bgOver = &st::lightButtonBgOver;
    cancelStyle.fg = &st::lightButtonFg;
    cancelStyle.radius = st::boxRadius;
    cancelStyle.height = st::boxButtonHeight;
    cancelStyle.paddingH = Style::ConvertScale(15);
    _cancel = new ::Ui::TextButton(
        QCoreApplication::translate("DialogsIncludeChatsBox", "Cancel"),
        cancelStyle,
        buttonsContainer);
    {
        auto f = st::baseFont(14);
        f.setWeight(QFont::DemiBold);
        _cancel->setFont(f);
    }
    connect(_cancel, &QAbstractButton::clicked, this, [this] { reject(); });
    buttonsLayout->addWidget(_cancel);

    ::Ui::TextButton::Style addStyle;
    addStyle.bg = &st::activeButtonBg;
    addStyle.bgOver = &st::activeButtonBgOver;
    addStyle.fg = &st::activeButtonFg;
    addStyle.radius = st::boxRadius;
    addStyle.height = st::boxButtonHeight;
    addStyle.paddingH = Style::ConvertScale(15);
    _add = new ::Ui::TextButton(
        QCoreApplication::translate("DialogsIncludeChatsBox", "Save"),
        addStyle,
        buttonsContainer);
    {
        auto f = st::baseFont(14);
        f.setWeight(QFont::DemiBold);
        _add->setFont(f);
    }
    _add->setFixedHeight(st::boxButtonHeight);
    connect(_add, &QAbstractButton::clicked, this, [this] { accept(); });
    buttonsLayout->addWidget(_add);

    // Wiring: search filters the list; clicking a row toggles a chip; chip
    // changes (incl. chip-× removal) re-sync the list checkmarks.
    connect(_chipBar->inputField(), &QLineEdit::textChanged, this,
            [this](const QString &query) { _inner->setFilter(query); });
    connect(_inner, &ChatPickInner::roomClicked, this,
            [this](const QString &id) { toggleRoom(id); });
    connect(_chipBar, &ChatChipBar::chipAdded, this,
            [this](const QString &) { syncListSelection(); });
    connect(_chipBar, &ChatChipBar::chipRemoved, this,
            [this](const QString &) { syncListSelection(); });

    QTimer::singleShot(0, _chipBar->inputField(), [this] {
        _chipBar->inputField()->setFocus();
    });

    // Resolve avatar URLs.
    if (bridge) {
        connect(bridge, &ProtocolBridge::mediaResolved,
            this, [this](bool success, const QString &mxcUrl, const QString &localPath) {
                if (!success || localPath.isEmpty()) {
                    if (mxcUrl.startsWith(QStringLiteral("mxc://"))) {
                        MediaCache::clearRequested(mxcUrl);
                    }
                    return;
                }
                MediaCache::insertPath(mxcUrl, localPath);
                if (_inner) {
                    _inner->update();
                }
                if (_chipBar) {
                    _chipBar->update();
                }
            });
        for (const auto &room : rooms) {
            if (room.avatarUrl.startsWith(QStringLiteral("mxc://"))
                    && MediaCache::needsResolution(room.avatarUrl)) {
                MediaCache::markRequested(room.avatarUrl);
                bridge->resolveAvatar(room.avatarUrl);
            }
        }
    }
}

QSet<QString> DialogsIncludeChatsBox::currentChipIds() const {
    return _chipBar ? _chipBar->currentIds() : QSet<QString>();
}

void DialogsIncludeChatsBox::toggleRoom(const QString &roomId) {
    if (!_chipBar) {
        return;
    }
    if (_chipBar->hasChip(roomId)) {
        _chipBar->removeChip(roomId);
    } else {
        _chipBar->addChip(_entryById.value(roomId));
    }
    // chipAdded/chipRemoved → syncListSelection().
}

void DialogsIncludeChatsBox::syncListSelection() {
    if (_inner) {
        _inner->setSelected(currentChipIds());
    }
}

int DialogsIncludeChatsBox::exec() {
    const auto restoreFocus = ::TeleMatrix::Focus::saveFocusForPopup();

    raise();
    show();
    setFocus();

    if (_a_shown) _a_shown->start();
    if (_a_layerShown) _a_layerShown->start();

    QEventLoop loop;
    _loop = &loop;
    loop.exec();
    _loop = nullptr;

    hide();
    ::TeleMatrix::Focus::restoreFocusAfterPopup(restoreFocus);
    return _result;
}

void DialogsIncludeChatsBox::accept() {
    _result_selected = currentChipIds();
    _result = Accepted;
    if (_loop) _loop->quit();
}

void DialogsIncludeChatsBox::reject() {
    _result = Rejected;
    if (_loop) _loop->quit();
}

QSet<QString> DialogsIncludeChatsBox::selectedRoomIds() const {
    return _result_selected;
}

void DialogsIncludeChatsBox::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setOpacity(_bgOpacity);
    p.fillRect(rect(), st::layerBg);
    if (_panel && _layerOpacity > 0) {
        p.setOpacity(_layerOpacity);
        paintBoxShadow(p, _panel->geometry());
    }
}

void DialogsIncludeChatsBox::mousePressEvent(QMouseEvent *event) {
    if (_panel && !_panel->geometry().contains(event->pos())) {
        reject();
        return;
    }
    QWidget::mousePressEvent(event);
}

void DialogsIncludeChatsBox::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool DialogsIncludeChatsBox::eventFilter(QObject *obj, QEvent *event) {
    if (obj == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace TeleMatrix

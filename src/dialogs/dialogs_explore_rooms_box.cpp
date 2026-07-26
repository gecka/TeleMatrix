// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "dialogs_explore_rooms_box.h"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QEnterEvent>
#include <QEventLoop>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include "dialogs/room_directory_filter.h"
#include "protocol/media_cache.h"
#include "protocol/protocol_bridge.h"
#include "styles/style_constants.h"
#include "ui/empty_userpic.h"
#include "ui/focus_restore.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/close_button.h"

namespace TeleMatrix {

namespace {

constexpr int kShadowExtend = 10;
constexpr int kSearchDelayMs = 180;
constexpr int kListVisibleRows = 6;
constexpr int kPageLimit = 50;
// Start the next page while there is still a screenful to scroll through.
constexpr int kLoadMoreThresholdPx = 200;

constexpr int kRowHeight = 64;
constexpr int kPhotoSize = 42;
constexpr int kPhotoX = 16;
constexpr int kPhotoY = 11;
constexpr int kNameX = 74;
constexpr int kNameY = 12;
constexpr int kTopicY = 36;
constexpr int kChevronWidth = 20;
constexpr int kListPaddingTop = 10;
constexpr int kListPaddingBottom = 10;
// Floor only. The box grows to whatever the window allows — see updateListHeight().
constexpr int kListMinHeight =
    kListPaddingTop
    + (kRowHeight * kListVisibleRows)
    + kListPaddingBottom;
// Breathing room above and below the box so it never touches the window edges.
constexpr int kPanelVerticalMargin = 24;

QString directoryHintText() {
    return QCoreApplication::translate(
        "DialogsExploreRoomsBox",
        "Search the public rooms on your homeserver.");
}

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

void drawSearchIcon(QPainter &p, int x, int y, const QColor &color) {
    PainterHighQualityEnabler hq(p);
    p.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(x + 7.5, y + 7.5), 5.5, 5.5);
    p.drawLine(QPointF(x + 11.5, y + 11.5), QPointF(x + 15, y + 15));
}

class SearchIconPainter final : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        drawSearchIcon(p, 10, 9, st::menuIconFg);
    }
};

void drawChevron(QPainter &p, int cx, int cy, const QColor &color) {
    PainterHighQualityEnabler hq(p);
    p.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawLine(QPointF(cx - 2.5, cy - 5), QPointF(cx + 2.5, cy));
    p.drawLine(QPointF(cx + 2.5, cy), QPointF(cx - 2.5, cy + 5));
}

// The box is twice the usual wide-box width. Computed at call time (a static const derived from a
// style value would hit the static-init order problem).
int panelWidth() {
    return 2 * st::boxWideWidth;
}

/// The right-hand badge: what this row is, in as few words as possible. Spaces carry their "Space"
/// marker as a chip next to the name instead (see paintRow), so here they show only the room count.
QString badgeFor(const RoomDirectoryEntry &entry) {
    if (entry.isSpace) {
        return entry.childrenCount > 0
            ? QCoreApplication::translate(
                "DialogsExploreRoomsBox", "%n room(s)", nullptr, entry.childrenCount)
            : QString();
    }
    return QCoreApplication::translate(
        "DialogsExploreRoomsBox", "%n member(s)", nullptr, entry.memberCount);
}

/// The second line: the topic, or the alias when there is no topic (better than nothing, and it is
/// what the user would type to find the room again).
QString subtitleFor(const RoomDirectoryEntry &entry) {
    if (!entry.topic.isEmpty()) {
        return entry.topic.simplified();
    }
    return entry.canonicalAlias;
}

// "<" back chevron to the left of the title (shown when drilled into a space), matching the
// rooms-toolbar pinned-list back arrow.
constexpr int kBackButtonSize = 32;
constexpr int kBackLeftMargin = 6;
class BackChevronButton final : public QAbstractButton {
public:
    explicit BackChevronButton(QWidget *parent) : QAbstractButton(parent) {
        setCursor(Qt::PointingHandCursor);
        setFixedSize(kBackButtonSize, kBackButtonSize);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        const auto color = _hovered ? st::menuIconFgOver : st::menuIconFg;
        p.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        const auto cx = width() / 2.0;
        const auto cy = height() / 2.0;
        p.drawLine(QPointF(cx + 3, cy - 6), QPointF(cx - 4, cy));
        p.drawLine(QPointF(cx - 4, cy), QPointF(cx + 3, cy + 6));
    }
    void enterEvent(QEnterEvent *) override { _hovered = true; update(); }
    void leaveEvent(QEvent *) override { _hovered = false; update(); }

private:
    bool _hovered = false;
};

} // namespace

ExploreRoomListInner::ExploreRoomListInner(QWidget *parent)
: QWidget(parent) {
    setMouseTracking(true);
    setStatusText(directoryHintText());
}

void ExploreRoomListInner::setEntries(const QVector<RoomDirectoryEntry> &entries) {
    _entries = entries;
    _statusText.clear();
    _hovered = -1;
    setFixedHeight(contentHeight());
    update();
}

void ExploreRoomListInner::setStatusText(const QString &text) {
    _entries.clear();
    _statusText = text;
    _hovered = -1;
    setFixedHeight(contentHeight());
    setCursor(Qt::ArrowCursor);
    update();
}

QVector<RoomDirectoryEntry> ExploreRoomListInner::entries() const {
    return _entries;
}

void ExploreRoomListInner::setViewportHeight(int height) {
    if (_viewportHeight == height) {
        return;
    }
    _viewportHeight = height;
    setFixedHeight(contentHeight());
    update();
}

QSize ExploreRoomListInner::sizeHint() const {
    return { width(), contentHeight() };
}

QSize ExploreRoomListInner::minimumSizeHint() const {
    return { 0, contentHeight() };
}

int ExploreRoomListInner::contentHeight() const {
    // Fill at least the viewport so the empty-state text centres over the whole visible area and a
    // short result set doesn't leave a gap under the last row.
    const int floor = qMax(kListMinHeight, _viewportHeight);
    if (_entries.isEmpty()) {
        return floor;
    }
    return qMax(
        floor,
        kListPaddingTop + _entries.size() * kRowHeight + kListPaddingBottom);
}

void ExploreRoomListInner::paintEvent(QPaintEvent *e) {
    QPainter p(this);
    const auto r = e->rect();
    p.setClipRect(r);
    p.fillRect(r, st::windowBg);

    if (_entries.isEmpty()) {
        p.setFont(st::normalFont);
        p.setPen(st::windowSubTextFg);
        p.drawText(rect().adjusted(24, 0, -24, 0),
            int(Qt::AlignCenter | Qt::TextWordWrap),
            _statusText.isEmpty() ? directoryHintText() : _statusText);
        return;
    }

    const int yFrom = r.y();
    const int yTo = r.y() + r.height();
    const int firstRow = qMax(0, (yFrom - kListPaddingTop) / kRowHeight);
    const int lastRow = qMin(
        _entries.size() - 1,
        (yTo - kListPaddingTop + kRowHeight - 1) / kRowHeight);

    for (int i = firstRow; i <= lastRow; ++i) {
        paintRow(p, i, i == _hovered);
    }
}

void ExploreRoomListInner::paintRow(QPainter &p, int index, bool hovered) {
    const auto &entry = _entries[index];
    const int y = kListPaddingTop + index * kRowHeight;
    const int w = width();

    p.fillRect(0, y, w, kRowHeight, hovered ? st::windowBgOver : st::windowBg);

    const QRect avatarRect(kPhotoX, y + kPhotoY, kPhotoSize, kPhotoSize);
    bool paintedAvatar = false;
    if (!entry.avatarUrl.isEmpty()) {
        const auto dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
        const auto avatar = MediaCache::loadAvatarPixmapAsync(
            entry.avatarUrl, kPhotoSize, dpr, this, avatarRect);
        if (!avatar.isNull()) {
            p.drawPixmap(avatarRect.topLeft(), avatar);
            paintedAvatar = true;
        }
    }
    if (!paintedAvatar) {
        Ui::EmptyUserpic::paint(
            p, entry.roomId, entry.name,
            avatarRect.x(), avatarRect.y(), avatarRect.width());
    }

    // A space is drilled into, so it gets a chevron and the text stops short of it.
    const int chevronSkip = entry.isSpace ? kChevronWidth : 0;
    const int rightEdge = w - kPhotoX - chevronSkip;

    const auto badge = badgeFor(entry);
    const QFontMetrics badgeFm(st::normalFont);
    const int badgeW = badge.isEmpty() ? 0 : badgeFm.horizontalAdvance(badge) + 12;

    // A "Space" chip drawn inline right after a space's name — styled like the "admin" badge in
    // the room members list: a solid accent-filled pill with light text.
    const auto chipText = entry.isSpace
        ? QCoreApplication::translate("DialogsExploreRoomsBox", "Space")
        : QString();
    const auto chipFont = st::baseFont(10, true);
    const QFontMetrics chipFm(chipFont);
    const int kChipPadX = 10;
    const int kChipGap = 8;
    const int chipW = chipText.isEmpty()
        ? 0
        : chipFm.horizontalAdvance(chipText) + kChipPadX;

    p.setFont(st::semiboldFont);
    p.setPen(st::windowFg);
    const QFontMetrics nameFm(st::semiboldFont);
    const int nameReserve = badgeW + (chipW > 0 ? chipW + kChipGap : 0);
    const int nameW = qMax(0, rightEdge - kNameX - nameReserve);
    const auto elidedName = nameFm.elidedText(entry.name, Qt::ElideRight, nameW);
    p.drawText(kNameX, y + kNameY + nameFm.ascent(), elidedName);

    if (chipW > 0) {
        PainterHighQualityEnabler hq(p);
        const int chipH = 16;
        const int chipX = kNameX + nameFm.horizontalAdvance(elidedName) + kChipGap;
        const int chipY = y + kNameY + (nameFm.height() - chipH) / 2;
        const QRect chipRect(chipX, chipY, chipW, chipH);
        p.setPen(Qt::NoPen);
        p.setBrush(st::windowActiveTextFg);
        p.drawRoundedRect(chipRect, 4, 4);
        p.setFont(chipFont);
        p.setPen(st::activeButtonFg);
        p.drawText(chipRect, Qt::AlignCenter, chipText);
    }

    if (!badge.isEmpty()) {
        p.setFont(st::normalFont);
        p.setPen(st::windowSubTextFg);
        p.drawText(
            QRect(rightEdge - badgeW, y + kNameY, badgeW, nameFm.height()),
            Qt::AlignRight | Qt::AlignVCenter,
            badge);
    }

    const auto subtitle = subtitleFor(entry);
    if (!subtitle.isEmpty()) {
        p.setFont(st::normalFont);
        p.setPen(hovered ? st::windowSubTextFgOver : st::windowSubTextFg);
        const QFontMetrics subFm(st::normalFont);
        p.drawText(
            kNameX,
            y + kTopicY + subFm.ascent(),
            subFm.elidedText(subtitle, Qt::ElideRight, qMax(0, rightEdge - kNameX)));
    }

    if (entry.isSpace) {
        drawChevron(p, w - kPhotoX - kChevronWidth / 2, y + kRowHeight / 2,
            hovered ? st::windowSubTextFgOver : st::windowSubTextFg);
    }
}

int ExploreRoomListInner::indexAt(const QPoint &pos) const {
    if (_entries.isEmpty()) {
        return -1;
    }
    const int row = (pos.y() - kListPaddingTop) / kRowHeight;
    if (row < 0 || row >= _entries.size()) {
        return -1;
    }
    return row;
}

void ExploreRoomListInner::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) {
        return;
    }
    const auto index = indexAt(e->pos());
    if (index < 0) {
        return;
    }
    const auto &entry = _entries[index];
    if (entry.isSpace) {
        emit spaceClicked(entry.roomId, entry.name);
    } else {
        // Choosing a room closes the box, which hides without a leave event — reset the hover
        // cursor now so a "hand" pointer doesn't linger over the timeline underneath.
        setCursor(Qt::ArrowCursor);
        emit roomClicked(entry.roomId, entry.via);
    }
}

void ExploreRoomListInner::mouseMoveEvent(QMouseEvent *e) {
    const auto index = indexAt(e->pos());
    if (index == _hovered) {
        return;
    }
    _hovered = index;
    setCursor(index >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    update();
}

void ExploreRoomListInner::leaveEvent(QEvent *) {
    if (_hovered < 0) {
        return;
    }
    _hovered = -1;
    setCursor(Qt::ArrowCursor);
    update();
}

DialogsExploreRoomsBox::DialogsExploreRoomsBox(
    ProtocolBridge *bridge,
    QWidget *parent)
: QWidget(parent ? parent->window() : nullptr)
, _bridge(bridge) {
    if (parentWidget()) {
        setGeometry(parentWidget()->rect());
        parentWidget()->installEventFilter(this);
    }
    setFocusPolicy(Qt::StrongFocus);

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
    _panel->setFixedWidth(panelWidth());
    // The drop shadow is painted by this widget OUTSIDE the panel rect, so a
    // panel resize/re-centre must repaint the whole overlay or the ring at the
    // old edges survives as an artifact.
    _panel->installEventFilter(this);
    root->addWidget(_panel, 0, Qt::AlignHCenter);
    root->addStretch(1);

    auto *panelLayout = new QVBoxLayout(_panel);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(0);

    auto *titleBar = new QWidget(_panel);
    titleBar->setFixedHeight(st::boxTitleHeight);
    panelLayout->addWidget(titleBar);

    _titleText = new QLabel(titleBar);
    _titleText->setFont(st::boxTitleFont);
    {
        QPalette pal = _titleText->palette();
        pal.setColor(QPalette::WindowText, st::boxTitleFg);
        _titleText->setPalette(pal);
    }
    _titleText->move(st::boxTitlePosition.x(), st::boxTitlePosition.y());

    ::Ui::TextButton::Style linkStyle;
    linkStyle.bgOver = &st::lightButtonBgOver;
    linkStyle.fg = &st::lightButtonFg;
    linkStyle.radius = st::boxRadius;
    linkStyle.height = st::boxButtonHeight;
    linkStyle.paddingH = 12;

    _back = new BackChevronButton(titleBar);
    _back->hide();
    connect(_back, &QAbstractButton::clicked, this, [this] { leaveSpace(); });

    // Shared top-right close (×). The "<" chevron above is top-LEFT and only steps
    // back through spaces; this cross dismisses the whole box, matching Cancel /
    // Escape / an outside click.
    auto *close = new ::Ui::CloseButton(titleBar);
    close->move(panelWidth() - st::settingsCloseButtonSize, 0);
    connect(close, &::Ui::CloseButton::clicked, this, [this] { reject(); });

    auto *searchContainer = new QWidget(_panel);
    searchContainer->setFixedHeight(
        st::boxSearchPadding.top()
        + st::boxSearchFieldHeight
        + st::boxSearchPadding.bottom());
    panelLayout->addWidget(searchContainer);

    _searchField = new QLineEdit(searchContainer);
    _searchField->setFrame(false);
    _searchField->setAttribute(Qt::WA_MacShowFocusRect, false);
    _searchField->setFixedHeight(st::boxSearchFieldHeight);
    const int searchIconSkip = 36;
    _searchField->setGeometry(
        st::boxSearchPadding.left() + searchIconSkip,
        st::boxSearchPadding.top(),
        panelWidth() - st::boxSearchPadding.left()
            - st::boxSearchPadding.right() - searchIconSkip,
        st::boxSearchFieldHeight);
    _searchField->setFont(st::baseFont(13));
    {
        QPalette pal = _searchField->palette();
        pal.setColor(QPalette::Base, Qt::transparent);
        pal.setColor(QPalette::Text, st::windowFg);
        _searchField->setPalette(pal);
    }
    _searchField->setTextMargins(0, 0, 0, 0);

    auto *searchIcon = new SearchIconPainter(searchContainer);
    searchIcon->setFixedSize(searchIconSkip, st::boxSearchFieldHeight);
    searchIcon->move(st::boxSearchPadding.left(), st::boxSearchPadding.top());
    searchIcon->setAttribute(Qt::WA_TransparentForMouseEvents);

    auto *searchSep = new QWidget(_panel);
    searchSep->setFixedHeight(1);
    searchSep->setAutoFillBackground(true);
    {
        QPalette pal = searchSep->palette();
        pal.setColor(QPalette::Window, st::shadowFg);
        searchSep->setPalette(pal);
    }
    panelLayout->addWidget(searchSep);

    _inner = new ExploreRoomListInner(nullptr);

    _scroll = new QScrollArea(_panel);
    _scroll->setFrameShape(QFrame::NoFrame);
    _scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    _scroll->setWidgetResizable(true);
    _scroll->setFixedHeight(kListMinHeight);
    _scroll->setWidget(_inner);
    makeScrollTransparent(_scroll);
    panelLayout->addWidget(_scroll);

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
    buttonsLayout->setSpacing(8);
    buttonsLayout->addStretch(1);

    _cancel = new ::Ui::TextButton(
        QCoreApplication::translate("DialogsExploreRoomsBox", "Cancel"),
        linkStyle,
        buttonsContainer);
    {
        auto f = st::baseFont(14);
        f.setWeight(QFont::DemiBold);
        _cancel->setFont(f);
    }
    connect(_cancel, &QAbstractButton::clicked, this, [this] { reject(); });
    buttonsLayout->addWidget(_cancel);

    _searchTimer = new QTimer(this);
    _searchTimer->setSingleShot(true);
    connect(_searchTimer, &QTimer::timeout,
        this, &DialogsExploreRoomsBox::triggerDirectorySearch);

    connect(_searchField, &QLineEdit::textChanged,
        this, &DialogsExploreRoomsBox::onSearchTextChanged);

    connect(_inner, &ExploreRoomListInner::spaceClicked,
        this, &DialogsExploreRoomsBox::enterSpace);
    connect(_inner, &ExploreRoomListInner::roomClicked,
        this, &DialogsExploreRoomsBox::chooseRoom);

    if (auto *bar = _scroll->verticalScrollBar()) {
        connect(bar, &QScrollBar::valueChanged,
            this, [this] { maybeLoadMore(); });
    }

    if (_bridge) {
        connect(_bridge, &ProtocolBridge::roomDirectoryPageReady,
            this, &DialogsExploreRoomsBox::onPageReady);
        connect(_bridge, &ProtocolBridge::roomDirectoryFailed,
            this, &DialogsExploreRoomsBox::onRequestFailed);
        connect(_bridge, &ProtocolBridge::mediaResolved,
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
        });
    }

    updateHeader();
    updateListHeight();

    // An empty query is meaningful: it browses the whole directory. That is the landing state, and
    // triggerDirectorySearch() shows "Loading…" (not "Searching…") for it.
    triggerDirectorySearch();

    QTimer::singleShot(0, _searchField, [this] {
        _searchField->setFocus();
    });
}

int DialogsExploreRoomsBox::exec() {
    const auto restoreFocus = ::TeleMatrix::Focus::saveFocusForPopup();

    raise();
    show();
    setFocus();

    if (_a_shown) {
        _a_shown->start();
    }
    if (_a_layerShown) {
        _a_layerShown->start();
    }

    QEventLoop loop;
    _loop = &loop;
    loop.exec();
    _loop = nullptr;

    hide();
    // Only hand focus back to whatever had it before (the chat search field) when the box was
    // dismissed without a choice. Choosing a room opens a preview that should own focus — restoring
    // the search field there would steal it and pop the cursor into the filter.
    if (_result != Accepted) {
        ::TeleMatrix::Focus::restoreFocusAfterPopup(restoreFocus);
    }
    return _result;
}

QString DialogsExploreRoomsBox::chosenRoomId() const {
    return _chosenRoomId;
}

QStringList DialogsExploreRoomsBox::chosenVia() const {
    return _chosenVia;
}

void DialogsExploreRoomsBox::accept() {
    _result = Accepted;
    if (_loop) {
        _loop->quit();
    }
}

void DialogsExploreRoomsBox::reject() {
    if (_bridge && _activeRequestId) {
        _bridge->cancelRoomDirectoryRequest(_activeRequestId);
    }
    _result = Rejected;
    if (_loop) {
        _loop->quit();
    }
}

void DialogsExploreRoomsBox::updateHeader() {
    const bool inSpace = (_view == View::Space);

    _titleText->setText(inSpace
        ? _spaceName
        : QCoreApplication::translate("DialogsExploreRoomsBox", "Explore public rooms"));
    _titleText->adjustSize();

    _searchField->setPlaceholderText(inSpace
        ? QCoreApplication::translate(
            "DialogsExploreRoomsBox", "Search rooms in this space")
        : QCoreApplication::translate(
            "DialogsExploreRoomsBox", "Search public rooms"));

    // "<" chevron on the left of the title (like the rooms toolbar's pinned back), shown only
    // when there is somewhere to go back TO: the directory (normal open) or a parent space
    // (drilled in). A box opened straight into a space has no directory home, so at its root
    // space the chevron is hidden — it appears once the user drills into a sub-space. The title
    // shifts right to reserve room for it only when it is shown.
    const bool canGoBack = inSpace && (!_rootIsSpace || !_spaceStack.isEmpty());
    _back->setVisible(canGoBack);
    _back->move(kBackLeftMargin, (st::boxTitleHeight - kBackButtonSize) / 2);
    _titleText->move(
        canGoBack ? (kBackLeftMargin + kBackButtonSize) : st::boxTitlePosition.x(),
        st::boxTitlePosition.y());
}

void DialogsExploreRoomsBox::onSearchTextChanged(const QString &text) {
    if (_view == View::Space) {
        // No server-side search inside a space — filter what we have paged in.
        applySpaceFilter();
        return;
    }

    _directoryQuery = text.trimmed();
    if (_searchTimer) {
        _searchTimer->stop();
        _searchTimer->start(kSearchDelayMs);
    }
}

void DialogsExploreRoomsBox::triggerDirectorySearch() {
    if (!_bridge) {
        return;
    }
    if (_activeRequestId) {
        _bridge->cancelRoomDirectoryRequest(_activeRequestId);
    }

    _directoryEntries.clear();
    _directoryNextToken.clear();
    _directoryDone = false;
    _loadingMore = false;
    // Browsing the whole directory (empty query) is a load; a typed query is a search.
    _inner->setStatusText(_directoryQuery.isEmpty()
        ? QCoreApplication::translate(
            "DialogsExploreRoomsBox", "Loading public rooms…")
        : QCoreApplication::translate(
            "DialogsExploreRoomsBox", "Searching…"));

    _activeRequestId = ++_requestCounter;
    _bridge->searchPublicRoomsAsync(_activeRequestId, _directoryQuery, kPageLimit);
}

void DialogsExploreRoomsBox::enterSpace(const QString &spaceId, const QString &name) {
    if (_view == View::Directory) {
        // Coming back to the directory must not refetch, so keep where we were in it.
        _directoryScrollPos = (_scroll && _scroll->verticalScrollBar())
            ? _scroll->verticalScrollBar()->value() : 0;
    } else if (_view == View::Space && !_spaceId.isEmpty()) {
        // Drilling into a sub-space: remember the current one so "back" returns here.
        _spaceStack.push_back({ _spaceId, _spaceName });
    }
    loadSpace(spaceId, name);
}

void DialogsExploreRoomsBox::loadSpace(const QString &spaceId, const QString &name) {
    if (!_bridge) {
        return;
    }
    if (_activeRequestId) {
        _bridge->cancelRoomDirectoryRequest(_activeRequestId);
    }
    if (_searchTimer) {
        _searchTimer->stop();
    }

    _view = View::Space;
    _spaceId = spaceId;
    _spaceName = name;
    _spaceEntries.clear();
    _spaceNextToken.clear();
    _spaceDone = false;
    _loadingMore = false;

    // setText() would re-enter onSearchTextChanged and fire a directory search.
    {
        const QSignalBlocker blocker(_searchField);
        _searchField->clear();
    }

    updateHeader();
    _inner->setStatusText(QCoreApplication::translate(
        "DialogsExploreRoomsBox", "Loading rooms…"));

    _activeRequestId = ++_requestCounter;
    _bridge->getSpaceChildrenAsync(_activeRequestId, _spaceId, kPageLimit);
}

void DialogsExploreRoomsBox::leaveSpace() {
    if (_bridge && _activeRequestId) {
        _bridge->cancelRoomDirectoryRequest(_activeRequestId);
        _activeRequestId = 0;
    }

    // Back to the parent space when we drilled into a sub-space; only the top level returns home.
    if (!_spaceStack.isEmpty()) {
        const auto parent = _spaceStack.takeLast();
        loadSpace(parent.first, parent.second);
        return;
    }

    _view = View::Directory;
    _spaceId.clear();
    _spaceName.clear();
    _spaceEntries.clear();
    _loadingMore = false;

    {
        const QSignalBlocker blocker(_searchField);
        _searchField->setText(_directoryQuery);
    }

    updateHeader();
    showEntries(_directoryEntries);

    if (auto *bar = _scroll->verticalScrollBar()) {
        bar->setValue(_directoryScrollPos);
    }
}

void DialogsExploreRoomsBox::chooseRoom(const QString &roomId, const QStringList &via) {
    if (roomId.isEmpty()) {
        return;
    }
    _chosenRoomId = roomId;
    _chosenVia = via;
    accept();
}

void DialogsExploreRoomsBox::onPageReady(const RoomDirectoryPage &page) {
    if (page.requestId != _activeRequestId) {
        return; // superseded by a newer query, or answered after we left the view
    }
    // The user may have hit Back before this landed.
    if (page.isSpaceChildren != (_view == View::Space)
        || (page.isSpaceChildren && page.spaceId != _spaceId)) {
        return;
    }

    _activeRequestId = 0;
    _loadingMore = false;

    if (_view == View::Space) {
        _spaceEntries.append(page.entries);
        _spaceNextToken = page.nextToken;
        _spaceDone = page.done;
        applySpaceFilter();
        resolveAvatars(page.entries);
        return;
    }

    _directoryEntries.append(page.entries);
    _directoryNextToken = page.nextToken;
    _directoryDone = page.done;
    showEntries(_directoryEntries);
    resolveAvatars(page.entries);
}

void DialogsExploreRoomsBox::onRequestFailed(quint64 requestId, const QString &error) {
    if (requestId != _activeRequestId) {
        return;
    }
    _activeRequestId = 0;
    _loadingMore = false;

    // Show the homeserver's own words — "rate limited", "directory disabled" and "no such space" are
    // all meaningfully different, and we cannot phrase them better than it can.
    if (_inner->entries().isEmpty()) {
        _inner->setStatusText(error.isEmpty()
            ? QCoreApplication::translate(
                "DialogsExploreRoomsBox", "Couldn't load rooms.")
            : error);
    }
}

void DialogsExploreRoomsBox::applySpaceFilter() {
    const auto needle = _searchField ? _searchField->text() : QString();
    const auto filtered = filterRoomEntries(_spaceEntries, needle);

    if (filtered.isEmpty()) {
        _inner->setStatusText(_spaceEntries.isEmpty()
            ? QCoreApplication::translate(
                "DialogsExploreRoomsBox", "This space has no rooms you can see.")
            : QCoreApplication::translate(
                "DialogsExploreRoomsBox", "No rooms match your filter."));
        return;
    }
    _inner->setEntries(filtered);
}

void DialogsExploreRoomsBox::showEntries(const QVector<RoomDirectoryEntry> &entries) {
    if (entries.isEmpty()) {
        // An unfiltered directory fetch that came back empty means the homeserver publishes no
        // public rooms at all: say so in the list and disable the search field, since there is
        // nothing to search. A non-empty query that matched nothing keeps the field enabled —
        // rooms exist, just not for this term.
        const bool serverHasNoRooms = _directoryQuery.isEmpty();
        _searchField->setEnabled(!serverHasNoRooms);
        _inner->setStatusText(serverHasNoRooms
            ? QCoreApplication::translate(
                "DialogsExploreRoomsBox",
                "No public rooms are published on this homeserver.")
            : QCoreApplication::translate(
                "DialogsExploreRoomsBox", "No public rooms match your search."));
        return;
    }
    _searchField->setEnabled(true);
    _inner->setEntries(entries);
}

void DialogsExploreRoomsBox::maybeLoadMore() {
    if (!_bridge || _loadingMore || _activeRequestId) {
        return;
    }

    const bool done = (_view == View::Space) ? _spaceDone : _directoryDone;
    const auto &token = (_view == View::Space) ? _spaceNextToken : _directoryNextToken;
    if (done || token.isEmpty()) {
        return;
    }

    auto *bar = _scroll->verticalScrollBar();
    if (!bar || bar->value() < bar->maximum() - kLoadMoreThresholdPx) {
        return;
    }

    _loadingMore = true;
    _activeRequestId = ++_requestCounter;
    if (_view == View::Space) {
        _bridge->getSpaceChildrenAsync(_activeRequestId, _spaceId, kPageLimit, token);
    } else {
        _bridge->searchPublicRoomsAsync(
            _activeRequestId, _directoryQuery, kPageLimit, token);
    }
}

void DialogsExploreRoomsBox::resolveAvatars(const QVector<RoomDirectoryEntry> &entries) {
    if (!_bridge) {
        return;
    }
    for (const auto &entry : entries) {
        if (entry.avatarUrl.startsWith(QStringLiteral("mxc://"))
            && MediaCache::needsResolution(entry.avatarUrl)) {
            MediaCache::markRequested(entry.avatarUrl);
            _bridge->resolveAvatar(entry.avatarUrl);
        }
    }
}

void DialogsExploreRoomsBox::paintEvent(QPaintEvent *) {
    QPainter p(this);

    p.setOpacity(_bgOpacity);
    p.fillRect(rect(), st::layerBg);

    if (_panel && _layerOpacity > 0) {
        p.setOpacity(_layerOpacity);
        paintBoxShadow(p, _panel->geometry());
    }
}

void DialogsExploreRoomsBox::mousePressEvent(QMouseEvent *event) {
    if (_panel && !_panel->geometry().contains(event->pos())) {
        reject();
        return;
    }
    QWidget::mousePressEvent(event);
}

void DialogsExploreRoomsBox::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        // Escape always closes the box; the "<" button is for stepping back through spaces.
        reject();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool DialogsExploreRoomsBox::eventFilter(QObject *obj, QEvent *event) {
    if (obj == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
        updateListHeight();
    } else if (obj == _panel
        && (event->type() == QEvent::Resize
            || event->type() == QEvent::Move)) {
        update();
    }
    return QWidget::eventFilter(obj, event);
}

void DialogsExploreRoomsBox::updateListHeight() {
    if (!_scroll || !_panel || !_inner) {
        return;
    }
    // Everything in the panel except the scrollable list has a fixed height; the list takes the rest.
    const int chrome =
        st::boxTitleHeight
        + (st::boxSearchPadding.top()
            + st::boxSearchFieldHeight
            + st::boxSearchPadding.bottom())
        + 1 // the search separator
        + (st::boxButtonPadding.top()
            + st::boxButtonHeight
            + st::boxButtonPadding.bottom());
    const int available = height() - 2 * kPanelVerticalMargin - chrome;
    const int listHeight = qMax(kListMinHeight, available);
    _scroll->setFixedHeight(listHeight);
    _inner->setViewportHeight(listHeight);
}

} // namespace TeleMatrix

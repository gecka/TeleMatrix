// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_forward_dialog.h"

#include <QEventLoop>
#include <QFont>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include "dialogs/saved_messages.h"
#include "protocol/media_cache.h"
#include "protocol/protocol_bridge.h"
#include "styles/style_constants.h"
#include "ui/empty_userpic.h"
#include "ui/focus_restore.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"

namespace TeleMatrix {

namespace {

// Box round shadow: 8px radius, extend = margins(10,10,10,10).
constexpr int kShadowExtend = 10;

// peerListBoxItem (from boxes.style) — row layout.
constexpr int kRowHeight = 56;
constexpr int kPhotoSize = 42;           // contactsPhotoSize
constexpr int kPhotoX = 16;              // photoPosition.x
constexpr int kPhotoY = 7;               // photoPosition.y
constexpr int kNameX = 74;               // namePosition.x
constexpr int kNameY = 9;                // namePosition.y
constexpr int kStatusX = 74;             // statusPosition.x
constexpr int kStatusY = 30;             // statusPosition.y
constexpr int kListPaddingTop = 10;      // membersMarginTop
constexpr int kListPaddingBottom = 10;   // membersMarginBottom

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

// Panel surface painted with live st:: colors (so it tracks theme changes)
// instead of a frozen stylesheet background.
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

// Make a scroll area paint no background of its own; the RoundedPanel behind it
// (st::boxBg) shows through, so it stays theme-correct without a frozen QSS bg.
void makeScrollTransparent(QScrollArea *scroll) {
    scroll->setAutoFillBackground(false);
    if (auto *viewport = scroll->viewport()) {
        viewport->setAutoFillBackground(false);
        viewport->setAttribute(Qt::WA_TranslucentBackground);
    }
}

/// Draw a search (magnifying glass) icon.
void drawSearchIcon(QPainter &p, int x, int y, const QColor &color) {
    PainterHighQualityEnabler hq(p);
    p.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    // Circle part (radius ~5px, centered at x+7, y+7).
    p.drawEllipse(QPointF(x + 7.5, y + 7.5), 5.5, 5.5);
    // Handle line.
    p.drawLine(QPointF(x + 11.5, y + 11.5), QPointF(x + 15, y + 15));
}

} // namespace

// ─────────────────────────────────────────────
// PeerListInner — custom-painted vertical list
// ─────────────────────────────────────────────

PeerListInner::PeerListInner(QWidget *parent)
    : QWidget(parent) {
    setMouseTracking(true);
}

void PeerListInner::setRooms(const QVector<RoomSummary> &rooms) {
    _allRooms = rooms;
    _selected = -1;
    _hovered = -1;
    _filtered.clear();
    for (int i = 0; i < _allRooms.size(); ++i) {
        _filtered.push_back(i);
    }
    setFixedHeight(contentHeight());
    update();
}

void PeerListInner::setSavedMessagesRoomId(const QString &roomId) {
    _savedRoomId = roomId;
    update();
}

void PeerListInner::setFilter(const QString &query) {
    const auto needle = query.trimmed().toLower();
    _filtered.clear();
    for (int i = 0; i < _allRooms.size(); ++i) {
        const auto &room = _allRooms[i];
        const auto name = room.displayName.isEmpty()
            ? room.roomId
            : room.displayName;
        if (needle.isEmpty()
            || name.toLower().contains(needle)
            || room.roomId.toLower().contains(needle)) {
            _filtered.push_back(i);
        }
    }
    _selected = -1;
    _hovered = -1;
    setFixedHeight(contentHeight());
    update();
}

QString PeerListInner::selectedRoomId() const {
    if (_selected < 0 || _selected >= _filtered.size()) {
        return {};
    }
    return _allRooms[_filtered[_selected]].roomId;
}

int PeerListInner::contentHeight() const {
    return kListPaddingTop
        + _filtered.size() * kRowHeight
        + kListPaddingBottom;
}

QSize PeerListInner::sizeHint() const {
    return { width(), contentHeight() };
}

QSize PeerListInner::minimumSizeHint() const {
    return sizeHint();
}

void PeerListInner::paintEvent(QPaintEvent *e) {
    QPainter p(this);
    const auto r = e->rect();
    p.setClipRect(r);

    // Fill background (contactsBg = windowBg = #fff).
    p.fillRect(r, st::windowBg);

    if (_filtered.isEmpty()) {
        p.setFont(st::normalFont);
        p.setPen(st::windowSubTextFg);
        p.drawText(rect(), int(Qt::AlignCenter),
                   tr("No chats found"));
        return;
    }

    // Calculate visible row range from clip rect.
    const int yFrom = r.y();
    const int yTo = r.y() + r.height();
    const int firstRow = qMax(0, (yFrom - kListPaddingTop) / kRowHeight);
    const int lastRow = qMin(
        (int)_filtered.size() - 1,
        (yTo - kListPaddingTop + kRowHeight - 1) / kRowHeight);

    for (int i = firstRow; i <= lastRow; ++i) {
        paintRow(p, i, i == _selected, i == _hovered);
    }
}

void PeerListInner::paintRow(
        QPainter &p,
        int filteredIndex,
        bool selected,
        bool hovered) {
    const auto &room = _allRooms[_filtered[filteredIndex]];
    const auto name = room.displayName.isEmpty()
        ? room.roomId
        : room.displayName;

    const int y = kListPaddingTop + filteredIndex * kRowHeight;
    const int w = width();

    // 1. Background fill (textBg / textBgOver).
    const auto &bg = (selected || hovered)
        ? st::windowBgOver    // contactsBgOver = #f1f1f1
        : st::windowBg;       // contactsBg = #ffffff
    p.fillRect(0, y, w, kRowHeight, bg);

    // 2. Userpic (resolved avatar image or fallback initial).
    const QRect userpicRect(kPhotoX, y + kPhotoY, kPhotoSize, kPhotoSize);
    const bool savedRow = room.roomId == SavedMessages::kPendingRoomId
        || (!_savedRoomId.isEmpty() && room.roomId == _savedRoomId);
	bool paintedAvatar = false;
    if (savedRow) {
        // The drawn bookmark always wins over any uploaded room avatar.
        Ui::EmptyUserpic::paintSavedMessages(
            p, userpicRect.x(), userpicRect.y(), userpicRect.width());
        paintedAvatar = true;
    }
	if (!paintedAvatar && !room.avatarUrl.isEmpty()) {
		const auto dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
		const auto avatar = MediaCache::loadAvatarPixmapAsync(
			room.avatarUrl,
			kPhotoSize,
			dpr,
			this,
			userpicRect);
		if (!avatar.isNull()) {
			p.drawPixmap(userpicRect.topLeft(), avatar);
			paintedAvatar = true;
		}
	}
    if (!paintedAvatar) {
        Ui::EmptyUserpic::paint(p, room.avatarEntityId.isEmpty() ? room.roomId : room.avatarEntityId, name, userpicRect.x(), userpicRect.y(), userpicRect.width());
    }

    // 3. Name (semiboldTextStyle, contactsNameFg).
    // skipRight = photoPosition.x = 16 (same padding on right side).
    const int skipRight = kPhotoX;
    const int namew = w - kNameX - skipRight;
    p.setFont(st::semiboldFont);
    p.setPen(st::windowFg); // contactsNameFg = boxTextFg = windowFg
    const QFontMetrics nameFm(st::semiboldFont);
    const auto elidedName = nameFm.elidedText(name, Qt::ElideRight, namew);
    p.drawText(kNameX, y + kNameY + nameFm.ascent(), elidedName);

    // 4. Status text (contactsStatusFont, contactsStatusFg).
    const auto &statusFg = hovered
        ? st::windowSubTextFgOver    // contactsStatusFgOver = #919191
        : st::windowSubTextFg;       // contactsStatusFg = #999999
    p.setFont(st::normalFont); // contactsStatusFont = font(fsize) = 13px
    p.setPen(statusFg);
    const int statusw = w - kStatusX - skipRight;
    const QFontMetrics statusFm(st::normalFont);
    // tdesktop-style captions: purpose line for Saved Messages, presence for
    // users, member count for rooms — never a message preview or raw id.
    auto status = QString();
    if (savedRow) {
        status = tr("Forward messages here for quick access");
    } else if (room.isDirect) {
        status = (room.peerPresence == 1)
            ? tr("online")
            : tr("last seen recently");
    } else {
        status = tr("%n member(s)", "",
            int(qMax(quint64(1), room.memberCount)));
    }
    const auto elidedStatus = statusFm.elidedText(
        status, Qt::ElideRight, statusw);
    p.drawText(kStatusX, y + kStatusY + statusFm.ascent(), elidedStatus);
}

int PeerListInner::indexAt(const QPoint &pos) const {
    if (_filtered.isEmpty()) {
        return -1;
    }
    const int row = (pos.y() - kListPaddingTop) / kRowHeight;
    if (row < 0 || row >= _filtered.size()) {
        return -1;
    }
    return row;
}

void PeerListInner::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) {
        return;
    }
    const int idx = indexAt(e->pos());
    if (idx < 0) {
        return;
    }
    _selected = idx;
    update();
    emit roomClicked(_allRooms[_filtered[idx]].roomId);
}

void PeerListInner::mouseMoveEvent(QMouseEvent *e) {
    const int idx = indexAt(e->pos());
    if (idx != _hovered) {
        _hovered = idx;
        setCursor(idx >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void PeerListInner::leaveEvent(QEvent *) {
    if (_hovered >= 0) {
        _hovered = -1;
        setCursor(Qt::ArrowCursor);
        update();
    }
}

// ─────────────────────────────────────────────
// HistoryForwardDialog — PeerListBox-style modal
// ─────────────────────────────────────────────

HistoryForwardDialog::HistoryForwardDialog(
    const QVector<RoomSummary> &rooms,
    ProtocolBridge *bridge,
    QWidget *parent)
    : QWidget(parent ? parent->window() : nullptr)
    , _rooms(rooms) {
    // The layer is a CHILD widget of the main window body.
    if (parentWidget()) {
        setGeometry(parentWidget()->rect());
        parentWidget()->installEventFilter(this);
    }

    setFocusPolicy(Qt::StrongFocus);

    _bgOpacity = 0.0;
    _layerOpacity = 0.0;

    // _a_shown: background overlay, easeOutCirc, 200ms.
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

    // _a_layerShown: box shadow, linear, 200ms.
    // The panel itself is shown/hidden directly to avoid QGraphicsOpacityEffect
    // rendering bugs where the scroll-area content stays transparent until an
    // interaction forces a child repaint.
    _a_layerShown = new QVariantAnimation(this);
    _a_layerShown->setDuration(200);
    _a_layerShown->setEasingCurve(QEasingCurve::Linear);
    _a_layerShown->setStartValue(0.0);
    _a_layerShown->setEndValue(1.0);
    connect(_a_layerShown, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) {
        _layerOpacity = value.toReal();
        // Show the panel on the first animation tick so it appears
        // together with the darkening overlay, not before it.
        if (_panel && !_panel->isVisible() && _layerOpacity > 0) {
            _panel->setVisible(true);
        }
        update();
    });

    // --- Root layout: centers the panel in the overlay ---
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addStretch(1);

    // --- Panel: the white box (364px wide = boxWideWidth) ---
    // Panel content width = st::boxWideWidth.
    _panel = new RoundedPanel(this);
    _panel->setVisible(false); // shown on first animation tick
    _panel->setFixedWidth(st::boxWideWidth);
    root->addWidget(_panel, 0, Qt::AlignHCenter);
    root->addStretch(1);

    auto *panelLayout = new QVBoxLayout(_panel);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(0);

    // --- Title bar: "Forward to..." + close X ---
    auto *titleBar = new QWidget(_panel);
    titleBar->setFixedHeight(st::boxTitleHeight);
    panelLayout->addWidget(titleBar);

    // Title label.
    auto *titleText = new QLabel(tr("Forward to..."), titleBar);
    titleText->setFont(st::boxTitleFont);
    {
        QPalette pal = titleText->palette();
        pal.setColor(QPalette::WindowText, st::boxTitleFg);
        titleText->setPalette(pal);
    }
    titleText->move(st::boxTitlePosition.x(), st::boxTitlePosition.y());

    // The forward dialog has no close X button.
    // Dismissed via Cancel button, Escape key, or outside click.

    // --- Search area ---
    auto *searchContainer = new QWidget(_panel);
    searchContainer->setFixedHeight(
        st::boxSearchPadding.top()
        + st::boxSearchFieldHeight
        + st::boxSearchPadding.bottom());
    panelLayout->addWidget(searchContainer);

    _searchField = new QLineEdit(searchContainer);
    _searchField->setPlaceholderText(tr("Search"));
    _searchField->setFrame(false);
    _searchField->setAttribute(Qt::WA_MacShowFocusRect, false);
    _searchField->setFixedHeight(st::boxSearchFieldHeight);
    const int searchIconSkip = 36; // defaultMultiSelect.fieldIconSkip
    _searchField->setGeometry(
        st::boxSearchPadding.left() + searchIconSkip,
        st::boxSearchPadding.top(),
        st::boxWideWidth - st::boxSearchPadding.left()
            - st::boxSearchPadding.right() - searchIconSkip,
        st::boxSearchFieldHeight);
    // Frameless + transparent background; live st:: colors via QPalette so the
    // field tracks theme changes (a QSS built from .name() would freeze them).
    _searchField->setFont(st::baseFont(13));
    {
        QPalette pal = _searchField->palette();
        pal.setColor(QPalette::Base, Qt::transparent);
        pal.setColor(QPalette::Text, st::windowFg);
        _searchField->setPalette(pal);
    }
    _searchField->setTextMargins(0, 0, 0, 0);

    // Search icon widget.
    class SearchIconPainter : public QWidget {
    public:
        using QWidget::QWidget;
    protected:
        void paintEvent(QPaintEvent *) override {
            QPainter p(this);
            drawSearchIcon(p, 10, 9, st::menuIconFg);
        }
    };
    auto *searchIcon = new SearchIconPainter(searchContainer);
    searchIcon->setFixedSize(searchIconSkip, st::boxSearchFieldHeight);
    searchIcon->move(st::boxSearchPadding.left(), st::boxSearchPadding.top());
    searchIcon->setAttribute(Qt::WA_TransparentForMouseEvents);

    // Separator line below search. shadowFg is #00000018 (semi-transparent);
    // QPalette::Window keeps the alpha and re-resolves on theme change.
    auto *searchSep = new QWidget(_panel);
    searchSep->setFixedHeight(1);
    searchSep->setAutoFillBackground(true);
    {
        QPalette pal = searchSep->palette();
        pal.setColor(QPalette::Window, st::shadowFg);
        searchSep->setPalette(pal);
    }
    panelLayout->addWidget(searchSep);

    // --- Peer list (inside scroll area) ---
    _inner = new PeerListInner(nullptr);
    if (bridge) {
        _inner->setSavedMessagesRoomId(bridge->savedMessagesRoomId());
    }
    _inner->setRooms(_rooms);

    const int listH = _inner->sizeHint().height();
    const int scrollH = qMin(listH, st::boxMaxListHeight);

    _scroll = new QScrollArea(_panel);
    _scroll->setFrameShape(QFrame::NoFrame);
    _scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    _scroll->setWidgetResizable(true);
    _scroll->setFixedHeight(scrollH);
    _scroll->setWidget(_inner);
    makeScrollTransparent(_scroll);
    panelLayout->addWidget(_scroll);

    // --- Buttons area ---
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

    // Cancel button only (defaultBoxButton = defaultLightButton style).
    // The forward dialog has no Send button — clicking a row forwards immediately.
    ::Ui::TextButton::Style cancelStyle;
    cancelStyle.bgOver = &st::lightButtonBgOver; // transparent until hovered
    cancelStyle.fg = &st::lightButtonFg;
    cancelStyle.radius = st::boxRadius;
    cancelStyle.height = st::boxButtonHeight;
    cancelStyle.paddingH = 15;
    _cancel = new ::Ui::TextButton(tr("Cancel"), cancelStyle, buttonsContainer);
    {
        auto f = st::baseFont(14);
        f.setWeight(QFont::DemiBold);
        _cancel->setFont(f);
    }
    connect(_cancel, &QAbstractButton::clicked, this, [this] { reject(); });
    buttonsLayout->addWidget(_cancel);

    // --- Connections ---
    connect(_searchField, &QLineEdit::textChanged, this,
            [this](const QString &query) {
        _inner->setFilter(query);
        const int listH = _inner->sizeHint().height();
        _scroll->setFixedHeight(qMin(listH, st::boxMaxListHeight));
    });

    // Row click calls the callback directly, which closes the dialog.
    // Single click = forward.
    connect(_inner, &PeerListInner::roomClicked, this,
            [this](const QString &roomId) {
        if (!roomId.isEmpty()) {
            accept();
        }
    });

    // Focus search field on open.
    QTimer::singleShot(0, _searchField, [this] {
        _searchField->setFocus();
    });

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
            });
        for (const auto &room : _rooms) {
            if (room.avatarUrl.startsWith(QStringLiteral("mxc://"))
                    && MediaCache::needsResolution(room.avatarUrl)) {
                MediaCache::markRequested(room.avatarUrl);
                bridge->resolveAvatar(room.avatarUrl);
            }
        }
    }
}

int HistoryForwardDialog::exec() {
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
    ::TeleMatrix::Focus::restoreFocusAfterPopup(restoreFocus);
    return _result;
}

void HistoryForwardDialog::accept() {
    _result = Accepted;
    if (_loop) _loop->quit();
}

void HistoryForwardDialog::reject() {
    _result = Rejected;
    if (_loop) _loop->quit();
}

QString HistoryForwardDialog::selectedRoomId() const {
    return _inner ? _inner->selectedRoomId() : QString();
}

void HistoryForwardDialog::paintEvent(QPaintEvent *) {
    QPainter p(this);

    // Background overlay: opacity = bgOpacity, filled with layerBg.
    p.setOpacity(_bgOpacity);
    p.fillRect(rect(), st::layerBg);

    // Box shadow layer: opacity = layerOpacity.
    if (_panel && _layerOpacity > 0) {
        p.setOpacity(_layerOpacity);
        paintBoxShadow(p, _panel->geometry());
    }
}

void HistoryForwardDialog::mousePressEvent(QMouseEvent *event) {
    if (_panel && !_panel->geometry().contains(event->pos())) {
        reject();
        return;
    }
    QWidget::mousePressEvent(event);
}

void HistoryForwardDialog::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool HistoryForwardDialog::eventFilter(QObject *obj, QEvent *event) {
    if (obj == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace TeleMatrix

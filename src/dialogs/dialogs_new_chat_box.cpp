// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "dialogs_new_chat_box.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QScrollArea>
#include <QSet>
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
constexpr int kSearchDelayMs = 180;
constexpr int kSearchMinQueryLength = 2;
constexpr int kListVisibleRows = 5;

constexpr int kRowHeight = 56;
constexpr int kPhotoSize = 42;
constexpr int kPhotoX = 16;
constexpr int kPhotoY = 7;
constexpr int kNameX = 74;
constexpr int kNameY = 9;
constexpr int kStatusX = 74;
constexpr int kStatusY = 30;
constexpr int kListPaddingTop = 10;
constexpr int kListPaddingBottom = 10;
constexpr int kListHeight =
    kListPaddingTop
    + (kRowHeight * kListVisibleRows)
    + kListPaddingBottom;
// Breathing room above and below the box so it never touches the window edges.
constexpr int kPanelVerticalMargin = 24;

QString searchHintText() {
    return QCoreApplication::translate(
        "DialogsNewChatBox",
        "Type a name or Matrix user ID to search the homeserver.");
}

bool isExactUserId(const QString &text) {
    return text.startsWith(QLatin1Char('@'))
        && text.contains(QLatin1Char(':'));
}

QString displayNameFor(const UserProfile &user) {
    return user.displayName.isEmpty() ? user.userId : user.displayName;
}

QVector<UserProfile> initialUsersFromBridge(ProtocolBridge *bridge) {
    QVector<UserProfile> users;
    if (!bridge) {
        return users;
    }

    QSet<QString> seen;
    const auto ownUserId = bridge->cachedSessionInfo().userId;
    for (const auto &room : bridge->cachedRooms()) {
        if (!room.isDirect
            || room.avatarEntityId.isEmpty()
            || !room.avatarEntityId.startsWith(QLatin1Char('@'))
            || room.avatarEntityId == ownUserId
            || seen.contains(room.avatarEntityId)) {
            continue;
        }
        seen.insert(room.avatarEntityId);
        UserProfile user;
        user.userId = room.avatarEntityId;
        user.displayName = room.displayName;
        user.avatarUrl = room.avatarUrl;
        users.push_back(user);
    }
    return users;
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

} // namespace

NewChatUserListInner::NewChatUserListInner(QWidget *parent)
: QWidget(parent) {
    setMouseTracking(true);
    setStatusText(searchHintText());
}

void NewChatUserListInner::setUsers(const QVector<UserProfile> &users) {
    _users = users;
    _statusText.clear();
    _hovered = -1;
    setFixedHeight(contentHeight());
    update();
}

void NewChatUserListInner::setStatusText(const QString &text) {
    _users.clear();
    _statusText = text;
    _hovered = -1;
    setFixedHeight(contentHeight());
    setCursor(Qt::ArrowCursor);
    update();
}

QVector<UserProfile> NewChatUserListInner::users() const {
    return _users;
}

void NewChatUserListInner::setViewportHeight(int height) {
    if (_viewportHeight == height) {
        return;
    }
    _viewportHeight = height;
    setFixedHeight(contentHeight());
    update();
}

QSize NewChatUserListInner::sizeHint() const {
    return { width(), contentHeight() };
}

QSize NewChatUserListInner::minimumSizeHint() const {
    // Width 0 (not width()) so the widgetResizable scroll area can shrink us to
    // the viewport when the vertical scrollbar appears; otherwise the current
    // width ratchets and forces spurious horizontal scrolling.
    return { 0, contentHeight() };
}

int NewChatUserListInner::preferredHeight() const {
    if (_users.isEmpty()) {
        return kListHeight;
    }
    return qMax(
        kListHeight,
        kListPaddingTop + _users.size() * kRowHeight + kListPaddingBottom);
}

int NewChatUserListInner::contentHeight() const {
    // Fill at least the viewport so the empty-state text centres over the whole visible area and a
    // short result set doesn't leave a gap under the last row — but stay tall enough for every row.
    return qMax(preferredHeight(), _viewportHeight);
}

void NewChatUserListInner::paintEvent(QPaintEvent *e) {
    QPainter p(this);
    const auto r = e->rect();
    p.setClipRect(r);
    p.fillRect(r, st::windowBg);

    if (_users.isEmpty()) {
        p.setFont(st::normalFont);
        p.setPen(st::windowSubTextFg);
        const auto text = _statusText.isEmpty()
            ? searchHintText()
            : _statusText;
        p.drawText(rect().adjusted(24, 0, -24, 0),
            int(Qt::AlignCenter | Qt::TextWordWrap),
            text);
        return;
    }

    const int yFrom = r.y();
    const int yTo = r.y() + r.height();
    const int firstRow = qMax(0, (yFrom - kListPaddingTop) / kRowHeight);
    const int lastRow = qMin(
        _users.size() - 1,
        (yTo - kListPaddingTop + kRowHeight - 1) / kRowHeight);

    for (int i = firstRow; i <= lastRow; ++i) {
        paintRow(p, i, i == _hovered);
    }
}

void NewChatUserListInner::paintRow(QPainter &p, int index, bool hovered) {
    const auto &user = _users[index];
    const auto name = displayNameFor(user);
    const int y = kListPaddingTop + index * kRowHeight;
    const int w = width();

    p.fillRect(0, y, w, kRowHeight, hovered ? st::windowBgOver : st::windowBg);

    const QRect userpicRect(kPhotoX, y + kPhotoY, kPhotoSize, kPhotoSize);
	bool paintedAvatar = false;
	if (!user.avatarUrl.isEmpty()) {
		const auto dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
		const auto avatar = MediaCache::loadAvatarPixmapAsync(
			user.avatarUrl,
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
        Ui::EmptyUserpic::paint(
            p,
            user.userId,
            name,
            userpicRect.x(),
            userpicRect.y(),
            userpicRect.width());
    }

    const int skipRight = kPhotoX;
    const int nameW = w - kNameX - skipRight;
    p.setFont(st::semiboldFont);
    p.setPen(st::windowFg);
    const QFontMetrics nameFm(st::semiboldFont);
    p.drawText(
        kNameX,
        y + kNameY + nameFm.ascent(),
        nameFm.elidedText(name, Qt::ElideRight, nameW));

    const int statusW = w - kStatusX - skipRight;
    p.setFont(st::normalFont);
    p.setPen(hovered ? st::windowSubTextFgOver : st::windowSubTextFg);
    const QFontMetrics statusFm(st::normalFont);
    p.drawText(
        kStatusX,
        y + kStatusY + statusFm.ascent(),
        statusFm.elidedText(user.userId, Qt::ElideRight, statusW));
}

int NewChatUserListInner::indexAt(const QPoint &pos) const {
    if (_users.isEmpty()) {
        return -1;
    }
    const int row = (pos.y() - kListPaddingTop) / kRowHeight;
    if (row < 0 || row >= _users.size()) {
        return -1;
    }
    return row;
}

void NewChatUserListInner::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) {
        return;
    }
    const auto index = indexAt(e->pos());
    if (index < 0) {
        return;
    }
    const auto &user = _users[index];
    emit userClicked(user.userId, user.displayName);
}

void NewChatUserListInner::mouseMoveEvent(QMouseEvent *e) {
    const auto index = indexAt(e->pos());
    if (index == _hovered) {
        return;
    }
    _hovered = index;
    setCursor(index >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    update();
}

void NewChatUserListInner::leaveEvent(QEvent *) {
    if (_hovered < 0) {
        return;
    }
    _hovered = -1;
    setCursor(Qt::ArrowCursor);
    update();
}

DialogsNewChatBox::DialogsNewChatBox(
    ProtocolBridge *bridge,
    QWidget *parent)
: QWidget(parent ? parent->window() : nullptr)
, _bridge(bridge) {
    if (parentWidget()) {
        setGeometry(parentWidget()->rect());
        parentWidget()->installEventFilter(this);
    }
    setFocusPolicy(Qt::StrongFocus);

    if (_bridge) {
        _ownUserId = _bridge->cachedSessionInfo().userId;
        _initialUsers = initialUsersFromBridge(_bridge);
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

    auto *titleText = new QLabel(
        QCoreApplication::translate("DialogsNewChatBox", "New Chat"),
        titleBar);
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

    auto *searchContainer = new QWidget(_panel);
    searchContainer->setFixedHeight(
        st::boxSearchPadding.top()
        + st::boxSearchFieldHeight
        + st::boxSearchPadding.bottom());
    panelLayout->addWidget(searchContainer);

    _searchField = new QLineEdit(searchContainer);
    _searchField->setPlaceholderText(
        QCoreApplication::translate("DialogsNewChatBox", "Search"));
    _searchField->setFrame(false);
    _searchField->setAttribute(Qt::WA_MacShowFocusRect, false);
    _searchField->setFixedHeight(st::boxSearchFieldHeight);
    const int searchIconSkip = 36;
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

    auto *searchIcon = new SearchIconPainter(searchContainer);
    searchIcon->setFixedSize(searchIconSkip, st::boxSearchFieldHeight);
    searchIcon->move(st::boxSearchPadding.left(), st::boxSearchPadding.top());
    searchIcon->setAttribute(Qt::WA_TransparentForMouseEvents);

    // Separator: shadowFg is #00000018 (semi-transparent); QPalette::Window
    // keeps the alpha and re-resolves on theme change.
    auto *searchSep = new QWidget(_panel);
    searchSep->setFixedHeight(1);
    searchSep->setAutoFillBackground(true);
    {
        QPalette pal = searchSep->palette();
        pal.setColor(QPalette::Window, st::shadowFg);
        searchSep->setPalette(pal);
    }
    panelLayout->addWidget(searchSep);

    _inner = new NewChatUserListInner(nullptr);
    showInitialUsers();

    _scroll = new ::Ui::ScrollArea(_panel);
    _scroll->setFrameShape(QFrame::NoFrame);
    _scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _scroll->setWidgetResizable(true);
    _scroll->setFixedHeight(kListHeight);
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

    ::Ui::TextButton::Style cancelStyle;
    cancelStyle.bgOver = &st::lightButtonBgOver; // transparent until hovered
    cancelStyle.fg = &st::lightButtonFg;
    cancelStyle.radius = st::boxRadius;
    cancelStyle.height = st::boxButtonHeight;
    cancelStyle.paddingH = 15;
    _cancel = new ::Ui::TextButton(
        QCoreApplication::translate("DialogsNewChatBox", "Cancel"),
        cancelStyle,
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
        this, &DialogsNewChatBox::triggerDirectorySearch);

    connect(_searchField, &QLineEdit::textChanged,
        this, &DialogsNewChatBox::scheduleSearch);
    connect(_searchField, &QLineEdit::returnPressed, this, [this] {
        const auto query = _searchField->text().trimmed();
        if (query.isEmpty()) {
            return;
        }
        const auto users = _inner ? _inner->users() : QVector<UserProfile>();
        if (users.size() == 1) {
            const auto &user = users.front();
            chooseUser(user.userId, user.displayName);
            return;
        }
        if (isExactUserId(query) && query != _ownUserId) {
            chooseUser(query, query);
        }
    });
    connect(_inner, &NewChatUserListInner::userClicked,
        this, &DialogsNewChatBox::chooseUser);

    if (_bridge) {
        connect(_bridge, &ProtocolBridge::userDirectorySearchReady,
            this, [this](
                const QString &query,
                bool success,
                const QVector<UserProfile> &results,
                bool /*limited*/) {
            if (query != _lastDirectoryQuery) {
                return;
            }
            if (!success) {
                clearResults(QCoreApplication::translate(
                    "DialogsNewChatBox",
                    "User directory search is unavailable."));
                return;
            }
            applySearchResults(query, results);
        });
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

    updateListHeight();

    QTimer::singleShot(0, _searchField, [this] {
        _searchField->setFocus();
    });
}

int DialogsNewChatBox::exec() {
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

QString DialogsNewChatBox::selectedUserId() const {
    return _selectedUserId;
}

QString DialogsNewChatBox::selectedDisplayName() const {
    return _selectedDisplayName;
}

void DialogsNewChatBox::accept() {
    _result = Accepted;
    if (_loop) {
        _loop->quit();
    }
}

void DialogsNewChatBox::reject() {
    _result = Rejected;
    if (_loop) {
        _loop->quit();
    }
}

void DialogsNewChatBox::chooseUser(
    const QString &userId,
    const QString &displayName) {
    if (userId.isEmpty() || userId == _ownUserId) {
        return;
    }
    _selectedUserId = userId;
    _selectedDisplayName = displayName.isEmpty() ? userId : displayName;
    emit userSelected(_selectedUserId, _selectedDisplayName);
    accept();
}

void DialogsNewChatBox::scheduleSearch(const QString &query) {
    const auto trimmed = query.trimmed();
    _lastDirectoryQuery = trimmed;
    if (_searchTimer) {
        _searchTimer->stop();
    }
    if (trimmed.isEmpty()) {
        showInitialUsers();
        return;
    }
    if (trimmed.size() < kSearchMinQueryLength) {
        showInitialUsers(trimmed);
        return;
    }
    clearResults(QCoreApplication::translate(
        "DialogsNewChatBox",
        "Searching users..."));
    if (_searchTimer) {
        _searchTimer->start(kSearchDelayMs);
    }
}

void DialogsNewChatBox::triggerDirectorySearch() {
    if (!_bridge) {
        clearResults(QCoreApplication::translate(
            "DialogsNewChatBox",
            "User directory search is unavailable."));
        return;
    }
    const auto query = _searchField ? _searchField->text().trimmed() : QString();
    _lastDirectoryQuery = query;
    if (query.size() < kSearchMinQueryLength) {
        clearResults(searchHintText());
        return;
    }
    _bridge->searchUserDirectory(query, 50);
}

void DialogsNewChatBox::applySearchResults(
    const QString &query,
    const QVector<UserProfile> &results) {
    const auto current = _searchField ? _searchField->text().trimmed() : QString();
    if (query != current) {
        return;
    }

    QVector<UserProfile> filtered;
    filtered.reserve(results.size());
    for (const auto &result : results) {
        if (!result.userId.isEmpty() && result.userId != _ownUserId) {
            filtered.push_back(result);
        }
    }

    if (filtered.isEmpty()) {
        clearResults(QCoreApplication::translate(
            "DialogsNewChatBox",
            "No users found."));
        return;
    }

    _inner->setUsers(filtered);
    updateListHeight();
    resolveAvatars(filtered);
}

void DialogsNewChatBox::clearResults(const QString &statusText) {
    if (_inner) {
        _inner->setStatusText(statusText);
    }
    // Deliberately does NOT resize the box: transient states ("Searching…", "No users found")
    // keep the current height so it doesn't pulse smaller/larger on every keystroke. The box
    // only adapts when real content lands (applySearchResults / showInitialUsers).
}

void DialogsNewChatBox::showInitialUsers(const QString &query) {
    if (!_inner) {
        return;
    }

    QVector<UserProfile> users;
    const auto needle = query.trimmed().toLower();
    for (const auto &user : _initialUsers) {
        if (needle.isEmpty()
            || displayNameFor(user).toLower().contains(needle)
            || user.userId.toLower().contains(needle)) {
            users.push_back(user);
        }
    }

    if (!users.isEmpty()) {
        _inner->setUsers(users);
        updateListHeight();
        resolveAvatars(users);
        return;
    }

    clearResults(query.trimmed().isEmpty()
        ? searchHintText()
        : QCoreApplication::translate(
            "DialogsNewChatBox",
            "Type at least two characters to search the homeserver."));
}

void DialogsNewChatBox::resolveAvatars(const QVector<UserProfile> &users) {
    if (!_bridge) {
        return;
    }
    for (const auto &user : users) {
        if (user.avatarUrl.startsWith(QStringLiteral("mxc://"))
            && MediaCache::needsResolution(user.avatarUrl)) {
            MediaCache::markRequested(user.avatarUrl);
            _bridge->resolveAvatar(user.avatarUrl);
        }
    }
}

void DialogsNewChatBox::updateListHeight() {
    if (!_scroll || !_panel || !_inner) {
        return;
    }
    // Everything in the panel except the scrollable list has a fixed height. The hint row is a
    // fixed reserved slot, so its text appearing/clearing never changes any height; only the list
    // grows or shrinks with the amount of content (bounded below).
    const int chrome =
        st::boxTitleHeight
        + (st::boxSearchPadding.top()
            + st::boxSearchFieldHeight
            + st::boxSearchPadding.bottom())
        + 1 // the search separator
        + (st::boxButtonPadding.top()
            + st::boxButtonHeight
            + st::boxButtonPadding.bottom());
    // Adapt to content: grow with the number of results (min the visible-rows floor) up to the
    // available window height, then let the list scroll.
    const int available = qMax(kListHeight, height() - 2 * kPanelVerticalMargin - chrome);
    const int listHeight = qMin(_inner->preferredHeight(), available);
    _scroll->setFixedHeight(listHeight);
    _inner->setViewportHeight(listHeight);
}

void DialogsNewChatBox::paintEvent(QPaintEvent *) {
    QPainter p(this);

    p.setOpacity(_bgOpacity);
    p.fillRect(rect(), st::layerBg);

    if (_panel && _layerOpacity > 0) {
        p.setOpacity(_layerOpacity);
        paintBoxShadow(p, _panel->geometry());
    }
}

void DialogsNewChatBox::mousePressEvent(QMouseEvent *event) {
    if (_panel && !_panel->geometry().contains(event->pos())) {
        reject();
        return;
    }
    QWidget::mousePressEvent(event);
}

void DialogsNewChatBox::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool DialogsNewChatBox::eventFilter(QObject *obj, QEvent *event) {
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

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "dialogs_member_picker_box.h"

#include <QCoreApplication>
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

void drawSearchIcon(QPainter &p, int x, int y, const QColor &color) {
    PainterHighQualityEnabler hq(p);
    p.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(x + 7.5, y + 7.5), 5.5, 5.5);
    p.drawLine(QPointF(x + 11.5, y + 11.5), QPointF(x + 15, y + 15));
}

} // namespace

// ─────────────────────────────────────────────
// MemberListInner — custom-painted vertical list
// ─────────────────────────────────────────────

MemberListInner::MemberListInner(QWidget *parent)
    : QWidget(parent) {
    setMouseTracking(true);
}

void MemberListInner::setMembers(const QVector<UserProfile> &members) {
    _allMembers = members;
    _loaded = true;
    _hovered = -1;
    _filtered.clear();
    for (int i = 0; i < _allMembers.size(); ++i) {
        _filtered.push_back(i);
    }
    setFixedHeight(contentHeight());
    update();
}

void MemberListInner::setFilter(const QString &query) {
    const auto needle = query.trimmed().toLower();
    _filtered.clear();
    for (int i = 0; i < _allMembers.size(); ++i) {
        const auto &m = _allMembers[i];
        const auto name = m.displayName.isEmpty() ? m.userId : m.displayName;
        if (needle.isEmpty()
            || name.toLower().contains(needle)
            || m.userId.toLower().contains(needle)) {
            _filtered.push_back(i);
        }
    }
    _hovered = -1;
    setFixedHeight(contentHeight());
    update();
}

int MemberListInner::contentHeight() const {
    if (_filtered.isEmpty()) {
        // Reserve room for the centred "Loading…" / "No members found" text.
        return kListPaddingTop + 3 * kRowHeight + kListPaddingBottom;
    }
    return kListPaddingTop
        + _filtered.size() * kRowHeight
        + kListPaddingBottom;
}

QSize MemberListInner::sizeHint() const {
    return { width(), contentHeight() };
}

QSize MemberListInner::minimumSizeHint() const {
    return sizeHint();
}

void MemberListInner::paintEvent(QPaintEvent *e) {
    QPainter p(this);
    const auto r = e->rect();
    p.setClipRect(r);
    p.fillRect(r, st::windowBg);

    if (_filtered.isEmpty()) {
        p.setFont(st::normalFont);
        p.setPen(st::windowSubTextFg);
        p.drawText(rect(), int(Qt::AlignCenter),
                   _loaded
                       ? QCoreApplication::translate("MemberPickerBox", "No members found")
                       : QCoreApplication::translate("MemberPickerBox", "Loading members…"));
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

void MemberListInner::paintRow(QPainter &p, int filteredIndex, bool hovered) {
    const auto &member = _allMembers[_filtered[filteredIndex]];
    const auto name = member.displayName.isEmpty()
        ? member.userId
        : member.displayName;

    const int y = kListPaddingTop + filteredIndex * kRowHeight;
    const int w = width();

    // Background.
    p.fillRect(0, y, w, kRowHeight, hovered ? st::windowBgOver : st::windowBg);

    // Avatar.
    const QRect userpicRect(kPhotoX, y + kPhotoY, kPhotoSize, kPhotoSize);
	bool paintedAvatar = false;
	if (!member.avatarUrl.isEmpty()) {
		const auto dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
		const auto avatar = MediaCache::loadAvatarPixmapAsync(
			member.avatarUrl,
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
        Ui::EmptyUserpic::paint(p, member.userId, name, userpicRect.x(), userpicRect.y(), userpicRect.width());
    }

    // Name (semibold).
    const int skipRight = kPhotoX;
    const int nameW = w - kNameX - skipRight;
    p.setFont(st::semiboldFont);
    p.setPen(st::windowFg);
    const QFontMetrics nameFm(st::semiboldFont);
    p.drawText(kNameX, y + kNameY + nameFm.ascent(),
        nameFm.elidedText(name, Qt::ElideRight, nameW));

    // Status line (contacts-status foreground colour).
    const auto &statusFg = hovered ? st::windowSubTextFgOver : st::windowSubTextFg;
    p.setFont(st::normalFont);
    p.setPen(statusFg);
    const int statusW = w - kStatusX - skipRight;
    const QFontMetrics statusFm(st::normalFont);
    // No real status data available — show userId as fallback.
    const auto status = member.userId;
    p.drawText(kStatusX, y + kStatusY + statusFm.ascent(),
        statusFm.elidedText(status, Qt::ElideRight, statusW));
}

int MemberListInner::indexAt(const QPoint &pos) const {
    if (_filtered.isEmpty()) return -1;
    const int row = (pos.y() - kListPaddingTop) / kRowHeight;
    if (row < 0 || row >= _filtered.size()) return -1;
    return row;
}

void MemberListInner::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) return;
    const int idx = indexAt(e->pos());
    if (idx < 0) return;
    const auto &m = _allMembers[_filtered[idx]];
    emit memberClicked(m.userId, m.displayName);
}

void MemberListInner::mouseMoveEvent(QMouseEvent *e) {
    const int idx = indexAt(e->pos());
    if (idx != _hovered) {
        _hovered = idx;
        setCursor(idx >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void MemberListInner::leaveEvent(QEvent *) {
    if (_hovered >= 0) {
        _hovered = -1;
        setCursor(Qt::ArrowCursor);
        update();
    }
}

// ─────────────────────────────────────────────
// MemberPickerBox — PeerListBox-style modal
// ─────────────────────────────────────────────

MemberPickerBox::MemberPickerBox(
    const QVector<UserProfile> &members,
    ProtocolBridge *bridge,
    QWidget *parent)
    : QWidget(parent ? parent->window() : nullptr)
{
    _bridge = bridge;
    if (parentWidget()) {
        setGeometry(parentWidget()->rect());
        parentWidget()->installEventFilter(this);
    }
    setFocusPolicy(Qt::StrongFocus);

    _bgOpacity = 0.0;
    _layerOpacity = 0.0;

    // Background overlay animation.
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

    // Box/shadow animation.
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

    // --- Root layout: centers the panel in the overlay ---
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addStretch(1);

    // --- Panel: the white box (364px wide) ---
    _panel = new RoundedPanel(this);
    _panel->setVisible(false);
    _panel->setFixedWidth(st::boxWideWidth);
    root->addWidget(_panel, 0, Qt::AlignHCenter);
    root->addStretch(1);

    auto *panelLayout = new QVBoxLayout(_panel);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(0);

    // --- Title bar ---
    auto *titleBar = new QWidget(_panel);
    titleBar->setFixedHeight(st::boxTitleHeight);
    panelLayout->addWidget(titleBar);

    auto *titleText = new QLabel(
        QCoreApplication::translate("MemberPickerBox", "Show messages from"), titleBar);
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

    // --- Search area ---
    auto *searchContainer = new QWidget(_panel);
    searchContainer->setFixedHeight(
        st::boxSearchPadding.top()
        + st::boxSearchFieldHeight
        + st::boxSearchPadding.bottom());
    panelLayout->addWidget(searchContainer);

    _searchField = new QLineEdit(searchContainer);
    _searchField->setPlaceholderText(QCoreApplication::translate("MemberPickerBox", "Search"));
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

    // Search icon.
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

    // --- Member list ---
    _inner = new MemberListInner(nullptr);
    // Empty at construction keeps the inner in its "Loading…" state; setMembers() fills it later.
    if (!members.isEmpty()) {
        _inner->setMembers(members);
    }

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

    ::Ui::TextButton::Style cancelStyle;
    cancelStyle.bgOver = &st::lightButtonBgOver; // transparent until hovered
    cancelStyle.fg = &st::lightButtonFg;
    cancelStyle.radius = st::boxRadius;
    cancelStyle.height = st::boxButtonHeight;
    cancelStyle.paddingH = 15;
    _cancel = new ::Ui::TextButton(
        QCoreApplication::translate("MemberPickerBox", "Cancel"),
        cancelStyle,
        buttonsContainer);
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

    connect(_inner, &MemberListInner::memberClicked, this,
            [this](const QString &userId, const QString &displayName) {
        _selectedUserId = userId;
        _selectedDisplayName = displayName;
        emit memberSelected(userId, displayName);
        accept();
    });

    QTimer::singleShot(0, _searchField, [this] {
        _searchField->setFocus();
    });

    // Resolve member avatar URLs.
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
        for (const auto &m : members) {
            if (m.avatarUrl.startsWith(QStringLiteral("mxc://"))
                    && MediaCache::needsResolution(m.avatarUrl)) {
                MediaCache::markRequested(m.avatarUrl);
                bridge->resolveAvatar(m.avatarUrl);
            }
        }
    }
}

void MemberPickerBox::setMembers(const QVector<UserProfile> &members) {
    if (!_inner) {
        return;
    }
    _inner->setMembers(members);
    // Keep whatever the user has already typed applied to the freshly-loaded list.
    if (_searchField) {
        _inner->setFilter(_searchField->text());
    }
    if (_scroll) {
        _scroll->setFixedHeight(qMin(_inner->sizeHint().height(), st::boxMaxListHeight));
    }
    if (_bridge) {
        for (const auto &m : members) {
            if (m.avatarUrl.startsWith(QStringLiteral("mxc://"))
                    && MediaCache::needsResolution(m.avatarUrl)) {
                MediaCache::markRequested(m.avatarUrl);
                _bridge->resolveAvatar(m.avatarUrl);
            }
        }
    }
}

int MemberPickerBox::exec() {
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

void MemberPickerBox::accept() {
    _result = Accepted;
    if (_loop) _loop->quit();
}

void MemberPickerBox::reject() {
    _result = Rejected;
    if (_loop) _loop->quit();
}

QString MemberPickerBox::selectedUserId() const {
    return _selectedUserId;
}

QString MemberPickerBox::selectedDisplayName() const {
    return _selectedDisplayName;
}

void MemberPickerBox::paintEvent(QPaintEvent *) {
    QPainter p(this);

    p.setOpacity(_bgOpacity);
    p.fillRect(rect(), st::layerBg);

    if (_panel && _layerOpacity > 0) {
        p.setOpacity(_layerOpacity);
        paintBoxShadow(p, _panel->geometry());
    }
}

void MemberPickerBox::mousePressEvent(QMouseEvent *event) {
    if (_panel && !_panel->geometry().contains(event->pos())) {
        reject();
        return;
    }
    QWidget::mousePressEvent(event);
}

void MemberPickerBox::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool MemberPickerBox::eventFilter(QObject *obj, QEvent *event) {
    if (obj == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace TeleMatrix

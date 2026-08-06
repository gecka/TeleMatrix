// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "dialogs_invite_users_box.h"

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

#include <functional>

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
[[nodiscard]] QString inviteHintText() {
    return QCoreApplication::translate(
        "TeleMatrix::InviteUsersBox",
        "Type a name or Matrix user ID to search the homeserver,\n"
        "or press Enter on an exact @user:server ID to add it directly.");
}

// Chip (pill) dimensions.
constexpr int kChipHeight = 32;
constexpr int kChipPadding = 8;        // horizontal padding inside chip
constexpr int kChipSpacing = 4;        // gap between chips
constexpr int kChipRemoveSize = 16;    // "x" button hit area
constexpr int kChipMaxWidth = 128;     // max chip text width before ellipsis
constexpr int kChipRadius = 16;        // fully rounded ends

// Bar constraints.
constexpr int kBarPadding = 8;         // padding around chips area
constexpr int kBarMaxHeight = 104;     // defaultMultiSelect.maxHeight (3 rows)
constexpr int kBarMinHeight = 44;      // single row with padding
constexpr int kDirectorySearchDelayMs = 180;
constexpr int kDirectorySearchMinQueryLength = 2;
constexpr int kInviteHintMinHeight = 160;

// Search result list rows.
constexpr int kSearchRowHeight = 56;
constexpr int kSearchPhotoSize = 42;
constexpr int kSearchPhotoX = 16;
constexpr int kSearchPhotoY = 7;
constexpr int kSearchNameX = 74;
constexpr int kSearchNameY = 9;
constexpr int kSearchStatusX = 74;
constexpr int kSearchStatusY = 30;
constexpr int kSearchListPaddingTop = 10;
constexpr int kSearchListPaddingBottom = 10;
constexpr int kSearchListMaxVisibleRows = 4;
constexpr int kSearchListMaxHeight =
    kSearchListPaddingTop
    + (kSearchRowHeight * kSearchListMaxVisibleRows)
    + kSearchListPaddingBottom;

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

// Primary "Invite" button style. When disabled, it renders muted
// (windowBgOver / windowSubTextFg) — modelled here by swapping the TextButton
// style from updateInviteButton(). Uses 20px horizontal padding (QSS original).
::Ui::TextButton::Style inviteButtonStyle(bool enabled) {
    ::Ui::TextButton::Style s;
    s.radius = st::boxRadius;
    s.height = st::boxButtonHeight;
    s.paddingH = 20;
    if (enabled) {
        s.bg = &st::activeButtonBg;
        s.bgOver = &st::activeButtonBgOver;
        s.fg = &st::activeButtonFg;
    } else {
        s.bg = &st::windowBgOver;
        s.bgOver = &st::windowBgOver;
        s.fg = &st::windowSubTextFg;
    }
    return s;
}

} // namespace

// ─────────────────────────────────────────────
// InviteChipBar — wrapping flow layout of chips + input
// ─────────────────────────────────────────────

InviteChipBar::InviteChipBar(QWidget *parent)
    : QWidget(parent) {
    _input = new QLineEdit(this);
    _input->setPlaceholderText(tr("@user:server.org"));
    _input->setFrame(false);
    _input->setAttribute(Qt::WA_MacShowFocusRect, false);
    _input->setFixedHeight(kChipHeight);
    // Frameless + transparent background; live st:: colors via QPalette so the
    // field tracks theme changes (a QSS built from .name() would freeze them).
    _input->setFont(st::baseFont(13));
    {
        QPalette pal = _input->palette();
        pal.setColor(QPalette::Base, Qt::transparent);
        pal.setColor(QPalette::Text, st::windowFg);
        _input->setPalette(pal);
    }
    _input->setTextMargins(4, 0, 4, 0); // padding: 0 4px

    setMinimumHeight(kBarMinHeight);
    setMaximumHeight(kBarMaxHeight);
}

void InviteChipBar::addChip(const QString &userId, const QString &displayName) {
    if (hasChip(userId)) return;
    InviteChip chip;
    chip.userId = userId;
    chip.displayName = displayName.isEmpty() ? userId : displayName;
    _chips.push_back(chip);
    relayout();
    emit chipAdded(userId);
}

void InviteChipBar::removeChip(const QString &userId) {
    for (int i = 0; i < _chips.size(); ++i) {
        if (_chips[i].userId == userId) {
            _chips.remove(i);
            relayout();
            emit chipRemoved(userId);
            return;
        }
    }
}

bool InviteChipBar::hasChip(const QString &userId) const {
    for (const auto &c : _chips) {
        if (c.userId == userId) return true;
    }
    return false;
}

int InviteChipBar::chipCount() const {
    return _chips.size();
}

QVector<InviteChip> InviteChipBar::chips() const {
    return _chips;
}

QLineEdit *InviteChipBar::inputField() const {
    return _input;
}

QSize InviteChipBar::sizeHint() const {
    return { width(), qMax(kBarMinHeight, minimumHeight()) };
}

QSize InviteChipBar::minimumSizeHint() const {
    return { 100, kBarMinHeight };
}

void InviteChipBar::relayout() {
    const int availW = width() - 2 * kBarPadding;
    const QFontMetrics fm(st::normalFont);

    int x = kBarPadding;
    int y = kBarPadding;

    for (auto &chip : _chips) {
        const auto textW = qMin(fm.horizontalAdvance(chip.displayName), kChipMaxWidth);
        const int chipW = kChipPadding + textW + kChipPadding + kChipRemoveSize;
        // Wrap to next row if needed.
        if (x + chipW > width() - kBarPadding && x > kBarPadding) {
            x = kBarPadding;
            y += kChipHeight + kChipSpacing;
        }
        chip.rect = QRect(x, y, chipW, kChipHeight);
        chip.removeRect = QRect(
            x + chipW - kChipRemoveSize - 4,
            y + (kChipHeight - kChipRemoveSize) / 2,
            kChipRemoveSize,
            kChipRemoveSize);
        x += chipW + kChipSpacing;
    }

    // Position the input field after the last chip.
    const int inputMinW = 100;
    if (x + inputMinW > width() - kBarPadding && x > kBarPadding) {
        x = kBarPadding;
        y += kChipHeight + kChipSpacing;
    }
    const int inputW = width() - kBarPadding - x;
    _input->setGeometry(x, y, qMax(inputW, inputMinW), kChipHeight);

    const int totalH = y + kChipHeight + kBarPadding;
    const int clampedH = qBound(kBarMinHeight, totalH, kBarMaxHeight);
    if (height() != clampedH) {
        setFixedHeight(clampedH);
        emit heightChanged();
    }
    update();
}

void InviteChipBar::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), st::boxBg);

    const QFontMetrics fm(st::normalFont);

    for (int i = 0; i < _chips.size(); ++i) {
        const auto &chip = _chips[i];

        // Chip background (rounded pill).
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(st::windowBgOver);
        p.drawRoundedRect(chip.rect, kChipRadius, kChipRadius);

        // Chip text.
        p.setPen(st::windowFg);
        p.setFont(st::normalFont);
        const int textX = chip.rect.x() + kChipPadding;
        const int textW = chip.rect.width() - 2 * kChipPadding - kChipRemoveSize;
        const auto elidedName = fm.elidedText(
            chip.displayName, Qt::ElideRight, textW);
        p.drawText(
            textX,
            chip.rect.y() + (kChipHeight - fm.height()) / 2 + fm.ascent(),
            elidedName);

        // Remove "×" icon.
        p.setPen(QPen(st::windowSubTextFg, 1.5, Qt::SolidLine, Qt::RoundCap));
        const auto cx = chip.removeRect.center();
        const int s = 4; // half-size of the cross
        p.drawLine(cx.x() - s, cx.y() - s, cx.x() + s, cx.y() + s);
        p.drawLine(cx.x() + s, cx.y() - s, cx.x() - s, cx.y() + s);
    }
}

void InviteChipBar::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) return;
    const auto pos = e->pos();
    for (const auto &chip : _chips) {
        if (chip.removeRect.contains(pos)) {
            removeChip(chip.userId);
            return;
        }
    }
    // Click outside chips → focus input.
    _input->setFocus();
}

void InviteChipBar::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    relayout();
}

// ─────────────────────────────────────────────
// InviteSearchListInner — custom-painted search results
// ─────────────────────────────────────────────

class InviteSearchListInner final : public QWidget {
public:
    explicit InviteSearchListInner(QWidget *parent = nullptr)
        : QWidget(parent) {
        setMouseTracking(true);
    }

    void setMembers(const QVector<UserProfile> &members) {
        _members = members;
        _hovered = -1;
        setFixedHeight(contentHeight());
        update();
    }

    [[nodiscard]] QVector<UserProfile> members() const {
        return _members;
    }

    void setMemberClickedCallback(
        std::function<void(const QString &, const QString &)> callback) {
        _memberClicked = std::move(callback);
    }

    QSize sizeHint() const override {
        return { width(), contentHeight() };
    }

    QSize minimumSizeHint() const override {
        return sizeHint();
    }

protected:
    void paintEvent(QPaintEvent *e) override {
        QPainter p(this);
        const auto r = e->rect();
        p.setClipRect(r);
        p.fillRect(r, st::windowBg);

        const int yFrom = r.y();
        const int yTo = r.y() + r.height();
        const int firstRow = qMax(0, (yFrom - kSearchListPaddingTop) / kSearchRowHeight);
        const int lastRow = qMin(
            _members.size() - 1,
            (yTo - kSearchListPaddingTop + kSearchRowHeight - 1) / kSearchRowHeight);

        for (int i = firstRow; i <= lastRow; ++i) {
            paintRow(p, i, i == _hovered);
        }
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton) {
            return;
        }
        const auto idx = indexAt(e->pos());
        if (idx < 0 || idx >= _members.size() || !_memberClicked) {
            return;
        }
        const auto &member = _members[idx];
        _memberClicked(member.userId, member.displayName);
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        const auto idx = indexAt(e->pos());
        if (idx != _hovered) {
            _hovered = idx;
            setCursor(idx >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
            update();
        }
    }

    void leaveEvent(QEvent *) override {
        if (_hovered >= 0) {
            _hovered = -1;
            setCursor(Qt::ArrowCursor);
            update();
        }
    }

private:
    [[nodiscard]] int contentHeight() const {
        return kSearchListPaddingTop
            + _members.size() * kSearchRowHeight
            + kSearchListPaddingBottom;
    }

    [[nodiscard]] int indexAt(const QPoint &pos) const {
        if (_members.isEmpty()) {
            return -1;
        }
        const int row = (pos.y() - kSearchListPaddingTop) / kSearchRowHeight;
        if (row < 0 || row >= _members.size()) {
            return -1;
        }
        return row;
    }

    void paintRow(QPainter &p, int index, bool hovered) {
        const auto &member = _members[index];
        const auto name = member.displayName.isEmpty() ? member.userId : member.displayName;
        const int y = kSearchListPaddingTop + index * kSearchRowHeight;
        const int w = width();

        p.fillRect(0, y, w, kSearchRowHeight, hovered ? st::windowBgOver : st::windowBg);

        const QRect userpicRect(kSearchPhotoX, y + kSearchPhotoY, kSearchPhotoSize, kSearchPhotoSize);
		bool paintedAvatar = false;
		if (!member.avatarUrl.isEmpty()) {
			const auto dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
			const auto avatar = MediaCache::loadAvatarPixmapAsync(
				member.avatarUrl,
				kSearchPhotoSize,
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
                member.userId,
                name,
                userpicRect.x(),
                userpicRect.y(),
                userpicRect.width());
        }

        const int rightSkip = kSearchPhotoX;
        const int nameW = w - kSearchNameX - rightSkip;
        p.setFont(st::semiboldFont);
        p.setPen(st::windowFg);
        const QFontMetrics nameFm(st::semiboldFont);
        p.drawText(
            kSearchNameX,
            y + kSearchNameY + nameFm.ascent(),
            nameFm.elidedText(name, Qt::ElideRight, nameW));

        p.setFont(st::normalFont);
        p.setPen(hovered ? st::windowSubTextFgOver : st::windowSubTextFg);
        const int statusW = w - kSearchStatusX - rightSkip;
        const QFontMetrics statusFm(st::normalFont);
        p.drawText(
            kSearchStatusX,
            y + kSearchStatusY + statusFm.ascent(),
            statusFm.elidedText(member.userId, Qt::ElideRight, statusW));
    }

    QVector<UserProfile> _members;
    int _hovered = -1;
    std::function<void(const QString &, const QString &)> _memberClicked;
};

// ─────────────────────────────────────────────
// InviteUsersBox — PeerListBox-style modal
// ─────────────────────────────────────────────

InviteUsersBox::InviteUsersBox(
    const QString &roomId,
    ProtocolBridge *bridge,
    QWidget *parent,
    bool excludeExistingMembers)
    : QWidget(parent ? parent->window() : nullptr)
    , _roomId(roomId)
    , _bridge(bridge)
    , _memberExclusionReady(!excludeExistingMembers || !bridge)
{
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

    // --- Root layout: centers the panel ---
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addStretch(1);

    // --- Panel: white box (364px wide) ---
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
        tr("Invite Users"), titleBar);
    titleText->setFont(st::boxTitleFont);
    {
        QPalette pal = titleText->palette();
        pal.setColor(QPalette::WindowText, st::boxTitleFg);
        titleText->setPalette(pal);
    }
    titleText->move(st::boxTitlePosition.x(), st::boxTitlePosition.y());

    _close = new ::Ui::CloseButton(titleBar);
    _close->move(st::boxWideWidth - st::settingsCloseButtonSize, 0);
    connect(_close, &::Ui::CloseButton::clicked, this, [this] {
        reject();
    });

    // --- Chip bar with inline input ---
    _chipBar = new InviteChipBar(_panel);
    _chipBar->setFixedWidth(st::boxWideWidth);
    panelLayout->addWidget(_chipBar);

    // Separator line below chip bar. shadowFg is #00000018 (semi-transparent);
    // QPalette::Window keeps the alpha and re-resolves on theme change.
    auto *sep = new QWidget(_panel);
    sep->setFixedHeight(1);
    sep->setAutoFillBackground(true);
    {
        QPalette pal = sep->palette();
        pal.setColor(QPalette::Window, st::shadowFg);
        sep->setPalette(pal);
    }
    panelLayout->addWidget(sep);

    // --- User directory search results ---
    _resultsInner = new InviteSearchListInner(nullptr);
    _resultsInner->setMemberClickedCallback(
        [this](const QString &userId, const QString &displayName) {
            if (_chipBar->hasChip(userId)) {
                return;
            }
            if (isExcludedUser(userId)) {
                setStatusText(tr("User is already a room member."));
                return;
            }
            _chipBar->addChip(userId, displayName);
            _chipBar->inputField()->clear();
            clearSearchResults();
            setStatusText(QString());
            _chipBar->inputField()->setFocus();
        });

    _resultsScroll = new ::Ui::ScrollArea(_panel);
    _resultsScroll->setFrameShape(QFrame::NoFrame);
    _resultsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _resultsScroll->setWidgetResizable(true);
    _resultsScroll->setWidget(_resultsInner);
    _resultsScroll->setVisible(false);
    makeScrollTransparent(_resultsScroll);
    panelLayout->addWidget(_resultsScroll);

    // --- Hint label ---
    _hintLabel = new QLabel(_panel);
    _hintLabel->setText(inviteHintText());
    _hintLabel->setAlignment(Qt::AlignCenter);
    _hintLabel->setWordWrap(true);
    _hintLabel->setFont(st::normalFont);
    _hintLabel->setMinimumHeight(kInviteHintMinHeight);
    // padding: 24px 16px from the original QSS.
    _hintLabel->setContentsMargins(16, 24, 16, 24);
    {
        QPalette pal = _hintLabel->palette();
        pal.setColor(QPalette::WindowText, st::windowSubTextFg);
        _hintLabel->setPalette(pal);
    }
    panelLayout->addWidget(_hintLabel);

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

    // "Skip" button (secondary, defaultLightButton style).
    ::Ui::TextButton::Style skipStyle;
    skipStyle.bgOver = &st::lightButtonBgOver; // transparent until hovered
    skipStyle.fg = &st::lightButtonFg;
    skipStyle.radius = st::boxRadius;
    skipStyle.height = st::boxButtonHeight;
    skipStyle.paddingH = 15;
    _skip = new ::Ui::TextButton(tr("Skip"), skipStyle, buttonsContainer);
    {
        auto f = st::baseFont(14);
        f.setWeight(QFont::DemiBold);
        _skip->setFont(f);
    }
    connect(_skip, &QAbstractButton::clicked, this, [this] { reject(); });
    buttonsLayout->addWidget(_skip);

    // "Invite" button (primary, disabled until chips added).
    _invite = new ::Ui::TextButton(
        tr("Invite"), inviteButtonStyle(false), buttonsContainer);
    {
        auto f = st::baseFont(14);
        f.setWeight(QFont::DemiBold);
        _invite->setFont(f);
    }
    _invite->setFixedHeight(st::boxButtonHeight);
    _invite->setEnabled(false);
    updateInviteButton();
    connect(_invite, &QAbstractButton::clicked, this,
            &InviteUsersBox::startInvites);
    buttonsLayout->addWidget(_invite);

    // --- Connections ---

    // Enter key in input → add user.
    connect(_chipBar->inputField(), &QLineEdit::returnPressed,
            this, &InviteUsersBox::tryAddUser);

    // Chip count changes → update invite button.
    connect(_chipBar, &InviteChipBar::chipAdded,
            this, [this](const QString &) {
        updateInviteButton();
    });
    connect(_chipBar, &InviteChipBar::chipRemoved,
            this, [this](const QString &) {
        updateInviteButton();
        if (_invitesInFlight) {
            return;
        }
        const auto query = _chipBar->inputField()->text().trimmed();
        if (query.size() >= kDirectorySearchMinQueryLength && _searchTimer) {
            setStatusText(tr("Searching users..."));
            _searchTimer->start(kDirectorySearchDelayMs);
        }
    });

    _searchTimer = new QTimer(this);
    _searchTimer->setSingleShot(true);
    connect(_searchTimer, &QTimer::timeout, this,
            &InviteUsersBox::triggerDirectorySearch);

    connect(_chipBar->inputField(), &QLineEdit::textChanged, this,
            [this](const QString &text) {
        if (_invitesInFlight) {
            return;
        }
        _lastDirectoryQuery = text.trimmed();
        if (_lastDirectoryQuery.size() < kDirectorySearchMinQueryLength) {
            clearSearchResults();
            setStatusText(QString());
            if (_searchTimer) {
                _searchTimer->stop();
            }
            return;
        }
        setStatusText(tr("Searching users..."));
        if (_searchTimer) {
            _searchTimer->start(kDirectorySearchDelayMs);
        }
    });

    if (_bridge) {
        connect(_bridge, &ProtocolBridge::userInvited, this,
                [this](bool success) {
            if (!_invitesInFlight || _currentInviteUserId.isEmpty()) {
                return;
            }
            const auto userId = _currentInviteUserId;
            _currentInviteUserId.clear();
            if (success) {
                ++_successfulInvites;
                _chipBar->removeChip(userId);
            } else {
                _failedInviteIds.push_back(userId);
            }
            sendNextInvite();
        });
        connect(_bridge, &ProtocolBridge::userDirectorySearchReady, this,
                [this](const QString &query, bool success, const QVector<UserProfile> &results, bool limited) {
            if (_invitesInFlight) {
                return;
            }
            if (query != _chipBar->inputField()->text().trimmed()) {
                return;
            }
            if (!success) {
                clearSearchResults();
                setStatusText(tr("User directory search is unavailable."), true);
                return;
            }
            applySearchResults(query, results, limited);
        });
        connect(_bridge, &ProtocolBridge::mediaResolved, this,
                [this](bool success, const QString &mxcUrl, const QString &localPath) {
            if (!success || localPath.isEmpty()) {
                if (mxcUrl.startsWith(QStringLiteral("mxc://"))) {
                    MediaCache::clearRequested(mxcUrl);
                }
                return;
            }
            MediaCache::insertPath(mxcUrl, localPath);
            if (_resultsInner) {
                _resultsInner->update();
            }
        });
        if (excludeExistingMembers) {
            connect(_bridge, &ProtocolBridge::roomMembersSnapshotReady, this,
                    [this](const QString &roomId, bool success, const RoomMembersSnapshot &snapshot) {
                if (roomId != _roomId) {
                    return;
                }
                _excludedUserIds.clear();
                if (!success) {
                    _memberExclusionReady = false;
                    clearSearchResults();
                    setStatusText(tr("Failed to load room members."), true);
                    return;
                }
                for (const auto &member : snapshot.members) {
                    if (!member.userId.isEmpty()) {
                        _excludedUserIds.insert(member.userId);
                    }
                }
                _memberExclusionReady = true;
                clearSearchResults();
                const auto query = _chipBar->inputField()->text().trimmed();
                if (query.size() >= kDirectorySearchMinQueryLength) {
                    setStatusText(tr("Searching users..."));
                    triggerDirectorySearch();
                } else {
                    setStatusText(QString());
                }
            });
            setStatusText(tr("Loading members..."));
            // Force a full member fetch so the "already joined" exclusion set is
            // complete (the default fast path may return only cached members).
            _bridge->getRoomMembersSnapshotAsync(_roomId, true);
        }
    }

    QTimer::singleShot(0, _chipBar->inputField(), [this] {
        _chipBar->inputField()->setFocus();
    });
}

int InviteUsersBox::exec() {
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

QVector<QString> InviteUsersBox::selectedUserIds() const {
    QVector<QString> ids;
    for (const auto &chip : _chipBar->chips()) {
        ids.push_back(chip.userId);
    }
    return ids;
}

void InviteUsersBox::accept() {
    _result = Accepted;
    if (_loop) _loop->quit();
}

void InviteUsersBox::reject() {
    if (_invitesInFlight) {
        return;
    }
    _result = Rejected;
    if (_loop) _loop->quit();
}

void InviteUsersBox::tryAddUser() {
    auto text = _chipBar->inputField()->text().trimmed();
    if (text.isEmpty()) return;

    if (!_memberExclusionReady) {
        setStatusText(tr("Loading members..."));
        return;
    }

    // Auto-prepend @ if missing.
    if (!text.startsWith(QLatin1Char('@'))) {
        text.prepend(QLatin1Char('@'));
    }

    // Basic validation: must contain a colon (e.g. @user:server.org).
    if (!text.contains(QLatin1Char(':'))) {
        return;
    }

    if (_chipBar->hasChip(text)) {
        _chipBar->inputField()->clear();
        return;
    }

    if (isExcludedUser(text)) {
        setStatusText(tr("User is already a room member."));
        return;
    }

    _chipBar->addChip(text, text);
    _chipBar->inputField()->clear();
    clearSearchResults();
    setStatusText(QString());
}

void InviteUsersBox::triggerDirectorySearch() {
    if (!_bridge || _invitesInFlight) {
        clearSearchResults();
        return;
    }

    const auto query = _chipBar->inputField()->text().trimmed();
    _lastDirectoryQuery = query;
    if (query.size() < kDirectorySearchMinQueryLength) {
        clearSearchResults();
        return;
    }

    if (!_memberExclusionReady) {
        clearSearchResults();
        setStatusText(tr("Loading members..."));
        return;
    }

    _bridge->searchUserDirectory(query, 50);
}

void InviteUsersBox::applySearchResults(
    const QString &query,
    const QVector<UserProfile> &results,
    bool limited) {
    if (query != _chipBar->inputField()->text().trimmed()) {
        return;
    }

    QVector<UserProfile> filtered;
    filtered.reserve(results.size());
    for (const auto &result : results) {
        if (!_chipBar->hasChip(result.userId)
                && !isExcludedUser(result.userId)) {
            filtered.push_back(result);
        }
    }

    if (filtered.isEmpty()) {
        clearSearchResults();
        setStatusText(tr("No matching users found."));
        return;
    }

    _resultsInner->setMembers(filtered);
    for (const auto &result : filtered) {
        if (result.avatarUrl.startsWith(QStringLiteral("mxc://"))
                && MediaCache::needsResolution(result.avatarUrl)) {
            MediaCache::markRequested(result.avatarUrl);
            _bridge->resolveAvatar(result.avatarUrl);
        }
    }

    if (_resultsScroll) {
        _resultsScroll->setFixedHeight(
            qMin(_resultsInner->sizeHint().height(), kSearchListMaxHeight));
    }
    updateSearchResultsVisibility();
    // Directory search returns only the top matches (no offset pagination);
    // hint the user to refine when the server flagged the results as capped.
    setStatusText(limited
        ? tr("Showing the top matches — keep typing to narrow the search.")
        : QString());
}

bool InviteUsersBox::isExcludedUser(const QString &userId) const {
    return !userId.isEmpty() && _excludedUserIds.contains(userId);
}

void InviteUsersBox::clearSearchResults() {
    if (_resultsInner) {
        _resultsInner->setMembers({});
    }
    updateSearchResultsVisibility();
}

void InviteUsersBox::updateSearchResultsVisibility() {
    if (!_resultsScroll || !_resultsInner) {
        return;
    }
    const bool hasResults = !_resultsInner->members().isEmpty();
    _resultsScroll->setVisible(hasResults);
}

void InviteUsersBox::startInvites() {
    if (_invitesInFlight || _chipBar->chipCount() == 0) {
        return;
    }
    if (!_bridge) {
        accept();
        return;
    }

    _pendingInviteIds = selectedUserIds();
    _failedInviteIds.clear();
    _currentInviteUserId.clear();
    _successfulInvites = 0;
    _invitesInFlight = true;
    if (_searchTimer) {
        _searchTimer->stop();
    }
    clearSearchResults();
    setControlsEnabled(false);
    sendNextInvite();
}

void InviteUsersBox::sendNextInvite() {
    if (!_bridge) {
        _invitesInFlight = false;
        setControlsEnabled(true);
        setStatusText(tr("Invites are unavailable right now."), true);
        return;
    }

    if (_pendingInviteIds.isEmpty()) {
        _invitesInFlight = false;
        if (_failedInviteIds.isEmpty()) {
            emit invitesSent();
            accept();
            return;
        }
        setControlsEnabled(true);
        if (_successfulInvites > 0) {
            // Two counts, one %n each: Qt pluralises a single %n per string.
            setStatusText(
                QStringLiteral("%1 %2").arg(
                    tr("Sent %n invite(s).", "", _successfulInvites),
                    tr("%n failed; you can retry them.",
                        "", int(_failedInviteIds.size()))),
                true);
        } else {
            setStatusText(tr(
                "Failed to send invites. Check the Matrix IDs and try again."),
                true);
        }
        return;
    }

    _currentInviteUserId = _pendingInviteIds.takeFirst();
    setStatusText(tr("Sending invite to %1...")
        .arg(_currentInviteUserId));
    _bridge->inviteUser(_roomId, _currentInviteUserId);
}

void InviteUsersBox::updateInviteButton() {
    const bool hasChips = _chipBar->chipCount() > 0;
    const bool enabled = _controlsEnabled && hasChips;
    _invite->setEnabled(enabled);
    _invite->setButtonStyle(inviteButtonStyle(enabled));
    _invite->setFixedHeight(st::boxButtonHeight);
}

void InviteUsersBox::setControlsEnabled(bool enabled) {
    _controlsEnabled = enabled;
    if (_chipBar) {
        _chipBar->setEnabled(enabled);
    }
    if (_close) {
        _close->setEnabled(enabled);
    }
    if (_skip) {
        _skip->setEnabled(enabled);
    }
    if (_invite) {
        updateInviteButton();
    }
}

void InviteUsersBox::setStatusText(const QString &text, bool error) {
    if (!_hintLabel) {
        return;
    }
    _hintLabel->setText(text.isEmpty() ? inviteHintText() : text);
    {
        QPalette pal = _hintLabel->palette();
        pal.setColor(
            QPalette::WindowText,
            error ? st::attentionButtonFg : st::windowSubTextFg);
        _hintLabel->setPalette(pal);
    }
}

void InviteUsersBox::paintEvent(QPaintEvent *) {
    QPainter p(this);

    p.setOpacity(_bgOpacity);
    p.fillRect(rect(), st::layerBg);

    if (_panel && _layerOpacity > 0) {
        p.setOpacity(_layerOpacity);
        paintBoxShadow(p, _panel->geometry());
    }
}

void InviteUsersBox::mousePressEvent(QMouseEvent *event) {
    if (_panel && !_panel->geometry().contains(event->pos())) {
        reject();
        return;
    }
    QWidget::mousePressEvent(event);
}

void InviteUsersBox::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool InviteUsersBox::eventFilter(QObject *obj, QEvent *event) {
    if (obj == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace TeleMatrix

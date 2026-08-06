// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "dialogs_edit_folder_box.h"
#include "ui/widgets/emoji_input_field.h"

#include <functional>

#include <QCoreApplication>
#include <QEventLoop>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QLinearGradient>
#include <QPainter>
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
#include "ui/style/icon_provider.h"
#include "ui/toast_widget.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/close_button.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/scroll_area.h"

namespace TeleMatrix {

namespace {

constexpr int kShadowExtend = 10;
// A folder name becomes a `u.<name>` Matrix tag key (255-byte cap in the spec /
// the Rust backend). This UI limit counts UTF-16 units; 64 keeps well clear of
// the byte cap even for multi-byte scripts while allowing real folder names
// (the old 12 rejected e.g. 7+ Cyrillic characters).
constexpr int kMaxFolderNameLength = 64;

// Included-chats preview rows use compact filter-list-item metrics.
constexpr int kRowHeight = 44;        // windowFilterSmallItem.height
constexpr int kPhotoSize = 34;        // windowFilterSmallItem.photoSize
constexpr int kPhotoX = 13;           // windowFilterSmallItem.photoPosition.x
constexpr int kPhotoY = 5;            // windowFilterSmallItem.photoPosition.y
constexpr int kNameX = 59;            // windowFilterSmallItem.namePosition.x
constexpr int kRemoveButton = 30;     // windowFilterSmallRemove (notifyClose) size
constexpr int kRemoveRight = 10;      // windowFilterSmallRemoveRight
constexpr int kAddChatsHeight = 48;   // "Add chats" settingsButton row
constexpr int kListPaddingTop = 4;
constexpr int kListPaddingBottom = 8;

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

// A subtle banded separator between sections.
class DividerBand : public QWidget {
public:
    explicit DividerBand(QWidget *parent) : QWidget(parent) {
        setFixedHeight(Style::ConvertScale(8)); // boxDividerHeight
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), st::boxDividerBg);
        // Soft top/bottom shadow (approximates box_divider_top / _bottom).
        const int sh = Style::ConvertScale(2);
        QLinearGradient top(0, 0, 0, sh);
        top.setColorAt(0.0, st::shadowFg);
        top.setColorAt(1.0, Qt::transparent);
        p.fillRect(QRect(0, 0, width(), sh), top);
        QLinearGradient bottom(0, height() - sh, 0, height());
        bottom.setColorAt(0.0, Qt::transparent);
        bottom.setColorAt(1.0, st::shadowFg);
        p.fillRect(QRect(0, height() - sh, width(), sh), bottom);
    }
};

::Ui::TextButton::Style saveButtonStyle(bool enabled) {
    ::Ui::TextButton::Style s;
    s.radius = st::boxRadius;
    s.height = st::boxButtonHeight;
    s.paddingH = Style::ConvertScale(15);
    if (enabled) {
        s.bgOver = &st::lightButtonBgOver;  // transparent until hovered
        s.fg = &st::lightButtonFg;
    } else {
        s.fg = &st::windowSubTextFg;  // flat, muted, no hover
    }
    return s;
}

// Left-aligned "Add Chats" row with a filled accent circle, opens the picker.
class AddChatsButton : public QWidget {
public:
    AddChatsButton(QWidget *parent, std::function<void()> onClick)
        : QWidget(parent)
        , _onClick(std::move(onClick)) {
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);
        setFixedHeight(Style::ConvertScale(kAddChatsHeight));
    }

protected:
    void enterEvent(QEnterEvent *) override { _hovered = true; update(); }
    void leaveEvent(QEvent *) override { _hovered = false; update(); }
    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton && _onClick) {
            _onClick();
        }
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        if (_hovered) {
            p.fillRect(rect(), st::windowBgOver);
        }
        PainterHighQualityEnabler hq(p);
        const auto accent = st::windowActiveTextFg;
        // Round bg diameter = icon size (20px).
        const int d = Style::ConvertScale(20);
        const int cx = Style::ConvertScale(kPhotoX) + Style::ConvertScale(kPhotoSize) / 2;
        const int cy = height() / 2;
        const QRect circle(cx - d / 2, cy - d / 2, d, d);
        p.setPen(Qt::NoPen);
        p.setBrush(accent);
        p.drawEllipse(circle);
        const auto add = Style::IconProvider::tintedIcon(
            QStringLiteral(":/telematrix/icons/"),
            QStringLiteral("folders/folders_add"),
            st::windowFgActive);
        if (!add.isNull()) {
            const auto sz = add.size() / add.devicePixelRatio();
            p.drawImage(QPoint(cx - sz.width() / 2, cy - sz.height() / 2), add);
        }

        p.setFont(st::normalFont);
        p.setPen(accent);
        const QFontMetrics fm(st::normalFont);
        p.drawText(Style::ConvertScale(kNameX),
            (height() + fm.ascent() - fm.descent()) / 2,
            QCoreApplication::translate("DialogsEditFolderBox", "Add Chats"));
    }

private:
    std::function<void()> _onClick;
    bool _hovered = false;
};

} // namespace

// ─────────────────────────────────────────────
// IncludedChatsInner
// ─────────────────────────────────────────────

IncludedChatsInner::IncludedChatsInner(QWidget *parent)
    : QWidget(parent) {
    setMouseTracking(true);
}

void IncludedChatsInner::setRooms(const QVector<RoomPickEntry> &rooms) {
    _rooms = rooms;
    _hoveredRemove = -1;
    setFixedHeight(contentHeight());
    update();
}

int IncludedChatsInner::contentHeight() const {
    if (_rooms.isEmpty()) {
        return 0;
    }
    return kListPaddingTop + _rooms.size() * kRowHeight + kListPaddingBottom;
}

QSize IncludedChatsInner::sizeHint() const {
    // Width 0 so the scroll area sizes us to the viewport; otherwise the
    // right-side remove (×) lands off-screen.
    return { 0, contentHeight() };
}

QSize IncludedChatsInner::minimumSizeHint() const {
    return { 0, contentHeight() };
}

int IncludedChatsInner::indexAt(const QPoint &pos) const {
    if (_rooms.isEmpty()) return -1;
    const int row = (pos.y() - kListPaddingTop) / kRowHeight;
    if (row < 0 || row >= _rooms.size()) return -1;
    return row;
}

QRect IncludedChatsInner::removeRectFor(int rowTop) const {
    return QRect(
        width() - kRemoveRight - kRemoveButton,
        rowTop + (kRowHeight - kRemoveButton) / 2,
        kRemoveButton, kRemoveButton);
}

void IncludedChatsInner::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), st::boxBg);

    const QFontMetrics nameFm(st::semiboldFont);
    for (int i = 0; i < _rooms.size(); ++i) {
        const auto &room = _rooms[i];
        const int y = kListPaddingTop + i * kRowHeight;
        // No row highlight — only the remove (×) reacts to hover (the row is
        // not a button, only the remove IconButton is).

        const QRect userpicRect(kPhotoX, y + kPhotoY, kPhotoSize, kPhotoSize);
        bool paintedAvatar = false;
        if (!room.avatarUrl.isEmpty()) {
            const auto dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
            const auto avatar = MediaCache::loadAvatarPixmapAsync(
                room.avatarUrl, kPhotoSize, dpr, this, userpicRect);
            if (!avatar.isNull()) {
                p.drawPixmap(userpicRect.topLeft(), avatar);
                paintedAvatar = true;
            }
        }
        if (!paintedAvatar) {
            const auto &seed = room.avatarEntityId.isEmpty() ? room.id : room.avatarEntityId;
            Ui::EmptyUserpic::paint(p, seed, room.name, userpicRect.x(), userpicRect.y(), userpicRect.width());
        }

        // Remove (×) — windowFilterSmallRemove; colour changes on hover.
        const auto rm = removeRectFor(y);
        {
            PainterHighQualityEnabler hq(p);
            p.setPen(QPen((i == _hoveredRemove) ? st::menuIconFgOver : st::menuIconFg,
                1.5, Qt::SolidLine, Qt::RoundCap));
            const auto cx = rm.center().x();
            const auto cy = rm.center().y();
            const int half = Style::ConvertScale(4);
            p.drawLine(QPointF(cx - half, cy - half), QPointF(cx + half, cy + half));
            p.drawLine(QPointF(cx + half, cy - half), QPointF(cx - half, cy + half));
        }

        p.setFont(st::semiboldFont);
        p.setPen(st::windowBoldFg);
        const int nameW = rm.left() - kNameX - 8;
        p.drawText(kNameX,
            y + (kRowHeight + nameFm.ascent() - nameFm.descent()) / 2,
            nameFm.elidedText(room.name, Qt::ElideRight, qMax(0, nameW)));
    }
}

void IncludedChatsInner::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) return;
    const int idx = indexAt(e->pos());
    if (idx < 0) return;
    if (removeRectFor(kListPaddingTop + idx * kRowHeight).contains(e->pos())) {
        emit removeRequested(_rooms[idx].id);
    }
}

void IncludedChatsInner::mouseMoveEvent(QMouseEvent *e) {
    const int idx = indexAt(e->pos());
    int overRemove = -1;
    if (idx >= 0
        && removeRectFor(kListPaddingTop + idx * kRowHeight).contains(e->pos())) {
        overRemove = idx;
    }
    if (overRemove != _hoveredRemove) {
        _hoveredRemove = overRemove;
        setCursor(overRemove >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void IncludedChatsInner::leaveEvent(QEvent *) {
    if (_hoveredRemove >= 0) {
        _hoveredRemove = -1;
        setCursor(Qt::ArrowCursor);
        update();
    }
}

// ─────────────────────────────────────────────
// DialogsEditFolderBox
// ─────────────────────────────────────────────

DialogsEditFolderBox::DialogsEditFolderBox(
    Mode mode,
    int folderId,
    const QString &initialName,
    const QVector<RoomPickEntry> &allRooms,
    const QSet<QString> &initialSelected,
    ProtocolBridge *bridge,
    QWidget *parent)
    : QWidget(parent ? parent->window() : nullptr)
    , _mode(mode)
    , _folderId(folderId)
    , _bridge(bridge)
    , _allRooms(allRooms)
    , _draft(initialSelected) {
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
        (mode == Create)
            ? QCoreApplication::translate("DialogsEditFolderBox", "New Folder")
            : QCoreApplication::translate("DialogsEditFolderBox", "Edit Folder"),
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

    // Name input (flat underline; single field so no floating caption).
    auto *inputContainer = new QWidget(_panel);
    auto *inputLayout = new QVBoxLayout(inputContainer);
    inputLayout->setContentsMargins(
        st::boxPadding.left(),
        Style::ConvertScale(4),
        st::boxPadding.right(),
        Style::ConvertScale(12));
    inputLayout->setSpacing(0);
    auto *nameInput = new ::Ui::EmojiInputField(
        inputContainer,
        st::defaultInputField,
        QCoreApplication::translate("DialogsEditFolderBox", "Folder name"));
    _nameField = nameInput;
    _nameField->setMaxLength(kMaxFolderNameLength);
    _nameField->setText(initialName);
    inputLayout->addWidget(nameInput);
    panelLayout->addWidget(inputContainer);

    // Divider between name and included chats.
    panelLayout->addWidget(new DividerBand(_panel));

    // "Included chats" subtitle.
    auto *includeTitle = new QLabel(
        QCoreApplication::translate("DialogsEditFolderBox", "Included chats"), _panel);
    includeTitle->setFont(st::semiboldFont);
    includeTitle->setContentsMargins(
        st::boxPadding.left(), Style::ConvertScale(10),
        st::boxPadding.right(), Style::ConvertScale(2));
    {
        QPalette pal = includeTitle->palette();
        pal.setColor(QPalette::WindowText, st::windowActiveTextFg);
        includeTitle->setPalette(pal);
    }
    panelLayout->addWidget(includeTitle);

    // "Add Chats" row.
    auto *addChats = new AddChatsButton(_panel, [this] { openIncludePicker(); });
    panelLayout->addWidget(addChats);

    // Included-chats preview.
    _preview = new IncludedChatsInner(nullptr);
    _scroll = new ::Ui::ScrollArea(_panel);
    _scroll->setFrameShape(QFrame::NoFrame);
    _scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _scroll->setWidgetResizable(true);
    _scroll->setWidget(_preview);
    _scroll->setAutoFillBackground(false);
    if (auto *vp = _scroll->viewport()) {
        vp->setAutoFillBackground(false);
        vp->setAttribute(Qt::WA_TranslucentBackground);
    }
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

    // Buttons (Cancel + Save; no Delete — deletion lives in the Folders list).
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
        QCoreApplication::translate("DialogsEditFolderBox", "Cancel"),
        cancelStyle,
        buttonsContainer);
    {
        auto f = st::baseFont(14);
        f.setWeight(QFont::DemiBold);
        _cancel->setFont(f);
    }
    connect(_cancel, &QAbstractButton::clicked, this, [this] { reject(); });
    buttonsLayout->addWidget(_cancel);

    _save = new ::Ui::TextButton(
        (mode == Create)
            ? QCoreApplication::translate("DialogsEditFolderBox", "Create")
            : QCoreApplication::translate("DialogsEditFolderBox", "Save"),
        saveButtonStyle(true),
        buttonsContainer);
    {
        auto f = st::baseFont(14);
        f.setWeight(QFont::DemiBold);
        _save->setFont(f);
    }
    _save->setFixedHeight(st::boxButtonHeight);
    connect(_save, &QAbstractButton::clicked, this, [this] { accept(); });
    buttonsLayout->addWidget(_save);

    connect(_nameField, &QTextEdit::textChanged, this, [this] { updateSaveButton(); });
    connect(_nameField, &::Ui::EmojiInputField::submitted, this, [this] {
        if (_save->isEnabled()) accept();
    });
    connect(_preview, &IncludedChatsInner::removeRequested, this,
            [this](const QString &roomId) {
        _draft.remove(roomId);
        refreshPreview();
    });

    if (_bridge) {
        connect(_bridge, &ProtocolBridge::mediaResolved, this,
            [this](bool success, const QString &mxcUrl, const QString &localPath) {
                if (!success || localPath.isEmpty()) {
                    if (mxcUrl.startsWith(QStringLiteral("mxc://"))) {
                        MediaCache::clearRequested(mxcUrl);
                    }
                    return;
                }
                MediaCache::insertPath(mxcUrl, localPath);
                if (_preview) _preview->update();
            });
    }

    refreshPreview();
    updateSaveButton();

    QTimer::singleShot(0, _nameField, [this] {
        _nameField->setFocus();
        _nameField->selectAll();
    });
}

QVector<RoomPickEntry> DialogsEditFolderBox::draftRooms() const {
    QVector<RoomPickEntry> rooms;
    for (const auto &room : _allRooms) {
        if (_draft.contains(room.id)) {
            rooms.push_back(room);
        }
    }
    return rooms;
}

void DialogsEditFolderBox::refreshPreview() {
    if (!_preview || !_scroll) {
        return;
    }
    const auto rooms = draftRooms();
    _preview->setRooms(rooms);
    const int content = _preview->sizeHint().height();
    _scroll->setFixedHeight(qMin(content, Style::ConvertScale(220)));

    if (_bridge) {
        for (const auto &room : rooms) {
            if (room.avatarUrl.startsWith(QStringLiteral("mxc://"))
                    && MediaCache::needsResolution(room.avatarUrl)) {
                MediaCache::markRequested(room.avatarUrl);
                _bridge->resolveAvatar(room.avatarUrl);
            }
        }
    }
}

void DialogsEditFolderBox::openIncludePicker() {
    DialogsIncludeChatsBox box(_allRooms, _draft, _bridge, this);
    if (box.exec() == DialogsIncludeChatsBox::Accepted) {
        _draft = box.selectedRoomIds();
        refreshPreview();
    }
}

void DialogsEditFolderBox::updateSaveButton() {
    const bool hasText = _nameField
        && !_nameField->text().trimmed().isEmpty();
    if (_save) {
        _save->setEnabled(hasText);
        _save->setButtonStyle(saveButtonStyle(hasText));
        _save->setFixedHeight(st::boxButtonHeight);
    }
}

int DialogsEditFolderBox::exec() {
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

void DialogsEditFolderBox::accept() {
    // A folder must contain at least one chat.
    if (_draft.isEmpty()) {
        Ui::ShowToast(QCoreApplication::translate(
            "DialogsEditFolderBox",
            "Please choose at least one chat for this folder."));
        return;
    }
    _result = Accepted;
    if (_loop) _loop->quit();
}

void DialogsEditFolderBox::reject() {
    _result = Rejected;
    if (_loop) _loop->quit();
}

QString DialogsEditFolderBox::folderName() const {
    return _nameField ? _nameField->text().trimmed() : QString();
}

void DialogsEditFolderBox::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setOpacity(_bgOpacity);
    p.fillRect(rect(), st::layerBg);
    if (_panel && _layerOpacity > 0) {
        p.setOpacity(_layerOpacity);
        paintBoxShadow(p, _panel->geometry());
    }
}

void DialogsEditFolderBox::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool DialogsEditFolderBox::eventFilter(QObject *obj, QEvent *event) {
    if (obj == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace TeleMatrix

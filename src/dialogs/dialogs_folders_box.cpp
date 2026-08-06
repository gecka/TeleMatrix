// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "dialogs_folders_box.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFont>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QScrollArea>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include "styles/style_constants.h"
#include "ui/focus_restore.h"
#include "ui/painter.h"
#include "ui/style/icon_provider.h"
#include "ui/widgets/close_button.h"
#include "ui/widgets/scroll_area.h"

namespace TeleMatrix {

namespace {

constexpr int kShadowExtend = 10;

// Layout (logical px) for the folders section.
constexpr int kSubtitleHeight = 8;    // top padding ("My folders" caption removed)
constexpr int kRowHeight = 64;
constexpr int kIconX = 20;            // left of the folder-icon column
constexpr int kIconImg = 27;          // folder icon image → ~20px glyph (36px asset scaled by 20/27)
constexpr int kNameX = 64;
constexpr int kNameTop = 13;          // name baseline offset within the row
constexpr int kStatusTop = 35;        // "N chats" baseline offset
constexpr int kTrashSize = 24;        // info_media_delete glyph
constexpr int kTrashRight = 22;
constexpr int kCreateCircle = 20;     // settingsIconAdd Round bg = icon size
constexpr int kListPaddingBottom = 0;

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

// Name-derived folder icon (the data model carries no per-folder icon), matching
// the sidebar's heuristic; tinted with the accent to match the folders section.
QString folderIconName(const QString &name) {
    const auto t = name.trimmed().toLower();
    if (t.contains(QStringLiteral("unread"))) return QStringLiteral("folders/folders_unread");
    if (t.contains(QStringLiteral("personal"))
        || t.contains(QStringLiteral("private"))
        || t.contains(QStringLiteral("contact"))) return QStringLiteral("folders/folders_private");
    if (t.contains(QStringLiteral("group"))) return QStringLiteral("folders/folders_group");
    if (t.contains(QStringLiteral("channel"))) return QStringLiteral("folders/folders_channels");
    if (t.contains(QStringLiteral("bot"))) return QStringLiteral("folders/folders_bots");
    if (t.contains(QStringLiteral("unmut"))) return QStringLiteral("folders/folders_unmuted");
    if (t.contains(QStringLiteral("setup"))
        || t.contains(QStringLiteral("settings"))) return QStringLiteral("folders/folders_setup");
    return QStringLiteral("folders/folders_custom");
}

void drawCenteredIcon(
        QPainter &p,
        const QString &name,
        const QRect &box,
        const QColor &color,
        int targetSize) {
    auto icon = Style::IconProvider::tintedIcon(
        QStringLiteral(":/telematrix/icons/"), name, color);
    if (icon.isNull()) {
        return;
    }
    auto size = icon.size() / icon.devicePixelRatio();
    // Clamp oversized source glyphs to the target box.
    if (targetSize > 0 && (size.width() > targetSize || size.height() > targetSize)) {
        const auto dpr = icon.devicePixelRatio();
        icon = icon.scaled(
            int(targetSize * dpr), int(targetSize * dpr),
            Qt::KeepAspectRatio, Qt::SmoothTransformation);
        icon.setDevicePixelRatio(dpr);
        size = icon.size() / dpr;
    }
    p.drawImage(
        QPoint(box.left() + (box.width() - size.width()) / 2,
               box.top() + (box.height() - size.height()) / 2),
        icon);
}

} // namespace

// ─────────────────────────────────────────────
// FolderManagerInner
// ─────────────────────────────────────────────

FolderManagerInner::FolderManagerInner(QWidget *parent)
    : QWidget(parent) {
    setMouseTracking(true);
}

void FolderManagerInner::setFolders(const QVector<FolderManagerEntry> &folders) {
    _entries = folders;
    _hovered = -1;
    _hoverTrash = false;
    setFixedHeight(contentHeight());
    update();
}

QVector<int> FolderManagerInner::removedIds() const {
    QVector<int> ids;
    for (const auto id : _removed) {
        ids.push_back(id);
    }
    return ids;
}

int FolderManagerInner::contentHeight() const {
    return kSubtitleHeight + (_entries.size() + 1) * kRowHeight + kListPaddingBottom;
}

QSize FolderManagerInner::sizeHint() const {
    // Width 0 so the scroll area can size us to the viewport (a non-zero min
    // width would keep the widget wider than the panel and push the right-side
    // trash off-screen).
    return { 0, contentHeight() };
}

QSize FolderManagerInner::minimumSizeHint() const {
    return { 0, contentHeight() };
}

int FolderManagerInner::folderTop(int index) const {
    return kSubtitleHeight + index * kRowHeight;
}

QRect FolderManagerInner::trashRect(int rowTop) const {
    return QRect(
        width() - kTrashRight - kTrashSize,
        rowTop + (kRowHeight - kTrashSize) / 2,
        kTrashSize,
        kTrashSize);
}

int FolderManagerInner::elementAt(const QPoint &pos) const {
    if (pos.y() < kSubtitleHeight) {
        return -1;
    }
    const int idx = (pos.y() - kSubtitleHeight) / kRowHeight;
    if (idx < 0 || idx > _entries.size()) {
        return -1;
    }
    return idx; // _entries.size() == the create row
}

void FolderManagerInner::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const int w = width();
    p.fillRect(rect(), st::boxBg);

    const QFontMetrics nameFm(st::semiboldFont);
    const QFontMetrics statusFm(st::normalFont);
    for (int i = 0; i < _entries.size(); ++i) {
        const auto &e = _entries[i];
        const int top = folderTop(i);
        const bool hovered = (_hovered == i);
        p.fillRect(0, top, w, kRowHeight, hovered ? st::windowBgOver : st::boxBg);

        const bool removed = _removed.contains(e.id);

        // Right control: trash (normal) or "Undo" (marked for removal), full opacity.
        const auto tr = trashRect(top);
        int contentRight = tr.left();
        if (removed) {
            p.setFont(st::semiboldFont);
            p.setPen(st::windowActiveTextFg);
            const QFontMetrics undoFm(st::semiboldFont);
            const auto undo = QCoreApplication::translate("DialogsFoldersBox", "Undo");
            const int undoW = undoFm.horizontalAdvance(undo);
            p.drawText(w - kTrashRight - undoW,
                top + (kRowHeight + undoFm.ascent() - undoFm.descent()) / 2, undo);
            contentRight = w - kTrashRight - undoW - 8;
        } else {
            drawCenteredIcon(
                p,
                QStringLiteral("folders/folders_trash"),
                tr,
                (hovered && _hoverTrash) ? st::menuIconFgOver : st::menuIconFg,
                kTrashSize);
        }

        // Row content (icon + name + count), dimmed while marked for removal.
        p.setOpacity(removed ? 0.4 : 1.0);
        drawCenteredIcon(
            p,
            folderIconName(e.name),
            QRect(kIconX, top + (kRowHeight - kIconImg) / 2, kIconImg, kIconImg),
            st::windowActiveTextFg,
            kIconImg);

        const int nameW = contentRight - kNameX - 12;
        p.setFont(st::semiboldFont);
        p.setPen(st::windowBoldFg);
        p.drawText(kNameX, top + kNameTop + nameFm.ascent(),
            nameFm.elidedText(e.name, Qt::ElideRight, qMax(0, nameW)));

        const auto countText = (e.chatCount == 1)
            ? QCoreApplication::translate("DialogsFoldersBox", "1 chat")
            : QCoreApplication::translate("DialogsFoldersBox", "%1 chats").arg(e.chatCount);
        p.setFont(st::normalFont);
        p.setPen(st::windowSubTextFg);
        p.drawText(kNameX, top + kStatusTop + statusFm.ascent(), countText);
        p.setOpacity(1.0);
    }

    // "Create new folder" row.
    const int createTop = folderTop(_entries.size());
    const bool createHover = (_hovered == _entries.size());
    p.fillRect(0, createTop, w, kRowHeight, createHover ? st::windowBgOver : st::boxBg);
    {
        PainterHighQualityEnabler hq(p);
        const QRect circle(
            kIconX + kIconImg / 2 - kCreateCircle / 2,
            createTop + (kRowHeight - kCreateCircle) / 2,
            kCreateCircle, kCreateCircle);
        p.setPen(Qt::NoPen);
        p.setBrush(st::windowActiveTextFg);
        p.drawEllipse(circle);
    }
    drawCenteredIcon(
        p,
        QStringLiteral("folders/folders_add"),
        QRect(kIconX + kIconImg / 2 - kCreateCircle / 2,
              createTop + (kRowHeight - kCreateCircle) / 2,
              kCreateCircle, kCreateCircle),
        st::windowFgActive,
        kCreateCircle);
    p.setFont(st::semiboldFont);
    p.setPen(st::windowActiveTextFg);
    p.drawText(kNameX, createTop + (kRowHeight + nameFm.ascent() - nameFm.descent()) / 2,
        QCoreApplication::translate("DialogsFoldersBox", "Create new folder"));
}

void FolderManagerInner::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) {
        return;
    }
    const int el = elementAt(e->pos());
    if (el < 0) {
        return;
    }
    if (el == _entries.size()) {
        emit createRequested();
        return;
    }
    const auto id = _entries[el].id;
    if (_removed.contains(id)) {
        // Undo: clicking a removed row restores it.
        _removed.remove(id);
        update();
        return;
    }
    if (trashRect(folderTop(el)).contains(e->pos())) {
        _removed.insert(id); // deferred delete — applied when the popup closes
        update();
    } else {
        emit editRequested(id);
    }
}

void FolderManagerInner::mouseMoveEvent(QMouseEvent *e) {
    const int el = elementAt(e->pos());
    const bool overTrash = (el >= 0 && el < _entries.size())
        && trashRect(folderTop(el)).contains(e->pos());
    if (el != _hovered || overTrash != _hoverTrash) {
        _hovered = el;
        _hoverTrash = overTrash;
        setCursor(el >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void FolderManagerInner::leaveEvent(QEvent *) {
    if (_hovered >= 0 || _hoverTrash) {
        _hovered = -1;
        _hoverTrash = false;
        setCursor(Qt::ArrowCursor);
        update();
    }
}

// ─────────────────────────────────────────────
// DialogsFoldersBox
// ─────────────────────────────────────────────

DialogsFoldersBox::DialogsFoldersBox(QWidget *parent)
    : QWidget(parent ? parent->window() : nullptr) {
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
    panelLayout->setContentsMargins(0, 0, 0, Style::ConvertScale(8));
    panelLayout->setSpacing(0);

    // Title bar with close (X).
    auto *titleBar = new QWidget(_panel);
    titleBar->setFixedHeight(st::boxTitleHeight);
    panelLayout->addWidget(titleBar);
    auto *titleText = new QLabel(
        QCoreApplication::translate("DialogsFoldersBox", "Folders"), titleBar);
    titleText->setFont(st::boxTitleFont);
    {
        QPalette pal = titleText->palette();
        pal.setColor(QPalette::WindowText, st::boxTitleFg);
        titleText->setPalette(pal);
    }
    titleText->move(st::boxTitlePosition.x(), st::boxTitlePosition.y());

    auto *close = new ::Ui::CloseButton(titleBar);
    connect(close, &::Ui::CloseButton::clicked, this,
            [this] { reject(); });
    close->move(st::boxWideWidth - st::settingsCloseButtonSize, 0);

    // Separator below the title.
    auto *titleSep = new QWidget(_panel);
    titleSep->setFixedHeight(1);
    titleSep->setAutoFillBackground(true);
    {
        QPalette pal = titleSep->palette();
        pal.setColor(QPalette::Window, st::shadowFg);
        titleSep->setPalette(pal);
    }
    panelLayout->addWidget(titleSep);

    _inner = new FolderManagerInner(nullptr);

    _scroll = new ::Ui::ScrollArea(_panel);
    _scroll->setFrameShape(QFrame::NoFrame);
    _scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _scroll->setWidgetResizable(true);
    _scroll->setWidget(_inner);
    makeScrollTransparent(_scroll);
    panelLayout->addWidget(_scroll);
}

void DialogsFoldersBox::setFolders(const QVector<FolderManagerEntry> &folders) {
    if (_inner) {
        _inner->setFolders(folders);
        relayoutList();
    }
}

void DialogsFoldersBox::relayoutList() {
    if (!_inner || !_scroll) {
        return;
    }
    const int listH = _inner->sizeHint().height();
    _scroll->setFixedHeight(qMin(listH, st::boxMaxListHeight));
}

int DialogsFoldersBox::exec() {
    const auto restoreFocus = ::TeleMatrix::Focus::saveFocusForPopup();

    connect(_inner, &FolderManagerInner::createRequested, this,
            [this] { emit createFolderRequested(); });
    connect(_inner, &FolderManagerInner::editRequested, this,
            [this](int id) { emit editFolderRequested(id); });

    relayoutList();
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

void DialogsFoldersBox::reject() {
    // Apply pending removals when the popup closes (deferred delete + Undo):
    // folders marked with the trash are actually deleted only now.
    if (_inner) {
        for (const int id : _inner->removedIds()) {
            emit deleteFolderRequested(id);
        }
    }
    _result = Rejected;
    if (_loop) _loop->quit();
}

void DialogsFoldersBox::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setOpacity(_bgOpacity);
    p.fillRect(rect(), st::layerBg);
    if (_panel && _layerOpacity > 0) {
        p.setOpacity(_layerOpacity);
        paintBoxShadow(p, _panel->geometry());
    }
}

void DialogsFoldersBox::mousePressEvent(QMouseEvent *event) {
    if (_panel && !_panel->geometry().contains(event->pos())) {
        reject();
        return;
    }
    QWidget::mousePressEvent(event);
}

void DialogsFoldersBox::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool DialogsFoldersBox::eventFilter(QObject *obj, QEvent *event) {
    if (obj == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace TeleMatrix

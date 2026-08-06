// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "dialogs_leave_space_box.h"

#include <limits>

#include <QCoreApplication>
#include <QEventLoop>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QScrollArea>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include "protocol/media_cache.h"
#include "protocol/protocol_bridge.h"
#include "styles/style_constants.h"
#include "ui/focus_restore.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/close_button.h"
#include "ui/widgets/scroll_area.h"

namespace TeleMatrix {

namespace {

constexpr int kShadowExtend = 10;
constexpr int kSubHeaderHeight = 40;

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

QWidget *makeSeparator(QWidget *parent) {
    auto *sep = new QWidget(parent);
    sep->setFixedHeight(1);
    sep->setAutoFillBackground(true);
    QPalette pal = sep->palette();
    pal.setColor(QPalette::Window, st::shadowFg);
    sep->setPalette(pal);
    return sep;
}

int buttonAutoWidth(const QString &text, const QFont &font) {
    return QFontMetrics(font).horizontalAdvance(text) + st::boxButtonMinWidth;
}

} // namespace

DialogsLeaveSpaceBox::DialogsLeaveSpaceBox(
    const QString &spaceName,
    const QVector<RoomPickEntry> &rooms,
    ProtocolBridge *bridge,
    QWidget *parent)
    : QWidget(parent ? parent->window() : nullptr)
    , _rooms(rooms) {
    if (parentWidget()) {
        setGeometry(parentWidget()->rect());
        parentWidget()->installEventFilter(this);
    }
    setFocusPolicy(Qt::StrongFocus);

    const bool hasRooms = !_rooms.isEmpty();

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

    const int boxWidth = st::boxWidth;
    _panel = new RoundedPanel(this);
    _panel->setVisible(false);
    _panel->setFixedWidth(boxWidth);
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
        QCoreApplication::translate("DialogsLeaveSpaceBox", "Leave space"), titleBar);
    titleText->setFont(st::boxTitleFont);
    {
        QPalette pal = titleText->palette();
        pal.setColor(QPalette::WindowText, st::boxTitleFg);
        titleText->setPalette(pal);
    }
    titleText->move(st::boxTitlePosition.x(), st::boxTitlePosition.y());

    auto *close = new ::Ui::CloseButton(titleBar);
    close->move(boxWidth - st::settingsCloseButtonSize, 0);
    connect(close, &::Ui::CloseButton::clicked, this, [this] {
        reject();
    });

    // Description (word-wrapped). Wording depends on whether there are rooms to
    // optionally leave.
    const auto &padding = st::boxPadding;
    const QString description = hasRooms
        ? QCoreApplication::translate(
              "DialogsLeaveSpaceBox",
              "Are you sure you want to leave \"%1\"?\n\nYou'll stay in its rooms "
              "unless you tick the ones you also want to leave.")
              .arg(spaceName)
        : QCoreApplication::translate(
              "DialogsLeaveSpaceBox",
              "Are you sure you want to leave \"%1\"?")
              .arg(spaceName);
    auto *desc = new QLabel(description, _panel);
    desc->setFixedWidth(boxWidth);
    desc->setFont(st::normalFont);
    {
        QPalette pal = desc->palette();
        pal.setColor(QPalette::WindowText, st::boxTextFg);
        desc->setPalette(pal);
    }
    desc->setWordWrap(true);
    desc->setContentsMargins(padding.left(), 0, padding.right(), padding.bottom());
    {
        const int textWidth = boxWidth - padding.left() - padding.right();
        const QFontMetrics fm(st::normalFont);
        const auto bounds = fm.boundingRect(
            QRect(0, 0, textWidth, std::numeric_limits<int>::max()),
            Qt::TextWordWrap,
            description);
        const int lineCount = qMax(
            1, (bounds.height() + fm.lineSpacing() - 1) / fm.lineSpacing());
        desc->setFixedHeight(
            qMax(bounds.height(), lineCount * st::boxLabelLineHeight)
            + padding.bottom());
    }
    panelLayout->addWidget(desc);

    if (hasRooms) {
        // Sub-header: "Rooms in this space" + a Select all / Clear toggle.
        auto *subHeader = new QWidget(_panel);
        subHeader->setFixedHeight(kSubHeaderHeight);
        auto *subLayout = new QHBoxLayout(subHeader);
        subLayout->setContentsMargins(padding.left(), 0, padding.right() - Style::ConvertScale(8), 0);
        subLayout->setSpacing(0);
        auto *subTitle = new QLabel(
            QCoreApplication::translate("DialogsLeaveSpaceBox", "Rooms in this space"),
            subHeader);
        subTitle->setFont(st::normalFont);
        {
            QPalette pal = subTitle->palette();
            pal.setColor(QPalette::WindowText, st::windowSubTextFg);
            subTitle->setPalette(pal);
        }
        subLayout->addWidget(subTitle);
        subLayout->addStretch(1);

        ::Ui::TextButton::Style linkStyle;
        linkStyle.bgOver = &st::lightButtonBgOver;
        linkStyle.fg = &st::lightButtonFg;
        linkStyle.radius = st::buttonRadius;
        linkStyle.height = st::boxButtonHeight;
        linkStyle.paddingH = Style::ConvertScale(8);
        _selectAll = new ::Ui::TextButton(
            QCoreApplication::translate("DialogsLeaveSpaceBox", "Select all"),
            linkStyle,
            subHeader);
        _selectAll->setFont(st::boxButtonFont);
        connect(_selectAll, &QAbstractButton::clicked, this, [this] {
            if (_selected.size() == _rooms.size()) {
                _selected.clear();
            } else {
                for (const auto &r : _rooms) {
                    _selected.insert(r.id);
                }
            }
            if (_inner) {
                _inner->setSelected(_selected);
            }
            refreshSelectAll();
        });
        subLayout->addWidget(_selectAll);
        panelLayout->addWidget(subHeader);

        panelLayout->addWidget(makeSeparator(_panel));

        // Multi-select room list (default: nothing ticked).
        _inner = new ChatPickInner(nullptr);
        _inner->setRooms(_rooms);
        _inner->setSelected(_selected);

        auto *scroll = new ::Ui::ScrollArea(_panel);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setWidgetResizable(true);
        scroll->setFixedHeight(qMin(_inner->height(), st::boxMaxListHeight));
        scroll->setWidget(_inner);
        makeScrollTransparent(scroll);
        panelLayout->addWidget(scroll);

        panelLayout->addWidget(makeSeparator(_panel));

        connect(_inner, &ChatPickInner::roomClicked, this,
                [this](const QString &id) { toggleRoom(id); });

        refreshSelectAll();
    }

    // Buttons: Cancel + Leave (Leave = attention/destructive red).
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
    buttonsLayout->setSpacing(st::boxButtonPadding.left());
    buttonsLayout->addStretch(1);

    ::Ui::TextButton::Style cancelStyle;
    cancelStyle.bg = &st::lightButtonBg;
    cancelStyle.bgOver = &st::lightButtonBgOver;
    cancelStyle.fg = &st::lightButtonFg;
    cancelStyle.radius = st::buttonRadius;
    const auto cancelLabel =
        QCoreApplication::translate("DialogsLeaveSpaceBox", "Cancel");
    _cancel = new ::Ui::TextButton(cancelLabel, cancelStyle, buttonsContainer);
    _cancel->setFont(st::boxButtonFont);
    _cancel->setFixedSize(
        buttonAutoWidth(cancelLabel, st::boxButtonFont), st::boxButtonHeight);
    connect(_cancel, &QAbstractButton::clicked, this, [this] { reject(); });
    buttonsLayout->addWidget(_cancel);

    ::Ui::TextButton::Style leaveStyle;
    leaveStyle.bg = &st::lightButtonBg;
    leaveStyle.bgOver = &st::attentionButtonBgOver;
    leaveStyle.fg = &st::attentionButtonFg;
    leaveStyle.radius = st::buttonRadius;
    const auto leaveLabel =
        QCoreApplication::translate("DialogsLeaveSpaceBox", "Leave");
    _leaveButton = new ::Ui::TextButton(leaveLabel, leaveStyle, buttonsContainer);
    _leaveButton->setFont(st::boxButtonFont);
    _leaveButton->setFixedSize(
        buttonAutoWidth(leaveLabel, st::boxButtonFont), st::boxButtonHeight);
    connect(_leaveButton, &QAbstractButton::clicked, this, [this] { accept(); });
    buttonsLayout->addWidget(_leaveButton);

    // Resolve room avatar URLs (only matters when the list is shown).
    if (bridge && hasRooms) {
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

void DialogsLeaveSpaceBox::toggleRoom(const QString &roomId) {
    if (_selected.contains(roomId)) {
        _selected.remove(roomId);
    } else {
        _selected.insert(roomId);
    }
    if (_inner) {
        _inner->setSelected(_selected);
    }
    refreshSelectAll();
}

void DialogsLeaveSpaceBox::refreshSelectAll() {
    if (!_selectAll) {
        return;
    }
    const bool all = !_rooms.isEmpty() && _selected.size() == _rooms.size();
    _selectAll->setText(all
        ? QCoreApplication::translate("DialogsLeaveSpaceBox", "Clear")
        : QCoreApplication::translate("DialogsLeaveSpaceBox", "Select all"));
}

int DialogsLeaveSpaceBox::exec() {
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

void DialogsLeaveSpaceBox::accept() {
    _result_selected = _selected;
    _result = Accepted;
    if (_loop) _loop->quit();
}

void DialogsLeaveSpaceBox::reject() {
    _result = Rejected;
    if (_loop) _loop->quit();
}

void DialogsLeaveSpaceBox::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setOpacity(_bgOpacity);
    p.fillRect(rect(), st::layerBg);
    if (_panel && _layerOpacity > 0) {
        p.setOpacity(_layerOpacity);
        paintBoxShadow(p, _panel->geometry());
    }
}

void DialogsLeaveSpaceBox::mousePressEvent(QMouseEvent *event) {
    if (_panel && !_panel->geometry().contains(event->pos())) {
        reject();
        return;
    }
    QWidget::mousePressEvent(event);
}

void DialogsLeaveSpaceBox::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool DialogsLeaveSpaceBox::eventFilter(QObject *obj, QEvent *event) {
    if (obj == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace TeleMatrix

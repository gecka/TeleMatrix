// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "settings/appearance/theme_selector_panel.h"

#include <cmath>
#include <utility>

#include <QApplication>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPalette>
#include <QScrollArea>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include "app/app_controller.h"
#include "styles/style_constants.h"
#include "theme/chat_background.h"
#include "theme/theme_manager.h"
#include "theme/theme_registry.h"
#include "ui/painter.h"
#include "ui/style/runtime_scale.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/close_button.h"

namespace TeleMatrix {

namespace {

// Wider than a single settings column because it carries two theme cards
// side by side.
constexpr int kPanelWidth = 340;
constexpr int kAnimationDuration = 200;
constexpr int kBorderWidth = 1;
// Apply button commits the theme: flush across the bottom of the column,
// square, 46px tall, semibold label, active-button colours.
constexpr int kApplyButtonHeight = 46;

// Card aspect ratio 80x92, scaled up from a four-across grid to the
// two-across layout used in this panel.
constexpr int kCardRatioNum = 92;
constexpr int kCardRatioDen = 80;
constexpr int kBubbleW = 76;
constexpr int kBubbleH = 26;
constexpr int kBubbleX = 12;
constexpr int kBubbleY = 16;
constexpr int kBubbleSkip = 12;
constexpr int kBubbleRadius = 4;
constexpr int kRadioSize = 22;
constexpr int kRadioBottom = 22;
constexpr int kCardRadius = 10;

// Inner layout.
constexpr int kSidePadding = 20;
constexpr int kCardGap = 12;
constexpr int kTopSkip = 12;
constexpr int kNameHeight = 24;
constexpr int kCaptionSkip = 8;
constexpr int kCaptionHeight = 18;
constexpr int kBlockSkip = 14;

[[nodiscard]] int scaled(int value) {
    return Style::ConvertScale(value);
}

[[nodiscard]] QString dayNightCaption(bool night) {
    return night
        ? QCoreApplication::translate("ThemeSelectorPanel", "Night")
        : QCoreApplication::translate("ThemeSelectorPanel", "Day");
}

void makeScrollTransparent(QScrollArea *scroll) {
    scroll->setAutoFillBackground(false);
    if (auto *viewport = scroll->viewport()) {
        viewport->setAutoFillBackground(false);
        viewport->setAttribute(Qt::WA_TranslucentBackground);
    }
}

[[nodiscard]] ::Ui::TextButton::Style applyButtonStyle() {
    auto style = ::Ui::TextButton::Style();
    style.radius = 0; // FlatButton is square; the panel's corner does the rest
    style.height = Style::ConvertScale(kApplyButtonHeight);
    style.paddingH = Style::ConvertScale(17); // dialogsUpdateButton width: -34px
    style.bg = &st::activeButtonBg;
    style.bgOver = &st::activeButtonBgOver;
    style.fg = &st::activeButtonFg;
    return style;
}

[[nodiscard]] qreal luminance(const QColor &color) {
    return (0.299 * color.redF()) + (0.587 * color.greenF()) + (0.114 * color.blueF());
}

// Contrast-correct the radio against the wallpaper under it so an accent
// close to its own background doesn't vanish.
[[nodiscard]] QColor ensureContrast(const QColor &color, const QColor &background) {
    constexpr auto kMinDelta = 0.28;
    const auto backgroundIsLight = luminance(background) > 0.5;
    if (!color.isValid()
        || (std::abs(luminance(color) - luminance(background)) < kMinDelta)) {
        return backgroundIsLight ? QColor(0x30, 0x30, 0x30) : QColor(Qt::white);
    }
    return color;
}

} // namespace

// ─────────────────────────────────────────────
// ThemeSelectorInner
// ─────────────────────────────────────────────

ThemeSelectorInner::ThemeSelectorInner(
        Theme::ThemeManager *themeManager,
        QWidget *parent)
    : QWidget(parent)
    , _themeManager(themeManager) {
    setMouseTracking(true);
    for (const auto &theme : Theme::ThemesByName()) {
        for (const auto night : { false, true }) {
            _cards.push_back({ theme.id, theme.displayName(), night });
        }
    }
}

int ThemeSelectorInner::themeCount() const {
    return int(_cards.size()) / 2;
}

int ThemeSelectorInner::cardWidthFor(int width) const {
    const auto usable = width - (2 * scaled(kSidePadding)) - scaled(kCardGap);
    return qMax(usable / 2, 1);
}

int ThemeSelectorInner::cardHeightFor(int width) const {
    return cardWidthFor(width) * kCardRatioNum / kCardRatioDen;
}

int ThemeSelectorInner::themeBlockHeightFor(int width) const {
    return scaled(kNameHeight)
        + cardHeightFor(width)
        + scaled(kCaptionSkip)
        + scaled(kCaptionHeight)
        + scaled(kBlockSkip);
}

int ThemeSelectorInner::contentHeightFor(int width) const {
    return scaled(kTopSkip) + (themeCount() * themeBlockHeightFor(width));
}

QSize ThemeSelectorInner::sizeHint() const {
    // Width 0: let the scroll area size us to its viewport.
    return { 0, contentHeightFor(width()) };
}

QSize ThemeSelectorInner::minimumSizeHint() const {
    return { 0, contentHeightFor(width()) };
}

QRect ThemeSelectorInner::cardRect(int themeIndex, bool night) const {
    const auto cardW = cardWidthFor(width());
    const auto top = scaled(kTopSkip)
        + (themeIndex * themeBlockHeightFor(width()))
        + scaled(kNameHeight);
    const auto left = night
        ? (scaled(kSidePadding) + cardW + scaled(kCardGap))
        : scaled(kSidePadding);
    return QRect(left, top, cardW, cardHeightFor(width()));
}

int ThemeSelectorInner::cardAt(const QPoint &pos) const {
    for (auto i = 0; i != int(_cards.size()); ++i) {
        if (cardRect(i / 2, _cards[i].night).contains(pos)) {
            return i;
        }
    }
    return -1;
}

bool ThemeSelectorInner::isSelected(const Card &card) const {
    return _themeManager
        && (_themeManager->themeId() == card.themeId)
        && (_themeManager->isNight() == card.night);
}

QRect ThemeSelectorInner::selectedBlockRect() const {
    for (auto i = 0; i != int(_cards.size()); ++i) {
        if (isSelected(_cards[i])) {
            const auto index = i / 2;
            return QRect(
                0,
                scaled(kTopSkip) + (index * themeBlockHeightFor(width())),
                width(),
                themeBlockHeightFor(width()));
        }
    }
    return {};
}

void ThemeSelectorInner::buildCard(Card &card, QSize size, qreal dpr) {
    const auto device = QSize(
        qRound(size.width() * dpr),
        qRound(size.height() * dpr));
    const auto colors = Theme::PreviewColors(card.themeId, card.night);

    auto image = Theme::GenerateCornerGradient(device, colors.background);
    if (image.isNull()) {
        return;
    }
    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);

    // The bubbles: received top-left, sent below it on the right.
    const auto received = QRect(
        scaled(kBubbleX),
        scaled(kBubbleY),
        scaled(kBubbleW),
        scaled(kBubbleH));
    const auto sent = QRect(
        size.width() - received.width() - scaled(kBubbleX),
        received.bottom() + scaled(kBubbleSkip),
        received.width(),
        received.height());
    {
        auto p = QPainter(&image);
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        const auto radius = scaled(kBubbleRadius);
        p.setBrush(colors.received);
        p.drawRoundedRect(received, radius, radius);
        p.setBrush(colors.sent);
        p.drawRoundedRect(sent, radius, radius);
    }

    // Contrast the radio against the wallpaper it actually sits on.
    const auto radio = scaled(kRadioSize);
    const auto under = QRect(
        QPoint(
            qRound(((size.width() - radio) / 2) * dpr),
            qRound((size.height() - radio - scaled(kRadioBottom)) * dpr)),
        QSize(qRound(radio * dpr), qRound(radio * dpr)));
    const auto beneath = Theme::AverageColor(image.copy(under));
    card.accent = ensureContrast(colors.accent, beneath);
    card.ring = (luminance(beneath) > 0.5)
        ? QColor(0, 0, 0, 102)
        : QColor(255, 255, 255, 192);

    card.pixmap = QPixmap::fromImage(std::move(image));
}

void ThemeSelectorInner::dropCards() {
    for (auto &card : _cards) {
        card.pixmap = QPixmap();
    }
}

void ThemeSelectorInner::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    const auto wanted = contentHeightFor(width());
    if (height() != wanted) {
        setFixedHeight(wanted);
    }
    const auto size = QSize(cardWidthFor(width()), cardHeightFor(width()));
    const auto dpr = devicePixelRatioF();
    if (_builtFor != size || !qFuzzyCompare(_builtDpr, dpr)) {
        _builtFor = size;
        _builtDpr = dpr;
        dropCards();
    }
}

void ThemeSelectorInner::paintEvent(QPaintEvent *e) {
    QPainter p(this);
    p.fillRect(e->rect(), st::windowBg);

    const auto exposed = e->rect();
    const auto cardSize = QSize(cardWidthFor(width()), cardHeightFor(width()));
    const auto dpr = devicePixelRatioF();

    for (auto index = 0; index != themeCount(); ++index) {
        const auto blockTop = scaled(kTopSkip)
            + (index * themeBlockHeightFor(width()));
        const auto blockRect = QRect(
            0, blockTop, width(), themeBlockHeightFor(width()));
        if (!blockRect.intersects(exposed)) {
            continue; // scrolled out of view: never composites its gradient
        }

        p.setFont(st::semiboldFont);
        p.setPen(st::windowBoldFg);
        p.drawText(
            QRect(0, blockTop, width(), scaled(kNameHeight)),
            Qt::AlignCenter,
            _cards[index * 2].name);

        for (const auto night : { false, true }) {
            const auto cardIndex = (index * 2) + (night ? 1 : 0);
            auto &card = _cards[cardIndex];
            const auto target = cardRect(index, night);
            const auto selected = isSelected(card);

            if (card.pixmap.isNull() && !cardSize.isEmpty()) {
                buildCard(card, cardSize, dpr);
            }
            if (!card.pixmap.isNull()) {
                PainterHighQualityEnabler hq(p);
                auto path = QPainterPath();
                path.addRoundedRect(target, scaled(kCardRadius), scaled(kCardRadius));
                p.save();
                p.setClipPath(path);
                p.drawPixmap(target, card.pixmap);
                p.restore();
            }

            {
                PainterHighQualityEnabler hq(p);
                const auto radio = scaled(kRadioSize);
                const auto radioRect = QRect(
                    target.x() + ((target.width() - radio) / 2),
                    target.bottom() + 1 - radio - scaled(kRadioBottom),
                    radio,
                    radio);
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(selected ? card.accent : card.ring, 2));
                p.drawEllipse(radioRect);
                if (selected) {
                    const auto inset = radio / 4;
                    p.setPen(Qt::NoPen);
                    p.setBrush(card.accent);
                    p.drawEllipse(radioRect.adjusted(inset, inset, -inset, -inset));
                }

                if ((_hovered == cardIndex) && !selected) {
                    p.setBrush(Qt::NoBrush);
                    p.setPen(QPen(st::windowBgRipple, 2));
                    p.drawRoundedRect(
                        target.adjusted(1, 1, -1, -1),
                        scaled(kCardRadius),
                        scaled(kCardRadius));
                }
            }

            p.setFont(st::normalFont);
            p.setPen(selected ? st::windowActiveTextFg : st::windowSubTextFg);
            p.drawText(
                QRect(
                    target.x(),
                    target.bottom() + scaled(kCaptionSkip),
                    target.width(),
                    scaled(kCaptionHeight)),
                Qt::AlignCenter,
                dayNightCaption(night));
        }
    }
}

void ThemeSelectorInner::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) {
        return;
    }
    const auto index = cardAt(e->pos());
    if (index < 0 || !_themeManager) {
        return;
    }
    const auto &card = _cards[index];
    _themeManager->setThemeAndMode(
        card.themeId,
        card.night ? Theme::ThemeMode::Night : Theme::ThemeMode::Day);
}

void ThemeSelectorInner::mouseMoveEvent(QMouseEvent *e) {
    const auto index = cardAt(e->pos());
    if (index != _hovered) {
        _hovered = index;
        setCursor(index >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void ThemeSelectorInner::leaveEvent(QEvent *) {
    if (_hovered >= 0) {
        _hovered = -1;
        setCursor(Qt::ArrowCursor);
        update();
    }
}

// ─────────────────────────────────────────────
// ThemeSelectorPanel
// ─────────────────────────────────────────────

ThemeSelectorPanel::ThemeSelectorPanel(AppController *controller, QWidget *parent)
    : QWidget(parent)
    , _controller(controller)
    , _themeManager(controller ? controller->themeManager() : nullptr) {
    setFocusPolicy(Qt::StrongFocus);
    // The panel is opaque: the app behind it keeps its own colours, and we
    // never dim it -- watching the chat re-skin live is the point.
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    hide();

    auto *root = new QVBoxLayout(this);
    // The one-pixel separator is painted at x = 0; keep the children off it, or
    // the scroll area's opaque background paints straight over it.
    root->setContentsMargins(kBorderWidth, 0, 0, 0);
    root->setSpacing(0);

    _topBar = new QWidget(this);
    // Line the panel's header up with the room's top bar next to it.
    _topBar->setFixedHeight(st::topBarHeight);
    root->addWidget(_topBar);

    _title = new QLabel(
        QCoreApplication::translate("ThemeSelectorPanel", "Color theme"), _topBar);
    _title->setFont(st::boxTitleFont);
    _title->move(
        Style::ConvertScale(16),
        (st::topBarHeight - st::boxTitleFont->height) / 2);

    auto *close = new ::Ui::CloseButton(_topBar);
    connect(close, &::Ui::CloseButton::clicked, this,
            [this] { hideAnimated(); });
    _close = close;

    _inner = new ThemeSelectorInner(_themeManager, nullptr);

    _scroll = new QScrollArea(this);
    _scroll->setFrameShape(QFrame::NoFrame);
    _scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    _scroll->setWidgetResizable(true);
    _scroll->setWidget(_inner);
    makeScrollTransparent(_scroll);
    root->addWidget(_scroll, 1);

    // Commit the theme with a button flush across the bottom of the column,
    // not with a header action.
    _apply = new ::Ui::TextButton(
        QCoreApplication::translate("ThemeSelectorPanel", "Apply permanently"),
        applyButtonStyle(),
        this);
    _apply->setFont(st::semiboldFont);
    root->addWidget(_apply);
    connect(_apply, &QAbstractButton::clicked, this, [this] { save(); });

    _animation = new QVariantAnimation(this);
    _animation->setDuration(kAnimationDuration);
    connect(_animation, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) { applyProgress(value.toReal()); });
    connect(_animation, &QVariantAnimation::finished, this, [this] {
        if (_progress <= 0.0001) {
            _state = State::Hidden;
            hide();
            qApp->removeEventFilter(this);
            // Closing without Save puts the theme the panel opened on back.
            if (_controller) {
                _controller->endThemePreview();
            }
            emit closed();
        } else {
            _state = State::Visible;
        }
    });

    applyTheme();

    // Clicking a card applies its theme under the open panel: re-skin our own
    // chrome and move the radio.
    if (_themeManager) {
        connect(_themeManager, &Theme::ThemeManager::themeChanged, this,
                [this](bool, Theme::ThemeMode) { applyTheme(); });
    }
}

int ThemeSelectorPanel::panelWidth() const {
    const auto wanted = Style::ConvertScale(kPanelWidth);
    const auto available = parentWidget() ? parentWidget()->width() : wanted;
    return qMin(wanted, qMax(available, 1));
}

void ThemeSelectorPanel::applyTheme() {
    if (_title) {
        auto pal = _title->palette();
        pal.setColor(QPalette::WindowText, st::boxTitleFg);
        _title->setPalette(pal);
    }
    if (_apply) {
        _apply->setButtonStyle(applyButtonStyle());
    }
    if (_topBar) {
        _topBar->update();
    }
    if (_inner) {
        _inner->update();
    }
    update();
}

void ThemeSelectorPanel::syncGeometry() {
    auto *parent = parentWidget();
    if (!parent) {
        return;
    }
    const auto width = panelWidth();
    const auto shown = qRound(width * _progress);
    setGeometry(parent->width() - shown, 0, width, parent->height());
}

void ThemeSelectorPanel::applyProgress(qreal progress) {
    _progress = progress;
    syncGeometry();
}

void ThemeSelectorPanel::startAnimation(qreal to) {
    _animation->stop();
    _animation->setStartValue(_progress);
    _animation->setEndValue(to);
    _animation->setEasingCurve(
        (to > _progress) ? QEasingCurve::OutCubic : QEasingCurve::InCubic);
    _state = (to > _progress) ? State::Opening : State::Closing;
    _animation->start();
}

bool ThemeSelectorPanel::isShown() const {
    return (_state == State::Visible) || (_state == State::Opening);
}

void ThemeSelectorPanel::showAnimated() {
    if (isShown()) {
        return;
    }
    if (_controller) {
        _controller->beginThemePreview();
    }
    applyProgress(0.);
    show();
    raise();
    setFocus();
    // Escape must close the panel wherever the focus went, so watch the whole
    // application while we are up. Removed again when we finish hiding.
    qApp->installEventFilter(this);
    startAnimation(1.);

    // Queued: the scroll area has not laid the cards out yet, and with twenty
    // themes the current one is usually below the fold.
    QMetaObject::invokeMethod(this, [this] { scrollToSelected(); },
                              Qt::QueuedConnection);
}

void ThemeSelectorPanel::hideAnimated() {
    if ((_state == State::Hidden) || (_state == State::Closing)) {
        return;
    }
    startAnimation(0.);
}

void ThemeSelectorPanel::scrollToSelected() {
    if (!_inner || !_scroll || (_inner->width() <= 0)) {
        return;
    }
    const auto block = _inner->selectedBlockRect();
    if (!block.isNull()) {
        _scroll->ensureVisible(0, block.center().y(), 0, block.height() / 2);
    }
}

void ThemeSelectorPanel::save() {
    if (_controller) {
        _controller->saveThemePreview();
    }
    hideAnimated();
}

void ThemeSelectorPanel::paintEvent(QPaintEvent *e) {
    QPainter p(this);
    p.fillRect(e->rect(), st::windowBg);
    // Separate the panel from the chat with a one-pixel shadow.
    p.fillRect(QRect(0, 0, kBorderWidth, height()), st::shadowFg);
    if (_topBar) {
        p.fillRect(
            QRect(kBorderWidth, _topBar->height() - 1, width() - kBorderWidth, 1),
            st::shadowFg);
    }
}

void ThemeSelectorPanel::layoutBars() {
    if (_close && _topBar) {
        _close->move(
            _topBar->width() - _close->width() - Style::ConvertScale(6),
            (_topBar->height() - _close->height()) / 2);
    }
}

void ThemeSelectorPanel::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    layoutBars();
}

void ThemeSelectorPanel::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape) {
        hideAnimated();
        e->accept();
        return;
    }
    QWidget::keyPressEvent(e);
}

bool ThemeSelectorPanel::eventFilter(QObject *object, QEvent *event) {
    // The filter is on the application, and only while the panel is up.
    if ((object == parentWidget()) && (event->type() == QEvent::Resize)) {
        syncGeometry();
    } else if (isShown()
        && ((event->type() == QEvent::KeyPress)
            || (event->type() == QEvent::ShortcutOverride))) {
        const auto *key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            hideAnimated();
            event->accept();
            return true;
        }
    }
    return QWidget::eventFilter(object, event);
}

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "dialogs_update_bar.h"

#include "styles/style_constants.h"
#include "ui/painter.h"
#include "ui/style/runtime_scale.h"

#include <QEnterEvent>
#include <QFontMetrics>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>

namespace TeleMatrix {
namespace {

// tdesktop ui/arc_angles.h — QPainter angles are sixteenths of a degree.
constexpr auto kFullLength = 360 * 16;
constexpr auto kQuarterLength = kFullLength / 4;
constexpr auto kHalfLength = kFullLength / 2;

constexpr auto kRippleShowDuration = 650;
constexpr auto kRippleHideDuration = 200;

/// tdesktop's `UpdateIcon()`: a filled disc with the two circular-arrow strokes
/// and their arrowheads punched back out of it, so the gradient shows through the
/// glyph rather than the glyph being painted on top.
///
/// Palette-independent by construction — white and black here only feed
/// CompositionMode_Clear — so unlike upstream it needs no rebuild on theme change.
[[nodiscard]] QImage UpdateIcon() {
    const auto iconSize = st::dialogsInstallUpdateIconSize;
    const auto ratio = Style::DevicePixelRatio();
    auto result = QImage(
        QSize(iconSize, iconSize) * ratio,
        QImage::Format_ARGB32_Premultiplied);
    result.setDevicePixelRatio(ratio);
    result.fill(Qt::transparent);
    {
        auto p = QPainter(&result);
        auto hq = PainterHighQualityEnabler(p);
        auto path = QPainterPath();

        const auto fullRect = QRectF(0, 0, iconSize, iconSize);
        const auto margin = st::dialogsInstallUpdateIconInnerMargin;
        const auto rect = fullRect.adjusted(margin, margin, -margin, -margin);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::white);
        p.drawEllipse(fullRect);

        p.setCompositionMode(QPainter::CompositionMode_Clear);

        auto pen = QPen(Qt::black);
        pen.setWidthF(Style::ConvertScaleExact(2.));
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);

        constexpr auto kShift = int(20 * 16);
        p.drawArc(rect, -kShift, kQuarterLength + kShift);
        p.drawArc(rect, kHalfLength - kShift, kQuarterLength + kShift);

        const auto side1 = st::dialogsInstallUpdateIconSide1;
        const auto side2 = st::dialogsInstallUpdateIconSide2;
        const auto top = rect.y() - side1;
        const auto bottom = rect.y() + rect.height() - side1;
        const auto centerX = rect.center().x();
        path.moveTo(centerX, bottom + side1 + side2);
        path.lineTo(centerX, bottom + side1 - side2);
        path.lineTo(centerX + side2, bottom + side1);
        path.closeSubpath();

        path.moveTo(centerX, top + side1 + side2);
        path.lineTo(centerX, top + side1 - side2);
        path.lineTo(centerX - side2, top + side1);
        path.closeSubpath();

        p.fillPath(path, Qt::black);
    }
    return result;
}

/// lib_ui `anim::with_alpha` — scales the existing alpha, it does not replace it.
[[nodiscard]] QColor WithAlpha(QColor color, qreal alpha) {
    color.setAlphaF(color.alphaF() * alpha);
    return color;
}

/// Matches HistoryWidget's inviteButtonFont: baseFont(14) at DemiBold.
[[nodiscard]] QFont ActionFont() {
    return st::baseFont(14, true);
}

} // namespace

DialogsUpdateBar::DialogsUpdateBar(QWidget *parent)
    : QWidget(parent)
    , _textIcon(UpdateIcon()) {
    setMouseTracking(true);
    resize(st::columnMinimalWidthLeft, barHeight());
}

DialogsUpdateBar::~DialogsUpdateBar() = default;

int DialogsUpdateBar::barHeight() const {
    return (_mode == Mode::Ready)
        ? st::dialogsUpdateButtonHeight
        : st::dialogsUpdateBarTwoLineHeight;
}

void DialogsUpdateBar::setReadyMode(const QString &label, bool enabled) {
    if (_mode == Mode::Ready && _label == label && _enabled == enabled) {
        return;
    }
    _mode = Mode::Ready;
    _label = label;
    _enabled = enabled;
    _hovered = Action::None;
    refreshCursor();
    update();
}

void DialogsUpdateBar::setPromptMode(const QString &message) {
    if (_mode == Mode::Prompt && _message == message) {
        return;
    }
    _mode = Mode::Prompt;
    _message = message;
    _enabled = true;
    _hovered = Action::None;
    stopRipple();
    refreshCursor();
    update();
}

void DialogsUpdateBar::setDownloadingMode(const QString &message, int percent) {
    if (_mode == Mode::Downloading && _message == message && _percent == percent) {
        return;
    }
    _mode = Mode::Downloading;
    _message = message;
    _percent = percent;
    _enabled = true;
    stopRipple();
    refreshCursor();
    update();
}

void DialogsUpdateBar::refreshCursor() {
    // Derived, never hard-set: setDownloadingMode() runs on every progress tick,
    // and assigning ArrowCursor there stomped the hand cursor while the pointer
    // sat on CANCEL.
    setCursor((_hovered != Action::None || surfaceClickable())
        ? Qt::PointingHandCursor
        : Qt::ArrowCursor);
}

bool DialogsUpdateBar::surfaceClickable() const {
    return (_mode == Mode::Ready) && _enabled;
}

QString DialogsUpdateBar::primaryText() const {
    return (_mode == Mode::Downloading) ? tr("CANCEL") : tr("UPDATE");
}

QString DialogsUpdateBar::secondaryText() const {
    return tr("SKIP");
}

int DialogsUpdateBar::firstRowHeight() const {
    return std::max(0, height() - st::dialogsUpdateActionRowHeight);
}

QRect DialogsUpdateBar::actionRowRect() const {
    const auto top = firstRowHeight();
    return QRect(0, top, width(), height() - top);
}

QRect DialogsUpdateBar::primaryRect() const {
    if (_mode == Mode::Ready) {
        return QRect();
    }
    const auto row = actionRowRect();
    if (_mode == Mode::Downloading) {
        // Only one action, so it takes the whole row.
        return row;
    }
    // Right half. Width taken as the remainder so an odd width leaves no seam.
    const auto half = row.width() / 2;
    return QRect(row.x() + half, row.y(), row.width() - half, row.height());
}

QRect DialogsUpdateBar::secondaryRect() const {
    if (_mode != Mode::Prompt) {
        return QRect();
    }
    const auto row = actionRowRect();
    return QRect(row.x(), row.y(), row.width() / 2, row.height());
}

DialogsUpdateBar::Action DialogsUpdateBar::actionAt(QPoint pos) const {
    if (primaryRect().contains(pos)) {
        return Action::Primary;
    }
    if (secondaryRect().contains(pos)) {
        return Action::Secondary;
    }
    return Action::None;
}

void DialogsUpdateBar::enterEvent(QEnterEvent *e) {
    _over = true;
    update();
    QWidget::enterEvent(e);
}

void DialogsUpdateBar::leaveEvent(QEvent *e) {
    _over = false;
    if (_hovered != Action::None) {
        _hovered = Action::None;
        refreshCursor();
    }
    update();
    QWidget::leaveEvent(e);
}

void DialogsUpdateBar::mouseMoveEvent(QMouseEvent *e) {
    const auto hovered = actionAt(e->pos());
    if (_hovered != hovered) {
        _hovered = hovered;
        refreshCursor();
        update();
    }
    QWidget::mouseMoveEvent(e);
}

void DialogsUpdateBar::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(e);
        return;
    }
    if (surfaceClickable() && actionAt(e->pos()) == Action::None) {
        startRipple(e->pos());
    }
    QWidget::mousePressEvent(e);
}

void DialogsUpdateBar::mouseReleaseEvent(QMouseEvent *e) {
    stopRipple();
    if (e->button() == Qt::LeftButton) {
        switch (actionAt(e->pos())) {
        case Action::Primary:
            if (_mode == Mode::Downloading) {
                Q_EMIT cancelRequested();
            } else {
                Q_EMIT updateRequested();
            }
            break;
        case Action::Secondary:
            Q_EMIT skipRequested();
            break;
        case Action::None:
            if (surfaceClickable() && rect().contains(e->pos())) {
                Q_EMIT applyRequested();
            }
            break;
        }
    }
    QWidget::mouseReleaseEvent(e);
}

void DialogsUpdateBar::startRipple(QPoint origin) {
    _rippleOrigin = origin;
    _rippleOpacity = 1.;
    if (_rippleFade) {
        _rippleFade->stop();
    }
    // Grow to the farthest corner, so the fill always reaches every edge.
    auto farthest = 0.;
    for (const auto &corner : {
            rect().topLeft(), rect().topRight(),
            rect().bottomLeft(), rect().bottomRight() }) {
        const auto delta = QPointF(corner - origin);
        farthest = std::max(
            farthest,
            std::sqrt(delta.x() * delta.x() + delta.y() * delta.y()));
    }
    if (!_rippleGrow) {
        _rippleGrow = new QVariantAnimation(this);
        _rippleGrow->setDuration(kRippleShowDuration);
        _rippleGrow->setEasingCurve(QEasingCurve::OutQuint);
        connect(_rippleGrow, &QVariantAnimation::valueChanged, this,
                [this](const QVariant &value) {
            _rippleRadius = value.toReal();
            update();
        });
    }
    _rippleGrow->stop();
    _rippleGrow->setStartValue(qreal(0.));
    _rippleGrow->setEndValue(farthest);
    _rippleGrow->start();
    update();
}

void DialogsUpdateBar::stopRipple() {
    if (_rippleOpacity <= 0.) {
        return;
    }
    if (!_rippleFade) {
        _rippleFade = new QVariantAnimation(this);
        _rippleFade->setDuration(kRippleHideDuration);
        connect(_rippleFade, &QVariantAnimation::valueChanged, this,
                [this](const QVariant &value) {
            _rippleOpacity = value.toReal();
            update();
        });
        connect(_rippleFade, &QVariantAnimation::finished, this, [this] {
            _rippleRadius = 0.;
            if (_rippleGrow) {
                _rippleGrow->stop();
            }
            update();
        });
    }
    _rippleFade->stop();
    _rippleFade->setStartValue(_rippleOpacity);
    _rippleFade->setEndValue(qreal(0.));
    _rippleFade->start();
}

void DialogsUpdateBar::paintRipple(QPainter &p) {
    if (_rippleRadius <= 0. || _rippleOpacity <= 0.) {
        return;
    }
    auto hq = PainterHighQualityEnabler(p);
    p.save();
    p.setClipRect(rect());
    p.setPen(Qt::NoPen);
    p.setOpacity(_rippleOpacity);
    p.setBrush(st::shadowFg);
    p.drawEllipse(QPointF(_rippleOrigin), _rippleRadius, _rippleRadius);
    p.restore();
}

void DialogsUpdateBar::paintAction(
        QPainter &p,
        const QRect &rect,
        const QString &text,
        bool hovered) {
    if (rect.isEmpty()) {
        return;
    }
    // Square and edge-to-edge, transparent until hovered: HistoryWidget's
    // invitation DECLINE/ACCEPT bar (Ui::TextButton, radius 0, bg nullptr,
    // bgOver windowBgOver, both Expanding). Transparent here means the gradient
    // shows through, so the hover wash is translucent white rather than
    // windowBgOver — an opaque grey would flash over the gradient.
    if (hovered) {
        p.fillRect(rect, WithAlpha(st::activeButtonFg, .18));
    }
    // White for every action, not the invitation bar's red/blue split: those are
    // themed tokens meant for windowBg, and most of the 20 themes map
    // windowActiveTextFg to something dark (catalonia's is #942c1c, all but
    // indistinguishable from attentionButtonFg) which on this fixed gradient
    // loses both the distinction and what little contrast there was.
    p.setFont(ActionFont());
    p.setPen(st::activeButtonFg);
    p.drawText(rect, Qt::AlignCenter, text);
}

void DialogsUpdateBar::paintActionRowTopLine(QPainter &p) {
    const auto row = actionRowRect();
    if (row.isEmpty()) {
        return;
    }
    p.fillRect(
        QRect(row.x(), row.y(), row.width(), st::dialogsUpdateBarSeparator),
        WithAlpha(st::activeButtonFg, .35));
}

void DialogsUpdateBar::paintEvent(QPaintEvent *e) {
    auto p = QPainter(this);

    auto gradient = QLinearGradient(0, 0, width(), 0);
    gradient.setStops({
        { 0., st::groupCallLive1 },
        { 1., st::groupCallLive2 },
    });
    p.fillRect(rect(), QBrush(std::move(gradient)));
    if (_over && surfaceClickable() && _hovered == Action::None) {
        p.fillRect(rect(), WithAlpha(st::shadowFg, .3));
    }
    paintRipple(p);

    p.setRenderHint(QPainter::TextAntialiasing);
    if (_mode == Mode::Ready) {
        paintReady(p);
    } else {
        paintTwoLine(p);
    }
}

void DialogsUpdateBar::paintReady(QPainter &p) {
    const auto over = _over && _enabled && _hovered == Action::None;
    auto r = QRect(
        0,
        height() - st::dialogsUpdateButtonHeight,
        width(),
        st::dialogsUpdateButtonHeight);

    const auto font = QFont(st::semiboldFont);
    p.setFont(font);
    p.setPen(over ? st::activeButtonFgOver : st::activeButtonFg);

    const auto iconSize = _textIcon.size() / Style::DevicePixelRatio();
    if (width() >= st::columnMinimalWidthLeft) {
        r.setTop(st::dialogsUpdateButtonTextTop);
        const auto skip = st::dialogsInstallUpdateIconSkip;
        const auto textWidth = QFontMetrics(font).horizontalAdvance(_label);
        const auto rect = QRect(
            (width() - (iconSize.width() + textWidth + skip)) / 2,
            r.y(),
            textWidth,
            r.height());
        p.drawText(
            rect.translated(iconSize.width() + skip, 0),
            Qt::AlignTop | Qt::AlignHCenter,
            _label);
        p.drawImage(rect.x(), (height() - iconSize.height()) / 2, _textIcon);
    } else {
        p.drawImage(
            (width() - iconSize.width()) / 2,
            (height() - iconSize.height()) / 2,
            _textIcon);
    }
}

void DialogsUpdateBar::paintTwoLine(QPainter &p) {
    const auto pad = st::dialogsUpdateBarPaddingX;

    // Progress is the message row itself filling left to right, painted before
    // the text so the label stays on top. Indeterminate (no total yet) fills
    // nothing rather than faking a position — the real length lands within a
    // chunk or two.
    if (_mode == Mode::Downloading && _percent >= 0) {
        const auto filled = std::clamp(_percent, 0, 100) * width() / 100;
        if (filled > 0) {
            p.fillRect(
                QRect(0, 0, filled, firstRowHeight()),
                WithAlpha(st::activeButtonFg, .25));
        }
    }

    p.setFont(QFont(st::semiboldFont));
    p.setPen(st::activeButtonFg);
    p.drawText(
        QRect(pad, 0, std::max(0, width() - 2 * pad), firstRowHeight()),
        Qt::AlignCenter,
        _message);

    if (_mode == Mode::Downloading) {
        paintAction(p, primaryRect(), primaryText(), _hovered == Action::Primary);
        paintActionRowTopLine(p);
        return;
    }

    paintAction(p, secondaryRect(), secondaryText(), _hovered == Action::Secondary);
    paintAction(p, primaryRect(), primaryText(), _hovered == Action::Primary);
    paintActionRowTopLine(p);
    // Hairline between the halves: the invitation pair relies on its red/blue
    // text alone, which reads as one wide button here. White, not shadowFg —
    // 9% black is invisible against the gradient.
    const auto row = actionRowRect();
    const auto mid = row.x() + row.width() / 2;
    p.fillRect(
        QRect(mid, row.y(), st::dialogsUpdateBarSeparator, row.height()),
        WithAlpha(st::activeButtonFg, .35));
}

} // namespace TeleMatrix

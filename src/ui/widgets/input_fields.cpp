// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "input_fields.h"

#include <algorithm>
#include <QApplication>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QPainter>
#include <QPalette>
#include <QCommonStyle>

#include "ui/painter.h"

namespace Ui {

namespace {

class InputStyle final : public QCommonStyle {
public:
    InputStyle() {
        setParent(QCoreApplication::instance());
    }

    void drawPrimitive(
        PrimitiveElement,
        const QStyleOption *,
        QPainter *,
        const QWidget * = nullptr) const override {
    }

    static InputStyle *instance() {
        static InputStyle *result = nullptr;
        if (!result && QApplication::instance()) {
            result = new InputStyle();
        }
        return result;
    }
};

QColor MixColors(const QColor &from, const QColor &to, qreal progress) {
    const auto t = std::clamp(progress, 0.0, 1.0);
    return QColor::fromRgbF(
        from.redF() + (to.redF() - from.redF()) * t,
        from.greenF() + (to.greenF() - from.greenF()) * t,
        from.blueF() + (to.blueF() - from.blueF()) * t,
        from.alphaF() + (to.alphaF() - from.alphaF()) * t);
}

} // namespace

BorderedLineEdit::BorderedLineEdit(QWidget *parent)
    : QLineEdit(parent)
{
    // We paint our own border; suppress the native frame and the macOS focus
    // ring so the focused field doesn't show a second (native) border.
    setFrame(false);
    setAttribute(Qt::WA_MacShowFocusRect, false);
    setTextMargins(8, 0, 8, 0);
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Base, st::boxBg);
    pal.setColor(QPalette::Text, st::windowFg);
    pal.setColor(QPalette::Highlight, st::windowBgActive);
    setPalette(pal);
}

BorderedLineEdit::~BorderedLineEdit() = default;

void BorderedLineEdit::paintEvent(QPaintEvent *e) {
    QLineEdit::paintEvent(e);
    QPainter p(this);
    PainterHighQualityEnabler hq(p);
    p.setBrush(Qt::NoBrush);
    const auto &border = hasFocus() ? st::activeLineFg : st::inputBorderFg;
    p.setPen(border);
    const auto r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.drawRoundedRect(r, 4, 4);
}

InputField::InputField(
    QWidget *parent,
    const st::InputFieldStyle &style,
    rpl::producer<QString> placeholder)
    : QLineEdit(parent)
{
    init(style, placeholder.value);
}

InputField::InputField(QWidget *parent)
    : QLineEdit(parent)
{
    init(st::dialogsFilter, QString());
}

void InputField::init(const st::InputFieldStyle &style, const QString &placeholder) {
    _style = style;
    _textMargins = _style.textMargins;
    _placeholder = placeholder;
    _originalPlaceholder = placeholder;

    QLineEdit::setPlaceholderText(QString());
    setFont(st::normalFont);
    setAlignment(_style.textAlign);
    setFrame(false);
    setAttribute(Qt::WA_MacShowFocusRect, false);
    setStyle(InputStyle::instance());

    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    QLineEdit::setTextMargins(0, 0, 0, 0);
    applyFieldMetrics();

    QPalette palette = this->palette();
    palette.setColor(QPalette::Text, _style.textFg);
    palette.setColor(QPalette::Highlight, st::msgInBgSelected);
    palette.setColor(QPalette::HighlightedText, st::windowFgActive);
    palette.setColor(QPalette::Base, Qt::transparent);
    setPalette(palette);

    // Transparent Base (above) + setFrame(false) [in init()] give the
    // equivalent of the former "QLineEdit { background: transparent; border:
    // none; }" stylesheet, but track the theme live via QPalette instead of a
    // frozen QSS string. (QLineEdit is not a QFrame, so there is no
    // setFrameShape; setFrame(false) already removes the frame.)
    setFrame(false);

    _focusAnimation.setDuration(qMax(0, _style.duration));
    _focusAnimation.setEasingCurve(QEasingCurve::Linear);
    connect(&_focusAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        _focusedProgress = value.toReal();
        update();
    });

    // The floating caption slides + shrinks between "inside" and "lifted" on the
    // same curve tdesktop uses (easeOutCirc feel via OutCirc), keyed off focus and
    // content by updatePlaceholderShown().
    _placeholderShownAnimation.setDuration(qMax(0, _style.duration));
    _placeholderShownAnimation.setEasingCurve(QEasingCurve::OutCirc);
    connect(&_placeholderShownAnimation, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) {
        _placeholderShownProgress = value.toReal();
        update();
    });

    connect(this, &QLineEdit::textChanged, this, [this] {
        updatePlaceholderShown();
        update();
    });

    setMouseTracking(true);
    updatePlaceholderShown();
}

void InputField::refreshStyle(const st::InputFieldStyle &style) {
    _style = style;
    setFont(st::normalFont);
    setAlignment(_style.textAlign);

    _focusAnimation.setDuration(qMax(0, _style.duration));
    applyFieldMetrics();

    QPalette pal = palette();
    pal.setColor(QPalette::Text, _style.textFg);
    pal.setColor(QPalette::Highlight, st::msgInBgSelected);
    pal.setColor(QPalette::HighlightedText, st::windowFgActive);
    pal.setColor(QPalette::PlaceholderText, _style.placeholderFg);
    setPalette(pal);
    updateGeometry();
    update();
}

void InputField::setPlaceholderText(const QString &) {
    // Always keep the original placeholder set at construction ("Search").
    // Native QLineEdit placeholder stays empty — we paint our own.
    QLineEdit::setPlaceholderText(QString());
}

void InputField::startFocusAnimation(bool focused) {
    _focusAnimation.stop();
    _focusAnimation.setStartValue(_focusedProgress);
    _focusAnimation.setEndValue(focused ? 1.0 : 0.0);
    _focusAnimation.start();
}

void InputField::paintRoundSurrounding(QPainter &p) const {
    const auto divide = qMax(1, _style.borderDenominator);
    const auto border = qreal(_style.border) / qreal(divide);
    const auto borderHalf = border / 2.0;
    const auto borderColor = MixColors(_style.borderFg, _style.borderFgActive, _focusedProgress);
    const auto bgColor = MixColors(_style.textBg, _style.textBgActive, _focusedProgress);

    QPen pen(borderColor);
    pen.setWidthF(border);
    p.setPen(pen);
    p.setBrush(bgColor);

    PainterHighQualityEnabler hq(p);
    const auto radius = qreal(_style.borderRadius) - borderHalf;
    p.drawRoundedRect(
        QRectF(rect()).marginsRemoved(QMarginsF(borderHalf, borderHalf, borderHalf, borderHalf)),
        radius,
        radius);
}

namespace InputChrome {

void PaintFlatSurrounding(QPainter &p, const State &state) {
    const auto &style = *state.style;
    const auto focused = (state.focusedProgress > 0.);
    p.fillRect(
        state.rect,
        MixColors(style.textBg, style.textBgActive, state.focusedProgress));

    const auto border = focused
        ? qMax(1, style.borderActive)
        : qMax(0, style.border);
    if (border > 0) {
        p.fillRect(
            state.rect.left(),
            state.rect.bottom() + 1 - border,
            state.rect.width(),
            border,
            MixColors(style.borderFg, style.borderFgActive, state.focusedProgress));
    }
}

} // namespace InputChrome

InputChrome::State InputField::chromeState() const {
    return {
        .style = &_style,
        .rect = rect(),
        .textMargins = _textMargins,
        .font = font(),
        .placeholder = _placeholder,
        .focusedProgress = _focusedProgress,
        .placeholderShownProgress = _placeholderShownProgress,
        .placeholderAnimating
            = (_placeholderShownAnimation.state() == QAbstractAnimation::Running),
        .focused = hasFocus(),
        .empty = text().isEmpty(),
        .floating = _floatingPlaceholder,
    };
}

void InputField::paintFlatSurrounding(QPainter &p) const {
    InputChrome::PaintFlatSurrounding(p, chromeState());
}

namespace InputChrome {

void PaintPlaceholder(QPainter &p, const State &state) {
    const auto &_style = *state.style;
    const auto &_placeholder = state.placeholder;
    const auto &_textMargins = state.textMargins;
    const auto _floatingPlaceholder = state.floating;
    const auto _focusedProgress = state.focusedProgress;
    const auto _placeholderShownProgress = state.placeholderShownProgress;
    if (_placeholder.isEmpty()) {
        return;
    }
    const auto colour =
        MixColors(_style.placeholderFg, _style.placeholderFgActive, _focusedProgress);

    if (!_floatingPlaceholder) {
        // No caption: the placeholder sits inside only while empty AND unfocused,
        // so it never sits under the caret of a focused field.
        if (!state.empty || state.focused) {
            return;
        }
        const auto r = state.rect.marginsRemoved(_textMargins + _style.placeholderMargins);
        if (r.width() <= 0 || r.height() <= 0) {
            return;
        }
        p.setPen(colour);
        p.setFont(state.font);
        const auto metrics = QFontMetrics(state.font);
        p.drawText(
            r,
            _style.placeholderAlign,
            metrics.elidedText(_placeholder, Qt::ElideRight, r.width()));
        return;
    }

    // tdesktop-style floating caption: interpolate the placeholder between its
    // "inside" pose (text size, on the text line) and its "lifted" pose (small,
    // in the top strip) by the animated shown-progress. Colour follows focus.
    // At rest (not animating) snap to the correct pose from focus + content, so a
    // focused field always shows the caption LIFTED — never over the cursor.
    auto t = qBound(0., _placeholderShownProgress, 1.);
    if (!state.placeholderAnimating) {
        t = (state.focused || !state.empty) ? 1. : 0.;
    }
    const auto left = _textMargins.left() + _style.placeholderMargins.left();
    const auto availWidth = qMax(0, state.rect.width() - left - _textMargins.right());
    if (availWidth <= 0) {
        return;
    }

    const auto insideTop = _textMargins.top() + _style.placeholderMargins.top();
    const auto insideBottom = state.rect.height() - _textMargins.bottom();
    const auto liftedTop = 2;
    const auto liftedHeight = qMax(0, _textMargins.top() - 4);

    const auto bigPx = (state.font.pixelSize() > 0) ? state.font.pixelSize() : 14;
    const auto smallPx = 12;
    auto capFont = state.font;
    capFont.setPixelSize(qMax(1, qRound(bigPx + (smallPx - bigPx) * t)));

    const auto top = qRound(insideTop + (liftedTop - insideTop) * t);
    const auto insideHeight = qMax(0, insideBottom - insideTop);
    const auto h = qMax(1, qRound(insideHeight + (liftedHeight - insideHeight) * t));
    const QRect r(left, top, availWidth, h);
    p.setPen(colour);
    p.setFont(capFont);
    const auto metrics = QFontMetrics(capFont);
    p.drawText(
        r,
        Qt::AlignLeft | Qt::AlignVCenter,
        metrics.elidedText(_placeholder, Qt::ElideRight, r.width()));
}

} // namespace InputChrome

void InputField::paintPlaceholder(QPainter &p) const {
    InputChrome::PaintPlaceholder(p, chromeState());
}

void InputField::setFloatingPlaceholder(bool enabled) {
    if (_floatingPlaceholder == enabled) {
        return;
    }
    _floatingPlaceholder = enabled;
    applyFieldMetrics();
    updatePlaceholderShown();
    update();
}

void InputField::focusInEvent(QFocusEvent *e) {
    QLineEdit::focusInEvent(e);
    startFocusAnimation(true);
    updatePlaceholderShown();
}

void InputField::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape) {
        if (text().isEmpty()) {
            emit cancelled();
        } else {
            clear();
        }
        e->accept();
        return;
    }
    QLineEdit::keyPressEvent(e);
}

void InputField::focusOutEvent(QFocusEvent *e) {
    QLineEdit::focusOutEvent(e);
    startFocusAnimation(false);
    updatePlaceholderShown();
}

void InputField::updatePlaceholderShown() {
    const auto target = (_floatingPlaceholder
        && (hasFocus() || !text().isEmpty())) ? 1. : 0.;
    if (qFuzzyCompare(target, _placeholderShownProgress)
        && _placeholderShownAnimation.state() != QAbstractAnimation::Running) {
        return;
    }
    _placeholderShownAnimation.stop();
    _placeholderShownAnimation.setStartValue(_placeholderShownProgress);
    _placeholderShownAnimation.setEndValue(target);
    _placeholderShownAnimation.start();
}

void InputField::setCancelVisible(bool visible) {
    if (_cancelVisible == visible) {
        return;
    }
    _cancelVisible = visible;
    applyFieldMetrics();
    update();
}

void InputField::applyFieldMetrics() {
    // A flat caption field (borderRadius 0) reserves a top strip so a focused or
    // filled placeholder can float up as a caption. With no caption (floating
    // off — single-field forms) that strip is dropped and the field shrinks.
    // Round filter fields (dialogsFilter, borderRadius > 0) keep their metrics.
    const auto compact = (_style.borderRadius == 0) && !_floatingPlaceholder;
    const auto minH = compact ? 38 : qMax(1, _style.heightMin);
    const auto maxH = compact ? 38 : qMax(minH, _style.heightMax);
    setMinimumHeight(minH);
    setMaximumHeight(maxH);
    auto margins = _style.textMargins;
    if (compact) {
        margins.setTop(qMin(margins.top(), 8));
    }
    if (_cancelVisible) {
        margins.setRight(_style.cancelButtonSize);
    }
    applyTextMargins(margins);
}

void InputField::applyTextMargins(const QMargins &margins) {
    _textMargins = margins;
    QLineEdit::setTextMargins(_textMargins);
    setContentsMargins(0, 0, 0, 0);
}

QRect InputField::cancelButtonRect() const {
    const auto sz = _style.cancelButtonSize;
    return QRect(width() - sz, (height() - sz) / 2, sz, sz);
}

void InputField::paintCancelButton(QPainter &p) const {
    if (!_cancelVisible) {
        return;
    }
    const auto r = cancelButtonRect();
    const auto cx = r.center().x();
    const auto cy = r.center().y();
    const auto arm = qMax(4.0, (_style.cancelButtonSize / 2.0) - 12.0);

    const auto color = _cancelHovered
        ? st::dialogsCancelSearchCrossFgOver
        : st::dialogsCancelSearchCrossFg;

    QPen pen(color);
    pen.setWidthF(1.5);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    PainterHighQualityEnabler hq(p);
    p.drawLine(QPointF(cx - arm, cy - arm), QPointF(cx + arm, cy + arm));
    p.drawLine(QPointF(cx + arm, cy - arm), QPointF(cx - arm, cy + arm));
}

void InputField::updateCancelCursor(const QPoint &pos) {
    if (!_cancelVisible) {
        if (_cancelHovered) {
            _cancelHovered = false;
            setCursor(Qt::IBeamCursor);
            update();
        }
        return;
    }
    const auto over = cancelButtonRect().contains(pos);
    if (over != _cancelHovered) {
        _cancelHovered = over;
        setCursor(over ? Qt::PointingHandCursor : Qt::IBeamCursor);
        update();
    }
}

void InputField::mousePressEvent(QMouseEvent *e) {
    if (_cancelVisible && e->button() == Qt::LeftButton) {
        if (cancelButtonRect().contains(e->pos())) {
            clear();
            emit cancelled();
            return;
        }
    }
    QLineEdit::mousePressEvent(e);
}

void InputField::mouseMoveEvent(QMouseEvent *e) {
    updateCancelCursor(e->pos());
    QLineEdit::mouseMoveEvent(e);
}

void InputField::paintEvent(QPaintEvent *e) {
    {
        QPainter p(this);
        if (_style.borderRadius > 0) {
            paintRoundSurrounding(p);
        } else {
            paintFlatSurrounding(p);
        }
        paintPlaceholder(p);
        paintCancelButton(p);
    }

    QLineEdit::paintEvent(e);
}

InputField::~InputField() = default;

} // namespace Ui

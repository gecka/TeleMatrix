// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "intro/intro_widgets.h"

#include "intro/intro_colors.h"
#include "styles/style_constants.h"
#include "ui/painter.h"

#include <QEvent>
#include <QFontMetrics>
#include <QPainter>
#include <QPalette>

namespace intro {
namespace {

constexpr int kRadioOuter = 17;
constexpr int kRadioInner = 9;
constexpr qreal kRadioBorderWidth = 1.5;
constexpr int kRadioTextGap = 13;
constexpr int kCardTitleToDesc = 3;

[[nodiscard]] QFont cardTitleFont() {
    auto font = st::baseFont(metrics::fieldSize);
    font.setWeight(QFont::DemiBold);
    return font;
}

/// Round-rect stroked from the centre of the pen, so a 2px border does not
/// bleed outside the widget the way a naive rect() would.
void strokeRounded(
        QPainter &p,
        const QRect &rect,
        qreal radius,
        const QColor &border,
        qreal penWidth) {
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(border, penWidth));
    const auto inset = penWidth / 2.0;
    p.drawRoundedRect(
        QRectF(rect).adjusted(inset, inset, -inset, -inset), radius, radius);
}

} // namespace

[[nodiscard]] QFont headingFont() {
    auto font = st::baseFont(metrics::headingSize);
    font.setWeight(QFont::Bold);
    font.setLetterSpacing(QFont::PercentageSpacing, 98.0); // -.02em
    return font;
}

[[nodiscard]] QFont actionFont() {
    auto font = st::baseFont(metrics::fieldSize);
    font.setWeight(QFont::DemiBold);
    return font;
}

// --- Field ---------------------------------------------------------------

Field::Field(QWidget *parent) : QLineEdit(parent) {
    setFrame(false);
    // The rounded border is painted below; macOS would otherwise draw a second,
    // offset focus halo around it.
    setAttribute(Qt::WA_MacShowFocusRect, false);
    setFixedHeight(metrics::fieldHeight);
    setFont(st::baseFont(metrics::fieldSize));
    // Left/right padding per the spec. The vertical margins stay 0 so the text
    // keeps its own centring inside the fixed height.
    setTextMargins(metrics::fieldPaddingH - 2, 0, metrics::fieldPaddingH - 2, 0);
    applyPalette();
}

void Field::paintEvent(QPaintEvent *event) {
    {
        // The translucent fill has to go UNDER the text, and QLineEdit paints
        // its own background from the palette — which cannot express alpha over
        // a gradient, since the palette brush is composited against the parent's
        // opaque backing. So fill here and leave the palette base transparent.
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(isEnabled() ? intro::surfaceFill : intro::inputDisabledBg);
        p.drawRoundedRect(
            QRectF(rect()), metrics::fieldRadius, metrics::fieldRadius);
    }
    QLineEdit::paintEvent(event);
    QPainter p(this);
    PainterHighQualityEnabler hq(p);
    // Focus changes the border COLOUR only — the width stays 1px, as in the
    // design's markup. Thickening it to 2px made the focused field visibly
    // heavier than its neighbours and nudged its content.
    strokeRounded(
        p,
        rect(),
        metrics::fieldRadius,
        hasFocus() ? intro::accentFill : intro::surfaceBorder,
        1.0);
}

void Field::changeEvent(QEvent *e) {
    QLineEdit::changeEvent(e);
    if (e->type() == QEvent::EnabledChange) {
        applyPalette();
        update();
    }
}

void Field::applyPalette() {
    QPalette pal = palette();
    // Transparent base: the fill is painted in paintEvent so it can be
    // translucent over the wash.
    pal.setColor(QPalette::Base, Qt::transparent);
    pal.setColor(
        QPalette::Text, isEnabled() ? intro::inkField : intro::inputDisabledFg);
    pal.setColor(QPalette::PlaceholderText, intro::mutedFg);
    pal.setColor(QPalette::Highlight, intro::accentFill);
    pal.setColor(QPalette::HighlightedText, Qt::white);
    setPalette(pal);
}

// --- FilledButton --------------------------------------------------------

FilledButton::FilledButton(const QString &text, QWidget *parent)
    : QPushButton(text, parent) {
    setFixedHeight(metrics::buttonHeight);
    setFont(actionFont());
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
}

void FilledButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    PainterHighQualityEnabler hq(p);
    p.setPen(Qt::NoPen);
    const auto fill = !isEnabled()
        ? intro::buttonDisabledBg
        : (_hovered ? intro::accentFillOver : intro::accentFill);
    p.setBrush(fill);
    p.drawRoundedRect(
        QRectF(rect()), metrics::buttonRadius, metrics::buttonRadius);
    p.setPen(intro::buttonFg);
    p.setFont(font());
    p.drawText(rect(), Qt::AlignCenter, text());
}

void FilledButton::enterEvent(QEnterEvent *) { _hovered = true; update(); }
void FilledButton::leaveEvent(QEvent *) { _hovered = false; update(); }

// --- GhostButton ---------------------------------------------------------

GhostButton::GhostButton(const QString &text, QWidget *parent)
    : QPushButton(text, parent) {
    setFixedHeight(metrics::buttonHeight);
    setFont(actionFont());
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
}

void GhostButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    PainterHighQualityEnabler hq(p);
    p.setPen(Qt::NoPen);
    p.setBrush(_hovered ? intro::ghostBgOver : intro::surfaceFill);
    p.drawRoundedRect(
        QRectF(rect()), metrics::buttonRadius, metrics::buttonRadius);
    strokeRounded(p, rect(), metrics::buttonRadius, intro::ghostBorder, 1.0);
    p.setPen(intro::ghostFg);
    p.setFont(font());
    p.drawText(rect(), Qt::AlignCenter, text());
}

void GhostButton::enterEvent(QEnterEvent *) { _hovered = true; update(); }
void GhostButton::leaveEvent(QEvent *) { _hovered = false; update(); }

// --- LinkButton ----------------------------------------------------------

LinkButton::LinkButton(const QString &text, QWidget *parent, bool muted)
    : QPushButton(text, parent)
    , _muted(muted) {
    setFont(st::baseFont(metrics::smallSize));
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
    setFlat(true);
}

void LinkButton::setMuted(bool muted) {
    if (_muted == muted) {
        return;
    }
    _muted = muted;
    update();
}

QSize LinkButton::sizeHint() const {
    const QFontMetrics fm(font());
    return QSize(fm.horizontalAdvance(text()) + 16, fm.height() + 8);
}

void LinkButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    PainterHighQualityEnabler hq(p);
    auto colour = _muted ? intro::mutedFg : intro::accentText;
    if (!isEnabled()) {
        colour = intro::inputDisabledFg;
    } else if (_hovered && !_muted) {
        colour = intro::accentFillOver;
    }
    p.setPen(colour);
    p.setFont(font());
    p.drawText(rect(), Qt::AlignCenter, text());
}

void LinkButton::enterEvent(QEnterEvent *) { _hovered = true; update(); }
void LinkButton::leaveEvent(QEvent *) { _hovered = false; update(); }

// --- OptionCard ----------------------------------------------------------

OptionCard::OptionCard(
        const QString &title,
        const QString &description,
        bool selectable,
        QWidget *parent)
    : QPushButton(parent)
    , _title(title)
    , _description(description)
    , _selectable(selectable) {
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
    setFlat(true);
}

void OptionCard::setSelected(bool selected) {
    if (_selected == selected) {
        return;
    }
    _selected = selected;
    update();
}

void OptionCard::setDescription(const QString &description) {
    if (_description == description) {
        return;
    }
    _description = description;
    updateGeometry();
    update();
}

int OptionCard::textLeft() const {
    return metrics::cardPaddingH
        + (_selectable ? kRadioOuter + kRadioTextGap : 0);
}

int OptionCard::heightForWidth(int width) const {
    const QFontMetrics titleMetrics(cardTitleFont());
    const QFontMetrics descMetrics(st::baseFont(metrics::smallSize));
    const auto textWidth = qMax(
        1, width - textLeft() - metrics::cardPaddingH);
    const auto descHeight = _description.isEmpty()
        ? 0
        : descMetrics.boundingRect(
              QRect(0, 0, textWidth, 0),
              Qt::TextWordWrap,
              _description).height() + kCardTitleToDesc;
    // The selectable card is a touch taller: the design pads it 15px against
    // the method card's 14px.
    const auto padding = _selectable ? metrics::cardPaddingV : 14;
    return padding * 2 + titleMetrics.height() + descHeight;
}

void OptionCard::paintEvent(QPaintEvent *) {
    QPainter p(this);
    PainterHighQualityEnabler hq(p);

    p.setPen(Qt::NoPen);
    p.setBrush(intro::surfaceFill);
    p.drawRoundedRect(QRectF(rect()), metrics::cardRadius, metrics::cardRadius);

    // Exactly ONE outline is ever drawn. Painting the 1px border and then the
    // 2px selection ring on top left the two strokes at different geometry, and
    // the pale border showed through unevenly along the curves — which reads as
    // a ring whose thickness changes around the corners.
    const auto selected = _selectable && _selected;
    const auto hoverBorder = !_selectable && _hovered;
    strokeRounded(
        p,
        rect(),
        metrics::cardRadius,
        selected ? intro::accentFill
                 : (hoverBorder ? intro::accentFill : intro::surfaceBorder),
        selected ? 2.0 : 1.0);

    const auto padding = _selectable ? metrics::cardPaddingV : 14;
    if (_selectable) {
        const QRectF outer(
            metrics::cardPaddingH,
            padding + 2,
            kRadioOuter,
            kRadioOuter);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(
            _selected ? intro::accentFill : intro::radioBorder,
            kRadioBorderWidth));
        p.drawEllipse(outer);
        if (_selected) {
            p.setPen(Qt::NoPen);
            p.setBrush(intro::accentFill);
            const auto inset = (kRadioOuter - kRadioInner) / 2.0;
            p.drawEllipse(outer.adjusted(inset, inset, -inset, -inset));
        }
    }

    const auto left = textLeft();
    const auto textWidth = qMax(1, width() - left - metrics::cardPaddingH);

    const auto titleF = cardTitleFont();
    const QFontMetrics titleMetrics(titleF);
    p.setFont(titleF);
    p.setPen(intro::inkHeading);
    p.drawText(
        QRect(left, padding, textWidth, titleMetrics.height()),
        Qt::AlignLeft | Qt::AlignVCenter,
        _title);

    if (!_description.isEmpty()) {
        const auto descF = st::baseFont(metrics::smallSize);
        p.setFont(descF);
        p.setPen(intro::mutedFg);
        const auto top = padding + titleMetrics.height() + kCardTitleToDesc;
        p.drawText(
            QRect(left, top, textWidth, height() - top - padding),
            Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop,
            _description);
    }
}

void OptionCard::enterEvent(QEnterEvent *) { _hovered = true; update(); }
void OptionCard::leaveEvent(QEvent *) { _hovered = false; update(); }

} // namespace intro

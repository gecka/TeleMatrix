// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "intro_step.h"
#include "intro_widget.h"
#include "../styles/style_constants.h"
#include "intro_colors.h"
#include "intro_widgets.h"

#include "ui/painter.h"

#include <QFont>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>
#include <QPushButton>
#include <QLinearGradient>
#include <QResizeEvent>

namespace TeleMatrix {

namespace {

// Flat text button (transparent background) whose only decoration is a
// text-color change on hover / disabled. Used for the back arrow.
class IntroFlatButton : public QPushButton {
public:
    IntroFlatButton(
        const QColor *fg,
        const QColor *fgOver,
        const QColor *disabledFg,
        QWidget *parent)
        : QPushButton(parent)
        , _fg(fg)
        , _fgOver(fgOver)
        , _disabledFg(disabledFg) {
        setMouseTracking(true);
        // Suppress the native macOS focus ring at
        // construction so the back arrow never shows a stray halo on focus.
        setAttribute(Qt::WA_MacShowFocusRect, false);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        const QColor *text = _fg;
        if (!isEnabled() && _disabledFg) {
            text = _disabledFg;
        } else if (_hovered && _fgOver) {
            text = _fgOver;
        }
        if (text) {
            p.setPen(*text);
            p.setFont(font());
            p.drawText(rect(), Qt::AlignCenter, this->text());
        }
    }
    void enterEvent(QEnterEvent *) override { _hovered = true; update(); }
    void leaveEvent(QEvent *) override { _hovered = false; update(); }

private:
    const QColor *_fg = nullptr;
    const QColor *_fgOver = nullptr;
    const QColor *_disabledFg = nullptr;
    bool _hovered = false;
};

} // namespace

IntroStep::IntroStep(IntroWidget *parent, bool hasCover)
    : QWidget(parent)
    , _introWidget(parent)
    , _hasCover(hasCover)
{
    // Title label — styled differently for cover vs non-cover in updateLayout.
    _title = new QLabel(this);

    // Description label.
    _description = new QLabel(this);
    _description->setWordWrap(true);

    // Error label: red text, initially hidden.
    _error = new QLabel(this);
    _error->setWordWrap(true);
    {
        auto errorFont = st::baseFont(13);
        _error->setFont(errorFont);
    }
    {
        QPalette pal = _error->palette();
        pal.setColor(QPalette::WindowText, intro::attentionFg);
        _error->setPalette(pal);
    }
    _error->hide();

    // The shared primary action, in the redesign's filled style. Every step
    // inherits it, so styling it here restyles the main button everywhere.
    _next = new intro::FilledButton(QString(), this);
    _next->setFixedSize(st::introNextButtonWidth, st::introNextButtonHeight);

    connect(_next, &QPushButton::clicked, this, &IntroStep::submit);

    // Apply fonts and colors based on cover mode.
    if (_hasCover) {
        auto titleFont = st::baseFont(22, true);
        _title->setFont(titleFont);
        _title->setAlignment(Qt::AlignHCenter);
        // Title color is set via painting (white on gradient), not stylesheet.

        auto descFont = st::baseFont(15);
        _description->setFont(descFont);
        _description->setAlignment(Qt::AlignHCenter);
    } else {
        // The redesign's h2: 23px/700 with -.02em. This was 17px, which read as
        // a form label rather than a screen heading.
        _title->setFont(intro::headingFont());
        _title->setAlignment(Qt::AlignHCenter);
        {
            QPalette pal = _title->palette();
            pal.setColor(QPalette::WindowText, intro::inkHeading);
            _title->setPalette(pal);
        }

        _description->setFont(st::baseFont(intro::metrics::bodySize));
        _description->setAlignment(Qt::AlignHCenter);
        {
            QPalette pal = _description->palette();
            pal.setColor(QPalette::WindowText, intro::mutedFg);
            _description->setPalette(pal);
        }
    }

    // Center error label text.
    _error->setAlignment(Qt::AlignHCenter);
}

void IntroStep::activate() {
    _next->setText(nextButtonText());
    // The button is shared across steps; a step that gates it (login's
    // all-fields-filled rule) must not leave the next step with a dead button.
    _next->setEnabled(true);
}

void IntroStep::setTitleText(const QString &text) {
    _title->setText(text);
    _title->adjustSize();
}

void IntroStep::setDescriptionText(const QString &text) {
    _description->setText(text);
    // An empty description means the screen has no subtitle at all (the
    // redesign drops them from sign in / create / reset). Leaving the label
    // visible-but-empty would reserve a line's worth of space and, worse, feed
    // that phantom height into centerContentVertically().
    _description->setVisible(!_hasCover && !text.isEmpty());
}

void IntroStep::showError(const QString &text) {
    // No relayout: the slot is reserved permanently, so the message appears
    // without anything around it moving.
    _error->setText(text);
    _error->show();
}

void IntroStep::hideError() {
    _error->clear();
    _error->hide();
}

void IntroStep::relayout() {
    updateLayout();
}

QString IntroStep::friendlyError(
        const QString &raw,
        const QString &homeserver) const {
    if (raw.isEmpty()) {
        return QString();
    }
    // Transport failures arrive as the HTTP client's own text, which embeds the
    // full request URL ("error sending request for url (https://…/requestToken)").
    // That is debugging output, not something to put in front of someone who
    // mistyped a server name.
    if (raw.contains(QStringLiteral("Network error"))
        || raw.contains(QStringLiteral("error sending request"))
        || raw.contains(QStringLiteral("dns error"))
        || raw.contains(QStringLiteral("Connection refused"))) {
        return homeserver.isEmpty()
            ? tr("Couldn't reach the server. Check your internet connection.")
            : tr("Couldn't reach %1. Check the address and your internet "
                 "connection.").arg(homeserver);
    }
    if (raw.contains(QStringLiteral("timed out"))
        || raw.contains(QStringLiteral("timeout"))) {
        return homeserver.isEmpty()
            ? tr("The server took too long to respond. Please try again.")
            : tr("%1 took too long to respond. Please try again.")
                  .arg(homeserver);
    }
    // A Matrix errcode: the server's own sentence follows it, and that is the
    // useful half.
    if (raw.startsWith(QStringLiteral("M_"))) {
        const auto text = raw.section(QStringLiteral(": "), 1);
        if (!text.isEmpty()) {
            return text;
        }
    }
    // Anything still carrying a URL or a Rust type name is an internal string
    // that escaped; say something plain rather than leak it.
    if (raw.contains(QStringLiteral("://"))
        || raw.contains(QStringLiteral("Error("))) {
        return tr("Something went wrong. Please try again.");
    }
    return raw;
}

int IntroStep::errorSlotHeight() const {
    // A FIXED two-line slot, independent of whether an error is showing or how
    // long it is. Sizing this to the current message is what made forms jump on
    // submit: the gap grew when an error appeared and shrank when it cleared.
    // Two lines because the longest messages here ("This doesn't look like a
    // Matrix server. Check the address and try again") wrap to two at the form
    // width.
    return QFontMetrics(_error->font()).height() * 2;
}

void IntroStep::placeErrorAbove(int firstControlY) {
    // Bottom-aligned inside the reserved slot, so a one-line message sits just
    // above the field and a two-line one grows upward into space that was
    // already there.
    const auto height = errorSlotHeight();
    _error->setFixedWidth(st::introStepWidth);
    _error->setAlignment(Qt::AlignHCenter | Qt::AlignBottom);
    _error->setGeometry(
        contentLeft(),
        firstControlY - height - st::introFieldSpacing,
        st::introStepWidth,
        height);
}

QPushButton *IntroStep::addBackButton() {
    auto *back = new IntroFlatButton(
        &intro::subtextFg,   // normal
        &intro::titleFg,     // hover
        nullptr,             // no disabled styling
        this);
    _back = back;
    _back->setFixedSize(st::introBackButtonSize, st::introBackButtonSize);
    _back->setCursor(Qt::PointingHandCursor);
    _back->setText(QStringLiteral("\u2190")); // Unicode left arrow ←
    // The original stylesheet used font-size:22px for the arrow glyph.
    {
        auto f = back->font();
        f.setPixelSize(22);
        back->setFont(f);
    }
    _back->move(0, 0);
    connect(_back, &QPushButton::clicked, this, &IntroStep::goBack);
    return _back;
}

int IntroStep::contentLeft() const {
    return (width() - st::introStepWidth) / 2;
}

int IntroStep::contentTop() const {
    return qMax(st::introStepTopMin, (height() - st::introHeight) / 2);
}

int IntroStep::contentStartTop() const {
    // Where a step's own controls should begin: just under the heading (and the
    // subtitle when there is one), rather than at the fixed introStepFieldTop.
    // That constant assumed a 17px heading and left ~60px of dead air under the
    // redesign's 23px one — space the taller forms need in order to fit above
    // the stage furniture.
    auto y = contentTop() + st::introTitleTop
        + QFontMetrics(_title->font()).height();
    if (_description->isVisible() && !_description->text().isEmpty()) {
        const auto height =
            _description->heightForWidth(st::introStepWidth);
        y += st::introHeadingGap
            + (height > 0 ? height : _description->sizeHint().height());
    }
    // The error slot is ALWAYS reserved, showing or not, so that submitting a
    // form never moves it.
    return y + st::introHeadingToFields
        + errorSlotHeight() + st::introFieldSpacing;
}

int IntroStep::bottomReservedHeight() const {
    // How much of the window the stage furniture owns, measured from the
    // bottom. The keys line sits above the version line, so it decides when
    // shown. The margin keeps content from butting right up against it.
    constexpr int kFurnitureMargin = 16;
    const QFontMetrics fm(st::baseFont(intro::metrics::smallSize));
    if (_showsKeysLine) {
        return st::introKeysLineBottom + fm.height() + kFurnitureMargin;
    }
    if (_showsVersion) {
        return st::introVersionBottom + fm.height() + kFurnitureMargin;
    }
    return 0;
}

void IntroStep::placeHeadings() {
    if (_hasCover || _managesHeadings) {
        return;
    }
    const auto left = contentLeft();
    const auto top = contentTop();
    _title->setVisible(true);
    _description->setVisible(!_description->text().isEmpty());
    _title->move(left, top + st::introTitleTop);
    _title->setFixedWidth(st::introStepWidth);
    // Derived from the heading's real height rather than the old fixed
    // introDescriptionTop, which was tuned for a 17px title and left only a few
    // pixels once the heading grew to the redesign's 23px.
    const auto titleHeight = QFontMetrics(_title->font()).height();
    _description->move(
        left, top + st::introTitleTop + titleHeight + st::introHeadingGap);
    _description->setFixedWidth(st::introStepWidth);
}

void IntroStep::centerContentVertically() {
    // Put the heading and subtitle back at their absolute positions first.
    //
    // This function works by measuring the content block and shifting every
    // child by a delta. That is only correct if everything it measures is at a
    // known, absolute position — and the title/description are placed by
    // updateLayout(), which a step's own re-layout does NOT re-run. Without
    // this, a partial re-layout (toggling Show/Hide, showing an error) measured
    // the heading where the PREVIOUS shift had left it, computed a fresh delta
    // from the wrong baseline, and moved the whole form again. Repeated presses
    // walked it off the top of the window.
    placeHeadings();

    // Measure the content block: the union of every visible direct child except
    // the back arrow (which is a fixed top-left nav control, not content).
    const auto children = findChildren<QWidget *>(Qt::FindDirectChildrenOnly);
    bool any = false;
    int minTop = 0;
    int maxBottom = 0;
    for (auto *w : children) {
        if (w == _back || w->isHidden()) {
            continue;
        }
        const int t = w->y();
        const int b = w->y() + w->height();
        if (!any) {
            minTop = t;
            maxBottom = b;
            any = true;
        } else {
            minTop = qMin(minTop, t);
            maxBottom = qMax(maxBottom, b);
        }
    }
    if (!any) {
        return;
    }

    // Centre within the band ABOVE the stage furniture, not the whole window.
    // The keys and version lines are painted at fixed offsets from the bottom
    // and are not children, so centring against the full height let a tall form
    // (create-account, once every field gained a caption) run straight over
    // them.
    const int contentHeight = maxBottom - minTop;
    const int available = qMax(0, height() - bottomReservedHeight());
    const int desiredTop = qMax(st::introStepTopMin, (available - contentHeight) / 2);
    const int delta = desiredTop - minTop;
    if (delta == 0) {
        return;
    }
    for (auto *w : children) {
        if (w == _back || w->isHidden()) {
            continue;
        }
        w->move(w->x(), w->y() + delta);
    }
}

void IntroStep::paintEvent(QPaintEvent *e) {
    QPainter p(this);

    // The stage is a vertical warm-paper wash and NOTHING else — no shapes, no
    // pattern. In the Add-Account popup the three stops collapse to one themed
    // colour, so this same code paints a flat background there.
    QLinearGradient wash(0, 0, 0, height());
    wash.setColorAt(0.0, intro::washTop);
    wash.setColorAt(0.52, intro::washMid);
    wash.setColorAt(1.0, intro::washBottom);
    p.fillRect(rect(), wash);

    paintStageFurniture(p);

    if (_hasCover) {
        paintCover(p);
    }
}

void IntroStep::paintStageFurniture(QPainter &p) {
    PainterHighQualityEnabler hq(p);
    const auto small = st::baseFont(intro::metrics::smallSize);
    const QFontMetrics fm(small);
    p.setFont(small);

    // "Keys: <choice>  Change" — absolutely positioned stage furniture, not part
    // of any screen's own layout, so it does not participate in the vertical
    // centring above it.
    if (_showsKeysLine) {
        const auto label = tr("Keys: %1").arg(_keysLabel);
        const auto action = tr("Change");
        // "Change" is drawn DemiBold, so it must be measured DemiBold too —
        // measuring it with the regular-weight metrics makes the rect narrower
        // than the glyphs and Qt clips the tail.
        auto actionFont = small;
        actionFont.setWeight(QFont::DemiBold);
        const QFontMetrics actionMetrics(actionFont);

        const auto labelWidth = fm.horizontalAdvance(label);
        const auto actionWidth = actionMetrics.horizontalAdvance(action);
        const auto gap = 8;
        const auto totalWidth = labelWidth + gap + actionWidth;
        const auto left = (width() - totalWidth) / 2;
        const auto lineHeight = qMax(fm.height(), actionMetrics.height());
        const auto top = height() - st::introKeysLineBottom - lineHeight;

        p.setPen(intro::mutedFg);
        p.drawText(QRect(left, top, labelWidth, lineHeight),
            Qt::AlignLeft | Qt::AlignVCenter, label);

        p.setFont(actionFont);
        p.setPen(intro::accentText);
        _keysChangeRect = QRect(
            left + labelWidth + gap, top, actionWidth, lineHeight);
        p.drawText(_keysChangeRect, Qt::AlignLeft | Qt::AlignVCenter, action);
        p.setFont(small);
    } else {
        _keysChangeRect = QRect();
    }

    if (_showsVersion) {
        p.setPen(intro::mutedFg);
        const auto version = tr("Version %1")
            .arg(QStringLiteral(TELEMATRIX_VERSION_STR));
        p.drawText(
            QRect(0, height() - st::introVersionBottom - fm.height(),
                  width(), fm.height()),
            Qt::AlignHCenter | Qt::AlignVCenter,
            version);
    }
}

void IntroStep::setShowsKeysLine(bool shows) {
    if (_showsKeysLine == shows) {
        return;
    }
    _showsKeysLine = shows;
    setMouseTracking(shows || hasMouseTracking());
    update();
}

void IntroStep::setShowsVersion(bool shows) {
    if (_showsVersion == shows) {
        return;
    }
    _showsVersion = shows;
    update();
}

void IntroStep::setKeysLabel(const QString &label) {
    if (_keysLabel == label) {
        return;
    }
    _keysLabel = label;
    update();
}

void IntroStep::mousePressEvent(QMouseEvent *e) {
    if (_showsKeysLine
        && e->button() == Qt::LeftButton
        && _keysChangeRect.contains(e->pos())) {
        Q_EMIT changeKeyStorage();
        return;
    }
    QWidget::mousePressEvent(e);
}

void IntroStep::mouseMoveEvent(QMouseEvent *e) {
    if (_showsKeysLine) {
        setCursor(_keysChangeRect.contains(e->pos())
            ? Qt::PointingHandCursor
            : Qt::ArrowCursor);
    }
    QWidget::mouseMoveEvent(e);
}

void IntroStep::paintCover(QPainter &p) {
    const auto coverWidth = width();
    const auto coverHeight = st::introCoverHeight;

    // Blue gradient from top to bottom of cover zone.
    QLinearGradient gradient(0, 0, 0, coverHeight);
    gradient.setColorAt(0.0, intro::coverTopBg);
    gradient.setColorAt(1.0, intro::coverBottomBg);
    p.fillRect(0, 0, coverWidth, coverHeight, gradient);

    // Title centered on the gradient.
    const auto titleY = st::introCoverTitleTop;
    p.setFont(st::baseFont(22, true));
    p.setPen(intro::coverTitleFg);
    p.drawText(QRect(0, titleY, coverWidth, 30),
               Qt::AlignHCenter | Qt::AlignTop,
               _title->text());

    // Description centered below title, with 70% white opacity.
    const auto descY = st::introCoverDescriptionTop;
    p.setFont(st::baseFont(15));
    p.setPen(intro::coverDescFg);
    p.drawText(QRect(0, descY, coverWidth, 24),
               Qt::AlignHCenter | Qt::AlignTop,
               _description->text());
}

void IntroStep::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    updateLayout();
}

void IntroStep::updateLayout() {
    const auto left = contentLeft();
    const auto top = contentTop();

    if (_hasCover) {
        // In cover mode, title and description are painted directly on the
        // gradient by paintCover(), so hide the QLabel widgets.
        _title->setVisible(false);
        _description->setVisible(false);

        // Next button is in the content zone below the cover.
        const auto nextLeft = (width() - st::introNextButtonWidth) / 2;
        _next->move(nextLeft, top + st::introNextTop);

        // Error below button area.
        _error->move(left, top + st::introErrorTop);
        _error->setFixedWidth(st::introStepWidth);
    } else {
        // No cover — title and description are visible QLabel widgets.
        placeHeadings();

        // Next button centered.
        const auto nextLeft = (width() - st::introNextButtonWidth) / 2;
        _next->move(nextLeft, top + st::introNextTop);

        // Error between fields and button.
        _error->move(left, top + st::introErrorTop);
        _error->setFixedWidth(st::introStepWidth);
    }
}

} // namespace TeleMatrix

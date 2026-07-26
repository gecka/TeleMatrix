// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_confirm_dialog.h"

#include <QEventLoop>
#include <QFont>
#include <QFontMetrics>
#include <QGraphicsOpacityEffect>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include <limits>

#include "styles/style_constants.h"
#include "ui/focus_restore.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"

namespace TeleMatrix {

namespace {

void paintBoxShadow(QPainter &p, const QRect &boxRect) {
    // Approximate a pre-rendered 9-patch box shadow.
    PainterHighQualityEnabler hq(p);
    p.setPen(Qt::NoPen);
    const auto extend = qMax(1, st::layerShadowExtend);
    for (int i = extend; i >= 1; --i) {
        const auto progress = qreal(extend - i) / extend;
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

// Buttons auto-size to textWidth + 30px (defaultBoxButton min width).
// Button height = 34px, buttonRadius = 4px.
int buttonAutoWidth(const QString &text, const QFont &font) {
    const QFontMetrics fm(font);
    return fm.horizontalAdvance(text) + st::boxButtonMinWidth;
}

// Button styles built from POINTERS to st:: globals so the TextButton repaints
// with the current theme's colors (a QSS built from .name() would freeze them).
::Ui::TextButton::Style normalButtonStyle() {
    ::Ui::TextButton::Style s;
    s.bg = &st::lightButtonBg;
    s.bgOver = &st::lightButtonBgOver;
    s.fg = &st::lightButtonFg;
    s.radius = st::buttonRadius;
    return s;
}

::Ui::TextButton::Style attentionButtonStyle() {
    ::Ui::TextButton::Style s;
    s.bg = &st::lightButtonBg;
    s.bgOver = &st::attentionButtonBgOver;
    s.fg = &st::attentionButtonFg;
    s.radius = st::buttonRadius;
    return s;
}

::Ui::TextButton::Style filledAttentionButtonStyle() {
    ::Ui::TextButton::Style s;
    s.bg = &st::attentionButtonBgOver;
    s.bgOver = &st::attentionButtonBgRipple;
    s.fg = &st::attentionButtonFg;
    s.radius = st::buttonRadius;
    return s;
}

} // namespace

HistoryConfirmDialog::HistoryConfirmDialog(
    QWidget *parent,
    const QString &title,
    const QString &text,
    const QString &confirmText,
    const QString &cancelText,
    ConfirmStyle confirmStyle,
    int customWidth,
    int customButtonBottomPadding,
    bool showCancel,
    bool richText)
    : QWidget(parent ? parent->window() : nullptr) {
    // The layer is a CHILD widget of the main window body, not a separate
    // QDialog/window. It covers the entire parent via
    // setGeometry(parentWidget()->rect()) and paints the dark overlay
    // + box shadow + centered panel content.
    if (parentWidget()) {
        setGeometry(parentWidget()->rect());
        parentWidget()->installEventFilter(this);
    }

    setFocusPolicy(Qt::StrongFocus);

    // Two show animations, both 200ms:
    //   _a_shown: background overlay, easeOutCirc
    //   _a_layerShown: box content + shadow, linear
    _bgOpacity = 0.0;
    _layerOpacity = 0.0;

    _a_shown = new QVariantAnimation(this);
    _a_shown->setDuration(200);
    _a_shown->setEasingCurve(QEasingCurve::OutCirc);
    _a_shown->setStartValue(0.0);
    _a_shown->setEndValue(1.0);
    connect(_a_shown, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        _bgOpacity = value.toReal();
        update();
    });

    _a_layerShown = new QVariantAnimation(this);
    _a_layerShown->setDuration(200);
    _a_layerShown->setEasingCurve(QEasingCurve::Linear);
    _a_layerShown->setStartValue(0.0);
    _a_layerShown->setEndValue(1.0);
    connect(_a_layerShown, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        _layerOpacity = value.toReal();
        if (_panelEffect) {
            _panelEffect->setOpacity(_layerOpacity);
        }
        update();
    });

    // Centering layout.
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addStretch(1);

    _panel = new RoundedPanel(this);
    _panelEffect = new QGraphicsOpacityEffect(_panel);
    _panelEffect->setOpacity(0.0);
    _panel->setGraphicsEffect(_panelEffect);
    const auto boxWidth = customWidth > 0 ? customWidth : st::boxWidth;
    auto btnPad = st::boxButtonPadding;
    if (customButtonBottomPadding >= 0) {
        btnPad.setBottom(customButtonBottomPadding);
    }

    _panel->setFixedWidth(boxWidth);
    root->addWidget(_panel, 0, Qt::AlignHCenter);
    root->addStretch(1);

    // Box layout:
    //
    //   contentTop:
    //     no title -> boxTopMargin (8px)
    //     with title -> boxTitleHeight (48px)
    //   content area:
    //     label with boxPadding margins (24, 14, 24, 8)
    //   buttons area:
    //     boxButtonPadding margins (6, 10, 10, 10)
    //     buttons positioned from right edge, 34px height
    //
    // Total height = contentTop + contentHeight + buttonsHeight

    const auto hasTitle = !title.isEmpty();
    const auto &padding = st::boxPadding;
    const auto font = st::baseFont(14);

    auto *panelLayout = new QVBoxLayout(_panel);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(0);
    panelLayout->setSizeConstraint(QLayout::SetMinimumSize);

    // Title area or boxTopMargin spacer.
    if (hasTitle) {
        _title = new QLabel(title, _panel);
        _title->setFont(st::boxTitleFont);
        {
            QPalette pal = _title->palette();
            pal.setColor(QPalette::WindowText, st::boxTitleFg);
            _title->setPalette(pal);
        }
        _title->setWordWrap(true);
        _title->setContentsMargins(
            st::boxTitlePosition.x(), st::boxTitlePosition.y(),
            st::boxTitlePosition.x(), 0);
        _title->setFixedHeight(st::boxTitleHeight);
        panelLayout->addWidget(_title);
    } else {
        // contentTop = boxTopMargin when no title.
        panelLayout->addSpacing(st::boxTopMargin);
    }

    // Text label with boxPadding.
    // Label added as a row with use = boxPadding (no title)
    //   or margins(padding.left, 0, padding.right, padding.bottom) (with title).
    _text = new QLabel(text, _panel);
    _text->setFixedWidth(boxWidth);
    _text->setFont(font);
    {
        // line-height was set in QSS but QLabel ignores CSS line-height for
        // plain text; the multi-line height is computed manually below using
        // st::boxLabelLineHeight, so only the text color needs preserving.
        QPalette pal = _text->palette();
        pal.setColor(QPalette::WindowText, st::boxTextFg);
        _text->setPalette(pal);
    }
    _text->setWordWrap(true);
    if (hasTitle) {
        _text->setContentsMargins(padding.left(), 0, padding.right(), padding.bottom());
    } else {
        _text->setContentsMargins(
            padding.left(), padding.top(), padding.right(), padding.bottom());
    }
    if (richText) {
        // QLabel measures rich text itself (the plain-text line-height math below
        // would count markup as characters), so defer sizing to heightForWidth.
        _text->setTextFormat(Qt::RichText);
        _text->setOpenExternalLinks(true);
        _text->setTextInteractionFlags(
            Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
        _text->setFixedHeight(_text->heightForWidth(boxWidth));
    } else {
        const auto textMargins = _text->contentsMargins();
        const int textWidth = boxWidth - padding.left() - padding.right();
        const auto textBounds = QFontMetrics(font).boundingRect(
            QRect(0, 0, textWidth, std::numeric_limits<int>::max()),
            Qt::TextWordWrap,
            text);
        const auto metrics = QFontMetrics(font);
        const auto lineCount = qMax(
            1,
            (textBounds.height() + metrics.lineSpacing() - 1) / metrics.lineSpacing());
        _text->setFixedHeight(
            qMax(textBounds.height(), lineCount * st::boxLabelLineHeight)
            + textMargins.top()
            + textMargins.bottom());
    }
    panelLayout->addWidget(_text);

    // Button area: buttonPadding = margins(6, 10, 10, 10).
    // Buttons positioned from right: confirm at far right, cancel to its left.
    // Spacing between buttons = buttonPadding.left = 6px.
    auto *buttonsRow = new QHBoxLayout();
    buttonsRow->setContentsMargins(
        btnPad.left(), btnPad.top(), btnPad.right(), btnPad.bottom());
    buttonsRow->setSpacing(btnPad.left()); // 6px between buttons
    buttonsRow->addStretch(1);

    if (showCancel) {
        const auto cancelLabel = cancelText.isEmpty() ? tr("Cancel") : cancelText;
        _cancel = new ::Ui::TextButton(cancelLabel, normalButtonStyle(), _panel);
        _cancel->setFont(st::boxButtonFont);
        _cancel->setFixedSize(
            buttonAutoWidth(cancelLabel, st::boxButtonFont),
            st::boxButtonHeight);
        connect(_cancel, &QAbstractButton::clicked, this, [this] { reject(); });
        buttonsRow->addWidget(_cancel);
    }

    const auto confirmButtonStyle =
        confirmStyle == FilledAttention
            ? filledAttentionButtonStyle()
            : (confirmStyle == Attention
                ? attentionButtonStyle()
                : normalButtonStyle());
    _confirm = new ::Ui::TextButton(confirmText, confirmButtonStyle, _panel);
    _confirm->setFont(st::boxButtonFont);
    _confirm->setFixedSize(
        buttonAutoWidth(confirmText, st::boxButtonFont),
        st::boxButtonHeight);
    connect(_confirm, &QAbstractButton::clicked, this, [this] {
        // Busy mode (sign-out): keep the dialog open, disable the buttons, show a
        // centered spinner, and hand off to the async action instead of closing.
        if (_busyOnConfirm) {
            enterBusyState();
            _busyOnConfirm();
        } else {
            accept();
        }
    });
    buttonsRow->addWidget(_confirm);

    panelLayout->addLayout(buttonsRow);
    _panel->adjustSize();
    _panel->setFixedSize(boxWidth, _panel->height());
}

int HistoryConfirmDialog::exec() {
    const auto restoreFocus = ::TeleMatrix::Focus::saveFocusForPopup();

    raise();
    show();
    setFocus();

    if (_a_shown) {
        _a_shown->start();
    }
    if (_a_layerShown) {
        _a_layerShown->start();
    }

    QEventLoop loop;
    _loop = &loop;
    loop.exec();
    _loop = nullptr;

    hide();
    ::TeleMatrix::Focus::restoreFocusAfterPopup(restoreFocus);
    return _result;
}

void HistoryConfirmDialog::accept() {
    _result = Accepted;
    if (_loop) _loop->quit();
}

void HistoryConfirmDialog::reject() {
    _result = Rejected;
    if (_loop) _loop->quit();
}

void HistoryConfirmDialog::setBusyOnConfirm(std::function<void()> callback) {
    _busyOnConfirm = std::move(callback);
}

void HistoryConfirmDialog::finishBusy() {
    accept();
}

void HistoryConfirmDialog::enterBusyState() {
    if (_busy) {
        return;
    }
    _busy = true;
    if (_cancel) {
        _cancel->setEnabled(false);
    }
    if (_confirm) {
        _confirm->setEnabled(false);
    }
    // No preloader for this action — the dialog just stays disabled until it
    // closes (finishBusy) once the async teardown completes.
}

void HistoryConfirmDialog::paintEvent(QPaintEvent *) {
    QPainter p(this);

    // Background overlay: opacity = bgOpacity, filled with layerBg.
    p.setOpacity(_bgOpacity);
    p.fillRect(rect(), st::layerBg);

    // Box shadow layer: opacity = layerOpacity.
    if (_panel && _layerOpacity > 0) {
        p.setOpacity(_layerOpacity);
        paintBoxShadow(p, _panel->geometry());
    }
}

void HistoryConfirmDialog::mousePressEvent(QMouseEvent *event) {
    // While busy (async action running) the dialog can't be dismissed.
    if (!_busy && _panel && !_panel->geometry().contains(event->pos())) {
        reject();
        return;
    }
    QWidget::mousePressEvent(event);
}

void HistoryConfirmDialog::keyPressEvent(QKeyEvent *event) {
    if (_busy) {
        return; // Ignore Esc/Enter while the async action runs.
    }
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    // Enter/Return triggers confirm button.
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool HistoryConfirmDialog::eventFilter(QObject *obj, QEvent *event) {
    if (obj == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace TeleMatrix

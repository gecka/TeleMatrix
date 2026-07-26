// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "ui/internal_choice_dialog.h"

#include <QEvent>
#include <QEventLoop>
#include <QFontMetrics>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPalette>
#include <QPainter>
#include <QVBoxLayout>

#include <functional>
#include <utility>

#include "styles/style_constants.h"
#include "ui/focus_restore.h"
#include "ui/painter.h"
#include "ui/widgets/close_button.h"
#include "ui/widgets/scroll_area.h"

namespace TeleMatrix::Ui {

namespace {

int rowHeight(const InternalChoiceEntry &entry) {
    return entry.subtitle.isEmpty()
        ? st::settingsButtonHeight
        : st::internalChoiceSubtitleRowHeight;
}

void paintBoxShadow(QPainter &p, const QRect &boxRect) {
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

void fillWithBoxBackground(QWidget *widget) {
    auto palette = widget->palette();
    palette.setColor(QPalette::Window, st::boxBg);
    palette.setColor(QPalette::Base, st::boxBg);
    widget->setPalette(palette);
    widget->setAutoFillBackground(true);
}

class InternalChoiceRow final : public QWidget {
public:
    InternalChoiceRow(
            InternalChoiceEntry entry,
            bool checked,
            QWidget *parent = nullptr)
        : QWidget(parent)
        , _entry(std::move(entry))
        , _checked(checked) {
        setFixedHeight(rowHeight(_entry));
        setMouseTracking(true);
        setCursor(_entry.enabled ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }

    void setClickedCallback(std::function<void(QString)> callback) {
        _clicked = std::move(callback);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (_entry.enabled && _hovered) {
            p.fillRect(rect(), st::windowBgOver);
        }

        const int left = st::settingsButtonPaddingLeft;
        const int right = st::settingsButtonPaddingRight;
        const int radioSize = st::internalChoiceRadioSize;
        const int radioLeft = width() - right - radioSize;
        const int textRight = radioLeft - st::internalChoiceTextRadioSkip;
        const QRect textRect(left, 0, qMax(0, textRight - left), height());

        auto titleFont = _entry.titleFont;
        if (titleFont.family().isEmpty()) {
            titleFont = st::baseFont(14);
        }
        const QFontMetrics titleMetrics(titleFont);
        p.setFont(titleFont);
        p.setPen(_entry.enabled ? st::settingsCheckboxTextFg : st::windowSubTextFg);
        if (_entry.subtitle.isEmpty()) {
            p.drawText(
                textRect,
                Qt::AlignLeft | Qt::AlignVCenter,
                titleMetrics.elidedText(_entry.title, Qt::ElideRight, textRect.width()));
        } else {
            const int titleY = st::internalChoiceSubtitleTitleTop
                + titleMetrics.ascent();
            p.drawText(
                left,
                titleY,
                titleMetrics.elidedText(_entry.title, Qt::ElideRight, textRect.width()));

            const auto subtitleFont = st::baseFont(13);
            const QFontMetrics subtitleMetrics(subtitleFont);
            p.setFont(subtitleFont);
            p.setPen(st::windowSubTextFg);
            const int subtitleY = titleY
                + st::internalChoiceSubtitleSkip
                + subtitleMetrics.ascent();
            p.drawText(
                left,
                subtitleY,
                subtitleMetrics.elidedText(_entry.subtitle, Qt::ElideRight, textRect.width()));
        }

        const QRect radioRect(
            radioLeft,
            (height() - radioSize) / 2,
            radioSize,
            radioSize);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(_checked ? st::windowActiveTextFg : st::windowSubTextFg, 2));
        p.drawEllipse(radioRect);
        if (_checked) {
            p.setPen(Qt::NoPen);
            p.setBrush(st::windowActiveTextFg);
            p.drawEllipse(radioRect.adjusted(5, 5, -5, -5));
        }
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton && _entry.enabled && _clicked) {
            _clicked(_entry.id);
        }
    }

    void enterEvent(QEnterEvent *) override {
        if (!_entry.enabled) {
            return;
        }
        _hovered = true;
        update();
    }

    void leaveEvent(QEvent *) override {
        if (_hovered) {
            _hovered = false;
            update();
        }
    }

private:
    InternalChoiceEntry _entry;
    bool _checked = false;
    bool _hovered = false;
    std::function<void(QString)> _clicked;
};

class InternalChoicePanel final : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(st::boxBg);
        p.drawRoundedRect(rect(), st::boxRadius, st::boxRadius);
    }
};

} // namespace

InternalChoiceDialog::InternalChoiceDialog(
        QWidget *parent,
        const QString &title,
        QVector<InternalChoiceEntry> entries,
        const QString &current)
    : QWidget(parent ? parent->window() : nullptr)
    , _chosen(current) {
    if (parentWidget()) {
        setGeometry(parentWidget()->rect());
        parentWidget()->installEventFilter(this);
    }
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_DeleteOnClose, false);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addStretch(1);

    _panel = new InternalChoicePanel(this);
    _panel->setObjectName(QStringLiteral("InternalChoicePanel"));
    _panel->setFixedWidth(st::internalChoicePopupWidth);
    _panel->setAutoFillBackground(false);
    root->addWidget(_panel, 0, Qt::AlignHCenter);
    root->addStretch(1);

    auto *layout = new QVBoxLayout(_panel);
    layout->setContentsMargins(0, 0, 0, st::boxRadius);
    layout->setSpacing(0);

    auto *titleLabel = new QLabel(title, _panel);
    titleLabel->setFixedHeight(st::settingsTopBarHeight);
    titleLabel->setContentsMargins(st::settingsButtonPaddingLeft, 0, 0, 0);
    titleLabel->setFont(st::boxTitleFont);
    titleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    {
        QPalette pal = titleLabel->palette();
        pal.setColor(QPalette::WindowText, st::boxTitleFg);
        titleLabel->setPalette(pal);
    }
    layout->addWidget(titleLabel);

    _scroll = new ::Ui::ScrollArea(_panel);
    _scroll->setFrameShape(QFrame::NoFrame);
    fillWithBoxBackground(_scroll);
    fillWithBoxBackground(_scroll->viewport());

    auto *content = new QWidget(_scroll);
    fillWithBoxBackground(content);
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, st::internalChoiceBottomSkip);
    contentLayout->setSpacing(0);

    int rowsHeight = 0;
    for (const auto &entry : std::as_const(entries)) {
        rowsHeight += rowHeight(entry);
        auto *row = new InternalChoiceRow(entry, entry.id == current, content);
        row->setClickedCallback([this](const QString &id) {
            _chosen = id;
            accept();
        });
        contentLayout->addWidget(row);
    }
    content->setFixedHeight(rowsHeight + st::internalChoiceBottomSkip);

    _scroll->setWidget(content);
    _scroll->updateBars();
    layout->addWidget(_scroll, 1);

    const int desiredHeight = st::settingsTopBarHeight
        + rowsHeight
        + st::internalChoiceBottomSkip
        + st::boxRadius;
    _panel->setFixedHeight(qMin(st::internalChoicePopupMaxHeight, desiredHeight));

    // Shared top-right close (×), same dismissal as Escape / an outside click.
    // The title is left-aligned, so the top-right corner is clear for it. Created
    // last so it sits above the title row in the child stacking order.
    auto *close = new ::Ui::CloseButton(_panel);
    close->move(st::internalChoicePopupWidth - st::settingsCloseButtonSize, 0);
    connect(close, &::Ui::CloseButton::clicked, this, [this] { reject(); });

    syncScrollContentWidth();
}

InternalChoiceDialog::~InternalChoiceDialog() {
    if (parentWidget()) {
        parentWidget()->removeEventFilter(this);
    }
}

int InternalChoiceDialog::exec() {
    const auto restoreFocus = ::TeleMatrix::Focus::saveFocusForPopup();

    raise();
    show();
    setFocus();
    syncScrollContentWidth();

    QEventLoop loop;
    _loop = &loop;
    loop.exec();
    _loop = nullptr;

    hide();
    ::TeleMatrix::Focus::restoreFocusAfterPopup(restoreFocus);
    return _result;
}

QString InternalChoiceDialog::chosenId() const {
    return _chosen;
}

void InternalChoiceDialog::accept() {
    _result = Accepted;
    if (_loop) {
        _loop->quit();
    }
}

void InternalChoiceDialog::reject() {
    _result = Rejected;
    if (_loop) {
        _loop->quit();
    }
}

void InternalChoiceDialog::syncScrollContentWidth() {
    if (!_scroll) {
        return;
    }
    if (auto *content = _scroll->widget()) {
        const auto viewportWidth = _scroll->viewport()->width();
        if (viewportWidth > 0 && content->width() != viewportWidth) {
            content->resize(viewportWidth, content->height());
        }
    }
    _scroll->updateBars();
}

void InternalChoiceDialog::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), st::layerBg);
    if (_panel) {
        const auto panelRect = _panel->geometry();
        paintBoxShadow(p, panelRect);
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        p.setBrush(st::boxBg);
        p.drawRoundedRect(panelRect, st::boxRadius, st::boxRadius);
    }
}

void InternalChoiceDialog::mousePressEvent(QMouseEvent *event) {
    if (_panel && !_panel->geometry().contains(event->pos())) {
        reject();
        return;
    }
    QWidget::mousePressEvent(event);
}

void InternalChoiceDialog::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool InternalChoiceDialog::eventFilter(QObject *obj, QEvent *event) {
    if (obj == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
        syncScrollContentWidth();
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace TeleMatrix::Ui

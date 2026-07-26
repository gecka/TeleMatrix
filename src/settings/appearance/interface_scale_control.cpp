// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "settings/appearance/interface_scale_control.h"

#include "settings/settings_common_widgets.h"
#include "styles/style_constants.h"
#include "ui/painter.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QSlider>

#include <cmath>

namespace TeleMatrix {

namespace {

// Custom-painted horizontal slider matching settingsSliderStyleSheet() exactly
// (groove 4px windowBgOver r2; 16px windowActiveTextFg knob r8; sub-page
// windowActiveTextFg). Reads st:: at paint time so it tracks theme changes.
class ScaleSlider final : public QSlider {
public:
    explicit ScaleSlider(QWidget *parent)
        : QSlider(Qt::Horizontal, parent) {
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);

        constexpr int kGrooveH = 4;
        constexpr int kHandle = 16;

        const auto cy = height() / 2.0;
        const QRectF groove(
            kHandle / 2.0,
            cy - kGrooveH / 2.0,
            qMax(0.0, width() - qreal(kHandle)),
            kGrooveH);

        // Groove background.
        p.setBrush(st::windowBgOver);
        p.drawRoundedRect(groove, 2, 2);

        // Position of the handle centre along the groove.
        const auto span = maximum() - minimum();
        const auto progress = (span > 0)
            ? qreal(value() - minimum()) / span
            : 0.0;
        const auto knobCx = groove.left() + groove.width() * progress;

        // Sub-page (filled portion to the left of the knob).
        if (knobCx > groove.left()) {
            const QRectF sub(
                groove.left(),
                groove.top(),
                knobCx - groove.left(),
                groove.height());
            p.setBrush(st::windowActiveTextFg);
            p.drawRoundedRect(sub, 2, 2);
        }

        // Handle.
        const QRectF knob(
            knobCx - kHandle / 2.0,
            cy - kHandle / 2.0,
            kHandle,
            kHandle);
        p.setBrush(st::windowActiveTextFg);
        p.drawEllipse(knob);
    }
};

} // namespace

InterfaceScaleControl::InterfaceScaleControl(
        QVector<int> values,
        int currentScale,
        QWidget *parent)
    : QWidget(parent)
    , _values(std::move(values)) {
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(20, 4, 20, 8);
    layout->setSpacing(12);

    _slider = new ScaleSlider(this);
    _slider->setMinimum(0);
    _slider->setMaximum(qMax(0, _values.size() - 1));
    _slider->setPageStep(1);
    _slider->setSingleStep(1);
    _slider->setFixedHeight(28);
    _slider->setCursor(Qt::PointingHandCursor);

    _label = new QLabel(this);
    _label->setFont(st::baseFont(14));
    _label->setFixedWidth(48);
    _label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    {
        QPalette pal = _label->palette();
        pal.setColor(QPalette::WindowText, st::windowActiveTextFg);
        _label->setPalette(pal);
    }

    layout->addWidget(_slider, 1);
    // (refreshTheme below re-applies the label color on theme change.)
    layout->addWidget(_label);

    setScale(currentScale);

    connect(_slider, &QSlider::valueChanged, this, [this](int index) {
        if (index < 0 || index >= _values.size()) {
            return;
        }
        _label->setText(QStringLiteral("%1%").arg(_values[index]));
        emit scaleLabelChanged(_values[index]);
    });
    connect(_slider, &QSlider::sliderReleased, this, [this] {
        emit scaleReleased(this->currentScale());
    });
}

void InterfaceScaleControl::setScale(int scale) {
    if (!_slider || _values.isEmpty()) {
        return;
    }
    const auto index = indexForScale(scale);
    _slider->setValue(index);
    _label->setText(QStringLiteral("%1%").arg(_values[index]));
}

int InterfaceScaleControl::currentScale() const {
    if (!_slider || _values.isEmpty()) {
        return 0;
    }
    const auto index = qBound(0, _slider->value(), _values.size() - 1);
    return _values[index];
}

void InterfaceScaleControl::refreshTheme() {
    if (!_label) {
        return;
    }
    QPalette pal = _label->palette();
    pal.setColor(QPalette::WindowText, st::windowActiveTextFg);
    _label->setPalette(pal);
    update();
}

int InterfaceScaleControl::indexForScale(int scale) const {
    if (_values.isEmpty()) {
        return 0;
    }
    auto best = 0;
    auto bestDist = std::abs(_values[0] - scale);
    for (auto i = 1; i < _values.size(); ++i) {
        const auto dist = std::abs(_values[i] - scale);
        if (dist < bestDist) {
            bestDist = dist;
            best = i;
        }
    }
    return best;
}

} // namespace TeleMatrix

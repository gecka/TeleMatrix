// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QVector>
#include <QWidget>

class QLabel;
class QSlider;

namespace TeleMatrix {

class InterfaceScaleControl final : public QWidget {
    Q_OBJECT

public:
    InterfaceScaleControl(
        QVector<int> values,
        int currentScale,
        QWidget *parent = nullptr);

    void setScale(int scale);
    [[nodiscard]] int currentScale() const;

    // Re-apply the value label's color from the current st:: tokens. Call on
    // theme change (the slider self-paints live; the QLabel palette does not).
    void refreshTheme();

Q_SIGNALS:
    void scaleLabelChanged(int scale);
    void scaleReleased(int scale);

private:
    [[nodiscard]] int indexForScale(int scale) const;

    QVector<int> _values;
    QSlider *_slider = nullptr;
    QLabel *_label = nullptr;
};

} // namespace TeleMatrix

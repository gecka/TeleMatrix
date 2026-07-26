// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include <QPointer>
#include <QWidget>

namespace TeleMatrix {

/// Full-size overlay container that dims the background and hosts a layer widget.
class LayerStackWidget : public QWidget {
    Q_OBJECT

public:
    explicit LayerStackWidget(QWidget *parent);

    /// Show a widget as a layer. LayerStackWidget does NOT take ownership.
    void showLayer(QWidget *layer);

    /// Hide and remove the current layer.
    void hideLayer();

    /// Whether a layer is currently shown.
    [[nodiscard]] bool hasLayer() const { return _layer != nullptr; }

Q_SIGNALS:
    void layerHidden();

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    void positionLayer();

    QWidget *_layer = nullptr;
    QPointer<QWidget> _restoreFocus;
};

} // namespace TeleMatrix

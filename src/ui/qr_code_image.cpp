// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "ui/qr_code_image.h"

#include <QByteArray>
#include <QColor>
#include <QPainter>
#include <QRect>

namespace TeleMatrix {

void paintQrModules(
        QPainter &p,
        const QRect &target,
        const QByteArray &modules,
        int size,
        const QColor &dark,
        const QColor &light,
        int quiet) {
    if (size <= 0 || modules.size() < qsizetype(size) * size) {
        return;
    }
    const int total = size + 2 * quiet;
    const int module = qMin(target.width(), target.height()) / total;
    if (module <= 0) {
        return;
    }
    const int qrPx = module * total;
    const int originX = target.x() + (target.width() - qrPx) / 2;
    const int originY = target.y() + (target.height() - qrPx) / 2;

    p.fillRect(QRect(originX, originY, qrPx, qrPx), light);
    p.setPen(Qt::NoPen);
    p.setBrush(dark);
    const auto *cells =
        reinterpret_cast<const unsigned char *>(modules.constData());
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (cells[y * size + x]) {
                p.drawRect(
                    originX + (quiet + x) * module,
                    originY + (quiet + y) * module,
                    module,
                    module);
            }
        }
    }
}

} // namespace TeleMatrix

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

class QByteArray;
class QColor;
class QPainter;
class QRect;

namespace TeleMatrix {

// Paint a QR module grid (`size`*`size` bytes; 1 = dark, 0 = light, row-major)
// crisply centered to fit `target`, using whole-pixel modules so the code stays
// scannable. Fills the fitted square with `light` first, then paints `dark`
// modules. `quiet` is the quiet-zone width in modules.
void paintQrModules(
    QPainter &p,
    const QRect &target,
    const QByteArray &modules,
    int size,
    const QColor &dark,
    const QColor &light,
    int quiet = 4);

} // namespace TeleMatrix

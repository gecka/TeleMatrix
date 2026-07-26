// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

// Compatibility header: provides PainterHighQualityEnabler.
// In lib_ui this is part of ui/painter.h.
#pragma once

#include <QPainter>

// RAII helper that enables antialiasing and smooth pixmap transform
// on a QPainter for its lifetime, restoring previous state on destruction.
class PainterHighQualityEnabler {
public:
    explicit PainterHighQualityEnabler(QPainter &p)
        : _painter(p)
        , _hints(p.renderHints())
    {
        _painter.setRenderHints(
            QPainter::Antialiasing
            | QPainter::SmoothPixmapTransform
            | QPainter::TextAntialiasing);
    }

    ~PainterHighQualityEnabler() {
        _painter.setRenderHints(_hints);
    }

    PainterHighQualityEnabler(const PainterHighQualityEnabler &) = delete;
    PainterHighQualityEnabler &operator=(const PainterHighQualityEnabler &) = delete;

private:
    QPainter &_painter;
    QPainter::RenderHints _hints;
};

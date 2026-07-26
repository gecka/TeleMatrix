// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "history_attach_popup.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QScreen>

#include "styles/style_constants.h"
#include "ui/painter.h"
#include "ui/platform/ui_utility_mac.h"
#include "ui/style/icon_provider.h"

namespace TeleMatrix {

namespace {

// Shadow tile loading — reuse the same helpers as PopupMenu.
struct ShadowTiles {
    QImage left;
    QImage topLeft;
    QImage top;
    QImage topRight;
    QImage right;
    QImage bottomLeft;
    QImage bottom;
    QImage bottomRight;
};

[[nodiscard]] ShadowTiles loadShadowTiles(qreal dpr) {
    static QHash<int, ShadowTiles> cache;
    const auto key = dpr >= 2.5 ? 3 : (dpr >= 1.5 ? 2 : 1);
    if (const auto i = cache.constFind(key); i != cache.cend()) {
        return i.value();
    }

    const auto base = QStringLiteral(":/telematrix/icons/shadow/");
    auto load = [&](const QString &name) -> QImage {
        const auto mask = TeleMatrix::Style::IconProvider::loadScaledMask(base, name);
        auto colorized = TeleMatrix::Style::IconProvider::colorizeMask(mask, st::windowShadowFg);
        if (key == 3) {
            colorized.setDevicePixelRatio(3.0);
        } else if (key == 2) {
            colorized.setDevicePixelRatio(2.0);
        } else {
            colorized.setDevicePixelRatio(1.0);
        }
        return colorized;
    };

    ShadowTiles tiles;
    tiles.left = load(QStringLiteral("round_shadow_box_left"));
    tiles.topLeft = load(QStringLiteral("round_shadow_box_top_left"));
    tiles.top = load(QStringLiteral("round_shadow_box_top"));
    tiles.bottomLeft = load(QStringLiteral("round_shadow_box_bottom_left"));
    tiles.bottom = load(QStringLiteral("round_shadow_box_bottom"));
    tiles.topRight = tiles.topLeft.flipped(Qt::Horizontal);
    tiles.topRight.setDevicePixelRatio(tiles.topLeft.devicePixelRatio());
    tiles.right = tiles.left.flipped(Qt::Horizontal);
    tiles.right.setDevicePixelRatio(tiles.left.devicePixelRatio());
    tiles.bottomRight = tiles.bottomLeft.flipped(Qt::Horizontal);
    tiles.bottomRight.setDevicePixelRatio(tiles.bottomLeft.devicePixelRatio());

    cache.insert(key, tiles);
    return tiles;
}

void paintShadow(QPainter &p, const QRect &box, const ShadowTiles &st, int extend) {
    const auto ratio = st.topLeft.devicePixelRatio();
    auto logicalW = [ratio](const QImage &img) {
        return qRound(img.width() / ratio);
    };
    auto logicalH = [ratio](const QImage &img) {
        return qRound(img.height() / ratio);
    };

    const auto tlW = logicalW(st.topLeft);
    const auto tlH = logicalH(st.topLeft);
    const auto trW = logicalW(st.topRight);
    const auto trH = logicalH(st.topRight);
    const auto blW = logicalW(st.bottomLeft);
    const auto blH = logicalH(st.bottomLeft);
    const auto brW = logicalW(st.bottomRight);
    const auto brH = logicalH(st.bottomRight);
    const auto leftW = logicalW(st.left);
    const auto rightW = logicalW(st.right);
    const auto topH = logicalH(st.top);
    const auto bottomH = logicalH(st.bottom);

    // Left edge.
    {
        auto from = box.y();
        auto to = box.y() + box.height();
        p.drawImage(box.x() - extend, box.y() - extend, st.topLeft);
        from += tlH - extend;
        p.drawImage(box.x() - extend, box.y() + box.height() + extend - blH, st.bottomLeft);
        to -= blH - extend;
        if (to > from) {
            p.drawImage(
                QRect(box.x() - extend, from, leftW, to - from),
                st.left,
                QRect(0, 0, st.left.width(), st.left.height()));
        }
    }

    // Right edge.
    {
        auto from = box.y();
        auto to = box.y() + box.height();
        p.drawImage(box.x() + box.width() + extend - trW, box.y() - extend, st.topRight);
        from += trH - extend;
        p.drawImage(box.x() + box.width() + extend - brW, box.y() + box.height() + extend - brH, st.bottomRight);
        to -= brH - extend;
        if (to > from) {
            p.drawImage(
                QRect(box.x() + box.width() + extend - rightW, from, rightW, to - from),
                st.right,
                QRect(0, 0, st.right.width(), st.right.height()));
        }
    }

    // Top edge.
    {
        auto from = box.x() + tlW - extend;
        auto to = box.x() + box.width() - trW + extend;
        if (to > from) {
            p.drawImage(
                QRect(from, box.y() - extend, to - from, topH),
                st.top,
                QRect(0, 0, st.top.width(), st.top.height()));
        }
    }

    // Bottom edge.
    {
        auto from = box.x() + blW - extend;
        auto to = box.x() + box.width() - brW + extend;
        if (to > from) {
            p.drawImage(
                QRect(from, box.y() + box.height() + extend - bottomH, to - from, bottomH),
                st.bottom,
                QRect(0, 0, st.bottom.width(), st.bottom.height()));
        }
    }
}

/// Paint a camera icon (simplified: circle with lens-like shape).
void paintPhotoIcon(QPainter &p, const QRect &rect, const QColor &color) {
    PainterHighQualityEnabler hq(p);
    const auto cx = rect.center().x();
    const auto cy = rect.center().y();
    const auto s = qMin(rect.width(), rect.height());

    // Camera body — rounded rect.
    p.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    const auto bodyW = s * 0.8;
    const auto bodyH = s * 0.6;
    const auto bodyX = cx - bodyW / 2.0;
    const auto bodyY = cy - bodyH / 2.0 + 1.0;
    p.drawRoundedRect(QRectF(bodyX, bodyY, bodyW, bodyH), 2.0, 2.0);

    // Lens circle.
    const auto lensR = s * 0.16;
    p.drawEllipse(QPointF(cx, cy + 1.0), lensR, lensR);

    // Flash bump on top.
    const auto bumpW = s * 0.25;
    const auto bumpH = 2.0;
    const auto bumpX = cx - bumpW / 2.0 - s * 0.1;
    const auto bumpY = bodyY - bumpH;
    p.drawRoundedRect(QRectF(bumpX, bumpY, bumpW, bumpH), 1.0, 1.0);
}

/// Paint a document/file icon.
void paintDocumentIcon(QPainter &p, const QRect &rect, const QColor &color) {
    PainterHighQualityEnabler hq(p);
    const auto cx = rect.center().x();
    const auto cy = rect.center().y();
    const auto s = qMin(rect.width(), rect.height());

    p.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);

    // Document body.
    const auto docW = s * 0.55;
    const auto docH = s * 0.75;
    const auto docX = cx - docW / 2.0;
    const auto docY = cy - docH / 2.0;
    const auto foldSize = s * 0.15;

    QPainterPath path;
    path.moveTo(docX, docY + 2.0);
    path.lineTo(docX, docY + docH - 2.0);
    path.quadTo(docX, docY + docH, docX + 2.0, docY + docH);
    path.lineTo(docX + docW - 2.0, docY + docH);
    path.quadTo(docX + docW, docY + docH, docX + docW, docY + docH - 2.0);
    path.lineTo(docX + docW, docY + foldSize);
    path.lineTo(docX + docW - foldSize, docY);
    path.lineTo(docX + 2.0, docY);
    path.quadTo(docX, docY, docX, docY + 2.0);
    p.drawPath(path);

    // Fold corner.
    p.drawLine(
        QPointF(docX + docW - foldSize, docY),
        QPointF(docX + docW - foldSize, docY + foldSize));
    p.drawLine(
        QPointF(docX + docW - foldSize, docY + foldSize),
        QPointF(docX + docW, docY + foldSize));
}

} // namespace

HistoryAttachPopup::HistoryAttachPopup(QWidget *parent)
    : QWidget(parent) {
    setWindowFlags(
        Qt::Tool
        | Qt::FramelessWindowHint
        | Qt::NoDropShadowWindowHint
        | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);

    const auto bodyHeight = kScrollPaddingTop
        + kRowCount * kRowHeight
        + kScrollPaddingBottom;
    setFixedSize(
        kPopupWidth + 2 * kShadowExtend,
        bodyHeight + 2 * kShadowExtend);
}

void HistoryAttachPopup::showNear(QWidget *anchor) {
    if (!anchor) {
        return;
    }

    // Position above the anchor, aligned to its left edge.
    const auto anchorGlobal = anchor->mapToGlobal(QPoint(0, 0));
    auto x = anchorGlobal.x() - kShadowExtend;
    auto y = anchorGlobal.y() - height() + kShadowExtend;

    // Clamp to screen.
    if (const auto *scr = anchor->screen()) {
        const auto avail = scr->availableGeometry();
        if (x + width() > avail.right() + 1) {
            x = avail.right() + 1 - width();
        }
        if (y < avail.top()) {
            // Show below the anchor instead.
            y = anchorGlobal.y() + anchor->height() - kShadowExtend;
        }
        x = qMax(x, avail.left());
    }

    move(x, y);
    show();
    raise();
    activateWindow();

    qApp->installEventFilter(this);
    Platform::AcceptAllMouseInput(this);
}

int HistoryAttachPopup::rowAtPos(const QPoint &pos) const {
    const auto innerX = pos.x() - kShadowExtend;
    const auto innerY = pos.y() - kShadowExtend - kScrollPaddingTop;
    if (innerX < 0 || innerX >= kPopupWidth) {
        return -1;
    }
    if (innerY < 0 || innerY >= kRowCount * kRowHeight) {
        return -1;
    }
    return innerY / kRowHeight;
}

void HistoryAttachPopup::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const auto innerRect = rect().adjusted(
        kShadowExtend, kShadowExtend,
        -kShadowExtend, -kShadowExtend);

    // Shadow.
    const auto tiles = loadShadowTiles(devicePixelRatioF());
    paintShadow(p, innerRect, tiles, kShadowExtend);

    // Rounded body background.
    QPainterPath clipPath;
    clipPath.addRoundedRect(
        QRectF(innerRect).adjusted(0.5, 0.5, -0.5, -0.5),
        kPopupRadius,
        kPopupRadius);
    p.fillPath(clipPath, st::menuBg);
    p.setClipPath(clipPath);

    const auto f = st::baseFont(13);
    p.setFont(f);
    const QFontMetrics fm(f);

    struct RowData {
        QString label;
        // 0 = photo, 1 = document
        int iconType;
    };
    const RowData rows[kRowCount] = {
        { tr("Photo or Video"), 0 },
        { tr("Document"), 1 },
    };

    for (int i = 0; i < kRowCount; ++i) {
        const auto rowY = kShadowExtend + kScrollPaddingTop + i * kRowHeight;
        const auto rowRect = QRect(kShadowExtend, rowY, kPopupWidth, kRowHeight);

        const auto isHovered = (i == _hoveredRow);
        p.fillRect(rowRect, isHovered ? st::menuBgOver : st::menuBg);

        // Icon area.
        const auto iconLeft = rowRect.left() + kPadding;
        const auto iconTop = rowRect.top() + (kRowHeight - kIconSize) / 2;
        const auto iconRect = QRect(iconLeft, iconTop, kIconSize, kIconSize);
        const auto iconColor = isHovered ? st::menuIconFgOver : st::menuIconFg;

        if (rows[i].iconType == 0) {
            paintPhotoIcon(p, iconRect, iconColor);
        } else {
            paintDocumentIcon(p, iconRect, iconColor);
        }

        // Label text.
        const auto textLeft = iconLeft + kIconSize + kPadding;
        const auto textY = rowRect.top() + (kRowHeight + fm.ascent() - fm.descent()) / 2;
        p.setPen(isHovered ? st::windowFgOver : st::windowFg);
        p.drawText(textLeft, textY, rows[i].label);
    }
}

void HistoryAttachPopup::mouseMoveEvent(QMouseEvent *e) {
    const auto row = rowAtPos(e->pos());
    if (row != _hoveredRow) {
        _hoveredRow = row;
        update();
        if (row >= 0) {
            Platform::ForcePointingHandCursor();
        }
    }
}

void HistoryAttachPopup::mousePressEvent(QMouseEvent *e) {
    const auto row = rowAtPos(e->pos());
    if (row < 0) {
        return;
    }

    qApp->removeEventFilter(this);
    hide();

    if (row == 0) {
        emit photoOrVideoChosen();
    } else if (row == 1) {
        emit documentChosen();
    }

    deleteLater();
}

void HistoryAttachPopup::leaveEvent(QEvent *e) {
    QWidget::leaveEvent(e);
    _hoveredRow = -1;
    update();
}

void HistoryAttachPopup::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape) {
        qApp->removeEventFilter(this);
        hide();
        deleteLater();
        return;
    }
    QWidget::keyPressEvent(e);
}

bool HistoryAttachPopup::eventFilter(QObject *obj [[maybe_unused]], QEvent *e) {
    if (e->type() == QEvent::MouseButtonPress) {
        const auto *me = static_cast<QMouseEvent *>(e);
        const auto globalPos = me->globalPosition().toPoint();
        if (!geometry().contains(globalPos)) {
            qApp->removeEventFilter(this);
            hide();
            deleteLater();
            return false;
        }
    } else if (e->type() == QEvent::ApplicationDeactivate) {
        qApp->removeEventFilter(this);
        hide();
        deleteLater();
    }
    return false;
}

} // namespace TeleMatrix

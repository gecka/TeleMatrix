// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "app/tray_icon.h"

#include "app/app_main_window.h"

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QRect>
#include <QSystemTrayIcon>

namespace TeleMatrix {

TrayIcon::TrayIcon(AppMainWindow *window, QObject *parent)
    : QObject(parent)
    , _window(window) {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return; // no tray host (some minimal WMs) — silently do nothing
    }
    _tray = new QSystemTrayIcon(this);

    auto *menu = new QMenu(_window);
    QObject::connect(
        menu->addAction(tr("Open TeleMatrix")), &QAction::triggered, this,
        [this] {
            if (_window) {
                _window->bringToFront();
            }
        });
    QObject::connect(
        menu->addAction(tr("Quit")), &QAction::triggered, this, [this] {
            if (_window) {
                _window->requestQuit(QuitReason::MenuAction);
            } else {
                qApp->quit();
            }
        });
    _tray->setContextMenu(menu);

    QObject::connect(
        _tray, &QSystemTrayIcon::activated, this,
        [this](QSystemTrayIcon::ActivationReason reason) {
            if ((reason == QSystemTrayIcon::Trigger
                 || reason == QSystemTrayIcon::DoubleClick)
                && _window) {
                _window->bringToFront();
            }
        });

    render();
    _tray->show();
}

TrayIcon::~TrayIcon() {
    if (_tray) {
        _tray->hide();
    }
}

void TrayIcon::setUnreadCount(int count) {
    count = qMax(0, count);
    if (count == _count) {
        return;
    }
    _count = count;
    render();
}

void TrayIcon::render() {
    if (!_tray) {
        return;
    }
    constexpr int kSize = 64;
    const QPixmap base(QStringLiteral(":/telematrix/app/icon.png"));
    QPixmap canvas = base.isNull()
        ? QPixmap(kSize, kSize)
        : base.scaled(kSize, kSize, Qt::KeepAspectRatio,
                      Qt::SmoothTransformation);
    if (base.isNull()) {
        canvas.fill(Qt::transparent);
    }

    if (_count > 0) {
        QPainter painter(&canvas);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QString text =
            (_count > 99) ? QStringLiteral("99+") : QString::number(_count);
        const int d = int(canvas.width() * 0.55);
        const QRect badge(canvas.width() - d, canvas.height() - d, d, d);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0xE5, 0x39, 0x35));
        painter.drawEllipse(badge);
        QFont font = painter.font();
        font.setBold(true);
        font.setPixelSize(int(d * (text.size() >= 3 ? 0.42 : 0.6)));
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(badge, Qt::AlignCenter, text);
    }

    _tray->setIcon(QIcon(canvas));
    _tray->setToolTip(
        _count > 0 ? tr("TeleMatrix — %n unread message(s)", nullptr, _count)
                   : QStringLiteral("TeleMatrix"));
}

} // namespace TeleMatrix

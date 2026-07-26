// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.
//
// Windows-only: compiled solely on WIN32 (see the if(WIN32) block in
// CMakeLists.txt). Uses C++/WinRT (Windows 10 SDK) + Win32 taskbar APIs. Not
// built or verified on the macOS dev host.

#include "window/notifications_manager_win.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <shobjidl.h> // ITaskbarList3, CLSID_TaskbarList

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QString>

#include "app/app_main_window.h"
#include "app/platform/win/app_user_model_id.h"
#include "window/notifications_win_keys.h"

using namespace winrt::Windows::UI::Notifications;
using namespace winrt::Windows::Data::Xml::Dom;

namespace TeleMatrix::Notifications {

namespace {

winrt::hstring toHString(const QString &s) {
    return winrt::hstring(
        reinterpret_cast<const wchar_t *>(s.utf16()),
        static_cast<uint32_t>(s.size()));
}

QString xmlEscape(const QString &s) {
    QString out;
    out.reserve(s.size() + 16);
    for (const QChar ch : s) {
        switch (ch.unicode()) {
        case u'&': out += QStringLiteral("&amp;"); break;
        case u'<': out += QStringLiteral("&lt;"); break;
        case u'>': out += QStringLiteral("&gt;"); break;
        case u'"': out += QStringLiteral("&quot;"); break;
        case u'\'': out += QStringLiteral("&apos;"); break;
        default: out += ch; break;
        }
    }
    return out;
}

// QImage -> HICON via a 32bpp top-down DIB with per-pixel alpha (Qt6 dropped the
// QtWin helper, so this is done by hand).
HICON createHIconFromImage(const QImage &source) {
    const QImage image = source.convertToFormat(QImage::Format_ARGB32);
    const int w = image.width();
    const int h = image.height();

    BITMAPV5HEADER header = {};
    header.bV5Size = sizeof(BITMAPV5HEADER);
    header.bV5Width = w;
    header.bV5Height = -h; // top-down
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    HDC hdc = ::GetDC(nullptr);
    void *bits = nullptr;
    HBITMAP color = ::CreateDIBSection(
        hdc, reinterpret_cast<BITMAPINFO *>(&header), DIB_RGB_COLORS, &bits,
        nullptr, 0);
    ::ReleaseDC(nullptr, hdc);
    if (!color || !bits) {
        if (color) {
            ::DeleteObject(color);
        }
        return nullptr;
    }

    // QImage ARGB32 is 0xAARRGGBB per pixel == bytes B,G,R,A little-endian, which
    // matches the BGRA layout of the DIB above; copy row by row.
    for (int y = 0; y < h; ++y) {
        memcpy(
            static_cast<uchar *>(bits) + static_cast<size_t>(y) * w * 4,
            image.constScanLine(y),
            static_cast<size_t>(w) * 4);
    }

    HBITMAP mask = ::CreateBitmap(w, h, 1, 1, nullptr);
    ICONINFO info = {};
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = mask;
    HICON icon = ::CreateIconIndirect(&info);

    ::DeleteObject(color);
    ::DeleteObject(mask);
    return icon;
}

HICON renderBadgeIcon(int count) {
    const int metric = ::GetSystemMetrics(SM_CXSMICON);
    const int size = (metric > 0) ? metric : 16;
    const QString text =
        (count > 99) ? QStringLiteral("99+") : QString::number(count);

    QImage image(size, size, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0xE5, 0x39, 0x35));
        painter.drawEllipse(0, 0, size - 1, size - 1);

        QFont font = painter.font();
        font.setBold(true);
        font.setPixelSize(int(size * (text.size() >= 3 ? 0.42 : 0.62)));
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(image.rect(), Qt::AlignCenter, text);
    }
    return createHIconFromImage(image);
}

} // namespace

struct WinManager::Impl {
    ToastNotifier notifier{ nullptr };
    winrt::com_ptr<ITaskbarList3> taskbar;

    Impl() {
        try {
            // Ensure the Windows Runtime is up on this (the GUI) thread. Qt has
            // already initialised COM as STA, so single-threaded matches and this
            // is effectively a no-op (S_FALSE). A single app-lifetime init needs
            // no matching uninit.
            winrt::init_apartment(winrt::apartment_type::single_threaded);
        } catch (winrt::hresult_error const &) {
            // Already initialised in a compatible mode — fine.
        }
        try {
            notifier = ToastNotificationManager::CreateToastNotifier(
                winrt::hstring{ Platform::Win::kAppUserModelId });
        } catch (...) {
            notifier = nullptr;
        }
    }

    bool ensureTaskbar() {
        if (taskbar) {
            return true;
        }
        try {
            taskbar = winrt::create_instance<ITaskbarList3>(
                CLSID_TaskbarList, CLSCTX_INPROC_SERVER);
            if (taskbar) {
                taskbar->HrInit();
            }
        } catch (...) {
            taskbar = nullptr;
        }
        return static_cast<bool>(taskbar);
    }
};

WinManager::WinManager(AppMainWindow *window)
    : _impl(std::make_unique<Impl>())
    , _window(window) {
}

WinManager::~WinManager() {
    clearAll();
}

void WinManager::showNotification(
    const QString &roomId,
    const QString &eventId,
    const QString &senderName,
    const QString &chatName,
    const QString &messageText,
    bool isDirect,
    bool isMention,
    const QString &avatarPath,
    bool isInvite) {
    if (!_impl->notifier) {
        return;
    }
    try {
        // Body truncated to ~200 like the macOS manager. Toast audio is silenced:
        // the cross-platform System::maybePlaySound() is the single sound source
        // (respecting soundNotify + the throttle), matching macOS.
        const QString body = messageText.left(200);
        const QString scenario =
            isMention ? QStringLiteral("urgent") : QStringLiteral("default");

        QString xml = QStringLiteral("<toast launch=\"room=");
        xml += xmlEscape(roomId);
        xml += QStringLiteral("\" scenario=\"");
        xml += scenario;
        xml += QStringLiteral("\"><visual><binding template=\"ToastGeneric\">");
        if (!avatarPath.isEmpty()) {
            // Sender avatar as the toast's app-logo override (circular crop).
            QString uri = avatarPath;
            uri.replace(QLatin1Char('\\'), QLatin1Char('/'));
            xml += QStringLiteral(
                "<image placement=\"appLogoOverride\" hint-crop=\"circle\" src=\"file:///");
            xml += xmlEscape(uri);
            xml += QStringLiteral("\"/>");
        }
        xml += QStringLiteral("<text hint-maxLines=\"1\">");
        xml += xmlEscape(senderName);
        xml += QStringLiteral("</text>");
        if (!isDirect && !chatName.isEmpty()) {
            xml += QStringLiteral("<text>");
            xml += xmlEscape(chatName);
            xml += QStringLiteral("</text>");
        }
        xml += QStringLiteral("<text>");
        xml += xmlEscape(body);
        xml += QStringLiteral("</text></binding></visual>");

        // Inline reply + Mark-as-read. activationType="foreground" routes to the
        // Activated callback while the app runs; closed-app / Action-Center
        // activation would additionally need the ToastActivator COM object (not
        // yet implemented). Skipped for an invite — neither action applies;
        // clicking the toast body still activates and opens the room.
        if (!isInvite) {
            const QString roomArg = xmlEscape(roomId);
            xml += QStringLiteral(
                "<actions>"
                "<input id=\"reply\" type=\"text\" placeHolderContent=\"Reply\"/>"
                "<action content=\"Send\" arguments=\"action=reply&amp;room=");
            xml += roomArg;
            xml += QStringLiteral(
                "\" activationType=\"foreground\" hint-inputId=\"reply\"/>"
                "<action content=\"Mark as read\" arguments=\"action=markread&amp;room=");
            xml += roomArg;
            xml += QStringLiteral(
                "\" activationType=\"foreground\"/>"
                "</actions>");
        }
        xml += QStringLiteral("<audio silent=\"true\"/></toast>");

        XmlDocument doc;
        doc.LoadXml(toHString(xml));

        ToastNotification toast{ doc };
        // Group = per room (native stacking + the handle clearFromRoom removes);
        // Tag = per event (idempotent identity).
        toast.Group(toHString(toastGroupKey(roomId)));
        toast.Tag(toHString(toastTagKey(eventId)));

        // Foreground click routing. The handler fires on a WinRT thread, so marshal
        // back to the Qt main thread. Capturing `this` mirrors the macOS delegate's
        // assumption that the manager outlives delivered toasts (it lives for the
        // app session); invokeMethod on a destroyed receiver is dropped by Qt.
        WinManager *self = this;
        toast.Activated([self](
            ToastNotification const &,
            winrt::Windows::Foundation::IInspectable const &arg) {
            auto args = arg.try_as<ToastActivatedEventArgs>();
            if (!args) {
                return;
            }
            const winrt::hstring raw = args.Arguments();
            const QString argString =
                QString::fromWCharArray(raw.c_str(), int(raw.size()));
            QString action;
            QString room;
            for (const QString &pair :
                 argString.split(QLatin1Char('&'), Qt::SkipEmptyParts)) {
                const int eq = pair.indexOf(QLatin1Char('='));
                if (eq < 0) {
                    continue;
                }
                const QString key = pair.left(eq);
                const QString value = pair.mid(eq + 1);
                if (key == QLatin1String("action")) {
                    action = value;
                } else if (key == QLatin1String("room")) {
                    room = value;
                }
            }
            if (room.isEmpty()) {
                return;
            }
            if (action == QLatin1String("reply")) {
                QString text;
                try {
                    const auto input = args.UserInput();
                    if (input != nullptr && input.HasKey(L"reply")) {
                        text = QString::fromWCharArray(
                            winrt::unbox_value<winrt::hstring>(
                                input.Lookup(L"reply"))
                                .c_str());
                    }
                } catch (...) {
                }
                QMetaObject::invokeMethod(
                    self,
                    [self, room, text]() {
                        emit self->notificationReplied(room, text);
                    },
                    Qt::QueuedConnection);
            } else if (action == QLatin1String("markread")) {
                QMetaObject::invokeMethod(
                    self,
                    [self, room]() { emit self->notificationMarkRead(room); },
                    Qt::QueuedConnection);
            } else {
                QMetaObject::invokeMethod(
                    self,
                    [self, room]() { emit self->notificationActivated(room); },
                    Qt::QueuedConnection);
            }
        });

        _impl->notifier.Show(toast);
    } catch (...) {
        // A toast failure must never crash the app.
    }
}

void WinManager::clearFromRoom(const QString &roomId) {
    try {
        ToastNotificationManager::History().RemoveGroup(
            toHString(toastGroupKey(roomId)),
            winrt::hstring{ Platform::Win::kAppUserModelId });
    } catch (...) {
    }
}

void WinManager::clearAll() {
    try {
        ToastNotificationManager::History().Clear(
            winrt::hstring{ Platform::Win::kAppUserModelId });
    } catch (...) {
    }
}

void WinManager::updateDockBadge(int totalUnread) {
    if (!_window) {
        return;
    }
    const HWND hwnd = reinterpret_cast<HWND>(_window->winId());
    if (!hwnd || !_impl->ensureTaskbar()) {
        return;
    }
    if (totalUnread <= 0) {
        _impl->taskbar->SetOverlayIcon(hwnd, nullptr, L"");
        return;
    }
    HICON icon = renderBadgeIcon(totalUnread);
    _impl->taskbar->SetOverlayIcon(hwnd, icon, L"Unread messages");
    if (icon) {
        ::DestroyIcon(icon);
    }
}

void WinManager::bounceDockIcon() {
    // Taskbar flash — Qt's alert() calls FlashWindowEx under the hood on Windows.
    if (_window) {
        QApplication::alert(_window);
    }
}

} // namespace TeleMatrix::Notifications

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "dialogs_widget.h"
#include "dialogs_chat_search_in.h"
#include "dialogs_filter_sidebar.h"
#include "dialogs_folders_box.h"
#include "dialogs_edit_folder_box.h"
#include "dialogs_leave_space_box.h"
#include "dialogs_inner.h"
#include "dialogs_update_bar.h"
#include "../app/app_controller.h"
#include "../core/update_service.h"
#include "../core/core_settings.h"
#include "ui/safe_url.h"
#include "../app/unread_state_store.h"
#include "../history/history_confirm_dialog.h"

#include "dialogs_member_picker_box.h"

#include <algorithm>
#include <QApplication>
#include <QCoreApplication>
#include <QFontMetricsF>
#include <QHash>
#include <QHideEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScreen>
#include <QSet>
#include <QShowEvent>
#include <QTimer>

#include "../protocol/protocol_bridge.h"
#include "../protocol/media_cache.h"

#include "ui/platform/ui_utility_mac.h"
#include "ui/style/icon_provider.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/scroll_area.h"
#include "ui/empty_userpic.h"
#include "ui/painter.h"
#include "styles/style_dialogs.h"

namespace TeleMatrix {

namespace {
// Search bar height from dialogs.style (runtime-scaled).
#define kSearchBarHeight st::dialogsFilterHeight
constexpr int kSearchPopupRadius = 8;
constexpr int kSearchPopupShadowExtend = 10;
constexpr int kSearchPopupWidthSlack = 12;

/// Thin indeterminate progress bar (2px high, animated).
class FolderLoadingBar : public QWidget {
public:
    FolderLoadingBar(QWidget *parent) : QWidget(parent) {
        setFixedHeight(2);
        _timer.setInterval(16); // ~60fps
        connect(&_timer, &QTimer::timeout, this, [this] {
            _phase += 0.02;
            if (_phase > 1.0) _phase -= 1.0;
            update();
        });
    }
    void start() { _phase = 0; _timer.start(); show(); raise(); }
    void stop() { _timer.stop(); hide(); }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), Qt::transparent);
        const auto barW = width() / 3;
        const auto x = int(_phase * (width() + barW)) - barW;
        p.fillRect(QRect(x, 0, barW, 2), st::windowBgActive);
    }
private:
    QTimer _timer;
    double _phase = 0;
};

struct SearchPopupItem {
    QString label;
    QString avatarName;
    QString avatarUrl;
    QString entityId;
    bool useChatsIcon = false;
    bool checked = false;
};

[[nodiscard]] QString searchInPeerLabel(bool isDirect) {
    return isDirect ? QCoreApplication::translate("DialogsWidget", "This Chat")
                    : QCoreApplication::translate("DialogsWidget", "This Room");
}

[[nodiscard]] QString iconPathForDpr(const QString &basePath, qreal dpr) {
    auto path = basePath;
    const auto suffix = (dpr > 2.0) ? QStringLiteral("@3x")
        : (dpr > 1.0) ? QStringLiteral("@2x")
        : QString();
    if (!suffix.isEmpty()) {
        path.insert(path.lastIndexOf(QLatin1Char('.')), suffix);
    }
    return path;
}

[[nodiscard]] QImage loadMaskIcon(const QString &basePath, qreal dpr) {
    auto mask = QImage(iconPathForDpr(basePath, dpr));
    if (mask.isNull()) {
        mask = QImage(basePath);
    }
    if (!mask.isNull()) {
        mask.setDevicePixelRatio((dpr > 2.0) ? 3.0 : (dpr > 1.0) ? 2.0 : 1.0);
    }
    return mask.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

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

[[nodiscard]] ShadowTiles loadSearchPopupShadowTiles(qreal dpr) {
    static QHash<int, ShadowTiles> cache;
    const auto key = (dpr > 2.0) ? 3 : (dpr > 1.0) ? 2 : 1;
    if (const auto i = cache.constFind(key); i != cache.cend()) {
        return i.value();
    }

    auto load = [&](const QString &base) -> QImage {
        auto image = loadMaskIcon(base, dpr);
        if (image.isNull()) {
            return {};
        }
        return TeleMatrix::Style::IconProvider::colorizeMask(image, st::windowShadowFg);
    };

    ShadowTiles result;
    result.left = load(QStringLiteral(":/dialogs/shadow_left.png"));
    result.topLeft = load(QStringLiteral(":/dialogs/shadow_top_left.png"));
    result.top = load(QStringLiteral(":/dialogs/shadow_top.png"));
    result.bottomLeft = load(QStringLiteral(":/dialogs/shadow_bottom_left.png"));
    result.bottom = load(QStringLiteral(":/dialogs/shadow_bottom.png"));
    result.topRight = result.topLeft.flipped(Qt::Horizontal);
    result.topRight.setDevicePixelRatio(result.topLeft.devicePixelRatio());
    result.right = result.left.flipped(Qt::Horizontal);
    result.right.setDevicePixelRatio(result.left.devicePixelRatio());
    result.bottomRight = result.bottomLeft.flipped(Qt::Horizontal);
    result.bottomRight.setDevicePixelRatio(result.bottomLeft.devicePixelRatio());

    cache.insert(key, result);
    return result;
}

class DialogsInitialLoadingOverlay final : public Ui::RpWidget {
public:
    explicit DialogsInitialLoadingOverlay(QWidget *parent = nullptr)
    : Ui::RpWidget(parent) {
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        setAutoFillBackground(false);
    }

protected:
    void paintEvent(QPaintEvent *e) override {
        QPainter p(this);
        p.fillRect(e->rect(), st::windowBg);
        // Just the "Loading…" line, centred in the content area. The hamburger
        // stays where it always is — in the folder rail to the left, which this
        // overlay deliberately does NOT cover.
        p.setPen(st::windowSubTextFgOver);
        p.setFont(st::normalFont);
        p.drawText(rect(), Qt::AlignCenter,
            QCoreApplication::translate("DialogsWidget", "Loading..."));
    }
};

// The seam under the search bar, mirroring HistoryWidget's TopBarShadowWidget
// so both columns close their toolbar the same way.
class DialogsVerificationBanner final : public Ui::RpWidget {
public:
    explicit DialogsVerificationBanner(QWidget *parent = nullptr)
    : Ui::RpWidget(parent) {
        setMouseTracking(true);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
    }

    void setRequest(
        const QString &transactionId,
        const QString &deviceId,
        const QString &deviceName) {
        _transactionId = transactionId;
        _deviceId = deviceId;
        _deviceName = deviceName.trimmed().isEmpty() ? deviceId : deviceName.trimmed();
        _title = QCoreApplication::translate("DialogsVerificationBanner", "Verify a new device");
        // Method-neutral: a request advertises capabilities, not the method its
        // user picked, so naming one here promises something the other session
        // may never be looking at.
        _description = QCoreApplication::translate(
            "DialogsVerificationBanner",
            "A verification request was received from %1. "
            "Accept it to confirm this session is yours.")
            .arg(_deviceName.isEmpty()
                ? QCoreApplication::translate("DialogsVerificationBanner", "another session")
                : _deviceName);
        updateGeometry();
        update();
    }

    // Variant for an incoming CROSS-USER verification request (another person,
    // not our own device). `flowId` is reused as the transaction id.
    void setUserRequest(
        const QString &flowId,
        const QString &userId,
        const QString &displayName) {
        _transactionId = flowId;
        _deviceId.clear();
        _deviceName = displayName.trimmed().isEmpty()
            ? userId
            : displayName.trimmed();
        _title = QCoreApplication::translate("DialogsVerificationBanner", "Verify user");
        _description = QCoreApplication::translate(
            "DialogsVerificationBanner",
            "%1 wants to verify with you. "
            "Accept it to confirm their identity.")
            .arg(_deviceName.isEmpty()
                ? QCoreApplication::translate("DialogsVerificationBanner", "Someone")
                : _deviceName);
        updateGeometry();
        update();
    }

    [[nodiscard]] int contentHeight(int outerWidth) const {
        const auto available = qMax(1, outerWidth - 2 * padding());
        const auto titleH = QFontMetrics(st::semiboldFont).height();
        const auto descH = QFontMetrics(st::normalFont).boundingRect(
            QRect(0, 0, available, 1000),
            Qt::TextWordWrap,
            _description).height();
        return paddingTop()
            + titleH
            + titleSkip()
            + descH
            + buttonsSkip()
            + st::boxButtonHeight
            + paddingBottom();
    }

    void setAcceptedCallback(std::function<void()> callback) {
        _acceptedCallback = std::move(callback);
    }

    void setDeclinedCallback(std::function<void()> callback) {
        _declinedCallback = std::move(callback);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), st::historyPinnedBg);
        p.fillRect(
            0,
            height() - separatorHeight(),
            width(),
            separatorHeight(),
            st::shadowFg);

        const auto left = padding();
        const auto available = qMax(1, width() - 2 * padding());
        auto top = paddingTop();

        p.setPen(st::windowFg);
        p.setFont(st::semiboldFont);
        const auto titleH = QFontMetrics(st::semiboldFont).height();
        p.drawText(QRect(left, top, available, titleH), Qt::AlignLeft | Qt::AlignVCenter, _title);
        top += titleH + titleSkip();

        p.setPen(st::windowSubTextFg);
        p.setFont(st::normalFont);
        const auto descH = QFontMetrics(st::normalFont).boundingRect(
            QRect(0, 0, available, 1000),
            Qt::TextWordWrap,
            _description).height();
        p.drawText(
            QRect(left, top, available, descH),
            Qt::AlignLeft | Qt::TextWordWrap,
            _description);

        paintButton(
            p,
            acceptRect(),
            QCoreApplication::translate("DialogsVerificationBanner", "Accept"),
            true,
            _hovered == HoveredButton::Accept);
        paintButton(
            p,
            declineRect(),
            QCoreApplication::translate("DialogsVerificationBanner", "Decline"),
            false,
            _hovered == HoveredButton::Decline);
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        const auto hovered = buttonAt(event->pos());
        if (_hovered != hovered) {
            _hovered = hovered;
            setCursor(_hovered == HoveredButton::None ? Qt::ArrowCursor : Qt::PointingHandCursor);
            update();
        }
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton) {
            return;
        }
        switch (buttonAt(event->pos())) {
        case HoveredButton::Accept:
            if (_acceptedCallback) {
                _acceptedCallback();
            }
            break;
        case HoveredButton::Decline:
            if (_declinedCallback) {
                _declinedCallback();
            }
            break;
        case HoveredButton::None:
            break;
        }
    }

    void leaveEvent(QEvent *) override {
        if (_hovered != HoveredButton::None) {
            _hovered = HoveredButton::None;
            setCursor(Qt::ArrowCursor);
            update();
        }
    }

private:
    enum class HoveredButton {
        None,
        Accept,
        Decline,
    };

    [[nodiscard]] static int padding() {
        return st::dialogsFilterPadding.x();
    }

    [[nodiscard]] static int paddingTop() {
        return st::dialogsVerificationBannerPaddingTop;
    }

    [[nodiscard]] static int paddingBottom() {
        return st::dialogsVerificationBannerPaddingBottom;
    }

    [[nodiscard]] static int titleSkip() {
        return st::dialogsVerificationBannerTitleSkip;
    }

    [[nodiscard]] static int buttonsSkip() {
        return st::dialogsVerificationBannerButtonsSkip;
    }

    [[nodiscard]] static int separatorHeight() {
        return st::dialogsVerificationBannerSeparator;
    }

    [[nodiscard]] static int buttonTextPadding() {
        return st::dialogsVerificationBannerButtonTextPadding;
    }

    [[nodiscard]] static int buttonGap() {
        return st::dialogsVerificationBannerButtonGap;
    }

    [[nodiscard]] int acceptButtonWidth() const {
        return qMax(
            st::boxButtonMinWidth,
            QFontMetrics(st::boxButtonFont).horizontalAdvance(
                QCoreApplication::translate("DialogsVerificationBanner", "Accept"))
                + buttonTextPadding());
    }

    [[nodiscard]] int declineButtonWidth() const {
        return qMax(
            st::boxButtonMinWidth,
            QFontMetrics(st::boxButtonFont).horizontalAdvance(
                QCoreApplication::translate("DialogsVerificationBanner", "Decline"))
                + buttonTextPadding());
    }

    [[nodiscard]] QRect acceptRect() const {
        const auto top = height() - paddingBottom() - st::boxButtonHeight;
        return QRect(padding(), top, acceptButtonWidth(), st::boxButtonHeight);
    }

    [[nodiscard]] QRect declineRect() const {
        const auto a = acceptRect();
        return QRect(
            a.right() + 1 + buttonGap(),
            a.y(),
            declineButtonWidth(),
            st::boxButtonHeight);
    }

    [[nodiscard]] HoveredButton buttonAt(const QPoint &point) const {
        if (acceptRect().contains(point)) {
            return HoveredButton::Accept;
        } else if (declineRect().contains(point)) {
            return HoveredButton::Decline;
        }
        return HoveredButton::None;
    }

    void paintButton(
        QPainter &p,
        const QRect &rect,
        const QString &text,
        bool primary,
        bool hovered) const {
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        const auto bg = primary
            ? (hovered ? st::activeButtonBgOver : st::activeButtonBg)
            : (hovered ? st::attentionButtonBgRipple : st::attentionButtonBgOver);
        p.setBrush(bg);
        p.drawRoundedRect(rect, rect.height() / 2, rect.height() / 2);

        p.setFont(st::boxButtonFont);
        p.setPen(primary ? st::activeButtonFg : st::attentionButtonFg);
        p.drawText(rect, Qt::AlignCenter, text);
    }

    QString _transactionId;
    QString _deviceId;
    QString _deviceName;
    QString _title;
    QString _description;
    HoveredButton _hovered = HoveredButton::None;
    std::function<void()> _acceptedCallback;
    std::function<void()> _declinedCallback;
};

// "New login. Was this you?" alert, on the same sidebar strip as the
// verification banner: two pills, "Yes, it was me" and "No, sign out".
class DialogsNewLoginBanner final : public Ui::RpWidget {
public:
    explicit DialogsNewLoginBanner(QWidget *parent = nullptr)
    : Ui::RpWidget(parent) {
        setMouseTracking(true);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
    }

    void setLogin(
            const QString &deviceId,
            const QString &displayName,
            const QString &lastSeenIp,
            qint64 lastSeenTs) {
        _deviceId = deviceId;
        const auto name = displayName.trimmed().isEmpty()
            ? deviceId
            : displayName.trimmed();
        _title = QCoreApplication::translate(
            "DialogsNewLoginBanner", "New login. Was this you?");
        auto parts = QStringList{ name };
        if (!lastSeenIp.trimmed().isEmpty()) {
            parts << lastSeenIp.trimmed();
        }
        if (lastSeenTs > 0) {
            parts << QDateTime::fromSecsSinceEpoch(lastSeenTs)
                .toString(QStringLiteral("d MMM yyyy, HH:mm"));
        }
        _description = parts.join(QStringLiteral(" · "));
        updateGeometry();
        update();
    }

    [[nodiscard]] const QString &deviceId() const {
        return _deviceId;
    }

    [[nodiscard]] int contentHeight(int outerWidth) const {
        const auto available = qMax(1, outerWidth - 2 * padding());
        const auto titleH = QFontMetrics(st::semiboldFont).height();
        const auto descH = QFontMetrics(st::normalFont).boundingRect(
            QRect(0, 0, available, 1000),
            Qt::TextWordWrap,
            _description).height();
        return paddingTop()
            + titleH
            + titleSkip()
            + descH
            + buttonsSkip()
            + st::boxButtonHeight
            + paddingBottom();
    }

    void setYesCallback(std::function<void()> callback) {
        _yesCallback = std::move(callback);
    }

    void setSignOutCallback(std::function<void()> callback) {
        _signOutCallback = std::move(callback);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), st::historyPinnedBg);
        p.fillRect(
            0,
            height() - separatorHeight(),
            width(),
            separatorHeight(),
            st::shadowFg);

        const auto left = padding();
        const auto available = qMax(1, width() - 2 * padding());
        auto top = paddingTop();

        p.setPen(st::windowFg);
        p.setFont(st::semiboldFont);
        const auto titleH = QFontMetrics(st::semiboldFont).height();
        p.drawText(QRect(left, top, available, titleH), Qt::AlignLeft | Qt::AlignVCenter, _title);
        top += titleH + titleSkip();

        p.setPen(st::windowSubTextFg);
        p.setFont(st::normalFont);
        const auto descH = QFontMetrics(st::normalFont).boundingRect(
            QRect(0, 0, available, 1000),
            Qt::TextWordWrap,
            _description).height();
        p.drawText(
            QRect(left, top, available, descH),
            Qt::AlignLeft | Qt::TextWordWrap,
            _description);

        paintPill(
            p,
            yesRect(),
            QCoreApplication::translate("DialogsNewLoginBanner", "Yes, it was me"),
            true,
            _hovered == Hovered::Yes);
        paintPill(
            p,
            signOutRect(),
            QCoreApplication::translate("DialogsNewLoginBanner", "No, sign out"),
            false,
            _hovered == Hovered::SignOut);
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        const auto hovered = regionAt(event->pos());
        if (_hovered != hovered) {
            _hovered = hovered;
            setCursor(_hovered == Hovered::None ? Qt::ArrowCursor : Qt::PointingHandCursor);
            update();
        }
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton) {
            return;
        }
        switch (regionAt(event->pos())) {
        case Hovered::Yes:
            if (_yesCallback) {
                _yesCallback();
            }
            break;
        case Hovered::SignOut:
            if (_signOutCallback) {
                _signOutCallback();
            }
            break;
        case Hovered::None:
            break;
        }
    }

    void leaveEvent(QEvent *) override {
        if (_hovered != Hovered::None) {
            _hovered = Hovered::None;
            setCursor(Qt::ArrowCursor);
            update();
        }
    }

private:
    enum class Hovered {
        None,
        Yes,
        SignOut,
    };

    [[nodiscard]] static int padding() {
        return st::dialogsFilterPadding.x();
    }

    [[nodiscard]] static int paddingTop() {
        return st::dialogsVerificationBannerPaddingTop;
    }

    [[nodiscard]] static int paddingBottom() {
        return st::dialogsVerificationBannerPaddingBottom;
    }

    [[nodiscard]] static int titleSkip() {
        return st::dialogsVerificationBannerTitleSkip;
    }

    [[nodiscard]] static int buttonsSkip() {
        return st::dialogsVerificationBannerButtonsSkip;
    }

    [[nodiscard]] static int separatorHeight() {
        return st::dialogsVerificationBannerSeparator;
    }

    [[nodiscard]] static int buttonTextPadding() {
        return st::dialogsVerificationBannerButtonTextPadding;
    }

    [[nodiscard]] static int buttonGap() {
        return st::dialogsVerificationBannerButtonGap;
    }

    [[nodiscard]] static int pillWidth(const QString &text) {
        return qMax(
            st::boxButtonMinWidth,
            QFontMetrics(st::boxButtonFont).horizontalAdvance(text)
                + buttonTextPadding());
    }

    // Bottom-anchored so the const hit-test rects match the painter without
    // recomputing the wrapped description height.
    [[nodiscard]] int pillRowTop() const {
        return height() - paddingBottom() - st::boxButtonHeight;
    }

    [[nodiscard]] QRect yesRect() const {
        const auto w = pillWidth(QCoreApplication::translate(
            "DialogsNewLoginBanner", "Yes, it was me"));
        return QRect(padding(), pillRowTop(), w, st::boxButtonHeight);
    }

    [[nodiscard]] QRect signOutRect() const {
        const auto a = yesRect();
        const auto w = pillWidth(QCoreApplication::translate(
            "DialogsNewLoginBanner", "No, sign out"));
        return QRect(a.right() + 1 + buttonGap(), a.y(), w, st::boxButtonHeight);
    }

    [[nodiscard]] Hovered regionAt(const QPoint &point) const {
        if (yesRect().contains(point)) {
            return Hovered::Yes;
        } else if (signOutRect().contains(point)) {
            return Hovered::SignOut;
        }
        return Hovered::None;
    }

    void paintPill(
            QPainter &p,
            const QRect &rect,
            const QString &text,
            bool primary,
            bool hovered) const {
        PainterHighQualityEnabler hq(p);
        p.setPen(Qt::NoPen);
        const auto bg = primary
            ? (hovered ? st::activeButtonBgOver : st::activeButtonBg)
            : (hovered ? st::attentionButtonBgRipple : st::attentionButtonBgOver);
        p.setBrush(bg);
        p.drawRoundedRect(rect, rect.height() / 2, rect.height() / 2);

        p.setFont(st::boxButtonFont);
        p.setPen(primary ? st::activeButtonFg : st::attentionButtonFg);
        p.drawText(rect, Qt::AlignCenter, text);
    }

    QString _deviceId;
    QString _title;
    QString _description;
    Hovered _hovered = Hovered::None;
    std::function<void()> _yesCallback;
    std::function<void()> _signOutCallback;
};

void paintSearchPopupShadow(QPainter &p, const QRect &bodyRect, const ShadowTiles &tiles) {
    const auto ratio = tiles.topLeft.devicePixelRatio();
    auto logicalWidth = [ratio](const QImage &image) {
        return qRound(image.width() / ratio);
    };
    auto logicalHeight = [ratio](const QImage &image) {
        return qRound(image.height() / ratio);
    };

    const auto tlW = logicalWidth(tiles.topLeft);
    const auto tlH = logicalHeight(tiles.topLeft);
    const auto trW = logicalWidth(tiles.topRight);
    const auto trH = logicalHeight(tiles.topRight);
    const auto blW = logicalWidth(tiles.bottomLeft);
    const auto blH = logicalHeight(tiles.bottomLeft);
    const auto brW = logicalWidth(tiles.bottomRight);
    const auto brH = logicalHeight(tiles.bottomRight);
    const auto leftW = logicalWidth(tiles.left);
    const auto rightW = logicalWidth(tiles.right);
    const auto topH = logicalHeight(tiles.top);
    const auto bottomH = logicalHeight(tiles.bottom);

    {
        auto from = bodyRect.y();
        auto to = bodyRect.y() + bodyRect.height();
        p.drawImage(bodyRect.x() - kSearchPopupShadowExtend, bodyRect.y() - kSearchPopupShadowExtend, tiles.topLeft);
        from += tlH - kSearchPopupShadowExtend;
        p.drawImage(
            bodyRect.x() - kSearchPopupShadowExtend,
            bodyRect.y() + bodyRect.height() + kSearchPopupShadowExtend - blH,
            tiles.bottomLeft);
        to -= blH - kSearchPopupShadowExtend;
        if (to > from) {
            p.drawImage(
                QRect(bodyRect.x() - kSearchPopupShadowExtend, from, leftW, to - from),
                tiles.left,
                QRect(0, 0, tiles.left.width(), tiles.left.height()));
        }
    }

    {
        auto from = bodyRect.y();
        auto to = bodyRect.y() + bodyRect.height();
        p.drawImage(
            bodyRect.x() + bodyRect.width() + kSearchPopupShadowExtend - trW,
            bodyRect.y() - kSearchPopupShadowExtend,
            tiles.topRight);
        from += trH - kSearchPopupShadowExtend;
        p.drawImage(
            bodyRect.x() + bodyRect.width() + kSearchPopupShadowExtend - brW,
            bodyRect.y() + bodyRect.height() + kSearchPopupShadowExtend - brH,
            tiles.bottomRight);
        to -= brH - kSearchPopupShadowExtend;
        if (to > from) {
            p.drawImage(
                QRect(
                    bodyRect.x() + bodyRect.width() + kSearchPopupShadowExtend - rightW,
                    from,
                    rightW,
                    to - from),
                tiles.right,
                QRect(0, 0, tiles.right.width(), tiles.right.height()));
        }
    }

    {
        auto from = bodyRect.x() + tlW - kSearchPopupShadowExtend;
        auto to = bodyRect.x() + bodyRect.width() - (trW - kSearchPopupShadowExtend);
        if (to > from) {
            p.drawImage(
                QRect(from, bodyRect.y() - kSearchPopupShadowExtend, to - from, topH),
                tiles.top,
                QRect(0, 0, tiles.top.width(), tiles.top.height()));
        }
    }

    {
        auto from = bodyRect.x() + blW - kSearchPopupShadowExtend;
        auto to = bodyRect.x() + bodyRect.width() - (brW - kSearchPopupShadowExtend);
        if (to > from) {
            p.drawImage(
                QRect(
                    from,
                    bodyRect.y() + bodyRect.height() + kSearchPopupShadowExtend - bottomH,
                    to - from,
                    bottomH),
                tiles.bottom,
                QRect(0, 0, tiles.bottom.width(), tiles.bottom.height()));
        }
    }
}

class SearchScopePopup final : public QWidget {
public:
    explicit SearchScopePopup(QWidget *parent = nullptr)
        : QWidget(parent)
        , _dpr(devicePixelRatioF())
        , _shadowTiles(loadSearchPopupShadowTiles(_dpr))
        , _checkIcon(TeleMatrix::Style::IconProvider::colorizeMask(
            loadMaskIcon(QStringLiteral(":/dialogs/player_check.png"), _dpr),
            st::windowBgActive))
        , _chatsIcon(TeleMatrix::Style::IconProvider::colorizeMask(
            loadMaskIcon(QStringLiteral(":/dialogs/menu_chats.png"), _dpr),
            st::menuIconColor))
    {
        setWindowFlags(
            Qt::FramelessWindowHint
            | Qt::BypassWindowManagerHint
            | Qt::Popup
            | Qt::NoDropShadowWindowHint
        );
        setAttribute(Qt::WA_DeleteOnClose);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setMouseTracking(true);
    }

    void setItems(QVector<SearchPopupItem> items) {
        _items = std::move(items);
        recalculateGeometry();
    }

    void setTriggeredCallback(std::function<void(int)> callback) {
        _onTriggered = std::move(callback);
    }

    void popupAtRow(const QPoint &rowTopGlobal, int selectedIndex, int positionIndex) {
        if (_items.isEmpty()) {
            return;
        }

        recalculateGeometry();
        const auto clampedSelectedIndex = qBound(0, selectedIndex, _items.size() - 1);
        const auto clampedPositionIndex = qBound(0, positionIndex, _items.size() - 1);
        setHovered(clampedSelectedIndex);

        // Position the search-scope menu:
        // 1. choose TopLeft / BottomLeft based on the row the popup is anchored to,
        // 2. prepare geometry for that origin,
        // 3. shift by inner.x() and anchorIndex * rowHeight.
        const auto count = _items.size();
        const auto rowHeight = st::dialogsSearchInHeight;
        const auto bottomLeft = (clampedPositionIndex * 2 >= count);
        const auto anchor = rowTopGlobal + QPoint(0, bottomLeft ? count * rowHeight : 0);

        auto widgetPos = anchor - QPoint(0, kSearchPopupShadowExtend);
        if (bottomLeft) {
            widgetPos.setY(anchor.y() - height() + kSearchPopupShadowExtend);
        }

        if (const auto *screen = QGuiApplication::screenAt(anchor)) {
            const auto available = screen->availableGeometry();
            if (widgetPos.x() + width() - kSearchPopupShadowExtend > available.right() + 1) {
                widgetPos.setX(available.right() + 1 + kSearchPopupShadowExtend - width());
            }
            if (widgetPos.x() + kSearchPopupShadowExtend < available.left()) {
                widgetPos.setX(available.left() - kSearchPopupShadowExtend);
            }
            if (widgetPos.y() + height() - kSearchPopupShadowExtend > available.bottom() + 1) {
                widgetPos.setY(available.bottom() + 1 + kSearchPopupShadowExtend - height());
            }
            if (widgetPos.y() + kSearchPopupShadowExtend < available.top()) {
                widgetPos.setY(available.top() - kSearchPopupShadowExtend);
            }
        }

        widgetPos -= QPoint(_bodyRect.x(), clampedPositionIndex * rowHeight);
        move(widgetPos);
        show();
        raise();
        activateWindow();
        setFocus(Qt::PopupFocusReason);
        qApp->installEventFilter(this);
        Platform::AcceptAllMouseInput(this);
        updateCursor();
    }

protected:
    bool eventFilter(QObject *obj [[maybe_unused]], QEvent *event) override {
        if (event->type() == QEvent::MouseButtonPress) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (!geometry().contains(mouse->globalPosition().toPoint())) {
                hide();
            }
        } else if (event->type() == QEvent::ApplicationDeactivate) {
            hide();
        }
        return false;
    }

    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        paintSearchPopupShadow(p, _bodyRect, _shadowTiles);

        QPainterPath clipPath;
        clipPath.addRoundedRect(
            QRectF(_bodyRect).adjusted(0.5, 0.5, -0.5, -0.5),
            kSearchPopupRadius,
            kSearchPopupRadius);
        p.fillPath(clipPath, st::menuBg);
        p.setClipPath(clipPath);

        p.setFont(st::semiboldFont);
        const auto metrics = QFontMetrics(st::semiboldFont);
        const auto photoLeft = st::dialogsSearchInPhotoPadding;
        const auto photoSize = st::dialogsSearchInPhotoSize;
        const auto textX = photoLeft + photoSize + st::dialogsSearchInSkip;
        const auto checkWidth = _checkIcon.isNull()
            ? 0
            : qRound(_checkIcon.width() / _checkIcon.devicePixelRatio());
        const auto available = _bodyRect.width()
            - textX
            - st::dialogsSearchInCheckSkip
            - checkWidth
            - st::dialogsSearchInCheckSkip;

        for (int i = 0; i < _items.size(); ++i) {
            const auto &item = _items[i];
            const auto rowRect = QRect(
                _bodyRect.x(),
                _bodyRect.y() + i * st::dialogsSearchInHeight,
                _bodyRect.width(),
                st::dialogsSearchInHeight);
            p.fillRect(rowRect, (i == _hoveredIndex) ? st::menuBgOver : st::menuBg);

            const auto avatarRect = QRect(
                rowRect.x() + photoLeft,
                rowRect.y() + (rowRect.height() - photoSize) / 2,
                photoSize,
                photoSize);
            if (item.useChatsIcon) {
                if (!_chatsIcon.isNull()) {
                    const auto iconW = qRound(_chatsIcon.width() / _chatsIcon.devicePixelRatio());
                    const auto iconH = qRound(_chatsIcon.height() / _chatsIcon.devicePixelRatio());
                    const auto iconX = avatarRect.x() + (avatarRect.width() - iconW) / 2;
                    const auto iconY = avatarRect.y() + (avatarRect.height() - iconH) / 2;
                    p.drawImage(QPoint(iconX, iconY), _chatsIcon);
                }
			} else if (!item.avatarUrl.isEmpty()) {
				const auto avatar = MediaCache::loadAvatarPixmapAsync(
					item.avatarUrl,
					photoSize,
					_dpr,
					this,
					avatarRect);
				if (!avatar.isNull()) {
					p.drawPixmap(avatarRect.topLeft(), avatar);
				} else {
					paintFallbackAvatar(p, avatarRect, item.avatarName, item.entityId);
				}
            } else {
                paintFallbackAvatar(p, avatarRect, item.avatarName, item.entityId);
            }

            p.setPen(st::windowFg);
            const auto elided = metrics.elidedText(item.label, Qt::ElideRight, available);
            p.drawText(
                rowRect.x() + textX,
                rowRect.y() + st::dialogsSearchInNameTop + metrics.ascent(),
                elided);

            if (item.checked && !_checkIcon.isNull()) {
                const auto iconW = qRound(_checkIcon.width() / _checkIcon.devicePixelRatio());
                const auto iconH = qRound(_checkIcon.height() / _checkIcon.devicePixelRatio());
                const auto iconX = rowRect.x() + rowRect.width() - st::dialogsSearchInCheckSkip - iconW;
                const auto iconY = rowRect.y() + (rowRect.height() - iconH) / 2;
                p.drawImage(QPoint(iconX, iconY), _checkIcon);
            }
        }
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        setHovered(itemIndexAt(e->pos()));
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton) {
            return;
        }
        const auto index = itemIndexAt(e->position().toPoint());
        if (index >= 0) {
            hide();
            if (_onTriggered) {
                _onTriggered(index);
            }
        } else {
            hide();
        }
    }

    void leaveEvent(QEvent *e) override {
        QWidget::leaveEvent(e);
        setHovered(-1);
    }

    void hideEvent(QHideEvent *e) override {
        QWidget::hideEvent(e);
        qApp->removeEventFilter(this);
        Platform::ForceArrowCursor();
    }

    void keyPressEvent(QKeyEvent *e) override {
        switch (e->key()) {
        case Qt::Key_Escape:
            hide();
            break;
        case Qt::Key_Up:
            navigate(-1);
            break;
        case Qt::Key_Down:
            navigate(1);
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (_hoveredIndex >= 0 && _hoveredIndex < _items.size()) {
                hide();
                if (_onTriggered) {
                    _onTriggered(_hoveredIndex);
                }
            }
            break;
        default:
            QWidget::keyPressEvent(e);
        }
    }

private:
    void recalculateGeometry() {
        const auto metrics = QFontMetricsF(st::semiboldFont);
        auto maxTextWidth = 0;
        for (const auto &item : _items) {
            maxTextWidth = qMax(maxTextWidth, qCeil(metrics.horizontalAdvance(item.label)));
        }
        const auto checkWidth = _checkIcon.isNull()
            ? 0
            : qRound(_checkIcon.width() / _checkIcon.devicePixelRatio());
        const auto bodyWidth = st::dialogsSearchInPhotoPadding
            + st::dialogsSearchInPhotoSize
            + st::dialogsSearchInSkip
            + maxTextWidth
            + st::dialogsSearchInCheckSkip
            + checkWidth
            + st::dialogsSearchInCheckSkip
            + kSearchPopupWidthSlack;
        const auto bodyHeight = _items.size() * st::dialogsSearchInHeight;
        _bodyRect = QRect(
            kSearchPopupShadowExtend,
            kSearchPopupShadowExtend,
            bodyWidth,
            bodyHeight);
        setFixedSize(
            bodyWidth + 2 * kSearchPopupShadowExtend,
            bodyHeight + 2 * kSearchPopupShadowExtend);
    }

    void paintFallbackAvatar(QPainter &p, const QRect &rect, const QString &name, const QString &entityId) const {
        Ui::EmptyUserpic::paint(p, entityId.isEmpty() ? name : entityId, name, rect.x(), rect.y(), rect.width());
    }

    [[nodiscard]] int itemIndexAt(const QPoint &position) const {
        if (!_bodyRect.contains(position)) {
            return -1;
        }
        const auto relativeY = position.y() - _bodyRect.y();
        const auto index = relativeY / st::dialogsSearchInHeight;
        return (index >= 0 && index < _items.size()) ? index : -1;
    }

    void setHovered(int index) {
        if (_hoveredIndex == index) {
            return;
        }
        _hoveredIndex = index;
        updateCursor();
        update();
    }

    void updateCursor() {
        if (_hoveredIndex >= 0) {
            setCursor(Qt::PointingHandCursor);
            Platform::ForcePointingHandCursor();
        } else {
            setCursor(Qt::ArrowCursor);
            Platform::ForceArrowCursor();
        }
    }

    void navigate(int delta) {
        if (_items.isEmpty()) {
            return;
        }
        auto index = _hoveredIndex;
        if (index < 0) {
            index = (delta > 0) ? 0 : (_items.size() - 1);
        } else {
            index = (index + delta + _items.size()) % _items.size();
        }
        setHovered(index);
    }

    QVector<SearchPopupItem> _items;
    QRect _bodyRect;
    qreal _dpr = 1.0;
    ShadowTiles _shadowTiles;
    QImage _checkIcon;
    QImage _chatsIcon;
    int _hoveredIndex = -1;
    std::function<void(int)> _onTriggered;
};

/// Painted icon button for search filter actions.
class SearchFilterButton : public QWidget {
public:
    SearchFilterButton(const QString &iconPath, QWidget *parent)
        : QWidget(parent)
    {
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);

        // Load the search-from icon from resource.
        // Pick the best resolution for the screen DPR.
        const auto dpr = devicePixelRatioF();
        const auto suffix = (dpr > 2.0) ? QStringLiteral("@3x")
                          : (dpr > 1.0) ? QStringLiteral("@2x")
                          : QString();
        auto path = iconPath;
        if (!suffix.isEmpty()) {
            path.insert(path.lastIndexOf(QLatin1Char('.')), suffix);
        }
        _iconBase = QImage(path);
        if (!_iconBase.isNull()) {
            _iconBase = _iconBase.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            // The SPRITE's scale, not the screen's: only 1x/2x/3x assets exist, so
            // at a fractional ratio (Windows at 125%/150%) tagging the @2x file
            // 1.25 would draw it 1.6x oversized.
            _iconBase.setDevicePixelRatio((dpr > 2.0) ? 3 : (dpr > 1.0) ? 2 : 1);
        }

        setFixedSize(st::dialogsSearchFromWidth, st::dialogsSearchFilterHeight);
    }

    void setClickCallback(std::function<void()> cb) {
        _onClick = std::move(cb);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        if (_iconBase.isNull()) {
            return;
        }

        // Menu icon foreground: #999999 idle / #8a8a8a on hover.
        const auto &color = _hovered ? st::menuIconFgOver : st::menuIconFg;

        // Source icons are white-on-black RGB (no alpha).
        // Use luminance as alpha mask and apply the desired color.
        // Write premultiplied values (format is ARGB32_Premultiplied).
        QImage tinted = _iconBase;
        const auto r = color.red();
        const auto g = color.green();
        const auto b = color.blue();
        for (int y = 0; y < tinted.height(); ++y) {
            auto *line = reinterpret_cast<QRgb *>(tinted.scanLine(y));
            for (int x = 0; x < tinted.width(); ++x) {
                const auto a = qGray(line[x]);
                line[x] = qRgba(r * a / 255, g * a / 255, b * a / 255, a);
            }
        }
        // tinted inherits _iconBase's sprite dpr.

        // Center the icon in the button area.
        const auto iconW = tinted.width() / tinted.devicePixelRatio();
        const auto iconH = tinted.height() / tinted.devicePixelRatio();
        const auto dx = (width() - iconW) / 2.0;
        const auto dy = (height() - iconH) / 2.0;

        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.drawImage(QPointF(dx, dy), tinted);
    }

    void enterEvent(QEnterEvent *) override {
        _hovered = true;
        update();
    }

    void leaveEvent(QEvent *) override {
        _hovered = false;
        update();
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton && _onClick) {
            _onClick();
        }
    }

private:
    QImage _iconBase;
    bool _hovered = false;
    std::function<void()> _onClick;
};

} // namespace

QString filterDisplayName(int filterId) {
    switch (filterId) {
    case 0:
        return QCoreApplication::translate("DialogsWidget", "All");
    case 1:
        return QCoreApplication::translate("DialogsWidget", "Personal");
    case 2:
        return QCoreApplication::translate("DialogsWidget", "Unread");
    default:
        return QCoreApplication::translate("DialogsWidget", "Folder %1").arg(filterId);
    }
}

bool DialogsWidget::isDefaultSearchActive() const {
    return !isMessageSearchActive()
        && _search
        && !_search->getLastText().trimmed().isEmpty();
}

bool DialogsWidget::isServerSearchActive() const {
    return isMessageSearchActive() || isDefaultSearchActive();
}

bool DialogsWidget::hasActiveSearch() const {
    return isMessageSearchActive()
        || (_search && !_search->getLastText().trimmed().isEmpty());
}

DialogsWidget::DialogsWidget(
    QWidget *parent,
    AppController *controller,
    ProtocolBridge *bridge)
    : Ui::RpWidget(parent)
    , _controller(controller)
    , _bridge(bridge)
    , _unreadStateStore(controller ? controller->unreadStateStore() : nullptr)
{
    // Ensure white background even in macOS dark mode.
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, st::windowBg);
    setPalette(pal);

    setupSearchBar();
    setupVerificationBanner();
    setupNewLoginBanner();


    // Refresh colors when theme changes (day/night toggle).
    if (auto *tm = _controller->themeManager()) {
        QObject::connect(tm, &Theme::ThemeManager::themeChanged,
                this, [this](bool /*isNight*/, Theme::ThemeMode /*mode*/) {
            // Refresh own background palette.
            QPalette p = palette();
            p.setColor(QPalette::Window, st::windowBg);
            setPalette(p);
            // Refresh search field style with current st:: colors.
            if (_search) {
                _search->refreshStyle(st::dialogsFilter);
            }
            // Force full repaint of all children with fresh theme colors.
            const auto children = findChildren<QWidget *>();
            for (auto *child : children) {
                child->update();
            }
            update();
        });
    }
    // "Include muted chats in folders counters" toggle: recompute the folder badges live.
    if (_controller) {
        connect(_controller, &AppController::includeMutedInFoldersChanged,
                this, [this] { rebuildFilterCountersFromRows(); });
    }

    setupFilterButtons();
    setupFilterSidebar();
    setupScrollArea();

    // After setupScrollArea(): checkUpdateStatus() lays the controls out, and a
    // payload downloaded before this rooms list existed (a re-login under
    // auto-download) makes it build the bar right here.
    if (auto *update = _controller ? _controller->updateService() : nullptr) {
        // updateReady is the only signal that can raise the bar; the others can
        // only take it away — a newer version clears the stale ready path, and
        // a failed apply drops it.
        connect(update, &Core::UpdateService::updateReady,
                this, [this](const QString &) { checkUpdateStatus(); });
        connect(update, &Core::UpdateService::updateAvailable,
                this, [this](const QString &) { checkUpdateStatus(); });
        connect(update, &Core::UpdateService::updateUpToDate,
                this, [this] { checkUpdateStatus(); });
        connect(update, &Core::UpdateService::updateError,
                this, [this](const QString &) { checkUpdateStatus(); });
        connect(update, &Core::UpdateService::applyStarted,
                this, [this] { checkUpdateStatus(); });
        connect(update, &Core::UpdateService::downloadStarted, this, [this] {
            _downloadPercent = -1;
            checkUpdateStatus();
        });
        connect(update, &Core::UpdateService::downloadCancelled,
                this, [this] { checkUpdateStatus(); });
        connect(update, &Core::UpdateService::updateProgress, this,
                [this](quint64 received, quint64 total) {
            const auto percent = (total > 0)
                ? int((received * 100) / total)
                : -1;
            if (percent == _downloadPercent) {
                return;
            }
            _downloadPercent = percent;
            // Straight to the bar, not through checkUpdateStatus(): progress
            // fires per chunk and re-deriving the whole state that often is waste.
            auto *service = _controller->updateService();
            if (service
                && _updateTelegram
                && _updateTelegram->mode() == DialogsUpdateBar::Mode::Downloading) {
                _updateTelegram->setDownloadingMode(
                    tr("Downloading %1…").arg(service->availableVersion()),
                    _downloadPercent);
            }
        });
        checkUpdateStatus();
    }

    if (_unreadStateStore) {
        QObject::connect(
            _unreadStateStore,
            &UnreadStateStore::roomUnreadStateChanged,
            this,
            [this](const QString &roomId) {
                applyUnreadStateToRoom(roomId);
                rebuildFilterCountersFromRows();
            });
    }
    QObject::connect(_bridge, &ProtocolBridge::roomsReady, this,
        [this](quint64 requestId, bool success, const QVector<RoomSummary> &rooms) {
            if (requestId != _latestRoomsRequestId) {
                return;
            }
            _roomsRefreshInFlight = false;
            if (success) {
                applyRoomsResult(rooms);
            }
            if (_roomsRefreshQueued) {
                _roomsRefreshQueued = false;
                refreshRooms();
            }
        });
    QObject::connect(_bridge, &ProtocolBridge::customFoldersReady,
        this, [this](bool success, const QVector<FolderInfo> &) {
            if (success) {
                _customFoldersLoaded = true;
                // The "Edit folders" button appears at reveal, not now — during
                // loading the rail must show only its hamburger.
                // Folders and their order travel together; refresh the unified
                // order too (cheap — reads the Rust cache) so a remote reorder
                // that refetches folders also updates the rail order.
                if (_bridge) {
                    _bridge->getSidebarOrderAsync();
                }
                // Rebuild the rail synchronously from the fresh cache so the
                // reveal (below) shows the final arrangement, then refresh the
                // room list for good measure (a no-op behind the overlay).
                rebuildSidebar();
                maybeRevealInitialContent();
                refreshRooms();
            }
        });

    // Subscribe to room list changes from the bridge.
    QObject::connect(_bridge, &ProtocolBridge::roomListChanged, this, [this]() {
        // Debounce: coalesce rapid-fire room list updates into one refresh.
        // The Rust sync fires roomListChanged for every timeline update on
        // every room; debounce so async room-list refreshes stay coalesced.
        if (!_refreshRoomsTimer) {
            _refreshRoomsTimer = new QTimer(this);
            _refreshRoomsTimer->setSingleShot(true);
            _refreshRoomsTimer->setInterval(2000); // 2s debounce for ongoing sync
            connect(_refreshRoomsTimer, &QTimer::timeout, this, [this] {
                // Re-fetch folders too: a section created/renamed in Element lands
                // in its account-data settings and reaches us via the account-data
                // handler → this callback. get_folders reads the cached section
                // registry (cheap), but throttle anyway to avoid redundant calls.
                const auto nowMs = QDateTime::currentMSecsSinceEpoch();
                if (_bridge && (nowMs - _lastFolderRefetchMs) > 5000) {
                    _lastFolderRefetchMs = nowMs;
                    _bridge->getCustomFoldersAsync();
                    // Joined spaces likewise appear/leave via sync (e.g. a space
                    // created in Element, or one that only just loaded into the
                    // sliding-sync window), so re-fetch them on the same throttle.
                    _bridge->getJoinedSpacesAsync();
                }
                refreshRooms();
            });
        }
        // If room list is empty, refresh immediately (first populate).
        // Otherwise debounce to avoid blocking during continuous sync updates.
        // Use shorter debounce during initial sync (first 10s) for faster
        // last-message population, then switch to longer debounce.
        const bool listEmpty = !_inner || _inner->allRows().isEmpty();
        if (listEmpty) {
            if (_refreshRoomsTimer) _refreshRoomsTimer->stop();
            refreshRooms();
        } else {
            if (!_refreshRoomsTimer->isActive()) {
                // After initial burst, use longer debounce.
                const auto elapsed = _startupTime.msecsTo(QDateTime::currentDateTime());
                _refreshRoomsTimer->setInterval(elapsed < 10000 ? 300 : 2000);
                _refreshRoomsTimer->start();
            }
        }
        // Reset the loading bar settle timer — hide after 3s of no updates. But
        // never auto-hide while a folder operation is explicitly in flight; that
        // preloader is driven by updateFolderLoadingBar().
        if (_folderLoadingBar && _folderLoadingBar->isVisible()
            && !hasPendingFolderWork()) {
            if (!_loadingSettleTimer) {
                _loadingSettleTimer = new QTimer(this);
                _loadingSettleTimer->setSingleShot(true);
                _loadingSettleTimer->setInterval(3000);
                connect(_loadingSettleTimer, &QTimer::timeout, this, [this] {
                    hideFolderLoading();
                });
            }
            _loadingSettleTimer->start(); // restart on each signal
        }

        // When cached startup rooms are shown, keep their ordering stable
        // until the authoritative initial dialogs load reaches Ready.
        if (!_hasCachedStartupRooms
            && _initialDialogsLoadState != InitialDialogsLoadState::Ready) {
            applyInitialDialogsLoadState(InitialDialogsLoadState::Ready);
        }
    });
    QObject::connect(
        _bridge,
        &ProtocolBridge::initialDialogsLoadStateChanged,
        this,
        [this](InitialDialogsLoadState state) {
            applyInitialDialogsLoadState(state);
        });
    // When sync reaches SYNCED (state 2): dismiss the loading overlay even if no
    // rooms exist yet (e.g. fresh account), run the one-time folder migration,
    // and fetch custom folders.
    QObject::connect(_bridge, &ProtocolBridge::syncStateChanged, this, [this](int state) {
        if (state == 2) {
            handleSyncSynced();
        }
    });
    // Sync can reach SYNCED while the user is still on the intro/verification
    // screens, before this widget exists. syncStateChanged is edge-only and is
    // not replayed to late subscribers, so replay the SYNCED work once if we
    // already passed it — otherwise a fresh sign-in never fetches custom folders
    // (nor runs the one-time migration). Queued so it runs after construction,
    // exactly as a real signal would have arrived.
    if (_bridge->syncState() == 2) {
        QMetaObject::invokeMethod(this, [this] { handleSyncSynced(); },
            Qt::QueuedConnection);
    }

	// Receive search results for dialogs-wide and in-room message search.
	QObject::connect(_bridge, &ProtocolBridge::searchPageReady, this, [this](const SearchPage &page) {
		if (page.requestId != _activeSearchRequestId) {
			return; // stale result
		}
        if (_inner && isServerSearchActive()) {
            _inner->setSearchLoading(false);
            if (_searchLoadingMore) {
                // Pagination: append to existing results.
                _inner->appendSearchResults(page);
                _searchLoadingMore = false;
            } else {
                _inner->setSearchResults(page);
            }
            _searchNextToken = page.nextToken;
            _searchDone = page.done || page.nextToken.isEmpty();
        }
    });
    QObject::connect(_bridge, &ProtocolBridge::searchFailed, this, [this](quint64 requestId, const QString &) {
        if (requestId != _activeSearchRequestId) {
            return;
        }
        _searchLoadingMore = false;
        _searchDone = true;
        _searchNextToken.clear();
        if (_inner && isServerSearchActive()) {
            _inner->setSearchLoading(false);
        }
    });
    QObject::connect(
        _bridge,
        &ProtocolBridge::roomMembersReady,
        this,
        [this](const QString &roomId, const QVector<UserProfile> &members) {
            if (roomId != _pendingMemberPickerRoomId) {
                return;
            }
            // The picker is already on screen (opened by showMemberPicker) in its loading state —
            // just fill it in now that the async fetch has landed.
            _memberPickerMembers = members;
            if (_memberPicker) {
                _memberPicker->setMembers(members);
            }
        });

    // The server-synced unified rail order (folders + spaces). Apply it on load so
    // a dragged interleaving persists; a live drag's optimistic order takes priority.
    QObject::connect(_bridge, &ProtocolBridge::sidebarOrderReady, this,
        [this](bool success, const QVector<SidebarEntry> &order) {
            if (success) {
                _sidebarOrderLoaded = true;
                _persistedSidebarOrder = order;
                _hasPersistedSidebarOrder = true;
                rebuildSidebar();
                maybeRevealInitialContent();
                refreshRooms();
            }
        });

    // Joined spaces surface as folder-like tabs; refresh the rail when they arrive
    // and resolve their logos (spaces aren't in the room list, so nothing else does).
    QObject::connect(_bridge, &ProtocolBridge::joinedSpacesReady, this,
        [this](bool success, const QVector<SpaceInfo> &spaces) {
            if (!success) {
                return;
            }
            _joinedSpacesLoaded = true;
            for (const auto &space : spaces) {
                if (!space.avatarUrl.isEmpty()
                    && MediaCache::needsResolution(space.avatarUrl)) {
                    _bridge->resolveAvatar(space.avatarUrl);
                }
            }
            rebuildSidebar();
            maybeRevealInitialContent();
            refreshRooms();
        });

    // When mxc:// avatars resolve, repaint the chat list (and the space rail).
    QObject::connect(_bridge, &ProtocolBridge::mediaResolved,
        this, [this](bool success, const QString &mxcUrl, const QString &localPath) {
            if (success && !localPath.isEmpty()) {
                MediaCache::insertPath(mxcUrl, localPath);
                if (_inner) {
                    _inner->update();
                }
                if (_filterSidebar) {
                    _filterSidebar->update();
                }
                return;
            }
            if (mxcUrl.startsWith(QStringLiteral("mxc://"))) {
                MediaCache::clearRequested(mxcUrl);
            }
        });
    QObject::connect(_bridge, &ProtocolBridge::mediaBytesResolved,
        this, [this](bool success, const QString &mxcUrl, const QByteArray &bytes, const QString &mime, const QString &) {
            if (success && MediaCache::insertImageBytes(mxcUrl, bytes, mime)) {
                if (_inner) {
                    _inner->update();
                }
                return;
            }
            if (mxcUrl.startsWith(QStringLiteral("mxc://"))) {
                MediaCache::clearRequested(mxcUrl);
            }
        });

    // Live online status updates from presence events.
    connect(_bridge, &ProtocolBridge::presenceChanged,
            this, [this](const QString &userId, int state, qint64 /*lastActiveTs*/) {
        if (_inner) {
            _inner->updateOnlineStatus(userId, state);
        }
    });

    // Show loading bar during initial sync.
    showFolderLoading();

    // Defer initial load until after the widget is laid out and has real geometry.
    QTimer::singleShot(0, this, [this]() {
        refreshRooms();
    });

    _initialDialogsLoadState = _bridge
        ? _bridge->initialDialogsLoadState()
        : InitialDialogsLoadState::NotStarted;
    if (_inner) {
        _inner->setInitialLoading(true);
    }
    if (_filterSidebar) {
        // Rail shows only its hamburger until everything is ready.
        _filterSidebar->setLoading(true);
    }
    if (_initialLoadingOverlay) {
        _initialLoadingOverlay->show();
        _initialLoadingOverlay->raise();
    }
    // Backstop: never let the gate hold the overlay up forever if a fetch is
    // lost or sync never reaches SYNCED (offline). After a few seconds, reveal
    // whatever we have.
    _initialRevealTimeout = new QTimer(this);
    _initialRevealTimeout->setSingleShot(true);
    _initialRevealTimeout->setInterval(8000);
    connect(_initialRevealTimeout, &QTimer::timeout, this, [this] {
        if (_initialContentRevealed || !_inner) {
            return;
        }
        _initialContentRevealed = true;
        if (_initialLoadingOverlay) {
            _initialLoadingOverlay->hide();
        }
        _inner->setInitialLoading(false);
        if (_filterSidebar) {
            _filterSidebar->setLoading(false);
        }
        rebuildSidebar(); // push whatever folders we have into the rail
        if (_filterSidebar && _customFoldersLoaded) {
            _filterSidebar->setEditButtonVisible(true);
        }
    });
    _initialRevealTimeout->start();
    applyInitialDialogsLoadState(_initialDialogsLoadState);
}

void DialogsWidget::setupSearchBar() {
    _search = new Ui::InputField(
        this,
        st::dialogsFilter,
        rpl::single(tr("Search")));
    // Keep automatic focus assignment (app start, tab-chain fallbacks) away
    // from the search field; clicks and programmatic setFocus still reach it.
    _search->setFocusPolicy(Qt::ClickFocus);

    _searchTimer = new QTimer(this);
    _searchTimer->setSingleShot(true);
    _searchTimer->setInterval(st::searchRequestDelay);
    QObject::connect(_searchTimer, &QTimer::timeout, this, [this] {
        if (!_inner || !_search) {
            return;
        }
        const auto query = _search->getLastText().trimmed();
        if (isServerSearchActive()) {
            performServerSearch(query);
        }
        updateControlsGeometry();
    });
    QObject::connect(_search, &Ui::InputField::textChanged, this, [this](const QString &text) {
        const auto query = text.trimmed();
        if (_searchTimer) {
            _searchTimer->start();
        }
        if (_inner) {
            if (!isMessageSearchActive()) {
                _inner->setSearchFilter(query);
                if (_filterSidebar) {
                    _inner->setActiveFilter(
                        query.isEmpty() ? _filterSidebar->activeFilterId() : 0);
                }
                if (query.isEmpty()) {
                    _inner->setMessageSearchQuery(QString());
                    cancelActiveSearchRequest();
                    resetSearchPaginationState();
                    _inner->setSearchLoading(false);
                    _inner->clearSearchResults();
                }
            }
            if (isMessageSearchActive()) {
                _inner->setMessageSearchQuery(query);
            }
        }
        if (_inner) {
            const auto shouldPrepareServerSearch = isMessageSearchActive()
                || !query.isEmpty()
                || (_activeSearchRequestId != 0)
                || (_inner->searchResultsCount() > 0);
            if (shouldPrepareServerSearch) {
                _inner->setMessageSearchQuery(query);
                cancelActiveSearchRequest();
                resetSearchPaginationState(query.isEmpty());
                if (query.isEmpty()) {
                    _inner->setSearchLoading(false);
                    _inner->clearSearchResults();
                } else {
                    _inner->setSearchLoading(true);
                    _inner->clearSearchResults();
                }
            }
        }
        // When the search-scope banner is visible, it owns cancel UI.
        if (_chatSearchIn && _chatSearchIn->isVisible()) {
            return;
        }
        _search->setCancelVisible(
            (_messageSearchMode == MessageSearchMode::MyMessages)
            || !text.trimmed().isEmpty());
    });
    QObject::connect(_search, &Ui::InputField::cancelled, this, [this] {
        handleSearchCancelled();
        // Final cancel step (Esc on an empty field / the cancel cross): drop
        // the caret too, so the keyboard leaves the search field.
        if (_search->hasFocus()) {
            _search->clearFocus();
        }
    });
}

void DialogsWidget::setupVerificationBanner() {
    auto *banner = new DialogsVerificationBanner(this);
    _verificationBanner = banner;
    banner->hide();
    banner->setAcceptedCallback([this] {
        const auto transactionId = _verificationBannerTransactionId;
        const auto isUser = _verificationBannerIsUser;
        const auto displayName = _verificationBannerDisplayName;
        hideVerificationBanner();
        if (isUser) {
            emit userVerificationRequestAccepted(transactionId, displayName);
        } else {
            emit verificationRequestAccepted(transactionId);
        }
    });
    banner->setDeclinedCallback([this] {
        const auto transactionId = _verificationBannerTransactionId;
        hideVerificationBanner();
        if (_bridge && !transactionId.isEmpty()) {
            _bridge->cancelVerification(transactionId);
        }
    });

    if (!_bridge) {
        return;
    }

    QObject::connect(
        _bridge,
        &ProtocolBridge::incomingVerificationRequestReceived,
        this,
        [this](
            const QString &transactionId,
            const QString &deviceId,
            const QString &deviceName) {
            showVerificationBanner(transactionId, deviceId, deviceName);
        });

    QObject::connect(
        _bridge,
        &ProtocolBridge::incomingUserVerificationRequestReceived,
        this,
        [this](
            const QString &flowId,
            const QString &userId,
            const QString &displayName) {
            showUserVerificationBanner(flowId, userId, displayName);
        });

    // The request stopped being answerable — another of our sessions took it,
    // the requester withdrew it, or it expired. Without this the banner sits
    // there offering to accept a request that no longer exists.
    QObject::connect(
        _bridge,
        &ProtocolBridge::verificationRequestClosed,
        this,
        [this](const QString &flowId) {
            if (_verificationBannerTransactionId.isEmpty()
                || flowId != _verificationBannerTransactionId) {
                return;
            }
            hideVerificationBanner();
        });

    QObject::connect(_bridge, &ProtocolBridge::verificationStateChanged, this, [this](int state, const QString &flowId) {
        constexpr int kDone = 8;
        constexpr int kCancelled = 9;
        if (state != kDone && state != kCancelled) {
            return;
        }
        // Only react to terminal states for the request this banner is showing
        // (or untagged states, e.g. recovery-key / global Done), so a different
        // flow's cancellation cannot dismiss this banner.
        if (!flowId.isEmpty() && !_verificationBannerTransactionId.isEmpty()
            && flowId != _verificationBannerTransactionId) {
            return;
        }
        hideVerificationBanner();
    });

    // A request may have arrived before this banner's consumer existed (main
    // window built after the intro, account switch); replay it now that we
    // are listening.
    _bridge->replayPendingVerificationRequest();
}

void DialogsWidget::showVerificationBanner(
    const QString &transactionId,
    const QString &deviceId,
    const QString &deviceName) {
    if (!_verificationBanner) {
        return;
    }
    cancelPendingBannerRequest(transactionId);
    deferNewLoginBanner();
    _verificationBannerTransactionId = transactionId;
    _verificationBannerIsUser = false;
    _verificationBannerDisplayName.clear();
    static_cast<DialogsVerificationBanner *>(_verificationBanner)
        ->setRequest(transactionId, deviceId, deviceName);
    _verificationBanner->show();
    _verificationBanner->raise();
    updateControlsGeometry();
}

void DialogsWidget::showUserVerificationBanner(
    const QString &flowId,
    const QString &userId,
    const QString &displayName) {
    if (!_verificationBanner) {
        return;
    }
    cancelPendingBannerRequest(flowId);
    deferNewLoginBanner();
    _verificationBannerTransactionId = flowId;
    _verificationBannerIsUser = true;
    _verificationBannerDisplayName = displayName.trimmed().isEmpty()
        ? userId
        : displayName.trimmed();
    static_cast<DialogsVerificationBanner *>(_verificationBanner)
        ->setUserRequest(flowId, userId, displayName);
    _verificationBanner->show();
    _verificationBanner->raise();
    updateControlsGeometry();
}

void DialogsWidget::cancelPendingBannerRequest(const QString &replacingId) {
    // A new incoming request is superseding the one the banner is showing. Cancel
    // the old flow so the remote peer isn't left waiting on a request we silently
    // dropped. No-op when there's nothing pending or it's the same flow.
    if (!_bridge || _verificationBannerTransactionId.isEmpty()
        || _verificationBannerTransactionId == replacingId) {
        return;
    }
    _bridge->cancelVerification(_verificationBannerTransactionId);
}

void DialogsWidget::hideVerificationBanner() {
    _verificationBannerTransactionId.clear();
    _verificationBannerIsUser = false;
    _verificationBannerDisplayName.clear();
    if (_verificationBanner && _verificationBanner->isVisible()) {
        _verificationBanner->hide();
        updateControlsGeometry();
    }
    // Strip is free again: show a login that arrived (or was deferred) meanwhile.
    showNextNewLoginBanner();
}

void DialogsWidget::setupNewLoginBanner() {
    auto *banner = new DialogsNewLoginBanner(this);
    _newLoginBanner = banner;
    banner->hide();
    banner->setYesCallback([this] { dismissNewLoginBanner(); });
    banner->setSignOutCallback([this] {
        const auto deviceId = _currentNewLogin.deviceId;
        dismissNewLoginBanner();
        if (!deviceId.isEmpty()) {
            emit signOutDeviceRequested(deviceId);
        }
    });

    if (!_bridge) {
        return;
    }
    QObject::connect(
        _bridge,
        &ProtocolBridge::newLoginReceived,
        this,
        [this](
            const QString &deviceId,
            const QString &displayName,
            const QString &lastSeenIp,
            qint64 lastSeenTs) {
            _pendingNewLogins.enqueue(
                PendingNewLogin{ deviceId, displayName, lastSeenIp, lastSeenTs });
            showNextNewLoginBanner();
        });
}

// The strip holds one banner at a time: a second new login — or one arriving
// while a verification request is up — waits instead of stacking.
void DialogsWidget::showNextNewLoginBanner() {
    if (!_newLoginBanner
        || _pendingNewLogins.isEmpty()
        || _newLoginBanner->isVisible()
        || (_verificationBanner && _verificationBanner->isVisible())) {
        return;
    }
    _currentNewLogin = _pendingNewLogins.dequeue();
    static_cast<DialogsNewLoginBanner *>(_newLoginBanner)->setLogin(
        _currentNewLogin.deviceId,
        _currentNewLogin.displayName,
        _currentNewLogin.lastSeenIp,
        _currentNewLogin.lastSeenTs);
    _newLoginBanner->show();
    _newLoginBanner->raise();
    updateControlsGeometry();
}

// The user answered this one: drop it and let the next queued login take the strip.
void DialogsWidget::dismissNewLoginBanner() {
    hideNewLoginBanner();
    showNextNewLoginBanner();
}

// A verification request needs the strip. Put the shown login back at the head of
// the queue so it returns once the verification banner is gone.
void DialogsWidget::deferNewLoginBanner() {
    if (!_newLoginBanner || !_newLoginBanner->isVisible()) {
        return;
    }
    _pendingNewLogins.prepend(_currentNewLogin);
    hideNewLoginBanner();
}

void DialogsWidget::hideNewLoginBanner() {
    _currentNewLogin = PendingNewLogin();
    if (!_newLoginBanner || !_newLoginBanner->isVisible()) {
        return;
    }
    _newLoginBanner->hide();
    updateControlsGeometry();
}

DialogsUpdateBar *DialogsWidget::ensureUpdateBar() {
    if (_updateTelegram) {
        return _updateTelegram;
    }
    _updateTelegram = new DialogsUpdateBar(this);
    _updateTelegram->show();

    connect(_updateTelegram, &DialogsUpdateBar::applyRequested, this, [this] {
        // Staging runs on a worker, so this only kicks it off; the quit is
        // AppController's, on applyReady.
        if (auto *update = _controller->updateService()) {
            update->applyAndRestart();
        }
    });
    connect(_updateTelegram, &DialogsUpdateBar::updateRequested, this, [this] {
        auto *update = _controller->updateService();
        if (!update) {
            return;
        }
        if (update->applyMode() == Core::UpdateService::ApplyMode::OneClick) {
            // downloadAndApply, not download: this bar's contract is one click
            // for the whole thing, and the flag is what keeps an auto-download
            // from inheriting that behaviour.
            update->downloadAndApply();
        } else if (!update->releasePage().isEmpty()) {
            // deb/rpm and friends: a package manager owns those files, and the
            // manifest carries no asset for them, so the page is the only route.
            OpenSafeExternalUrl(update->releasePage());
        }
    });
    connect(_updateTelegram, &DialogsUpdateBar::skipRequested, this, [this] {
        auto *update = _controller->updateService();
        if (!update || update->availableVersion().isEmpty()) {
            return;
        }
        _controller->settings().setSkippedUpdateVersion(update->availableVersion());
        _controller->saveSettingsDelayed();
        checkUpdateStatus();
    });
    connect(_updateTelegram, &DialogsUpdateBar::cancelRequested, this, [this] {
        if (auto *update = _controller->updateService()) {
            update->cancelDownload();
        }
    });
    return _updateTelegram;
}

void DialogsWidget::destroyUpdateBar() {
    if (!_updateTelegram) {
        return;
    }
    // deleteLater, not delete: a failed applyAndRestart() clears the ready path
    // and emits updateError, which lands here while we are still inside the
    // bar's own mouse-release handler. Destroying it now would return into a
    // freed widget.
    _updateTelegram->hide();
    _updateTelegram->deleteLater();
    _updateTelegram = nullptr;
}

void DialogsWidget::checkUpdateStatus() {
    auto *service = _controller ? _controller->updateService() : nullptr;
    if (!service) {
        destroyUpdateBar();
        return;
    }
    // The same condition AppController uses to auto-download. When it holds the
    // user has already opted in, so there is nothing to prompt about and the
    // download stays silent until it is ready — today's behaviour.
    const auto autoDownloading =
        (_controller->settings().updatePolicy()
            == static_cast<int>(Core::UpdateService::Policy::AutoDownload))
        && (service->applyMode() == Core::UpdateService::ApplyMode::OneClick);
    const auto version = service->availableVersion();

    if (!service->readyPath().isEmpty()) {
        // Staging keeps the ready path set, so the bar stays put and reports that
        // it is working instead of silently doing nothing for ~3s.
        const auto applying = service->applying();
        ensureUpdateBar()->setReadyMode(
            applying ? tr("Updating…") : tr("Update TeleMatrix"),
            !applying);
    } else if (service->downloading() && !autoDownloading) {
        ensureUpdateBar()->setDownloadingMode(
            tr("Downloading %1…").arg(version), _downloadPercent);
    } else if (!version.isEmpty()
               && !autoDownloading
               && version != _controller->settings().skippedUpdateVersion()) {
        ensureUpdateBar()->setPromptMode(
            tr("New version available (%1)").arg(version));
    } else {
        destroyUpdateBar();
    }
    updateControlsGeometry();
}

void DialogsWidget::setupFilterButtons() {
    // The "from user" button overlaps the search field's right edge.
    // Parent to `this` and raise above the search field.

    // "From" person icon button (29×35, inside search field).
    auto *fromBtn = new SearchFilterButton(
        QStringLiteral(":/dialogs/search_from.png"), this);
    fromBtn->setClickCallback([this] { showMemberPicker(); });
    fromBtn->hide();
    _fromUserButton = fromBtn;
}

void DialogsWidget::updateFilterButtonsVisibility() {
    // Show "from user" filter only for group chats, not direct (1:1) chats.
    const auto visible = isRoomMessageSearch()
        && !_searchInRoomId.isEmpty()
        && !_searchInRoomIsDirect;
    if (_fromUserButton) {
        _fromUserButton->setVisible(visible);
    }
}

void DialogsWidget::ensureChatSearchIn() {
    if (_chatSearchIn) {
        return;
    }

    _chatSearchIn = new ChatSearchIn(_inner);
    QObject::connect(_chatSearchIn, &ChatSearchIn::roomCleared, this, [this] {
        _search->clear();
        _search->setCancelVisible(false);
        clearSearchInChat();
        if (_inner && _filterSidebar) {
            syncInnerActiveSelection();
        }
        updateControlsGeometry();
    });
    QObject::connect(_chatSearchIn, &ChatSearchIn::senderCleared, this, [this] {
        _searchSenderFilter.clear();
        _searchSenderName.clear();
        if (_chatSearchIn) {
            _chatSearchIn->clearSender();
        }
        const auto query = _search ? _search->getLastText().trimmed() : QString();
        performServerSearch(query);
        updateControlsGeometry();
    });
    QObject::connect(_chatSearchIn, &ChatSearchIn::roomFilterClicked, this, [this] {
        showRoomFilterDropdown();
    });
    QObject::connect(_chatSearchIn, &ChatSearchIn::senderFilterClicked, this, [this] {
        showMemberPicker();
    });
}

void DialogsWidget::updateChatSearchInPresentation() {
    if (!_chatSearchIn) {
        return;
    }

    const auto isMyMessages = (_messageSearchMode == MessageSearchMode::MyMessages);
    _chatSearchIn->setRoom(_searchInRoomId, isMyMessages
        ? tr("My Messages")
        : _searchInRoomName);
    _chatSearchIn->setRoomLabel(isMyMessages
        ? tr("My Messages")
        : searchInPeerLabel(_searchInRoomIsDirect));
    _chatSearchIn->setRoomUseChatsIcon(isMyMessages);
    _chatSearchIn->setRoomAvatar(isMyMessages ? QString() : _searchInRoomAvatarUrl);
    if (isMyMessages) {
        _chatSearchIn->clearSender();
    }
    _chatSearchIn->show();
    _chatSearchIn->raise();

    if (_search) {
        _search->setPlaceholderText(isMyMessages
            ? tr("Search messages in")
            : tr("Search messages in %1").arg(_searchInRoomName));
        _search->setCancelVisible(false);
    }
    if (_inner) {
        _inner->setMessageSearchGlobalScope(isMyMessages);
        _inner->setMessageSearchQuery(_search ? _search->getLastText().trimmed() : QString());
    }
}

void DialogsWidget::showMemberPicker() {
    if (_searchInRoomId.isEmpty() || !_bridge) {
        return;
    }
    if (_memberPicker) {
        return; // already open
    }
    _pendingMemberPickerRoomId = _searchInRoomId;
    _memberPickerMembers.clear();

    // Open the picker immediately in its "Loading…" state, then fetch the members in the
    // background — roomMembersReady fills the list in while the box is already on screen.
    auto *picker = new MemberPickerBox({}, _bridge, window());
    _memberPicker = picker;
    QObject::connect(picker, &MemberPickerBox::memberSelected, this,
        [this](const QString &userId, const QString &displayName) {
            _searchSenderFilter = userId;
            _searchSenderName = displayName;
            QString avatarUrl;
            const auto i = std::find_if(
                _memberPickerMembers.begin(),
                _memberPickerMembers.end(),
                [&](const UserProfile &member) { return member.userId == userId; });
            if (i != _memberPickerMembers.end()) {
                avatarUrl = i->avatarUrl;
            }
            if (_chatSearchIn) {
                _chatSearchIn->setSender(userId, displayName);
                _chatSearchIn->setSenderAvatar(avatarUrl);
                updateControlsGeometry();
            }
            const auto query = _search ? _search->getLastText().trimmed() : QString();
            performServerSearch(query);
        });

    _bridge->getRoomMembersAsync(_searchInRoomId);
    picker->exec();

    _memberPicker = nullptr;
    _pendingMemberPickerRoomId.clear();
    picker->deleteLater();
}

void DialogsWidget::showRoomFilterDropdown() {
    if (!_chatSearchIn || _searchInRoomId.isEmpty()) {
        return;
    }

    const auto myUserId = _controller ? _controller->userId() : QString();
    const bool isMyMessages = (_messageSearchMode == MessageSearchMode::MyMessages);
    const int activeIndex = isMyMessages ? 1 : 0;

    QVector<SearchPopupItem> items;
    items.push_back({
        searchInPeerLabel(_searchInRoomIsDirect),
        _searchInRoomName,
        _searchInRoomAvatarUrl,
        _searchInRoomId,
        false,
        !isMyMessages,
    });
    if (!myUserId.isEmpty()) {
        items.push_back({
            tr("My Messages"),
            QString(),
            QString(),
            QString(),
            true,
            isMyMessages,
        });
    }

    auto *popup = new SearchScopePopup(_chatSearchIn);
    popup->setItems(std::move(items));
    popup->setTriggeredCallback([this, myUserId](int index) {
        if (index == 0) {
            _messageSearchMode = MessageSearchMode::Room;
            _searchSenderFilter.clear();
            _searchSenderName.clear();
            if (_chatSearchIn) {
                _chatSearchIn->clearSender();
            }
            updateChatSearchInPresentation();
        } else if (index == 1 && !myUserId.isEmpty()) {
            activateGlobalMyMessagesSearch();
        }
        const auto query = _search ? _search->getLastText().trimmed() : QString();
        performServerSearch(query);
        updateControlsGeometry();
    });

    const auto roomRowGlobal = _chatSearchIn->mapToGlobal(
        QPoint(0, st::searchedBarHeight));
    popup->popupAtRow(roomRowGlobal, activeIndex, 0);
}

void DialogsWidget::setupFilterSidebar() {
    _filterSidebar = new DialogsFilterSidebar(this);
    QObject::connect(_filterSidebar, &DialogsFilterSidebar::mainMenuRequested,
        this, &DialogsWidget::mainMenuRequested);
    QObject::connect(_filterSidebar, &DialogsFilterSidebar::filterSelected, this, [this](int filterId) {
        if (_inner) {
            _inner->setActiveFilter(filterId);
        }
    });
    QObject::connect(_filterSidebar, &DialogsFilterSidebar::spaceSelected, this, [this](const QString &spaceId) {
        if (_inner) {
            _inner->setActiveSpace(spaceId);
        }
    });
    QObject::connect(_filterSidebar, &DialogsFilterSidebar::manageFoldersRequested,
        this, [this]() { openFoldersManager(); });
    QObject::connect(_filterSidebar, &DialogsFilterSidebar::editFolderRequested,
        this, [this](int filterId) { promptEditFolder(filterId); });
    QObject::connect(_filterSidebar, &DialogsFilterSidebar::deleteFolderRequested,
        this, [this](int filterId) { requestDeleteFolder(filterId); });
    QObject::connect(_filterSidebar, &DialogsFilterSidebar::editSpaceRequested,
        this, [this](const QString &spaceId) { openSpaceInfo(spaceId); });
    QObject::connect(_filterSidebar, &DialogsFilterSidebar::exploreSpaceRequested,
        this, [this](const QString &spaceId, const QString &name) {
        emit exploreSpaceRequested(spaceId, name);
    });
    QObject::connect(_filterSidebar, &DialogsFilterSidebar::leaveSpaceRequested,
        this, [this](const QString &spaceId) { requestLeaveSpace(spaceId); });
    QObject::connect(_filterSidebar, &DialogsFilterSidebar::sidebarReordered,
        this, [this](const QVector<SidebarEntry> &order) {
        requestSidebarOrder(order);
    });
}

void DialogsWidget::syncInnerActiveSelection() {
    if (!_inner || !_filterSidebar) {
        return;
    }
    if (!_filterSidebar->activeSpaceId().isEmpty()) {
        _inner->setActiveSpace(_filterSidebar->activeSpaceId());
    } else {
        _inner->setActiveFilter(_filterSidebar->activeFilterId());
    }
}

void DialogsWidget::applyFolderMembership(const QString &roomId, int filterId,
        const QString &sectionKey, bool member) {
    if (!_bridge || roomId.isEmpty() || sectionKey.isEmpty() || filterId <= 0) {
        return;
    }
    // Optimistic: reflect the change immediately.
    if (_inner) {
        _inner->setRoomInFolder(roomId, filterId, member);
    }
    // Supersede any earlier in-flight change for this same (room, section).
    for (int i = _pendingFolderChanges.size() - 1; i >= 0; --i) {
        if (_pendingFolderChanges[i].roomId == roomId
            && _pendingFolderChanges[i].sectionKey == sectionKey) {
            _pendingFolderChanges.removeAt(i);
        }
    }
    _pendingFolderChanges.append(
        PendingFolderChange{ roomId, filterId, sectionKey, member, false });
    _bridge->addRoomToFolder(roomId, sectionKey);
    refreshOpenFoldersBox();
    updateFolderLoadingBar();
}

void DialogsWidget::reapplyPendingFolderChanges() {
    if (!_inner) {
        return;
    }
    // Re-assert each optimistic change on top of the freshly-rebuilt rows so a
    // stale rebuild can't revert it. A confirmed change whose membership the
    // cache now reflects is done — drop it.
    for (int i = _pendingFolderChanges.size() - 1; i >= 0; --i) {
        const auto &p = _pendingFolderChanges[i];
        if (p.confirmed && _inner->roomInFolder(p.roomId, p.filterId) == p.member) {
            _pendingFolderChanges.removeAt(i);
            continue;
        }
        _inner->setRoomInFolder(p.roomId, p.filterId, p.member);
    }
    updateFolderLoadingBar();
}

bool DialogsWidget::hasPendingFolderWork() const {
    if (_folderListOpInFlight || _folderOrderSaveInFlight) {
        return true;
    }
    for (const auto &p : _pendingFolderChanges) {
        if (!p.confirmed) {
            return true;
        }
    }
    return false;
}

void DialogsWidget::updateFolderLoadingBar() {
    if (hasPendingFolderWork()) {
        showFolderLoading();
        // Our explicit control is authoritative while work is in flight; don't
        // let the idle settle timer hide the preloader mid-operation.
        if (_loadingSettleTimer) {
            _loadingSettleTimer->stop();
        }
    } else {
        hideFolderLoading();
    }
}

void DialogsWidget::showFolderError(const QString &message) {
    auto *dlg = new HistoryConfirmDialog(
        this,
        tr("Couldn't update folder"),
        message.isEmpty() ? tr("The change was rejected by the server.") : message,
        tr("OK"));
    dlg->exec();
    dlg->deleteLater();
}

void DialogsWidget::promptCreateFolder(const QSet<QString> &preselected) {
    if (!_bridge) {
        return;
    }
    auto *box = new DialogsEditFolderBox(
        DialogsEditFolderBox::Create, 0, QString(),
        buildRoomEntries(), preselected, _bridge, window());
    const int result = box->exec();
    if (result == DialogsEditFolderBox::Accepted) {
        const auto name = box->folderName();
        if (!name.isEmpty()) {
            _pendingNewFolderRooms = box->selectedRoomIds();
            _folderListOpInFlight = true;
            _bridge->createFolder(name);
            updateFolderLoadingBar();
        }
    }
    box->deleteLater();
}

QString DialogsWidget::sectionKeyForFilter(int filterId) const {
    for (const auto &f : _filters) {
        if (!f.isSpace && f.filterId == filterId) {
            return f.sectionKey;
        }
    }
    return QString();
}

void DialogsWidget::promptEditFolder(int filterId) {
    if (!_bridge) {
        return;
    }
    QString currentName;
    for (const auto &f : _filters) {
        if (f.filterId == filterId) {
            currentName = f.displayName;
            break;
        }
    }
    const auto oldKey = sectionKeyForFilter(filterId);
    if (oldKey.isEmpty()) {
        return;
    }
    const auto current = roomsInFolder(filterId);
    auto *box = new DialogsEditFolderBox(
        DialogsEditFolderBox::Edit, filterId, currentName,
        buildRoomEntries(), current, _bridge, window());
    const int result = box->exec();
    if (result == DialogsEditFolderBox::Accepted) {
        const auto newName = box->folderName();
        const auto newSet = box->selectedRoomIds();
        // Reconcile membership — each change is optimistic and tracked until the
        // server confirms (so a sync rebuild can't revert it).
        for (const auto &roomId : newSet) {
            if (!current.contains(roomId)) {
                applyFolderMembership(roomId, filterId, oldKey, true);
            }
        }
        for (const auto &roomId : current) {
            if (!newSet.contains(roomId)) {
                applyFolderMembership(roomId, filterId, oldKey, false);
            }
        }
        if (!newName.isEmpty() && newName != currentName) {
            // Optimistic rename in the rail; preloader until the server confirms.
            for (auto &f : _filters) {
                if (!f.isSpace && f.filterId == filterId) {
                    f.displayName = newName;
                }
            }
            if (_filterSidebar) {
                _filterSidebar->setFilters(_filters);
            }
            refreshOpenFoldersBox();
            _folderListOpInFlight = true;
            _bridge->editFolder(oldKey, newName);
            updateFolderLoadingBar();
        }
    }
    box->deleteLater();
}

void DialogsWidget::requestDeleteFolder(int filterId) {
    const auto key = sectionKeyForFilter(filterId);
    if (_bridge && filterId > 2 && !key.isEmpty()) {
        _pendingDeleteFolderId = filterId;
        _folderListOpInFlight = true;
        _bridge->deleteFolder(key);
        updateFolderLoadingBar();
    }
}

void DialogsWidget::openSpaceInfo(const QString &spaceId) {
    if (spaceId.isEmpty()) {
        return;
    }
    // A space is an m.space room: reuse the standard Room Settings layer, which
    // already permission-gates name/avatar/topic editing. AppMainWidget owns the
    // layer stack, so route the request up to it.
    emit openRoomSettingsRequested(spaceId);
}

void DialogsWidget::requestLeaveSpace(const QString &spaceId) {
    if (!_bridge || spaceId.isEmpty()) {
        return;
    }
    QString spaceName = spaceId;
    for (const auto &f : _filters) {
        if (f.isSpace && f.spaceId == spaceId) {
            spaceName = f.displayName;
            break;
        }
    }
    // Element-parity: offer to also leave the space's rooms. Only rooms that are
    // in the chat list (i.e. joined) and carry this space handle qualify —
    // sub-spaces are not chat-list rows and are left alone.
    QVector<RoomPickEntry> rooms;
    if (_inner) {
        for (const auto &row : _inner->allRows()) {
            if (!row.spaceIds().contains(spaceId)) {
                continue;
            }
            RoomPickEntry e;
            e.id = row.roomId();
            e.name = row.displayName().isEmpty() ? row.roomId() : row.displayName();
            e.avatarUrl = row.avatarUrl();
            e.avatarEntityId = row.avatarEntityId();
            rooms.push_back(e);
        }
    }

    auto *dlg = new DialogsLeaveSpaceBox(spaceName, rooms, _bridge, this);
    if (dlg->exec() == DialogsLeaveSpaceBox::Accepted) {
        const auto alsoLeave = dlg->selectedRoomIds();
        // Clear the active-space selection so the list returns to All once the
        // tab disappears.
        if (_inner && _inner->activeSpaceId() == spaceId) {
            _inner->setActiveFilter(0);
        }
        for (const auto &roomId : alsoLeave) {
            _bridge->leaveRoom(roomId);
        }
        _bridge->leaveRoom(spaceId);
        _bridge->getJoinedSpacesAsync();
    }
    dlg->deleteLater();
}

void DialogsWidget::openFoldersManager() {
    if (!_bridge) {
        return;
    }
    auto *box = new DialogsFoldersBox(window());
    _openFoldersBox = box;
    box->setFolders(buildFolderManagerEntries());
    QObject::connect(box, &DialogsFoldersBox::createFolderRequested,
        box, [this] { promptCreateFolder(); });
    QObject::connect(box, &DialogsFoldersBox::editFolderRequested,
        box, [this](int filterId) { promptEditFolder(filterId); });
    QObject::connect(box, &DialogsFoldersBox::deleteFolderRequested,
        box, [this](int filterId) { requestDeleteFolder(filterId); });
    QObject::connect(box, &DialogsFoldersBox::folderOrderChanged,
        box, [this](const QVector<int> &folderIds) {
        requestCustomFolderOrder(folderIds);
    });
    box->exec();
    _openFoldersBox = nullptr;
    box->deleteLater();
}

void DialogsWidget::refreshOpenFoldersBox() {
    if (_openFoldersBox) {
        _openFoldersBox->setFolders(buildFolderManagerEntries());
    }
}

QVector<FolderManagerEntry> DialogsWidget::buildFolderManagerEntries() const {
    QHash<int, int> counts;
    if (_inner) {
        for (const auto &row : _inner->allRows()) {
            for (const auto id : row.filterIds()) {
                if (id > 2) {
                    counts[id] += 1;
                }
            }
        }
    }
    QVector<FolderManagerEntry> entries;
    for (const auto &f : _filters) {
        if (f.filterId > 2) {
            FolderManagerEntry e;
            e.id = f.filterId;
            e.name = f.displayName;
            e.chatCount = counts.value(f.filterId, 0);
            entries.push_back(e);
        }
    }
    return entries;
}

QVector<RoomPickEntry> DialogsWidget::buildRoomEntries() const {
    QVector<RoomPickEntry> entries;
    if (!_inner) {
        return entries;
    }
    // Precompute filterId -> display name once, so the per-room folder subtitle
    // is an O(1) lookup instead of an O(rooms x filters) nested scan.
    QHash<int, QString> filterNames;
    filterNames.reserve(_filters.size());
    for (const auto &f : _filters) {
        filterNames.insert(f.filterId, f.displayName);
    }
    const auto &rows = _inner->allRows();
    entries.reserve(rows.size());
    for (const auto &row : rows) {
        RoomPickEntry e;
        e.id = row.roomId();
        e.name = row.displayName();
        e.avatarUrl = row.avatarUrl();
        e.avatarEntityId = row.avatarEntityId();
        // Subtitle: the custom folders this room already belongs to.
        QStringList folders;
        for (const auto fid : row.filterIds()) {
            if (fid <= 2) {
                continue;
            }
            const auto it = filterNames.constFind(fid);
            if (it != filterNames.constEnd()) {
                folders << it.value();
            }
        }
        // Fallback to the built-in filter when the room is in no custom
        // folder: personal chats → "Personal", others → "Unread".
        e.status = folders.isEmpty()
            ? filterDisplayName(row.isDirect() ? 1 : 2)
            : folders.join(QStringLiteral(", "));
        entries.push_back(e);
    }
    return entries;
}

QSet<QString> DialogsWidget::roomsInFolder(int filterId) const {
    QSet<QString> set;
    if (_inner) {
        for (const auto &row : _inner->allRows()) {
            if (row.filterIds().contains(filterId)) {
                set.insert(row.roomId());
            }
        }
    }
    return set;
}

void DialogsWidget::setupScrollArea() {
    _scroll = new Ui::ScrollArea(this);
    _inner = new DialogsInner(_scroll);
    _scroll->setOwnedWidget(object_ptr<DialogsInner>::fromRaw(_inner));
    if (_bridge) {
        _inner->setSavedMessagesRoomId(_bridge->savedMessagesRoomId());
        QObject::connect(_bridge, &ProtocolBridge::savedMessagesRoomChanged,
            _inner, &DialogsInner::setSavedMessagesRoomId);
    }
    _initialLoadingOverlay = new DialogsInitialLoadingOverlay(this);
    _initialLoadingOverlay->hide();
    QObject::connect(_inner, &DialogsInner::initialLoadingPainted, this, [this] {
        if (!_deferInitialDialogsReadyUntilLoadingPaint) {
            return;
        }
        _deferInitialDialogsReadyUntilLoadingPaint = false;
        applyInitialDialogsLoadState(_initialDialogsLoadState);
    });

    QObject::connect(_inner, &DialogsInner::activeRowPainted, this,
        [this](QRect rowRect) {
            if (!_controller) {
                return;
            }
            // Clipped to the scroll viewport: a row scrolled half out of view must
            // only continue across the seam for the part still on screen, and one
            // scrolled fully away must not paint at all. Only the y-range travels
            // — the seam's width belongs to the splitter handle, not to us.
            const auto visible = rowRect.isEmpty()
                ? QRect()
                : QRect(0, _inner->mapTo(this, rowRect.topLeft()).y(),
                        1, rowRect.height())
                      .intersected(QRect(0, _scroll->y(), 1, _scroll->height()));
            _controller->setActiveRoomSeamCover(visible);
        });

    QObject::connect(_inner, &DialogsInner::pinRoomRequested, this, [this](const QString &roomId, bool pinned) {
        if (_bridge && !roomId.isEmpty()) {
            _pendingPinRequests.enqueue(PendingPinRequest{roomId, pinned});
            // Record where the room lands in the m.favourite tag itself, so the
            // arrangement is the server's and not this device's. A newly pinned room
            // goes to the bottom of the pinned block; giving it an order computed
            // from the new count keeps it after all the existing ones without having
            // to rewrite theirs (which would race this same write).
            const auto ids = _inner->pinnedRoomIds();
            const auto index = pinned ? ids.indexOf(roomId) : -1;
            const auto order = (index >= 0)
                ? double(index + 1) / double(ids.size() + 1)
                : -1.0;
            _bridge->pinRoom(roomId, pinned, order);

            // Keep the local list too: it is what an older pin's order falls back to
            // until the server has one for it.
            if (_controller) {
                _controller->accountSettings().setPinnedRoomIds(ids);
                _controller->saveSettingsDelayed();
            }
        }
    });
    QObject::connect(_inner, &DialogsInner::pinnedOrderChanged, this, [this](const QVector<QString> &pinnedIds) {
        // A drag reshuffles the whole block, so republish every room's order.
        if (_bridge) {
            _bridge->setPinnedOrder(pinnedIds);
        }
        if (_controller) {
            _controller->accountSettings().setPinnedRoomIds(pinnedIds);
            _controller->saveSettingsDelayed();
        }
    });
    QObject::connect(_inner, &DialogsInner::roomNotificationModeRequested, this, [this](const QString &roomId, RoomNotificationMode mode) {
        if (_bridge && !roomId.isEmpty()) {
            _bridge->setRoomNotificationMode(roomId, mode);
        }
    });
    QObject::connect(_inner, &DialogsInner::markReadRequested, this, [this](const QString &roomId, bool read) {
        if (_bridge && !roomId.isEmpty()) {
            quint64 unreadRevision = 0;
            if (_unreadStateStore) {
                if (read) {
                    unreadRevision = _unreadStateStore->optimisticMarkRead(roomId);
                } else {
                    unreadRevision = _unreadStateStore->optimisticMarkUnread(roomId);
                }
            }
            const auto requestId = _bridge->markRoomRead(roomId, read);
            if (_unreadStateStore) {
                _unreadStateStore->bindExplicitRequest(roomId, unreadRevision, requestId);
            }

            if (!_unreadStateStore && read) {
                _inner->setRoomUnreadCount(roomId, 0);
                _inner->setRoomHighlightCount(roomId, 0);
                _inner->setRoomMarkedUnread(roomId, false);
            } else if (!_unreadStateStore) {
                // Mark as unread — set a visual indicator.
                _inner->setRoomMarkedUnread(roomId, true);
            }
        }
    });
    QObject::connect(_inner, &DialogsInner::leaveRoomRequested, this, [this](const QString &roomId) {
        if (!_bridge || roomId.isEmpty()) {
            return;
        }
        // Look up room display name for the confirmation dialog.
        QString roomName = roomId;
        const auto rooms = _bridge->cachedRooms();
        for (const auto &room : rooms) {
            if (room.roomId == roomId) {
                roomName = room.displayName;
                break;
            }
        }
        auto *dlg = new HistoryConfirmDialog(
            this,
            tr("Leave Room"),
            tr("Are you sure you want to leave \"%1\"?")
                .arg(roomName),
            tr("Leave"));
        if (dlg->exec() == HistoryConfirmDialog::Accepted) {
            _bridge->leaveRoom(roomId);
        }
        dlg->deleteLater();
    });
    QObject::connect(_inner, &DialogsInner::deleteSavedMessagesRequested, this, [this] {
        if (!_bridge) {
            return;
        }
        auto *dlg = new HistoryConfirmDialog(
            this,
            tr("Delete Saved Messages"),
            tr("This permanently deletes Saved Messages and everything in it. "
               "This can't be undone."),
            tr("Delete"));
        if (dlg->exec() == HistoryConfirmDialog::Accepted) {
            _bridge->deleteSavedMessages();
        }
        dlg->deleteLater();
    });
    QObject::connect(_inner, &DialogsInner::addToFolderRequested, this, [this](const QString &roomId, int folderId) {
        const auto sectionKey = sectionKeyForFilter(folderId);
        if (roomId.isEmpty() || folderId <= 0 || !_bridge || sectionKey.isEmpty() || !_inner) {
            return;
        }
        // Toggle: desired state is the opposite of the current membership.
        const bool member = !_inner->roomInFolder(roomId, folderId);
        applyFolderMembership(roomId, folderId, sectionKey, member);
    });
    QObject::connect(_inner, &DialogsInner::createFolderRequested, this, [this](const QString &roomId) {
        // Open the New Folder popup with this chat preselected.
        promptCreateFolder(roomId.isEmpty() ? QSet<QString>() : QSet<QString>{ roomId });
    });

    // Forward search result clicks to parent (AppMainWidget).
    QObject::connect(_inner, &DialogsInner::searchResultClicked,
        this, &DialogsWidget::searchResultClicked);

    // Pagination: load more results when requested by inner widget.
    QObject::connect(_inner, &DialogsInner::loadMoreSearchResults,
        this, &DialogsWidget::loadMoreSearchResults);

    // Scroll-to-bottom detection for pagination.
    QObject::connect(_scroll->verticalScrollBar(), &QScrollBar::valueChanged,
        this, [this](int value) {
            if (!isServerSearchActive() || _searchDone || _searchLoadingMore) {
                return;
            }
            if (!_inner || _inner->searchResultsCount() == 0) {
                return;
            }
            const auto maxScroll = _scroll->verticalScrollBar()->maximum();
            if (maxScroll > 0 && value >= maxScroll - 50) {
                loadMoreSearchResults();
            }
        });

    QObject::connect(_bridge, &ProtocolBridge::roomPinned, this, [this](bool success) {
        if (!_pendingPinRequests.isEmpty()) {
            const auto request = _pendingPinRequests.dequeue();
            if (!success && _inner) {
                // Revert optimistic update on failure.
                _inner->setRoomPinned(request.roomId, !request.pinned);
                // Update saved pins to match reverted state.
                if (_controller) {
                    _controller->accountSettings().setPinnedRoomIds(_inner->pinnedRoomIds());
                    _controller->saveSettingsDelayed();
                }
            }
        }
        // Don't refreshRooms() here — the periodic refresh will pick up
        // the server state. Saved pins in settings already reflect the
        // correct state from the original pin action.
    });
    QObject::connect(_bridge, &ProtocolBridge::roomNotificationModeSet, this, [this](bool /*success*/) {
        refreshRooms();
    });
    QObject::connect(_bridge, &ProtocolBridge::roomMarkedRead, this,
        [this](quint64 requestId, const QString &roomId, bool read, bool success) {
        if (_unreadStateStore) {
            _unreadStateStore->ackMarkRead(roomId, read, requestId, success);
        }
        if (success) {
            refreshRooms();
        }
    });
    QObject::connect(_bridge, &ProtocolBridge::roomLeft, this, [this](bool success) {
        if (success) {
            refreshRooms();
        }
    });
    QObject::connect(_bridge, &ProtocolBridge::folderCreated, this,
            [this](bool success, int folderId, const QString &sectionKey, const QString &error) {
        _folderListOpInFlight = false;
        updateFolderLoadingBar();
        if (success && _controller && !sectionKey.isEmpty()) {
            // Add chats chosen in the New Folder box (includes a chat preselected
            // from the room context menu). Each add is optimistic + tracked.
            const auto pendingRooms = std::exchange(_pendingNewFolderRooms, QSet<QString>());
            for (const auto &roomId : pendingRooms) {
                applyFolderMembership(roomId, folderId, sectionKey, true);
            }

            _pendingSelectFolderId = folderId;
            _bridge->getCustomFoldersAsync();
        } else if (!success) {
            _pendingNewFolderRooms.clear();
            showFolderError(error);
        }
    });
    QObject::connect(_bridge, &ProtocolBridge::folderEdited, this, [this](bool success, const QString &error) {
        _folderListOpInFlight = false;
        updateFolderLoadingBar();
        if (success) {
            _bridge->getCustomFoldersAsync();
        } else {
            // Undo the optimistic rename by re-fetching the authoritative list.
            _bridge->getCustomFoldersAsync();
            showFolderError(error);
        }
    });
    QObject::connect(_bridge, &ProtocolBridge::folderDeleted, this, [this](bool success, const QString &error) {
        _folderListOpInFlight = false;
        updateFolderLoadingBar();
        if (!_controller || _pendingDeleteFolderId <= 2) {
            return;
        }
        _pendingDeleteFolderId = 0;

        // Server is authoritative — Rust backend already removed the folder.
        // Switch back to "All" if the deleted folder was active.
        _filterSidebar->setActiveFilter(0);
        if (_inner) {
            _inner->setActiveFilter(0);
        }
        if (success) {
            _bridge->getCustomFoldersAsync();
        } else {
            refreshRooms();
            showFolderError(error);
        }
    });
    // Optimistic folder membership: confirm on success (keep re-applying until the
    // cache catches up), revert + show the reason on failure.
    QObject::connect(_bridge, &ProtocolBridge::roomFolderChanged, this,
            [this](bool success, const QString &roomId, const QString &sectionKey, const QString &error) {
        for (int i = 0; i < _pendingFolderChanges.size(); ++i) {
            auto &p = _pendingFolderChanges[i];
            if (p.roomId != roomId || p.sectionKey != sectionKey) {
                continue;
            }
            if (success) {
                p.confirmed = true;
            } else {
                if (_inner) {
                    _inner->setRoomInFolder(roomId, p.filterId, !p.member);
                }
                _pendingFolderChanges.removeAt(i);
                refreshOpenFoldersBox();
                showFolderError(error);
            }
            break;
        }
        updateFolderLoadingBar();
    });
    QObject::connect(_bridge, &ProtocolBridge::sidebarOrderSaved, this, [this](bool success) {
        handleSidebarOrderSaved(success);
    });
}

void DialogsWidget::handleSyncSynced() {
    // Sync reached SYNCED — stop the initial-sync loading bar. Without this it is
    // only hidden by _loadingSettleTimer, which restarts on every roomListChanged;
    // since the sync emits roomListChanged for every timeline update on every room,
    // it never stays quiet for the 3s settle window, so the bar would spin forever.
    hideFolderLoading();

    // Mark the room list ready even if no rooms exist yet (e.g. a fresh
    // account). This drives the initial sidebar fetch (startInitialSidebarFetch)
    // and the gated reveal; it does not lift the overlay on its own.
    if (_initialDialogsLoadState != InitialDialogsLoadState::Ready) {
        applyInitialDialogsLoadState(InitialDialogsLoadState::Ready);
    } else {
        // Already Ready (warm start): make sure the sidebar fetch has been
        // kicked off so the reveal can complete.
        maybeRevealInitialContent();
    }
}

void DialogsWidget::applyInitialDialogsLoadState(InitialDialogsLoadState state) {
    _initialDialogsLoadState = state;
    if (state == InitialDialogsLoadState::Ready) {
        _hasCachedStartupRooms = false;
    }
    if (!_inner) {
        return;
    }
    // Populate the list behind the overlay; the overlay only lifts once the
    // whole sidebar has loaded too (maybeRevealInitialContent).
    if (state == InitialDialogsLoadState::Ready) {
        refreshRooms();
    }
    maybeRevealInitialContent();
}

void DialogsWidget::startInitialSidebarFetch() {
    if (_initialSidebarFetchStarted || !_bridge) {
        return;
    }
    _initialSidebarFetchStarted = true;
    // Folders + joined spaces. The saved rail order is fetched by the
    // customFoldersReady handler AFTER the folders load — get_sidebar_order
    // reads the Rust cache that get_folders fills, so fetching it in parallel
    // here would read an EMPTY order, reveal in default order, then re-sort
    // when the real order arrived (the "sorted after displayed" bug).
    _bridge->getCustomFoldersAsync();
    _bridge->getJoinedSpacesAsync();
}

void DialogsWidget::maybeRevealInitialContent() {
    if (_initialContentRevealed || !_inner) {
        return;
    }
    // The room list is "ready" once sync is done (empty accounts included) or
    // rows are already on screen (a warm start from cache).
    const bool roomsReady =
        (_initialDialogsLoadState == InitialDialogsLoadState::Ready)
        || (_inner->rowCount() > 0);
    if (!roomsReady) {
        return;
    }
    // Rooms are ready: now (and only now) fetch the sidebar, then hold the
    // reveal until folders, spaces and their order have all landed.
    startInitialSidebarFetch();
    if (!initialSidebarDataReady()) {
        return;
    }
    _initialContentRevealed = true;
    if (_initialRevealTimeout) {
        _initialRevealTimeout->stop();
    }
    if (_initialLoadingOverlay) {
        _initialLoadingOverlay->hide();
    }
    _inner->setInitialLoading(false);
    // Now that we're revealed, let the rail accept folders again and push the
    // (final, sorted) list into it, plus its "Edit folders" button.
    if (_filterSidebar) {
        _filterSidebar->setLoading(false);
    }
    rebuildSidebar();
    if (_filterSidebar && _customFoldersLoaded) {
        _filterSidebar->setEditButtonVisible(true);
    }
}

void DialogsWidget::refreshRooms() {
    if (!_bridge) {
        return;
    }
    if (_hasCachedStartupRooms
        && _initialDialogsLoadState != InitialDialogsLoadState::Ready) {
        return;
    }
    if (_roomsRefreshInFlight) {
        _roomsRefreshQueued = true;
        return;
    }
    _roomsRefreshInFlight = true;
    _latestRoomsRequestId = _bridge->nextRequestId();
    _bridge->getRoomsAsync(_latestRoomsRequestId);
}

void DialogsWidget::applyCachedRooms(const QVector<RoomSummary> &rooms) {
    if (rooms.isEmpty()) {
        return;
    }
    _hasCachedStartupRooms = true;
    applyRoomsResult(rooms);
}

void DialogsWidget::rebuildSidebar() {
    if (!_bridge || !_inner) {
        return;
    }
    _filters.clear();

    // Built-in folder: All chats (filterId=0).
    FolderInfo all;
    all.filterId = 0;
    all.displayName = filterDisplayName(0);
    _filters.push_back(all);

    // Built-in folder: Personal / direct chats (filterId=1).
    FolderInfo personal;
    personal.filterId = 1;
    personal.displayName = filterDisplayName(1);
    _filters.push_back(personal);

    // Built-in folder: Unread (filterId=2).
    // Include rooms with unread count > 0 OR marked-unread flag.
    FolderInfo unread;
    unread.filterId = 2;
    unread.displayName = filterDisplayName(2);
    _filters.push_back(unread);

    // Custom folders from server/cache for instant startup.
    auto bridgeFolders = _bridge->cachedCustomFolders();
    for (const auto &bf : bridgeFolders) {
        FolderInfo folder;
        folder.filterId = bf.filterId;
        folder.displayName = bf.displayName;
        folder.sectionKey = bf.sectionKey;
        _filters.push_back(folder);
    }
    // Joined spaces render as folder-like tabs after the custom folders.
    for (const auto &space : _bridge->cachedJoinedSpaces()) {
        FolderInfo folder;
        folder.isSpace = true;
        folder.spaceId = space.roomId;
        folder.displayName = space.displayName;
        folder.avatarUrl = space.avatarUrl;
        _filters.push_back(folder);
    }
    if (_hasOptimisticSidebarOrder) {
        reorderEntriesInFilters(_optimisticSidebarOrder);
    } else if (_hasPersistedSidebarOrder) {
        reorderEntriesInFilters(_persistedSidebarOrder);
    }
    rebuildFilterCountersFromRows();
    refreshOpenFoldersBox();
}

void DialogsWidget::applyRoomsResult(const QVector<RoomSummary> &rooms) {
    if (_unreadStateStore && !rooms.isEmpty()) {
        // Keep the shared unread store aligned with every dialogs refresh path,
        // not only bridge roomListChanged signals. The dock badge reads from
        // the store, while the rooms list renders from this snapshot.
        _unreadStateStore->applyRoomListSnapshot(rooms);
    }

    // Always accept room list updates — this includes legitimate room removals.
    // The previous guard (incomingWithMsg < existingWithMsg) prevented partial
    // updates during timeline warmup but also blocked room removals from being
    // reflected in the UI. The merge logic below preserves cached preview text
    // for rooms that arrive with an empty lastMessage.
    if (_inner && _inner->rowCount() > 0) {
        if (rooms.isEmpty()) {
            return;
        }
    }
    // The overlay is NOT lifted here — the list is built behind it and revealed
    // only once the sidebar has loaded too (maybeRevealInitialContent at the end
    // of this function), so folders/spaces never pop in after the rooms.
    // Merge: if incoming rooms have empty lastMessage but cached version
    // has one, preserve the cached message and timestamp for display
    // until timeline sync provides fresh data.
    auto merged = rooms;
    if (_inner && _inner->rowCount() > 0) {
        QHash<QString, int> existingByRoomId;
        for (int i = 0; i < _inner->rowCount(); ++i) {
            existingByRoomId.insert(_inner->roomIdAt(i), i);
        }
        for (auto &r : merged) {
            if (r.lastMessage.isEmpty()) {
                auto it = existingByRoomId.find(r.roomId);
                if (it != existingByRoomId.end()) {
                    const auto &existing = _inner->roomAt(*it);
                    if (!existing.lastMessage().isEmpty()) {
                        r.lastMessage = existing.lastMessage();
                        r.lastSender = existing.lastSender();
                        r.isLastMessageOutgoing = existing.isLastMessageOutgoing();
                        r.lastMessageSendState = existing.lastMessageSendState();
                        r.timestamp = existing.timestamp();
                    }
                }
            }
        }
    }
    applyUnreadStateToRooms(merged);
    _inner->setRooms(merged);

    // Request resolution for any mxc:// avatar URLs (server thumbnails; the result
    // arrives on mediaResolved, already handled above via insertPath).
    for (const auto &room : rooms) {
        if (room.avatarUrl.startsWith(QStringLiteral("mxc://"))
                && MediaCache::needsResolution(room.avatarUrl)) {
            MediaCache::markRequested(room.avatarUrl);
            _bridge->resolveAvatar(room.avatarUrl);
        }
    }

    if (_controller) {
        const auto &savedPins = _controller->accountSettings().pinnedRoomIds();
        _inner->applyPinnedOrder(savedPins);

        // Rooms pinned by a build that didn't record an order have none on the
        // server, so their arrangement would still die with this device's settings
        // file. Publish what we have, once, and it becomes the shared order.
        if (_bridge
                && !_pinnedOrderPublished
                && _inner->hasPinnedRoomsWithoutOrder()) {
            _pinnedOrderPublished = true;
            _bridge->setPinnedOrder(_inner->pinnedRoomIds());
        }
    }

    // Re-apply optimistic pin state for any in-flight pin requests
    // so the server refresh doesn't revert the user's action.
    for (const auto &req : _pendingPinRequests) {
        _inner->setRoomPinned(req.roomId, req.pinned);
    }
    // Same for in-flight folder-membership changes.
    reapplyPendingFolderChanges();

    const auto query = _search ? _search->getLastText().trimmed() : QString();
    const auto roomListQuery = isMessageSearchActive() ? QString() : query;
    _inner->setSearchFilter(roomListQuery);

    rebuildSidebar();
    if (_pendingSelectFolderId > 0) {
        const auto pendingFolderId = _pendingSelectFolderId;
        const auto hasPendingFolder = std::any_of(
            _filters.cbegin(),
            _filters.cend(),
            [pendingFolderId](const FolderInfo &folder) {
                return folder.filterId == pendingFolderId;
            });
        if (hasPendingFolder) {
            _filterSidebar->setActiveFilter(pendingFolderId);
            _pendingSelectFolderId = 0;
        }
    }
    const auto hasSearch = !roomListQuery.isEmpty();
    if (hasSearch) {
        _inner->setActiveFilter(0);
    } else {
        syncInnerActiveSelection();
    }
    updateControlsGeometry();
    // Force scroll area viewport to repaint.
    _scroll->viewport()->update();

    // The list (rooms + sidebar) is now built; lift the overlay if the sidebar
    // data has arrived, otherwise start fetching it and keep waiting.
    maybeRevealInitialContent();
}

void DialogsWidget::setRoomDraft(const QString &roomId, const QString &text) {
    if (!_inner) {
        return;
    }
    _inner->setDraft(roomId, text);
}

void DialogsWidget::setRoomUnreadCount(const QString &roomId, int count) {
    if (_unreadStateStore) {
        _unreadStateStore->optimisticReadProgress(
            roomId,
            QString(),
            QString(),
            count);
        return;
    }
    if (!_inner) {
        return;
    }
    _inner->setRoomUnreadCount(roomId, count);
    rebuildFilterCountersFromRows();
}

void DialogsWidget::applyUnreadStateToRooms(QVector<RoomSummary> &rooms) const {
    if (!_unreadStateStore) {
        return;
    }
    for (auto &room : rooms) {
        const auto state = _unreadStateStore->roomState(room.roomId);
        if (state.roomId.isEmpty()) {
            continue;
        }
        // display* is effective* clamped to 0 for the active read-consuming room
        // (a message arriving at its live bottom is auto-read at once, so the
        // badge must not blink the raise). The marked-unread flag is never clamped.
        room.unreadCount = state.displayUnreadCount;
        room.highlightCount = state.displayHighlightCount;
        room.isMarkedUnread = state.effectiveMarkedUnread;
    }
}

void DialogsWidget::applyUnreadStateToRoom(const QString &roomId) {
    if (!_inner || !_unreadStateStore || roomId.isEmpty()) {
        return;
    }
    const auto state = _unreadStateStore->roomState(roomId);
    if (state.roomId.isEmpty()) {
        return;
    }
    _inner->setRoomUnreadCount(roomId, state.displayUnreadCount);
    _inner->setRoomHighlightCount(roomId, state.displayHighlightCount);
    _inner->setRoomMarkedUnread(roomId, state.effectiveMarkedUnread);
}

void DialogsWidget::rebuildFilterCountersFromRows() {
    if (!_filterSidebar || !_inner || _filters.isEmpty()) {
        return;
    }

    for (auto &folder : _filters) {
        folder.unreadCount = 0;
        folder.unreadMuted = false;
    }

    // Spaces share the _filters vector but must NEVER be keyed by filterId (they
    // carry the default 0, which would collide with "All"). Index folders by
    // handle and spaces by their room id, separately.
    QHash<int, int> byId;
    QHash<QString, int> bySpaceId;
    byId.reserve(_filters.size());
    for (int i = 0; i < _filters.size(); ++i) {
        if (_filters[i].isSpace) {
            bySpaceId.insert(_filters[i].spaceId, i);
        } else {
            byId.insert(_filters[i].filterId, i);
        }
    }

    // "Include muted chats in folders counters": when off, muted rooms contribute nothing to a
    // folder's unread number. Any non-default notification level counts as muted.
    const bool includeMuted =
        !_controller || _controller->settings().includeMutedInFolders();

    for (const auto &row : _inner->allRows()) {
        const auto unreadCount = qMax(0, row.unreadCount());
        const auto hasUnread = (unreadCount > 0);
        const auto hasUnreadIndicator = hasUnread || row.isMarkedUnread() || row.hasMentionBadge();

        const bool muted = roomCountsAsMuted(row.notificationMode(), row.isMuted());
        const auto add = (includeMuted || !muted) ? unreadCount : 0;
        // Only a muted room that actually contributes should tint the folder badge.
        const bool markMuted = (muted && add > 0);

        if (hasUnread && byId.contains(0)) {
            auto &folder = _filters[byId.value(0)];
            folder.unreadCount += add;
            if (markMuted) {
                folder.unreadMuted = true;
            }
        }

        if (hasUnread && row.isDirect() && byId.contains(1)) {
            auto &folder = _filters[byId.value(1)];
            folder.unreadCount += add;
            if (markMuted) {
                folder.unreadMuted = true;
            }
        }

        if (hasUnreadIndicator && byId.contains(2)) {
            auto &folder = _filters[byId.value(2)];
            folder.unreadCount += add;
            if (markMuted) {
                folder.unreadMuted = true;
            }
        }

        if (!hasUnread) {
            continue;
        }
        for (const auto filterId : row.filterIds()) {
            if (filterId <= 2 || !byId.contains(filterId)) {
                continue;
            }
            auto &folder = _filters[byId.value(filterId)];
            folder.unreadCount += add;
            if (markMuted) {
                folder.unreadMuted = true;
            }
        }
        for (const auto &spaceId : row.spaceIds()) {
            const auto it = bySpaceId.constFind(spaceId);
            if (it == bySpaceId.constEnd()) {
                continue;
            }
            auto &folder = _filters[it.value()];
            folder.unreadCount += add;
            if (markMuted) {
                folder.unreadMuted = true;
            }
        }
    }

    // The rail ignores this while loading (it shows only its hamburger); the
    // reveal re-pushes the final list. The inner still gets the folders (it's
    // under the overlay) so filtering is ready the instant we reveal.
    _filterSidebar->setFilters(_filters);
    _inner->setFolders(_filters);
}

void DialogsWidget::reorderEntriesInFilters(const QVector<SidebarEntry> &order) {
    if (_filters.isEmpty()) {
        return;
    }

    const auto matches = [](const FolderInfo &f, const SidebarEntry &e) {
        return e.isSpace ? (f.isSpace && f.spaceId == e.key)
                         : (!f.isSpace && f.filterId > 2 && f.sectionKey == e.key);
    };

    QVector<FolderInfo> ordered;
    ordered.reserve(_filters.size());
    QVector<int> consumed; // indices of _filters already placed

    // Built-in filters (All/Personal/Unread) keep their positions at the top.
    for (int i = 0; i < _filters.size(); ++i) {
        if (!_filters[i].isSpace && _filters[i].filterId <= 2) {
            ordered.push_back(_filters[i]);
            consumed.push_back(i);
        }
    }

    // Reorderable entries (folders + spaces) in the requested token order.
    for (const auto &entry : order) {
        for (int i = 0; i < _filters.size(); ++i) {
            if (consumed.contains(i)) {
                continue;
            }
            if (matches(_filters[i], entry)) {
                ordered.push_back(_filters[i]);
                consumed.push_back(i);
                break;
            }
        }
    }

    // Anything reorderable not named in the order keeps its relative position.
    for (int i = 0; i < _filters.size(); ++i) {
        if (!consumed.contains(i)) {
            ordered.push_back(_filters[i]);
        }
    }

    _filters = ordered;
}

void DialogsWidget::applySidebarOrder(const QVector<SidebarEntry> &order) {
    reorderEntriesInFilters(order);
    rebuildFilterCountersFromRows();
}

// The folders-manager box reorders folders only, by int handle. Adapt that to a
// unified order: the given folders in order, then the current spaces after them.
void DialogsWidget::requestCustomFolderOrder(const QVector<int> &folderIds) {
    QVector<SidebarEntry> order;
    order.reserve(_filters.size());
    for (const auto id : folderIds) {
        const auto key = sectionKeyForFilter(id);
        if (!key.isEmpty()) {
            order.append(SidebarEntry{ false, key });
        }
    }
    for (const auto &f : _filters) {
        if (f.isSpace && !f.spaceId.isEmpty()) {
            order.append(SidebarEntry{ true, f.spaceId });
        }
    }
    requestSidebarOrder(order);
}

void DialogsWidget::requestSidebarOrder(const QVector<SidebarEntry> &order) {
    _optimisticSidebarOrder = order;
    _hasOptimisticSidebarOrder = true;
    applySidebarOrder(order);

    if (!_bridge) {
        return;
    }
    if (_folderOrderSaveInFlight) {
        if (order == _activeSidebarOrderSave) {
            _folderOrderSaveQueued = false;
            _queuedSidebarOrder.clear();
        } else {
            _queuedSidebarOrder = order;
            _folderOrderSaveQueued = true;
        }
        return;
    }
    startSidebarOrderSave(order);
}

void DialogsWidget::startSidebarOrderSave(const QVector<SidebarEntry> &order) {
    if (!_bridge) {
        return;
    }
    _activeSidebarOrderSave = order;
    _folderOrderSaveInFlight = true;
    _bridge->setSidebarOrder(order);
    updateReorderLockState();
}

void DialogsWidget::updateReorderLockState() {
    if (_filterSidebar) {
        _filterSidebar->setReorderLocked(_folderOrderSaveInFlight);
    }
    updateFolderLoadingBar();
}

void DialogsWidget::handleSidebarOrderSaved(bool success) {
    if (!_folderOrderSaveInFlight) {
        return;
    }

    _folderOrderSaveInFlight = false;
    _activeSidebarOrderSave.clear();

    if (!success) {
        if (_folderOrderSaveQueued) {
            const auto next = _queuedSidebarOrder;
            _queuedSidebarOrder.clear();
            _folderOrderSaveQueued = false;
            applySidebarOrder(next);
            startSidebarOrderSave(next);
        } else {
            _hasOptimisticSidebarOrder = false;
            _optimisticSidebarOrder.clear();
            showFolderError(tr("Couldn't save the new order."));
            refreshRooms();
            updateReorderLockState();
        }
        return;
    }

    if (_folderOrderSaveQueued) {
        const auto next = _queuedSidebarOrder;
        _queuedSidebarOrder.clear();
        _folderOrderSaveQueued = false;
        applySidebarOrder(next);
        startSidebarOrderSave(next);
        return;
    }

    if (_hasOptimisticSidebarOrder) {
        applySidebarOrder(_optimisticSidebarOrder);
        // Promote the just-saved order to the persisted baseline so a later
        // refresh keeps it (instead of reverting to the pre-drag order). The
        // server's account-data echo later confirms the same value.
        _persistedSidebarOrder = _optimisticSidebarOrder;
        _hasPersistedSidebarOrder = true;
    }
    _hasOptimisticSidebarOrder = false;
    _optimisticSidebarOrder.clear();
    updateReorderLockState();
}

void DialogsWidget::setRoomSelectedCallback(RoomSelectedCallback callback) {
    _inner->setRoomSelectedCallback(std::move(callback));
}

void DialogsWidget::selectRoomById(const QString &roomId) {
    _inner->selectRoomById(roomId);
}

int DialogsWidget::chatListLeft() const {
    return _filterSidebar ? _filterSidebar->width() : 0;
}

void DialogsWidget::focusSearch(const QString &query) {
    if (!_search) {
        return;
    }
    if (isMessageSearchActive()) {
        clearSearchInChat();
    }
    if (!query.isNull()) {
        _search->setText(query);
    }
    _search->setFocus(Qt::ShortcutFocusReason);
    _search->selectAll();
    if (_searchTimer) {
        _searchTimer->start();
    }
}

void DialogsWidget::focusSearchInChat(const QString &roomId, const QString &roomName, bool isDirect) {
    if (!_search || roomId.isEmpty()) {
        return;
    }
    _messageSearchMode = MessageSearchMode::Room;
    _searchInRoomId = roomId;
    _searchInRoomName = roomName;
    _searchInRoomIsDirect = isDirect;
    _searchInRoomAvatarUrl.clear();
    cancelActiveSearchRequest();
    resetSearchPaginationState();
    _searchSenderFilter.clear();
    _searchSenderName.clear();

    if (_bridge) {
        const auto rooms = _bridge->cachedRooms();
        const auto i = std::find_if(
            rooms.begin(),
            rooms.end(),
            [&](const RoomSummary &room) { return room.roomId == roomId; });
        if (i != rooms.end()) {
            _searchInRoomAvatarUrl = i->avatarUrl;
            if (_searchInRoomAvatarUrl.startsWith(QStringLiteral("mxc://"))
                    && MediaCache::needsResolution(_searchInRoomAvatarUrl)) {
                MediaCache::markRequested(_searchInRoomAvatarUrl);
                _bridge->resolveAvatar(_searchInRoomAvatarUrl);
            }
        }
    }

    // Update placeholder to show search-in-room context.
    _search->clear();
    _search->setCancelVisible(false);
    _search->setFocus(Qt::ShortcutFocusReason);

    ensureChatSearchIn();
    updateChatSearchInPresentation();
    if (_inner) {
        _inner->setMessageSearchMode(true, _chatSearchIn->contentHeight());
    }

    // Show filter buttons.
    updateFilterButtonsVisibility();

    // Clear room list search filter (we're in message search mode now).
    if (_inner) {
        _inner->setSearchFilter(QString());
        _inner->clearSearchResults();
    }
    updateControlsGeometry();
}

void DialogsWidget::activateGlobalMyMessagesSearch() {
    const auto myUserId = _controller ? _controller->userId() : QString();
    if (myUserId.isEmpty() || _searchInRoomId.isEmpty()) {
        return;
    }

    _messageSearchMode = MessageSearchMode::MyMessages;
    cancelActiveSearchRequest();
    resetSearchPaginationState();
    _searchSenderFilter = myUserId;
    _searchSenderName = _controller && !_controller->displayName().isEmpty()
        ? _controller->displayName()
        : tr("Me");
    ensureChatSearchIn();
    updateChatSearchInPresentation();

    updateFilterButtonsVisibility();

    if (_inner) {
        _inner->setSearchFilter(QString());
        _inner->clearSearchResults();
        _inner->setMessageSearchMode(true, _chatSearchIn ? _chatSearchIn->contentHeight() : 0);
    }
}

void DialogsWidget::clearSearchInChat() {
    cancelActiveSearchRequest();
    _messageSearchMode = MessageSearchMode::None;
    _searchInRoomId.clear();
    _searchInRoomName.clear();
    _searchInRoomAvatarUrl.clear();
    _searchInRoomIsDirect = false;

    // Clear filter state.
    _searchSenderFilter.clear();
    _searchSenderName.clear();

    // Clear pagination state.
    resetSearchPaginationState();

    // Destroy ChatSearchIn banner.
    if (_inner) {
        _inner->setMessageSearchMode(false);
    }
    if (_chatSearchIn) {
        _chatSearchIn->hide();
        _chatSearchIn->deleteLater();
        _chatSearchIn = nullptr;
    }

    // Hide filter buttons.
    updateFilterButtonsVisibility();

    if (_search) {
        _search->setPlaceholderText(QString());
    }
    if (_inner) {
        _inner->setMessageSearchGlobalScope(false);
        _inner->setMessageSearchQuery(QString());
        _inner->setSearchLoading(false);
        _inner->clearSearchResults();
    }
    updateControlsGeometry();
    emit searchInChatClosed();
}

bool DialogsWidget::handleSearchCancelled() {
    if (!_search || !hasActiveSearch()) {
        return false;
    }
    // Two-step cancel when searching in a room:
    // First cancel: clear query text, keep room context.
    // Second cancel (or if query is already empty): clear room context.
    if (isRoomMessageSearch()) {
        if (_search->text().trimmed().isEmpty()) {
            _search->clear();
            _search->setCancelVisible(false);
            clearSearchInChat();
            if (_inner && _filterSidebar) {
                syncInnerActiveSelection();
            }
        } else {
            _search->clear();
            if (_inner) {
                _inner->clearSearchResults();
            }
        }
    } else if (_messageSearchMode == MessageSearchMode::MyMessages) {
        if (_search->text().trimmed().isEmpty()) {
            _search->clear();
            _search->setCancelVisible(false);
            clearSearchInChat();
            if (_inner && _filterSidebar) {
                syncInnerActiveSelection();
            }
        } else {
            _search->clear();
            _search->setCancelVisible(false);
            if (_inner) {
                _inner->clearSearchResults();
            }
        }
    } else {
        _search->clear();
        _search->setCancelVisible(false);
        cancelActiveSearchRequest();
        resetSearchPaginationState();
        if (_inner) {
            _inner->setMessageSearchQuery(QString());
            _inner->setSearchLoading(false);
            _inner->clearSearchResults();
            _inner->setSearchFilter(QString());
            if (_filterSidebar) {
                syncInnerActiveSelection();
            }
        }
    }
    updateControlsGeometry();
    return true;
}

void DialogsWidget::cancelActiveSearchRequest() {
    if (_bridge && _activeSearchRequestId != 0) {
        _bridge->cancelSearch(_activeSearchRequestId);
    }
    _activeSearchRequestId = 0;
}

void DialogsWidget::resetSearchPaginationState(bool done) {
    _searchNextToken.clear();
    _searchDone = done;
    _searchLoadingMore = false;
}

void DialogsWidget::performServerSearch(const QString &query) {
    if (!isServerSearchActive() || !_bridge) {
        return;
    }
    if (isRoomMessageSearch() && _searchInRoomId.isEmpty()) {
        return;
    }
    if (query.trimmed().isEmpty()) {
        if (_inner) {
            _inner->setSearchLoading(false);
            _inner->clearSearchResults();
        }
        resetSearchPaginationState();
        return;
    }
    // Reset pagination for fresh search.
    resetSearchPaginationState(false);
    if (_inner) {
        _inner->setSearchLoading(true);
        _inner->clearSearchResults();
    }

    SearchRequest req;
    req.requestId = ++_searchRequestCounter;
    req.scope = isRoomMessageSearch()
        ? SearchScope::Room
        : SearchScope::AllRooms;
    if (isRoomMessageSearch()) {
        req.roomId = _searchInRoomId;
    }
    req.query = query;
    req.senderFilter = (_messageSearchMode == MessageSearchMode::MyMessages)
        ? (_controller ? _controller->userId() : QString())
        : _searchSenderFilter;
    _activeSearchRequestId = req.requestId;
    _bridge->searchMessagesAsync(req);
}

void DialogsWidget::loadMoreSearchResults() {
    if (!isServerSearchActive() || !_bridge || _searchDone || _searchLoadingMore) {
        return;
    }
    if (_searchNextToken.isEmpty()) {
        return;
    }
    const auto query = _search ? _search->getLastText().trimmed() : QString();
    if (query.isEmpty()) {
        return;
    }

    _searchLoadingMore = true;
    SearchRequest req;
    req.requestId = ++_searchRequestCounter;
    req.scope = isRoomMessageSearch()
        ? SearchScope::Room
        : SearchScope::AllRooms;
    if (isRoomMessageSearch()) {
        req.roomId = _searchInRoomId;
    }
    req.query = query;
    req.nextToken = _searchNextToken;
    req.senderFilter = (_messageSearchMode == MessageSearchMode::MyMessages)
        ? (_controller ? _controller->userId() : QString())
        : _searchSenderFilter;
    _activeSearchRequestId = req.requestId;
    _bridge->searchMessagesAsync(req);
}

void DialogsWidget::clearSelection() {
    _inner->clearSelection();
}

bool DialogsWidget::handleSearchEscape() {
    return handleSearchCancelled();
}

void DialogsWidget::showEvent(QShowEvent *e) {
    Ui::RpWidget::showEvent(e);
    // When the widget first becomes visible, load rooms with real geometry.
    updateControlsGeometry();
    refreshRooms();
}

void DialogsWidget::keyPressEvent(QKeyEvent *e) {
    if (!_inner || !isMessageSearchActive()) {
        Ui::RpWidget::keyPressEvent(e);
        return;
    }
    if (e->key() == Qt::Key_F3) {
        if (e->modifiers() & Qt::ShiftModifier) {
            _inner->selectPreviousSearchResult();
        } else {
            _inner->selectNextSearchResult();
        }
        e->accept();
        return;
    }
    Ui::RpWidget::keyPressEvent(e);
}

void DialogsWidget::resizeEvent(QResizeEvent *e) {
    Ui::RpWidget::resizeEvent(e);
    updateControlsGeometry();
}

void DialogsWidget::showFolderLoading() {
    if (!_folderLoadingBar) {
        _folderLoadingBar = new FolderLoadingBar(this);
    }
    // Position at the top of the scroll area.
    const auto sidebarW = _filterSidebar ? _filterSidebar->width() : 0;
    const auto y = _search ? (_search->y() + _search->height()) : 0;
    _folderLoadingBar->setGeometry(sidebarW, y, width() - sidebarW, 2);
    static_cast<FolderLoadingBar*>(_folderLoadingBar)->start();
}

void DialogsWidget::hideFolderLoading() {
    if (_folderLoadingBar) {
        static_cast<FolderLoadingBar*>(_folderLoadingBar)->stop();
    }
}

void DialogsWidget::updateControlsGeometry() {
    const auto w = width();
    const auto sidebarW = _filterSidebar ? _filterSidebar->width() : 0;
    const auto contentLeft = sidebarW;
    const auto contentWidth = qMax(0, w - contentLeft);
    const auto searchTop = (st::topBarHeight - kSearchBarHeight) / 2;
    const auto searchMarginX = st::dialogsFilterPadding.x();

    if (_filterSidebar) {
        _filterSidebar->setGeometry(0, 0, sidebarW, height());
    }

    // Search bar at top (full width — filter buttons overlay on top).
    const auto searchFieldWidth = qMax(0, contentWidth - 2 * searchMarginX);
    _search->setGeometry(
        contentLeft + searchMarginX,
        searchTop,
        searchFieldWidth,
        kSearchBarHeight);
    _search->raise();

    // Filter icons overlap the search field's right edge.
    // _jumpToDate (calendar, 32px) sits flush at the right edge;
    // _chooseFromUser (29px) goes to its left. We only have fromUser,
    // so position it where the calendar would be — flush right.
    {
        auto right = _search->x() + _search->width() - 2;
        if (_fromUserButton && _fromUserButton->isVisible()) {
            right -= _fromUserButton->width();
            _fromUserButton->move(right, searchTop);
            _fromUserButton->raise();
        }
    }

    // Reposition loading bar under search field.
    if (_folderLoadingBar && _folderLoadingBar->isVisible()) {
        const auto barY = _search ? (_search->y() + _search->height()) : 0;
        _folderLoadingBar->setGeometry(contentLeft, barY, contentWidth, 2);
        _folderLoadingBar->raise();
    }

    auto verificationBannerHeight = 0;
    if (_verificationBanner && _verificationBanner->isVisible()) {
        auto *banner = static_cast<DialogsVerificationBanner *>(_verificationBanner);
        verificationBannerHeight = banner->contentHeight(contentWidth);
        _verificationBanner->setGeometry(
            contentLeft,
            st::topBarHeight,
            contentWidth,
            verificationBannerHeight);
        _verificationBanner->raise();
    }

    auto newLoginBannerHeight = 0;
    if (_newLoginBanner && _newLoginBanner->isVisible()) {
        auto *banner = static_cast<DialogsNewLoginBanner *>(_newLoginBanner);
        newLoginBannerHeight = banner->contentHeight(contentWidth);
        _newLoginBanner->setGeometry(
            contentLeft,
            st::topBarHeight + verificationBannerHeight,
            contentWidth,
            newLoginBannerHeight);
        _newLoginBanner->raise();
    }

    const auto scrollTop =
        st::topBarHeight + verificationBannerHeight + newLoginBannerHeight;

    // Bottom-pinned bars, stacked upwards from the bottom edge like tdesktop's
    // putBottomButton(). The room list gives up the height rather than being
    // overlapped, so the last row stays reachable.
    auto bottomSkip = 0;
    if (_updateTelegram && !_updateTelegram->isHidden()) {
        const auto buttonHeight = _updateTelegram->barHeight();
        bottomSkip += buttonHeight;
        _updateTelegram->setGeometry(
            contentLeft,
            height() - bottomSkip,
            contentWidth,
            buttonHeight);
        _updateTelegram->raise();
    }
    if (_controller) {
        _controller->setConnectingBottomSkip(bottomSkip);
    }

    // Scroll area fills the rest.
    _scroll->setGeometry(
        contentLeft,
        scrollTop,
        contentWidth,
        qMax(0, height() - scrollTop - bottomSkip));
    if (_initialLoadingOverlay) {
        // Cover only the room-list area (right of the rail, below the search
        // bar) and show the centred "Loading…". The dark rail keeps its
        // hamburger and the search bar stays visible/usable; only the folders
        // and the room list are withheld until everything is ready.
        _initialLoadingOverlay->setGeometry(
            contentLeft, scrollTop, contentWidth, qMax(0, height() - scrollTop));
        if (_initialLoadingOverlay->isVisible()) {
            _initialLoadingOverlay->raise();
        }
    }

    // Inner widget width matches scroll area viewport.
    // ScrollArea::resizeEvent already syncs widget width to viewport.
    _inner->setMinimumWidth(contentWidth);
    _inner->setViewportHeight(_scroll->height());
    if (_chatSearchIn && _chatSearchIn->isVisible()) {
        const auto bannerH = _chatSearchIn->contentHeight();
        _chatSearchIn->setGeometry(0, 0, contentWidth, bannerH);
        _chatSearchIn->raise();
        _inner->setMessageSearchMode(true, bannerH);
    } else if (_messageSearchMode == MessageSearchMode::MyMessages) {
        _inner->setMessageSearchMode(true, 0);
    }

}

} // namespace TeleMatrix

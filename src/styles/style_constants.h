// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

// Style constants ported from upstream .style and .palette files.
// These replicate the st:: namespace values that codegen_style would generate.
//
// Color values sourced from: lib_ui/ui/colors.palette (default light theme)
// Layout values sourced from:
//   lib_ui/ui/basic.style
//   intro/intro.style
//   dialogs/dialogs.style
//   ui/chat/chat.style
//   window/window.style
//
// Long-term: replace with codegen_style for automatic DPI scaling.
#pragma once

#include <QDir>
#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QMargins>
#include <QPainter>
#include <QPoint>
#include <QSize>
#include <QString>
#include <QStringList>

#include "../ui/style/runtime_font.h"
#include "../ui/style/runtime_scale.h"

namespace st {

inline QString cssRgba(const QColor &color) {
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

inline QColor withAlpha(const QColor &color, int alpha) {
    auto result = color;
    result.setAlpha(alpha);
    return result;
}

namespace detail {

inline QString pickMacUiTextFamily() {
#ifdef Q_OS_MAC
    const auto candidates = QStringList{
        QStringLiteral("SF Pro Text"),
        QStringLiteral("Helvetica Neue"),
        QStringLiteral("Lucida Grande"),
        QFontDatabase::systemFont(QFontDatabase::GeneralFont).family(),
    };
    for (const auto &candidate : candidates) {
        if (!candidate.isEmpty() && QFontDatabase::hasFamily(candidate)) {
            return candidate;
        }
    }
#endif // Q_OS_MAC
    return QFont().defaultFamily();
}

inline void ensureFontsLoaded() {
    [[maybe_unused]] static const bool loaded = [] {
        const auto basePath = QStringLiteral(":/gui/fonts/");
        const auto files = QDir(basePath).entryList(QDir::Files, QDir::Name);
        for (const auto &file : files) {
            QFontDatabase::addApplicationFont(basePath + file);
        }

        const auto openSans = QStringLiteral("Open Sans");
        QFont::insertSubstitution(openSans, QStringLiteral("Vazirmatn UI NL"));
#ifdef Q_OS_MAC
        const auto macUiText = pickMacUiTextFamily();
        QFont::insertSubstitution(QStringLiteral("Stixgeneral"), macUiText);
        QFont::insertSubstitution(QStringLiteral("STIXGeneral"), macUiText);
        QFont::insertSubstitution(QStringLiteral(".sf Ns Text"), macUiText);
        QFont::insertSubstitution(QStringLiteral(".SF NS Text"), macUiText);
        QFont::insertSubstitution(QStringLiteral(".SFNSText"), macUiText);
        QFont::insertSubstitutions(openSans, QStringList{
            macUiText,
            QStringLiteral("Helvetica Neue"),
            QStringLiteral("Lucida Grande"),
        });
#endif // Q_OS_MAC
        return true;
    }();
}

} // namespace detail

inline QString baseFontFamily() {
    detail::ensureFontsLoaded();
    // Honor the user's chosen font family (set at startup via SetCustomFont, before
    // any st:: font is lazily built) across the whole custom-painted UI, not just
    // qApp's native widgets. Empty = no custom font → the bundled default.
    const auto custom = TeleMatrix::Style::EffectiveFontFamily();
    return custom.isEmpty() ? QStringLiteral("Open Sans") : custom;
}

inline QFont baseFont(int pixelSize, bool semibold = false) {
    detail::ensureFontsLoaded();
    const auto scaled = TeleMatrix::Style::ConvertScale(pixelSize);
    QFont font;
    font.setFamily(baseFontFamily());
    font.setPixelSize(qMax(1, scaled));
    font.setWeight(semibold ? QFont::DemiBold : QFont::Normal);
    return font;
}

inline const QString &monospaceFamily() {
    static const auto family = [] {
        detail::ensureFontsLoaded();

#ifdef Q_OS_WIN
        const auto tryFirst = QStringList{
            QStringLiteral("Consolas"),
            QStringLiteral("Cascadia Mono"),
            QStringLiteral("Courier New"),
        };
#elif defined(Q_OS_MAC)
        const auto tryFirst = QStringList{
            QStringLiteral("Menlo"),
            QStringLiteral("SF Mono"),
            QStringLiteral("Monaco"),
            QStringLiteral("Courier"),
        };
#else // Q_OS_MAC
        const auto tryFirst = QStringList{
            QStringLiteral("Liberation Mono"),
            QStringLiteral("DejaVu Sans Mono"),
            QStringLiteral("Noto Sans Mono"),
            QStringLiteral("Consolas"),
        };
#endif // Q_OS_WIN

        QString manual;
        for (const auto &candidate : tryFirst) {
            if (QFontDatabase::hasFamily(candidate)) {
                manual = candidate;
                break;
            }
        }

        const auto system = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
#ifdef Q_OS_WIN
        const auto useSystem = manual.isEmpty();
#else // Q_OS_WIN
        const auto metrics = QFontMetrics(QFont(system));
        const auto useSystem = manual.isEmpty()
            || (metrics.horizontalAdvance(QChar('i'))
                == metrics.horizontalAdvance(QChar('W')));
#endif // Q_OS_WIN
        return useSystem ? system : manual;
    }();
    detail::ensureFontsLoaded();
    return family;
}

inline QFont monospaceFont(int pixelSize) {
    QFont font;
    font.setFamily(monospaceFamily());
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    font.setPixelSize(TeleMatrix::Style::ConvertScale(pixelSize));
    return font;
}

// ─────────────────────────────────────────────
// FontData: wraps QFont + QFontMetrics for the
// st::font->member access pattern.
// ─────────────────────────────────────────────
class FontData {
public:
    FontData() = default;

    int height = 0;
    int ascent = 0;
    int descent = 0;

    int width(const QString &text) const {
        return _metrics.horizontalAdvance(text);
    }

    operator const QFont &() const { return _font; }
    const QFont &toQFont() const { return _font; }

private:
    QFont _font;
    QFontMetrics _metrics = QFontMetrics(QFont());

    friend class FontPtr;
};

// FontPtr: pointer-like wrapper so st::semiboldFont->height works.
// Uses lazy initialization to defer QFont/QFontMetrics creation until
// first use, so inline const variables are safe before QApplication.
class FontPtr {
public:
    FontPtr(int pixelSize, bool bold = false) noexcept
        : _pixelSize(pixelSize), _bold(bold) {}

    const FontData *operator->() const { ensure(); return &_data; }
    const FontData &operator*() const { ensure(); return _data; }

    // Allow implicit conversion to QFont for QPainter::setFont(st::semiboldFont).
    operator const QFont &() const { ensure(); return _data._font; }

private:
    void ensure() const {
        if (!_init) {
            _data._font = baseFont(_pixelSize, _bold);
            _data._metrics = QFontMetrics(_data._font);
            _data.height = _data._metrics.height();
            _data.ascent = _data._metrics.ascent();
            _data.descent = _data._metrics.descent();
            _init = true;
        }
    }

    int _pixelSize;
    bool _bold;
    mutable FontData _data;
    mutable bool _init = false;
};

// ─────────────────────────────────────────────
// InputFieldStyle: stub for st::dialogsFilter etc.
// Upstream these are complex style structs; we
// use a simple struct with key fields.
// ─────────────────────────────────────────────
struct InputFieldStyle {
    int width = 0;
    int heightMin = 35;
    int heightMax = 35;
    int border = 3;
    int borderActive = 2;
    int borderRadius = 18;
    int borderDenominator = 1;
    int duration = 150;
    int cancelButtonSize = 35;
    Qt::Alignment textAlign = Qt::AlignLeft | Qt::AlignTop;
    Qt::Alignment placeholderAlign = Qt::AlignLeft | Qt::AlignTop;

    QMargins textMargins;
    QMargins placeholderMargins;

    QColor textBg;
    QColor textBgActive;
    QColor textFg;

    QColor placeholderFg;
    QColor placeholderFgActive;

    QColor borderFg;
    QColor borderFgActive;
};

// ─────────────────────────────────────────────
// Base font sizes (from lib_ui/ui/basic.style)
// ─────────────────────────────────────────────
inline int fsize = 13;
inline int boxFontSize = 14;

// Cached font instances (matching st:: naming with -> access).
// FontPtr uses lazy initialization, so these are safe as global variables.
//   st::semiboldFont->height   (arrow access)
//   p.setFont(st::semiboldFont) (implicit QFont conversion)
inline const FontPtr normalFont(fsize);
inline const FontPtr semiboldFont(fsize, true);

// ─────────────────────────────────────────────
// Window palette (from colors.palette)
// ─────────────────────────────────────────────
inline QColor windowBg             = QColor(0xFF, 0xFF, 0xFF); // #ffffff
inline QColor windowFg             = QColor(0x00, 0x00, 0x00); // #000000
inline QColor windowBgOver         = QColor(0xF1, 0xF1, 0xF1); // #f1f1f1
inline QColor windowBgRipple       = QColor(0xE5, 0xE5, 0xE5); // #e5e5e5
inline QColor windowFgOver         = windowFg;
inline QColor windowSubTextFg      = QColor(0x99, 0x99, 0x99); // #999999
inline QColor windowSubTextFgOver  = QColor(0x91, 0x91, 0x91); // #919191
inline QColor windowBoldFg         = QColor(0x22, 0x22, 0x22); // #222222
inline QColor windowBoldFgOver     = QColor(0x22, 0x22, 0x22); // #222222
inline QColor windowBgActive       = QColor(0x40, 0xA7, 0xE3); // #40a7e3
inline QColor windowFgActive       = QColor(0xFF, 0xFF, 0xFF); // #ffffff
inline QColor windowActiveTextFg   = QColor(0x16, 0x8A, 0xCD); // #168acd
inline QColor windowShadowFg       = QColor(0x00, 0x00, 0x00); // #000000
inline QColor windowShadowFgFallback = QColor(0xF1, 0xF1, 0xF1); // #f1f1f1

inline QColor shadowFg             = QColor(0x00, 0x00, 0x00, 0x18); // #00000018

// ─────────────────────────────────────────────
// Scroll bars (from colors.palette)
// ─────────────────────────────────────────────
inline QColor scrollBarBg          = QColor(0x00, 0x00, 0x00, 0x53); // #00000053
inline QColor scrollBarBgOver      = QColor(0x00, 0x00, 0x00, 0x7A); // #0000007a
inline QColor scrollBg             = QColor(0x00, 0x00, 0x00, 0x1A); // #0000001a
inline QColor scrollBgOver         = QColor(0x00, 0x00, 0x00, 0x2C); // #0000002c
inline QColor splitterHandleBg     = QColor(0x00, 0x00, 0x00, 0x17);
inline QColor toolbarSeparatorFg   = QColor(0x00, 0x00, 0x00, 0x18);

// ─────────────────────────────────────────────
// Menu icons (from colors.palette)
// ─────────────────────────────────────────────
inline QColor menuBg               = windowBg;             // #ffffff
inline QColor menuBgOver           = windowBgOver;         // #f1f1f1
inline QColor menuBgRipple         = windowBgRipple;       // #e5e5e5
inline QColor menuIconFg           = QColor(0x99, 0x99, 0x99); // #999999
inline QColor menuIconFgOver       = QColor(0x8A, 0x8A, 0x8A); // #8a8a8a
inline QColor menuFgDisabled       = QColor(0xCC, 0xCC, 0xCC); // #cccccc
inline QColor menuSeparatorFg      = QColor(0xF1, 0xF1, 0xF1); // #f1f1f1
inline QColor menuIconColor        = windowBoldFg;         // #222222

// ─────────────────────────────────────────────
// Window layout (from window.style)
// ─────────────────────────────────────────────
inline int windowMinWidth          = 380;
inline int windowMinHeight         = 480;
inline int windowDefaultWidth      = 800;
inline int windowDefaultHeight     = 600;
inline int windowBigDefaultWidth   = 1024;
inline int windowBigDefaultHeight  = 768;
inline int columnMinimalWidthLeft  = 260;
inline int columnMaximalWidthLeft  = 540;
inline int columnMinimalWidthMain  = 380;

// ─────────────────────────────────────────────
// Dialogs palette (from colors.palette)
// ─────────────────────────────────────────────
inline QColor dialogsBg                = windowBg;             // #ffffff
inline QColor dialogsNameFg            = windowBoldFg;         // #222222
inline QColor dialogsChatIconFg        = dialogsNameFg;
inline QColor dialogsDateFg            = windowSubTextFg;      // #999999
inline QColor dialogsTextFg            = windowSubTextFg;      // #999999
inline QColor dialogsTextFgService     = windowActiveTextFg;   // #168acd
inline QColor dialogsDraftFg           = QColor(0xDD, 0x4B, 0x39); // #dd4b39
inline QColor dialogsVerifiedIconBg    = windowBgActive;
inline QColor dialogsVerifiedIconFg    = windowFgActive;
inline QColor dialogsSendingIconFg     = QColor(0xC1, 0xC1, 0xC1); // #c1c1c1
inline QColor dialogsSentIconFg        = QColor(0x5D, 0xC4, 0x52); // #5dc452
inline QColor dialogsUnreadBg          = windowBgActive;       // #40a7e3
inline QColor dialogsUnreadBgMuted     = QColor(0xBB, 0xBB, 0xBB); // #bbbbbb
inline QColor dialogsUnreadFg          = windowFgActive;       // #ffffff
inline QColor dialogsOnlineBadgeFg     = QColor(0x4D, 0xC9, 0x20); // #4dc920
inline QColor dialogsScamFg            = dialogsDraftFg;
inline QColor dialogsSearchPlaceholderFg = QColor(0xE2, 0xE2, 0xE2);

inline QColor dialogsBgOver            = windowBgOver;         // #f1f1f1
inline QColor dialogsNameFgOver        = windowBoldFgOver;     // #222222
inline QColor dialogsChatIconFgOver    = dialogsNameFgOver;
inline QColor dialogsDateFgOver        = windowSubTextFgOver;  // #919191
inline QColor dialogsTextFgOver        = windowSubTextFgOver;  // #919191
inline QColor dialogsTextFgServiceOver = dialogsTextFgService;
inline QColor dialogsDraftFgOver       = dialogsDraftFg;
inline QColor dialogsSendingIconFgOver = dialogsSendingIconFg;
inline QColor dialogsSentIconFgOver    = QColor(0x58, 0xB8, 0x4D); // #58b84d
inline QColor dialogsUnreadBgOver      = dialogsUnreadBg;
inline QColor dialogsUnreadBgMutedOver = dialogsUnreadBgMuted;
inline QColor dialogsUnreadFgOver      = dialogsUnreadFg;

inline QColor dialogsBgActive            = QColor(0x41, 0x9F, 0xD9); // #419fd9
inline QColor dialogsNameFgActive        = windowFgActive;     // #ffffff
inline QColor dialogsChatIconFgActive    = dialogsNameFgActive;
inline int dialogsChatTypeSkip = 3;
inline QColor dialogsDateFgActive        = windowFgActive;     // #ffffff
inline QColor dialogsTextFgActive        = windowFgActive;     // #ffffff
inline QColor dialogsTextFgServiceActive = dialogsTextFgActive;
inline QColor dialogsDraftFgActive       = QColor(0xC6, 0xE1, 0xF7); // #c6e1f7
inline QColor dialogsSendingIconFgActive = QColor(0xFF, 0xFF, 0xFF, 0x99); // #ffffff99
inline QColor dialogsSentIconFgActive    = dialogsTextFgActive;
inline QColor dialogsUnreadBgActive      = dialogsTextFgActive;
inline QColor dialogsUnreadBgMutedActive = dialogsTextFgActive; // #ffffff
inline QColor dialogsUnreadFgActive      = dialogsBgActive;
inline QColor dialogsOnlineBadgeFgActive = QColor(0xFF, 0xFF, 0xFF); // #ffffff
inline QColor dialogsScamFgActive        = dialogsDraftFgActive;

inline QColor dialogsRippleBg            = windowBgRipple;     // #e5e5e5

// ─────────────────────────────────────────────
// Dialog row layout (from dialogs.style)
// ─────────────────────────────────────────────
inline int dialogsRowHeight    = 62;
inline int dialogsPhotoSize    = 46;
inline int dialogsNameLeft     = 68;
inline int dialogsNameTop      = 10;
inline int dialogsTextLeft     = 68;
inline int dialogsTextTop      = 34;
inline QMargins dialogsPadding     = QMargins(10, 8, 10, 8);

// Unread badge.
inline int dialogsUnreadHeight  = 19;
inline int dialogsUnreadPadding = 5;
inline int dialogsUnreadMarkDiameter = 8;

// Date.
inline int dialogsDateSkip     = 5;

// Send state.
inline int dialogsSendStateSkip = 20;

// Search filter.
inline int dialogsFilterHeight  = 35;
inline int dialogsFilterRadius  = 18;
inline QPoint dialogsFilterPadding  = QPoint(7, 7);
inline int dialogsFilterSkip    = 4;
inline int dialogsVerificationBannerPaddingTop = 14;
inline int dialogsVerificationBannerPaddingBottom = 14;
inline int dialogsVerificationBannerTitleSkip = 5;
inline int dialogsVerificationBannerButtonsSkip = 12;
inline int dialogsVerificationBannerSeparator = 1;
inline int dialogsVerificationBannerButtonTextPadding = 30;
inline int dialogsVerificationBannerButtonGap = 8;
inline int topBarHeight         = 54;

// Search debounce (ms). 900ms suits server round-trips;
// 400ms is fine for local room-list filtering.
inline constexpr int searchRequestDelay   = 400;

// Cancel/cross button in search field.
inline int dialogsCancelSearchCrossSize = 35;
inline QColor dialogsCancelSearchCrossFg    = QColor(0xA8, 0xA8, 0xA8); // #a8a8a8
inline QColor dialogsCancelSearchCrossFgOver = QColor(0x8A, 0x8A, 0x8A); // #8a8a8a

// Search filter icon buttons: choose-from-user 29x35, jump-to-date 32x35.
inline int dialogsSearchFromWidth = 29;
inline int dialogsSearchCalendarWidth = 32;
inline int dialogsSearchFilterHeight = 35;

// ChatSearchIn banner.
inline int searchedBarHeight = 28;
inline QPoint searchedBarPosition = QPoint(14, 5);
inline QColor searchedBarBg = windowBgOver;              // #f1f1f1
inline QColor searchedBarFg = QColor(0x91, 0x91, 0x91); // windowSubTextFgOver
inline int dialogsSearchInHeight = 38;
inline int dialogsSearchInPhotoSize = 28;
inline int dialogsSearchInPhotoPadding = 10;
inline int dialogsSearchInSkip = 10;
inline int dialogsSearchInNameTop = 9;
inline int dialogsSearchInDownTop = 15;
inline int dialogsSearchInDownSkip = 4;
inline int dialogsSearchInCancelWidth = 40;          // dialogsMenuToggle width/height
inline int dialogsSearchInCancelIconPos = 11;        // icon offset within cancel button
inline int dialogsSearchInCheckSkip = 8;

// Chat filters tabs.
inline int chatsFiltersTabsHeight   = 33;
inline int chatsFiltersBarTop       = 30;
inline int chatsFiltersBarStroke    = 6;
inline int chatsFiltersBarRadius    = 2;
inline int chatsFiltersLabelTop     = 7;
inline int chatsFiltersTabPadding   = 9;
inline int chatsFiltersOuterPadding = 9;
inline constexpr int chatsFiltersAnimDuration = 150;
inline QColor chatsFiltersLabelFg       = windowSubTextFg;  // #999999
inline QColor chatsFiltersLabelFgActive = QColor(0x16, 0x8A, 0xCD); // #168acd
inline QColor chatsFiltersBarFgActive   = windowBgActive;   // #40a7e3
inline QColor chatsFiltersRippleBg      = windowBgOver;     // #f1f1f1
inline QColor chatsFiltersRippleBgActive = QColor(0xE3, 0xF1, 0xFA); // #e3f1fa

// Filter sidebar.
inline int sideBarWidth = 72;
inline int sideBarButtonHeight = 62;
inline int sideBarMainButtonHeight = 54;
inline int sideBarTextTop = 40;
inline int sideBarTextSkip = 6;
// Logical font sizes; baseFont() applies ConvertScale once — do NOT pre-scale.
inline int sideBarTextFontSize = 11;
inline int sideBarBadgeFontSize = 12;
inline int sideBarBadgeSkip = 4;
inline int sideBarBadgeHeight = 17;
inline int sideBarBadgeStroke = 2;
inline QPoint sideBarIconPosition(-1, 6);
inline QPoint sideBarMainMenuIconPosition(-1, -1);
inline QPoint sideBarBadgePosition(3, 7);
inline QColor sideBarBg = QColor(0x29, 0x3A, 0x4C);        // #293a4c
inline QColor sideBarBgActive = QColor(0x17, 0x21, 0x2B);  // #17212b
inline QColor sideBarBgRipple = QColor(0x1E, 0x2B, 0x38);  // #1e2b38
inline QColor sideBarTextFg = QColor(0x88, 0x97, 0xA6);     // #8897a6
inline QColor sideBarTextFgActive = QColor(0x64, 0xB9, 0xFA); // #64b9fa
inline QColor sideBarIconFg = QColor(0x83, 0x93, 0xA3);     // #8393a3
inline QColor sideBarIconFgActive = QColor(0x5E, 0xB5, 0xF7); // #5eb5f7
inline QColor sideBarBadgeBg = QColor(0x5E, 0xB5, 0xF7);    // #5eb5f7
inline QColor sideBarBadgeBgMuted = QColor(0x83, 0x93, 0xA3); // #8393a3
inline QColor sideBarBadgeFg = QColor(0xFF, 0xFF, 0xFF);    // #ffffff

// Online badge.
inline int dialogsOnlineBadgeStroke = 2;
inline int dialogsOnlineBadgeSize   = 10;
inline QPoint dialogsOnlineBadgeSkip = QPoint(0, 2);

// Scam label.
inline int dialogsScamRadius = 2;

// Text width minimum.
inline int dialogsTextWidthMin = 150;

// Dialogs fonts (FontPtr for -> access pattern).
inline const FontPtr dialogsTextFont(fsize);
inline const FontPtr dialogsDateFont(13);
inline const FontPtr dialogsUnreadFont(12, true);

// Dialog filter style (mutable so theme manager can update colors).
inline InputFieldStyle dialogsFilter = {
    .heightMin = 35,
    .heightMax = 35,
    .border = 3,
    .borderActive = 2,
    .borderRadius = 18,
    .borderDenominator = 2,
    .duration = 150,
    .cancelButtonSize = 35,
    .textAlign = Qt::AlignLeft | Qt::AlignVCenter,
    .placeholderAlign = Qt::AlignLeft | Qt::AlignVCenter,
    .textMargins = QMargins(12, 8, 30, 5),
    .placeholderMargins = QMargins(5, 0, 2, 0),
    .textBg = windowBgOver,
    .textBgActive = windowBg,
    .textFg = windowFg,
    .placeholderFg = windowSubTextFg,
    .placeholderFgActive = QColor(0xAA, 0xAA, 0xAA),
    .borderFg = windowBgOver,
    .borderFgActive = windowBgRipple,
};

// ─────────────────────────────────────────────
// Intro/Login (from intro.style + colors.palette)
// ─────────────────────────────────────────────
inline int introCoverHeight     = 208;
inline int introStepWidth       = 340;
inline int introStepHeight      = 384;
inline int introStepHeightFull  = 590;
inline int introStepTopMin      = 76;
// Stage furniture, measured from the bottom of the intro window. Absolutely
// positioned and outside every screen's own vertical centring.
inline int introKeysLineBottom  = 52;
inline int introVersionBottom   = 24;
inline int introHeight          = 406;
inline int introNextTop         = 266;
inline int introNextSlide       = 200;
inline int introContentTopAdd   = 30;
inline constexpr int introSlideDuration   = 200;
inline constexpr int introCoverDuration   = 200;

// Intro title/description positions.
inline int introTitleTop        = 1;
inline int introDescriptionTop  = 34;
// Heading -> subtitle gap (redesign: 7px under the h2).
inline int introHeadingGap      = 7;
// Heading (or subtitle) -> first field. Redesign: 24-26px under the h2.
inline int introHeadingToFields = 24;
// Last field -> primary button. Doubled from the old 16 so the action reads as
// separate from the form rather than attached to it.
inline int introFieldsToButton  = 32;
inline int introStepFieldTop    = 96;

// Intro next button: 300px wide, 42px tall, 6px radius.
inline int introNextButtonWidth  = 340;
inline int introNextButtonHeight = 46;
inline int introNextButtonRadius = 9;
inline int introNextButtonTextTop = 11;

// Intro input fields: 300px wide, 61px min height.
inline int introCountryWidth     = 340;
inline int introCountryHeight    = 44;
inline int introPhoneWidth       = 225;
inline int introCountryCodeWidth = 64;

// Intro code digits.
inline int introCodeDigitHeight      = 50;
inline int introCodeDigitBorderWidth  = 4;
inline int introCodeDigitSkip         = 10;

// Intro error position.
inline int introErrorTop             = 235;
inline int introErrorBelowLinkTop    = 220;

// Intro cover title/description positions.
inline int introCoverTitleTop        = 136;
inline int introCoverDescriptionTop  = 174;
inline int introCoverMaxWidth        = 880;

// Intro back button (top-left arrow to return from login to welcome).
inline int introBackButtonSize       = 56;

// Intro field spacing (between stacked QLineEdits).
inline int introFieldSpacing         = 16;

// Intro fonts.
inline const FontPtr introTitleFont(17, true);
inline int introDescriptionLineHeight = 20;
inline const FontPtr introCoverTitleFont(22, true);
inline const FontPtr introCoverDescriptionFont(15);

// Intro colors (from colors.palette).
inline QColor introBg              = windowBg;
inline QColor introTitleFg         = windowBoldFg;             // #222222
inline QColor introDescriptionFg   = windowSubTextFg;          // #999999
inline QColor introCoverTopBg      = QColor(0x0F, 0x89, 0xD0); // #0f89d0
inline QColor introCoverBottomBg   = QColor(0x39, 0xB0, 0xF0); // #39b0f0
inline QColor introCoverIconsFg    = QColor(0x5E, 0xC6, 0xFF); // #5ec6ff
inline QColor introCoverTitleFg    = QColor(0xFF, 0xFF, 0xFF); // #ffffff
inline QColor introCoverDescFg     = QColor(0xFF, 0xFF, 0xFF, 0xB3); // #ffffff @ 70% opacity
inline QColor introButtonFg        = windowFgActive;           // #ffffff
inline QColor introButtonDisabledBg = menuFgDisabled;          // #cccccc
inline QColor introInputDisabledBg = QColor(0xF5, 0xF5, 0xF5); // #f5f5f5
inline QColor introInputDisabledFg = QColor(0xAA, 0xAA, 0xAA); // #aaaaaa

// ─────────────────────────────────────────────
// Login footer links
// ─────────────────────────────────────────────
inline int introLinkTop              = 12;  // Space below Sign In button to link row
inline int introLinkGap              = 24;  // Horizontal gap between footer links
inline int introDiscoveryFontSize    = 11;  // Caption text below homeserver field

// ─────────────────────────────────────────────
// Verification cards (IntroVerifyChoice)
// ─────────────────────────────────────────────
inline int introVerifyCardWidth      = 300;
inline int introVerifyCardHeight     = 60;
inline int introVerifyCardRadius     = 8;
inline int introVerifyCardTitleSize  = 13;
inline int introVerifyCardSubSize    = 12;
inline int introVerifyCardTextGap    = 2;
inline int introVerifySkipTop        = 20;

// ─────────────────────────────────────────────
// Emoji verification (IntroVerifyEmoji)
// ─────────────────────────────────────────────
inline int introVerifyEmojiSize       = 48;
inline int introVerifyEmojiFontSize   = 32;
inline int introVerifyEmojiLabelSize  = 11;
inline int introVerifyEmojiPadding    = 4;
inline int introVerifyEmojiCellGap    = 2;
inline int introVerifyEmojiContainerW = 450;
inline int introVerifyEmojiContainerH = 160;
inline int introVerifyEmojiContainerR = 12;
inline int introVerifyMismatchTop     = 16;

// ─────────────────────────────────────────────
// Recovery key input (IntroVerifyRecoveryKey)
// ─────────────────────────────────────────────
inline int introRecoveryKeyWidth      = 492;
inline int introRecoveryKeyHeight     = 36;
inline int introRecoveryKeyFontSize   = 12;
inline int introRecoveryKeyErrorTop   = 8;
inline int introRecoveryKeyErrorHeight = 20;
inline int introRecoveryKeyButtonGap  = 10;

// ─────────────────────────────────────────────
// Success screen (IntroVerifySuccess)
// ─────────────────────────────────────────────
inline int introVerifyCheckSize       = 64;
inline QColor introVerifySuccessBg        = QColor(0x5D, 0xC4, 0x52); // #5dc452

// ─────────────────────────────────────────────
// Message/Chat layout (from chat.style)
// ─────────────────────────────────────────────
inline int msgMaxWidth    = 430;
inline int msgMinWidth    = 160;
inline int msgPhotoSize   = 33;
inline int msgPhotoSkip   = 40;
inline QMargins msgPadding    = QMargins(11, 8, 11, 8);
inline QMargins msgMargin     = QMargins(16, 6, 56, 2);
inline int msgMarginTopAttached = 0;
inline int msgShadow      = 2;
inline int msgDateSpace   = 12;
inline QPoint msgDateDelta    = QPoint(2, 5);
inline int msgDateImgDelta = 4;
inline QPoint msgDateImgPadding = QPoint(8, 2);
inline int mediaCaptionSkip = 5;
inline int minPhotoSize   = 100;
inline int maxMediaSize   = 430;

// Bubble radii.
inline int bubbleRadiusSmall = 6;   // roundRadiusLarge
inline int bubbleRadiusLarge = 16;

// History layout.
inline int historyPhotoLeft    = 14;
inline int historyPhotoBubbleMinWidth = 200;
inline int historyMinimalWidth = 380;
inline int historyPaddingBottom = 8;

// Unread bar.
inline int historyUnreadBarHeight = 32;
inline int historyUnreadBarMargin = 8;

// Service message.
inline QMargins msgServicePadding = QMargins(12, 3, 12, 4);
inline QMargins msgServiceMargin  = QMargins(10, 10, 10, 2);

// Reply.
inline int historyReplyTop     = 2;
inline int historyReplyBottom  = 2;
inline int historyReplyPreview = 32;
inline int historyReplyHeight  = 49;
inline int historyReplySkip    = 53;
inline QPoint historyReplyIconPosition(7, 7);
inline QMargins msgReplyPadding(6, 6, 11, 6);
// historyReplyPadding: text positioning within the reply block.
inline QMargins historyReplyPadding(11, 2, 6, 2);
inline QPoint msgReplyBarPos(1, 0);
inline QSize msgReplyBarSize(2, 36);
inline int msgReplyBarSkip = 10;
inline QMargins historyReplyPreviewMargin(7, 4, 4, 4);

// Message fonts.
inline const FontPtr msgFont(fsize);
inline const FontPtr msgDateFont(13);
inline const FontPtr msgNameFont(fsize, true);
inline const FontPtr msgServiceFont(13, true);
inline const FontPtr msgServiceNameFont(13, true);
inline const FontPtr historyForwardChooseFont(16);

// ─────────────────────────────────────────────
// Message colors (from colors.palette)
// ─────────────────────────────────────────────

// Bubble backgrounds.
inline QColor msgInBg              = windowBg;                  // #ffffff
inline QColor msgInBgSelected      = QColor(0xC2, 0xDC, 0xF2); // #c2dcf2
inline QColor msgOutBg             = QColor(0xEF, 0xFD, 0xDE); // #effdde
inline QColor msgOutBgSelected     = QColor(0xB7, 0xDB, 0xDB); // #b7dbdb

inline QColor utdBg                = QColor(0xFE, 0xF4, 0xE5);
inline QColor utdTitleFg           = QColor(0x1B, 0x1B, 0x1C);
inline QColor utdBodyFg            = QColor(0x41, 0x47, 0x4F);
inline QColor utdLinkFg            = QColor(0x20, 0x63, 0x90);

// Bubble shadows (2px below bubble, semi-transparent).
inline QColor msgInShadow          = QColor(0x74, 0x8E, 0xA2, 0x29); // #748ea229
inline QColor msgInShadowSelected  = QColor(0x54, 0x8D, 0xBB, 0x29); // #548dbb29
inline QColor msgOutShadow         = QColor(0x3A, 0xC3, 0x46, 0x1D); // #3ac3461d
inline QColor msgOutShadowSelected = QColor(0x37, 0xA7, 0x8D, 0x22); // #37a78d22

// Bubble selection overlay.
inline QColor msgSelectOverlay     = QColor(0x35, 0x8C, 0xD4, 0x4C); // #358cd44c
inline QColor msgStickerOverlay    = QColor(0x35, 0x8C, 0xD4, 0x7F); // #358cd47f

// Service/info text inside messages.
inline QColor msgInServiceFg           = windowActiveTextFg;    // #168acd
inline QColor msgInServiceFgSelected   = windowActiveTextFg;    // #168acd
inline QColor msgOutServiceFg          = QColor(0x45, 0xA3, 0x2D); // #45a32d
inline QColor msgOutServiceFgSelected  = QColor(0x46, 0x99, 0x92); // #469992

// Timestamp text.
inline QColor msgInDateFg              = QColor(0xA0, 0xAC, 0xB6); // #a0acb6
inline QColor msgInDateFgSelected      = QColor(0x6A, 0x9C, 0xC5); // #6a9cc5
inline QColor msgOutDateFg             = QColor(0x6D, 0xB5, 0x66); // #6db566
inline QColor msgOutDateFgSelected     = QColor(0x56, 0xB2, 0xA6); // #56b2a6

// Date/time media overlay.
inline QColor msgDateImgFg             = windowFgActive;        // #ffffff (msgServiceFg)
inline QColor msgDateImgBg             = QColor(0x00, 0x00, 0x00, 0x54); // #00000054
inline QColor msgDateImgBgOver         = QColor(0x00, 0x00, 0x00, 0x74); // #00000074
inline QColor msgDateImgBgSelected     = QColor(0x1C, 0x4A, 0x71, 0x87); // #1c4a7187

// Service messages (date dividers, group actions).
inline QColor msgServiceFg             = windowFgActive;        // #ffffff
inline QColor msgServiceBg             = QColor(0x51, 0x7C, 0x41, 0x7F); // #517c417f (adjusted)
inline QColor msgServiceBgSelected     = QColor(0x96, 0xB3, 0x8B, 0xA2); // #96b38ba2 (adjusted)

// Reply bar.
inline QColor activeLineFg             = QColor(0x37, 0xA1, 0xDE); // #37a1de
inline QColor activeLineFgError        = QColor(0xE4, 0x83, 0x83); // #e48383
inline QColor msgInReplyBarColor       = activeLineFg;          // #37a1de
inline QColor msgInReplyBarSelColor    = activeLineFg;          // #37a1de
inline QColor msgOutReplyBarColor      = QColor(0x5E, 0xB8, 0x54); // #5eb854
inline QColor msgImgReplyBarColor      = msgServiceFg;          // #ffffff

// Monospace text.
inline QColor msgInMonoFg              = QColor(0x4E, 0x73, 0x91); // #4e7391
inline QColor msgOutMonoFg             = QColor(0x45, 0x98, 0x66); // #459866

// ─────────────────────────────────────────────
// History view colors (from colors.palette)
// ─────────────────────────────────────────────
inline QColor historyBg                = QColor(0xA9, 0xC5, 0x95); // average of 4-color gradient
inline QColor historyBgTopLeft         = QColor(0xDB, 0xDD, 0xBB);
inline QColor historyBgTopRight        = QColor(0x6B, 0xA5, 0x87);
inline QColor historyBgBottomRight     = QColor(0xD5, 0xD8, 0x8D);
inline QColor historyBgBottomLeft      = QColor(0x88, 0xB8, 0x84);
inline QColor topBarBg                 = windowBg;              // #ffffff

inline QColor historyTextInFg          = windowFg;              // #000000
inline QColor historyTextOutFg         = windowFg;              // #000000
inline QColor historyLinkInFg          = windowActiveTextFg;    // #168acd
inline QColor historyLinkInFgSelected  = historyLinkInFg;       // #168acd (night: #aadcff)
inline QColor historyLinkOutFg         = QColor(0x4B, 0x94, 0x32); // #4b9432
inline QColor historyLinkOutFgSelected = QColor(0x32, 0x94, 0x6B); // #32946b (night: #aadcff)

// Tick / double tick icons.
inline QColor historyOutIconFg         = QColor(0x57, 0xB8, 0x4C); // #57b84c
inline QColor historyOutIconFgSelected = QColor(0x45, 0xA3, 0xAA); // #45a3aa
inline QColor historyIconFgInverted    = windowFgActive;        // #ffffff

// Sending state icons.
inline QColor historySendingOutIconFg   = QColor(0x98, 0xD2, 0x92); // #98d292
inline QColor historySendingInIconFg    = QColor(0xA0, 0xAD, 0xB5); // #a0adb5
inline QColor historySendingInvertedIconFg = QColor(0xFF, 0xFF, 0xFF, 0xC8); // #ffffffc8

// Unread bar.
inline QColor historyUnreadBarBg       = QColor(0xFC, 0xFB, 0xFA); // #fcfbfa
inline QColor historyUnreadBarBorder   = shadowFg;              // #00000018
inline QColor historyUnreadBarFg       = QColor(0x53, 0x8B, 0xB4); // #538bb4
inline const FontPtr historyUnreadBarFont    = semiboldFont;          // historyUnreadBarFont: semiboldFont

// Forward choose.
inline QColor historyForwardChooseBg   = QColor(0x00, 0x00, 0x00, 0x4C); // #0000004c
inline QColor historyForwardChooseFg   = windowFgActive;        // #ffffff
inline QMargins historyForwardChooseMargins(30, 10, 30, 10);

// Scroll bar in chat view (adjusted colors).
inline QColor historyScrollBarBg       = QColor(0x51, 0x7C, 0x41, 0x7A); // #517c417a
inline QColor historyScrollBarBgOver   = QColor(0x51, 0x7C, 0x41, 0xBC); // #517c41bc
inline QColor historyScrollBg          = QColor(0x51, 0x7C, 0x41, 0x4C); // #517c414c
inline QColor historyScrollBgOver      = QColor(0x51, 0x7C, 0x41, 0x6B); // #517c416b

// Scroll-to-bottom button.
inline QColor historyToDownBg          = windowBg;              // #ffffff
inline QColor historyToDownBgOver      = windowBgOver;          // #f1f1f1
inline QColor historyToDownBgRipple    = windowBgRipple;        // #e5e5e5
inline QColor historyToDownFg          = menuIconFg;            // #999999
inline QColor historyToDownFgOver      = menuIconFgOver;        // #8a8a8a
inline QColor historyToDownShadow      = QColor(0x00, 0x00, 0x00, 0x40); // #00000040

// Compose area (message input).
inline QColor historyComposeAreaBg     = windowBg;              // msgInBg = windowBg
inline QColor historyComposeAreaFg     = windowFg;              // historyTextInFg
inline QColor historyComposeAreaFgService = msgInDateFg;        // #a0acb6
inline QColor historyComposeIconFg     = menuIconFg;            // #999999
inline QColor historyComposeIconFgOver = menuIconFgOver;        // #8a8a8a
inline QColor historyComposeIconFgDisabled = windowSubTextFg;   // #999999
inline QColor historySendIconFg        = windowBgActive;        // #40a7e3
inline QColor historySendIconFgOver    = windowBgActive;        // #40a7e3
inline QColor historyRecordVoiceFg     = historyComposeIconFg;
inline QColor historyRecordVoiceFgOver = historyComposeIconFgOver;
inline QColor historyRecordVoiceFgInactive = QColor(0xD1, 0x4E, 0x4E); // attentionButtonFg
inline QColor historyRecordVoiceFgActive = windowBgActive;
inline QColor historyRecordVoiceFgActiveIcon = windowFgActive;
inline int historyRecordSignalRadius = 5;
inline int historyRecordDurationSkip = 12;
inline int historyRecordMainBlobMinRadius = 23;
inline int historyRecordTextLeft = 15;
inline QColor historyRecordCancel = windowSubTextFg;
inline QColor historyRecordCancelActive = historySendIconFg;
inline QColor historyRecordDurationFg = historyComposeAreaFg;
inline QFont historyRecordFont() {
    return baseFont(13);
}
inline QColor historyPinnedBg          = windowBg;              // historyComposeAreaBg
inline QColor historyReplyBg           = windowBg;              // historyComposeAreaBg
inline QColor historyReplyNameFg       = windowActiveTextFg;    // #168acd
inline QColor historyReplyIconFg       = windowBgActive;        // #40a7e3
inline QColor historyReplyCancelFg     = menuIconFg;            // cancelIconFg = menuIconFg
inline QColor historyReplyCancelFgOver = menuIconFgOver;        // cancelIconFgOver
inline QColor historyFastReplyShadowBg = QColor(0x00, 0x00, 0x00, 0x14); // #00000014
inline QColor historyFastReplyBorderFg = QColor(0x00, 0x00, 0x00, 0x0F); // #0000000f
inline QColor historyPinIconFg         = msgInDateFg;           // #a0acb6

inline constexpr int historyScrollDateHideTimeout = 1000;
inline constexpr int historyDateFadeDuration = 200;

// Media viewer overlay (from media_view.style + colors.palette).
inline QColor mediaviewBg              = QColor(0x22, 0x22, 0x22, 0xEB); // #222222eb
inline QColor mediaviewControlFg       = QColor(0xFF, 0xFF, 0xFF);       // #ffffff
inline QColor mediaviewControlBg       = QColor(0x00, 0x00, 0x00, 0x3C); // #0000003c
inline QColor mediaviewVideoLoadingBg  = QColor(0x00, 0x00, 0x00, 0x78); // #00000078
inline QColor mediaviewCaptionBg       = QColor(0x11, 0x11, 0x11, 0x80); // #11111180
inline QColor mediaviewCaptionFg       = mediaviewControlFg;
inline QColor mediaviewShadowTop       = QColor(0x00, 0x00, 0x00, 0x70);
inline QColor mediaviewShadowBottom    = QColor(0x00, 0x00, 0x00, 0x80);

inline int mediaviewControlSize    = 90;
inline int mediaviewHeaderTop      = 47;
inline int mediaviewTextTop        = 26;
inline int mediaviewTextLeft       = 14;
inline int mediaviewTextSkip       = 10;
inline int mediaviewIconW          = 46;
inline int mediaviewIconH          = 54;
inline int mediaviewIconOver       = 36;
inline int mediaviewCaptionRadius  = 6;
inline int mediaviewCaptionPaddingH = 11;
inline int mediaviewCaptionPaddingV = 6;
inline int mediaviewCaptionMargin  = 11;
inline constexpr int mediaviewWaitHide       = 2000;
inline constexpr int mediaviewShowDuration   = 200;
inline constexpr int mediaviewHideDuration   = 1000;
inline constexpr int mediaviewOverDuration   = 150;
inline constexpr double mediaviewMaxIconOpacity = 0.6;
inline constexpr double mediaviewNormalIconOpacity = 0.9;
inline constexpr double mediaviewOverBgOpacity = 0.2775;
inline int mediaviewDefaultWidth   = 800;
inline int mediaviewDefaultHeight  = 600;
inline int mediaviewMinWidth       = 480;
inline int mediaviewMinHeight      = 360;
inline int mediaviewControllerWidth = 480;
inline int mediaviewControllerHeight = 72;
inline int mediaviewControllerRadius = 9;
inline int mediaviewControllerBottom = 6;
inline int mediaviewPlayButtonTop = 2;
inline int mediaviewPlayButtonSize = 40;
inline int mediaviewSmallButtonSize = 32;
inline int mediaviewButtonsTop = 6;
inline int mediaviewButtonsRight = 8;
inline int mediaviewVolumeLeft = 6;
inline int mediaviewVolumeSkip = 3;
inline int mediaviewVolumeWidth = 75;
inline int mediaviewPlaybackTop = 49;
inline int mediaviewProgressTop = 46;
inline int mediaviewProgressSkip = 10;
inline int mediaviewSeekTrackHeight = 3;
inline int mediaviewSeekHandleSize = 12;
inline constexpr int mediaviewSeekStepMs = 5000;

inline QColor mediaviewPlaybackActive = QColor(0xC7, 0xC7, 0xC7);      // #c7c7c7
inline QColor mediaviewPlaybackInactive = QColor(0x25, 0x25, 0x25);    // #252525
inline QColor mediaviewPlaybackActiveOver = QColor(0xFF, 0xFF, 0xFF);  // #ffffff
inline QColor mediaviewPlaybackInactiveOver = QColor(0x47, 0x47, 0x47);// #474747
inline QColor mediaviewPlaybackProgressFg = QColor(0xFF, 0xFF, 0xFF, 0xC7); // #ffffffc7
inline QColor mediaviewPlaybackIconFg = QColor(0xC7, 0xC7, 0xC7);      // #c7c7c7
inline QColor mediaviewPlaybackIconFgOver = QColor(0xFF, 0xFF, 0xFF);  // #ffffff
inline QColor mediaviewPlaybackBg = QColor(0x00, 0x00, 0x00, 0xB2);    // #000000b2

// Reactions (chat.style + chat_helpers.style).
inline QMargins reactionInlinePadding(5, 2, 7, 2);
inline int reactionInlineSize = 18;
// Visual size of the emoji inside that 18px slot. Not the same number: the slot is
// the layout box, the emoji sits inside it with a margin. A sprite cell is
// edge-to-edge artwork, so drawing one at reactionInlineSize would fill the whole
// slot and crowd the pill — the old text path rendered a ~14px glyph from a 12px font.
inline int reactionInlineEmoji = 15;
inline int reactionInlineImage = 32;
inline int reactionInlineSkip = 3;
inline int reactionInlineTagSkip = 6;
inline int reactionInlineTagLeftRadius = 6;
inline int reactionInlineTagRightRadius = 3;
inline int reactionInlineTagArrow = 5;
inline int reactionInlineTagDot = 5;
inline int reactionInlineTagDotSkip = 2;
inline int reactionInlineEmptySkip = 2;
inline const FontPtr reactionInlineTagFont(12);
inline QPoint reactionInlineTagNamePosition(26, 2);
inline QPoint reactionInlineTagPromoPosition(20, 2);
inline int reactionInlineBetween = 4;
inline int reactionInlineInBubbleLeft = -3;
inline QMargins reactionInlineUserpicsPadding(1, 1, 1, 1);

inline int reactionInfoSize = 15;
inline int reactionInfoImage = 30;
inline int reactionInfoSkip = 3;
inline int reactionInfoDigitSkip = 6;
inline int reactionInfoBetween = 3;

inline QSize reactionCornerSize(36, 32);
inline QPoint reactionCornerCenter(7, -9);
inline int reactionCornerImage = 22;
// Visual size of the emoji inside the corner button / reaction column cell.
// reactionCornerImage is the image slot; the emoji sits inside it. Measured against
// what the old text path rendered (a 15px font produced an 18px glyph).
inline int reactionCornerEmoji = 18;
inline QMargins reactionCornerShadow(4, 8, 4, 8);
inline QMargins reactionCornerActiveAreaPadding(10, 10, 10, 10);
inline int reactionCornerAddedHeightMax = 100;
inline int reactionCornerSkip = -4;
inline int reactionExpandedSkip = 2;
inline int reactionGradientStart = 8;
inline int reactionGradientSize = 24;
inline int reactionGradientFadeSize = 24;
inline int reactionAppearStartSkip = 2;
inline int reactionMainAppearShift = 20;
inline int reactionCollapseFadeThreshold = 40;
inline int reactionFlyUp = 50;

inline constexpr qreal reactionInNonChosenOpacity = 0.12;
inline constexpr qreal reactionOutNonChosenOpacity = 0.18;
inline QColor reactionMediaShadowBg = QColor(0x00, 0x00, 0x00, 0x20); // #00000020

inline QColor emojiPanBg = windowBg;
inline QColor emojiPanCategories = QColor(0xF7, 0xF7, 0xF7);
inline QColor emojiPanHeaderFg = windowSubTextFg;
inline QColor emojiPanHover = windowBgOver;
inline QColor emojiIconFg = QColor(0x99, 0x99, 0x99);
inline QColor emojiIconFgActive = QColor(0x66, 0x66, 0x66);
inline int emojiPanRadius = 8;

inline int reactStripHeight = 40;
inline int reactStripSize = 32;
inline int reactStripMinWidth = 60;
inline int reactStripImage = 26;
inline int reactStripSkip = 7;
inline int reactStripBubbleRight = 20;

inline int mediaInBubbleSkip = 5;

// Mention autocomplete (from chat.style lines 762-774).
inline int mentionHeight       = 40;
inline QMargins mentionPadding     = QMargins(8, 5, 8, 5);
inline int mentionTop          = 11;
inline int mentionPhotoSize    = 33;
inline QColor mentionNameFg        = windowFg;            // #000000
inline QColor mentionNameFgOver    = windowFgOver;        // #000000
inline QColor mentionBg            = windowBg;            // #ffffff
inline QColor mentionBgOver        = windowBgOver;        // #f1f1f1
inline QColor mentionFg            = windowSubTextFg;     // #999999
inline QColor mentionFgOver        = windowSubTextFgOver; // #919191
inline QColor mentionFgActive      = windowActiveTextFg;  // #168acd
inline QColor mentionFgOverActive  = windowActiveTextFg;  // #168acd

inline int webPagePhotoDelta = 8;

// ─────────────────────────────────────────────
// Peer name colors — 8 predefined sender colors
// (from colors.palette: historyPeerNNameFg)
// ─────────────────────────────────────────────
inline QColor historyPeer1NameFg = QColor(0xC0, 0x3D, 0x33); // #c03d33 red
inline QColor historyPeer2NameFg = QColor(0x4F, 0xAD, 0x2D); // #4fad2d green
inline QColor historyPeer3NameFg = QColor(0xD0, 0x93, 0x06); // #d09306 yellow
inline QColor historyPeer4NameFg = windowActiveTextFg;        // #168acd blue
inline QColor historyPeer5NameFg = QColor(0x85, 0x44, 0xD6); // #8544d6 purple
inline QColor historyPeer6NameFg = QColor(0xCD, 0x40, 0x73); // #cd4073 pink
inline QColor historyPeer7NameFg = QColor(0x29, 0x96, 0xAD); // #2996ad sea
inline QColor historyPeer8NameFg = QColor(0xCE, 0x67, 0x1B); // #ce671b orange

// ─────────────────────────────────────────────
// Userpic background colors — 8 predefined colors
// (from colors.palette: historyPeerNUserpicBg)
// ─────────────────────────────────────────────
inline QColor peerUserpicBg1 = QColor(0xFF, 0x84, 0x5E); // #ff845e red
inline QColor peerUserpicBg2 = QColor(0x9A, 0xD1, 0x64); // #9ad164 green
inline QColor peerUserpicBg3 = QColor(0xE5, 0xCA, 0x77); // #e5ca77 yellow
inline QColor peerUserpicBg4 = QColor(0x5C, 0xAF, 0xFA); // #5caffa blue
inline QColor peerUserpicBg5 = QColor(0xB6, 0x94, 0xF9); // #b694f9 purple
inline QColor peerUserpicBg6 = QColor(0xFF, 0x8A, 0xAC); // #ff8aac pink
inline QColor peerUserpicBg7 = QColor(0x5B, 0xCB, 0xE3); // #5bcbe3 sea
inline QColor peerUserpicBg8 = QColor(0xFE, 0xBB, 0x5B); // #febb5b orange
inline QColor historyPeerUserpicFg = windowFgActive;          // #ffffff

// Second userpic gradient colors (historyPeerNUserpicBg2).
inline QColor peerUserpicBg1_2 = QColor(0xD4, 0x52, 0x46); // #d45246
inline QColor peerUserpicBg2_2 = QColor(0x46, 0xBA, 0x43); // #46ba43
inline QColor peerUserpicBg3_2 = QColor(0xE5, 0xCA, 0x77); // #e5ca77
inline QColor peerUserpicBg4_2 = QColor(0x40, 0x8A, 0xCF); // #408acf
inline QColor peerUserpicBg5_2 = QColor(0x6C, 0x61, 0xDF); // #6c61df
inline QColor peerUserpicBg6_2 = QColor(0xD9, 0x55, 0x74); // #d95574
inline QColor peerUserpicBg7_2 = QColor(0x35, 0x9A, 0xD4); // #359ad4
inline QColor peerUserpicBg8_2 = QColor(0xF6, 0x81, 0x36); // #f68136

// ─────────────────────────────────────────────
// Active button colors (from colors.palette)
// ─────────────────────────────────────────────
inline QColor activeButtonBg           = windowBgActive;        // #40a7e3
inline QColor activeButtonBgOver       = QColor(0x39, 0xA5, 0xDB); // #39a5db
inline QColor activeButtonBgRipple     = QColor(0x20, 0x95, 0xD0); // #2095d0
inline QColor activeButtonFg           = windowFgActive;        // #ffffff
inline QColor activeButtonFgOver       = windowFgActive;        // #ffffff
inline QColor activeButtonSecondaryFg  = QColor(0xCC, 0xEE, 0xFF); // #cceeff

// ─────────────────────────────────────────────
// Light button colors (from colors.palette)
// ─────────────────────────────────────────────
inline QColor lightButtonBg            = windowBg;              // #ffffff
inline QColor lightButtonBgOver        = QColor(0xE3, 0xF1, 0xFA); // #e3f1fa
inline QColor lightButtonBgRipple      = QColor(0xC9, 0xE4, 0xF6); // #c9e4f6
inline QColor lightButtonFg            = windowActiveTextFg;    // #168acd
inline QColor lightButtonFgOver        = lightButtonFg;

// ─────────────────────────────────────────────
// Attention button colors (from colors.palette)
// ─────────────────────────────────────────────
inline QColor attentionButtonFg        = QColor(0xD1, 0x4E, 0x4E); // #d14e4e
inline QColor attentionButtonFgOver    = QColor(0xD1, 0x4E, 0x4E); // #d14e4e
inline QColor attentionButtonBgOver    = QColor(0xFC, 0xDF, 0xDE); // #fcdfde
inline QColor attentionButtonBgRipple  = QColor(0xF4, 0xC3, 0xC2); // #f4c3c2

// ─────────────────────────────────────────────
// Placeholder / input colors (from colors.palette)
// ─────────────────────────────────────────────
inline QColor placeholderFg            = windowSubTextFg;       // #999999
inline QColor placeholderFgActive      = QColor(0xAA, 0xAA, 0xAA); // #aaaaaa
inline QColor inputBorderFg            = QColor(0xE0, 0xE0, 0xE0); // #e0e0e0
inline QColor filterInputBorderFg      = QColor(0x54, 0xC3, 0xF3); // #54c3f3
inline QColor filterInputActiveBg      = windowBg;              // #ffffff
inline QColor filterInputInactiveBg    = windowBgOver;          // #f1f1f1

// ─────────────────────────────────────────────
// Box layout (from layers.style)
// ─────────────────────────────────────────────
inline int boxRadius               = 8;                     // boxRadius: 8px
inline int boxWidth                = 320;                   // boxWidth: 320px (layers.style)
inline int boxWideWidth            = 364;                   // boxWideWidth: 364px
// Add-account popup card. Wide enough that the intro's fixed-width in-login
// verification steps (recovery-key input 492px) are not clipped; height clamped
// to the window at runtime.
inline int addAccountBoxWidth      = 540;
inline int addAccountBoxHeight     = 620;
inline int signOutConfirmWidth     = 350;
inline int boxMaxListHeight        = 492;                   // boxMaxListHeight: 492px
inline int boxTitleHeight          = 48;                    // boxTitleHeight: 48px
inline QPoint boxTitlePosition(24, 13);                         // boxTitlePosition: point(24px, 13px)
inline const FontPtr boxTitleFont(16, true);                          // boxTitleFont: font(16px semibold)
inline QMargins boxButtonPadding(6, 10, 10, 10);               // defaultBox.buttonPadding
inline int boxButtonHeight         = 34;                    // defaultBox.buttonHeight: 34px
inline int boxButtonMinWidth       = 30;                    // defaultBoxButton.width: -30px (auto: textWidth + 30)
inline int buttonRadius            = 4;                     // buttonRadius: 4px (widgets.style)
inline const FontPtr boxButtonFont(14, true);                         // boxButtonFont: font(14px semibold)
inline QMargins boxPadding(24, 14, 24, 8);                     // boxPadding: margins(24px, 14px, 24px, 8px)
inline int boxTopMargin            = 8;                     // boxTopMargin: 8px (content top when no title)
inline int boxLabelLineHeight      = 22;                    // boxLabelStyle.lineHeight: 22px

// Box colors (from colors.palette)
// ─────────────────────────────────────────────
inline QColor boxBg                    = windowBg;              // #ffffff
inline QColor boxTextFg                = windowFg;              // #000000
inline QColor boxTextFgGood            = QColor(0x4A, 0xB4, 0x4A); // #4ab44a
inline QColor boxTextFgError           = QColor(0xD8, 0x4D, 0x4D); // #d84d4d
// Amber caution used by the "verified but has unverified sessions" trust badge.
inline QColor trustWarningFg           = QColor(0xC8, 0x8A, 0x00); // #c88a00
inline QColor boxTitleFg               = QColor(0x40, 0x40, 0x40); // #404040
inline QColor boxTitleCloseFg          = menuIconFg;            // #999999 cancelIconFg
inline QColor boxTitleCloseFgOver      = menuIconFgOver;        // #8a8a8a cancelIconFgOver
inline QColor boxSearchBg              = boxBg;                 // boxSearchBg: boxBg
inline QColor boxDividerBg             = windowBgOver;          // #f1f1f1
inline QColor layerBg                  = QColor(0x00, 0x00, 0x00, 0x7F); // layerBg: #0000007f

// Layer shadow (extend: 10px).
inline int layerShadowExtend       = 10;
inline int layerVerticalMargin     = 20;  // vertical padding around layer panel

// ShareBox (forward dialog) — from boxes.style
inline int shareRowsTop            = 12;                    // shareRowsTop: 12px
inline int shareRowHeight          = 108;                   // shareRowHeight: 108px
inline int sharePhotoTop           = 6;                     // sharePhotoTop: 6px
inline int shareNameTop            = 6;                     // shareNameTop: 6px
inline int shareColumnSkip         = 6;                     // shareColumnSkip: 6px
inline int shareColumnCount        = 4;
inline int shareImageRadius        = 28;                    // shareBoxListItem.checkbox.imageRadius: 28px
inline constexpr int shareActivateDuration   = 150;                   // shareActivateDuration: 150
inline const FontPtr shareNameFont(11);                               // shareBoxListItem.nameStyle.font: 11px
inline QMargins boxSearchPadding(8, 6, 8, 6);                  // defaultMultiSelect.padding
inline int boxSearchFieldHeight    = 32;                    // defaultMultiSelectSearchField.heightMin: 32px

// ─────────────────────────────────────────────
// Call arrow colors (from colors.palette)
// ─────────────────────────────────────────────
inline QColor callArrowFg              = QColor(0x2D, 0xAD, 0x2D); // #2dad2d
inline QColor callArrowMissedFg        = QColor(0xDD, 0x5B, 0x4A); // #dd5b4a

// ─────────────────────────────────────────────
// Tooltip colors (from colors.palette)
// ─────────────────────────────────────────────
inline QColor tooltipBg                = QColor(0xEE, 0xF2, 0xF5); // #eef2f5
inline QColor tooltipFg                = QColor(0x5D, 0x6C, 0x80); // #5d6c80
inline QColor tooltipBorderFg          = QColor(0xC9, 0xD1, 0xDB); // #c9d1db

// ─────────────────────────────────────────────
// Toast colors (from colors.palette)
// ─────────────────────────────────────────────
inline QColor toastBg                  = QColor(0x2C, 0x30, 0x33, 0xE5); // #2c3033e5
inline QColor toastFg                  = windowFgActive;                  // #ffffff

// ─────────────────────────────────────────────
// File message colors (from colors.palette)
// ─────────────────────────────────────────────
inline QColor msgFileInBg              = windowBgActive;        // #40a7e3
inline QColor msgFileInBgOver          = QColor(0x4E, 0xAD, 0xE3); // #4eade3
inline QColor msgFileInBgSelected      = QColor(0x51, 0xA3, 0xD3); // #51a3d3
inline QColor msgFileOutBg             = QColor(0x5F, 0xBE, 0x67); // #5fbe67
inline QColor msgFileOutBgSelected     = QColor(0x50, 0xAC, 0x9B); // #50ac9b

inline QColor msgFile1Bg               = QColor(0x72, 0xB1, 0xDF); // #72b1df blue
inline QColor msgFile2Bg               = QColor(0x5F, 0xBE, 0x67); // #5fbe67 green
inline QColor msgFile3Bg               = QColor(0xE4, 0x72, 0x72); // #e47272 red
inline QColor msgFile4Bg               = QColor(0xEF, 0xC2, 0x74); // #efc274 yellow
inline QColor sendMediaDeleteBg        = QColor(0x00, 0x00, 0x00, 0x56); // #00000056
inline QColor sendMediaDeleteBgOver    = QColor(0x00, 0x00, 0x00, 0x80); // #00000080
inline QColor sendMediaScrollBarBg     = QColor(0x00, 0x00, 0x00, 0x28); // #00000028

// Poll layout (from chat.style).
inline int historyPollQuestionTop = 7;
inline int historyPollSubtitleSkip = 4;
inline QMargins historyPollAnswerPadding = QMargins(32, 10, 0, 10);
inline int historyPollAnswersSkip = 2;
inline int historyPollPercentSkip = 5;
inline int historyPollPercentTop = 0;
inline int historyPollTotalVotesSkip = 5;
inline int historyPollFillingMin = 4;
inline int historyPollFillingHeight = 4;
inline int historyPollFillingRadius = 1;
inline int historyPollFillingBottom = 2;
inline int historyPollFillingRight = 4;
inline constexpr double historyPollRadioOpacity = 0.7;
inline constexpr double historyPollRadioOpacityOver = 1.0;
inline constexpr int historyPollDuration = 300;
inline int historyPollBottomButtonSkip = 15;
inline int historyPollBottomButtonTop = 4;
inline constexpr double historyPollRippleOpacity = 0.3;
inline int historyPollBottomPadding = 8;
inline int historyPollRadioSize = 18;

// Document / file attachment layout (from chat.style: msgFileLayout).
inline int docThumbSize            = 44;
inline int docThumbSkip            = 11;
inline int docPaddingLeft          = 12;
inline int docPaddingTop           = 8;
inline int docPaddingRight         = 10;
inline int docPaddingBottom        = 8;
inline int docNameTop              = 12;
inline int docStatusTop            = 34;
inline int docNameLeft             = 67; // 12 + 44 + 11
inline int docMinWidth             = 268;

// Media status colors (from colors.palette: mediaInFg/mediaOutFg).
inline QColor mediaInFg                = msgInDateFg;
inline QColor mediaOutFg               = msgOutDateFg;
inline QColor mediaPlaceholderBg       = inputBorderFg;
inline QColor historyMediaOverlayBg    = QColor(0x00, 0x00, 0x00, 0x50); // #00000050
inline QColor historyMediaSpinnerFg    = QColor(0xFF, 0xFF, 0xFF, 0xC8); // #ffffffc8
inline QColor historyMediaDurationBg   = QColor(0x00, 0x00, 0x00, 0xB4); // #000000b4
inline QColor historyMediaYoutubePlayBg = QColor(0xE8, 0x31, 0x31, 0xC8); // #e83131c8
inline QColor historyMediaVideoPlayBg  = layerBg;                         // #0000007f
inline QColor historyVideoPlaceholderBg = QColor(0x1C, 0x1C, 0x1E);       // #1c1c1e
inline QColor historyVideoControlBg    = QColor(0x00, 0x00, 0x00, 0x78); // #00000078
inline QColor historyVideoProgressBg   = QColor(0xFF, 0xFF, 0xFF, 0x3C); // #ffffff3c
inline QColor historyVideoProgressFg   = historyMediaSpinnerFg;

// ─────────────────────────────────────────────
// Audio / waveform (from chat.style & colors.palette)
// ─────────────────────────────────────────────
inline int msgWaveformBar          = 2;   // msgWaveformBar: 2px
inline int msgWaveformSkip         = 1;   // msgWaveformSkip: 1px
inline int msgWaveformMin          = 3;   // msgWaveformMin: 3px
inline int msgWaveformMax          = 17;  // msgWaveformMax: 17px

// Waveform / seek bar colors (from colors.palette).
inline QColor msgWaveformInActive      = windowBgActive;               // #40a7e3
inline QColor msgWaveformInInactive    = QColor(0xD4, 0xDE, 0xE6);    // #d4dee6
inline QColor msgWaveformOutActive     = QColor(0x5E, 0xBD, 0x66);    // #5ebd66
inline QColor msgWaveformOutInactive   = QColor(0xB3, 0xE2, 0xB4);    // #b3e2b4

// File icon FG colors (from colors.palette: historyFileIn/OutIconFg).
inline QColor historyFileInIconFg      = msgInBg;                      // white
inline QColor historyFileOutIconFg     = msgOutBg;                     // light green bg

// File overlay animation.
inline constexpr int msgFileOverDuration     = 200;  // msgFileOverDuration: 200ms
inline int msgFileRadialLine       = 3;    // msgFileRadialLine: 3px
inline constexpr int radialDuration          = 350;  // radialDuration: 350ms
inline constexpr int radialPeriod            = 3000; // radialPeriod: 3000ms
inline int uploadRadialSize        = 44;   // docThumbSize (set in initPxValues)
inline int uploadRadialLine        = 3;    // msgFileRadialLine (set in initPxValues)

// ─────────────────────────────────────────────
// Default widget styles (stub structs for the
// Ui::FlatLabel / Ui::InputField / Ui::IconButton
// constructors that take const StyleType &).
// ─────────────────────────────────────────────
struct FlatLabelStyle {
    int maxWidth = 0;
};
inline const FlatLabelStyle defaultFlatLabel;

inline InputFieldStyle defaultInputField = {
    .width = 0,
    .heightMin = 55,
    .heightMax = 148,
    .border = 1,
    .borderActive = 2,
    .borderRadius = 0,
    .borderDenominator = 1,
    .duration = 150,
    .textMargins = QMargins(0, 28, 0, 4),
    .placeholderMargins = QMargins(0, 0, 0, 0),
    .textBg = windowBg,
    .textBgActive = windowBg,
    .textFg = windowFg,
    .placeholderFg = windowSubTextFg,
    .placeholderFgActive = windowActiveTextFg,
    .borderFg = inputBorderFg,
    .borderFgActive = activeLineFg,
};

struct IconButtonStyle {
    int width = 34;
    int height = 34;
};
inline const IconButtonStyle defaultIconButton;

// ─────────────────────────────────────────────
// QuoteStyle: block-level decoration parameters
// (from chat_helpers.style: historyQuoteStyle)
// ─────────────────────────────────────────────
struct QuoteStyle {
    QMargins padding;
    int verticalSkip = 0;
    int header = 0;         // Pre header strip height (0 for blockquote)
    QPoint headerPosition;
    QString iconName;
    QPoint iconPosition;
    int outline = 0;        // Left bar width
    int outlineShift = 0;   // Outline gradient shift
    int radius = 0;         // Corner radius
};

// From chat_helpers.style: historyQuoteStyle base + blockquote/pre overrides
inline QuoteStyle historyBlockquoteStyle = {
    .padding = QMargins(10, 2, 20, 2),
    .verticalSkip = 4,
    .header = 0,
    .headerPosition = {},
    .iconName = QStringLiteral("mini_quote"),
    .iconPosition = QPoint(4, 4),
    .outline = 3,
    .outlineShift = 2,
    .radius = 5,
};
// messageQuoteStyle for reply decoration (padding.right=4, no icon).
inline QuoteStyle messageQuoteStyle = {
    .padding = QMargins(10, 2, 4, 2),
    .verticalSkip = 0,
    .header = 0,
    .headerPosition = {},
    .iconName = {},
    .iconPosition = {},
    .outline = 3,
    .outlineShift = 2,
    .radius = 5,
};
inline QuoteStyle historyPreStyle = {
    .padding = QMargins(10, 2, 4, 2),
    .verticalSkip = 4,
    .header = 20,
    .headerPosition = QPoint(10, 2),
    .iconName = QStringLiteral("mini_copy"),
    .iconPosition = QPoint(4, 2),
    .outline = 3,
    .outlineShift = 2,
    .radius = 5,
};
// Link preview card (from chat.style: historyPagePreview).
inline QuoteStyle historyPagePreviewStyle = {
    .padding = QMargins(10, 5, 7, 7),
    .verticalSkip = 0,
    .header = 0,
    .headerPosition = {},
    .iconName = {},
    .iconPosition = {},
    .outline = 3,
    .outlineShift = 2,
    .radius = 5,
};

// ─────────────────────────────────────────────
// Settings panel
// ─────────────────────────────────────────────

// Top bar — height 54.
inline int settingsTopBarHeight = 54;

inline QFont settingsTitleFont() {
    return baseFont(16, true);
}
inline QColor settingsTitleFg(0x22, 0x22, 0x22);  // windowBoldFg

// Title position (24px, 17px).
inline int settingsTitleLeft = 24;
inline int settingsTitleTop = 17;

// Close button — width 48px.
inline int settingsCloseButtonSize = 48;
inline QColor settingsCloseIconFg(0x99, 0x99, 0x99);     // menuIconFg
inline QColor settingsCloseIconFgOver(0x8a, 0x8a, 0x8a); // menuIconFgOver

// Section titles.
inline QFont settingsSubsectionTitleFont() {
    QFont f = static_cast<const QFont &>(semiboldFont);
    f.setPixelSize(14);
    return f;
}
inline QColor settingsSubsectionTitleFg(0x16, 0x8a, 0xcd); // windowActiveTextFg

// Content padding.
inline int settingsSubsectionTitleTop = 7;
inline int settingsSubsectionTitleBottom = 9;
inline int settingsCheckboxesSkip = 12;

// Divider.
inline int settingsDividerHeight = 6;
inline QColor settingsDividerBg(0xf1, 0xf1, 0xf1);  // windowBgOver

// Toggle switch.
// Toggle geometry (knob + track):
// diameter=16, width=14, shift=1, border=2.
// fullWidth = diameter + width = 30.
// Total outer: (fullWidth + 2*border) x (diameter + 2*border) = 34 x 20.
inline int settingsToggleDiameter = 16;     // knob diameter
inline int settingsToggleExtraWidth = 14;   // track extra width beyond diameter
inline int settingsToggleShift = 1;         // track inset from knob edge
inline int settingsToggleBorder = 2;        // knob border width
inline int settingsToggleWidth = 34;        // settingsToggleDiameter + settingsToggleExtraWidth + 2 * settingsToggleBorder
inline int settingsToggleHeight = 20;       // settingsToggleDiameter + 2 * settingsToggleBorder
// Colors (from the light palette + dark theme):
inline QColor settingsToggleToggledBg  = windowBg;               // knob fill when on
inline QColor settingsToggleToggledFg  = windowBgActive;          // track fill + knob border when on
inline QColor settingsToggleUntoggledBg = windowBg;              // knob fill when off
inline QColor settingsToggleUntoggledFg = QColor(0xB3, 0xB3, 0xB3); // checkboxFg: track fill + knob border when off

// Settings button row.
inline int settingsButtonHeight = 44;
inline int settingsButtonPaddingLeft = 22;
inline int settingsButtonPaddingRight = 22;
inline int settingsButtonPaddingTop = 10;
inline int settingsButtonToggleSkip = 22;   // distance from right edge to toggle center
inline int settingsButtonRightSkip = 23;

// Row text color.
inline QColor settingsCheckboxTextFg(0x22, 0x22, 0x22);    // windowBoldFg
inline QColor settingsButtonBgOver(0x00, 0x00, 0x00, 0x0D);

// Settings panel sizing.
inline int settingsMaxWidth = 780;

// Sidebar (two-column layout).
inline int settingsSidebarWidth = 200;
inline int settingsSidebarSelectedRadius = 8;
inline QColor settingsSidebarSelectedBg(0xf1, 0xf1, 0xf1); // windowBgOver
inline QColor settingsSidebarBgOver(0x00, 0x00, 0x00, 0x0A);

// --- Settings profile cover ---
inline int settingsPhotoLeft = 22;
inline int settingsPhotoTop = 8;
inline int settingsPhotoBottom = 16;
inline int settingsPhotoSize = 72;  // 72x72px avatar circle
inline int settingsCoverHeight = 96; // settingsPhotoTop + photoSize + settingsPhotoBottom

inline int settingsNameLeft = 112;
inline int settingsNameTop = 12;

inline QFont settingsCoverNameFont() {
    QFont f = static_cast<const QFont &>(semiboldFont);
    f.setPixelSize(17);
    return f;
}

inline int settingsPhoneLeft = 112;  // same as settingsNameLeft
inline int settingsPhoneTop = 37;

inline int settingsUsernameLeft = 112;
inline int settingsUsernameTop = 58;

// --- Settings menu button with icon ---
inline int settingsButtonWithIconPaddingLeft = 60;
inline int settingsButtonIconLeft = 20;
inline QColor settingsMenuIconFg(0x99, 0x99, 0x99);      // menuIconFg
inline QColor settingsMenuIconFgOver(0x8a, 0x8a, 0x8a);  // menuIconFgOver

// --- Settings info row (key-value display) ---
inline int settingsInfoRowHeight = 44;
inline int settingsInfoRowLabelWidth = 120;  // width for "Device ID" / "Session key" labels

// --- Settings session row (device list) ---
inline int settingsSessionRowHeight = 60;
inline QColor settingsSessionActiveFg(0x4f, 0xae, 0x4e);  // green for "Active"
inline QColor settingsVerifiedDotColor(0x4f, 0xae, 0x4e); // green dot
inline int settingsVerifiedDotSize = 8;

// --- Back button in top bar (width 60px) ---
inline int settingsBackButtonWidth = 60;

// --- Calendar box colors ---
inline QColor calendarDaysFg = QColor(0x80, 0x80, 0x80);       // boxTitleAdditionalFg (#808080)
inline QColor calendarDayFg = QColor(0x00, 0x00, 0x00);        // boxTextFg / windowFg
inline QColor calendarGrayedOutFg = QColor(0x99, 0x99, 0x99);  // windowSubTextFg
inline QColor calendarTodayBg = QColor(0x41, 0x9F, 0xD9);      // dialogsBgActive (#419fd9)
inline QColor calendarTodayFg = QColor(0xFF, 0xFF, 0xFF);      // dialogsNameFgActive
inline QColor calendarHoverBg = QColor(0xF1, 0xF1, 0xF1);      // windowBgOver

// --- Main menu drawer ---
inline int mainMenuWidth = 274;
inline int mainMenuCoverHeight = 134;
inline int mainMenuAvatarLeft = 24;
inline int mainMenuAvatarTop = 20;
inline int mainMenuAvatarSize = 48;
inline int mainMenuVerifyButtonTop = 30;
inline int mainMenuVerifyButtonHeight = 28;
inline int mainMenuVerifyButtonRadius = 4;
inline int mainMenuVerifyButtonHorizontalPadding = 12;
inline int mainMenuNameLeft = 26;
inline int mainMenuNameTop = 84;
inline int mainMenuStatusLeft = 26;
inline int mainMenuStatusTop = 103;
inline int mainMenuRowHeight = 44;
inline int mainMenuRowIconLeft = 21;
inline int mainMenuRowTextLeft = 61;
inline int mainMenuRowRightPadding = 16;
inline int mainMenuFooterMinHeight = 80;
inline int mainMenuFooterLeft = 25;
inline int mainMenuFooterProductBottom = 38;
inline int mainMenuFooterVersionBottom = 17;
inline constexpr int mainMenuAnimationDuration = 200;

// Account switcher (tdesktop's ToggleAccountsButton + SetupAccounts rows).
inline int mainMenuToggleRight = 20;      // chevron centre, from the right edge
inline int mainMenuToggleSize = 8;        // half-width of the chevron's stroke
inline int mainMenuToggleBadgeSkip = 12;  // gap between collapsed badge and chevron
inline int mainMenuAccountRowHeight = 52;
inline int mainMenuAccountAvatarLeft = 22;
inline int mainMenuAccountAvatarSize = 32;
inline int mainMenuAccountTextLeft = 66;
inline constexpr int mainMenuAccountsAnimationDuration = 180;
// Account settings cover: the "edit" link beside the name, and the accounts
// chevron right-aligned on the Matrix ID line.
inline int settingsCoverEditNameSkip = 8;
inline int settingsCoverToggleHitSize = 24;
// Distance from the right edge to the CENTRE of an expand chevron. Shared by the
// cover's accounts toggle and the collapsible rows below it, so every chevron on
// the page sits on one vertical line.
inline int settingsChevronRight = 30;
// Account rows in the settings switcher, mirroring the main menu's switcher.
inline int settingsAccountAvatarSize = 32;
inline int settingsAccountTextLeft = 66;

// Account switch: the outgoing account's UI slides away to reveal the new one.
inline constexpr int accountSwitchSlideDuration = 500;

inline QColor mainMenuRowBgOver       = QColor(0xF4, 0xF4, 0xF4);
inline QColor mainMenuRowBgDown       = QColor(0xE8, 0xE8, 0xE8);
inline QColor mainMenuRowText         = windowFg;
inline QColor mainMenuRowTextAttention = attentionButtonFg;
// tdesktop's menuIconColor is windowBoldFg, not the muted menuIconFg: the icons
// read as dark on a light theme and bright on a dark one, matching the label.
inline QColor mainMenuRowIcon         = QColor(0x7B, 0x8A, 0x99); // menuIconColor
inline QColor mainMenuRowIconAttention = attentionButtonFg;

// Semibold, as tdesktop's mainMenuButton (style: semiboldTextStyle).
inline QFont mainMenuRowFont() { return baseFont(st::fsize); }
// Switcher rows carry Matrix IDs, not actions: regular weight, matching the
// cover's identity line rather than the semibold menu entries.
inline QFont mainMenuAccountRowFont() { return baseFont(st::fsize); }
inline QColor mainMenuHeaderNameFg     = windowFg;
// The cover's identity line (the Matrix ID). Not bold: the menu's semibold
// weight belongs to the actions below, and the id reads as a label, not a title.
inline QFont mainMenuHeaderNameFont() { return baseFont(st::fsize + 1); }
inline QColor mainMenuHeaderStatusFg   = windowFg;
inline QFont mainMenuHeaderStatusFont() { return baseFont(st::fsize - 1); }
inline QColor mainMenuFooterLabelFg    = windowSubTextFg;
inline QFont mainMenuFooterNameFont() { return baseFont(st::fsize - 1, true); }
inline QColor mainMenuFooterVersionFg  = windowSubTextFg;
inline QFont mainMenuFooterVersionFont() { return baseFont(st::fsize - 2); }

// ─────────────────────────────────────────────
// Internal choice dialog (language, font, power level)
// ─────────────────────────────────────────────
inline int internalChoicePopupWidth        = 420;
inline int internalChoicePopupMaxHeight    = 620;
inline int internalChoiceBottomSkip        = 12;
inline int internalChoiceSubtitleRowHeight = 58;
inline int internalChoiceSubtitleTitleTop  = 9;
inline int internalChoiceSubtitleSkip      = 5;
inline int internalChoiceRadioSize         = 18;
inline int internalChoiceTextRadioSkip     = 16;

// ─────────────────────────────────────────────
// User Profile Popup
// ─────────────────────────────────────────────
inline int userProfilePopupWidth        = 392;  // desired popup width
inline int userProfileTopBarHeight      = 54;   // infoTopBarHeight: 54px
inline int userProfileCoverHeight       = 108;  // infoProfileCover height
inline int userProfileAvatarSize        = 72;   // cover photo inner size
inline int userProfileAvatarTop         = 18;   // centered within cover
inline int userProfileNameTop           = 14;   // below avatar
inline int userProfileStatusTop         = 4;    // below name
inline int userProfileActionRowHeight   = 44;   // action row height
inline int userProfileActionIconSize    = 20;   // action icon area
inline int userProfileActionLeftPad     = 24;   // left padding for action rows
inline int userProfileActionValueSkip   = 12;   // gap before right-side value
inline int userProfileActionsTopSkip    = 16;   // gap before action rows
inline int userProfileTopBottomPadding  = 10;   // grey profile area padding below status
inline int userProfileBottomSkip        = 8;    // bottom padding
inline int userProfileSectionDividerH   = 8;    // divider between sections
inline int userProfileCloseButtonSize   = 24;   // close X button
inline int userProfileCloseIconLeft     = 4;    // close X offset inside button
inline int userProfileCopyIconSize      = 16;   // user id copy icon
inline int userProfileCopyIconSkip      = 6;    // gap after user id text
inline int userProfileUserIdSideSkip    = 80;   // side padding for user id text

inline QColor userProfileTopBg           = windowBgOver;
inline QColor userProfileActionsBg       = boxBg;
inline QColor userProfileActionBgOver    = windowBgRipple;
inline QColor userProfileCoverBg         = userProfileTopBg;
inline QColor userProfileNameFg          = windowFg;
inline QColor userProfileStatusFg        = windowSubTextFg;
inline QColor userProfileActionFg        = windowFg;
inline QColor userProfileActionFgOver    = windowFgActive;
inline QColor userProfileActionDangerFg  = attentionButtonFg;
inline QColor userProfileDividerBg       = boxDividerBg;
inline QColor userProfileUserIdFg        = windowSubTextFg;

inline QFont userProfileNameFont() { return baseFont(st::fsize + 3, true); }
inline QFont userProfileStatusFont() { return baseFont(st::fsize - 1); }
inline QFont userProfileActionFont() { return baseFont(st::fsize); }
inline QFont userProfileUserIdFont() { return baseFont(st::fsize - 1); }

// ─────────────────────────────────────────────
// initPxValues(): apply runtime scale to every
// pixel constant.
// Must be called once after Style::SetScale().
// ─────────────────────────────────────────────
inline void initPxValues() {
    using TeleMatrix::Style::ConvertScale;

    // Font sizes: NOT scaled here — baseFont() applies ConvertScale
    // to every pixelSize argument. Scaling fsize here would double-scale
    // FontPtr instances that captured fsize at static init time.
    // fsize and boxFontSize remain at their base values.

    // Window layout.
    windowMinWidth = ConvertScale(380);
    windowMinHeight = ConvertScale(480);
    windowDefaultWidth = ConvertScale(800);
    windowDefaultHeight = ConvertScale(600);
    windowBigDefaultWidth = ConvertScale(1024);
    windowBigDefaultHeight = ConvertScale(768);
    columnMinimalWidthLeft = ConvertScale(260);
    columnMaximalWidthLeft = ConvertScale(540);
    columnMinimalWidthMain = ConvertScale(380);

    // Dialog active.
    dialogsChatTypeSkip = ConvertScale(3);

    // Dialog row layout.
    dialogsRowHeight = ConvertScale(62);
    dialogsPhotoSize = ConvertScale(46);
    dialogsNameLeft = ConvertScale(68);
    dialogsNameTop = ConvertScale(10);
    dialogsTextLeft = ConvertScale(68);
    dialogsTextTop = ConvertScale(34);
    dialogsPadding = QMargins(
        ConvertScale(10), ConvertScale(8),
        ConvertScale(10), ConvertScale(8));

    // Unread badge.
    dialogsUnreadHeight = ConvertScale(19);
    dialogsUnreadPadding = ConvertScale(5);
    dialogsUnreadMarkDiameter = ConvertScale(8);

    // Date.
    dialogsDateSkip = ConvertScale(5);

    // Send state.
    dialogsSendStateSkip = ConvertScale(20);

    // Search filter.
    dialogsFilterHeight = ConvertScale(35);
    dialogsFilterRadius = ConvertScale(18);
    dialogsFilterPadding = QPoint(ConvertScale(7), ConvertScale(7));
    dialogsFilterSkip = ConvertScale(4);
    dialogsVerificationBannerPaddingTop = ConvertScale(14);
    dialogsVerificationBannerPaddingBottom = ConvertScale(14);
    dialogsVerificationBannerTitleSkip = ConvertScale(5);
    dialogsVerificationBannerButtonsSkip = ConvertScale(12);
    dialogsVerificationBannerSeparator = ConvertScale(1);
    dialogsVerificationBannerButtonTextPadding = ConvertScale(30);
    dialogsVerificationBannerButtonGap = ConvertScale(8);
    topBarHeight = ConvertScale(54);

    dialogsFilter.heightMin = dialogsFilterHeight;
    dialogsFilter.heightMax = dialogsFilterHeight;
    dialogsFilter.border = ConvertScale(3);
    dialogsFilter.borderActive = ConvertScale(2);
    dialogsFilter.borderRadius = dialogsFilterRadius;
    dialogsFilter.cancelButtonSize = ConvertScale(35);
    const auto dialogsFilterFont = baseFont(fsize);
    const auto dialogsFilterFontHeight = QFontMetrics(dialogsFilterFont).height();
    const auto dialogsFilterVerticalSpace = qMax(
        0,
        dialogsFilter.heightMin - dialogsFilterFontHeight);
    const auto dialogsFilterTop = dialogsFilterVerticalSpace / 2;
    const auto dialogsFilterBottom = dialogsFilterVerticalSpace - dialogsFilterTop;
    dialogsFilter.textMargins = QMargins(
        ConvertScale(12),
        dialogsFilterTop,
        ConvertScale(30),
        dialogsFilterBottom);
    dialogsFilter.placeholderMargins = QMargins(
        ConvertScale(5),
        0,
        ConvertScale(2),
        0);

    // Cancel/cross button.
    dialogsCancelSearchCrossSize = ConvertScale(35);

    // Search filter icon buttons.
    dialogsSearchFromWidth = ConvertScale(29);
    dialogsSearchCalendarWidth = ConvertScale(32);
    dialogsSearchFilterHeight = ConvertScale(35);

    defaultInputField.heightMin = ConvertScale(55);
    defaultInputField.heightMax = ConvertScale(148);
    defaultInputField.border = ConvertScale(1);
    defaultInputField.borderActive = ConvertScale(2);
    defaultInputField.textMargins = QMargins(
        0,
        ConvertScale(28),
        0,
        ConvertScale(4));
    defaultInputField.placeholderMargins = QMargins(0, 0, 0, 0);

    // ChatSearchIn banner.
    searchedBarHeight = ConvertScale(28);
    searchedBarPosition = QPoint(ConvertScale(14), ConvertScale(5));
    dialogsSearchInHeight = ConvertScale(38);
    dialogsSearchInPhotoSize = ConvertScale(28);
    dialogsSearchInPhotoPadding = ConvertScale(10);
    dialogsSearchInSkip = ConvertScale(10);
    dialogsSearchInNameTop = ConvertScale(9);
    dialogsSearchInDownTop = ConvertScale(15);
    dialogsSearchInDownSkip = ConvertScale(4);
    dialogsSearchInCancelWidth = ConvertScale(40);
    dialogsSearchInCancelIconPos = ConvertScale(11);
    dialogsSearchInCheckSkip = ConvertScale(8);

    // Chat filters tabs.
    chatsFiltersTabsHeight = ConvertScale(33);
    chatsFiltersBarTop = ConvertScale(30);
    chatsFiltersBarStroke = ConvertScale(6);
    chatsFiltersBarRadius = ConvertScale(2);
    chatsFiltersLabelTop = ConvertScale(7);
    chatsFiltersTabPadding = ConvertScale(9);
    chatsFiltersOuterPadding = ConvertScale(9);

    // Filter sidebar.
    sideBarWidth = ConvertScale(72);
    sideBarButtonHeight = ConvertScale(62);
    sideBarMainButtonHeight = ConvertScale(54);
    sideBarTextTop = ConvertScale(40);
    sideBarTextSkip = ConvertScale(6);
    sideBarBadgeSkip = ConvertScale(4);
    sideBarBadgeHeight = ConvertScale(17);
    sideBarBadgeStroke = ConvertScale(2);
    sideBarIconPosition = QPoint(ConvertScale(-1), ConvertScale(6));
    sideBarMainMenuIconPosition = QPoint(ConvertScale(-1), ConvertScale(-1));
    sideBarBadgePosition = QPoint(ConvertScale(3), ConvertScale(7));

    // Online badge.
    dialogsOnlineBadgeStroke = ConvertScale(2);
    dialogsOnlineBadgeSize = ConvertScale(10);
    dialogsOnlineBadgeSkip = QPoint(ConvertScale(0), ConvertScale(2));

    // Scam label.
    dialogsScamRadius = ConvertScale(2);

    // Text width minimum.
    dialogsTextWidthMin = ConvertScale(150);

    // Intro/Login.
    introCoverHeight = ConvertScale(208);
    introStepWidth = ConvertScale(340);
    introStepHeight = ConvertScale(384);
    introStepHeightFull = ConvertScale(590);
    introStepTopMin = ConvertScale(76);
    introKeysLineBottom = ConvertScale(52);
    introVersionBottom = ConvertScale(24);
    introHeight = ConvertScale(406);
    introNextTop = ConvertScale(266);
    introNextSlide = ConvertScale(200);
    introContentTopAdd = ConvertScale(30);

    introTitleTop = ConvertScale(1);
    introDescriptionTop = ConvertScale(34);
    introHeadingGap = ConvertScale(7);
    introHeadingToFields = ConvertScale(24);
    introFieldsToButton = ConvertScale(32);
    introStepFieldTop = ConvertScale(96);

    introNextButtonWidth = ConvertScale(340);
    introNextButtonHeight = ConvertScale(46);
    introNextButtonRadius = ConvertScale(9);
    introNextButtonTextTop = ConvertScale(11);

    introCountryWidth = ConvertScale(340);
    introCountryHeight = ConvertScale(44);
    introPhoneWidth = ConvertScale(225);
    introCountryCodeWidth = ConvertScale(64);

    introCodeDigitHeight = ConvertScale(50);
    introCodeDigitBorderWidth = ConvertScale(4);
    introCodeDigitSkip = ConvertScale(10);

    introErrorTop = ConvertScale(235);
    introErrorBelowLinkTop = ConvertScale(220);

    introCoverTitleTop = ConvertScale(136);
    introCoverDescriptionTop = ConvertScale(174);
    introCoverMaxWidth = ConvertScale(880);

    introBackButtonSize = ConvertScale(56);
    introFieldSpacing = ConvertScale(16);
    introDescriptionLineHeight = ConvertScale(20);

    // Login footer links.
    introLinkTop = ConvertScale(12);
    introLinkGap = ConvertScale(24);
    introDiscoveryFontSize = ConvertScale(11);

    // Verification cards.
    introVerifyCardWidth = ConvertScale(300);
    introVerifyCardHeight = ConvertScale(60);
    introVerifyCardRadius = ConvertScale(8);
    introVerifyCardTitleSize = ConvertScale(13);
    introVerifyCardSubSize = ConvertScale(12);
    introVerifyCardTextGap = ConvertScale(2);
    introVerifySkipTop = ConvertScale(20);

    // Emoji verification.
    introVerifyEmojiSize = ConvertScale(48);
    introVerifyEmojiFontSize = ConvertScale(32);
    introVerifyEmojiLabelSize = ConvertScale(11);
    introVerifyEmojiPadding = ConvertScale(4);
    introVerifyEmojiCellGap = ConvertScale(2);
    introVerifyEmojiContainerW = ConvertScale(450);
    introVerifyEmojiContainerH = ConvertScale(160);
    introVerifyEmojiContainerR = ConvertScale(12);
    introVerifyMismatchTop = ConvertScale(16);

    // Recovery key.
    introRecoveryKeyWidth = ConvertScale(492);
    introRecoveryKeyHeight = ConvertScale(36);
    introRecoveryKeyFontSize = ConvertScale(12);
    introRecoveryKeyErrorTop = ConvertScale(8);
    introRecoveryKeyErrorHeight = ConvertScale(20);
    introRecoveryKeyButtonGap = ConvertScale(10);

    // Success screen.
    introVerifyCheckSize = ConvertScale(64);

    // Message/Chat layout.
    msgMaxWidth = ConvertScale(430);
    msgMinWidth = ConvertScale(160);
    msgPhotoSize = ConvertScale(33);
    msgPhotoSkip = ConvertScale(40);
    msgPadding = QMargins(
        ConvertScale(11), ConvertScale(8),
        ConvertScale(11), ConvertScale(8));
    msgMargin = QMargins(
        ConvertScale(16), ConvertScale(6),
        ConvertScale(56), ConvertScale(2));
    msgMarginTopAttached = ConvertScale(0);
    msgShadow = ConvertScale(2);
    msgDateSpace = ConvertScale(12);
    msgDateDelta = QPoint(ConvertScale(2), ConvertScale(5));
    msgDateImgDelta = ConvertScale(4);
    msgDateImgPadding = QPoint(ConvertScale(8), ConvertScale(2));
    mediaCaptionSkip = ConvertScale(5);
    minPhotoSize = ConvertScale(100);
    maxMediaSize = ConvertScale(430);

    // Bubble radii.
    bubbleRadiusSmall = ConvertScale(6);
    bubbleRadiusLarge = ConvertScale(16);

    // History layout.
    historyPhotoLeft = ConvertScale(14);
    historyPhotoBubbleMinWidth = ConvertScale(200);
    historyMinimalWidth = ConvertScale(380);
    historyPaddingBottom = ConvertScale(8);

    // Unread bar.
    historyUnreadBarHeight = ConvertScale(32);
    historyUnreadBarMargin = ConvertScale(8);

    // Service message.
    msgServicePadding = QMargins(
        ConvertScale(12), ConvertScale(3),
        ConvertScale(12), ConvertScale(4));
    msgServiceMargin = QMargins(
        ConvertScale(10), ConvertScale(10),
        ConvertScale(10), ConvertScale(2));

    // Reply.
    historyReplyTop = ConvertScale(2);
    historyReplyBottom = ConvertScale(2);
    historyReplyPreview = ConvertScale(32);
    historyReplyHeight = ConvertScale(49);
    historyReplySkip = ConvertScale(53);
    historyReplyIconPosition = QPoint(ConvertScale(7), ConvertScale(7));
    msgReplyPadding = QMargins(
        ConvertScale(6), ConvertScale(6),
        ConvertScale(11), ConvertScale(6));
    historyReplyPadding = QMargins(
        ConvertScale(11), ConvertScale(2),
        ConvertScale(6), ConvertScale(2));
    msgReplyBarPos = QPoint(ConvertScale(1), ConvertScale(0));
    msgReplyBarSize = QSize(ConvertScale(2), ConvertScale(36));
    msgReplyBarSkip = ConvertScale(10);
    historyReplyPreviewMargin = QMargins(
        ConvertScale(7), ConvertScale(4),
        ConvertScale(4), ConvertScale(4));
    historyRecordSignalRadius = ConvertScale(5);
    historyRecordDurationSkip = ConvertScale(12);
    historyRecordMainBlobMinRadius = ConvertScale(23);
    historyRecordTextLeft = ConvertScale(15);

    // Forward choose.
    historyForwardChooseMargins = QMargins(
        ConvertScale(30), ConvertScale(10),
        ConvertScale(30), ConvertScale(10));

    // Media viewer.
    mediaviewControlSize = ConvertScale(90);
    mediaviewHeaderTop = ConvertScale(47);
    mediaviewTextTop = ConvertScale(26);
    mediaviewTextLeft = ConvertScale(14);
    mediaviewTextSkip = ConvertScale(10);
    mediaviewIconW = ConvertScale(46);
    mediaviewIconH = ConvertScale(54);
    mediaviewIconOver = ConvertScale(36);
    mediaviewCaptionRadius = ConvertScale(6);
    mediaviewCaptionPaddingH = ConvertScale(11);
    mediaviewCaptionPaddingV = ConvertScale(6);
    mediaviewCaptionMargin = ConvertScale(11);
    mediaviewDefaultWidth = ConvertScale(800);
    mediaviewDefaultHeight = ConvertScale(600);
    mediaviewMinWidth = ConvertScale(480);
    mediaviewMinHeight = ConvertScale(360);
    mediaviewControllerWidth = ConvertScale(480);
    mediaviewControllerHeight = ConvertScale(72);
    mediaviewControllerRadius = ConvertScale(9);
    mediaviewControllerBottom = ConvertScale(6);
    mediaviewPlayButtonTop = ConvertScale(2);
    mediaviewPlayButtonSize = ConvertScale(40);
    mediaviewSmallButtonSize = ConvertScale(32);
    mediaviewButtonsTop = ConvertScale(6);
    mediaviewButtonsRight = ConvertScale(8);
    mediaviewVolumeLeft = ConvertScale(6);
    mediaviewVolumeSkip = ConvertScale(3);
    mediaviewVolumeWidth = ConvertScale(75);
    mediaviewPlaybackTop = ConvertScale(49);
    mediaviewProgressTop = ConvertScale(46);
    mediaviewProgressSkip = ConvertScale(10);
    mediaviewSeekTrackHeight = ConvertScale(3);
    mediaviewSeekHandleSize = ConvertScale(12);

    // Reactions.
    reactionInlinePadding = QMargins(
        ConvertScale(5), ConvertScale(2),
        ConvertScale(7), ConvertScale(2));
    reactionInlineSize = ConvertScale(18);
    reactionInlineEmoji = ConvertScale(15);
    reactionInlineImage = ConvertScale(32);
    reactionInlineSkip = ConvertScale(3);
    reactionInlineTagSkip = ConvertScale(6);
    reactionInlineTagLeftRadius = ConvertScale(6);
    reactionInlineTagRightRadius = ConvertScale(3);
    reactionInlineTagArrow = ConvertScale(5);
    reactionInlineTagDot = ConvertScale(5);
    reactionInlineTagDotSkip = ConvertScale(2);
    reactionInlineEmptySkip = ConvertScale(2);
    reactionInlineTagNamePosition = QPoint(ConvertScale(26), ConvertScale(2));
    reactionInlineTagPromoPosition = QPoint(ConvertScale(20), ConvertScale(2));
    reactionInlineBetween = ConvertScale(4);
    reactionInlineInBubbleLeft = ConvertScale(-3);
    reactionInlineUserpicsPadding = QMargins(
        ConvertScale(1), ConvertScale(1),
        ConvertScale(1), ConvertScale(1));

    reactionInfoSize = ConvertScale(15);
    reactionInfoImage = ConvertScale(30);
    reactionInfoSkip = ConvertScale(3);
    reactionInfoDigitSkip = ConvertScale(6);
    reactionInfoBetween = ConvertScale(3);

    reactionCornerSize = QSize(ConvertScale(36), ConvertScale(32));
    reactionCornerCenter = QPoint(ConvertScale(7), ConvertScale(-9));
    reactionCornerImage = ConvertScale(22);
    reactionCornerEmoji = ConvertScale(18);
    reactionCornerShadow = QMargins(
        ConvertScale(4), ConvertScale(8),
        ConvertScale(4), ConvertScale(8));
    reactionCornerActiveAreaPadding = QMargins(
        ConvertScale(10), ConvertScale(10),
        ConvertScale(10), ConvertScale(10));
    reactionCornerAddedHeightMax = ConvertScale(100);
    reactionCornerSkip = ConvertScale(-4);
    reactionExpandedSkip = ConvertScale(2);
    reactionGradientStart = ConvertScale(8);
    reactionGradientSize = ConvertScale(24);
    reactionGradientFadeSize = ConvertScale(24);
    reactionAppearStartSkip = ConvertScale(2);
    reactionMainAppearShift = ConvertScale(20);
    reactionCollapseFadeThreshold = ConvertScale(40);
    reactionFlyUp = ConvertScale(50);

    // Emoji panel.
    emojiPanRadius = ConvertScale(8);

    // React strip.
    reactStripHeight = ConvertScale(40);
    reactStripSize = ConvertScale(32);
    reactStripMinWidth = ConvertScale(60);
    reactStripImage = ConvertScale(26);
    reactStripSkip = ConvertScale(7);
    reactStripBubbleRight = ConvertScale(20);

    mediaInBubbleSkip = ConvertScale(5);

    // Mention autocomplete.
    mentionHeight = ConvertScale(40);
    mentionPadding = QMargins(
        ConvertScale(8), ConvertScale(5),
        ConvertScale(8), ConvertScale(5));
    mentionTop = ConvertScale(11);
    mentionPhotoSize = ConvertScale(33);

    webPagePhotoDelta = ConvertScale(8);

    // Box layout.
    boxRadius = ConvertScale(8);
    boxWidth = ConvertScale(320);
    boxWideWidth = ConvertScale(364);
    addAccountBoxWidth = ConvertScale(540);
    addAccountBoxHeight = ConvertScale(620);
    signOutConfirmWidth = ConvertScale(350);
    boxMaxListHeight = ConvertScale(492);
    boxTitleHeight = ConvertScale(48);
    boxTitlePosition = QPoint(ConvertScale(24), ConvertScale(13));
    boxButtonPadding = QMargins(
        ConvertScale(6), ConvertScale(10),
        ConvertScale(10), ConvertScale(10));
    boxButtonHeight = ConvertScale(34);
    boxButtonMinWidth = ConvertScale(30);
    buttonRadius = ConvertScale(4);
    boxPadding = QMargins(
        ConvertScale(24), ConvertScale(14),
        ConvertScale(24), ConvertScale(8));
    boxTopMargin = ConvertScale(8);
    boxLabelLineHeight = ConvertScale(22);

    // Layer shadow.
    layerShadowExtend = ConvertScale(10);
    layerVerticalMargin = ConvertScale(20);

    // ShareBox.
    shareRowsTop = ConvertScale(12);
    shareRowHeight = ConvertScale(108);
    sharePhotoTop = ConvertScale(6);
    shareNameTop = ConvertScale(6);
    shareColumnSkip = ConvertScale(6);
    shareColumnCount = 4; // not a pixel value, but count
    shareImageRadius = ConvertScale(28);
    boxSearchPadding = QMargins(
        ConvertScale(8), ConvertScale(6),
        ConvertScale(8), ConvertScale(6));
    boxSearchFieldHeight = ConvertScale(32);

    // Poll layout.
    historyPollQuestionTop = ConvertScale(7);
    historyPollSubtitleSkip = ConvertScale(4);
    historyPollAnswerPadding = QMargins(
        ConvertScale(32), ConvertScale(10),
        ConvertScale(0), ConvertScale(10));
    historyPollAnswersSkip = ConvertScale(2);
    historyPollPercentSkip = ConvertScale(5);
    historyPollPercentTop = ConvertScale(0);
    historyPollTotalVotesSkip = ConvertScale(5);
    historyPollFillingMin = ConvertScale(4);
    historyPollFillingHeight = ConvertScale(4);
    historyPollFillingRadius = ConvertScale(1);
    historyPollFillingBottom = ConvertScale(2);
    historyPollFillingRight = ConvertScale(4);
    historyPollBottomButtonSkip = ConvertScale(15);
    historyPollBottomButtonTop = ConvertScale(4);
    historyPollBottomPadding = ConvertScale(8);
    historyPollRadioSize = ConvertScale(18);

    // Document / file attachment layout.
    docThumbSize = ConvertScale(44);
    docThumbSkip = ConvertScale(11);
    docPaddingLeft = ConvertScale(12);
    docPaddingTop = ConvertScale(8);
    docPaddingRight = ConvertScale(10);
    docPaddingBottom = ConvertScale(8);
    docNameTop = ConvertScale(12);
    docStatusTop = ConvertScale(34);
    docNameLeft = ConvertScale(67);
    docMinWidth = ConvertScale(268);

    // Audio / waveform.
    msgWaveformBar = ConvertScale(2);
    msgWaveformSkip = ConvertScale(1);
    msgWaveformMin = ConvertScale(3);
    msgWaveformMax = ConvertScale(17);

    // File overlay.
    msgFileRadialLine = ConvertScale(3);
    uploadRadialSize = ConvertScale(44);
    uploadRadialLine = ConvertScale(3);

    // QuoteStyle pixel fields.
    historyBlockquoteStyle.padding = QMargins(
        ConvertScale(10), ConvertScale(2),
        ConvertScale(20), ConvertScale(2));
    historyBlockquoteStyle.verticalSkip = ConvertScale(4);
    historyBlockquoteStyle.iconPosition = QPoint(ConvertScale(4), ConvertScale(4));
    historyBlockquoteStyle.outline = ConvertScale(3);
    historyBlockquoteStyle.outlineShift = ConvertScale(2);
    historyBlockquoteStyle.radius = ConvertScale(5);

    messageQuoteStyle.padding = QMargins(
        ConvertScale(10), ConvertScale(2),
        ConvertScale(4), ConvertScale(2));
    messageQuoteStyle.outline = ConvertScale(3);
    messageQuoteStyle.outlineShift = ConvertScale(2);
    messageQuoteStyle.radius = ConvertScale(5);

    historyPreStyle.padding = QMargins(
        ConvertScale(10), ConvertScale(2),
        ConvertScale(4), ConvertScale(2));
    historyPreStyle.verticalSkip = ConvertScale(4);
    historyPreStyle.header = ConvertScale(20);
    historyPreStyle.headerPosition = QPoint(ConvertScale(10), ConvertScale(2));
    historyPreStyle.iconPosition = QPoint(ConvertScale(4), ConvertScale(2));
    historyPreStyle.outline = ConvertScale(3);
    historyPreStyle.outlineShift = ConvertScale(2);
    historyPreStyle.radius = ConvertScale(5);

    historyPagePreviewStyle.padding = QMargins(
        ConvertScale(10), ConvertScale(5),
        ConvertScale(7), ConvertScale(7));
    historyPagePreviewStyle.outline = ConvertScale(3);
    historyPagePreviewStyle.outlineShift = ConvertScale(2);
    historyPagePreviewStyle.radius = ConvertScale(5);

    // Settings panel.
    settingsTopBarHeight = ConvertScale(54);
    settingsTitleLeft = ConvertScale(24);
    settingsTitleTop = ConvertScale(17);
    settingsCloseButtonSize = ConvertScale(48);
    settingsSubsectionTitleTop = ConvertScale(7);
    settingsSubsectionTitleBottom = ConvertScale(9);
    settingsCheckboxesSkip = ConvertScale(12);
    settingsDividerHeight = ConvertScale(6);
    settingsToggleDiameter = ConvertScale(16);
    settingsToggleExtraWidth = ConvertScale(14);
    settingsToggleShift = ConvertScale(1);
    settingsToggleBorder = ConvertScale(2);
    settingsToggleWidth = ConvertScale(34);
    settingsToggleHeight = ConvertScale(20);
    settingsButtonHeight = ConvertScale(44);
    settingsButtonPaddingLeft = ConvertScale(22);
    settingsButtonPaddingRight = ConvertScale(22);
    settingsButtonPaddingTop = ConvertScale(10);
    settingsButtonToggleSkip = ConvertScale(22);
    settingsButtonRightSkip = ConvertScale(23);
    settingsMaxWidth = ConvertScale(780);
    settingsSidebarWidth = ConvertScale(200);
    settingsSidebarSelectedRadius = ConvertScale(8);
    settingsPhotoLeft = ConvertScale(22);
    settingsPhotoTop = ConvertScale(8);
    settingsPhotoBottom = ConvertScale(16);
    settingsPhotoSize = ConvertScale(72);
    settingsCoverHeight = ConvertScale(96);
    settingsNameLeft = ConvertScale(112);
    settingsNameTop = ConvertScale(12);
    settingsPhoneLeft = ConvertScale(112);
    settingsPhoneTop = ConvertScale(37);
    settingsUsernameLeft = ConvertScale(112);
    settingsUsernameTop = ConvertScale(58);
    settingsButtonWithIconPaddingLeft = ConvertScale(60);
    settingsButtonIconLeft = ConvertScale(20);
    settingsInfoRowHeight = ConvertScale(44);
    settingsInfoRowLabelWidth = ConvertScale(120);
    settingsSessionRowHeight = ConvertScale(60);
    settingsVerifiedDotSize = ConvertScale(8);
    settingsBackButtonWidth = ConvertScale(60);

    // Main menu drawer.
    mainMenuWidth = ConvertScale(274);
    mainMenuCoverHeight = ConvertScale(134);
    mainMenuAvatarLeft = ConvertScale(24);
    mainMenuAvatarTop = ConvertScale(20);
    mainMenuAvatarSize = ConvertScale(48);
    mainMenuVerifyButtonTop = ConvertScale(30);
    mainMenuVerifyButtonHeight = ConvertScale(28);
    mainMenuVerifyButtonRadius = ConvertScale(4);
    mainMenuVerifyButtonHorizontalPadding = ConvertScale(12);
    mainMenuNameLeft = ConvertScale(26);
    mainMenuNameTop = ConvertScale(84);
    mainMenuStatusLeft = ConvertScale(26);
    mainMenuStatusTop = ConvertScale(103);
    mainMenuRowHeight = ConvertScale(44);
    mainMenuRowIconLeft = ConvertScale(21);
    mainMenuRowTextLeft = ConvertScale(61);
    mainMenuRowRightPadding = ConvertScale(16);
    mainMenuFooterMinHeight = ConvertScale(80);
    mainMenuFooterLeft = ConvertScale(25);
    mainMenuFooterProductBottom = ConvertScale(38);
    mainMenuFooterVersionBottom = ConvertScale(17);
    mainMenuToggleRight = ConvertScale(20);
    mainMenuToggleSize = ConvertScale(8);
    mainMenuToggleBadgeSkip = ConvertScale(12);
    mainMenuAccountRowHeight = ConvertScale(52);
    mainMenuAccountAvatarLeft = ConvertScale(22);
    mainMenuAccountAvatarSize = ConvertScale(32);
    mainMenuAccountTextLeft = ConvertScale(66);
    settingsCoverEditNameSkip = ConvertScale(8);
    settingsCoverToggleHitSize = ConvertScale(24);
    settingsChevronRight = ConvertScale(30);
    settingsAccountAvatarSize = ConvertScale(32);
    settingsAccountTextLeft = ConvertScale(66);

    // Internal choice dialog.
    internalChoicePopupWidth = ConvertScale(420);
    internalChoicePopupMaxHeight = ConvertScale(620);
    internalChoiceBottomSkip = ConvertScale(12);
    internalChoiceSubtitleRowHeight = ConvertScale(58);
    internalChoiceSubtitleTitleTop = ConvertScale(9);
    internalChoiceSubtitleSkip = ConvertScale(5);
    internalChoiceRadioSize = ConvertScale(18);
    internalChoiceTextRadioSkip = ConvertScale(16);

    // User Profile Popup.
    userProfilePopupWidth = ConvertScale(392);
    userProfileTopBarHeight = ConvertScale(54);
    userProfileCoverHeight = ConvertScale(108);
    userProfileAvatarSize = ConvertScale(72);
    userProfileAvatarTop = ConvertScale(18);
    userProfileNameTop = ConvertScale(14);
    userProfileStatusTop = ConvertScale(4);
    userProfileActionRowHeight = ConvertScale(44);
    userProfileActionIconSize = ConvertScale(20);
    userProfileActionLeftPad = ConvertScale(24);
    userProfileActionValueSkip = ConvertScale(12);
    userProfileActionsTopSkip = ConvertScale(16);
    userProfileTopBottomPadding = ConvertScale(10);
    userProfileBottomSkip = ConvertScale(8);
    userProfileSectionDividerH = ConvertScale(8);
    userProfileCloseButtonSize = ConvertScale(24);
    userProfileCloseIconLeft = ConvertScale(4);
    userProfileCopyIconSize = ConvertScale(16);
    userProfileCopyIconSkip = ConvertScale(6);
    userProfileUserIdSideSkip = ConvertScale(80);
}

} // namespace st

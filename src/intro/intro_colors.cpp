// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "intro/intro_colors.h"

namespace intro {

void applyLight() {
    bg = light::bg;
    bgOver = light::bgOver;
    bgActive = light::bgActive;
    titleFg = light::titleFg;
    subtextFg = light::subtextFg;
    activeTextFg = light::activeTextFg;
    inputBorder = light::inputBorder;
    activeLine = light::activeLine;
    buttonBg = light::buttonBg;
    buttonBgOver = light::buttonBgOver;
    buttonBgRipple = light::buttonBgRipple;
    coverTopBg = light::coverTopBg;
    coverBottomBg = light::coverBottomBg;
    coverTitleFg = light::coverTitleFg;
    coverDescFg = light::coverDescFg;
    buttonFg = light::buttonFg;
    buttonDisabledBg = light::buttonDisabledBg;
    inputDisabledBg = light::inputDisabledBg;
    inputDisabledFg = light::inputDisabledFg;
    attentionFg = light::attentionFg;
    verifySuccessBg = light::verifySuccessBg;
    washTop = light::washTop;
    washMid = light::washMid;
    washBottom = light::washBottom;
    surfaceFill = light::surfaceFill;
    surfaceBorder = light::surfaceBorder;
    accentFill = light::accentFill;
    accentFillOver = light::accentFillOver;
    accentText = light::accentText;
    inkHeading = light::inkHeading;
    inkField = light::inkField;
    mutedFg = light::mutedFg;
    ghostBorder = light::ghostBorder;
    ghostFg = light::ghostFg;
    ghostBgOver = light::ghostBgOver;
    radioBorder = light::radioBorder;
}

void applyCurrentTheme() {
    // Form chrome follows the live app theme.
    bg = st::introBg;
    bgOver = st::windowBgOver;
    titleFg = st::introTitleFg;
    subtextFg = st::introDescriptionFg;
    inputBorder = st::inputBorderFg;
    attentionFg = st::attentionButtonFg;
    verifySuccessBg = st::introVerifySuccessBg;
    // The intro's own disabled/greyed tokens are fixed light literals (not in the
    // theme apply-list), so a disabled Sign In button would stay light-grey on a
    // dark card. Source themed equivalents instead: menuFgDisabled is exactly what
    // introButtonDisabledBg aliases in the light default, and windowBgOver /
    // windowSubTextFg are the themed muted bg / text.
    buttonDisabledBg = st::menuFgDisabled;
    inputDisabledBg = st::windowBgOver;
    inputDisabledFg = st::windowSubTextFg;

    // Buttons, links, the accent and the active input line follow the theme's
    // accent (was fixed brand green) so the popup matches the app fully.
    buttonBg = st::activeButtonBg;
    buttonBgOver = st::activeButtonBgOver;
    buttonBgRipple = st::activeButtonBgRipple;
    buttonFg = st::activeButtonFg;
    bgActive = st::windowBgActive;
    activeTextFg = st::windowActiveTextFg;
    activeLine = st::windowBgActive;
    // The welcome cover gradient only shows in the full-window (always-light)
    // intro, never in the popup, so it stays fixed.
    coverTopBg = light::coverTopBg;
    coverBottomBg = light::coverBottomBg;
    coverTitleFg = light::coverTitleFg;
    coverDescFg = light::coverDescFg;

    // Redesign tokens in the popup. The warm-paper wash is a full-window
    // treatment; inside the app it would clash with whatever theme is running,
    // so the three stops collapse to one flat themed background and everything
    // else follows the theme too. The wash-specific contrast reasoning behind
    // the three greens does not apply here — the theme's own accent already
    // passes against the theme's own background.
    washTop = st::introBg;
    washMid = st::introBg;
    washBottom = st::introBg;
    surfaceFill = st::windowBg;
    surfaceBorder = st::inputBorderFg;
    accentFill = st::activeButtonBg;
    accentFillOver = st::activeButtonBgOver;
    accentText = st::windowActiveTextFg;
    inkHeading = st::introTitleFg;
    inkField = st::windowFg;
    mutedFg = st::windowSubTextFg;
    ghostBorder = st::inputBorderFg;
    ghostFg = st::windowFg;
    ghostBgOver = st::windowBgOver;
    radioBorder = st::inputBorderFg;
}

} // namespace intro

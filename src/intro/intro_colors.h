// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#pragma once

#include "styles/style_constants.h"

/// Colors for the intro/welcome flow.
///
/// The full-window intro (first-run / logged-out) is ALWAYS light ("Dubai")
/// — see `light::` below and `applyLight()`. The in-app Add-Account popup instead
/// renders the intro in the live app theme via `applyCurrentTheme()`, keeping the
/// green brand accents fixed. Each of the two IntroWidget call sites sets its
/// context before building the widget (the two are mutually exclusive in time),
/// so the widgets can keep reading `intro::<color>` unchanged.
namespace intro {

/// The frozen light palette — captured once at static init, exactly as the intro
/// has always used it. Never changes at runtime; the source for applyLight() and
/// for the brand-fixed colors in applyCurrentTheme().
namespace light {

inline const QColor bg = st::introBg;
inline const QColor bgOver = st::windowBgOver;
inline const QColor bgActive = QColor(0x4D, 0xB2, 0x4E);        // icon green (accent)
inline const QColor titleFg = st::introTitleFg;
inline const QColor subtextFg = st::introDescriptionFg;
inline const QColor activeTextFg = QColor(0x2E, 0x9E, 0x43);    // darker green for links/active text
inline const QColor inputBorder = st::inputBorderFg;
inline const QColor activeLine = QColor(0x4D, 0xB2, 0x4E);      // active input underline
inline const QColor buttonBg = QColor(0x4D, 0xB2, 0x4E);        // filled button
inline const QColor buttonBgOver = QColor(0x44, 0xA8, 0x48);    // button hover
inline const QColor buttonBgRipple = QColor(0x3A, 0x9A, 0x3E);  // button ripple
inline const QColor coverTopBg = QColor(0x2E, 0x9E, 0x43);      // cover gradient top (dark green)
inline const QColor coverBottomBg = QColor(0x57, 0xC9, 0x5A);   // cover gradient bottom (light green)
inline const QColor coverTitleFg = st::introCoverTitleFg;
inline const QColor coverDescFg = st::introCoverDescFg;
inline const QColor buttonFg = st::introButtonFg;
inline const QColor buttonDisabledBg = st::introButtonDisabledBg;
inline const QColor inputDisabledBg = st::introInputDisabledBg;
inline const QColor inputDisabledFg = st::introInputDisabledFg;
inline const QColor attentionFg = st::attentionButtonFg;
inline const QColor verifySuccessBg = st::introVerifySuccessBg;

// --- Redesign tokens (2026-07 intro) -------------------------------------
//
// The stage is a vertical warm-paper wash with NOTHING drawn on it. Because it
// darkens toward the bottom, a colour that passes contrast at the top can fail
// lower down — which is why there are three greens rather than one, and why the
// muted grey is as dark as it is. Test any new colour against `washBottom`, not
// against white, and do not lighten these.
inline const QColor washTop = QColor(0xFE, 0xFC, 0xF7);
inline const QColor washMid = QColor(0xF6, 0xEF, 0xE2);     // at 52%
inline const QColor washBottom = QColor(0xEC, 0xE1, 0xCD);
/// Translucent so fields and cards lift off the wash instead of sitting on it.
inline const QColor surfaceFill = QColor(0xFF, 0xFF, 0xFF, 0xB8); // white @ .72
inline const QColor surfaceBorder = QColor(0xE3, 0xDA, 0xC9);
/// Filled buttons, radio dots, selection rings, status dots.
inline const QColor accentFill = QColor(0x2A, 0x7F, 0x3E);
inline const QColor accentFillOver = QColor(0x34, 0x93, 0x4A);
/// Every link. Deliberately darker than accentFill — 4.85:1 on the wash's
/// darkest point.
inline const QColor accentText = QColor(0x24, 0x70, 0x2F);
/// Logo mark only. Fails contrast as text or as a fill. Tracks the app icon's
/// body, which tools/icon/recolor.py derives from accentFill/accentFillOver.
inline const QColor appIconGreen = QColor(0x32, 0x90, 0x48);
inline const QColor inkHeading = QColor(0x2F, 0x2B, 0x24);
inline const QColor inkField = QColor(0x22, 0x22, 0x1F);
inline const QColor mutedFg = QColor(0x5F, 0x5B, 0x55);
/// Ghost ("Create account", "They don't match") — border, text, hover fill.
inline const QColor ghostBorder = QColor(0xDE, 0xD6, 0xCA);
inline const QColor ghostFg = QColor(0x4A, 0x45, 0x3D);
inline const QColor ghostBgOver = QColor(0xF4, 0xEF, 0xE6);
/// Unselected radio ring on an option card.
inline const QColor radioBorder = QColor(0xCF, 0xC6, 0xB8);

} // namespace light

/// The live palette the intro paints from. Mutable: set per-context before an
/// IntroWidget is built (applyLight / applyCurrentTheme). Defaults to light, so a
/// read before either call is identical to the old always-light behaviour.
inline QColor bg = light::bg;
inline QColor bgOver = light::bgOver;
inline QColor bgActive = light::bgActive;
inline QColor titleFg = light::titleFg;
inline QColor subtextFg = light::subtextFg;
inline QColor activeTextFg = light::activeTextFg;
inline QColor inputBorder = light::inputBorder;
inline QColor activeLine = light::activeLine;
inline QColor buttonBg = light::buttonBg;
inline QColor buttonBgOver = light::buttonBgOver;
inline QColor buttonBgRipple = light::buttonBgRipple;
inline QColor coverTopBg = light::coverTopBg;
inline QColor coverBottomBg = light::coverBottomBg;
inline QColor coverTitleFg = light::coverTitleFg;
inline QColor coverDescFg = light::coverDescFg;
inline QColor buttonFg = light::buttonFg;
inline QColor buttonDisabledBg = light::buttonDisabledBg;
inline QColor inputDisabledBg = light::inputDisabledBg;
inline QColor inputDisabledFg = light::inputDisabledFg;
inline QColor attentionFg = light::attentionFg;
inline QColor verifySuccessBg = light::verifySuccessBg;

// Redesign tokens. The full-window intro is always light, so these hold their
// literal values there; applyCurrentTheme() re-points them at theme colours for
// the in-app Add-Account popup, where a fixed warm-paper wash would clash.
inline QColor washTop = light::washTop;
inline QColor washMid = light::washMid;
inline QColor washBottom = light::washBottom;
inline QColor surfaceFill = light::surfaceFill;
inline QColor surfaceBorder = light::surfaceBorder;
inline QColor accentFill = light::accentFill;
inline QColor accentFillOver = light::accentFillOver;
inline QColor accentText = light::accentText;
inline QColor inkHeading = light::inkHeading;
inline QColor inkField = light::inkField;
inline QColor mutedFg = light::mutedFg;
inline QColor ghostBorder = light::ghostBorder;
inline QColor ghostFg = light::ghostFg;
inline QColor ghostBgOver = light::ghostBgOver;
inline QColor radioBorder = light::radioBorder;

/// Restore the always-light palette (full-window intro).
void applyLight();

/// Render from the live app theme (used for the in-app Add-Account popup): form
/// chrome (bg, text, inputs, borders, disabled/greyed) AND buttons/links/accent
/// follow the theme. Only the welcome cover gradient (never shown in the popup)
/// stays fixed green.
void applyCurrentTheme();

} // namespace intro

// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

#include "theme_manager.h"
#include "theme_registry.h"
#include "styles/style_constants.h"
#include "ui/style/icon_provider.h"

#include <QApplication>
#include <QPalette>

namespace TeleMatrix {
// Defined in media/media_view_overlay.cpp. Forward-declared here to avoid
// pulling the heavy overlay header into the theme manager.
void clearMediaviewTintCache();
} // namespace TeleMatrix

namespace TeleMatrix::Theme {

namespace {

// Alias map: TeleMatrix token name -> palette key.
// Used for the 77 tokens that don't directly match a palette key name.
const QHash<QString, QString> &aliasMap() {
	static const QHash<QString, QString> map = {
		// Userpic colors -> historyPeerNUserpicBg
		{ "peerUserpicBg1", "historyPeer1UserpicBg" },
		{ "peerUserpicBg2", "historyPeer2UserpicBg" },
		{ "peerUserpicBg3", "historyPeer3UserpicBg" },
		{ "peerUserpicBg4", "historyPeer4UserpicBg" },
		{ "peerUserpicBg5", "historyPeer5UserpicBg" },
		{ "peerUserpicBg6", "historyPeer6UserpicBg" },
		{ "peerUserpicBg7", "historyPeer7UserpicBg" },
		{ "peerUserpicBg8", "historyPeer8UserpicBg" },
		{ "peerUserpicBg1_2", "historyPeer1UserpicBg2" },
		{ "peerUserpicBg2_2", "historyPeer2UserpicBg2" },
		{ "peerUserpicBg3_2", "historyPeer3UserpicBg2" },
		{ "peerUserpicBg4_2", "historyPeer4UserpicBg2" },
		{ "peerUserpicBg5_2", "historyPeer5UserpicBg2" },
		{ "peerUserpicBg6_2", "historyPeer6UserpicBg2" },
		{ "peerUserpicBg7_2", "historyPeer7UserpicBg2" },
		{ "peerUserpicBg8_2", "historyPeer8UserpicBg2" },
		// Main menu custom tokens -> nearest palette equivalents.
		{ "mainMenuRowBgOver", "windowBgOver" },
		{ "mainMenuRowBgDown", "windowBgRipple" },
		{ "mainMenuRowText", "windowFg" },
		{ "mainMenuRowTextAttention", "attentionButtonFg" },
		// The muted context-menu grey (tdesktop's menuIconColor look), not the
		// full-strength windowBoldFg it briefly used.
		{ "mainMenuRowIcon", "menuIconFg" },
		{ "mainMenuRowIconAttention", "attentionButtonFg" },
		{ "mainMenuHeaderNameFg", "windowFg" },
		{ "mainMenuHeaderStatusFg", "windowFg" },
		{ "mainMenuFooterLabelFg", "windowSubTextFg" },
		{ "mainMenuFooterVersionFg", "windowSubTextFg" },
		// Mention autocomplete -> window colors.
		{ "mentionNameFg", "windowFg" },
		{ "mentionNameFgOver", "windowFgOver" },
		{ "mentionBg", "windowBg" },
		{ "mentionBgOver", "windowBgOver" },
		{ "mentionFg", "windowSubTextFg" },
		{ "mentionFgOver", "windowSubTextFgOver" },
		{ "mentionFgActive", "windowActiveTextFg" },
		{ "mentionFgOverActive", "windowActiveTextFg" },
		// Settings custom tokens -> window colors.
		{ "settingsTitleFg", "windowBoldFg" },
		{ "settingsCloseIconFg", "menuIconFg" },
		{ "settingsCloseIconFgOver", "menuIconFgOver" },
		{ "settingsSubsectionTitleFg", "windowActiveTextFg" },
		{ "settingsDividerBg", "windowBgOver" },
		{ "settingsToggleToggledBg", "windowBg" },
		{ "settingsToggleToggledFg", "windowBgActive" },
		{ "settingsToggleUntoggledBg", "windowBg" },
		{ "settingsToggleUntoggledFg", "checkboxFg" },
		{ "settingsCheckboxTextFg", "windowBoldFg" },
		{ "settingsSidebarSelectedBg", "windowBgOver" },
		{ "settingsMenuIconFg", "menuIconFg" },
		{ "settingsMenuIconFgOver", "menuIconFgOver" },
		{ "settingsSessionActiveFg", "boxTextFgGood" },
		{ "settingsVerifiedDotColor", "boxTextFgGood" },
		// Chat filters -> window colors.
		{ "chatsFiltersLabelFg", "windowSubTextFg" },
		{ "chatsFiltersLabelFgActive", "windowActiveTextFg" },
		{ "chatsFiltersBarFgActive", "windowBgActive" },
		{ "chatsFiltersRippleBg", "windowBgOver" },
		{ "chatsFiltersRippleBgActive", "lightButtonBgOver" },
		// Calendar tokens -> nearest palette equivalents.
		{ "calendarDaysFg", "windowSubTextFg" },
		{ "calendarDayFg", "windowFg" },
		{ "calendarGrayedOutFg", "windowSubTextFg" },
		{ "calendarTodayBg", "dialogsBgActive" },
		{ "calendarTodayFg", "dialogsNameFgActive" },
		{ "calendarHoverBg", "windowBgOver" },
		// Box/layer.
		{ "boxDividerBg", "windowBgOver" },
		// Dialog search.
		{ "dialogsCancelSearchCrossFg", "menuIconFg" },
		{ "dialogsCancelSearchCrossFgOver", "menuIconFgOver" },
		{ "dialogsScamFg", "dialogsDraftFg" },
		{ "dialogsScamFgActive", "dialogsDraftFgActive" },
		// Emoji panel.
		{ "emojiPanHover", "windowBgOver" },
		// History custom.
		{ "historyBg", "windowBg" },
		{ "historyComposeIconFgDisabled", "windowSubTextFg" },
		{ "historyPinIconFg", "msgInDateFg" },
		{ "historyReplyNameFg", "windowActiveTextFg" },
		// Intro custom.
		{ "introCoverTitleFg", "windowFgActive" },
		{ "introCoverDescFg", "windowFgActive" },
		{ "introVerifySuccessBg", "boxTextFgGood" },
		// Menu.
		{ "menuIconColor", "windowBoldFg" },
		// Sidebar.
		{ "sideBarBadgeFg", "windowFgActive" },
		// Mediaview.
		{ "mediaviewShadowTop", "windowShadowFg" },
		{ "mediaviewShadowBottom", "windowShadowFg" },
		{ "mediaviewPlaybackBg", "windowShadowFg" },
		// User profile popup.
		{ "userProfileTopBg", "windowBgOver" },
		{ "userProfileActionsBg", "boxBg" },
		{ "userProfileActionBgOver", "windowBgRipple" },
		{ "userProfileCoverBg", "windowBgOver" },
		{ "userProfileNameFg", "windowFg" },
		{ "userProfileStatusFg", "windowSubTextFg" },
		{ "userProfileActionFg", "windowFg" },
		{ "userProfileActionFgOver", "windowFgActive" },
		{ "userProfileActionDangerFg", "attentionButtonFg" },
		{ "userProfileDividerBg", "boxDividerBg" },
		{ "userProfileUserIdFg", "windowSubTextFg" },
	};
	return map;
}

// Look up a color from the palette, trying direct match first, then alias.
QColor resolve(const QHash<QString, QColor> &palette, const QString &token) {
	auto it = palette.constFind(token);
	if (it != palette.cend()) {
		return it.value();
	}
	const auto &aliases = aliasMap();
	auto aliasIt = aliases.constFind(token);
	if (aliasIt != aliases.cend()) {
		it = palette.constFind(aliasIt.value());
		if (it != palette.cend()) {
			return it.value();
		}
	}
	return {};
}

// Helper macro: resolve token, assign to st:: if valid.
#define APPLY_TOKEN(name) do { \
	const auto c = resolve(palette, QStringLiteral(#name)); \
	if (c.isValid()) st::name = c; \
} while (false)

void applyAllTokens(const QHash<QString, QColor> &palette) {
	// Window palette.
	APPLY_TOKEN(windowBg);
	APPLY_TOKEN(windowFg);
	APPLY_TOKEN(windowBgOver);
	APPLY_TOKEN(windowBgRipple);
	APPLY_TOKEN(windowFgOver);
	APPLY_TOKEN(windowSubTextFg);
	APPLY_TOKEN(windowSubTextFgOver);
	APPLY_TOKEN(windowBoldFg);
	APPLY_TOKEN(windowBoldFgOver);
	APPLY_TOKEN(windowBgActive);
	APPLY_TOKEN(windowFgActive);
	APPLY_TOKEN(windowActiveTextFg);
	APPLY_TOKEN(windowShadowFg);
	APPLY_TOKEN(windowShadowFgFallback);
	APPLY_TOKEN(shadowFg);
	APPLY_TOKEN(splitterHandleBg);
	APPLY_TOKEN(toolbarSeparatorFg);

	// Scroll bars.
	APPLY_TOKEN(scrollBarBg);
	APPLY_TOKEN(scrollBarBgOver);
	APPLY_TOKEN(scrollBg);
	APPLY_TOKEN(scrollBgOver);

	// Menu.
	APPLY_TOKEN(menuBg);
	APPLY_TOKEN(menuBgOver);
	APPLY_TOKEN(menuBgRipple);
	APPLY_TOKEN(menuIconFg);
	APPLY_TOKEN(menuIconFgOver);
	APPLY_TOKEN(menuFgDisabled);
	APPLY_TOKEN(menuSeparatorFg);
	APPLY_TOKEN(menuIconColor);

	// Dialogs.
	APPLY_TOKEN(dialogsBg);
	APPLY_TOKEN(dialogsNameFg);
	APPLY_TOKEN(dialogsChatIconFg);
	APPLY_TOKEN(dialogsDateFg);
	APPLY_TOKEN(dialogsTextFg);
	APPLY_TOKEN(dialogsTextFgService);
	APPLY_TOKEN(dialogsDraftFg);
	APPLY_TOKEN(dialogsVerifiedIconBg);
	APPLY_TOKEN(dialogsVerifiedIconFg);
	APPLY_TOKEN(dialogsSendingIconFg);
	APPLY_TOKEN(dialogsSentIconFg);
	APPLY_TOKEN(dialogsUnreadBg);
	APPLY_TOKEN(dialogsUnreadBgMuted);
	APPLY_TOKEN(dialogsUnreadFg);
	APPLY_TOKEN(dialogsOnlineBadgeFg);
	APPLY_TOKEN(dialogsScamFg);
	APPLY_TOKEN(dialogsBgOver);
	APPLY_TOKEN(dialogsNameFgOver);
	APPLY_TOKEN(dialogsChatIconFgOver);
	APPLY_TOKEN(dialogsDateFgOver);
	APPLY_TOKEN(dialogsTextFgOver);
	APPLY_TOKEN(dialogsTextFgServiceOver);
	APPLY_TOKEN(dialogsDraftFgOver);
	APPLY_TOKEN(dialogsSendingIconFgOver);
	APPLY_TOKEN(dialogsSentIconFgOver);
	APPLY_TOKEN(dialogsUnreadBgOver);
	APPLY_TOKEN(dialogsUnreadBgMutedOver);
	APPLY_TOKEN(dialogsUnreadFgOver);
	APPLY_TOKEN(dialogsBgActive);
	APPLY_TOKEN(dialogsNameFgActive);
	APPLY_TOKEN(dialogsChatIconFgActive);
	APPLY_TOKEN(dialogsDateFgActive);
	APPLY_TOKEN(dialogsTextFgActive);
	APPLY_TOKEN(dialogsTextFgServiceActive);
	APPLY_TOKEN(dialogsDraftFgActive);
	APPLY_TOKEN(dialogsSendingIconFgActive);
	APPLY_TOKEN(dialogsSentIconFgActive);
	APPLY_TOKEN(dialogsUnreadBgActive);
	APPLY_TOKEN(dialogsUnreadBgMutedActive);
	APPLY_TOKEN(dialogsUnreadFgActive);
	APPLY_TOKEN(dialogsOnlineBadgeFgActive);
	APPLY_TOKEN(dialogsScamFgActive);
	APPLY_TOKEN(dialogsRippleBg);
	APPLY_TOKEN(dialogsCancelSearchCrossFg);
	APPLY_TOKEN(dialogsCancelSearchCrossFgOver);

	// Chat filters.
	APPLY_TOKEN(chatsFiltersLabelFg);
	APPLY_TOKEN(chatsFiltersLabelFgActive);
	APPLY_TOKEN(chatsFiltersBarFgActive);
	APPLY_TOKEN(chatsFiltersRippleBg);
	APPLY_TOKEN(chatsFiltersRippleBgActive);

	// Sidebar.
	APPLY_TOKEN(sideBarBg);
	APPLY_TOKEN(sideBarBgActive);
	APPLY_TOKEN(sideBarBgRipple);
	APPLY_TOKEN(sideBarTextFg);
	APPLY_TOKEN(sideBarTextFgActive);
	APPLY_TOKEN(sideBarIconFg);
	APPLY_TOKEN(sideBarIconFgActive);
	APPLY_TOKEN(sideBarBadgeBg);
	APPLY_TOKEN(sideBarBadgeBgMuted);
	APPLY_TOKEN(sideBarBadgeFg);

	// Searched bar.
	APPLY_TOKEN(searchedBarBg);
	APPLY_TOKEN(searchedBarFg);

	// Intro.
	APPLY_TOKEN(introBg);
	APPLY_TOKEN(introTitleFg);
	APPLY_TOKEN(introDescriptionFg);
	APPLY_TOKEN(introCoverTopBg);
	APPLY_TOKEN(introCoverBottomBg);
	APPLY_TOKEN(introCoverIconsFg);
	APPLY_TOKEN(introCoverTitleFg);
	APPLY_TOKEN(introCoverDescFg);
	APPLY_TOKEN(introVerifySuccessBg);

	// Messages.
	APPLY_TOKEN(msgInBg);
	APPLY_TOKEN(msgInBgSelected);
	APPLY_TOKEN(msgOutBg);
	APPLY_TOKEN(msgOutBgSelected);
	APPLY_TOKEN(msgInShadow);
	APPLY_TOKEN(msgInShadowSelected);
	APPLY_TOKEN(msgOutShadow);
	APPLY_TOKEN(msgOutShadowSelected);
	APPLY_TOKEN(msgSelectOverlay);
	APPLY_TOKEN(msgStickerOverlay);
	APPLY_TOKEN(msgInServiceFg);
	APPLY_TOKEN(msgInServiceFgSelected);
	APPLY_TOKEN(msgOutServiceFg);
	APPLY_TOKEN(msgOutServiceFgSelected);
	APPLY_TOKEN(msgInDateFg);
	APPLY_TOKEN(msgInDateFgSelected);
	APPLY_TOKEN(msgOutDateFg);
	APPLY_TOKEN(msgOutDateFgSelected);
	APPLY_TOKEN(msgDateImgFg);
	APPLY_TOKEN(msgDateImgBg);
	APPLY_TOKEN(msgDateImgBgOver);
	APPLY_TOKEN(msgDateImgBgSelected);
	APPLY_TOKEN(msgServiceFg);
	APPLY_TOKEN(msgServiceBg);
	APPLY_TOKEN(msgServiceBgSelected);
	APPLY_TOKEN(activeLineFg);
	APPLY_TOKEN(activeLineFgError);
	APPLY_TOKEN(msgInReplyBarColor);
	APPLY_TOKEN(msgInReplyBarSelColor);
	APPLY_TOKEN(msgOutReplyBarColor);
	APPLY_TOKEN(msgImgReplyBarColor);
	APPLY_TOKEN(msgInMonoFg);
	APPLY_TOKEN(msgOutMonoFg);

	// History view.
	APPLY_TOKEN(historyBg);
	// The chat wallpaper gradient. Every palette file must define all four, or
	// the previous theme's corners survive in these mutable globals.
	APPLY_TOKEN(historyBgTopLeft);
	APPLY_TOKEN(historyBgTopRight);
	APPLY_TOKEN(historyBgBottomRight);
	APPLY_TOKEN(historyBgBottomLeft);
	APPLY_TOKEN(topBarBg);
	APPLY_TOKEN(historyTextInFg);
	APPLY_TOKEN(historyTextOutFg);
	APPLY_TOKEN(historyLinkInFg);
	APPLY_TOKEN(historyLinkInFgSelected);
	APPLY_TOKEN(historyLinkOutFg);
	APPLY_TOKEN(historyLinkOutFgSelected);
	APPLY_TOKEN(historyOutIconFg);
	APPLY_TOKEN(historyOutIconFgSelected);
	APPLY_TOKEN(historyIconFgInverted);
	APPLY_TOKEN(historySendingOutIconFg);
	APPLY_TOKEN(historySendingInIconFg);
	APPLY_TOKEN(historySendingInvertedIconFg);
	APPLY_TOKEN(historyUnreadBarBg);
	APPLY_TOKEN(historyUnreadBarBorder);
	APPLY_TOKEN(historyUnreadBarFg);
	APPLY_TOKEN(historyForwardChooseBg);
	APPLY_TOKEN(historyForwardChooseFg);
	APPLY_TOKEN(historyScrollBarBg);
	APPLY_TOKEN(historyScrollBarBgOver);
	APPLY_TOKEN(historyScrollBg);
	APPLY_TOKEN(historyScrollBgOver);
	APPLY_TOKEN(historyToDownBg);
	APPLY_TOKEN(historyToDownBgOver);
	APPLY_TOKEN(historyToDownBgRipple);
	APPLY_TOKEN(historyToDownFg);
	APPLY_TOKEN(historyToDownFgOver);
	APPLY_TOKEN(historyToDownShadow);
	APPLY_TOKEN(historyComposeAreaBg);
	APPLY_TOKEN(historyComposeAreaFg);
	APPLY_TOKEN(historyComposeAreaFgService);
	APPLY_TOKEN(historyComposeIconFg);
	APPLY_TOKEN(historyComposeIconFgOver);
	APPLY_TOKEN(historyComposeIconFgDisabled);
	APPLY_TOKEN(historySendIconFg);
	APPLY_TOKEN(historySendIconFgOver);
	APPLY_TOKEN(historyPinnedBg);
	APPLY_TOKEN(historyReplyBg);
	APPLY_TOKEN(historyReplyNameFg);
	APPLY_TOKEN(historyReplyIconFg);
	APPLY_TOKEN(historyReplyCancelFg);
	APPLY_TOKEN(historyReplyCancelFgOver);
	APPLY_TOKEN(historyPinIconFg);

	// Peer name colors.
	APPLY_TOKEN(historyPeer1NameFg);
	APPLY_TOKEN(historyPeer2NameFg);
	APPLY_TOKEN(historyPeer3NameFg);
	APPLY_TOKEN(historyPeer4NameFg);
	APPLY_TOKEN(historyPeer5NameFg);
	APPLY_TOKEN(historyPeer6NameFg);
	APPLY_TOKEN(historyPeer7NameFg);
	APPLY_TOKEN(historyPeer8NameFg);

	// Peer userpic colors.
	APPLY_TOKEN(historyPeerUserpicFg);
	APPLY_TOKEN(peerUserpicBg1);
	APPLY_TOKEN(peerUserpicBg2);
	APPLY_TOKEN(peerUserpicBg3);
	APPLY_TOKEN(peerUserpicBg4);
	APPLY_TOKEN(peerUserpicBg5);
	APPLY_TOKEN(peerUserpicBg6);
	APPLY_TOKEN(peerUserpicBg7);
	APPLY_TOKEN(peerUserpicBg8);
	APPLY_TOKEN(peerUserpicBg1_2);
	APPLY_TOKEN(peerUserpicBg2_2);
	APPLY_TOKEN(peerUserpicBg3_2);
	APPLY_TOKEN(peerUserpicBg4_2);
	APPLY_TOKEN(peerUserpicBg5_2);
	APPLY_TOKEN(peerUserpicBg6_2);
	APPLY_TOKEN(peerUserpicBg7_2);
	APPLY_TOKEN(peerUserpicBg8_2);

	// Buttons.
	APPLY_TOKEN(activeButtonBg);
	APPLY_TOKEN(activeButtonBgOver);
	APPLY_TOKEN(activeButtonBgRipple);
	APPLY_TOKEN(activeButtonFg);
	APPLY_TOKEN(activeButtonFgOver);
	APPLY_TOKEN(activeButtonSecondaryFg);
	APPLY_TOKEN(lightButtonBg);
	APPLY_TOKEN(lightButtonBgOver);
	APPLY_TOKEN(lightButtonBgRipple);
	APPLY_TOKEN(lightButtonFg);
	APPLY_TOKEN(lightButtonFgOver);
	APPLY_TOKEN(attentionButtonFg);
	APPLY_TOKEN(attentionButtonFgOver);
	APPLY_TOKEN(attentionButtonBgOver);
	APPLY_TOKEN(attentionButtonBgRipple);

	// Input.
	APPLY_TOKEN(placeholderFg);
	APPLY_TOKEN(placeholderFgActive);
	APPLY_TOKEN(inputBorderFg);
	APPLY_TOKEN(filterInputBorderFg);
	APPLY_TOKEN(filterInputActiveBg);
	APPLY_TOKEN(filterInputInactiveBg);

	// Box.
	APPLY_TOKEN(boxBg);
	APPLY_TOKEN(boxTextFg);
	APPLY_TOKEN(boxTextFgGood);
	APPLY_TOKEN(boxTextFgError);
	APPLY_TOKEN(trustWarningFg);
	APPLY_TOKEN(boxTitleFg);
	APPLY_TOKEN(boxTitleCloseFg);
	APPLY_TOKEN(boxTitleCloseFgOver);
	APPLY_TOKEN(boxSearchBg);
	APPLY_TOKEN(boxDividerBg);
	APPLY_TOKEN(layerBg);

	// Call.
	APPLY_TOKEN(callArrowFg);
	APPLY_TOKEN(callArrowMissedFg);

	// Tooltip.
	APPLY_TOKEN(tooltipBg);
	APPLY_TOKEN(tooltipFg);
	APPLY_TOKEN(tooltipBorderFg);

	// File.
	APPLY_TOKEN(msgFileInBg);
	APPLY_TOKEN(msgFileInBgOver);
	APPLY_TOKEN(msgFileInBgSelected);
	APPLY_TOKEN(msgFileOutBg);
	APPLY_TOKEN(msgFileOutBgSelected);
	APPLY_TOKEN(msgFile1Bg);
	APPLY_TOKEN(msgFile2Bg);
	APPLY_TOKEN(msgFile3Bg);
	APPLY_TOKEN(msgFile4Bg);
	APPLY_TOKEN(historyFileInIconFg);
	APPLY_TOKEN(historyFileOutIconFg);
	APPLY_TOKEN(mediaInFg);
	APPLY_TOKEN(mediaOutFg);

	// Waveform.
	APPLY_TOKEN(msgWaveformInActive);
	APPLY_TOKEN(msgWaveformInInactive);
	APPLY_TOKEN(msgWaveformOutActive);
	APPLY_TOKEN(msgWaveformOutInactive);

	// Emoji panel.
	APPLY_TOKEN(emojiPanBg);
	APPLY_TOKEN(emojiPanCategories);
	APPLY_TOKEN(emojiPanHeaderFg);
	APPLY_TOKEN(emojiPanHover);
	APPLY_TOKEN(emojiIconFg);
	APPLY_TOKEN(emojiIconFgActive);

	// Media viewer.
	APPLY_TOKEN(mediaviewBg);
	APPLY_TOKEN(mediaviewControlFg);
	APPLY_TOKEN(mediaviewControlBg);
	APPLY_TOKEN(mediaviewCaptionBg);
	APPLY_TOKEN(mediaviewCaptionFg);
	APPLY_TOKEN(mediaviewShadowTop);
	APPLY_TOKEN(mediaviewShadowBottom);
	APPLY_TOKEN(mediaviewPlaybackActive);
	APPLY_TOKEN(mediaviewPlaybackActiveOver);
	APPLY_TOKEN(mediaviewPlaybackInactive);
	APPLY_TOKEN(mediaviewPlaybackInactiveOver);
	APPLY_TOKEN(mediaviewPlaybackProgressFg);
	APPLY_TOKEN(mediaviewPlaybackIconFg);
	APPLY_TOKEN(mediaviewPlaybackIconFgOver);
	APPLY_TOKEN(mediaviewPlaybackBg);

	// Mention autocomplete.
	APPLY_TOKEN(mentionNameFg);
	APPLY_TOKEN(mentionNameFgOver);
	APPLY_TOKEN(mentionBg);
	APPLY_TOKEN(mentionBgOver);
	APPLY_TOKEN(mentionFg);
	APPLY_TOKEN(mentionFgOver);
	APPLY_TOKEN(mentionFgActive);
	APPLY_TOKEN(mentionFgOverActive);

	// Main menu.
	APPLY_TOKEN(mainMenuRowBgOver);
	APPLY_TOKEN(mainMenuRowBgDown);
	APPLY_TOKEN(mainMenuRowText);
	APPLY_TOKEN(mainMenuRowTextAttention);
	APPLY_TOKEN(mainMenuRowIcon);
	APPLY_TOKEN(mainMenuRowIconAttention);
	APPLY_TOKEN(mainMenuHeaderNameFg);
	APPLY_TOKEN(mainMenuHeaderStatusFg);
	APPLY_TOKEN(mainMenuFooterLabelFg);
	APPLY_TOKEN(mainMenuFooterVersionFg);

	// Settings.
	APPLY_TOKEN(settingsTitleFg);
	APPLY_TOKEN(settingsCloseIconFg);
	APPLY_TOKEN(settingsCloseIconFgOver);
	APPLY_TOKEN(settingsSubsectionTitleFg);
	APPLY_TOKEN(settingsDividerBg);
	APPLY_TOKEN(settingsToggleToggledBg);
	APPLY_TOKEN(settingsToggleToggledFg);
	APPLY_TOKEN(settingsToggleUntoggledBg);
	APPLY_TOKEN(settingsToggleUntoggledFg);
	APPLY_TOKEN(settingsCheckboxTextFg);
	APPLY_TOKEN(settingsSidebarSelectedBg);
	APPLY_TOKEN(settingsMenuIconFg);
	APPLY_TOKEN(settingsMenuIconFgOver);
	APPLY_TOKEN(settingsSessionActiveFg);
	APPLY_TOKEN(settingsVerifiedDotColor);

	// Calendar.
	APPLY_TOKEN(calendarDaysFg);
	APPLY_TOKEN(calendarDayFg);
	APPLY_TOKEN(calendarGrayedOutFg);
	APPLY_TOKEN(calendarTodayBg);
	APPLY_TOKEN(calendarTodayFg);
	APPLY_TOKEN(calendarHoverBg);

	// User profile popup.
	APPLY_TOKEN(userProfileTopBg);
	APPLY_TOKEN(userProfileActionsBg);
	APPLY_TOKEN(userProfileActionBgOver);
	APPLY_TOKEN(userProfileCoverBg);
	APPLY_TOKEN(userProfileNameFg);
	APPLY_TOKEN(userProfileStatusFg);
	APPLY_TOKEN(userProfileActionFg);
	APPLY_TOKEN(userProfileActionFgOver);
	APPLY_TOKEN(userProfileActionDangerFg);
	APPLY_TOKEN(userProfileDividerBg);
	APPLY_TOKEN(userProfileUserIdFg);

	// Update InputFieldStyle structs whose color fields were captured at init.
	st::dialogsFilter.textBg = st::windowBgOver;
	st::dialogsFilter.textBgActive = st::windowBg;
	st::dialogsFilter.textFg = st::windowFg;
	st::dialogsFilter.placeholderFg = st::windowSubTextFg;
	st::dialogsFilter.borderFg = st::windowBgOver;
	st::dialogsFilter.borderFgActive = st::windowBgRipple;

	st::defaultInputField.textBg = st::windowBg;
	st::defaultInputField.textBgActive = st::windowBg;
	st::defaultInputField.textFg = st::windowFg;
	st::defaultInputField.placeholderFg = st::windowSubTextFg;
	st::defaultInputField.placeholderFgActive = st::windowActiveTextFg;
	st::defaultInputField.borderFg = st::inputBorderFg;
	st::defaultInputField.borderFgActive = st::activeLineFg;

	// Re-derive aliased colors that may not be in the palette directly.
	// These were initialized from base colors at startup, but APPLY_TOKEN
	// only updates them if the palette explicitly contains the key.
#define FALLBACK(name, base) do { \
	if (!resolve(palette, QStringLiteral(#name)).isValid()) st::name = st::base; \
} while (false)

	FALLBACK(historyToDownBg, windowBg);
	FALLBACK(historyToDownBgOver, windowBgOver);
	FALLBACK(historyToDownBgRipple, windowBgRipple);
	FALLBACK(historyToDownFg, menuIconFg);
	FALLBACK(historyToDownFgOver, menuIconFgOver);
	FALLBACK(historyComposeAreaBg, windowBg);
	FALLBACK(historyComposeAreaFg, windowFg);
	FALLBACK(placeholderFg, windowSubTextFg);
	FALLBACK(placeholderFgActive, windowActiveTextFg);
	FALLBACK(historyComposeAreaFgService, msgInDateFg);
	FALLBACK(historyComposeIconFg, menuIconFg);
	FALLBACK(historyComposeIconFgOver, menuIconFgOver);
	FALLBACK(historyRecordVoiceFg, historyComposeIconFg);
	FALLBACK(historyRecordVoiceFgOver, historyComposeIconFgOver);
	FALLBACK(historyRecordVoiceFgActive, windowBgActive);
	FALLBACK(historyRecordVoiceFgActiveIcon, windowFgActive);

#undef FALLBACK
}

#undef APPLY_TOKEN

} // namespace

ThemeManager::ThemeManager(QObject *parent)
	: QObject(parent)
	, _themeId(kDefaultThemeId)
{
	loadPalettes();
}

ThemeManager::~ThemeManager() = default;

bool ThemeManager::nightForMode(ThemeMode mode) const {
	switch (mode) {
	case ThemeMode::Day:
		return false;
	case ThemeMode::Night:
		return true;
	case ThemeMode::System:
		return _systemDark.value_or(false);
	}
	return false;
}

void ThemeManager::initializeFromSettings(
		const QString &themeId,
		ThemeMode mode,
		bool systemDarkModeEnabled) {
	// Resolve the family first: applyTheme() below must read the right palettes.
	const auto &theme = ThemeById(themeId);
	if (theme.id != _themeId) {
		_themeId = theme.id;
		loadPalettes();
	}
	_mode = mode;
	_systemDarkModeEnabled = systemDarkModeEnabled;
	auto isNight = nightForMode(mode);
	if (_systemDarkModeEnabled && _systemDark.has_value()) {
		isNight = *_systemDark;
		_mode = isNight ? ThemeMode::Night : ThemeMode::Day;
	} else if (_mode == ThemeMode::System) {
		_mode = isNight ? ThemeMode::Night : ThemeMode::Day;
	}
	applyTheme(isNight);
}

void ThemeManager::setMode(ThemeMode mode) {
	// Manually choosing a concrete Day/Night theme overrides "Auto-night mode"
	// (following the OS) so the system watcher no longer flips it back.
	const bool disableAutoNight =
		(mode == ThemeMode::Day || mode == ThemeMode::Night)
		&& _systemDarkModeEnabled;
	if (_mode == mode && !disableAutoNight) {
		return;
	}
	if (disableAutoNight) {
		_systemDarkModeEnabled = false;
	}
	_mode = mode;
	applyTheme(nightForMode(mode));
}

void ThemeManager::toggleNightMode() {
	setMode(_isNight ? ThemeMode::Day : ThemeMode::Night);
}

void ThemeManager::setThemeAndMode(const QString &id, ThemeMode mode) {
	const auto &theme = ThemeById(id);
	const auto disableAutoNight =
		(mode == ThemeMode::Day || mode == ThemeMode::Night)
		&& _systemDarkModeEnabled;
	const auto isNight = nightForMode(mode);
	if (theme.id == _themeId && _mode == mode && !disableAutoNight) {
		return;
	}
	if (disableAutoNight) {
		_systemDarkModeEnabled = false;
	}
	if (theme.id != _themeId) {
		_themeId = theme.id;
		loadPalettes();
	}
	_mode = mode;
	applyTheme(isNight);
}

void ThemeManager::setSystemDarkModeEnabled(bool enabled) {
	if (_systemDarkModeEnabled == enabled) {
		return;
	}
	_systemDarkModeEnabled = enabled;
	if (enabled && _systemDark.has_value() && (*_systemDark != _isNight)) {
		_mode = *_systemDark ? ThemeMode::Night : ThemeMode::Day;
		applyTheme(*_systemDark);
	}
}

void ThemeManager::setSystemDarkState(std::optional<bool> isDark) {
	_systemDark = isDark;
	if (_systemDarkModeEnabled && isDark.has_value() && (*isDark != _isNight)) {
		_mode = *isDark ? ThemeMode::Night : ThemeMode::Day;
		applyTheme(*isDark);
	}
}

void ThemeManager::loadPalettes() {
	const auto &theme = ThemeById(_themeId);
	_dayPalette = LoadPalette(theme.palettePath(false));
	_nightPalette = LoadPalette(theme.palettePath(true));
	// The wallpaper compositor rasterises whichever doodle this points at.
	SetPatternPath(theme.patternPath());
}

void ThemeManager::applyTheme(bool isNight) {
	_isNight = isNight;

	const auto &palette = isNight ? _nightPalette : _dayPalette;
	applyPalette(palette);

	if (isNight) {
		st::utdBg = QColor(0x22, 0x1B, 0x12);
		st::utdTitleFg = QColor(0xFF, 0xAB, 0x00);
		st::utdBodyFg = QColor(0xC0, 0xC7, 0xD2);
		st::utdLinkFg = QColor(0x92, 0xCC, 0xFF);
	} else {
		st::utdBg = QColor(0xFE, 0xF4, 0xE5);
		st::utdTitleFg = QColor(0x1B, 0x1B, 0x1C);
		st::utdBodyFg = QColor(0x41, 0x47, 0x4F);
		st::utdLinkFg = QColor(0x20, 0x63, 0x90);
	}

	applyAppPalette(isNight);
	TeleMatrix::Style::IconProvider::clearCache();
	TeleMatrix::clearMediaviewTintCache();

	emit themeChanged(isNight, _mode);
}

void ThemeManager::applyPalette(const QHash<QString, QColor> &palette) {
	applyAllTokens(palette);
}

void ThemeManager::applyAppPalette(bool isNight) {
	// Update the global QPalette so all standard Qt widgets pick up
	// the correct base colors.
	QPalette pal;
	pal.setColor(QPalette::Window, st::windowBg);
	pal.setColor(QPalette::WindowText, st::windowFg);
	pal.setColor(QPalette::Base, st::windowBg);
	pal.setColor(QPalette::AlternateBase, st::windowBgOver);
	pal.setColor(QPalette::Text, st::windowFg);
	pal.setColor(QPalette::Button, st::windowBgOver);
	pal.setColor(QPalette::ButtonText, st::windowFg);
	pal.setColor(QPalette::Highlight, st::windowBgActive);
	pal.setColor(QPalette::HighlightedText, st::windowFgActive);
	pal.setColor(QPalette::PlaceholderText, st::placeholderFg);
	pal.setColor(QPalette::ToolTipBase, st::tooltipBg);
	pal.setColor(QPalette::ToolTipText, st::tooltipFg);

	if (auto *app = qApp) {
		app->setPalette(pal);
	}
}

} // namespace TeleMatrix::Theme

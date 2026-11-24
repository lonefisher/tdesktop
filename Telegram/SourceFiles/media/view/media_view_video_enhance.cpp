/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/view/media_view_video_enhance.h"

#include "lang/lang_keys.h"
#include "media/streaming/media_streaming_rtx_vsr.h"
#include "menu/menu_checked_action.h"
#include "ui/widgets/popup_menu.h"

namespace Media::View {
namespace {

void AddSuperResolution(not_null<Ui::PopupMenu*> menu) {
	const auto state = Streaming::RtxVsrToggleState(
		Streaming::RtxVideoSuperResolutionEnabled(),
		Streaming::RtxVideoSuperResolutionSupported());
	const auto action = Menu::AddCheckedAction(
		menu,
		tr::lng_fishgram_rtx_vsr(tr::now),
		[=] {
			Streaming::SetRtxVideoSuperResolutionEnabled(
				Streaming::RtxVsrStoredAfterToggle(
					Streaming::RtxVideoSuperResolutionEnabled(),
					Streaming::RtxVideoSuperResolutionSupported()));
		},
		nullptr,
		state.checked);
	action->setEnabled(state.available);
}

} // namespace

void FillVideoEnhanceMenu(not_null<Ui::PopupMenu*> menu) {
	AddSuperResolution(menu);
}

} // namespace Media::View

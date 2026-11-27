/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/view/media_view_video_enhance.h"

#include "lang/lang_keys.h"
#include "media/streaming/media_streaming_rtx_fg.h"
#include "media/streaming/media_streaming_rtx_nr.h"
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

void AddVideoHdr(not_null<Ui::PopupMenu*> menu, quintptr windowId) {
	const auto available = Streaming::RtxHdrShortcutAvailable(
		Streaming::RtxVideoHdrSupported(),
		Streaming::RtxWindowOnHdrDisplay(windowId));
	const auto action = Menu::AddCheckedAction(
		menu,
		tr::lng_fishgram_rtx_hdr(tr::now),
		[=] {
			const auto allowed = Streaming::RtxHdrShortcutAvailable(
				Streaming::RtxVideoHdrSupported(),
				Streaming::RtxWindowOnHdrDisplay(windowId));
			if (!allowed) {
				return;
			}
			const auto next = !Streaming::RtxVideoHdrEnabled();
			Streaming::RtxVideoHdrSetEnabled(next);
		},
		nullptr,
		Streaming::RtxVideoHdrEnabled());
	action->setEnabled(available);
}

void AddFrameGeneration(not_null<Ui::PopupMenu*> menu) {
	const auto available = Streaming::RtxVideoFrameGenerationMenuAvailable();
	const auto action = Menu::AddCheckedAction(
		menu,
		tr::lng_fishgram_rtx_fg(tr::now),
		[=] {
			if (!Streaming::RtxVideoFrameGenerationMenuAvailable()) {
				return;
			}
			const auto next = !Streaming::RtxVideoFrameGenerationEnabled();
			Streaming::SetRtxVideoFrameGenerationEnabled(next);
		},
		nullptr,
		Streaming::RtxVideoFrameGenerationEnabled());
	action->setEnabled(available);
}

void AddDlssNr(not_null<Ui::PopupMenu*> menu) {
	const auto available = Streaming::RtxDlssNrAvailable();
	const auto action = Menu::AddCheckedAction(
		menu,
		tr::lng_fishgram_dlss_nr(tr::now),
		[=] {
			if (!Streaming::RtxDlssNrAvailable()) {
				return;
			}
			const auto next = !Streaming::RtxDlssNrEnabled();
			Streaming::RtxDlssNrSetEnabled(next);
		},
		nullptr,
		Streaming::RtxDlssNrEnabled());
	action->setEnabled(available);
}

} // namespace

void FillVideoEnhanceMenu(
		not_null<Ui::PopupMenu*> menu,
		quintptr windowId) {
	AddSuperResolution(menu);
	AddVideoHdr(menu, windowId);
	AddFrameGeneration(menu);
	AddDlssNr(menu);
}

} // namespace Media::View

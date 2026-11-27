/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "media/streaming/media_streaming_common.h"
#include "media/streaming/media_streaming_rtx_nr_plan.h"

#include <rpl/rpl.h>

namespace Media::Streaming {

[[nodiscard]] bool RtxDlssNrSettingVisible();
[[nodiscard]] bool RtxDlssNrEnabled();
void RtxDlssNrSetEnabled(bool enabled);
[[nodiscard]] rpl::producer<bool> RtxDlssNrEnabledValue();
[[nodiscard]] bool RtxDlssNrAvailable();
void RtxDlssNrNoteDiscontinuity();
void RtxDlssNrNoteNewClip();

[[nodiscard]] QImage RtxDlssNrImage(
	const FrameWithInfo &frame,
	bool fullscreen,
	int windowLongEdge);

[[nodiscard]] inline FrameWithInfo WithRtxDlssNr(
		FrameWithInfo frame,
		bool fullscreen,
		int windowLongEdge) {
	auto image = RtxDlssNrImage(frame, fullscreen, windowLongEdge);
	if (image.isNull()) {
		return frame;
	}
	frame.image = std::move(image);
	frame.yuv = nullptr;
	frame.nativeFrame = nullptr;
	frame.format = FrameFormat::ARGB32;
	frame.alpha = false;
	return frame;
}

} // namespace Media::Streaming

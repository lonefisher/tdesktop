/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "media/streaming/media_streaming_common.h"
#include "media/streaming/media_streaming_rtx_vsr_plan.h"

namespace Media::Streaming {

[[nodiscard]] bool RtxVideoSuperResolutionEnabled();
[[nodiscard]] bool RtxVideoSuperResolutionSupported();
void RtxVideoSuperResolutionNoteToggled(bool enabled);

[[nodiscard]] QImage RtxSuperResolvedImage(
	const FrameWithInfo &frame,
	QSize targetPixels);

[[nodiscard]] inline QSize RtxUnrotatedTarget(QSize target, int rotation) {
	if ((rotation % 180) == 90) {
		target.transpose();
	}
	return target;
}

[[nodiscard]] inline FrameWithInfo WithRtxSuperResolution(
		FrameWithInfo frame,
		QSize targetPixels) {
	auto image = RtxSuperResolvedImage(frame, targetPixels);
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

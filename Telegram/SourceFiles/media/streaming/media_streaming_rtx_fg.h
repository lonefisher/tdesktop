/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "media/streaming/media_streaming_rtx_fg_plan.h"
#include "rpl/producer.h"

#include <cstdint>

class QImage;

namespace Media::Streaming {

[[nodiscard]] bool RtxVideoFrameGenerationEnabled();
[[nodiscard]] rpl::producer<bool> RtxVideoFrameGenerationEnabledValue();
void SetRtxVideoFrameGenerationEnabled(bool enabled);
[[nodiscard]] bool RtxVideoFrameGenerationSupported();
[[nodiscard]] bool RtxVideoFrameGenerationMenuAvailable();
[[nodiscard]] bool RtxFrameGenerationBlocked();
void RtxVideoFrameGenerationNoteToggled(bool enabled);
void RtxFrameGenerationNoteClip(std::uintptr_t clip);
void RtxFrameGenerationNoteSkip(
	RtxFgReason reason,
	int width,
	int height);

[[nodiscard]] QImage RtxGeneratedFrame(
	const QImage &previous,
	const QImage &next,
	float timestep);

} // namespace Media::Streaming

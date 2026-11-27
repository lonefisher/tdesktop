/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "media/streaming/media_streaming_common.h"
#include "media/streaming/media_streaming_rtx_vsr_plan.h"

#include "rpl/producer.h"

#include <QRegion>

namespace Media::Streaming {

[[nodiscard]] bool RtxVideoSuperResolutionEnabled();
[[nodiscard]] rpl::producer<bool> RtxVideoSuperResolutionEnabledValue();
void SetRtxVideoSuperResolutionEnabled(bool enabled);
[[nodiscard]] bool RtxVideoSuperResolutionSupported();
void RtxVideoSuperResolutionNoteToggled(bool enabled);

enum class RtxVsrPath : unsigned char {
	Overlay,
	Track,
	Pip,
	Rhi,
};

void RtxNotePresentation(
	const char *swapFormat,
	int pixelWidth,
	int pixelHeight,
	float devicePixelRatio);

[[nodiscard]] bool RtxVideoHdrEnabled();
[[nodiscard]] rpl::producer<bool> RtxVideoHdrEnabledValue();
[[nodiscard]] bool RtxVideoHdrSupported();
[[nodiscard]] bool RtxWindowOnHdrDisplay(quintptr windowId);
void RtxVideoHdrSetEnabled(bool enabled);
void RtxVideoHdrNoteToggled(bool enabled);
void RtxHdrReleaseWindow(quintptr windowId);

struct RtxEnhanceRequest {
	QSize targetPixels;
	quintptr windowId = 0;
	QRect videoScreenPx;
	QRegion controlHoles;
	quint64 videoTag = 0;
	int rotation = 0;
	bool suspendHdr = false;
	bool toneMapOnly = false;
	bool viewerNr = false;
	bool viewerFullscreen = false;
	int viewerLongEdge = 0;
	RtxVsrPath path = RtxVsrPath::Overlay;

};

[[nodiscard]] bool RtxHdrOverlayIncludesNr();

[[nodiscard]] QRect RtxScreenRect(
	quintptr windowId,
	QRect logicalInTopLevel,
	double devicePixelRatio);

[[nodiscard]] QImage RtxSuperResolvedImage(
	const FrameWithInfo &frame,
	QSize targetPixels,
	RtxVsrPath path = RtxVsrPath::Overlay,
	quint64 videoTag = 0);

[[nodiscard]] QImage RtxEnhancedImage(
	const FrameWithInfo &frame,
	const RtxEnhanceRequest &request);

[[nodiscard]] inline QSize RtxUnrotatedTarget(QSize target, int rotation) {
	if ((rotation % 180) == 90) {
		target.transpose();
	}
	return target;
}

[[nodiscard]] inline FrameWithInfo ApplyRtxImage(
		FrameWithInfo frame,
		QImage image) {
	if (image.isNull()) {
		return frame;
	}
	frame.image = std::move(image);
	frame.yuv = nullptr;
	frame.nativeFrame = nullptr;
	frame.format = FrameFormat::ARGB32;
	frame.alpha = false;
	frame.avPixelFormat = -1;
	frame.sourceHdr = false;
	return frame;
}

[[nodiscard]] inline FrameWithInfo WithRtxSuperResolution(
		FrameWithInfo frame,
		QSize targetPixels,
		RtxVsrPath path = RtxVsrPath::Overlay,
		quint64 videoTag = 0) {
	auto image = RtxSuperResolvedImage(
		frame,
		targetPixels,
		path,
		videoTag);
	return ApplyRtxImage(std::move(frame), std::move(image));
}

[[nodiscard]] inline FrameWithInfo WithRtxEnhanced(
		FrameWithInfo frame,
		const RtxEnhanceRequest &request) {
	auto image = RtxEnhancedImage(frame, request);
	return ApplyRtxImage(std::move(frame), std::move(image));
}

} // namespace Media::Streaming

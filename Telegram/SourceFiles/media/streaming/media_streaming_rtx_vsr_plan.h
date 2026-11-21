/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace Media::Streaming {

inline constexpr auto kRtxVideoSuperResolutionPref
	= "fishgram/rtx_video_super_resolution";
inline constexpr auto kRtxVideoSuperResolutionDefault = true;
inline constexpr auto kRtxVideoHdrPref = "fishgram/rtx_video_hdr";
inline constexpr auto kRtxVideoHdrDefault = true;

// Any NVIDIA adapter. Device id is intentionally not part of the check.
inline constexpr auto kRtxNvidiaVendorId = 0x10DEu;

inline constexpr auto kRtxVsrMaxScale = 4;
inline constexpr auto kRtxVsrMaxLongEdge = 3840;
inline constexpr auto kRtxVsrMinSourceEdge = 64;
inline constexpr auto kRtxVsrFailureBudget = 3;

struct RtxVsrSize {
	int width = 0;
	int height = 0;

	[[nodiscard]] bool isEmpty() const {
		return width <= 0 || height <= 0;
	}

	friend bool operator==(RtxVsrSize a, RtxVsrSize b) {
		return a.width == b.width && a.height == b.height;
	}

};

enum class RtxVsrDecision {
	Skip,
	Upscale,
};

struct RtxVsrPlan {
	RtxVsrDecision decision = RtxVsrDecision::Skip;
	RtxVsrSize output;

};

struct RtxVsrOutputList {
	RtxVsrSize sizes[4] = {};
	int count = 0;

};

enum class RtxVsrSessionState {
	Ready,
	Unsupported,
	Blocked,
};

enum class RtxVsrBackendResult {
	Ok,
	Unsupported,
	Failed,
};

struct RtxVsrSession {
	RtxVsrSessionState state = RtxVsrSessionState::Ready;
	int failures = 0;

};

[[nodiscard]] inline bool RtxAdapterQualifies(
		unsigned vendorId,
		bool softwareAdapter,
		bool remoteSession) {
	return !remoteSession
		&& !softwareAdapter
		&& (vendorId == kRtxNvidiaVendorId);
}

// A missing pref uses the default. An empty stored value is an explicit off.
[[nodiscard]] inline bool RtxBoolPrefEnabled(
		bool present,
		bool nonEmpty,
		bool fallback = kRtxVideoSuperResolutionDefault) {
	return present ? nonEmpty : fallback;
}

[[nodiscard]] inline bool RtxVsrBlocked(RtxVsrSession session) {
	return session.state != RtxVsrSessionState::Ready;
}

[[nodiscard]] inline RtxVsrSession ResetRtxVsrSession() {
	return {};
}

[[nodiscard]] inline RtxVsrSession NoteRtxVsrResult(
		RtxVsrSession session,
		RtxVsrBackendResult result) {
	if (session.state != RtxVsrSessionState::Ready) {
		return session;
	}
	if (result == RtxVsrBackendResult::Ok) {
		session.failures = 0;
		return session;
	}
	if (result == RtxVsrBackendResult::Unsupported) {
		session.state = RtxVsrSessionState::Unsupported;
		session.failures = 0;
		return session;
	}
	++session.failures;
	if (session.failures >= kRtxVsrFailureBudget) {
		session.state = RtxVsrSessionState::Blocked;
	}
	return session;
}

[[nodiscard]] inline int RtxLongEdge(RtxVsrSize size) {
	return (size.width > size.height) ? size.width : size.height;
}

[[nodiscard]] inline RtxVsrSize RtxFitEven(
		RtxVsrSize source,
		int limitWidth,
		int limitHeight) {
	if (source.width <= 0
		|| source.height <= 0
		|| limitWidth <= source.width
		|| limitHeight <= source.height) {
		return {};
	}
	const auto widthBound = std::int64_t(limitWidth) * source.height;
	const auto heightBound = std::int64_t(limitHeight) * source.width;
	auto outWidth = 0;
	auto outHeight = 0;
	if (widthBound < heightBound) {
		outWidth = limitWidth;
		outHeight = int(
			(std::int64_t(limitWidth) * source.height) / source.width);
	} else {
		outHeight = limitHeight;
		outWidth = int(
			(std::int64_t(limitHeight) * source.width) / source.height);
	}
	outWidth &= ~1;
	outHeight &= ~1;
	if (outWidth <= source.width || outHeight <= source.height) {
		return {};
	}
	return { outWidth, outHeight };
}

[[nodiscard]] inline RtxVsrOutputList RtxVsrOutputs(
		RtxVsrSize source,
		RtxVsrSize target) {
	auto result = RtxVsrOutputList();
	if ((source.width < kRtxVsrMinSourceEdge)
		|| (source.height < kRtxVsrMinSourceEdge)
		|| (source.width % 2)
		|| (source.height % 2)
		|| target.isEmpty()) {
		return result;
	}
	const auto limitWidth = int(std::min(
		std::min(
			std::int64_t(target.width),
			std::int64_t(source.width) * kRtxVsrMaxScale),
		std::int64_t(kRtxVsrMaxLongEdge)));
	const auto limitHeight = int(std::min(
		std::min(
			std::int64_t(target.height),
			std::int64_t(source.height) * kRtxVsrMaxScale),
		std::int64_t(kRtxVsrMaxLongEdge)));
	const auto primary = RtxFitEven(source, limitWidth, limitHeight);
	if (primary.isEmpty()) {
		return result;
	}
	const auto push = [&](RtxVsrSize size) {
		if (size.isEmpty() || result.count == 4) {
			return;
		}
		for (auto i = 0; i != result.count; ++i) {
			if (result.sizes[i] == size) {
				return;
			}
		}
		result.sizes[result.count++] = size;
	};
	push(primary);
	for (const auto edge : { 2560, 1920 }) {
		if (RtxLongEdge(primary) <= edge) {
			continue;
		}
		const auto fitted = RtxFitEven(source, edge, edge);
		if (!fitted.isEmpty() && RtxLongEdge(fitted) < RtxLongEdge(primary)) {
			push(fitted);
		}
	}
	return result;
}

[[nodiscard]] inline RtxVsrPlan PlanRtxVideoSuperResolution(
		bool enabled,
		bool supported,
		bool blocked,
		RtxVsrSize source,
		RtxVsrSize target) {
	auto plan = RtxVsrPlan();
	if (!enabled || !supported || blocked) {
		return plan;
	}
	const auto outputs = RtxVsrOutputs(source, target);
	if (!outputs.count) {
		return plan;
	}
	plan.decision = RtxVsrDecision::Upscale;
	plan.output = outputs.sizes[0];
	return plan;
}

[[nodiscard]] inline bool PackNv12(
		const unsigned char *y,
		int yStride,
		const unsigned char *u,
		int uStride,
		const unsigned char *v,
		int vStride,
		int width,
		int height,
		unsigned char *dest,
		int destStride) {
	if (!y
		|| !u
		|| !v
		|| !dest
		|| width < 2
		|| height < 2
		|| (width % 2)
		|| (height % 2)
		|| yStride < width
		|| uStride < (width / 2)
		|| vStride < (width / 2)
		|| destStride < width) {
		return false;
	}
	for (auto row = 0; row != height; ++row) {
		std::memcpy(
			dest + (std::size_t(row) * destStride),
			y + (std::size_t(row) * yStride),
			width);
	}
	auto *uv = dest + (std::size_t(destStride) * height);
	for (auto row = 0; row != (height / 2); ++row) {
		auto *dst = uv + (std::size_t(row) * destStride);
		const auto *uRow = u + (std::size_t(row) * uStride);
		const auto *vRow = v + (std::size_t(row) * vStride);
		for (auto col = 0; col != (width / 2); ++col) {
			dst[col * 2] = uRow[col];
			dst[(col * 2) + 1] = vRow[col];
		}
	}
	return true;
}

[[nodiscard]] inline int RtxSampleMaxByte(
		const unsigned char *data,
		int width,
		int height,
		int stride,
		int pixelBytes,
		int channels) {
	if (!data
		|| width <= 0
		|| height <= 0
		|| pixelBytes <= 0
		|| channels <= 0
		|| stride < (width * pixelBytes)) {
		return 0;
	}
	auto maxValue = 0;
	const auto stepX = (width > 8) ? (width / 8) : 1;
	const auto stepY = (height > 8) ? (height / 8) : 1;
	for (auto y = 0; y < height; y += stepY) {
		const auto row = data + (std::size_t(y) * stride);
		for (auto x = 0; x < width; x += stepX) {
			const auto pixel = row + (std::size_t(x) * pixelBytes);
			for (auto channel = 0; channel != channels; ++channel) {
				const auto value = int(pixel[channel]);
				if (value > maxValue) {
					maxValue = value;
				}
			}
		}
	}
	return maxValue;
}

[[nodiscard]] inline bool RtxOutputRejectedAsBlank(
		int sourceMax,
		int outputMax) {
	return (sourceMax > 16) && (outputMax == 0);
}

enum class RtxHdrDecision {
	Skip,
	ToneMap,
};

struct RtxHdrPlan {
	RtxHdrDecision decision = RtxHdrDecision::Skip;

};

enum class RtxHdrSkipReason {
	None,
	Disabled,
	Unsupported,
	Blocked,
	DisplaySdr,
	SourceHdr,
	Rotated,
	Alpha,
};

[[nodiscard]] inline const char *RtxHdrSkipReasonName(
		RtxHdrSkipReason reason) {
	switch (reason) {
	case RtxHdrSkipReason::None: return "none";
	case RtxHdrSkipReason::Disabled: return "disabled";
	case RtxHdrSkipReason::Unsupported: return "unsupported";
	case RtxHdrSkipReason::Blocked: return "blocked";
	case RtxHdrSkipReason::DisplaySdr: return "display-sdr";
	case RtxHdrSkipReason::SourceHdr: return "source-hdr";
	case RtxHdrSkipReason::Rotated: return "rotated";
	case RtxHdrSkipReason::Alpha: return "alpha";
	}
	return "other";
}

[[nodiscard]] inline RtxHdrSkipReason RtxHdrSkipReasonFor(
		bool enabled,
		bool supported,
		bool blocked,
		bool displayHdr,
		bool sourceHdr,
		bool rotated,
		bool alpha) {
	if (!enabled) {
		return RtxHdrSkipReason::Disabled;
	}
	if (!supported) {
		return RtxHdrSkipReason::Unsupported;
	}
	if (blocked) {
		return RtxHdrSkipReason::Blocked;
	}
	if (!displayHdr) {
		return RtxHdrSkipReason::DisplaySdr;
	}
	if (sourceHdr) {
		return RtxHdrSkipReason::SourceHdr;
	}
	if (rotated) {
		return RtxHdrSkipReason::Rotated;
	}
	if (alpha) {
		return RtxHdrSkipReason::Alpha;
	}
	return RtxHdrSkipReason::None;
}

[[nodiscard]] inline RtxHdrPlan PlanRtxVideoHdr(
		bool enabled,
		bool supported,
		bool blocked,
		bool displayHdr,
		bool sourceHdr,
		bool rotated,
		bool alpha) {
	auto plan = RtxHdrPlan();
	if (RtxHdrSkipReasonFor(
			enabled,
			supported,
			blocked,
			displayHdr,
			sourceHdr,
			rotated,
			alpha) == RtxHdrSkipReason::None) {
		plan.decision = RtxHdrDecision::ToneMap;
	}
	return plan;
}

} // namespace Media::Streaming

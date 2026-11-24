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
[[nodiscard]] inline bool RtxBoolPrefEnabled(bool present, bool nonEmpty) {
	return present ? nonEmpty : kRtxVideoSuperResolutionDefault;
}

struct RtxVsrToggle {
	bool checked = false;
	bool available = false;
};

[[nodiscard]] inline RtxVsrToggle RtxVsrToggleState(
		bool storedEnabled,
		bool supported) {
	return {
		.checked = storedEnabled,
		.available = supported,
	};
}

// A gray row does not change the stored bit the settings switch reads.
[[nodiscard]] inline bool RtxVsrStoredAfterToggle(
		bool storedEnabled,
		bool supported) {
	return supported ? !storedEnabled : storedEnabled;
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

// NV12 needs an even coded size. One extra coded row or column is cropped.
[[nodiscard]] inline RtxVsrSize RtxEvenSize(RtxVsrSize size) {
	if (size.width > 0 && (size.width % 2)) {
		--size.width;
	}
	if (size.height > 0 && (size.height % 2)) {
		--size.height;
	}
	return size;
}

enum class RtxVsrRejectReason {
	None,
	EmptyTarget,
	TinySource,
	TargetNotLarger,
	OneSide,
	AlreadyLarge,
	EvenFit,
};

[[nodiscard]] inline const char *RtxVsrRejectReasonName(
		RtxVsrRejectReason reason) {
	switch (reason) {
	case RtxVsrRejectReason::None: return "none";
	case RtxVsrRejectReason::EmptyTarget: return "empty-target";
	case RtxVsrRejectReason::TinySource: return "tiny-source";
	case RtxVsrRejectReason::TargetNotLarger: return "target-not-larger";
	case RtxVsrRejectReason::OneSide: return "one-side";
	case RtxVsrRejectReason::AlreadyLarge: return "already-large";
	case RtxVsrRejectReason::EvenFit: return "even-fit";
	}
	return "other";
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
	source = RtxEvenSize(source);
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

[[nodiscard]] inline RtxVsrRejectReason RtxVsrRejectReasonFor(
		RtxVsrSize source,
		RtxVsrSize target) {
	if (target.isEmpty()) {
		return RtxVsrRejectReason::EmptyTarget;
	}
	source = RtxEvenSize(source);
	if ((source.width < kRtxVsrMinSourceEdge)
		|| (source.height < kRtxVsrMinSourceEdge)) {
		return RtxVsrRejectReason::TinySource;
	}
	if (RtxVsrOutputs(source, target).count) {
		return RtxVsrRejectReason::None;
	}
	const auto wider = target.width > source.width;
	const auto taller = target.height > source.height;
	if (wider && taller) {
		const auto cappedWidth = int(std::min(
			std::min(
				std::int64_t(target.width),
				std::int64_t(source.width) * kRtxVsrMaxScale),
			std::int64_t(kRtxVsrMaxLongEdge)));
		const auto cappedHeight = int(std::min(
			std::min(
				std::int64_t(target.height),
				std::int64_t(source.height) * kRtxVsrMaxScale),
			std::int64_t(kRtxVsrMaxLongEdge)));
		if (cappedWidth <= source.width || cappedHeight <= source.height) {
			return RtxVsrRejectReason::AlreadyLarge;
		}
		return RtxVsrRejectReason::EvenFit;
	}
	if (wider || taller) {
		return RtxVsrRejectReason::OneSide;
	}
	return RtxVsrRejectReason::TargetNotLarger;
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

} // namespace Media::Streaming

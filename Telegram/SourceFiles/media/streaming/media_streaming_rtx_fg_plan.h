/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Media::Streaming {

inline constexpr auto kRtxVideoFrameGenerationPref
	= "fishgram/rtx_video_frame_generation";
inline constexpr auto kRtxVideoFrameGenerationDefault = false;

inline constexpr auto kRtxFgMinEdge = 64;
inline constexpr auto kRtxFgMaxLongEdge = 3840;
inline constexpr auto kRtxFgMinWindowLongEdge = 1280;
inline constexpr auto kRtxFgMaxGapMs = 250;
inline constexpr auto kRtxFgMinRefreshMs = 4;
inline constexpr auto kRtxFgMaxRefreshMs = 50;
inline constexpr auto kRtxFgDefaultRefreshMs = 16;
inline constexpr auto kRtxFgEndpointMilli = 80;
inline constexpr auto kRtxFgFailureBudget = 3;

enum class RtxFgDecision {
	ShowSource,
	Interpolate,
};

enum class RtxFgReason {
	None,
	Off,
	Unsupported,
	Blocked,
	SmallWindow,
	NotPlaying,
	Alpha,
	Size,
	Pair,
	GapShort,
	GapLong,
	Endpoint,
};

enum class RtxFgSessionState {
	Ready,
	Unsupported,
	Blocked,
};

enum class RtxFgBackendResult {
	Ok,
	Unsupported,
	Failed,
};

struct RtxFgSession {
	RtxFgSessionState state = RtxFgSessionState::Ready;
	int failures = 0;
};

struct RtxFgPlan {
	RtxFgDecision decision = RtxFgDecision::ShowSource;
	RtxFgReason reason = RtxFgReason::Off;
	float timestep = 0.f;
	std::int64_t pictureTime = 0;
	std::int64_t nextWake = 0;
};

struct RtxFgClock {
	bool enabled = false;
	bool supported = false;
	bool blocked = false;
	bool fullscreen = false;
	bool playing = false;
	bool alpha = false;
	bool pair = false;
	int windowWidth = 0;
	int windowHeight = 0;
	int width = 0;
	int height = 0;
	int nextWidth = 0;
	int nextHeight = 0;
	std::int64_t previousDisplay = 0;
	std::int64_t nextDisplay = 0;
	std::int64_t previousPosition = 0;
	std::int64_t nextPosition = 0;
	std::int64_t now = 0;
	int refreshMs = 0;
	std::int64_t waitingSince = 0;
};

[[nodiscard]] inline const char *RtxFgReasonName(RtxFgReason reason) {
	switch (reason) {
	case RtxFgReason::None: return "none";
	case RtxFgReason::Off: return "off";
	case RtxFgReason::Unsupported: return "unsupported";
	case RtxFgReason::Blocked: return "blocked";
	case RtxFgReason::SmallWindow: return "small-window";
	case RtxFgReason::NotPlaying: return "not-playing";
	case RtxFgReason::Alpha: return "alpha";
	case RtxFgReason::Size: return "size";
	case RtxFgReason::Pair: return "pair";
	case RtxFgReason::GapShort: return "gap-short";
	case RtxFgReason::GapLong: return "gap-long";
	case RtxFgReason::Endpoint: return "endpoint";
	}
	return "unknown";
}

[[nodiscard]] inline bool RtxFgPrefEnabled(bool present, bool nonEmpty) {
	return present ? nonEmpty : kRtxVideoFrameGenerationDefault;
}

[[nodiscard]] inline bool RtxFgBlocked(RtxFgSession session) {
	return session.state != RtxFgSessionState::Ready;
}

[[nodiscard]] inline RtxFgSession ResetRtxFgSession() {
	return {};
}

[[nodiscard]] inline RtxFgSession NoteRtxFgResult(
		RtxFgSession session,
		RtxFgBackendResult result) {
	if (session.state != RtxFgSessionState::Ready) {
		return session;
	}
	if (result == RtxFgBackendResult::Ok) {
		session.failures = 0;
		return session;
	}
	if (result == RtxFgBackendResult::Unsupported) {
		session.state = RtxFgSessionState::Unsupported;
		session.failures = 0;
		return session;
	}
	++session.failures;
	if (session.failures >= kRtxFgFailureBudget) {
		session.state = RtxFgSessionState::Blocked;
	}
	return session;
}

[[nodiscard]] inline int RtxFgRefreshMs(int refreshMs) {
	if (refreshMs < kRtxFgMinRefreshMs) {
		return (refreshMs <= 0)
			? kRtxFgDefaultRefreshMs
			: kRtxFgMinRefreshMs;
	}
	return (refreshMs > kRtxFgMaxRefreshMs)
		? kRtxFgMaxRefreshMs
		: refreshMs;
}

[[nodiscard]] inline bool RtxFgViewerQualifies(
		bool fullscreen,
		int windowWidth,
		int windowHeight) {
	if (fullscreen) {
		return true;
	}
	const auto longEdge = (windowWidth > windowHeight)
		? windowWidth
		: windowHeight;
	return longEdge >= kRtxFgMinWindowLongEdge;
}

[[nodiscard]] inline bool RtxFgFrameSizeOk(int width, int height) {
	if (width < kRtxFgMinEdge || height < kRtxFgMinEdge) {
		return false;
	}
	const auto longEdge = (width > height) ? width : height;
	return longEdge <= kRtxFgMaxLongEdge;
}

// Synthesized pictures do not move the source clock the audio track follows.
[[nodiscard]] inline std::int64_t RtxFgPlaybackPosition(
		std::int64_t sourcePosition,
		const RtxFgPlan &) {
	return sourcePosition;
}

[[nodiscard]] inline std::int64_t RtxFgPictureTime(
		std::int64_t previousPosition,
		std::int64_t nextPosition,
		float timestep) {
	const auto span = double(nextPosition - previousPosition);
	return previousPosition + std::int64_t(std::llround(span * timestep));
}

[[nodiscard]] inline RtxFgPlan PlanRtxFrameGeneration(const RtxFgClock &clock) {
	auto plan = RtxFgPlan();
	const auto refresh = RtxFgRefreshMs(clock.refreshMs);
	const auto stop = [&](RtxFgReason reason) {
		plan.reason = reason;
		return plan;
	};
	if (!clock.enabled) {
		return stop(RtxFgReason::Off);
	}
	if (!clock.supported) {
		return stop(RtxFgReason::Unsupported);
	}
	if (clock.blocked) {
		return stop(RtxFgReason::Blocked);
	}
	if (!RtxFgViewerQualifies(
			clock.fullscreen,
			clock.windowWidth,
			clock.windowHeight)) {
		return stop(RtxFgReason::SmallWindow);
	}
	if (!clock.playing) {
		return stop(RtxFgReason::NotPlaying);
	}
	if (!clock.pair) {
		plan.reason = RtxFgReason::Pair;
		if (clock.waitingSince > 0
			&& clock.now >= clock.waitingSince
			&& (clock.now - clock.waitingSince) < kRtxFgMaxGapMs) {
			plan.nextWake = clock.now + refresh;
		}
		return plan;
	}
	if (clock.alpha) {
		return stop(RtxFgReason::Alpha);
	}
	if (!RtxFgFrameSizeOk(clock.width, clock.height)
		|| clock.nextWidth != clock.width
		|| clock.nextHeight != clock.height) {
		return stop(RtxFgReason::Size);
	}
	if (clock.nextDisplay <= clock.previousDisplay
		|| clock.nextPosition <= clock.previousPosition) {
		return stop(RtxFgReason::Pair);
	}
	const auto gap = clock.nextDisplay - clock.previousDisplay;
	if (gap > kRtxFgMaxGapMs) {
		return stop(RtxFgReason::GapLong);
	}
	const auto minGap = std::int64_t(refresh) * 2;
	if (gap < minGap) {
		return stop(RtxFgReason::GapShort);
	}
	const auto inside = (clock.now > clock.previousDisplay)
		&& (clock.now < clock.nextDisplay);
	if (inside && (clock.now + refresh) < clock.nextDisplay) {
		plan.nextWake = clock.now + refresh;
	}
	if (!inside) {
		plan.reason = RtxFgReason::Endpoint;
		return plan;
	}
	const auto timestep = float(
		double(clock.now - clock.previousDisplay) / double(gap));
	const auto endpoint = (timestep * 1000.f) < float(kRtxFgEndpointMilli)
		|| (timestep * 1000.f) > float(1000 - kRtxFgEndpointMilli);
	if (endpoint) {
		plan.reason = RtxFgReason::Endpoint;
		return plan;
	}
	plan.decision = RtxFgDecision::Interpolate;
	plan.reason = RtxFgReason::None;
	plan.timestep = timestep;
	plan.pictureTime = RtxFgPictureTime(
		clock.previousPosition,
		clock.nextPosition,
		timestep);
	return plan;
}

} // namespace Media::Streaming

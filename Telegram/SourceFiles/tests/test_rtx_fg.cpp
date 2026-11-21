/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_rtx_fg_plan.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int Failed = 0;

void Check(bool ok, const char *name) {
	if (ok) {
		return;
	}
	std::fprintf(stderr, "FAIL %s\n", name);
	++Failed;
}

using namespace Media::Streaming;

[[nodiscard]] RtxFgClock PlayingClock() {
	auto clock = RtxFgClock();
	clock.enabled = true;
	clock.supported = true;
	clock.fullscreen = true;
	clock.playing = true;
	clock.pair = true;
	clock.windowWidth = 800;
	clock.windowHeight = 600;
	clock.width = 1280;
	clock.height = 720;
	clock.nextWidth = 1280;
	clock.nextHeight = 720;
	clock.previousDisplay = 10000;
	clock.nextDisplay = 10042;
	clock.previousPosition = 5000;
	clock.nextPosition = 5042;
	clock.now = 10021;
	clock.refreshMs = 8;
	return clock;
}

void TestPrefAndSession() {
	Check(
		std::string(kRtxVideoFrameGenerationPref)
			== "fishgram/rtx_video_frame_generation",
		"pref key");
	Check(!kRtxVideoFrameGenerationDefault, "default stays off");
	Check(!RtxFgPrefEnabled(false, false), "missing pref stays off");
	Check(!RtxFgPrefEnabled(true, false), "stored off stays off");
	Check(RtxFgPrefEnabled(true, true), "stored on stays on");

	auto session = ResetRtxFgSession();
	Check(!RtxFgBlocked(session), "fresh session runs");
	session = NoteRtxFgResult(session, RtxFgBackendResult::Failed);
	session = NoteRtxFgResult(session, RtxFgBackendResult::Failed);
	Check(!RtxFgBlocked(session), "two failures stay ready");
	session = NoteRtxFgResult(session, RtxFgBackendResult::Ok);
	Check(session.failures == 0, "success clears failures");
	session = NoteRtxFgResult(session, RtxFgBackendResult::Failed);
	session = NoteRtxFgResult(session, RtxFgBackendResult::Failed);
	session = NoteRtxFgResult(session, RtxFgBackendResult::Failed);
	Check(RtxFgBlocked(session), "third failure blocks");
	Check(
		session.state == RtxFgSessionState::Blocked,
		"budget ends blocked");
	session = NoteRtxFgResult(
		ResetRtxFgSession(),
		RtxFgBackendResult::Unsupported);
	Check(
		session.state == RtxFgSessionState::Unsupported,
		"unsupported sticks");
	Check(
		std::string(RtxFgReasonName(RtxFgReason::GapShort)) == "gap-short",
		"reason name");
}

void TestGates() {
	auto off = PlayingClock();
	off.enabled = RtxFgPrefEnabled(false, false);
	const auto offPlan = PlanRtxFrameGeneration(off);
	Check(offPlan.decision == RtxFgDecision::ShowSource, "default shows source");
	Check(offPlan.reason == RtxFgReason::Off, "default reason is off");
	Check(offPlan.nextWake == 0, "off does not wake");

	auto unsupported = PlayingClock();
	unsupported.supported = false;
	Check(
		PlanRtxFrameGeneration(unsupported).reason == RtxFgReason::Unsupported,
		"unsupported skips");

	auto blocked = PlayingClock();
	blocked.blocked = true;
	Check(
		PlanRtxFrameGeneration(blocked).reason == RtxFgReason::Blocked,
		"blocked skips");

	auto small = PlayingClock();
	small.fullscreen = false;
	small.windowWidth = 960;
	small.windowHeight = 540;
	Check(
		PlanRtxFrameGeneration(small).reason == RtxFgReason::SmallWindow,
		"small window skips");

	auto large = small;
	large.windowWidth = 1280;
	large.windowHeight = 720;
	Check(
		PlanRtxFrameGeneration(large).decision == RtxFgDecision::Interpolate,
		"1280 window interpolates");

	auto paused = PlayingClock();
	paused.playing = false;
	const auto pausedPlan = PlanRtxFrameGeneration(paused);
	Check(pausedPlan.reason == RtxFgReason::NotPlaying, "paused skips");
	Check(pausedPlan.nextWake == 0, "paused does not wake");

	auto alpha = PlayingClock();
	alpha.alpha = true;
	Check(
		PlanRtxFrameGeneration(alpha).reason == RtxFgReason::Alpha,
		"alpha skips");

	auto tiny = PlayingClock();
	tiny.width = 32;
	tiny.height = 32;
	tiny.nextWidth = 32;
	tiny.nextHeight = 32;
	Check(
		PlanRtxFrameGeneration(tiny).reason == RtxFgReason::Size,
		"tiny frame skips");

	auto mismatch = PlayingClock();
	mismatch.nextWidth = 1920;
	Check(
		PlanRtxFrameGeneration(mismatch).reason == RtxFgReason::Size,
		"size mismatch skips");

	auto fourK = PlayingClock();
	fourK.width = 3840;
	fourK.height = 2160;
	fourK.nextWidth = 3840;
	fourK.nextHeight = 2160;
	Check(
		PlanRtxFrameGeneration(fourK).decision == RtxFgDecision::Interpolate,
		"4K frame interpolates");

	auto tooWide = fourK;
	tooWide.width = 4096;
	tooWide.nextWidth = 4096;
	Check(
		PlanRtxFrameGeneration(tooWide).reason == RtxFgReason::Size,
		"over 4K long edge skips");
}

void TestTimingAndSync() {
	const auto mid = PlanRtxFrameGeneration(PlayingClock());
	Check(mid.decision == RtxFgDecision::Interpolate, "24fps midpoint interpolates");
	Check(mid.timestep > 0.49f && mid.timestep < 0.51f, "midpoint timestep");
	Check(mid.pictureTime == 5021, "picture time is halfway");
	Check(
		RtxFgPlaybackPosition(5000, mid) == 5000,
		"playback stays on the source frame");
	Check(mid.pictureTime != RtxFgPlaybackPosition(5000, mid), "picture is not the clock");
	Check(mid.nextWake == 10029, "wake one refresh later");
	Check(mid.nextWake < 10042, "wake stays before the source frame");

	auto sixty = PlayingClock();
	sixty.previousDisplay = 0;
	sixty.nextDisplay = 33;
	sixty.previousPosition = 0;
	sixty.nextPosition = 33;
	sixty.now = 16;
	sixty.refreshMs = 16;
	const auto half = PlanRtxFrameGeneration(sixty);
	Check(half.decision == RtxFgDecision::Interpolate, "30fps on 60Hz interpolates");
	Check(half.nextWake == 32, "60Hz wake before the next source frame");
	Check(half.nextWake < sixty.nextDisplay, "wake does not pass the source frame");

	auto smooth = PlayingClock();
	smooth.previousDisplay = 0;
	smooth.nextDisplay = 8;
	smooth.previousPosition = 0;
	smooth.nextPosition = 8;
	smooth.now = 4;
	smooth.refreshMs = 8;
	Check(
		PlanRtxFrameGeneration(smooth).reason == RtxFgReason::GapShort,
		"already at refresh skips");

	auto cut = PlayingClock();
	cut.nextDisplay = cut.previousDisplay + 1000;
	cut.nextPosition = cut.previousPosition + 1000;
	cut.now = cut.previousDisplay + 100;
	const auto cutPlan = PlanRtxFrameGeneration(cut);
	Check(cutPlan.reason == RtxFgReason::GapLong, "long gap skips");
	Check(cutPlan.nextWake == 0, "long gap does not poll");

	auto early = PlayingClock();
	early.now = 10002;
	const auto earlyPlan = PlanRtxFrameGeneration(early);
	Check(earlyPlan.decision == RtxFgDecision::ShowSource, "near the start shows source");
	Check(earlyPlan.reason == RtxFgReason::Endpoint, "near the start is an endpoint");
	Check(earlyPlan.nextWake == 10010, "endpoint still wakes for a later sample");

	auto late = PlayingClock();
	late.now = 10040;
	const auto latePlan = PlanRtxFrameGeneration(late);
	Check(latePlan.reason == RtxFgReason::Endpoint, "near the end shows source");
	Check(latePlan.nextWake == 0, "no wake once the source frame is due");

	auto outside = PlayingClock();
	outside.now = 10042;
	Check(
		PlanRtxFrameGeneration(outside).reason == RtxFgReason::Endpoint,
		"at the source time shows source");

	for (std::int64_t now = 10001; now < 10042; ++now) {
		auto clock = PlayingClock();
		clock.now = now;
		const auto plan = PlanRtxFrameGeneration(clock);
		Check(
			plan.nextWake == 0 || plan.nextWake < clock.nextDisplay,
			"sweep wake stays before the source frame");
		Check(
			RtxFgPlaybackPosition(clock.previousPosition, plan)
				== clock.previousPosition,
			"sweep clock stays put");
		if (plan.decision == RtxFgDecision::Interpolate) {
			Check(
				plan.pictureTime > clock.previousPosition
					&& plan.pictureTime < clock.nextPosition,
				"picture time stays between the source frames");
		}
	}

	auto waiting = PlayingClock();
	waiting.pair = false;
	waiting.waitingSince = 10000;
	waiting.now = 10008;
	const auto wait = PlanRtxFrameGeneration(waiting);
	Check(wait.reason == RtxFgReason::Pair, "missing pair waits");
	Check(wait.nextWake == 10016, "missing pair polls");
	waiting.now = 10000 + kRtxFgMaxGapMs;
	Check(
		PlanRtxFrameGeneration(waiting).nextWake == 0,
		"stalled pair stops polling");

	Check(RtxFgRefreshMs(0) == kRtxFgDefaultRefreshMs, "missing refresh");
	Check(RtxFgRefreshMs(1) == kRtxFgMinRefreshMs, "refresh floor");
	Check(RtxFgRefreshMs(200) == kRtxFgMaxRefreshMs, "refresh cap");
	Check(RtxFgViewerQualifies(true, 100, 100), "fullscreen qualifies");
	Check(!RtxFgViewerQualifies(false, 1279, 800), "1279 does not qualify");
}

} // namespace

int main() {
	TestPrefAndSession();
	TestGates();
	TestTimingAndSync();
	if (Failed) {
		std::fprintf(stderr, "%d checks failed\n", Failed);
		return 1;
	}
	std::printf("ok\n");
	return 0;
}

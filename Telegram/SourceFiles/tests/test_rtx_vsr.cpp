/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_rtx_vsr_plan.h"

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

void TestSharedToggle() {
	const auto missing = RtxBoolPrefEnabled(false, false);
	const auto storedOn = RtxBoolPrefEnabled(true, true);
	const auto storedOff = RtxBoolPrefEnabled(true, false);
	const auto onRow = RtxVsrToggleState(storedOn, true);
	const auto offRow = RtxVsrToggleState(storedOff, true);
	const auto gray = RtxVsrToggleState(storedOn, false);
	const auto defaultRow = RtxVsrToggleState(missing, true);

	Check(missing, "missing pref is the settings default");
	Check(onRow.checked && onRow.available, "settings on is a checked row");
	Check(!offRow.checked && offRow.available, "settings off is unchecked");
	Check(defaultRow.checked, "viewer row follows the default");
	Check(gray.checked && !gray.available, "unsupported stays checked and gray");
	Check(!RtxVsrStoredAfterToggle(storedOn, true), "menu off matches settings off");
	Check(RtxVsrStoredAfterToggle(storedOff, true), "menu on matches settings on");
	Check(
		RtxVsrStoredAfterToggle(missing, true) == !missing,
		"default toggle writes the opposite");
	Check(
		RtxVsrStoredAfterToggle(storedOn, false) == storedOn,
		"gray row does not write");
	Check(
		RtxVsrStoredAfterToggle(storedOff, false) == storedOff,
		"gray off stays off");
}

void TestPrefAndAdapter() {
	Check(
		std::string(kRtxVideoSuperResolutionPref)
			== "fishgram/rtx_video_super_resolution",
		"pref key");
	Check(kRtxVideoSuperResolutionDefault, "default follows the driver");
	Check(RtxBoolPrefEnabled(false, false), "missing pref stays on");
	Check(!RtxBoolPrefEnabled(true, false), "stored off stays off");
	Check(RtxBoolPrefEnabled(true, true), "stored on stays on");

	Check(RtxAdapterQualifies(kRtxNvidiaVendorId, false, false), "nvidia");
	Check(!RtxAdapterQualifies(kRtxNvidiaVendorId, true, false), "software");
	Check(!RtxAdapterQualifies(kRtxNvidiaVendorId, false, true), "remote");
	Check(!RtxAdapterQualifies(0x8086u, false, false), "not nvidia");
	Check(RtxAdapterQualifies(0x10DEu, false, false), "any nvidia vendor");
}

void TestPlan() {
	const auto source = RtxVsrSize{ 640, 360 };
	const auto follow = PlanRtxVideoSuperResolution(
		RtxBoolPrefEnabled(false, false),
		true,
		false,
		source,
		{ 1280, 720 });
	Check(follow.decision == RtxVsrDecision::Upscale, "default requests vsr");

	const auto off = PlanRtxVideoSuperResolution(
		RtxBoolPrefEnabled(true, false),
		true,
		false,
		source,
		{ 1280, 720 });
	Check(off.decision == RtxVsrDecision::Skip, "explicit off skips");

	const auto unsupported = PlanRtxVideoSuperResolution(
		true,
		false,
		false,
		source,
		{ 1280, 720 });
	Check(unsupported.decision == RtxVsrDecision::Skip, "unsupported skips");

	const auto blocked = PlanRtxVideoSuperResolution(
		true,
		true,
		true,
		source,
		{ 1280, 720 });
	Check(blocked.decision == RtxVsrDecision::Skip, "blocked skips");

	const auto same = PlanRtxVideoSuperResolution(
		true,
		true,
		false,
		source,
		source);
	Check(same.decision == RtxVsrDecision::Skip, "no upscale skips");

	const auto oneSide = PlanRtxVideoSuperResolution(
		true,
		true,
		false,
		source,
		{ 1280, 360 });
	Check(oneSide.decision == RtxVsrDecision::Skip, "one side skips");

	const auto odd = PlanRtxVideoSuperResolution(
		true,
		true,
		false,
		{ 641, 361 },
		{ 1280, 720 });
	Check(odd.decision == RtxVsrDecision::Upscale, "odd source is cropped");
	Check(odd.output == RtxVsrSize{ 1280, 720 }, "odd crop is 2x");

	const auto tiny = PlanRtxVideoSuperResolution(
		true,
		true,
		false,
		{ 32, 32 },
		{ 200, 200 });
	Check(tiny.decision == RtxVsrDecision::Skip, "tiny source skips");

	const auto doubled = PlanRtxVideoSuperResolution(
		true,
		true,
		false,
		source,
		{ 1280, 720 });
	Check(doubled.decision == RtxVsrDecision::Upscale, "2x upscales");
	Check(doubled.output == RtxVsrSize{ 1280, 720 }, "2x size");

	const auto capped = PlanRtxVideoSuperResolution(
		true,
		true,
		false,
		{ 100, 100 },
		{ 10000, 10000 });
	Check(capped.output == RtxVsrSize{ 400, 400 }, "scale cap");

	const auto edge = PlanRtxVideoSuperResolution(
		true,
		true,
		false,
		{ 1920, 1080 },
		{ 8000, 4500 });
	Check(edge.output == RtxVsrSize{ 3840, 2160 }, "long edge cap");

	const auto fullHd = PlanRtxVideoSuperResolution(
		true,
		true,
		false,
		{ 3840, 2160 },
		{ 7680, 4320 });
	Check(fullHd.decision == RtxVsrDecision::Skip, "4k source stays");

	const auto outputs = RtxVsrOutputs({ 1920, 1080 }, { 8000, 4500 });
	Check(outputs.count == 2, "fallback count");
	Check(outputs.sizes[0] == RtxVsrSize{ 3840, 2160 }, "primary 4k");
	Check(outputs.sizes[1] == RtxVsrSize{ 2560, 1440 }, "fallback 1440p");
}

void Test720pAndRejectReasons() {
	const auto hd = PlanRtxVideoSuperResolution(
		true,
		true,
		false,
		{ 1280, 720 },
		{ 3840, 2160 });
	Check(hd.decision == RtxVsrDecision::Upscale, "720p to 4k upscales");
	Check(hd.output == RtxVsrSize{ 3840, 2160 }, "720p to 4k is 3x");
	Check(hd.output.width == 1280 * 3, "720p width scale is 3");
	Check(hd.output.height == 720 * 3, "720p height scale is 3");

	const auto fullHd = PlanRtxVideoSuperResolution(
		true,
		true,
		false,
		{ 1920, 1080 },
		{ 3840, 2160 });
	Check(fullHd.decision == RtxVsrDecision::Upscale, "1080p to 4k upscales");
	Check(fullHd.output == RtxVsrSize{ 3840, 2160 }, "1080p to 4k is 2x");

	const auto oddHd = PlanRtxVideoSuperResolution(
		true,
		true,
		false,
		{ 1281, 721 },
		{ 3840, 2160 });
	Check(oddHd.decision == RtxVsrDecision::Upscale, "odd 720p still upscales");
	Check(oddHd.output == RtxVsrSize{ 3840, 2160 }, "odd 720p crops to 3x");

	Check(
		RtxVsrRejectReasonFor({ 1280, 720 }, { 3840, 2160 })
			== RtxVsrRejectReason::None,
		"720p reason none");
	Check(
		RtxVsrRejectReasonFor({ 1280, 720 }, {})
			== RtxVsrRejectReason::EmptyTarget,
		"empty target");
	Check(
		std::string(RtxVsrRejectReasonName(RtxVsrRejectReason::EmptyTarget))
			== "empty-target",
		"empty target name");
	Check(
		RtxVsrRejectReasonFor({ 1280, 720 }, { 1280, 720 })
			== RtxVsrRejectReason::TargetNotLarger,
		"same size");
	Check(
		RtxVsrRejectReasonFor({ 1280, 720 }, { 1920, 720 })
			== RtxVsrRejectReason::OneSide,
		"one side");
	Check(
		RtxVsrRejectReasonFor({ 32, 720 }, { 3840, 2160 })
			== RtxVsrRejectReason::TinySource,
		"tiny edge");
	Check(
		RtxVsrRejectReasonFor({ 3840, 2160 }, { 7680, 4320 })
			== RtxVsrRejectReason::AlreadyLarge,
		"4k source stays large");
	Check(
		RtxEvenSize({ 1281, 721 }) == RtxVsrSize{ 1280, 720 },
		"even crop");
	const auto collapsed = RtxVsrRejectReasonFor({ 100, 100 }, { 101, 200 });
	Check(collapsed == RtxVsrRejectReason::EvenFit, "even fit collapses");
}

void TestFallbackList() {
	const auto outputs = RtxVsrOutputs({ 1920, 1080 }, { 8000, 4500 });
	Check(outputs.count >= 2, "has a smaller fallback");
	Check(outputs.sizes[1] == RtxVsrSize{ 2560, 1440 }, "1440 fallback");
	auto sawSame = false;
	for (auto i = 0; i != outputs.count; ++i) {
		if (outputs.sizes[i] == RtxVsrSize{ 1920, 1080 }) {
			sawSame = true;
		}
	}
	Check(!sawSame, "1920 fallback is not an upscale");
}

void TestSession() {
	auto session = RtxVsrSession();
	session = NoteRtxVsrResult(session, RtxVsrBackendResult::Failed);
	session = NoteRtxVsrResult(session, RtxVsrBackendResult::Failed);
	Check(session.state == RtxVsrSessionState::Ready, "two failures stay");
	Check(session.failures == 2, "failure count");
	session = NoteRtxVsrResult(session, RtxVsrBackendResult::Ok);
	Check(session.failures == 0, "success resets count");
	session = NoteRtxVsrResult(session, RtxVsrBackendResult::Failed);
	session = NoteRtxVsrResult(session, RtxVsrBackendResult::Failed);
	session = NoteRtxVsrResult(session, RtxVsrBackendResult::Failed);
	Check(session.state == RtxVsrSessionState::Blocked, "budget blocks");
	Check(RtxVsrBlocked(session), "blocked query");
	session = NoteRtxVsrResult(session, RtxVsrBackendResult::Ok);
	Check(session.state == RtxVsrSessionState::Blocked, "ok does not unblock");
	session = ResetRtxVsrSession();
	Check(session.state == RtxVsrSessionState::Ready, "reset opens");
	session = NoteRtxVsrResult(session, RtxVsrBackendResult::Unsupported);
	Check(
		session.state == RtxVsrSessionState::Unsupported,
		"driver unsupported");
	session = NoteRtxVsrResult(session, RtxVsrBackendResult::Failed);
	Check(
		session.state == RtxVsrSessionState::Unsupported,
		"unsupported sticks");
	session = ResetRtxVsrSession();
	Check(!RtxVsrBlocked(session), "reset clears block");
}

void TestPackAndBlank() {
	const unsigned char y[] = { 1, 2, 3, 4 };
	const unsigned char u[] = { 5 };
	const unsigned char v[] = { 6 };
	unsigned char dest[8] = {};
	Check(PackNv12(y, 2, u, 1, v, 1, 2, 2, dest, 2), "pack nv12");
	const unsigned char expected[] = { 1, 2, 3, 4, 5, 6 };
	Check(std::memcmp(dest, expected, 6) == 0, "nv12 bytes");
	Check(!PackNv12(y, 2, u, 1, v, 1, 3, 2, dest, 2), "odd pack rejected");

	unsigned char black[16] = {};
	unsigned char white[16];
	std::memset(white, 200, sizeof(white));
	Check(RtxSampleMaxByte(black, 2, 2, 8, 4, 3) == 0, "black sample");
	Check(RtxSampleMaxByte(white, 2, 2, 8, 4, 3) == 200, "white sample");
	Check(RtxOutputRejectedAsBlank(200, 0), "blank output rejected");
	Check(!RtxOutputRejectedAsBlank(0, 0), "black source kept");
	Check(!RtxOutputRejectedAsBlank(200, 1), "visible output kept");
}

} // namespace

int main() {
	TestSharedToggle();
	TestPrefAndAdapter();
	TestPlan();
	Test720pAndRejectReasons();
	TestFallbackList();
	TestSession();
	TestPackAndBlank();
	if (Failed) {
		std::fprintf(stderr, "%d checks failed\n", Failed);
		return 1;
	}
	std::printf("rtx vsr plan tests passed\n");
	return 0;
}

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
	Check(odd.decision == RtxVsrDecision::Skip, "odd source skips");

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

void TestHdrPlan() {
	Check(
		std::string(kRtxVideoHdrPref) == "fishgram/rtx_video_hdr",
		"hdr pref key");
	Check(kRtxVideoHdrDefault, "hdr default follows the driver");
	Check(
		RtxBoolPrefEnabled(false, false, kRtxVideoHdrDefault),
		"missing hdr pref stays on");
	Check(
		!RtxBoolPrefEnabled(true, false, kRtxVideoHdrDefault),
		"stored hdr off stays off");
	Check(
		RtxBoolPrefEnabled(true, true, kRtxVideoHdrDefault),
		"stored hdr on stays on");

	const auto apply = PlanRtxVideoHdr(
		RtxBoolPrefEnabled(false, false, kRtxVideoHdrDefault),
		true,
		false,
		true,
		false,
		false,
		false);
	Check(apply.decision == RtxHdrDecision::ToneMap, "sdr on hdr display");
	Check(
		RtxHdrSkipReasonFor(true, true, false, true, false, false, false)
			== RtxHdrSkipReason::None,
		"hdr reason none");

	const auto sameSize = PlanRtxVideoHdr(
		true, true, false, true, false, false, false);
	Check(
		sameSize.decision == RtxHdrDecision::ToneMap,
		"hdr does not need an upscale");

	const auto off = PlanRtxVideoHdr(
		RtxBoolPrefEnabled(true, false, kRtxVideoHdrDefault),
		true,
		false,
		true,
		false,
		false,
		false);
	Check(off.decision == RtxHdrDecision::Skip, "hdr off skips");
	Check(
		RtxHdrSkipReasonFor(false, true, false, true, false, false, false)
			== RtxHdrSkipReason::Disabled,
		"disabled wins");

	Check(
		PlanRtxVideoHdr(true, false, false, true, false, false, false)
			.decision == RtxHdrDecision::Skip,
		"hdr unsupported skips");
	Check(
		RtxHdrSkipReasonFor(true, false, false, true, false, false, false)
			== RtxHdrSkipReason::Unsupported,
		"unsupported reason");

	Check(
		PlanRtxVideoHdr(true, true, true, true, false, false, false)
			.decision == RtxHdrDecision::Skip,
		"hdr blocked skips");
	Check(
		RtxHdrSkipReasonFor(true, true, true, true, false, false, false)
			== RtxHdrSkipReason::Blocked,
		"blocked reason");

	Check(
		PlanRtxVideoHdr(true, true, false, false, false, false, false)
			.decision == RtxHdrDecision::Skip,
		"sdr display skips");
	Check(
		RtxHdrSkipReasonFor(true, true, false, false, false, false, false)
			== RtxHdrSkipReason::DisplaySdr,
		"display-sdr reason");

	Check(
		PlanRtxVideoHdr(true, true, false, true, true, false, false)
			.decision == RtxHdrDecision::Skip,
		"hdr source skips");
	Check(
		RtxHdrSkipReasonFor(true, true, false, true, true, false, false)
			== RtxHdrSkipReason::SourceHdr,
		"source-hdr reason");

	Check(
		PlanRtxVideoHdr(true, true, false, true, false, true, false)
			.decision == RtxHdrDecision::Skip,
		"rotated skips");
	Check(
		RtxHdrSkipReasonFor(true, true, false, true, false, true, false)
			== RtxHdrSkipReason::Rotated,
		"rotated reason");

	Check(
		PlanRtxVideoHdr(true, true, false, true, false, false, true)
			.decision == RtxHdrDecision::Skip,
		"alpha skips hdr");
	Check(
		RtxHdrSkipReasonFor(true, true, false, true, false, false, true)
			== RtxHdrSkipReason::Alpha,
		"alpha reason");

	Check(
		std::string(RtxHdrSkipReasonName(RtxHdrSkipReason::DisplaySdr))
			== "display-sdr",
		"display-sdr name");
	Check(
		std::string(RtxHdrSkipReasonName(RtxHdrSkipReason::SourceHdr))
			== "source-hdr",
		"source-hdr name");
	Check(
		std::string(RtxHdrSkipReasonName(RtxHdrSkipReason::None)) == "none",
		"none name");

	const auto vsr = PlanRtxVideoSuperResolution(
		true,
		true,
		false,
		{ 1280, 720 },
		{ 3840, 2160 });
	const auto hdr = PlanRtxVideoHdr(
		true, true, false, true, false, false, false);
	Check(vsr.decision == RtxVsrDecision::Upscale, "vsr still upscales");
	Check(hdr.decision == RtxHdrDecision::ToneMap, "hdr runs with vsr");
	Check(vsr.output == RtxVsrSize{ 3840, 2160 }, "vsr size unchanged");
}

} // namespace

int main() {
	TestPrefAndAdapter();
	TestPlan();
	TestFallbackList();
	TestSession();
	TestPackAndBlank();
	TestHdrPlan();
	if (Failed) {
		std::fprintf(stderr, "%d checks failed\n", Failed);
		return 1;
	}
	std::printf("rtx vsr plan tests passed\n");
	return 0;
}

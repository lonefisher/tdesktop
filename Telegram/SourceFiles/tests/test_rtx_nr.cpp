/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_rtx_nr_plan.h"

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

RtxNrRequest Ready() {
	return {
		.enabled = true,
		.rtx = true,
		.surface = RtxNrSurface::MediaViewer,
		.fullscreen = true,
		.windowLongEdge = 1920,
		.runtimeFile = true,
		.trust = RtxNrTrust::Ok,
		.versionOk = true,
		.width = 1280,
		.height = 720,
	};
}

void TestIdentity() {
	Check(std::string(kRtxDlssNrPref) == "fishgram/rtx_dlss_nr", "pref");
	Check(!kRtxDlssNrDefault, "default off");
	Check(std::string(kRtxDlssNrDllName) == "nvngx_dlssnr.dll", "dll");
	Check(std::string(kRtxDlssNrFolderName) == "ngx", "folder");
	Check(
		std::string(kRtxDlssNrEnvName) == "FISHGRAM_NGX_RUNTIME",
		"env");
	Check(kRtxDlssNrFeatureId == 18u, "feature 18");
	Check(kRtxDlssNrAppId == 0x0876232Cull, "app id");
	Check(RtxNrVersionOk(310, 8, 0, 0), "310.8.0.0");
	Check(!RtxNrVersionOk(310, 8, 3, 0), "lecram version");
	Check(!RtxNrVersionOk(310, 8, 2, 0), "sf version");
	Check(!RtxNrVersionOk(310, 9, 0, 0), "newer version");
	Check(
		std::string(kRtxDlssNrSha256)
			== "E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E",
		"published digest");
}

void TestSignatureAndPath() {
	Check(
		JudgeRtxNrSignature(true, "NVIDIA Corporation") == RtxNrTrust::Ok,
		"nvidia signer");
	Check(
		JudgeRtxNrSignature(true, "nvidia corporation") == RtxNrTrust::Ok,
		"signer case");
	Check(
		JudgeRtxNrSignature(true, "Contoso") == RtxNrTrust::NotNvidia,
		"other signer");
	Check(
		JudgeRtxNrSignature(false, "NVIDIA Corporation")
			== RtxNrTrust::Untrusted,
		"broken signature");
	Check(
		JudgeRtxNrSignature(false, "") == RtxNrTrust::Missing,
		"unsigned");
	Check(!RtxNrSignerIsNvidia("NVI DIA"), "split name");

	Check(
		ChooseRtxNrRuntime({
			.environmentSet = true,
			.environmentHasDll = true,
			.exeFolderHasDll = true,
		}) == RtxNrPathKind::Environment,
		"env wins");
	Check(
		ChooseRtxNrRuntime({
			.environmentSet = true,
			.environmentHasDll = false,
			.exeFolderHasDll = true,
		}) == RtxNrPathKind::None,
		"env does not fall through");
	Check(
		ChooseRtxNrRuntime({
			.environmentSet = false,
			.environmentHasDll = false,
			.exeFolderHasDll = true,
		}) == RtxNrPathKind::BesideExe,
		"ngx folder");
	Check(
		ChooseRtxNrRuntime({}) == RtxNrPathKind::None,
		"nothing found");
}

void TestGate() {
	Check(PlanRtxDlssNr(Ready()) == RtxNrSkipReason::None, "run");

	auto off = Ready();
	off.enabled = false;
	Check(PlanRtxDlssNr(off) == RtxNrSkipReason::Disabled, "off");

	auto pip = Ready();
	pip.surface = RtxNrSurface::PictureInPicture;
	Check(PlanRtxDlssNr(pip) == RtxNrSkipReason::Surface, "pip");

	auto chat = Ready();
	chat.surface = RtxNrSurface::Inline;
	Check(PlanRtxDlssNr(chat) == RtxNrSkipReason::Surface, "chat");

	auto gtx = Ready();
	gtx.rtx = false;
	Check(PlanRtxDlssNr(gtx) == RtxNrSkipReason::Device, "not rtx");

	auto small = Ready();
	small.fullscreen = false;
	small.windowLongEdge = 1279;
	Check(PlanRtxDlssNr(small) == RtxNrSkipReason::Window, "1279");

	auto wide = Ready();
	wide.fullscreen = false;
	wide.windowLongEdge = 1280;
	Check(PlanRtxDlssNr(wide) == RtxNrSkipReason::None, "1280");

	auto full = Ready();
	full.fullscreen = true;
	full.windowLongEdge = 640;
	Check(PlanRtxDlssNr(full) == RtxNrSkipReason::None, "fullscreen");

	auto missing = Ready();
	missing.runtimeFile = false;
	Check(
		PlanRtxDlssNr(missing) == RtxNrSkipReason::RuntimeMissing,
		"missing dll");

	auto bad = Ready();
	bad.trust = RtxNrTrust::Untrusted;
	Check(PlanRtxDlssNr(bad) == RtxNrSkipReason::Signature, "signature");

	auto other = Ready();
	other.trust = RtxNrTrust::NotNvidia;
	Check(PlanRtxDlssNr(other) == RtxNrSkipReason::Signature, "signer");

	auto version = Ready();
	version.versionOk = false;
	Check(PlanRtxDlssNr(version) == RtxNrSkipReason::Version, "version");

	auto blocked = Ready();
	blocked.blocked = true;
	Check(PlanRtxDlssNr(blocked) == RtxNrSkipReason::Blocked, "blocked");

	auto odd = Ready();
	odd.width = 1281;
	Check(PlanRtxDlssNr(odd) == RtxNrSkipReason::Size, "odd");

	auto tiny = Ready();
	tiny.width = 32;
	tiny.height = 32;
	Check(PlanRtxDlssNr(tiny) == RtxNrSkipReason::Size, "tiny");

	auto huge = Ready();
	huge.width = 7680;
	huge.height = 4320;
	Check(PlanRtxDlssNr(huge) == RtxNrSkipReason::Size, "huge");

	auto alpha = Ready();
	alpha.alpha = true;
	Check(PlanRtxDlssNr(alpha) == RtxNrSkipReason::Size, "alpha");
}

void TestCreateBudgetAndHdrPlace() {
	auto budget = RtxNrCreateBudget();
	budget = NoteRtxNrCreateFailure(budget);
	budget = NoteRtxNrCreateFailure(budget);
	Check(budget.failures == 2 && !budget.blocked, "two creates stay open");
	budget = NoteRtxNrCreateFailure(budget);
	Check(budget.failures == 3 && budget.blocked, "third create blocks");

	auto early = Ready();
	early.width = kRtxDlssNrMinEdge;
	early.height = kRtxDlssNrMinEdge;
	early.alpha = false;
	Check(PlanRtxDlssNr(early) == RtxNrSkipReason::None, "early size passes");
	auto odd = Ready();
	odd.width = 1281;
	Check(PlanRtxDlssNr(odd) == RtxNrSkipReason::Size, "real size after convert");
	auto alpha = early;
	alpha.alpha = true;
	Check(PlanRtxDlssNr(alpha) == RtxNrSkipReason::Size, "alpha skips convert");
	auto off = early;
	off.enabled = false;
	Check(PlanRtxDlssNr(off) == RtxNrSkipReason::Disabled, "off skips convert");

	Check(
		PlaceRtxNrRelativeToHdr(true, true) == RtxNrHdrPlace::BeforeHdrToneMap,
		"nr before hdr tone map");
	Check(
		PlaceRtxNrRelativeToHdr(false, true) == RtxNrHdrPlace::AfterPaint,
		"nr after paint without overlay");
	Check(
		PlaceRtxNrRelativeToHdr(true, false) == RtxNrHdrPlace::AfterPaint,
		"hdr overlay without nr");
	Check(
		PlaceRtxNrRelativeToHdr(false, false) == RtxNrHdrPlace::AfterPaint,
		"neither");
	Check(VideoEnhanceMenuShows(true), "playing video shows the switches");
	Check(!VideoEnhanceMenuShows(false), "photo hides the switches");
	Check(kHdrOverlayHitTransparent == -1, "overlay hit test is transparent");
	Check(EnhancementYieldsToInput(true), "pending input skips enhancement");
	Check(!EnhancementYieldsToInput(false), "idle frame may enhance");
	Check(NextEnhancementWakeMs(0, 40) == 0, "no wake stays unscheduled");
	Check(NextEnhancementWakeMs(1, 40) == 16, "slow frame waits");
	Check(NextEnhancementWakeMs(30, 40) == 30, "later wake is kept");
	Check(NextEnhancementWakeMs(1, 4) == 1, "fast frame can wake soon");
}

void TestHistory() {
	auto clock = RtxNrClock();
	Check(RtxNrTakeReset(clock), "first frame resets");
	Check(!RtxNrTakeReset(clock), "later frame keeps history");
	RtxNrBumpEpoch(clock);
	Check(RtxNrTakeReset(clock), "seek resets once");
	Check(!RtxNrTakeReset(clock), "playback continues");
	RtxNrBumpEpoch(clock);
	Check(RtxNrTakeReset(clock), "next clip resets");
}

} // namespace

int main() {
	TestIdentity();
	TestSignatureAndPath();
	TestGate();
	TestCreateBudgetAndHdrPlace();
	TestHistory();
	if (Failed) {
		std::fprintf(stderr, "rtx dlss nr plan tests failed: %d\n", Failed);
		return 1;
	}
	std::printf("rtx dlss nr plan tests passed\n");
	return 0;
}

/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <cstdint>

namespace Media::Streaming {

inline constexpr auto kRtxDlssNrPref = "fishgram/rtx_dlss_nr";
inline constexpr auto kRtxDlssNrDefault = false;
inline constexpr auto kRtxDlssNrDllName = "nvngx_dlssnr.dll";
inline constexpr auto kRtxDlssNrFolderName = "ngx";
inline constexpr auto kRtxDlssNrEnvName = "FISHGRAM_NGX_RUNTIME";

// NVIDIA-signed snippet 310.8.0.0, Feature 18. Same-resolution only.
inline constexpr auto kRtxDlssNrFeatureId = 18u;
inline constexpr auto kRtxDlssNrAppId = 0x0876232Cull;
inline constexpr auto kRtxDlssNrApiVersion = 0x16;
inline constexpr int kRtxDlssNrVersion[4] = { 310, 8, 0, 0 };
inline constexpr auto kRtxDlssNrSha256 =
	"E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E";

inline constexpr auto kRtxDlssNrMinWindowEdge = 1280;
inline constexpr auto kRtxDlssNrMinEdge = 64;
inline constexpr auto kRtxDlssNrMaxLongEdge = 3840;
inline constexpr auto kRtxDlssNrFailureBudget = 3;

enum class RtxNrSurface {
	MediaViewer,
	PictureInPicture,
	Inline,
};

enum class RtxNrPathKind {
	None,
	Environment,
	BesideExe,
};

enum class RtxNrTrust {
	Missing,
	Untrusted,
	NotNvidia,
	Ok,
};

enum class RtxNrSkipReason {
	None,
	Disabled,
	Device,
	Surface,
	Window,
	RuntimeMissing,
	Signature,
	Version,
	Size,
	Create,
	Blocked,
};

struct RtxNrPathRequest {
	bool environmentSet = false;
	bool environmentHasDll = false;
	bool exeFolderHasDll = false;

};

struct RtxNrRequest {
	bool enabled = false;
	bool rtx = false;
	RtxNrSurface surface = RtxNrSurface::MediaViewer;
	bool fullscreen = false;
	int windowLongEdge = 0;
	bool runtimeFile = false;
	RtxNrTrust trust = RtxNrTrust::Missing;
	bool versionOk = false;
	int width = 0;
	int height = 0;
	bool alpha = false;
	bool blocked = false;

};

struct RtxNrClock {
	std::uint64_t epoch = 1;
	std::uint64_t applied = 0;

};

[[nodiscard]] inline bool RtxNrVersionOk(
		int major,
		int minor,
		int patch,
		int build) {
	return major == kRtxDlssNrVersion[0]
		&& minor == kRtxDlssNrVersion[1]
		&& patch == kRtxDlssNrVersion[2]
		&& build == kRtxDlssNrVersion[3];
}

[[nodiscard]] inline bool RtxNrSignerIsNvidia(const char *signer) {
	if (!signer || !signer[0]) {
		return false;
	}
	const char needle[] = "nvidia";
	for (auto i = 0; signer[i]; ++i) {
		auto matched = true;
		for (auto j = 0; needle[j]; ++j) {
			const auto ch = signer[i + j];
			if (!ch) {
				matched = false;
				break;
			}
			const auto lower = (ch >= 'A' && ch <= 'Z')
				? char(ch - 'A' + 'a')
				: ch;
			if (lower != needle[j]) {
				matched = false;
				break;
			}
		}
		if (matched) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] inline RtxNrTrust JudgeRtxNrSignature(
		bool verifyOk,
		const char *signer) {
	if (!verifyOk) {
		return (signer && signer[0])
			? RtxNrTrust::Untrusted
			: RtxNrTrust::Missing;
	}
	return RtxNrSignerIsNvidia(signer)
		? RtxNrTrust::Ok
		: RtxNrTrust::NotNvidia;
}

[[nodiscard]] inline RtxNrPathKind ChooseRtxNrRuntime(
		RtxNrPathRequest request) {
	if (request.environmentSet) {
		return request.environmentHasDll
			? RtxNrPathKind::Environment
			: RtxNrPathKind::None;
	}
	return request.exeFolderHasDll
		? RtxNrPathKind::BesideExe
		: RtxNrPathKind::None;
}

[[nodiscard]] inline bool RtxNrSizeOk(int width, int height) {
	if (width < kRtxDlssNrMinEdge
		|| height < kRtxDlssNrMinEdge
		|| (width % 2)
		|| (height % 2)) {
		return false;
	}
	const auto longEdge = (width > height) ? width : height;
	return longEdge <= kRtxDlssNrMaxLongEdge;
}

[[nodiscard]] inline bool RtxNrViewerAllows(
		bool fullscreen,
		int windowLongEdge) {
	return fullscreen || windowLongEdge >= kRtxDlssNrMinWindowEdge;
}

[[nodiscard]] inline RtxNrSkipReason PlanRtxDlssNr(
		const RtxNrRequest &request) {
	if (!request.enabled) {
		return RtxNrSkipReason::Disabled;
	}
	if (request.surface != RtxNrSurface::MediaViewer) {
		return RtxNrSkipReason::Surface;
	}
	if (!request.rtx) {
		return RtxNrSkipReason::Device;
	}
	if (!RtxNrViewerAllows(request.fullscreen, request.windowLongEdge)) {
		return RtxNrSkipReason::Window;
	}
	if (!request.runtimeFile) {
		return RtxNrSkipReason::RuntimeMissing;
	}
	if (request.trust != RtxNrTrust::Ok) {
		return RtxNrSkipReason::Signature;
	}
	if (!request.versionOk) {
		return RtxNrSkipReason::Version;
	}
	if (request.blocked) {
		return RtxNrSkipReason::Blocked;
	}
	if (request.alpha || !RtxNrSizeOk(request.width, request.height)) {
		return RtxNrSkipReason::Size;
	}
	return RtxNrSkipReason::None;
}

struct RtxNrCreateBudget {
	int failures = 0;
	bool blocked = false;
};

[[nodiscard]] inline RtxNrCreateBudget NoteRtxNrCreateFailure(
		RtxNrCreateBudget budget) {
	++budget.failures;
	if (budget.failures >= kRtxDlssNrFailureBudget) {
		budget.blocked = true;
	}
	return budget;
}

enum class RtxNrHdrPlace {
	AfterPaint,
	BeforeHdrToneMap,
};

// HDR overlay tone-maps the SDR picture it is given. NR has to clean
// that picture first, or the overlay shows a frame NR never saw.
[[nodiscard]] inline RtxNrHdrPlace PlaceRtxNrRelativeToHdr(
		bool hdrOverlay,
		bool nrEnabled) {
	return (hdrOverlay && nrEnabled)
		? RtxNrHdrPlace::BeforeHdrToneMap
		: RtxNrHdrPlace::AfterPaint;
}

[[nodiscard]] inline bool VideoEnhanceMenuShows(bool playbackVisible) {
	return playbackVisible;
}

inline constexpr int kHdrOverlayHitTransparent = -1;

[[nodiscard]] inline bool EnhancementYieldsToInput(bool inputPending) {
	return inputPending;
}

[[nodiscard]] inline int NextEnhancementWakeMs(int wakeInMs, int lastPaintMs) {
	if (wakeInMs <= 0) {
		return 0;
	}
	if (lastPaintMs >= 16 && wakeInMs < 16) {
		return 16;
	}
	return wakeInMs;
}

inline void RtxNrBumpEpoch(RtxNrClock &clock) {
	++clock.epoch;
}

[[nodiscard]] inline bool RtxNrTakeReset(RtxNrClock &clock) {
	if (clock.applied == clock.epoch) {
		return false;
	}
	clock.applied = clock.epoch;
	return true;
}

} // namespace Media::Streaming

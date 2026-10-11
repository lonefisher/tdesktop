/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Core {

// Safe, user-facing update failure categories. Keep values stable while they
// are exposed through UpdateChecker and shared between worker/main threads.
enum class UpdateFailure {
	None,
	NotConfigured,
	Network,
	Timeout,
	Manifest,
	Index,
	Signature,
	Download,
	DownloadValidation,
	Staging,
};

enum class UpdateRequestStage {
	Manifest,
	ManifestSignature,
	Index,
};

[[nodiscard]] constexpr UpdateFailure FailureForRequestError(
		bool timedOut) {
	return timedOut ? UpdateFailure::Timeout : UpdateFailure::Network;
}

[[nodiscard]] constexpr UpdateFailure FailureForInvalidResponse(
		UpdateRequestStage stage) {
	switch (stage) {
	case UpdateRequestStage::Manifest:
		return UpdateFailure::Manifest;
	case UpdateRequestStage::ManifestSignature:
		return UpdateFailure::Signature;
	case UpdateRequestStage::Index:
		return UpdateFailure::Index;
	}
	return UpdateFailure::Manifest;
}

} // namespace Core

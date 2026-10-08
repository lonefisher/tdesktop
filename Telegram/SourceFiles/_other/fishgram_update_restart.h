/* FishGram update restart policy. See Telegram Desktop's LEGAL for license. */
#pragma once

#include <Windows.h>
#include <optional>

namespace Core::FishGramUpdates {

[[nodiscard]] inline std::optional<bool> IsCurrentProcessElevated() {
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return std::nullopt;
	TOKEN_ELEVATION elevation = {};
	DWORD length = 0;
	const auto okay = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &length);
	CloseHandle(token);
	return okay ? std::optional<bool>(elevation.TokenIsElevated != 0) : std::nullopt;
}

template <typename NormalUserLaunch, typename InheritedTokenLaunch>
[[nodiscard]] bool RestartUpdatedClient(
		bool writeProtected,
		std::optional<bool> elevated,
		NormalUserLaunch normalUserLaunch,
		InheritedTokenLaunch inheritedTokenLaunch) {
	if (!elevated) return false;
	// The caller may omit the command-line flag when running recovery manually.
	// An elevated token must never be inherited by the client, even on failure.
	return writeProtected || *elevated
		? normalUserLaunch() : inheritedTokenLaunch();
}

} // namespace Core::FishGramUpdates

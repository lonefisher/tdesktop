/* FishGram update restart policy. See Telegram Desktop's LEGAL for license. */
#pragma once

#include <Windows.h>
#include <shellapi.h>
#include <optional>
#include <string>

namespace Core::FishGramUpdates {

[[nodiscard]] inline DWORD LaunchUpdateProcess(
		HWND owner,
		const std::wstring &operation,
		const std::wstring &executable,
		const std::wstring &arguments,
		const std::wstring &directory,
		int show = SW_SHOWNORMAL,
		HANDLE *process = nullptr) {
	auto info = SHELLEXECUTEINFOW{};
	info.cbSize = sizeof(info);
	info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI
		| (process ? SEE_MASK_NOCLOSEPROCESS : 0);
	info.hwnd = owner;
	info.lpVerb = operation.empty() ? nullptr : operation.c_str();
	info.lpFile = executable.c_str();
	info.lpParameters = arguments.c_str();
	info.lpDirectory = directory.empty() ? nullptr : directory.c_str();
	info.nShow = show;
	if (!ShellExecuteExW(&info)) return GetLastError();
	if (process) *process = info.hProcess;
	return ERROR_SUCCESS;
}

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

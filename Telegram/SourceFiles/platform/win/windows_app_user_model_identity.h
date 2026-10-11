/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <string>
#include <string_view>

// Separate FishGram's notification COM class from Telegram's registration.
#define FISHGRAM_TOAST_ACTIVATOR_UUID "B6CB0D96-E271-4D11-8356-447586ED944B"

namespace Platform::WindowsAppIdentity {

inline constexpr auto kProductName = std::wstring_view(L"FishGram");

[[nodiscard]] inline std::wstring AppUserModelIdBase() {
	return std::wstring(kProductName) + L".Desktop";
}

[[nodiscard]] inline std::wstring PortableAppUserModelId(
		std::wstring_view instanceHash) {
	return AppUserModelIdBase() + L'.' + std::wstring(instanceHash);
}

[[nodiscard]] inline std::wstring ShortcutFileName(bool alpha = false) {
	return std::wstring(kProductName)
		+ (alpha ? L"Alpha" : L"")
		+ L".lnk";
}

[[nodiscard]] inline std::wstring InstalledShortcutRelativePath() {
	return std::wstring(kProductName)
		+ L" Desktop\\"
		+ ShortcutFileName();
}

[[nodiscard]] inline std::wstring ShortcutPath(
		std::wstring_view root,
		std::wstring_view relative = ShortcutFileName()) {
	if (root.empty()) {
		return std::wstring(relative);
	}
	const auto separator = (root.back() == L'\\' || root.back() == L'/')
		? L""
		: L"\\";
	return std::wstring(root) + separator + std::wstring(relative);
}

} // namespace Platform::WindowsAppIdentity

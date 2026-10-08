/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "../../SourceFiles/platform/win/windows_app_user_model_identity.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

int main() {
	using namespace Platform::WindowsAppIdentity;
	static_assert(std::string_view(FISHGRAM_TOAST_ACTIVATOR_UUID)
		!= "F11932D3-6110-4BBC-9B02-B2EC07A1BD19");

	assert(AppUserModelIdBase() == L"FishGram.Desktop");
	assert(PortableAppUserModelId(L"a1b2c3")
		== L"FishGram.Desktop.a1b2c3");
	assert(PortableAppUserModelId(L"a1b2c3")
		!= PortableAppUserModelId(L"d4e5f6"));
	assert(PortableAppUserModelId(L"a1b2c3")
		!= L"Telegram.TelegramDesktop.a1b2c3");
	assert(ShortcutFileName() == L"FishGram.lnk");
	assert(ShortcutFileName(true) == L"FishGramAlpha.lnk");
	assert(InstalledShortcutRelativePath()
		== L"FishGram Desktop\\FishGram.lnk");
	assert(ShortcutPath(L"C:\\Start Menu\\")
		== L"C:\\Start Menu\\FishGram.lnk");

	const auto root = std::filesystem::temp_directory_path()
		/ ("fishgram-identity-"
			+ std::to_string(std::chrono::steady_clock::now()
				.time_since_epoch().count()));
	std::filesystem::create_directories(root);
	const auto cleanup = [&] {
		std::filesystem::remove_all(root);
	};
	const auto official = root / "Telegram.lnk";
	{
		std::ofstream file(official, std::ios::binary);
		file << "official shortcut";
	}
	const auto fishgram = std::filesystem::path(
		ShortcutPath(root.wstring()));
	assert(fishgram == root / "FishGram.lnk");
	{
		std::ofstream file(fishgram, std::ios::binary);
		file << "FishGram shortcut";
	}
	assert(std::filesystem::exists(official));
	assert(std::filesystem::exists(fishgram));
	{
		std::ifstream file(official, std::ios::binary);
		std::string value;
		std::getline(file, value);
		assert(value == "official shortcut");
	}
	cleanup();
}

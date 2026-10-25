#include "_other/fishgram_update_restart.h"
#include "core/fishgram_update_launch.h"

#include <cassert>
#include <iostream>
#include <optional>
#include <vector>

#pragma comment(lib, "shell32.lib")

using namespace Core::FishGramUpdates;

void TestLaunchFailureKeepsClientAndCanRetry() {
	wchar_t executable[32768] = {};
	assert(GetModuleFileNameW(nullptr, executable, DWORD(std::size(executable))));
	UpdateLaunch launch;
	std::vector<int> order;
	auto restarting = true;
	auto quitting = false;
	auto error = DWORD(ERROR_SUCCESS);
	assert(!launch.request([&] {
		order.push_back(1);
		error = LaunchUpdateProcess(nullptr, {}, std::wstring(executable) + L".missing.exe", {}, {}, SW_HIDE);
		return error == ERROR_SUCCESS;
	}, [&] { quitting = true; }, [&] {
		restarting = false;
		order.push_back(2);
	}));
	assert(error == ERROR_FILE_NOT_FOUND);
	assert(!quitting && !restarting && !launch.started());
	assert((order == std::vector<int>{1, 2}));

	HANDLE child = nullptr;
	const auto eventName = L"Local\\FishGram-update-launch-test-" + std::to_wstring(GetCurrentProcessId());
	const auto mayExit = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
	assert(mayExit != nullptr);
	restarting = true;
	assert(launch.request([&] {
		order.push_back(3);
		return LaunchUpdateProcess(nullptr, {}, executable,
			L"--launch-child " + eventName, {}, SW_HIDE, &child) == ERROR_SUCCESS;
	}, [&] {
		assert(child != nullptr);
		assert(WaitForSingleObject(child, 0) == WAIT_TIMEOUT);
		quitting = true;
		order.push_back(4);
		assert(SetEvent(mayExit));
	}, [] { assert(false); }));
	assert(quitting && restarting && launch.started());
	assert((order == std::vector<int>{1, 2, 3, 4}));
	assert(launch.request([] { assert(false); return false; },
		[] { assert(false); }, [] { assert(false); }));
	assert(WaitForSingleObject(child, 30000) == WAIT_OBJECT_0);
	DWORD exitCode = 1;
	assert(GetExitCodeProcess(child, &exitCode) && exitCode == 0);
	CloseHandle(child);
	CloseHandle(mayExit);
}

void TestLaunchDoesNotReenter() {
	UpdateLaunch launch;
	auto quits = 0;
	assert(launch.request([&] {
		assert(!launch.request([] { assert(false); return false; },
			[] { assert(false); }, [] { assert(false); }));
		return true;
	}, [&] { ++quits; }, [] { assert(false); }));
	assert(quits == 1);
}

int main(int argc, char *argv[]) {
	if (argc == 3 && std::string(argv[1]) == "--launch-child") {
		const auto mayExit = OpenEventA(SYNCHRONIZE, FALSE, argv[2]);
		if (!mayExit) return 2;
		const auto exited = WaitForSingleObject(mayExit, 15000) == WAIT_OBJECT_0;
		CloseHandle(mayExit);
		return exited ? 0 : 3;
	}
	TestLaunchFailureKeepsClientAndCanRetry();
	TestLaunchDoesNotReenter();
	for (const auto explicitProtection : {false, true}) {
		for (const auto elevated : {false, true}) {
			for (const auto normalLaunchWorks : {false, true}) {
				auto normalCalls = 0;
				auto inheritedCalls = 0;
				const auto result = RestartUpdatedClient(explicitProtection, elevated,
					[&] { ++normalCalls; return normalLaunchWorks; },
					[&] { ++inheritedCalls; return true; });
				const auto mustDrop = explicitProtection || elevated;
				assert(normalCalls == (mustDrop ? 1 : 0));
				assert(inheritedCalls == (mustDrop ? 0 : 1));
				assert(result == (!mustDrop || normalLaunchWorks));
			}
		}
	}
	auto calls = 0;
	assert(!RestartUpdatedClient(false, std::nullopt,
		[&] { ++calls; return true; }, [&] { ++calls; return true; }));
	assert(calls == 0);
	assert(IsCurrentProcessElevated().has_value());
	std::cout << "Update launch failure/retry, native process ordering and restart privilege tests passed.\n";
}

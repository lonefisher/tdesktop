#include "_other/fishgram_update_restart.h"

#include <cassert>
#include <iostream>
#include <optional>

using namespace Core::FishGramUpdates;

int main() {
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
	std::cout << "Updated-client restart privilege tests passed.\n";
}

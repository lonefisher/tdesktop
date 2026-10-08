#include "core/fishgram_update_policy.h"
#include <cassert>
#include <limits>
#include <string>

int main() {
    using namespace Core::FishGramUpdates;
    assert(ParseVersion("30073400000000001").value() == 30073400000000001ULL);
    assert(ParseVersion("18446744073709551615").value() == std::numeric_limits<std::uint64_t>::max());
    for (const auto value : {"", "0", "01", "-1", "+1", "1.0", "1e3", " 1", "18446744073709551616"}) {
        assert(!ParseVersion(value));
    }
    assert(IsSafePayloadName("Telegram.exe"));
    assert(IsSafePayloadName("Updater.exe"));
    assert(IsSafePayloadName("safe-name.dll"));
    for (const auto value : {"../Telegram.exe", "..\\Telegram.exe", "C:\\Telegram.exe", "/Telegram.exe", "tdata/key_data", "FishGramData/key_data", "Telegram.exe:secret", "Telegram.exe ", "Telegram.exe.", "CON.dll", "aux.dll", "LPT1.dll", "other.exe", "ready", "build-manifest.json", "TelegramForcePortable", "a/b.dll"}) {
        assert(!IsSafePayloadName(value));
    }
    assert(!IsSafePayloadName(std::string("safe.dll\0bad", 12)));
    assert(IsNewer(7002009, 9, MakeVersion(7002009, 8)));
    assert(IsNewer(7003000, 1, MakeVersion(7002009, 9)));
    assert(!IsNewer(7002009, 8, MakeVersion(7002009, 8)));
    assert(!IsNewer(7002009, 0, MakeVersion(7002009, 8)));
    assert(!IsNewer(7002008, 10, MakeVersion(7002009, 8)));
}

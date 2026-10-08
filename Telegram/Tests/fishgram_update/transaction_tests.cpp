#include "_other/fishgram_update_transaction.h"

#include <Windows.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace Core::FishGramUpdates::WindowsTransaction;
namespace fs = std::filesystem;

constexpr std::uint32_t kReadyMagic = 0x7FFFFFFD;
constexpr std::uint32_t kStable = 0;
constexpr std::uint32_t kBeta = 1;

void Write(const fs::path &path, const std::string &value);

struct Fixture final {
	explicit Fixture(const std::wstring &label) {
		static std::atomic_uint64_t sequence = 0;
		wchar_t temp[MAX_PATH] = {};
		assert(GetTempPathW(MAX_PATH, temp) != 0);
		root = fs::path(temp) / (L"fishgram-update-" + label
			+ std::to_wstring(GetCurrentProcessId()) + L"-"
			+ std::to_wstring(++sequence));
		install = root / L"install";
		work = root / L"FishGramData";
		fs::create_directories(install);
		fs::create_directories(work / L"tupdates" / L"temp");
		Write(install / L"Telegram.exe", "old-telegram");
		Write(install / L"Updater.exe", "old-updater");
		Write(install / L"old.dll", "old-library");
		Write(install / L"FishGramData" / L"tdata" / L"key_data", "account-secret");
		Write(work / L"tdata" / L"map0", "working-account-secret");
	}

	~Fixture() {
		std::error_code error;
		fs::remove_all(root, error);
	}

	void Payload(
		std::uint64_t version,
		std::uint32_t channel = kStable,
		const std::map<std::wstring, std::string> &files = {
			{ L"Telegram.exe", "new-telegram" },
			{ L"Updater.exe", "new-updater" },
			{ L"zlib.dll", "new-library" } }) {
		const auto dir = work / L"tupdates" / L"temp";
		for (const auto &entry : fs::directory_iterator(dir)) {
			std::error_code error;
			fs::remove_all(entry.path(), error);
		}
		for (const auto &[name, contents] : files) Write(dir / name, contents);
		std::ofstream ready(dir / L"ready", std::ios::binary | std::ios::trunc);
		const std::uint32_t magic = kReadyMagic;
		ready.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
		ready.write(reinterpret_cast<const char*>(&version), sizeof(version));
		ready.write(reinterpret_cast<const char*>(&channel), sizeof(channel));
		ready.close();
		assert(ready.good());
	}

	Request RequestFor(std::uint64_t version, std::uint32_t channel = kStable) const {
		Request request;
		request.installDir = install.wstring();
		request.workDir = work.wstring();
		request.executableName = L"Telegram.exe";
		request.runningVersion = version;
		request.signedChannel = channel;
		return request;
	}

	fs::path root;
	fs::path install;
	fs::path work;
};

void Write(const fs::path &path, const std::string &value) {
	fs::create_directories(path.parent_path());
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file.write(value.data(), std::streamsize(value.size()));
	assert(file.good());
}

std::string Read(const fs::path &path) {
	std::ifstream file(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(file), {});
}

void TestApplyAndProtectAccountData() {
	Fixture fixture(L"apply-");
	fixture.Payload(7002009001ULL);
	const auto result = Apply(fixture.RequestFor(7002009001ULL));
	assert(result == Result::Applied);
	assert(Read(fixture.install / L"Telegram.exe") == "new-telegram");
	assert(Read(fixture.install / L"Updater.exe") == "new-updater");
	assert(Read(fixture.install / L"zlib.dll") == "new-library");
	assert(Read(fixture.install / L"old.dll") == "old-library");
	assert(Read(fixture.install / L"FishGramData" / L"tdata" / L"key_data") == "account-secret");
	assert(Read(fixture.work / L"tdata" / L"map0") == "working-account-secret");
	assert(Read(fixture.work / L"tupdates" / L"temp" / L"Updater.exe") == "new-updater");
}

void TestJournalSurvivesDurableCommit() {
	Fixture fixture(L"journal-");
	const auto path = (fixture.root / L"journal.bin").wstring();
	Details::Journal journal;
	journal.id = L"0123456789abcdef-00000001-0123456789abcdef";
	journal.version = 7002009001ULL;
	journal.channel = kStable;
	journal.payloadNames = { L"Telegram.exe", L"Updater.exe" };
	journal.originalNames = { L"Telegram.exe", L"old.dll" };
	assert(Details::WriteJournal(path, journal));
	Details::Journal read;
	const auto serialized = Details::Serialize(journal);
	assert(Details::ParseJournal(serialized, &read));
	read = {};
	assert(Details::ReadJournal(path, &read));
	assert(read.version == journal.version);
	assert(read.payloadNames == journal.payloadNames);
	assert(Details::MarkCommitted(path));
	assert(Details::ReadJournal(path, &read));
	assert(read.state == 2);
}

void TestPortableWorkingDirectoryInsideInstallation() {
	Fixture fixture(L"portable-");
	fixture.work = fixture.install / L"FishGramData";
	fs::create_directories(fixture.work / L"tupdates" / L"temp");
	Write(fixture.work / L"tdata" / L"map0", "working-account-secret");
	fixture.Payload(7002009001ULL);
	assert(Apply(fixture.RequestFor(7002009001ULL)) == Result::Applied);
	assert(Read(fixture.work / L"tdata" / L"map0") == "working-account-secret");
	assert(Read(fixture.install / L"Telegram.exe") == "new-telegram");
}

void TestRejectsLegacyAndMismatchedReadyData() {
	Fixture fixture(L"marker-");
	fixture.Payload(7002009001ULL);
	Write(fixture.work / L"tupdates" / L"temp" / L"ready", "1");
	assert(Apply(fixture.RequestFor(7002009001ULL)) == Result::InvalidReadyMarker);
	assert(Read(fixture.install / L"Telegram.exe") == "old-telegram");

	fixture.Payload(7002009000ULL);
	assert(Apply(fixture.RequestFor(7002009001ULL)) == Result::VersionMismatch);

	fixture.Payload(7002009001ULL, kBeta);
	assert(Apply(fixture.RequestFor(7002009001ULL, kStable)) == Result::ChannelMismatch);
}

void TestRejectsNestedPayloadAndLegacyVersionFile() {
	Fixture fixture(L"nested-");
	fixture.Payload(7002009001ULL);
	Write(fixture.work / L"tupdates" / L"temp" / L"nested" / L"Telegram.exe", "escape");
	assert(Apply(fixture.RequestFor(7002009001ULL)) == Result::InvalidPayload);
	assert(Read(fixture.install / L"Telegram.exe") == "old-telegram");

	fixture.Payload(7002009001ULL);
	Write(fixture.work / L"tupdates" / L"temp" / L"tdata" / L"version", "legacy");
	assert(Apply(fixture.RequestFor(7002009001ULL)) == Result::InvalidPayload);
}

void TestSpaceAndPreflightFailuresLeaveProgramUntouched() {
	Fixture fixture(L"preflight-");
	fixture.Payload(7002009001ULL);
	auto request = fixture.RequestFor(7002009001ULL);
	request.spaceAvailable = [](const std::wstring&, std::uint64_t) { return false; };
	assert(Apply(request) == Result::InsufficientSpace);
	assert(Read(fixture.install / L"Telegram.exe") == "old-telegram");

	request = fixture.RequestFor(7002009001ULL);
	request.writeAccess = [](const std::wstring&) { return false; };
	assert(Apply(request) == Result::WriteDenied);
	assert(Read(fixture.install / L"Telegram.exe") == "old-telegram");

	request = fixture.RequestFor(7002009001ULL);
	request.processRunning = [](const std::wstring&) { return true; };
	assert(Apply(request) == Result::AppRunning);
	assert(Read(fixture.install / L"Telegram.exe") == "old-telegram");
}

void TestCopyFailureRollsBackAllFiles() {
	Fixture fixture(L"copy-fail-");
	fixture.Payload(7002009001ULL);
	auto request = fixture.RequestFor(7002009001ULL);
	request.copyFile = [](const std::wstring &from, const std::wstring &to) {
		if (fs::path(from).filename() == L"zlib.dll"
			&& fs::path(from).parent_path().filename() == L"temp") {
			SetLastError(ERROR_DISK_FULL);
			return false;
		}
		return CopyFileW(from.c_str(), to.c_str(), FALSE) != FALSE;
	};
	assert(Apply(request) == Result::CopyFailed);
	assert(Read(fixture.install / L"Telegram.exe") == "old-telegram");
	assert(Read(fixture.install / L"Updater.exe") == "old-updater");
	assert(!fs::exists(fixture.install / L"zlib.dll"));
	assert(Read(fixture.install / L"old.dll") == "old-library");
	assert(Read(fixture.install / L"FishGramData" / L"tdata" / L"key_data") == "account-secret");

	fixture.Payload(7002009001ULL);
	assert(Apply(fixture.RequestFor(7002009001ULL)) == Result::Applied);
}

std::wstring Quote(const std::wstring &value) {
	std::wstring result = L"\"";
	for (const auto ch : value) {
		if (ch == L'"') result += L'\\';
		result += ch;
	}
	return result + L'"';
}

void TestAbruptInterruptionRecoversFromDurableJournal() {
	Fixture fixture(L"crash-");
	fixture.Payload(7002009001ULL);
	wchar_t executable[MAX_PATH] = {};
	assert(GetModuleFileNameW(nullptr, executable, MAX_PATH) != 0);
	std::wstring command = Quote(executable) + L" --crash-child "
		+ Quote(fixture.install.wstring()) + L" "
		+ Quote(fixture.work.wstring()) + L" 7002009001";
	STARTUPINFOW startup = { sizeof(startup) };
	PROCESS_INFORMATION process = {};
	assert(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process));
	CloseHandle(process.hThread);
	assert(WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0);
	DWORD exitCode = 0;
	assert(GetExitCodeProcess(process.hProcess, &exitCode));
	CloseHandle(process.hProcess);
	assert(exitCode == 73);
	assert(Read(fixture.install / L"Telegram.exe") == "new-telegram");
	const auto recovery = InspectStartupRecovery(fixture.install.wstring());
	assert(recovery.state == StartupRecovery::State::Required);
	assert(Read(recovery.updaterPath) == "old-updater");
	assert(fs::path(recovery.updaterPath).parent_path().filename() == L"recovery");
	assert(RecoverPending(fixture.install.wstring()) == Result::Recovered);
	assert(Read(fixture.install / L"Telegram.exe") == "old-telegram");
	assert(Read(fixture.install / L"Updater.exe") == "old-updater");
	assert(Read(fixture.install / L"old.dll") == "old-library");
	assert(Read(fixture.install / L"FishGramData" / L"tdata" / L"key_data") == "account-secret");
	assert(InspectStartupRecovery(fixture.install.wstring()).state == StartupRecovery::State::None);
}

void TestStartupRecoveryBeforeAnyReadyCleanup() {
	Fixture fixture(L"startup-");
	assert(InspectStartupRecovery(fixture.install.wstring()).state == StartupRecovery::State::None);
	assert(!fs::exists(fixture.install / L".fishgram-update"));
	fixture.Payload(7002009001ULL);
	auto request = fixture.RequestFor(7002009001ULL);
	request.afterCommit = [] { throw 42; };
	try { (void)Apply(request); assert(false); } catch (int code) { assert(code == 42); }
	// Committed interruption must keep the new program, even when its version
	// equals ready. Recovery does not depend on the work directory surviving.
	fs::remove_all(fixture.work / L"tupdates");
	const auto recovery = InspectStartupRecovery(fixture.install.wstring());
	assert(recovery.state == StartupRecovery::State::Required);
	assert(RecoverPending(fixture.install.wstring()) == Result::Recovered);
	assert(Read(fixture.install / L"Telegram.exe") == "new-telegram");
	assert(Read(fixture.install / L"Updater.exe") == "new-updater");
	assert(!fs::exists(fixture.install / L".fishgram-update" / L"pending"));
}

void TestStartupRecoveryBeforeJournalCreation() {
	Fixture fixture(L"pre-journal-");
	const auto meta = fixture.install / L".fishgram-update";
	fs::create_directories(meta / L"pending" / L"backup");
	fs::create_directories(meta / L"versions");
	Write(meta / L"recovery" / L"Updater.exe", "old-updater");
	assert(InspectStartupRecovery(fixture.install.wstring()).state == StartupRecovery::State::Required);
	assert(RecoverPending(fixture.install.wstring()) == Result::Recovered);
	assert(Read(fixture.install / L"Telegram.exe") == "old-telegram");
	assert(!fs::exists(meta / L"pending"));
	fs::create_directories(meta / L"pending");
	Write(meta / L"pending" / L"journal.bin", "corrupt");
	assert(InspectStartupRecovery(fixture.install.wstring()).state == StartupRecovery::State::Blocked);
}

void TestTransactionWritesOnlyAuthenticatedBytes() {
	Fixture fixture(L"authenticated-");
	fixture.Payload(7002009001ULL);
	auto request = fixture.RequestFor(7002009001ULL);
	for (const auto &name : {L"Telegram.exe", L"Updater.exe", L"zlib.dll"}) {
		const std::string content = "authenticated-program";
		request.authenticatedFiles.push_back({name, {content.begin(), content.end()}});
	}
	// A same-size staging change after verification cannot become installed code.
	Write(fixture.work / L"tupdates" / L"temp" / L"Telegram.exe", "tampered-program");
	assert(Apply(request) == Result::Applied);
	assert(Read(fixture.install / L"Telegram.exe") == "authenticated-program");
	assert(Read(fixture.install / L"Updater.exe") == "authenticated-program");
	assert(Read(fixture.work / L"tdata" / L"map0") == "working-account-secret");
	fixture.Payload(7002009002ULL);
	request = fixture.RequestFor(7002009002ULL);
	request.authenticatedFiles.push_back({L"Telegram.exe", {'x'}});
	assert(Apply(request) == Result::InvalidPayload);
	assert(Read(fixture.install / L"Telegram.exe") == "authenticated-program");
}

void TestPendingRecoveryDoesNotReplaceRunningProgram() {
	Fixture fixture(L"live-pending-");
	fixture.Payload(7002009001ULL);
	auto interrupted = fixture.RequestFor(7002009001ULL);
	interrupted.afterReplace = [](std::size_t) { throw 42; };
	try { (void)Apply(interrupted); assert(false); } catch (int code) { assert(code == 42); }
	assert(Read(fixture.install / L"Telegram.exe") == "new-telegram");
	auto busy = fixture.RequestFor(7002009001ULL);
	busy.processRunning = [](const std::wstring&) { return true; };
	assert(Apply(busy) == Result::AppRunning);
	assert(Read(fixture.install / L"Telegram.exe") == "new-telegram");
	assert(RecoverPending(fixture.install.wstring()) == Result::Recovered);
	assert(Read(fixture.install / L"Telegram.exe") == "old-telegram");
}

void TestTrustedUpdateRunnerAndAuthorizationGate() {
	Fixture fixture(L"runner-");
	std::wstring runner;
	assert(PrepareUpdateRunner(fixture.install.wstring(), &runner));
	assert(Read(runner) == "old-updater");
	assert(fs::path(runner).parent_path().filename() == L"runner");
	fixture.Payload(7002009001ULL);
	auto request = fixture.RequestFor(7002009001ULL);
	request.authorize = [] { return false; };
	assert(Apply(request) == Result::InvalidPayload);
	assert(Read(fixture.install / L"Telegram.exe") == "old-telegram");
}

void TestInstallLockSerializesSameDirectory() {
	Fixture fixture(L"lock-");
	fixture.Payload(7002009001ULL);
	std::mutex mutex;
	std::condition_variable changed;
	bool acquired = false;
	bool release = false;
	auto first = fixture.RequestFor(7002009001ULL);
	first.onLockAcquired = [&] {
		std::unique_lock lock(mutex);
		acquired = true;
		changed.notify_all();
		changed.wait(lock, [&] { return release; });
	};
	Result firstResult = Result::IoError;
	std::thread worker([&] { firstResult = Apply(first); });
	{
		std::unique_lock lock(mutex);
		assert(changed.wait_for(lock, std::chrono::seconds(10), [&] { return acquired; }));
	}
	assert(Apply(fixture.RequestFor(7002009001ULL)) == Result::Busy);
	{
		std::lock_guard lock(mutex);
		release = true;
	}
	changed.notify_all();
	worker.join();
	assert(firstResult == Result::Applied);
}

void TestRetainsCurrentAndTwoPriorVersions() {
	Fixture fixture(L"retention-");
	for (std::uint64_t version = 7002009001ULL; version <= 7002009004ULL; ++version) {
		fixture.Payload(version, kStable, {
			{ L"Telegram.exe", "version-" + std::to_string(version) },
			{ L"Updater.exe", "updater-" + std::to_string(version) }});
		assert(Apply(fixture.RequestFor(version)) == Result::Applied);
	}
	std::size_t versions = 0;
	const auto folder = fixture.install / L".fishgram-update" / L"versions";
	for (const auto &entry : fs::directory_iterator(folder)) {
		assert(entry.is_directory());
		++versions;
	}
	assert(versions == 2);
	assert(Read(fixture.install / L"Telegram.exe") == "version-7002009004");
}

int CrashChild(int argc, wchar_t **argv) {
	assert(argc == 5);
	Request request;
	request.installDir = argv[2];
	request.workDir = argv[3];
	request.executableName = L"Telegram.exe";
	request.runningVersion = _wcstoui64(argv[4], nullptr, 10);
	request.signedChannel = kStable;
	request.afterReplace = [](std::size_t count) {
		if (count == 1) TerminateProcess(GetCurrentProcess(), 73);
	};
	const auto result = Apply(request);
	return result == Result::Applied ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
	if (argc > 1 && std::wstring(argv[1]) == L"--crash-child") return CrashChild(argc, argv);
	TestJournalSurvivesDurableCommit();
	TestPortableWorkingDirectoryInsideInstallation();
	TestApplyAndProtectAccountData();
	TestRejectsLegacyAndMismatchedReadyData();
	TestRejectsNestedPayloadAndLegacyVersionFile();
	TestSpaceAndPreflightFailuresLeaveProgramUntouched();
	TestCopyFailureRollsBackAllFiles();
	TestAbruptInterruptionRecoversFromDurableJournal();
	TestStartupRecoveryBeforeAnyReadyCleanup();
	TestStartupRecoveryBeforeJournalCreation();
	TestTransactionWritesOnlyAuthenticatedBytes();
	TestPendingRecoveryDoesNotReplaceRunningProgram();
	TestTrustedUpdateRunnerAndAuthorizationGate();
	TestInstallLockSerializesSameDirectory();
	TestRetainsCurrentAndTwoPriorVersions();
	std::cout << "Windows update transaction tests passed.\n";
	return 0;
}

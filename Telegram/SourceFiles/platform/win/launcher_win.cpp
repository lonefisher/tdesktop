/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "platform/win/launcher_win.h"

#include "core/crash_reports.h"
#include "core/update_checker.h"
#include "core/update_channel.h"
#include "_other/fishgram_update_transaction.h"
#include "_other/fishgram_update_restart.h"

#include <windows.h>
#include <shellapi.h>
#include <VersionHelpers.h>

#include <crtdbg.h>
#include <cstdlib>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

namespace Platform {
namespace {

HWND UpdateOwnerWindow() {
	if (const auto owner = GetActiveWindow()) return owner;
	if (const auto window = QApplication::activeWindow()) {
		return reinterpret_cast<HWND>(window->winId());
	}
	return nullptr;
}

QString UpdateRunnerError(Core::FishGramUpdates::WindowsTransaction::Result error) {
	using Core::FishGramUpdates::WindowsTransaction::Result;
	switch (error) {
	case Result::InvalidInstallPath:
		return u"The FishGram installation folder is invalid."_q;
	case Result::WriteDenied:
		return u"The installation or update files failed permission and safety checks. Use a private writable installation folder."_q;
	case Result::Busy:
		return u"Another updater is using this installation. Close it and retry."_q;
	case Result::RecoveryFailed:
		return u"An interrupted update needs recovery before another update can start."_q;
	case Result::FileInUse:
		return u"The installed updater is in use. Close the other updater and retry."_q;
	case Result::CopyFailed:
		return u"The update program could not be copied. Check free space and file access, then retry."_q;
	default:
		return u"The installed update program could not be prepared. Check the installation files and retry."_q;
	}
}

// Turn a CRT/STL assertion (or pure-virtual / invalid-parameter) into a real,
// crash-reported failure instead of a modal Abort/Retry/Ignore dialog that
// would block a headless test agent forever.
[[noreturn]] void TestAgentReportCrash(
		const char *kind,
		const QString &message) {
	if (!message.isEmpty()) {
		CrashReports::SetAnnotation("CrtAssert", message);
	}
	// Be loud on stderr too (captured when the launch redirects it to a file).
	fprintf(stderr,
		"\n[testagent] %s: %s\n",
		kind,
		message.toLocal8Bit().constData());
	fflush(stderr);

	// Crashes with an access violation -> caught by the crash reporter, dumped
	// to tdata/working + tdata/dumps, and the process exits non-zero. No dialog.
	Unexpected("Test agent: CRT/STL assertion violation.");
}

#ifdef _DEBUG
int TestAgentReportHook(int reportType, char *message, int *returnValue) {
	if (returnValue) {
		*returnValue = 0; // Do not break into the (absent) debugger.
	}
	if (reportType == _CRT_ASSERT || reportType == _CRT_ERROR) {
		TestAgentReportCrash(
			"assert",
			QString::fromLocal8Bit(message ? message : ""));
	} else if (message) {
		fputs(message, stderr);
		fflush(stderr);
	}
	return TRUE; // Handled -> suppress the default message box.
}

int TestAgentReportHookW(int reportType, wchar_t *message, int *returnValue) {
	if (returnValue) {
		*returnValue = 0;
	}
	if (reportType == _CRT_ASSERT || reportType == _CRT_ERROR) {
		TestAgentReportCrash(
			"assert",
			QString::fromWCharArray(message ? message : L""));
	} else if (message) {
		fputws(message, stderr);
		fflush(stderr);
	}
	return TRUE;
}
#endif // _DEBUG

void InstallTestAgentCrashHandling() {
	// No Windows Error Reporting / general-protection / abort() message boxes —
	// each would block the test agent waiting on a button it cannot press.
	SetErrorMode(SEM_FAILCRITICALERRORS
		| SEM_NOGPFAULTERRORBOX
		| SEM_NOOPENFILEERRORBOX);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

	// Pure-virtual call / invalid CRT parameter -> reported crash, not a dialog.
	_set_purecall_handler([] {
		TestAgentReportCrash("purecall", u"Pure virtual function call"_q);
	});
	_set_invalid_parameter_handler([](
			const wchar_t *expression,
			const wchar_t *,
			const wchar_t *,
			unsigned int,
			uintptr_t) {
		TestAgentReportCrash(
			"invalid-parameter",
			QString::fromWCharArray(
				expression ? expression : L"CRT invalid parameter"));
	});

#ifdef _DEBUG
	// assert()/_ASSERTE and STL bounds/iterator checks (_STL_VERIFY) report via
	// _CrtDbgReport[W]; route them to stderr (never a window) and convert any
	// assertion into a reported crash through the hooks above.
	for (const auto type : { _CRT_WARN, _CRT_ERROR, _CRT_ASSERT }) {
		_CrtSetReportMode(type, _CRTDBG_MODE_FILE);
		_CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
	}
	_CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, TestAgentReportHook);
	_CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL, TestAgentReportHookW);
#endif // _DEBUG
}

} // namespace

Launcher::Launcher(int argc, char *argv[])
: Core::Launcher(argc, argv) {
}

void Launcher::initHook() {
	if (cTestAgent()) {
		InstallTestAgentCrashHandling();
	}
}

bool Launcher::acquireClientSessionHook() {
	const auto result = Core::FishGramClientGate::TryAcquire(
		QDir::toNativeSeparators(cExeDir()).toStdWString(),
		false,
		&_clientSessionLease);
	if (result != Core::FishGramClientGate::AcquireResult::Acquired) {
		MessageBoxW(nullptr,
			L"FishGram cannot safely open account data because this installation "
			L"is updating, already in use by an updater, or its private session lock is unavailable.",
			L"FishGram startup blocked", MB_OK | MB_ICONERROR);
		return false;
	}
	return true;
}

std::optional<int> Launcher::recoverUpdateHook() {
	namespace Transaction = Core::FishGramUpdates::WindowsTransaction;
	const auto recovery = Transaction::InspectStartupRecovery(
		QDir::toNativeSeparators(cExeDir()).toStdWString());
	if (recovery.state == Transaction::StartupRecovery::State::None) {
		return std::nullopt;
	}
	if (recovery.state == Transaction::StartupRecovery::State::Blocked) {
		MessageBoxW(nullptr,
			L"FishGram cannot recover the interrupted update safely. "
			L"Close other FishGram instances and use the program recovery backup before opening account data.",
			L"FishGram update recovery", MB_OK | MB_ICONERROR);
		return 1;
	}
	auto arguments = QStringList{
		u"-recover"_q,
		u"-installpath"_q, '"' + cExeDir() + '"',
		u"-workdir"_q, '"' + cWorkingDir() + '"',
		u"-waitpid"_q, QString::number(qulonglong(GetCurrentProcessId())),
	};
	if (recovery.requiresElevation) {
		arguments.push_back(u"-writeprotected"_q);
		arguments.push_back('"' + cExeDir() + '"');
	}
	return launch(recovery.requiresElevation ? u"runas"_q : QString(),
		QString::fromStdWString(recovery.updaterPath), arguments) ? 0 : 1;
}

std::optional<QStringList> Launcher::readArgumentsHook(
		int argc,
		char *argv[]) const {
	auto count = 0;
	if (const auto list = CommandLineToArgvW(GetCommandLine(), &count)) {
		const auto guard = gsl::finally([&] { LocalFree(list); });
		if (count > 0) {
			auto result = QStringList();
			result.reserve(count);
			for (auto i = 0; i != count; ++i) {
				result.push_back(QString::fromWCharArray(list[i]));
			}
			return result;
		}
	}
	return std::nullopt;
}

bool Launcher::launchUpdater(UpdaterLaunch action) {
	_updateLaunchError = u"The FishGram executable name is unavailable."_q;
	if (cExeName().isEmpty()) {
		return false;
	}

	const auto operation = (action == UpdaterLaunch::JustRelaunch)
		? QString()
		: (cWriteProtected()
			? u"runas"_q
			: QString());
	auto binaryPath = cExeDir() + cExeName();
	if (action != UpdaterLaunch::JustRelaunch) {
		if (!Core::checkReadyUpdate()) {
			_updateLaunchError = u"The downloaded update is no longer ready. Check for updates and retry."_q;
			return false;
		}
		std::wstring runner;
		auto failure = Core::FishGramUpdates::WindowsTransaction::Result::IoError;
		if (!Core::FishGramUpdates::WindowsTransaction::PrepareUpdateRunner(
			QDir::toNativeSeparators(cExeDir()).toStdWString(), &runner, &failure)) {
			_updateLaunchError = UpdateRunnerError(failure);
			return false;
		}
		// Never execute an unauthenticated binary from the work directory.
		binaryPath = QString::fromStdWString(runner);
	}

	auto argumentsList = QStringList();
	const auto pushArgument = [&](const QString &argument) {
		argumentsList.push_back(argument.trimmed());
	};
	if (cLaunchMode() == LaunchModeAutoStart) {
		pushArgument(u"-autostart"_q);
	}
	if (Logs::DebugEnabled()) {
		pushArgument(u"-debug"_q);
	}
	if (cStartInTray()) {
		pushArgument(u"-startintray"_q);
	}
	if (customWorkingDir() || action == UpdaterLaunch::PerformUpdate) {
		pushArgument(u"-workdir"_q);
		pushArgument('"' + cWorkingDir() + '"');
	}
	if (cDataFile() != u"data"_q) {
		pushArgument(u"-key"_q);
		pushArgument('"' + cDataFile() + '"');
	}

	if (action == UpdaterLaunch::JustRelaunch) {
		pushArgument(u"-noupdate"_q);
		if (cRestartingToSettings()) {
			pushArgument(u"-tosettings"_q);
		}
	} else {
		pushArgument(u"-update"_q);
		pushArgument(u"-waitpid"_q);
		pushArgument(QString::number(qulonglong(GetCurrentProcessId())));
		pushArgument(u"-fromversion"_q);
		pushArgument(QString::number(qulonglong(Core::RunningUpdateVersion())));
		if (cInstallBetaVersion()) pushArgument(u"-beta"_q);
		pushArgument(u"-exename"_q);
		pushArgument('"' + cExeName() + '"');
		pushArgument(u"-installpath"_q);
		pushArgument('"' + cExeDir() + '"');
		if (cWriteProtected()) {
			pushArgument(u"-writeprotected"_q);
			pushArgument('"' + cExeDir() + '"');
		}
	}
	return launch(operation, binaryPath, argumentsList,
		action == UpdaterLaunch::PerformUpdate);
}

void Launcher::showUpdateLaunchError() {
	LOG(("Update Error: %1").arg(_updateLaunchError));
	const auto message = (_updateLaunchError
		+ u"\n\nFishGram will continue running the current version."_q).toStdWString();
	MessageBoxW(UpdateOwnerWindow(), message.c_str(),
		L"FishGram update", MB_OK | MB_ICONWARNING);
}

bool Launcher::launch(
		const QString &operation,
		const QString &binaryPath,
		const QStringList &argumentsList,
		bool keepRunning) {
	const auto convertPath = [](const QString &path) {
		return QDir::toNativeSeparators(path).toStdWString();
	};
	const auto nativeBinaryPath = convertPath(binaryPath);
	const auto nativeWorkingDir = convertPath(cWorkingDir());
	const auto arguments = argumentsList.join(' ');

	DEBUG_LOG(("Application Info: executing %1 %2"
		).arg(binaryPath
		).arg(arguments
		));

	if (!keepRunning) {
		Logs::closeMain();
		CrashReports::Finish();
	}

	const auto error = Core::FishGramUpdates::LaunchUpdateProcess(
		UpdateOwnerWindow(), operation.toStdWString(), nativeBinaryPath,
		arguments.toStdWString(), nativeWorkingDir);
	if (error != ERROR_SUCCESS) {
		_updateLaunchError = (error == ERROR_CANCELLED)
			? u"Starting the update was cancelled."_q
			: u"Windows could not start the update program (error %1). Check file access and retry."_q.arg(error);
		return false;
	}
	return true;
}

} // namespace Platform

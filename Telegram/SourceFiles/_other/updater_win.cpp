/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "updater.h"

#include "base/platform/win/base_windows_safe_library.h"
#include "core/version.h"
#include "_other/fishgram_update_transaction.h"

#include <cstdint>

bool _debug = false;

wstring updaterName, updaterDir, updateTo, exeName, customWorkingDir, customKeyFile;

bool equal(const wstring &a, const wstring &b) {
	return !_wcsicmp(a.c_str(), b.c_str());
}

void updateError(const WCHAR *msg, DWORD errorCode) {
	WCHAR errMsg[2048];
	LPWSTR errorTextFormatted = nullptr;
	auto formatFlags = FORMAT_MESSAGE_FROM_SYSTEM
		| FORMAT_MESSAGE_ALLOCATE_BUFFER
		| FORMAT_MESSAGE_IGNORE_INSERTS;
	FormatMessage(
		formatFlags,
		NULL,
		errorCode,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPWSTR)&errorTextFormatted,
		0,
		0);
	auto errorText = errorTextFormatted
		? errorTextFormatted
		: L"(Unknown error)";
	wsprintf(errMsg, L"%s, error code: %d\nError message: %s", msg, errorCode, errorText);

	MessageBox(0, errMsg, L"Update error!", MB_ICONERROR);

	LocalFree(errorTextFormatted);
}

HANDLE _logFile = 0;
void openLog() {
	if (!_debug || _logFile) return;
	wstring logPath = L"DebugLogs";
	if (!CreateDirectory(logPath.c_str(), NULL)) {
		DWORD errorCode = GetLastError();
		if (errorCode && errorCode != ERROR_ALREADY_EXISTS) {
			updateError(L"Failed to create log directory", errorCode);
			return;
		}
	}

	SYSTEMTIME stLocalTime;

	GetLocalTime(&stLocalTime);

	static const int maxFileLen = MAX_PATH * 10;
	WCHAR logName[maxFileLen];
	wsprintf(logName, L"DebugLogs\\%04d%02d%02d_%02d%02d%02d_upd.txt",
		stLocalTime.wYear, stLocalTime.wMonth, stLocalTime.wDay,
		stLocalTime.wHour, stLocalTime.wMinute, stLocalTime.wSecond);
	_logFile = CreateFile(logName, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
	if (_logFile == INVALID_HANDLE_VALUE) { // :(
		updateError(L"Failed to create log file", GetLastError());
		_logFile = 0;
		return;
	}
}

void closeLog() {
	if (!_logFile) return;

	CloseHandle(_logFile);
	_logFile = 0;
}

void writeLog(const wstring &msg) {
	if (!_logFile) return;

	wstring full = msg + L'\n';
	DWORD written = 0;
	BOOL result = WriteFile(_logFile, full.c_str(), full.size() * sizeof(wchar_t), &written, 0);
	if (!result) {
		updateError((L"Failed to write log entry '" + msg + L"'").c_str(), GetLastError());
		closeLog();
		return;
	}
	BOOL flushr = FlushFileBuffers(_logFile);
	if (!flushr) {
		updateError((L"Failed to flush log on entry '" + msg + L"'").c_str(), GetLastError());
		closeLog();
		return;
	}
}

namespace FishGramTransaction = Core::FishGramUpdates::WindowsTransaction;

const WCHAR *updateResultMessage(FishGramTransaction::Result result) {
	switch (result) {
	case FishGramTransaction::Result::Busy:
		return L"Another update is already running.";
	case FishGramTransaction::Result::InvalidInstallPath:
		return L"The installation folder is invalid.";
	case FishGramTransaction::Result::InvalidWorkPath:
		return L"The update work folder is invalid.";
	case FishGramTransaction::Result::InvalidPayload:
		return L"The update package contains invalid files.";
	case FishGramTransaction::Result::InvalidReadyMarker:
		return L"The update marker is missing or invalid.";
	case FishGramTransaction::Result::VersionMismatch:
		return L"The update package is for a different FishGram build.";
	case FishGramTransaction::Result::ChannelMismatch:
		return L"The update package is for a different update channel.";
	case FishGramTransaction::Result::AppRunning:
		return L"Close FishGram before installing this update.";
	case FishGramTransaction::Result::InsufficientSpace:
		return L"There is not enough free space to install this update.";
	case FishGramTransaction::Result::WriteDenied:
		return L"The installation folder is not writable.";
	case FishGramTransaction::Result::FileInUse:
		return L"An installed program file is in use.";
	case FishGramTransaction::Result::CopyFailed:
		return L"Copying the update failed. The previous program was restored.";
	case FishGramTransaction::Result::RecoveryFailed:
		return L"The previous program could not be fully restored. Use the recovery backup before restarting FishGram.";
	case FishGramTransaction::Result::IoError:
		return L"The update transaction could not be saved.";
	case FishGramTransaction::Result::Applied:
	case FishGramTransaction::Result::NoUpdate:
	case FishGramTransaction::Result::Recovered:
	case FishGramTransaction::Result::NoRecoveryNeeded:
		return L"";
	}
	return L"The update failed safely.";
}

bool update() {
	writeLog(L"Transactional update started.");
	if (updateTo.empty()) {
		updateError(L"No installation folder was provided.", ERROR_INVALID_PARAMETER);
		return false;
	}
	wstring workDir = customWorkingDir;
	if (workDir.empty()) {
		WCHAR currentDirectory[32768] = {};
		const auto length = GetCurrentDirectoryW(DWORD(std::size(currentDirectory)), currentDirectory);
		if (!length || length >= std::size(currentDirectory)) {
			updateError(L"Failed to determine the update work folder", GetLastError());
			return false;
		}
		workDir = currentDirectory;
	}
	auto request = FishGramTransaction::Request();
	request.installDir = updateTo;
	request.workDir = workDir;
	request.executableName = L"Telegram.exe";
	request.runningVersion = (std::uint64_t(FISHGRAM_BASE_VERSION) << 32)
		| std::uint64_t(FISHGRAM_REVISION);
#ifndef TDESKTOP_UPDATE_CHANNEL
#define TDESKTOP_UPDATE_CHANNEL 0
#endif // TDESKTOP_UPDATE_CHANNEL
	request.signedChannel = TDESKTOP_UPDATE_CHANNEL;
	const auto result = FishGramTransaction::Apply(request);
	writeLog(L"Transactional update result: " + std::to_wstring(int(result)));
	if (result == FishGramTransaction::Result::Applied
		|| result == FishGramTransaction::Result::NoUpdate) {
		return true;
	}
	updateError(updateResultMessage(result), DWORD(result));
	return false;
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE prevInstance, LPWSTR cmdParamarg, int cmdShow) {
	base::Platform::InitDynamicLibraries();

	openLog();

	_oldWndExceptionFilter = SetUnhandledExceptionFilter(_exceptionFilter);
//	CAPIHook apiHook("kernel32.dll", "SetUnhandledExceptionFilter", (PROC)RedirectedSetUnhandledExceptionFilter);

	writeLog(L"Updaters started..");

	LPWSTR *args;
	int argsCount;

	bool needupdate = false, autostart = false, debug = false, writeprotected = false, startintray = false;
	args = CommandLineToArgvW(GetCommandLine(), &argsCount);
	if (args) {
		for (int i = 1; i < argsCount; ++i) {
			writeLog(std::wstring(L"Argument: ") + args[i]);
			if (equal(args[i], L"-update")) {
				needupdate = true;
			} else if (equal(args[i], L"-autostart")) {
				autostart = true;
			} else if (equal(args[i], L"-debug")) {
				debug = _debug = true;
				openLog();
			} else if (equal(args[i], L"-startintray")) {
				startintray = true;
			} else if (equal(args[i], L"-writeprotected") && ++i < argsCount) {
				writeLog(std::wstring(L"Argument: ") + args[i]);
				writeprotected = true;
				updateTo = args[i];
				for (int j = 0, l = updateTo.size(); j < l; ++j) {
					if (updateTo[j] == L'/') {
						updateTo[j] = L'\\';
					}
				}
			} else if (equal(args[i], L"-workdir") && ++i < argsCount) {
				writeLog(std::wstring(L"Argument: ") + args[i]);
				customWorkingDir = args[i];
			} else if (equal(args[i], L"-installpath") && ++i < argsCount) {
				updateTo = args[i];
			} else if (equal(args[i], L"-key") && ++i < argsCount) {
				writeLog(std::wstring(L"Argument: ") + args[i]);
				customKeyFile = args[i];
			} else if (equal(args[i], L"-exename") && ++i < argsCount) {
				writeLog(std::wstring(L"Argument: ") + args[i]);
				exeName = args[i];
				for (int j = 0, l = exeName.size(); j < l; ++j) {
					if (exeName[j] == L'/' || exeName[j] == L'\\') {
						exeName = L"Telegram.exe";
						break;
					}
				}
			}
		}
		if (exeName.empty()) {
			exeName = L"Telegram.exe";
		}
		if (needupdate) writeLog(L"Need to update!");
		if (autostart) writeLog(L"From autostart!");
		if (writeprotected) writeLog(L"Write Protected folder!");
		if (!customWorkingDir.empty()) writeLog(L"Will pass custom working dir: " + customWorkingDir);

		updaterName = args[0];
		writeLog(L"Updater name is: " + updaterName);
		if (updaterName.size() > 11) {
			if (equal(updaterName.substr(updaterName.size() - 11), L"Updater.exe")) {
				updaterDir = updaterName.substr(0, updaterName.size() - 11);
				writeLog(L"Updater dir is: " + updaterDir);
				// A staged Updater resides below the work directory. Installation
				// must use the explicit launcher destination, never its own folder.
				writeLog(L"Update to: " + updateTo);
				if (needupdate) {
					if (!update()) { closeLog(); return 1; }
				}

			} else {
				writeLog(L"Error: bad exe name!");
			}
		} else {
			writeLog(L"Error: short exe name!");
		}
		LocalFree(args);
	} else {
		writeLog(L"Error: No command line arguments!");
	}

	wstring targs;
	if (autostart) targs += L" -autostart";
	if (debug) targs += L" -debug";
	if (startintray) targs += L" -startintray";
	if (!customWorkingDir.empty()) {
		targs += L" -workdir \"" + customWorkingDir + L"\"";
	}
	if (!customKeyFile.empty()) {
		targs += L" -key \"" + customKeyFile + L"\"";
	}
	writeLog(L"Result arguments: " + targs);

	bool executed = false;
	if (writeprotected) { // run un-elevated
		writeLog(L"Trying to run un-elevated by temp.lnk");

		HRESULT hres = CoInitialize(0);
		if (SUCCEEDED(hres)) {
			IShellLink* psl;
			HRESULT hres = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLink, (LPVOID*)&psl);
			if (SUCCEEDED(hres)) {
				IPersistFile* ppf;

				wstring exe = FishGramTransaction::Details::Join(updateTo, exeName), dir = updateTo;
				psl->SetArguments((targs.size() ? targs.substr(1) : targs).c_str());
				psl->SetPath(exe.c_str());
				psl->SetWorkingDirectory(dir.c_str());
				psl->SetDescription(L"");

				hres = psl->QueryInterface(IID_IPersistFile, (LPVOID*)&ppf);

				if (SUCCEEDED(hres)) {
					wstring lnk = L"tupdates\\temp\\temp.lnk";
					hres = ppf->Save(lnk.c_str(), TRUE);
					if (!SUCCEEDED(hres)) {
						lnk = L"tupdates\\ready\\temp.lnk"; // old
						hres = ppf->Save(lnk.c_str(), TRUE);
					}
					ppf->Release();

					if (SUCCEEDED(hres)) {
						writeLog(L"Executing un-elevated through link..");
						ShellExecute(0, 0, L"explorer.exe", lnk.c_str(), 0, SW_SHOWNORMAL);
						executed = true;
					} else {
						writeLog(L"Error: ppf->Save failed");
					}
				} else {
					writeLog(L"Error: Could not create interface IID_IPersistFile");
				}
				psl->Release();
			} else {
				writeLog(L"Error: could not create instance of IID_IShellLink");
			}
			CoUninitialize();
		} else {
			writeLog(L"Error: Could not initialize COM");
		}
	}
	if (!executed) {
		ShellExecute(0, 0, (FishGramTransaction::Details::Join(updateTo, exeName)).c_str(), (L"-noupdate" + targs).c_str(), 0, SW_SHOWNORMAL);
	}

	writeLog(L"Executed '" + exeName + L"', closing log and quitting..");
	closeLog();

	return 0;
}

static const WCHAR *_programName = L"FishGram"; // folder in APPDATA, if current path is unavailable for writing
static const WCHAR *_exeName = L"Updater.exe";

LPTOP_LEVEL_EXCEPTION_FILTER _oldWndExceptionFilter = 0;

typedef BOOL (FAR STDAPICALLTYPE *t_miniDumpWriteDump)(
	_In_ HANDLE hProcess,
	_In_ DWORD ProcessId,
	_In_ HANDLE hFile,
	_In_ MINIDUMP_TYPE DumpType,
	_In_opt_ PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
	_In_opt_ PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
	_In_opt_ PMINIDUMP_CALLBACK_INFORMATION CallbackParam
);
t_miniDumpWriteDump miniDumpWriteDump = 0;

HANDLE _generateDumpFileAtPath(const WCHAR *path) {
	static const int maxFileLen = MAX_PATH * 10;

	WCHAR szPath[maxFileLen];
	wsprintf(szPath, L"%stdata\\", path);
	if (!CreateDirectory(szPath, NULL)) {
		if (GetLastError() != ERROR_ALREADY_EXISTS) {
			return 0;
		}
	}
	wsprintf(szPath, L"%sdumps\\", path);
	if (!CreateDirectory(szPath, NULL)) {
		if (GetLastError() != ERROR_ALREADY_EXISTS) {
			return 0;
		}
	}

	WCHAR szFileName[maxFileLen];
	WCHAR szExeName[maxFileLen];

	wcscpy_s(szExeName, _exeName);
	WCHAR *dotFrom = wcschr(szExeName, WCHAR(L'.'));
	if (dotFrom) {
		wsprintf(dotFrom, L"");
	}

	SYSTEMTIME stLocalTime;

	GetLocalTime(&stLocalTime);

	wsprintf(
		szFileName, L"%s%s-%s-%04d%02d%02d-%02d%02d%02d-%ld-%ld.dmp",
		szPath, szExeName, updaterVersionStr,
		stLocalTime.wYear, stLocalTime.wMonth, stLocalTime.wDay,
		stLocalTime.wHour, stLocalTime.wMinute, stLocalTime.wSecond,
		GetCurrentProcessId(), GetCurrentThreadId());
	return CreateFile(szFileName, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_WRITE|FILE_SHARE_READ, 0, CREATE_ALWAYS, 0, 0);
}

void _generateDump(EXCEPTION_POINTERS* pExceptionPointers) {
	static const int maxFileLen = MAX_PATH * 10;

	closeLog();

	HMODULE hDll = LoadLibrary(L"DBGHELP.DLL");
	if (!hDll) return;

	miniDumpWriteDump = (t_miniDumpWriteDump)GetProcAddress(hDll, "MiniDumpWriteDump");
	if (!miniDumpWriteDump) return;

	HANDLE hDumpFile = 0;

	WCHAR szPath[maxFileLen];
	DWORD len = GetModuleFileName(GetModuleHandle(0), szPath, maxFileLen);
	if (!len) return;

	WCHAR *pathEnd = szPath + len;

	if (!_wcsicmp(pathEnd - wcslen(_exeName), _exeName)) {
		wsprintf(pathEnd - wcslen(_exeName), L"");
		hDumpFile = _generateDumpFileAtPath(szPath);
	}
	if (!hDumpFile || hDumpFile == INVALID_HANDLE_VALUE) {
		WCHAR wstrPath[maxFileLen];
		DWORD wstrPathLen = GetEnvironmentVariable(L"APPDATA", wstrPath, maxFileLen);
		if (wstrPathLen) {
			wsprintf(wstrPath + wstrPathLen, L"\\%s\\", _programName);
			hDumpFile = _generateDumpFileAtPath(wstrPath);
		}
	}

	if (!hDumpFile || hDumpFile == INVALID_HANDLE_VALUE) {
		return;
	}

	MINIDUMP_EXCEPTION_INFORMATION ExpParam = {0};
	ExpParam.ThreadId = GetCurrentThreadId();
	ExpParam.ExceptionPointers = pExceptionPointers;
	ExpParam.ClientPointers = TRUE;

	miniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hDumpFile, MiniDumpWithDataSegs, &ExpParam, NULL, NULL);
}

LONG CALLBACK _exceptionFilter(EXCEPTION_POINTERS* pExceptionPointers) {
	_generateDump(pExceptionPointers);
	return _oldWndExceptionFilter ? (*_oldWndExceptionFilter)(pExceptionPointers) : EXCEPTION_CONTINUE_SEARCH;
}

// see http://www.codeproject.com/Articles/154686/SetUnhandledExceptionFilter-and-the-C-C-Runtime-Li
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI RedirectedSetUnhandledExceptionFilter(_In_opt_ LPTOP_LEVEL_EXCEPTION_FILTER lpTopLevelExceptionFilter) {
	// When the CRT calls SetUnhandledExceptionFilter with NULL parameter
	// our handler will not get removed.
	_oldWndExceptionFilter = lpTopLevelExceptionFilter;
	return 0;
}

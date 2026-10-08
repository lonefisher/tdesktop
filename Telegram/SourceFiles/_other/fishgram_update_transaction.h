#pragma once

#include "core/fishgram_update_policy.h"
#include "core/fishgram_client_gate_win.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <Aclapi.h>
#undef small

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Core::FishGramUpdates::WindowsTransaction {

inline constexpr auto kAnyChannel = 0xFFFFFFFFU;
inline constexpr auto kReadyMagic = 0x7FFFFFFDU;

enum class Result {
	Applied,
	NoUpdate,
	Busy,
	InvalidInstallPath,
	InvalidWorkPath,
	InvalidPayload,
	InvalidReadyMarker,
	VersionMismatch,
	ChannelMismatch,
	AppRunning,
	InsufficientSpace,
	WriteDenied,
	FileInUse,
	CopyFailed,
	RecoveryFailed,
	Recovered,
	NoRecoveryNeeded,
	IoError,
	DataSnapshotFailed,
};

struct AuthenticatedFile final {
	std::wstring name;
	std::vector<unsigned char> bytes;
};

struct Request final {
	std::wstring installDir;
	std::wstring workDir;
	std::wstring executableName = L"Telegram.exe";
	std::uint64_t runningVersion = 0;
	std::uint32_t signedChannel = kAnyChannel;
	std::vector<AuthenticatedFile> authenticatedFiles;
	std::function<bool()> authorize;
	std::function<bool(const std::wstring&, std::uint64_t)> spaceAvailable;
	std::function<bool(const std::wstring&)> writeAccess;
	std::function<bool(const std::wstring&)> processRunning;
	std::function<bool(const std::wstring&, const std::wstring&)> copyFile;
	std::function<void()> onLockAcquired;
	std::function<void(std::size_t)> afterReplace;
	std::function<void()> afterCommit;
	std::function<bool()> beforeProgramReplace;
};

struct StartupRecovery final {
	enum class State { None, Required, Blocked };
	State state = State::None;
	std::wstring updaterPath;
	bool requiresElevation = false;
};

namespace Details {

using Names = std::vector<std::wstring>;

[[nodiscard]] inline bool WriteAuthenticated(
		const std::wstring &path, const std::vector<unsigned char> &bytes) {
	const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
		CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	bool okay = !bytes.empty();
	std::size_t offset = 0;
	while (okay && offset < bytes.size()) {
		const auto remaining = bytes.size() - offset;
		const auto chunk = DWORD(remaining > 1024 * 1024 ? 1024 * 1024 : remaining);
		DWORD written = 0;
		okay = WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) && written == chunk;
		offset += written;
	}
	okay = okay && FlushFileBuffers(file);
	CloseHandle(file);
	return okay;
}

struct Payload final {
	Names names;
	std::uint64_t size = 0;
	std::uint64_t version = 0;
	std::uint32_t channel = 0;
};

struct Journal final {
	std::uint32_t state = 1;
	std::uint64_t version = 0;
	std::uint32_t channel = 0;
	std::wstring id;
	Names payloadNames;
	Names originalNames;
};

struct InstallLock final {
	HANDLE handle = INVALID_HANDLE_VALUE;
	~InstallLock() {
		if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
	}
};

struct DirectoryGuard final {
	std::vector<HANDLE> handles;
	~DirectoryGuard() { for (const auto handle : handles) CloseHandle(handle); }
};

[[nodiscard]] inline std::wstring Join(
		const std::wstring &left,
		const std::wstring &right) {
	return left + (left.empty() || left.back() == L'\\' ? L"" : L"\\") + right;
}

[[nodiscard]] inline bool Attributes(
		const std::wstring &path,
		DWORD *result = nullptr) {
	const auto value = GetFileAttributesW(path.c_str());
	if (value == INVALID_FILE_ATTRIBUTES) return false;
	if (result) *result = value;
	return true;
}

[[nodiscard]] inline bool IsAbsent(const std::wstring &path) {
	if (Attributes(path)) return false;
	const auto error = GetLastError();
	return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

[[nodiscard]] inline bool IsSafeDirectory(const std::wstring &path) {
	DWORD attributes = 0;
	return Attributes(path, &attributes)
		&& (attributes & FILE_ATTRIBUTE_DIRECTORY)
		&& !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

[[nodiscard]] inline bool CanonicalDirectory(
		const std::wstring &input,
		std::wstring *output) {
	if (input.empty() || input.find(L'\0') != std::wstring::npos) return false;
	std::vector<wchar_t> buffer(32768);
	const auto length = GetFullPathNameW(
		input.c_str(), DWORD(buffer.size()), buffer.data(), nullptr);
	if (!length || length >= buffer.size()) return false;
	std::wstring path(buffer.data(), length);
	for (auto &ch : path) if (ch == L'/') ch = L'\\';
	if (path.size() < 3 || path[1] != L':' || path[2] != L'\\') return false;
	for (auto i = std::size_t(2); i < path.size(); ++i) {
		if (path[i] == L':') return false;
	}
	while (path.size() > 3 && path.back() == L'\\') path.pop_back();
	for (auto end = std::size_t(3); end <= path.size(); ++end) {
		if (end != path.size() && path[end] != L'\\') continue;
		const auto part = path.substr(0, end);
		DWORD attributes = 0;
		if (!Attributes(part, &attributes)
			|| !(attributes & FILE_ATTRIBUTE_DIRECTORY)
			|| (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
			return false;
		}
	}
	*output = std::move(path);
	return true;
}

[[nodiscard]] inline bool EnsureDirectory(const std::wstring &path) {
	if (CreateDirectoryW(path.c_str(), nullptr)) return true;
	return GetLastError() == ERROR_ALREADY_EXISTS && IsSafeDirectory(path);
}

[[nodiscard]] inline bool HoldDirectoryPath(const std::wstring &path, DirectoryGuard *guard) {
	// Deny write/delete sharing on every path component during filesystem writes.
	// A caller cannot swap an ancestor or retarget it as a reparse point after
	// CanonicalDirectory's initial check and before the transaction uses it.
	for (auto end = std::size_t(3); end <= path.size(); ++end) {
		if (end != path.size() && path[end] != L'\\') continue;
		const auto part = path.substr(0, end);
		const auto handle = CreateFileW(part.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
			FILE_SHARE_READ, nullptr, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if (handle == INVALID_HANDLE_VALUE) return false;
		guard->handles.push_back(handle);
		BY_HANDLE_FILE_INFORMATION info = {};
		if (!GetFileInformationByHandle(handle, &info)
			|| !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			|| (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
	}
	return true;
}

// The journal, retained updater and trust record can authorize recovery writes.
// Keep their filesystem trust boundary at the current user, SYSTEM and admins.
struct TrustedPrincipals final {
	std::vector<unsigned char> user;
	unsigned char system[SECURITY_MAX_SID_SIZE] = {};
	unsigned char admins[SECURITY_MAX_SID_SIZE] = {};
	bool valid = false;
	bool elevated = false;
	TrustedPrincipals() {
		HANDLE token = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return;
		DWORD size = 0;
		GetTokenInformation(token, TokenUser, nullptr, 0, &size);
		user.resize(size);
		TOKEN_ELEVATION elevation = {};
		DWORD elevationSize = 0;
		const auto okay = size && GetTokenInformation(token, TokenUser, user.data(), size, &size)
			&& GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &elevationSize);
		elevated = elevation.TokenIsElevated != 0;
		CloseHandle(token);
		DWORD systemSize = sizeof(system), adminSize = sizeof(admins);
		valid = okay && CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &systemSize)
			&& CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins, &adminSize);
	}
	[[nodiscard]] PSID UserSid() const { return reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid; }
	[[nodiscard]] bool Contains(PSID sid) const {
		return valid && sid && IsValidSid(sid)
			&& ((!elevated && EqualSid(sid, UserSid())) || EqualSid(sid, const_cast<unsigned char*>(system))
				|| EqualSid(sid, const_cast<unsigned char*>(admins)));
	}
};

[[nodiscard]] inline bool HasTrustedPermissions(const std::wstring &path) {
	TrustedPrincipals trusted;
	if (!trusted.valid) return false;
	PSID owner = nullptr;
	PACL dacl = nullptr;
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT,
		OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
		&dacl, nullptr, &descriptor) != ERROR_SUCCESS) return false;
	bool okay = trusted.Contains(owner) && dacl && IsValidAcl(dacl);
	constexpr auto writes = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA
		| FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER
		| GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED;
	for (DWORD i = 0; okay && i < dacl->AceCount; ++i) {
		void *raw = nullptr;
		if (!GetAce(dacl, i, &raw)) { okay = false; break; }
		const auto header = static_cast<const ACE_HEADER*>(raw);
		if (header->AceFlags & INHERIT_ONLY_ACE) continue;
		if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
		if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) { okay = false; break; }
		const auto ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
		if ((ace->Mask & writes) && !trusted.Contains(const_cast<DWORD*>(&ace->SidStart))) okay = false;
	}
	LocalFree(descriptor);
	return okay;
}

[[nodiscard]] inline bool EnsurePrivateDirectory(const std::wstring &path) {
	if (!IsAbsent(path)) return IsSafeDirectory(path) && HasTrustedPermissions(path);
	TrustedPrincipals trusted;
	if (!trusted.valid) return false;
	EXPLICIT_ACCESSW access[3] = {};
	PSID principals[] = {trusted.UserSid(), trusted.system, trusted.admins};
	for (auto i = 0; i != 3; ++i) {
		access[i].grfAccessPermissions = i == 0 && trusted.elevated
			? FILE_GENERIC_READ | FILE_GENERIC_EXECUTE : FILE_ALL_ACCESS;
		access[i].grfAccessMode = SET_ACCESS;
		access[i].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
		access[i].Trustee.TrusteeForm = TRUSTEE_IS_SID;
		access[i].Trustee.ptstrName = static_cast<LPWSTR>(principals[i]);
	}
	PACL dacl = nullptr;
	if (SetEntriesInAclW(3, access, nullptr, &dacl) != ERROR_SUCCESS) return false;
	SECURITY_DESCRIPTOR descriptor = {};
	const auto ready = InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION)
		&& SetSecurityDescriptorDacl(&descriptor, TRUE, dacl, FALSE)
		&& SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED);
	SECURITY_ATTRIBUTES attributes = {sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};
	const auto created = ready && CreateDirectoryW(path.c_str(), &attributes);
	const auto error = GetLastError();
	LocalFree(dacl);
	return (created || error == ERROR_ALREADY_EXISTS)
		&& IsSafeDirectory(path) && HasTrustedPermissions(path);
}

[[nodiscard]] inline bool TrustedMetadataTree(const std::wstring &path, unsigned depth = 0) {
	if (depth > 8 || !IsSafeDirectory(path) || !HasTrustedPermissions(path)) return false;
	WIN32_FIND_DATAW data = {};
	const auto find = FindFirstFileW(Join(path, L"*").c_str(), &data);
	if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
	bool okay = true;
	do {
		const auto name = std::wstring(data.cFileName);
		if (name == L"." || name == L"..") continue;
		const auto child = Join(path, name);
		if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
			|| !HasTrustedPermissions(child)
			|| ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				&& !TrustedMetadataTree(child, depth + 1))) { okay = false; break; }
	} while (FindNextFileW(find, &data));
	const auto error = GetLastError();
	FindClose(find);
	return okay && error == ERROR_NO_MORE_FILES;
}

[[nodiscard]] inline bool IsAsciiSafeName(
		const std::wstring &wide,
		std::string *narrow = nullptr) {
	std::string value;
	value.reserve(wide.size());
	for (const auto ch : wide) {
		if (ch < 32 || ch > 126) return false;
		value.push_back(char(ch));
	}
	if (!Core::FishGramUpdates::IsSafePayloadName(value)) return false;
	if (narrow) *narrow = std::move(value);
	return true;
}

[[nodiscard]] inline bool SameName(
		const std::wstring &left,
		const std::wstring &right) {
	return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

[[nodiscard]] inline bool ContainsName(
		const Names &names,
		const std::wstring &value) {
	for (const auto &name : names) if (SameName(name, value)) return true;
	return false;
}

[[nodiscard]] inline bool FileSize(
		const std::wstring &path,
		std::uint64_t *size) {
	WIN32_FILE_ATTRIBUTE_DATA data = {};
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)
		|| (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
		return false;
	}
	*size = (std::uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
	return true;
}

[[nodiscard]] inline bool EnumerateFlat(
		const std::wstring &directory,
		bool allowReady,
		Names *names,
		std::uint64_t *size,
		std::wstring *readyPath) {
	if (!IsSafeDirectory(directory)) return false;
	WIN32_FIND_DATAW data = {};
	const auto pattern = Join(directory, L"*");
	const auto find = FindFirstFileW(pattern.c_str(), &data);
	if (find == INVALID_HANDLE_VALUE) return false;
	bool okay = true;
	do {
		const auto name = std::wstring(data.cFileName);
		if (name == L"." || name == L"..") continue;
		if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
			okay = false;
			break;
		}
		const auto path = Join(directory, name);
		if (allowReady && SameName(name, L"ready")) {
			if (!readyPath->empty()) {
				okay = false;
				break;
			}
			*readyPath = path;
			continue;
		}
		if (!IsAsciiSafeName(name) || ContainsName(*names, name)) {
			okay = false;
			break;
		}
		std::uint64_t fileSize = 0;
		if (!FileSize(path, &fileSize)
			|| *size > (UINT64_MAX - fileSize)) {
			okay = false;
			break;
		}
		names->push_back(name);
		*size += fileSize;
	} while (FindNextFileW(find, &data));
	const auto lastError = GetLastError();
	FindClose(find);
	return okay && (lastError == ERROR_NO_MORE_FILES);
}

[[nodiscard]] inline bool ReadReady(
		const std::wstring &path,
		std::uint64_t *version,
		std::uint32_t *channel) {
	std::uint64_t size = 0;
	if (!FileSize(path, &size) || size != 16) return false;
	const auto file = CreateFileW(
		path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
		OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	DWORD magic = 0;
	DWORD read = 0;
	std::uint64_t value = 0;
	std::uint32_t signedChannel = 0;
	const auto okay = ReadFile(file, &magic, sizeof(magic), &read, nullptr)
		&& read == sizeof(magic)
		&& ReadFile(file, &value, sizeof(value), &read, nullptr)
		&& read == sizeof(value)
		&& ReadFile(file, &signedChannel, sizeof(signedChannel), &read, nullptr)
		&& read == sizeof(signedChannel);
	CloseHandle(file);
	if (!okay || magic != kReadyMagic || !value || signedChannel > 1) return false;
	*version = value;
	*channel = signedChannel;
	return true;
}

[[nodiscard]] inline bool ReadPayload(
		const std::wstring &directory,
		const Request &request,
		Payload *payload) {
	std::wstring ready;
	if (!EnumerateFlat(directory, true, &payload->names, &payload->size, &ready)
		|| ready.empty()
		|| !ContainsName(payload->names, request.executableName)) {
		return false;
	}
	if (!ReadReady(ready, &payload->version, &payload->channel)) return false;
	if (payload->version != request.runningVersion) return false;
	if (request.signedChannel != kAnyChannel
		&& request.signedChannel != payload->channel) {
		return false;
	}
	return true;
}

[[nodiscard]] inline bool ReadReadyFields(
		const std::wstring &directory,
		std::uint64_t *version,
		std::uint32_t *channel) {
	Names ignored;
	std::uint64_t ignoredSize = 0;
	std::wstring ready;
	return EnumerateFlat(directory, true, &ignored, &ignoredSize, &ready)
		&& !ready.empty() && ReadReady(ready, version, channel);
}

[[nodiscard]] inline bool IsSafeId(const std::wstring &value) {
	if (value.empty() || value.size() > 64) return false;
	for (const auto ch : value) {
		if (!((ch >= L'0' && ch <= L'9')
			|| (ch >= L'a' && ch <= L'f')
			|| ch == L'-')) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] inline std::wstring NewId() {
	FILETIME now = {};
	GetSystemTimeAsFileTime(&now);
	ULARGE_INTEGER ticks = {};
	ticks.LowPart = now.dwLowDateTime;
	ticks.HighPart = now.dwHighDateTime;
	wchar_t value[80] = {};
	swprintf_s(value, L"%016llx-%08lx-%016llx",
		static_cast<unsigned long long>(ticks.QuadPart),
		static_cast<unsigned long>(GetCurrentProcessId()),
		static_cast<unsigned long long>(GetTickCount64()));
	return value;
}

inline void AppendU32(std::vector<unsigned char> *data, std::uint32_t value) {
	for (auto shift = 0; shift != 32; shift += 8) data->push_back((value >> shift) & 0xFF);
}

inline void AppendU64(std::vector<unsigned char> *data, std::uint64_t value) {
	for (auto shift = 0; shift != 64; shift += 8) data->push_back((value >> shift) & 0xFF);
}

inline void AppendText(
		std::vector<unsigned char> *data,
		const std::string &value) {
	AppendU32(data, std::uint32_t(value.size()));
	data->insert(data->end(), value.begin(), value.end());
}

[[nodiscard]] inline bool ReadU32(
		const std::vector<unsigned char> &data,
		std::size_t *position,
		std::uint32_t *value) {
	if (*position > data.size() || data.size() - *position < 4) return false;
	*value = 0;
	for (auto shift = 0; shift != 32; shift += 8) {
		*value |= std::uint32_t(data[(*position)++]) << shift;
	}
	return true;
}

[[nodiscard]] inline bool ReadU64(
		const std::vector<unsigned char> &data,
		std::size_t *position,
		std::uint64_t *value) {
	if (*position > data.size() || data.size() - *position < 8) return false;
	*value = 0;
	for (auto shift = 0; shift != 64; shift += 8) {
		*value |= std::uint64_t(data[(*position)++]) << shift;
	}
	return true;
}

[[nodiscard]] inline bool ReadText(
		const std::vector<unsigned char> &data,
		std::size_t *position,
		std::string *value,
		std::size_t maximum) {
	std::uint32_t length = 0;
	if (!ReadU32(data, position, &length)
		|| length == 0 || length > maximum
		|| *position > data.size() || data.size() - *position < length) {
		return false;
	}
	value->assign(data.begin() + *position, data.begin() + *position + length);
	*position += length;
	return true;
}

[[nodiscard]] inline bool ToWideNames(
		const std::vector<unsigned char> &data,
		std::size_t *position,
		Names *names) {
	std::uint32_t count = 0;
	if (!ReadU32(data, position, &count) || count > 4096) return false;
	for (auto i = 0U; i != count; ++i) {
		std::string name;
		if (!ReadText(data, position, &name, 240)
			|| !Core::FishGramUpdates::IsSafePayloadName(name)) {
			return false;
		}
		const auto wide = std::wstring(name.begin(), name.end());
		if (ContainsName(*names, wide)) return false;
		names->push_back(wide);
	}
	return true;
}

[[nodiscard]] inline std::vector<unsigned char> Serialize(const Journal &journal) {
	static constexpr unsigned char magic[] = { 'F', 'G', 'T', 'X', 'N', '0', '1', 0 };
	std::vector<unsigned char> data(magic, magic + sizeof(magic));
	AppendU32(&data, 1);
	AppendU32(&data, journal.state);
	AppendU64(&data, journal.version);
	AppendU32(&data, journal.channel);
	std::string id;
	for (const auto ch : journal.id) {
		if (ch > 127) return {};
		id.push_back(char(ch));
	}
	AppendText(&data, id);
	const auto appendNames = [&](const Names &names) {
		AppendU32(&data, std::uint32_t(names.size()));
		for (const auto &name : names) {
			std::string narrow;
			if (!IsAsciiSafeName(name, &narrow)) return false;
			AppendText(&data, narrow);
		}
		return true;
	};
	if (!appendNames(journal.payloadNames)
		|| !appendNames(journal.originalNames)) {
		return {};
	}
	return data;
}

[[nodiscard]] inline bool IsValidJournal(const Journal &journal) {
	if (!IsSafeId(journal.id) || !journal.version || journal.channel > 1
		|| !ContainsName(journal.payloadNames, L"Telegram.exe")) {
		return false;
	}
	const auto validNames = [](const Names &names) {
		for (std::size_t i = 0; i != names.size(); ++i) {
			if (!IsAsciiSafeName(names[i])
				|| ContainsName(Names(names.begin(), names.begin() + i), names[i])) {
				return false;
			}
		}
		return true;
	};
	return validNames(journal.payloadNames) && validNames(journal.originalNames);
}

[[nodiscard]] inline bool ParseJournal(
		const std::vector<unsigned char> &data,
		Journal *journal) {
	// A caller may reuse its result object after the durable commit marker is
	// appended. Do not accumulate payload names from an earlier read.
	*journal = {};
	static constexpr unsigned char magic[] = { 'F', 'G', 'T', 'X', 'N', '0', '1', 0 };
	if (data.size() < sizeof(magic)
		|| memcmp(data.data(), magic, sizeof(magic)) != 0) {
		return false;
	}
	std::size_t position = sizeof(magic);
	std::uint32_t schema = 0;
	std::string id;
	if (!ReadU32(data, &position, &schema)
		|| schema != 1
		|| !ReadU32(data, &position, &journal->state)
		|| (journal->state != 1 && journal->state != 2)
		|| !ReadU64(data, &position, &journal->version)
		|| !journal->version
		|| !ReadU32(data, &position, &journal->channel)
		|| journal->channel > 1
		|| !ReadText(data, &position, &id, 64)) {
		return false;
	}
	journal->id.assign(id.begin(), id.end());
	if (!IsSafeId(journal->id)
		|| !ToWideNames(data, &position, &journal->payloadNames)
		|| !ToWideNames(data, &position, &journal->originalNames)
		|| !ContainsName(journal->payloadNames, L"Telegram.exe")) {
		return false;
	}
	static constexpr unsigned char committed[] = { 'C', 'M', 'I', 'T' };
	const auto remaining = data.size() - position;
	if (remaining > sizeof(committed)) return false;
	for (auto i = std::size_t(0); i != remaining; ++i) {
		if (data[position + i] != committed[i]) return false;
	}
	if (remaining == sizeof(committed)) journal->state = 2;
	position += remaining;
	if (position != data.size()) return false;
	return true;
}

[[nodiscard]] inline bool ReadJournal(
		const std::wstring &path,
		Journal *journal) {
	DWORD attributes = 0;
	if (!Attributes(path, &attributes)
		|| (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
		return false;
	}
	const auto file = CreateFileW(
		path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
		OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER length = {};
	const auto sized = GetFileSizeEx(file, &length)
		&& length.QuadPart >= 32 && length.QuadPart <= 1024 * 1024;
	std::vector<unsigned char> data;
	if (sized) data.resize(std::size_t(length.QuadPart));
	DWORD read = 0;
	const auto okay = sized
		&& ReadFile(file, data.data(), DWORD(data.size()), &read, nullptr)
		&& read == data.size();
	CloseHandle(file);
	return okay && ParseJournal(data, journal);
}

[[nodiscard]] inline bool WriteJournal(
		const std::wstring &path,
		const Journal &journal) {
	if (!IsValidJournal(journal) || journal.state != 1) {
		SetLastError(ERROR_INVALID_DATA);
		return false;
	}
	DWORD attributes = 0;
	if (Attributes(path, &attributes)
		&& (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
		SetLastError(ERROR_REPARSE_TAG_INVALID);
		return false;
	}
	const auto data = Serialize(journal);
	if (data.empty()) {
		SetLastError(ERROR_INVALID_DATA);
		return false;
	}
	const auto file = CreateFileW(
		path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	BY_HANDLE_FILE_INFORMATION info = {};
	DWORD written = 0;
	const auto okay = GetFileInformationByHandle(file, &info)
		&& !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
		&& WriteFile(file, data.data(), DWORD(data.size()), &written, nullptr)
		&& written == DWORD(data.size())
		&& FlushFileBuffers(file);
	const auto writeError = okay ? ERROR_SUCCESS : GetLastError();
	CloseHandle(file);
	if (!okay) {
		DeleteFileW(path.c_str());
		SetLastError(writeError);
		return false;
	}
	return true;
}

[[nodiscard]] inline bool MarkCommitted(const std::wstring &path) {
	DWORD attributes = 0;
	if (!Attributes(path, &attributes)
		|| (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
		return false;
	}
	const auto file = CreateFileW(
		path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
		OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	BY_HANDLE_FILE_INFORMATION info = {};
	static constexpr unsigned char committed[] = { 'C', 'M', 'I', 'T' };
	DWORD written = 0;
	const auto okay = GetFileInformationByHandle(file, &info)
		&& !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
		&& WriteFile(file, committed, DWORD(sizeof(committed)), &written, nullptr)
		&& written == sizeof(committed)
		&& FlushFileBuffers(file);
	const auto error = okay ? ERROR_SUCCESS : GetLastError();
	CloseHandle(file);
	if (!okay) SetLastError(error);
	return okay;
}

[[nodiscard]] inline bool FlushPath(const std::wstring &path) {
	const auto file = CreateFileW(
		path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
		OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	BY_HANDLE_FILE_INFORMATION info = {};
	const auto okay = GetFileInformationByHandle(file, &info)
		&& !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
		&& FlushFileBuffers(file);
	CloseHandle(file);
	return okay;
}

[[nodiscard]] inline bool Copy(
		const Request &request,
		const std::wstring &from,
		const std::wstring &to,
		bool failIfExists) {
	if (request.copyFile) return request.copyFile(from, to);
	return CopyFileW(from.c_str(), to.c_str(), failIfExists) != FALSE;
}

[[nodiscard]] inline bool CopyDurable(
		const Request &request,
		const std::wstring &from,
		const std::wstring &to,
		bool failIfExists) {
	if (!Copy(request, from, to, failIfExists)) return false;
	if (!SetFileAttributesW(to.c_str(), FILE_ATTRIBUTE_NORMAL)) return false;
	return FlushPath(to);
}

[[nodiscard]] inline bool RemoveTree(const std::wstring &path) {
	DWORD attributes = 0;
	if (!Attributes(path, &attributes)) {
		const auto error = GetLastError();
		return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
	}
	if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return false;
	if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(path.c_str()) != FALSE;
	WIN32_FIND_DATAW data = {};
	const auto find = FindFirstFileW(Join(path, L"*").c_str(), &data);
	if (find == INVALID_HANDLE_VALUE) return false;
	bool okay = true;
	do {
		const auto name = std::wstring(data.cFileName);
		if (name == L"." || name == L"..") continue;
		if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT
			|| !RemoveTree(Join(path, name))) {
			okay = false;
			break;
		}
	} while (FindNextFileW(find, &data));
	const auto lastError = GetLastError();
	FindClose(find);
	return okay && lastError == ERROR_NO_MORE_FILES && RemoveDirectoryW(path.c_str());
}

[[nodiscard]] inline bool MakeWriteProbe(const std::wstring &directory) {
	wchar_t name[96] = {};
	swprintf_s(name, L".fishgram-write-%08lx-%016llx",
		static_cast<unsigned long>(GetCurrentProcessId()),
		static_cast<unsigned long long>(GetTickCount64()));
	const auto path = Join(directory, name);
	const auto file = CreateFileW(
		path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
		FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	BY_HANDLE_FILE_INFORMATION info = {};
	const auto okay = GetFileInformationByHandle(file, &info)
		&& !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
	CloseHandle(file);
	return okay && DeleteFileW(path.c_str());
}

[[nodiscard]] inline bool TargetProcessRunning(const std::wstring &target) {
	const auto slash = target.find_last_of(L"\\/");
	const auto targetName = target.substr(slash == std::wstring::npos ? 0 : slash + 1);
	const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) return true;
	PROCESSENTRY32W entry = {};
	entry.dwSize = sizeof(entry);
	bool running = false;
	if (Process32FirstW(snapshot, &entry)) {
		do {
			if (!SameName(entry.szExeFile, targetName)) continue;
			const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
			if (!process) {
				running = true;
				break;
			}
			std::vector<wchar_t> image(32768);
			DWORD length = DWORD(image.size());
			const auto queried = QueryFullProcessImageNameW(process, 0, image.data(), &length);
			CloseHandle(process);
			if (!queried) {
				running = true;
				break;
			}
			std::wstring full(image.data(), length);
			std::vector<wchar_t> canonical(32768);
			const auto fullLength = GetFullPathNameW(full.c_str(), DWORD(canonical.size()), canonical.data(), nullptr);
			if (!fullLength || fullLength >= canonical.size()
				|| _wcsicmp(canonical.data(), target.c_str()) == 0) {
				running = true;
				break;
			}
		} while (Process32NextW(snapshot, &entry));
	}
	CloseHandle(snapshot);
	return running;
}

[[nodiscard]] inline bool TargetIsReplaceable(
		const std::wstring &path,
		Result *failure) {
	DWORD attributes = 0;
	if (!Attributes(path, &attributes)) {
		const auto error = GetLastError();
		if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return true;
		*failure = Result::WriteDenied;
		return false;
	}
	if (attributes & FILE_ATTRIBUTE_REPARSE_POINT
		|| attributes & FILE_ATTRIBUTE_DIRECTORY
		|| attributes & FILE_ATTRIBUTE_READONLY) {
		*failure = Result::WriteDenied;
		return false;
	}
	const auto file = CreateFileW(
		path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr,
		OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		const auto error = GetLastError();
		*failure = (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION)
			? Result::FileInUse
			: Result::WriteDenied;
		return false;
	}
	BY_HANDLE_FILE_INFORMATION info = {};
	const auto okay = GetFileInformationByHandle(file, &info)
		&& !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
	CloseHandle(file);
	if (!okay) *failure = Result::WriteDenied;
	return okay;
}

[[nodiscard]] inline bool EnoughSpace(
		const Request &request,
		const std::wstring &installDir,
		std::uint64_t required) {
	if (request.spaceAvailable) return request.spaceAvailable(installDir, required);
	ULARGE_INTEGER available = {};
	return GetDiskFreeSpaceExW(installDir.c_str(), &available, nullptr, nullptr)
		&& available.QuadPart >= required;
}

[[nodiscard]] inline bool ReadProgramFiles(
		const std::wstring &installDir,
		Names *names,
		std::uint64_t *size) {
	WIN32_FIND_DATAW data = {};
	const auto find = FindFirstFileW(Join(installDir, L"*").c_str(), &data);
	if (find == INVALID_HANDLE_VALUE) return false;
	bool hasExecutable = false;
	bool okay = true;
	do {
		const auto name = std::wstring(data.cFileName);
		if (name == L"." || name == L".."
			|| SameName(name, L".fishgram-update")) continue;
		if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
			okay = false;
			break;
		}
		if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
		if (!IsAsciiSafeName(name)) continue;
		std::uint64_t fileSize = 0;
		if (!FileSize(Join(installDir, name), &fileSize)
			|| *size > (UINT64_MAX - fileSize)) {
			okay = false;
			break;
		}
		names->push_back(name);
		*size += fileSize;
		hasExecutable = hasExecutable || SameName(name, L"Telegram.exe");
	} while (FindNextFileW(find, &data));
	const auto lastError = GetLastError();
	FindClose(find);
	return okay && lastError == ERROR_NO_MORE_FILES && hasExecutable;
}

[[nodiscard]] inline bool AcquireLock(
		const std::wstring &meta,
		InstallLock *lock,
		Result *failure) {
	const auto path = Join(meta, L"install.lock");
	DWORD attributes = 0;
	if (Attributes(path, &attributes)
		&& (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
		*failure = Result::WriteDenied;
		return false;
	}
	lock->handle = CreateFileW(
		path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (lock->handle == INVALID_HANDLE_VALUE) {
		const auto error = GetLastError();
		*failure = (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION)
			? Result::Busy
			: Result::WriteDenied;
		return false;
	}
	BY_HANDLE_FILE_INFORMATION info = {};
	if (!GetFileInformationByHandle(lock->handle, &info)
		|| (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
		*failure = Result::WriteDenied;
		return false;
	}
	return true;
}

[[nodiscard]] inline bool DirectoryEntries(
		const std::wstring &directory,
		Names *entries) {
	WIN32_FIND_DATAW data = {};
	const auto find = FindFirstFileW(Join(directory, L"*").c_str(), &data);
	if (find == INVALID_HANDLE_VALUE) return false;
	bool okay = true;
	do {
		const auto name = std::wstring(data.cFileName);
		if (name == L"." || name == L"..") continue;
		if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			|| (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
			|| !IsSafeId(name)) {
			okay = false;
			break;
		}
		entries->push_back(name);
	} while (FindNextFileW(find, &data));
	const auto lastError = GetLastError();
	FindClose(find);
	return okay && lastError == ERROR_NO_MORE_FILES;
}

[[nodiscard]] inline bool TrimVersions(const std::wstring &versions) {
	Names entries;
	if (!DirectoryEntries(versions, &entries)) return false;
	for (auto i = std::size_t(0); i < entries.size(); ++i) {
		for (auto j = i + 1; j < entries.size(); ++j) {
			if (entries[i] > entries[j]) {
				const auto swap = entries[i];
				entries[i] = entries[j];
				entries[j] = swap;
			}
		}
	}
	while (entries.size() > 2) {
		if (!RemoveTree(Join(versions, entries.front()))) return false;
		entries.erase(entries.begin());
	}
	return true;
}

[[nodiscard]] inline bool TrustedProgramBoundary(const std::wstring &installDir) {
	if (!HasTrustedPermissions(installDir)) return false;
	// A private directory does not make an explicitly shared executable safe.
	WIN32_FIND_DATAW data = {};
	const auto find = FindFirstFileW(Join(installDir, L"*").c_str(), &data);
	if (find == INVALID_HANDLE_VALUE) return false;
	bool trusted = true;
	do {
		const auto name = std::wstring(data.cFileName);
		if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
		if (IsAsciiSafeName(name) && !HasTrustedPermissions(Join(installDir, name))) { trusted = false; break; }
	} while (FindNextFileW(find, &data));
	const auto error = GetLastError();
	FindClose(find);
	return trusted && error == ERROR_NO_MORE_FILES;
}

[[nodiscard]] inline bool PrepareMetadata(
		const std::wstring &installDir,
		std::wstring *meta,
		std::wstring *versions) {
	*meta = Join(installDir, L".fishgram-update");
	*versions = Join(*meta, L"versions");
	return TrustedProgramBoundary(installDir) && EnsurePrivateDirectory(*meta)
		&& EnsurePrivateDirectory(*versions) && TrustedMetadataTree(*meta);
}

[[nodiscard]] inline bool LoadJournalNames(
		const std::wstring &path,
		Journal *journal) {
	return ReadJournal(path, journal);
}

[[nodiscard]] inline bool HasOriginal(
		const Journal &journal,
		const std::wstring &name) {
	return ContainsName(journal.originalNames, name);
}

[[nodiscard]] inline Result Rollback(
		const std::wstring &meta,
		const std::wstring &pending,
		const std::wstring &versions,
		const Journal &journal,
		const Request *request) {
	const auto backup = Join(pending, L"backup");
	const auto archive = Join(versions, journal.id);
	const auto stage = Join(pending, L"stage");
	const auto sourceRoot = IsSafeDirectory(backup) ? backup : archive;
	if (!IsSafeDirectory(sourceRoot)) return Result::RecoveryFailed;
	if (!EnsureDirectory(stage)) return Result::RecoveryFailed;
	for (const auto &name : journal.payloadNames) {
		const auto target = Join(meta.substr(0, meta.size() - std::wstring(L".fishgram-update").size() - 1), name);
		DWORD attributes = 0;
		const auto exists = Attributes(target, &attributes);
		if (exists && (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
			return Result::RecoveryFailed;
		}
		if (HasOriginal(journal, name)) {
			const auto savedName = [&] {
				for (const auto &original : journal.originalNames) {
					if (SameName(original, name)) return original;
				}
				return name;
			}();
		const auto saved = Join(sourceRoot, savedName);
		if (IsAbsent(saved)) return Result::RecoveryFailed;
		const auto restore = Join(stage, name + L".restore");
		if (Attributes(restore) && !DeleteFileW(restore.c_str())) return Result::RecoveryFailed;
		const auto copied = request
			? CopyDurable(*request, saved, restore, true)
			: (CopyFileW(saved.c_str(), restore.c_str(), TRUE)
				&& SetFileAttributesW(restore.c_str(), FILE_ATTRIBUTE_NORMAL)
				&& FlushPath(restore));
		if (!copied || !MoveFileExW(
			restore.c_str(), target.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			return Result::RecoveryFailed;
		}
		if (!SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL)) return Result::RecoveryFailed;
		if (request && request->afterReplace) request->afterReplace(0);
		continue;
		}
		if (exists && !DeleteFileW(target.c_str())) return Result::RecoveryFailed;
	}
	if (Attributes(archive) && !RemoveTree(archive)) return Result::RecoveryFailed;
	if (!RemoveTree(pending)) return Result::RecoveryFailed;
	return Result::Recovered;
}

[[nodiscard]] inline Result RecoverLocked(
		const std::wstring &meta,
		const std::wstring &versions,
		const Request *request) {
	const auto pending = Join(meta, L"pending");
	if (IsAbsent(pending)) return Result::NoRecoveryNeeded;
	if (!IsSafeDirectory(pending)) return Result::RecoveryFailed;
	const auto journalPath = Join(pending, L"journal.bin");
	if (IsAbsent(journalPath)) {
		return RemoveTree(pending) ? Result::Recovered : Result::RecoveryFailed;
	}
	Journal journal;
	if (!LoadJournalNames(journalPath, &journal)) return Result::RecoveryFailed;
	if (journal.state == 2) {
		// A process can stop after the durable commit but before normal cleanup.
		// Keep the new program and apply the same retention policy on recovery.
		return TrimVersions(versions) && RemoveTree(pending)
			? Result::Recovered : Result::RecoveryFailed;
	}
	return Rollback(meta, pending, versions, journal, request);
}

[[nodiscard]] inline bool Preflight(
		const Request &request,
		const std::wstring &installDir,
		const Payload &payload,
		Names *originalNames,
		std::uint64_t *backupSize,
		Result *failure) {
	if (request.processRunning
		? request.processRunning(Join(installDir, request.executableName))
		: TargetProcessRunning(Join(installDir, request.executableName))) {
		*failure = Result::AppRunning;
		return false;
	}
	if (request.writeAccess
		? !request.writeAccess(installDir)
		: !MakeWriteProbe(installDir)) {
		*failure = Result::WriteDenied;
		return false;
	}
	std::uint64_t oldSize = 0;
	if (!ReadProgramFiles(installDir, originalNames, &oldSize)) {
		*failure = Result::InvalidInstallPath;
		return false;
	}
	for (const auto &name : payload.names) {
		if (!TargetIsReplaceable(Join(installDir, name), failure)) return false;
	}
	if (payload.size > UINT64_MAX - oldSize - (1024 * 1024)) {
		*failure = Result::InsufficientSpace;
		return false;
	}
	const auto required = oldSize + payload.size + (1024 * 1024);
	if (!EnoughSpace(request, installDir, required)) {
		*failure = Result::InsufficientSpace;
		return false;
	}
	*backupSize = oldSize;
	return true;
}

[[nodiscard]] inline Result Begin(
		const Request &request,
		const std::wstring &installDir,
		const std::wstring &workDir,
		const std::wstring &meta,
		const std::wstring &versions,
		const Payload &payload) {
	Names originalNames;
	std::uint64_t backupSize = 0;
	Result failure = Result::IoError;
	if (!Preflight(request, installDir, payload, &originalNames, &backupSize, &failure)) {
		return failure;
	}
	// Production uses this hook for a cross-baseline account snapshot. It runs
	// under the install lock after process/space/write checks and before replacing
	// any program byte or creating a pending installation transaction.
	if (request.beforeProgramReplace && !request.beforeProgramReplace()) return Result::DataSnapshotFailed;
	// Keep a trusted pre-update recovery executable outside pending and tupdates.
	// It must survive both ready cleanup and rollback while it is running.
	const auto recovery = Join(meta, L"recovery");
	const auto recoveryUpdater = Join(recovery, L"Updater.exe");
	const auto recoveryNew = Join(recovery, L"Updater.exe.new");
	if (!ContainsName(originalNames, L"Updater.exe") || !EnsureDirectory(recovery)) {
		return Result::InvalidInstallPath;
	}
	DWORD recoveryAttributes = 0;
	if (Attributes(recoveryUpdater, &recoveryAttributes)
		&& (recoveryAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
		return Result::InvalidInstallPath;
	}
	if (Attributes(recoveryNew) && !DeleteFileW(recoveryNew.c_str())) return Result::CopyFailed;
	if (!CopyDurable(request, Join(installDir, L"Updater.exe"), recoveryNew, true)
		|| !MoveFileExW(recoveryNew.c_str(), recoveryUpdater.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		return Result::CopyFailed;
	}
	const auto pending = Join(meta, L"pending");
	if (!EnsureDirectory(pending)) return Result::WriteDenied;
	const auto backup = Join(pending, L"backup");
	const auto stage = Join(pending, L"stage");
	if (!EnsureDirectory(backup) || !EnsureDirectory(stage)) {
		return RemoveTree(pending) ? Result::WriteDenied : Result::RecoveryFailed;
	}
	for (const auto &name : originalNames) {
		const auto source = Join(installDir, name);
		const auto saved = Join(backup, name);
		if (!CopyDurable(request, source, saved, true)) {
			return RemoveTree(pending) ? Result::CopyFailed : Result::RecoveryFailed;
		}
	}
	Journal journal;
	journal.state = 1;
	journal.version = payload.version;
	journal.channel = payload.channel;
	journal.id = NewId();
	journal.payloadNames = payload.names;
	journal.originalNames = originalNames;
	if (!WriteJournal(Join(pending, L"journal.bin"), journal)) {
		return RemoveTree(pending) ? Result::IoError : Result::RecoveryFailed;
	}
	std::size_t replaced = 0;
	failure = Result::IoError;
	for (const auto &name : payload.names) {
		const auto source = Join(Join(workDir, L"tupdates\\temp"), name);
		const auto staged = Join(stage, name + L".new");
		const auto target = Join(installDir, name);
		if (Attributes(staged) && !DeleteFileW(staged.c_str())) {
			failure = Result::CopyFailed;
			break;
		}
		const auto copied = [&] {
			if (request.authenticatedFiles.empty()) return CopyDurable(request, source, staged, true);
			for (const auto &file : request.authenticatedFiles) {
				if (SameName(file.name, name)) return WriteAuthenticated(staged, file.bytes);
			}
			return false;
		}();
		if (!copied
			|| !MoveFileExW(
				staged.c_str(), target.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			failure = Result::CopyFailed;
			break;
		}
		if (!SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL)) {
			failure = Result::CopyFailed;
			break;
		}
		if (request.afterReplace) request.afterReplace(++replaced);
	}
	if (failure != Result::IoError) {
		return Rollback(meta, pending, versions, journal, &request) == Result::Recovered
			? failure
			: Result::RecoveryFailed;
	}
	const auto archive = Join(versions, journal.id);
	if (!MoveFileExW(backup.c_str(), archive.c_str(), MOVEFILE_WRITE_THROUGH)) {
		return Rollback(meta, pending, versions, journal, &request) == Result::Recovered
			? Result::CopyFailed
			: Result::RecoveryFailed;
	}
	if (!MarkCommitted(Join(pending, L"journal.bin"))) {
		return Rollback(meta, pending, versions, journal, &request) == Result::Recovered
			? Result::IoError
			: Result::RecoveryFailed;
	}
	if (request.afterCommit) request.afterCommit();
	if (!TrimVersions(versions) || !RemoveTree(pending)) return Result::Applied;
	return Result::Applied;
}

[[nodiscard]] inline bool DirectoriesDisjoint(
		const std::wstring &left,
		const std::wstring &right) {
	const auto within = [](const std::wstring &child, const std::wstring &parent) {
		if (child.size() <= parent.size()
			|| _wcsnicmp(child.c_str(), parent.c_str(), parent.size()) != 0) {
			return false;
		}
		return parent.back() == L'\\' || child[parent.size()] == L'\\';
	};
	return !within(left, right) && !within(right, left);
}

} // namespace Details

[[nodiscard]] inline Result Apply(const Request &request) {
	using namespace Details;
	std::wstring installDir;
	if (!CanonicalDirectory(request.installDir, &installDir)) return Result::InvalidInstallPath;
	std::wstring workDir;
	if (!CanonicalDirectory(request.workDir, &workDir)
		|| !DirectoriesDisjoint(Join(installDir, L".fishgram-update"),
			Join(workDir, L"tupdates"))) {
		return Result::InvalidWorkPath;
	}
	DirectoryGuard installGuard, workGuard;
	if (!HoldDirectoryPath(installDir, &installGuard)
		|| !HoldDirectoryPath(workDir, &workGuard)) return Result::WriteDenied;
	if (!SameName(request.executableName, L"Telegram.exe")
		|| !IsAsciiSafeName(request.executableName)
		|| !request.runningVersion) {
		return Result::InvalidPayload;
	}
	std::wstring meta;
	std::wstring versions;
	if (!PrepareMetadata(installDir, &meta, &versions)) return Result::WriteDenied;
	InstallLock lock;
	Result failure = Result::IoError;
	if (!AcquireLock(meta, &lock, &failure)) return failure;
	Core::FishGramClientGate::Lease clientGate;
	const auto gate = Core::FishGramClientGate::TryAcquire(installDir, true, &clientGate);
	if (gate == Core::FishGramClientGate::AcquireResult::Busy) return Result::Busy;
	if (gate != Core::FishGramClientGate::AcquireResult::Acquired) return Result::WriteDenied;
	if (request.onLockAcquired) request.onLockAcquired();
	if (request.processRunning
		? request.processRunning(Join(installDir, request.executableName))
		: TargetProcessRunning(Join(installDir, request.executableName))) return Result::AppRunning;
	if (request.authorize && !request.authorize()) return Result::InvalidPayload;
	const auto recovered = RecoverLocked(meta, versions, &request);
	if (recovered == Result::RecoveryFailed) return recovered;
	const auto updatesDir = Join(workDir, L"tupdates");
	if (Attributes(updatesDir)
		&& (!IsSafeDirectory(updatesDir)
			|| (GetFileAttributesW(updatesDir.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT))) {
		return Result::InvalidWorkPath;
	}
	const auto payloadDir = Join(updatesDir, L"temp");
	if (Attributes(payloadDir)
		&& (!IsSafeDirectory(payloadDir)
			|| (GetFileAttributesW(payloadDir.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT))) {
		return Result::InvalidPayload;
	}
	if (IsAbsent(payloadDir)) return Result::NoUpdate;
	Payload payload;
	std::wstring ready;
	if (!EnumerateFlat(payloadDir, true, &payload.names, &payload.size, &ready)
		|| ready.empty()
		|| !ContainsName(payload.names, request.executableName)) {
		return Result::InvalidPayload;
	}
	if (!ReadReady(ready, &payload.version, &payload.channel)) return Result::InvalidReadyMarker;
	if (!request.authenticatedFiles.empty()) {
		if (payload.names.size() != request.authenticatedFiles.size()) return Result::InvalidPayload;
		Names authenticatedNames;
		payload.size = 0;
		for (const auto &file : request.authenticatedFiles) {
			if (!IsAsciiSafeName(file.name) || !ContainsName(payload.names, file.name)
				|| ContainsName(authenticatedNames, file.name) || file.bytes.empty()
				|| file.bytes.size() > (1024ULL * 1024 * 1024 - payload.size)) return Result::InvalidPayload;
			authenticatedNames.push_back(file.name);
			payload.size += file.bytes.size();
		}
	}
	if (payload.version != request.runningVersion) return Result::VersionMismatch;
	if (request.signedChannel != kAnyChannel && request.signedChannel != payload.channel) {
		return Result::ChannelMismatch;
	}
	return Begin(request, installDir, workDir, meta, versions, payload);
}

[[nodiscard]] inline Result RecoverPending(const std::wstring &inputInstallDir) {
	using namespace Details;
	std::wstring installDir;
	if (!CanonicalDirectory(inputInstallDir, &installDir)) return Result::InvalidInstallPath;
	DirectoryGuard guard;
	if (!HoldDirectoryPath(installDir, &guard)) return Result::WriteDenied;
	std::wstring meta;
	std::wstring versions;
	if (!PrepareMetadata(installDir, &meta, &versions)) return Result::WriteDenied;
	InstallLock lock;
	Result failure = Result::IoError;
	if (!AcquireLock(meta, &lock, &failure)) return failure;
	Core::FishGramClientGate::Lease clientGate;
	const auto gate = Core::FishGramClientGate::TryAcquire(installDir, true, &clientGate);
	if (gate == Core::FishGramClientGate::AcquireResult::Busy) return Result::Busy;
	if (gate != Core::FishGramClientGate::AcquireResult::Acquired) return Result::WriteDenied;
	if (TargetProcessRunning(Join(installDir, L"Telegram.exe"))) return Result::AppRunning;
	return RecoverLocked(meta, versions, nullptr);
}

[[nodiscard]] inline StartupRecovery InspectStartupRecovery(
		const std::wstring &inputInstallDir) {
	using namespace Details;
	std::wstring installDir;
	if (!CanonicalDirectory(inputInstallDir, &installDir)) return { StartupRecovery::State::Blocked, {} };
	const auto meta = Join(installDir, L".fishgram-update");
	if (IsAbsent(meta)) return {};
	const auto pending = Join(meta, L"pending");
	if (!IsSafeDirectory(meta)) return { StartupRecovery::State::Blocked, {} };
	if (IsAbsent(pending)) return {};
	DirectoryGuard guard;
	if (!HoldDirectoryPath(installDir, &guard) || !TrustedProgramBoundary(installDir)
		|| !TrustedMetadataTree(meta)) return { StartupRecovery::State::Blocked, {} };
	InstallLock lock;
	Result failure = Result::IoError;
	const auto locked = AcquireLock(meta, &lock, &failure);
	if (!locked && failure != Result::WriteDenied) return { StartupRecovery::State::Blocked, {} };
	if (IsAbsent(pending)) return {};
	if (!IsSafeDirectory(pending)) return { StartupRecovery::State::Blocked, {} };
	Journal journal;
	const auto journalPath = Join(pending, L"journal.bin");
	// Before the journal is committed no program file has been replaced.
	// The recovery process can safely remove this incomplete preparation.
	if (!IsAbsent(journalPath) && !ReadJournal(journalPath, &journal)) return { StartupRecovery::State::Blocked, {} };
	std::wstring recovery;
	if (!CanonicalDirectory(Join(meta, L"recovery"), &recovery)) return { StartupRecovery::State::Blocked, {} };
	const auto updater = Join(recovery, L"Updater.exe");
	DWORD attributes = 0;
	if (!Attributes(updater, &attributes)
		|| (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
		return { StartupRecovery::State::Blocked, {} };
	}
	return { StartupRecovery::State::Required, updater, !locked };
}

[[nodiscard]] inline bool PrepareUpdateRunner(
		const std::wstring &inputInstallDir, std::wstring *updaterPath) {
	using namespace Details;
	std::wstring installDir, meta, versions;
	if (!CanonicalDirectory(inputInstallDir, &installDir)) return false;
	DirectoryGuard guard;
	if (!HoldDirectoryPath(installDir, &guard) || !PrepareMetadata(installDir, &meta, &versions)) return false;
	InstallLock lock;
	Result failure = Result::IoError;
	if (!AcquireLock(meta, &lock, &failure) || !IsAbsent(Join(meta, L"pending"))) return false;
	const auto source = Join(installDir, L"Updater.exe");
	const auto held = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ,
		nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (held == INVALID_HANDLE_VALUE) return false;
	BY_HANDLE_FILE_INFORMATION info = {};
	const auto valid = GetFileInformationByHandle(held, &info)
		&& !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
	const auto runner = Join(meta, L"runner");
	const auto next = Join(runner, L"Updater.exe.new");
	const auto target = Join(runner, L"Updater.exe");
	const auto prepared = valid && EnsureDirectory(runner)
		&& (IsAbsent(next) || DeleteFileW(next.c_str()))
		&& CopyDurable(Request(), source, next, true)
		&& MoveFileExW(next.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	CloseHandle(held);
	if (prepared) *updaterPath = target;
	return prepared;
}

} // namespace Core::FishGramUpdates::WindowsTransaction

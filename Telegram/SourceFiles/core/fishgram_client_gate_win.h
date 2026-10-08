#pragma once

#include <Windows.h>
#include <Aclapi.h>

#include <string>
#include <utility>
#include <vector>

namespace Core::FishGramClientGate {

// This lease coordinates gate-aware clients and updater/recovery processes.
// Legacy clients still rely on the transaction's executable-path check, which
// cannot close their startup race; automatic updates require gate-aware peers.

enum class AcquireResult { Acquired, Busy, Denied };

class Lease final {
public:
	Lease() = default;
	Lease(const Lease&) = delete;
	Lease &operator=(const Lease&) = delete;
	Lease(Lease&& other) noexcept
	: _handle(other._handle), _directories(std::move(other._directories)) {
		other._handle = INVALID_HANDLE_VALUE;
		other._directories.clear();
	}
	Lease &operator=(Lease&& other) noexcept {
		if (this != &other) {
			Reset();
			_handle = other._handle;
			_directories = std::move(other._directories);
			other._handle = INVALID_HANDLE_VALUE;
			other._directories.clear();
		}
		return *this;
	}
	~Lease() { Reset(); }

	void Reset() {
		if (_handle != INVALID_HANDLE_VALUE) {
			OVERLAPPED overlapped = {};
			UnlockFileEx(_handle, 0, 1, 0, &overlapped);
			CloseHandle(_handle);
			_handle = INVALID_HANDLE_VALUE;
		}
		for (const auto directory : _directories) CloseHandle(directory);
		_directories.clear();
	}

private:
	friend AcquireResult TryAcquire(const std::wstring&, bool, Lease*);
	HANDLE _handle = INVALID_HANDLE_VALUE;
	std::vector<HANDLE> _directories;
};

namespace Details {

struct Principals final {
	std::vector<unsigned char> user;
	unsigned char system[SECURITY_MAX_SID_SIZE] = {};
	unsigned char admins[SECURITY_MAX_SID_SIZE] = {};
	bool valid = false;
	Principals() {
		HANDLE token = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return;
		DWORD bytes = 0;
		GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
		user.resize(bytes);
		const auto okay = bytes
			&& GetTokenInformation(token, TokenUser, user.data(), bytes, &bytes);
		CloseHandle(token);
		DWORD systemBytes = sizeof(system), adminBytes = sizeof(admins);
		valid = okay
			&& CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &systemBytes)
			&& CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins, &adminBytes);
	}
	PSID User() const {
		return reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid;
	}
	bool Allows(PSID sid) const {
		return valid && sid && IsValidSid(sid)
			&& (EqualSid(sid, User())
				|| EqualSid(sid, const_cast<unsigned char*>(system))
				|| EqualSid(sid, const_cast<unsigned char*>(admins)));
	}
};

inline bool PrivateSecurity(Principals &principals, SECURITY_ATTRIBUTES *attributes,
		SECURITY_DESCRIPTOR *descriptor, PACL *acl) {
	if (!principals.valid) return false;
	EXPLICIT_ACCESSW entries[3] = {};
	PSID sids[] = { principals.User(), principals.system, principals.admins };
	for (int i = 0; i != 3; ++i) {
		entries[i].grfAccessPermissions = FILE_ALL_ACCESS;
		entries[i].grfAccessMode = SET_ACCESS;
		entries[i].grfInheritance = NO_INHERITANCE;
		entries[i].Trustee.TrusteeForm = TRUSTEE_IS_SID;
		entries[i].Trustee.ptstrName = static_cast<LPWSTR>(sids[i]);
	}
	return SetEntriesInAclW(3, entries, nullptr, acl) == ERROR_SUCCESS
		&& InitializeSecurityDescriptor(descriptor, SECURITY_DESCRIPTOR_REVISION)
		&& SetSecurityDescriptorDacl(descriptor, TRUE, *acl, FALSE)
		&& SetSecurityDescriptorControl(descriptor,
			SE_DACL_PROTECTED, SE_DACL_PROTECTED)
		&& ((*attributes = { sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE }), true);
}

inline bool HasPrivateSecurity(HANDLE file, const Principals &principals) {
	PSID owner = nullptr;
	PACL dacl = nullptr;
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	if (GetSecurityInfo(file, SE_FILE_OBJECT,
		OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
		&owner, nullptr, &dacl, nullptr, &descriptor) != ERROR_SUCCESS) return false;
	bool okay = principals.Allows(owner) && dacl && IsValidAcl(dacl);
	for (DWORD i = 0; okay && i < dacl->AceCount; ++i) {
		void *raw = nullptr;
		if (!GetAce(dacl, i, &raw)) { okay = false; break; }
		const auto header = static_cast<const ACE_HEADER*>(raw);
		if (header->AceFlags & INHERIT_ONLY_ACE) continue;
		if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
		if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) { okay = false; break; }
		const auto ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
		if (!principals.Allows(const_cast<DWORD*>(&ace->SidStart))) okay = false;
	}
	LocalFree(descriptor);
	return okay;
}

inline bool SafeInstallDirectory(
		const std::wstring &input,
		std::wstring *output,
		std::vector<HANDLE> *guards) {
	std::vector<wchar_t> buffer(32768);
	const auto length = GetFullPathNameW(input.c_str(), DWORD(buffer.size()), buffer.data(), nullptr);
	if (!length || length >= buffer.size()) return false;
	std::wstring path(buffer.data(), length);
	for (auto &ch : path) if (ch == L'/') ch = L'\\';
	if (path.size() < 3 || path[1] != L':' || path[2] != L'\\') return false;
	for (auto end = std::size_t(3); end <= path.size(); ++end) {
		if (end != path.size() && path[end] != L'\\') continue;
		const auto part = path.substr(0, end);
		const auto directory = CreateFileW(part.c_str(), FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ, nullptr, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if (directory == INVALID_HANDLE_VALUE) return false;
		guards->push_back(directory);
		BY_HANDLE_FILE_INFORMATION info = {};
		if (!GetFileInformationByHandle(directory, &info)
			|| !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			|| (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
	}
	*output = std::move(path);
	return true;
}

} // namespace Details

[[nodiscard]] inline AcquireResult TryAcquire(
		const std::wstring &inputInstallDir,
		bool exclusive,
		Lease *lease) {
	if (!lease || lease->_handle != INVALID_HANDLE_VALUE) return AcquireResult::Denied;
	std::wstring installDir;
	std::vector<HANDLE> directories;
	if (!Details::SafeInstallDirectory(inputInstallDir, &installDir, &directories)) {
		for (const auto directory : directories) CloseHandle(directory);
		return AcquireResult::Denied;
	}
	Details::Principals principals;
	SECURITY_DESCRIPTOR descriptor = {};
	SECURITY_ATTRIBUTES security = {};
	PACL acl = nullptr;
	if (!Details::PrivateSecurity(principals, &security, &descriptor, &acl)) {
		for (const auto directory : directories) CloseHandle(directory);
		return AcquireResult::Denied;
	}
	const auto path = installDir + L"\\client-session.lock";
	auto file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, &security, CREATE_NEW,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_EXISTS) {
		file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	}
	LocalFree(acl);
	if (file == INVALID_HANDLE_VALUE) {
		for (const auto directory : directories) CloseHandle(directory);
		return AcquireResult::Denied;
	}
	BY_HANDLE_FILE_INFORMATION info = {};
	if (!GetFileInformationByHandle(file, &info)
		|| (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
		|| !Details::HasPrivateSecurity(file, principals)) {
		CloseHandle(file);
		for (const auto directory : directories) CloseHandle(directory);
		return AcquireResult::Denied;
	}
	OVERLAPPED overlapped = {};
	const auto flags = DWORD(LOCKFILE_FAIL_IMMEDIATELY
		| (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0));
	if (!LockFileEx(file, flags, 0, 1, 0, &overlapped)) {
		const auto error = GetLastError();
		CloseHandle(file);
		for (const auto directory : directories) CloseHandle(directory);
		return (error == ERROR_LOCK_VIOLATION || error == ERROR_SHARING_VIOLATION)
			? AcquireResult::Busy : AcquireResult::Denied;
	}
	lease->_handle = file;
	lease->_directories = std::move(directories);
	return AcquireResult::Acquired;
}

} // namespace Core::FishGramClientGate

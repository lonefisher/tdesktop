/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_rtx_nr.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "ffmpeg/ffmpeg_utility.h"
#include "logs.h"
#include "media/streaming/media_streaming_rtx_vsr_plan.h"

#include <rpl/event_stream.h>

#include <algorithm>
#include <cstring>
#include <vector>

#if defined Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <d3d12.h>
#include <dxgi.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <wrl/client.h>

#include <map>
#include <string>

#ifdef small
#undef small
#endif // small
#endif // Q_OS_WIN

namespace Media::Streaming {
namespace {

rpl::event_stream<bool> &EnabledStream() {
	static auto stream = rpl::event_stream<bool>();
	return stream;
}

#if defined Q_OS_WIN

using Microsoft::WRL::ComPtr;

constexpr auto kNgxSuccess = 0x1u;
constexpr auto kNgxFail = 0xBAD00000u;

// The signed snippet only accepts a caller whose module name contains this.
constexpr wchar_t kCallerName[] = L"nvngx.dll";

using InitExtFn = uint32_t (*)(
	unsigned long long,
	const wchar_t *,
	ID3D12Device *,
	int,
	void *);
using CreateFeatureFn = uint32_t (*)(
	ID3D12GraphicsCommandList *,
	uint32_t,
	void *,
	void **);
using EvaluateFeatureFn = uint32_t (*)(
	ID3D12GraphicsCommandList *,
	const void *,
	const void *,
	void *);
using ReleaseFeatureFn = uint32_t (*)(void *);
using ShutdownFn = uint32_t (*)(ID3D12Device *);

struct ParameterSlot {
	enum class Kind {
		None,
		Pointer,
		Int,
		UInt,
		Double,
		Float,
		UInt64,
	};
	Kind kind = Kind::None;
	void *pointer = nullptr;
	int integer = 0;
	unsigned uinteger = 0;
	double real = 0.;
	float single = 0.f;
	unsigned long long wide = 0;
};

struct Parameter {
	const void *vtable = nullptr;
	std::map<std::string, ParameterSlot> values;

	void setPointer(const char *name, void *value) {
		auto &slot = values[name];
		slot = {};
		slot.kind = ParameterSlot::Kind::Pointer;
		slot.pointer = value;
	}
	void setInt(const char *name, int value) {
		auto &slot = values[name];
		slot = {};
		slot.kind = ParameterSlot::Kind::Int;
		slot.integer = value;
	}
	void setUInt(const char *name, unsigned value) {
		auto &slot = values[name];
		slot = {};
		slot.kind = ParameterSlot::Kind::UInt;
		slot.uinteger = value;
	}
	void setDouble(const char *name, double value) {
		auto &slot = values[name];
		slot = {};
		slot.kind = ParameterSlot::Kind::Double;
		slot.real = value;
	}
	void setFloat(const char *name, float value) {
		auto &slot = values[name];
		slot = {};
		slot.kind = ParameterSlot::Kind::Float;
		slot.single = value;
	}
	void setUInt64(const char *name, unsigned long long value) {
		auto &slot = values[name];
		slot = {};
		slot.kind = ParameterSlot::Kind::UInt64;
		slot.wide = value;
	}
};

Parameter *AsParameter(void *self) {
	return static_cast<Parameter *>(self);
}

void SetPointer(void *self, const char *name, void *value) {
	if (self && name) {
		AsParameter(self)->setPointer(name, value);
	}
}
void SetResource(void *self, const char *name, void *value) {
	SetPointer(self, name, value);
}
void SetInt(void *self, const char *name, int value) {
	if (self && name) {
		AsParameter(self)->setInt(name, value);
	}
}
void SetUInt(void *self, const char *name, unsigned value) {
	if (self && name) {
		AsParameter(self)->setUInt(name, value);
	}
}
void SetDouble(void *self, const char *name, double value) {
	if (self && name) {
		AsParameter(self)->setDouble(name, value);
	}
}
void SetFloat(void *self, const char *name, float value) {
	if (self && name) {
		AsParameter(self)->setFloat(name, value);
	}
}
void SetUInt64(void *self, const char *name, unsigned long long value) {
	if (self && name) {
		AsParameter(self)->setUInt64(name, value);
	}
}

uint32_t CopyOut(const ParameterSlot &slot, auto *out, auto member) {
	if (!out || slot.kind == ParameterSlot::Kind::None) {
		return kNgxFail;
	}
	*out = member;
	return kNgxSuccess;
}

uint32_t GetPointer(void *self, const char *name, void **out) {
	if (!self || !name) {
		return kNgxFail;
	}
	const auto found = AsParameter(self)->values.find(name);
	if (found == AsParameter(self)->values.end()) {
		return kNgxFail;
	}
	return CopyOut(found->second, out, found->second.pointer);
}
uint32_t GetResource(void *self, const char *name, void **out) {
	return GetPointer(self, name, out);
}
uint32_t GetInt(void *self, const char *name, int *out) {
	if (!self || !name) {
		return kNgxFail;
	}
	const auto found = AsParameter(self)->values.find(name);
	if (found == AsParameter(self)->values.end()) {
		return kNgxFail;
	}
	return CopyOut(found->second, out, found->second.integer);
}
uint32_t GetUInt(void *self, const char *name, unsigned *out) {
	if (!self || !name) {
		return kNgxFail;
	}
	const auto found = AsParameter(self)->values.find(name);
	if (found == AsParameter(self)->values.end()) {
		return kNgxFail;
	}
	return CopyOut(found->second, out, found->second.uinteger);
}
uint32_t GetDouble(void *self, const char *name, double *out) {
	if (!self || !name) {
		return kNgxFail;
	}
	const auto found = AsParameter(self)->values.find(name);
	if (found == AsParameter(self)->values.end()) {
		return kNgxFail;
	}
	return CopyOut(found->second, out, found->second.real);
}
uint32_t GetFloat(void *self, const char *name, float *out) {
	if (!self || !name) {
		return kNgxFail;
	}
	const auto found = AsParameter(self)->values.find(name);
	if (found == AsParameter(self)->values.end()) {
		return kNgxFail;
	}
	return CopyOut(found->second, out, found->second.single);
}
uint32_t GetUInt64(void *self, const char *name, unsigned long long *out) {
	if (!self || !name) {
		return kNgxFail;
	}
	const auto found = AsParameter(self)->values.find(name);
	if (found == AsParameter(self)->values.end()) {
		return kNgxFail;
	}
	return CopyOut(found->second, out, found->second.wide);
}
void ResetParameter(void *self) {
	if (self) {
		AsParameter(self)->values.clear();
	}
}

// Measured Feature 18 slot order: Set void*, D3D12, D3D11, int, uint,
// double, float, uint64, then the matching Get slots, then Reset.
const void *kParameterVtable[] = {
	reinterpret_cast<const void *>(&SetPointer),
	reinterpret_cast<const void *>(&SetResource),
	reinterpret_cast<const void *>(&SetResource),
	reinterpret_cast<const void *>(&SetInt),
	reinterpret_cast<const void *>(&SetUInt),
	reinterpret_cast<const void *>(&SetDouble),
	reinterpret_cast<const void *>(&SetFloat),
	reinterpret_cast<const void *>(&SetUInt64),
	reinterpret_cast<const void *>(&GetPointer),
	reinterpret_cast<const void *>(&GetResource),
	reinterpret_cast<const void *>(&GetResource),
	reinterpret_cast<const void *>(&GetInt),
	reinterpret_cast<const void *>(&GetUInt),
	reinterpret_cast<const void *>(&GetDouble),
	reinterpret_cast<const void *>(&GetFloat),
	reinterpret_cast<const void *>(&GetUInt64),
	reinterpret_cast<const void *>(&ResetParameter),
};

using SetFloatFn = void (*)(void *, const char *, float);

uint32_t ScalingCallback(void *parameters) {
	if (!parameters) {
		return kNgxFail;
	}
	const auto vtable = *static_cast<void ***>(parameters);
	if (!vtable || !vtable[6]) {
		return kNgxFail;
	}
	reinterpret_cast<SetFloatFn>(vtable[6])(
		parameters,
		"DLSSNR.ScalingRatio",
		1.f);
	return kNgxSuccess;
}

uint32_t GuardInit(
		InitExtFn fn,
		unsigned long long appId,
		const wchar_t *directory,
		ID3D12Device *device) {
	auto code = kNgxFail;
	__try {
		code = fn(appId, directory, device, kRtxDlssNrApiVersion, nullptr);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		code = kNgxFail;
	}
	return code;
}

uint32_t GuardCreate(
		CreateFeatureFn fn,
		ID3D12GraphicsCommandList *list,
		void *parameters,
		void **handle) {
	auto code = kNgxFail;
	__try {
		code = fn(list, kRtxDlssNrFeatureId, parameters, handle);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		code = kNgxFail;
	}
	return code;
}

uint32_t GuardEvaluate(
		EvaluateFeatureFn fn,
		ID3D12GraphicsCommandList *list,
		const void *handle,
		const void *parameters) {
	auto code = kNgxFail;
	__try {
		code = fn(list, handle, parameters, nullptr);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		code = kNgxFail;
	}
	return code;
}

uint32_t GuardRelease(ReleaseFeatureFn fn, void *handle) {
	auto code = kNgxFail;
	__try {
		code = fn(handle);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		code = kNgxFail;
	}
	return code;
}

uint32_t GuardShutdown(ShutdownFn fn, ID3D12Device *device) {
	auto code = kNgxFail;
	__try {
		code = fn(device);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		code = kNgxFail;
	}
	return code;
}

using GetModuleFileNameWFn = DWORD (WINAPI *)(HMODULE, LPWSTR, DWORD);

GetModuleFileNameWFn RealGetModuleFileNameW = nullptr;

DWORD WINAPI CallerName(HMODULE module, LPWSTR filename, DWORD size) {
	const auto exe = GetModuleHandleW(nullptr);
	if (module == nullptr || module == exe) {
		constexpr auto chars = DWORD(9);
		if (!size) {
			return 0;
		}
		if (size <= chars) {
			for (DWORD i = 0; i + 1 < size; ++i) {
				filename[i] = kCallerName[i];
			}
			filename[size - 1] = 0;
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return size;
		}
		for (DWORD i = 0; i <= chars; ++i) {
			filename[i] = kCallerName[i];
		}
		return chars;
	}
	if (!RealGetModuleFileNameW) {
		SetLastError(ERROR_INVALID_FUNCTION);
		return 0;
	}
	return RealGetModuleFileNameW(module, filename, size);
}

void **FindImport(HMODULE module, const char *function) {
	auto *base = reinterpret_cast<unsigned char *>(module);
	const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
		return nullptr;
	}
	const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(
		base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) {
		return nullptr;
	}
	const auto &directory = nt->OptionalHeader.DataDirectory[
		IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!directory.VirtualAddress) {
		return nullptr;
	}
	auto *descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(
		base + directory.VirtualAddress);
	for (; descriptor->Name; ++descriptor) {
		const auto *dll = reinterpret_cast<const char *>(
			base + descriptor->Name);
		const auto kernel = !_stricmp(dll, "kernel32.dll");
		const auto api = !_strnicmp(dll, "api-ms-", 7)
			|| !_strnicmp(dll, "ext-ms-", 7);
		if ((!kernel && !api)
			|| !descriptor->OriginalFirstThunk
			|| !descriptor->FirstThunk) {
			continue;
		}
		const auto *lookup = reinterpret_cast<const IMAGE_THUNK_DATA *>(
			base + descriptor->OriginalFirstThunk);
		auto *iat = reinterpret_cast<IMAGE_THUNK_DATA *>(
			base + descriptor->FirstThunk);
		for (auto i = 0; lookup[i].u1.AddressOfData; ++i) {
#if defined _WIN64
			if (IMAGE_SNAP_BY_ORDINAL64(lookup[i].u1.Ordinal)) {
#else
			if (IMAGE_SNAP_BY_ORDINAL(lookup[i].u1.Ordinal)) {
#endif
				continue;
			}
			const auto *named = reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(
				base + lookup[i].u1.AddressOfData);
			if (!std::strcmp(named->Name, function)) {
				return reinterpret_cast<void **>(&iat[i].u1.Function);
			}
		}
	}
	return nullptr;
}

bool PatchCallerName(HMODULE module) {
	auto *slot = FindImport(module, "GetModuleFileNameW");
	if (!slot || !*slot) {
		return false;
	}
	RealGetModuleFileNameW = reinterpret_cast<GetModuleFileNameWFn>(*slot);
	DWORD old = 0;
	if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) {
		return false;
	}
	InterlockedExchangePointer(slot, reinterpret_cast<void *>(&CallerName));
	DWORD restored = 0;
	VirtualProtect(slot, sizeof(void *), old, &restored);
	FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));
	return true;
}

bool FileExists(const std::wstring &path) {
	const auto attributes = GetFileAttributesW(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES
		&& !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring FullPath(const std::wstring &path) {
	wchar_t buffer[MAX_PATH * 4] = {};
	const auto length = GetFullPathNameW(
		path.c_str(),
		DWORD(std::size(buffer)),
		buffer,
		nullptr);
	if (!length || length >= std::size(buffer)) {
		return path;
	}
	return buffer;
}

std::wstring ParentPath(std::wstring path) {
	const auto slash = path.find_last_of(L"\\/");
	if (slash == std::wstring::npos) {
		return {};
	}
	path.resize(slash);
	return path;
}

bool EndsWithDll(const std::wstring &path) {
	const auto wide = std::wstring(
		kRtxDlssNrDllName,
		kRtxDlssNrDllName + std::strlen(kRtxDlssNrDllName));
	return path.size() > wide.size()
		&& !_wcsicmp(
			path.c_str() + (path.size() - wide.size()),
			wide.c_str());
}

std::wstring DllIn(const std::wstring &directoryOrFile) {
	const auto full = FullPath(directoryOrFile);
	if (EndsWithDll(full) && FileExists(full)) {
		return full;
	}
	auto candidate = full;
	if (!candidate.empty() && candidate.back() != L'\\' && candidate.back() != L'/') {
		candidate.push_back(L'\\');
	}
	const auto wide = std::wstring(
		kRtxDlssNrDllName,
		kRtxDlssNrDllName + std::strlen(kRtxDlssNrDllName));
	candidate.append(wide);
	return FileExists(candidate) ? FullPath(candidate) : std::wstring();
}

std::wstring ExeDirectory() {
	wchar_t buffer[MAX_PATH * 4] = {};
	const auto length = GetModuleFileNameW(
		nullptr,
		buffer,
		DWORD(std::size(buffer)));
	if (!length || length >= std::size(buffer)) {
		return {};
	}
	return ParentPath(buffer);
}

struct RuntimeLocation {
	RtxNrPathKind kind = RtxNrPathKind::None;
	std::wstring dll;
	std::wstring directory;
};

RuntimeLocation LocateRuntime() {
	wchar_t env[MAX_PATH * 4] = {};
	const auto envLength = GetEnvironmentVariableW(
		L"FISHGRAM_NGX_RUNTIME",
		env,
		DWORD(std::size(env)));
	const auto envSet = envLength > 0 && envLength < std::size(env);
	const auto envDll = envSet ? DllIn(env) : std::wstring();
	const auto beside = ExeDirectory()
		+ L"\\ngx\\"
		+ std::wstring(kRtxDlssNrDllName, kRtxDlssNrDllName + std::strlen(kRtxDlssNrDllName));
	const auto kind = ChooseRtxNrRuntime({
		.environmentSet = envSet,
		.environmentHasDll = !envDll.empty(),
		.exeFolderHasDll = FileExists(beside),
	});
	auto location = RuntimeLocation();
	location.kind = kind;
	if (kind == RtxNrPathKind::Environment) {
		location.dll = envDll;
	} else if (kind == RtxNrPathKind::BesideExe) {
		location.dll = FullPath(beside);
	}
	location.directory = ParentPath(location.dll);
	return location;
}

RtxNrTrust TrustOf(const std::wstring &path) {
	WINTRUST_FILE_INFO file = {};
	file.cbStruct = sizeof(file);
	file.pcwszFilePath = path.c_str();
	WINTRUST_DATA data = {};
	data.cbStruct = sizeof(data);
	data.dwUIChoice = WTD_UI_NONE;
	data.fdwRevocationChecks = WTD_REVOKE_NONE;
	data.dwUnionChoice = WTD_CHOICE_FILE;
	data.pFile = &file;
	data.dwStateAction = WTD_STATEACTION_VERIFY;
	data.dwProvFlags = WTD_SAFER_FLAG | WTD_CACHE_ONLY_URL_RETRIEVAL;
	GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
	const auto status = WinVerifyTrust(
		reinterpret_cast<HWND>(INVALID_HANDLE_VALUE),
		&action,
		&data);
	wchar_t name[256] = {};
	if (auto *provider = WTHelperProvDataFromStateData(data.hWVTStateData)) {
		if (auto *signer = WTHelperGetProvSignerFromChain(
				provider,
				0,
				FALSE,
				0)) {
			if (auto *cert = WTHelperGetProvCertFromChain(signer, 0)) {
				CertGetNameStringW(
					cert->pCert,
					CERT_NAME_SIMPLE_DISPLAY_TYPE,
					0,
					nullptr,
					name,
					UINT(std::size(name)));
			}
		}
	}
	data.dwStateAction = WTD_STATEACTION_CLOSE;
	WinVerifyTrust(
		reinterpret_cast<HWND>(INVALID_HANDLE_VALUE),
		&action,
		&data);
	char utf8[256] = {};
	WideCharToMultiByte(
		CP_UTF8,
		0,
		name,
		-1,
		utf8,
		int(std::size(utf8)),
		nullptr,
		nullptr);
	return JudgeRtxNrSignature(status == ERROR_SUCCESS, utf8);
}

bool VersionOf(const std::wstring &path, int parts[4]) {
	DWORD handle = 0;
	const auto bytes = GetFileVersionInfoSizeW(path.c_str(), &handle);
	if (!bytes) {
		return false;
	}
	auto buffer = std::vector<unsigned char>(bytes);
	if (!GetFileVersionInfoW(path.c_str(), 0, bytes, buffer.data())) {
		return false;
	}
	void *fixed = nullptr;
	UINT length = 0;
	if (!VerQueryValueW(buffer.data(), L"\\", &fixed, &length)
		|| length < sizeof(VS_FIXEDFILEINFO)) {
		return false;
	}
	const auto *info = static_cast<const VS_FIXEDFILEINFO *>(fixed);
	parts[0] = HIWORD(info->dwFileVersionMS);
	parts[1] = LOWORD(info->dwFileVersionMS);
	parts[2] = HIWORD(info->dwFileVersionLS);
	parts[3] = LOWORD(info->dwFileVersionLS);
	return true;
}

bool DescriptionHasRtx(const DXGI_ADAPTER_DESC1 &desc) {
	const auto *text = desc.Description;
	for (auto i = 0; text[i]; ++i) {
		if ((text[i] == L'R' || text[i] == L'r')
			&& (text[i + 1] == L'T' || text[i + 1] == L't')
			&& (text[i + 2] == L'X' || text[i + 2] == L'x')) {
			return true;
		}
	}
	return false;
}

bool AdapterIsRtx() {
	if (GetSystemMetrics(SM_REMOTESESSION)) {
		return false;
	}
	ComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
		return false;
	}
	ComPtr<IDXGIAdapter1> adapter;
	for (UINT index = 0;
		factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND;
		++index) {
		DXGI_ADAPTER_DESC1 desc = {};
		if (FAILED(adapter->GetDesc1(&desc))) {
			adapter.Reset();
			continue;
		}
		const auto software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
		if (!software
			&& desc.VendorId == kRtxNvidiaVendorId
			&& DescriptionHasRtx(desc)) {
			return true;
		}
		adapter.Reset();
	}
	return false;
}

QImage ImageFromFrame(const FrameWithInfo &frame) {
	if (!frame.image.isNull()) {
		return (frame.image.format() == QImage::Format_ARGB32)
			? frame.image
			: frame.image.convertToFormat(QImage::Format_ARGB32);
	}
	if (!frame.yuv
		|| !frame.yuv->y.data
		|| !frame.yuv->u.data
		|| frame.yuv->size.isEmpty()) {
		return {};
	}
	const auto nv12 = (frame.format == FrameFormat::NV12);
	if (!nv12 && !frame.yuv->v.data) {
		return {};
	}
	auto result = FFmpeg::CreateFrameStorage(frame.yuv->size);
	const auto scale = FFmpeg::MakeSwscalePointer(
		frame.yuv->size,
		nv12 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P,
		frame.yuv->size,
		AV_PIX_FMT_BGRA);
	if (result.isNull() || !scale) {
		return {};
	}
	const uint8_t *srcData[4] = {
		static_cast<const uint8_t *>(frame.yuv->y.data),
		static_cast<const uint8_t *>(frame.yuv->u.data),
		nv12
			? nullptr
			: static_cast<const uint8_t *>(frame.yuv->v.data),
		nullptr,
	};
	int srcStride[4] = {
		frame.yuv->y.stride,
		frame.yuv->u.stride,
		nv12 ? 0 : frame.yuv->v.stride,
		0,
	};
	uint8_t *dstData[4] = { result.bits(), nullptr, nullptr, nullptr };
	int dstStride[4] = { int(result.bytesPerLine()), 0, 0, 0 };
	sws_scale(
		scale.get(),
		srcData,
		srcStride,
		0,
		frame.yuv->size.height(),
		dstData,
		dstStride);
	return result;
}

int SampleMax(const unsigned char *data, int width, int height, int stride) {
	if (!data || width < 1 || height < 1 || stride < width * 4) {
		return 0;
	}
	auto maxValue = 0;
	const auto stepX = (width > 8) ? (width / 8) : 1;
	const auto stepY = (height > 8) ? (height / 8) : 1;
	for (auto y = 0; y < height; y += stepY) {
		const auto row = data + (std::size_t(y) * stride);
		for (auto x = 0; x < width; x += stepX) {
			const auto pixel = row + (std::size_t(x) * 4);
			for (auto channel = 0; channel != 3; ++channel) {
				maxValue = std::max(maxValue, int(pixel[channel]));
			}
		}
	}
	return maxValue;
}

class Engine {
public:
	static Engine &Instance() {
		static Engine engine;
		return engine;
	}

	[[nodiscard]] bool available() {
		refreshIdentity();
		return _rtx && _kind != RtxNrPathKind::None && _trust == RtxNrTrust::Ok
			&& _versionOk && !_blocked;
	}

	void noteToggled(bool enabled) {
		_blocked = false;
		_failures = 0;
		_logged = 0;
		_loggedActive = false;
		if (enabled) {
			RtxNrBumpEpoch(_clock);
		}
	}

	void noteDiscontinuity() {
		RtxNrBumpEpoch(_clock);
	}

	void noteNewClip() {
		noteDiscontinuity();
		_logged = 0;
		_loggedActive = false;
	}

	[[nodiscard]] QImage apply(
			const FrameWithInfo &frame,
			bool fullscreen,
			int windowLongEdge) {
		if (!onOwnerThread() || frame.index < 0) {
			return {};
		}
		refreshIdentity();
		const auto early = PlanRtxDlssNr({
			.enabled = true,
			.rtx = _rtx,
			.surface = RtxNrSurface::MediaViewer,
			.fullscreen = fullscreen,
			.windowLongEdge = windowLongEdge,
			.runtimeFile = (_kind != RtxNrPathKind::None),
			.trust = _trust,
			.versionOk = _versionOk,
			.width = kRtxDlssNrMinEdge,
			.height = kRtxDlssNrMinEdge,
			.alpha = frame.alpha,
			.blocked = _blocked,
		});
		if (early != RtxNrSkipReason::None) {
			logSkip(early);
			return {};
		}
		if (_cache.valid
			&& _cache.index == frame.index
			&& _cache.epoch == _clock.epoch) {
			return _cache.image;
		}
		const auto source = ImageFromFrame(frame);
		const auto size = source.isNull()
			? QSize()
			: source.size();
		const auto reason = PlanRtxDlssNr({
			.enabled = true,
			.rtx = _rtx,
			.surface = RtxNrSurface::MediaViewer,
			.fullscreen = fullscreen,
			.windowLongEdge = windowLongEdge,
			.runtimeFile = (_kind != RtxNrPathKind::None),
			.trust = _trust,
			.versionOk = _versionOk,
			.width = size.width(),
			.height = size.height(),
			.alpha = frame.alpha,
			.blocked = _blocked,
		});
		if (reason != RtxNrSkipReason::None) {
			logSkip(reason);
			return {};
		}
		auto image = evaluate(source);
		if (image.isNull()) {
			remember(frame, size, {});
			return {};
		}
		if (!_loggedActive) {
			_loggedActive = true;
			LOG(("DLSS NR Info: NVIDIA DLSS NR is active."));
		}
		remember(frame, size, image);
		return image;
	}

private:
	Engine() {
		_parameters.vtable = kParameterVtable;
	}

	struct Cache {
		bool valid = false;
		int index = -1;
		std::uint64_t epoch = 0;
		int width = 0;
		int height = 0;
		QImage image;
	};

	[[nodiscard]] bool onOwnerThread() {
		const auto now = GetCurrentThreadId();
		if (!_thread) {
			_thread = now;
			return true;
		}
		return _thread == now;
	}

	void refreshIdentity() {
		const auto located = LocateRuntime();
		if (located.dll == _dll) {
			if (!_rtxProbed) {
				_rtx = AdapterIsRtx();
				_rtxProbed = true;
			}
			return;
		}
		releaseAll();
		_dll = located.dll;
		_directory = located.directory;
		_kind = located.kind;
		_trust = RtxNrTrust::Missing;
		_versionOk = false;
		_versionParts[0] = _versionParts[1] = _versionParts[2] = _versionParts[3] = 0;
		_logged = 0;
		if (located.kind == RtxNrPathKind::None) {
			return;
		}
		_trust = TrustOf(_dll);
		_versionOk = VersionOf(_dll, _versionParts)
			&& RtxNrVersionOk(
				_versionParts[0],
				_versionParts[1],
				_versionParts[2],
				_versionParts[3]);
		if (!_rtxProbed) {
			_rtx = AdapterIsRtx();
			_rtxProbed = true;
		}
	}

	void logSkip(RtxNrSkipReason reason) {
		const auto bit = 1u << int(reason);
		if (_logged & bit) {
			return;
		}
		_logged |= bit;
		switch (reason) {
		case RtxNrSkipReason::RuntimeMissing:
			LOG(("DLSS NR Debug: skip reason=runtime missing."));
			break;
		case RtxNrSkipReason::Signature:
			LOG(("DLSS NR Error: signature rejected."));
			break;
		case RtxNrSkipReason::Version:
			LOG(("DLSS NR Error: version %1.%2.%3.%4 rejected."
				).arg(_versionParts[0]
				).arg(_versionParts[1]
				).arg(_versionParts[2]
				).arg(_versionParts[3]));
			break;
		case RtxNrSkipReason::Device:
			LOG(("DLSS NR Debug: skip reason=device."));
			break;
		case RtxNrSkipReason::Window:
			LOG(("DLSS NR Debug: skip reason=window."));
			break;
		case RtxNrSkipReason::Size:
			LOG(("DLSS NR Debug: skip reason=size."));
			break;
		case RtxNrSkipReason::Create:
		case RtxNrSkipReason::Blocked:
			LOG(("DLSS NR Error: feature creation failed."));
			break;
		default:
			break;
		}
	}

	void remember(const FrameWithInfo &frame, QSize size, QImage image) {
		_cache.valid = true;
		_cache.index = frame.index;
		_cache.epoch = _clock.epoch;
		_cache.width = size.width();
		_cache.height = size.height();
		_cache.image = std::move(image);
	}

	void failCreate(uint32_t code) {
		const auto budget = NoteRtxNrCreateFailure({
			.failures = _failures,
			.blocked = _blocked,
		});
		_failures = budget.failures;
		_blocked = budget.blocked;
		RtxNrBumpEpoch(_clock);
		const auto bit = 1u << int(RtxNrSkipReason::Create);
		if (_logged & bit) {
			return;
		}
		_logged |= bit;
		LOG(("DLSS NR Error: feature creation failed, code %1."
			).arg(uint32_t(code), 8, 16, QChar('0')));
	}

	bool ensureDevice() {
		if (_device) {
			return true;
		}
		ComPtr<IDXGIFactory1> factory;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
			failCreate(kNgxFail);
			return false;
		}
		ComPtr<IDXGIAdapter1> adapter;
		ComPtr<IDXGIAdapter1> chosen;
		for (UINT index = 0;
			factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND;
			++index) {
			DXGI_ADAPTER_DESC1 desc = {};
			if (SUCCEEDED(adapter->GetDesc1(&desc))
				&& !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
				&& desc.VendorId == kRtxNvidiaVendorId
				&& DescriptionHasRtx(desc)) {
				chosen = adapter;
				break;
			}
			adapter.Reset();
		}
		if (!chosen
			|| FAILED(D3D12CreateDevice(
				chosen.Get(),
				D3D_FEATURE_LEVEL_11_0,
				IID_PPV_ARGS(&_device)))) {
			failCreate(kNgxFail);
			releaseDevice();
			return false;
		}
		D3D12_COMMAND_QUEUE_DESC queue = {};
		queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (FAILED(_device->CreateCommandQueue(
				&queue,
				IID_PPV_ARGS(&_queue)))
			|| FAILED(_device->CreateCommandAllocator(
				D3D12_COMMAND_LIST_TYPE_DIRECT,
				IID_PPV_ARGS(&_allocator)))
			|| FAILED(_device->CreateCommandList(
				0,
				D3D12_COMMAND_LIST_TYPE_DIRECT,
				_allocator.Get(),
				nullptr,
				IID_PPV_ARGS(&_list)))
			|| FAILED(_device->CreateFence(
				0,
				D3D12_FENCE_FLAG_NONE,
				IID_PPV_ARGS(&_fence)))) {
			failCreate(kNgxFail);
			releaseDevice();
			return false;
		}
		_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!_fenceEvent) {
			failCreate(kNgxFail);
			releaseDevice();
			return false;
		}
		if (!loadSnippet()) {
			failCreate(kNgxFail);
			releaseDevice();
			return false;
		}
		const auto init = GuardInit(
			_init,
			kRtxDlssNrAppId,
			_directory.c_str(),
			_device.Get());
		if (init != kNgxSuccess) {
			failCreate(init);
			releaseDevice();
			return false;
		}
		_inited = true;
		return true;
	}

	bool loadSnippet() {
		if (_module) {
			return _init && _create && _evaluate && _release && _shutdown;
		}
		LoadLibraryW(L"nvapi64.dll");
		_module = LoadLibraryExW(
			_dll.c_str(),
			nullptr,
			LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (!_module) {
			return false;
		}
		if (!PatchCallerName(_module) && !_loggedCaller) {
			_loggedCaller = true;
			LOG(("DLSS NR Debug: skip reason=caller."));
		}
		_init = reinterpret_cast<InitExtFn>(
			GetProcAddress(_module, "NVSDK_NGX_D3D12_Init_Ext"));
		_create = reinterpret_cast<CreateFeatureFn>(
			GetProcAddress(_module, "NVSDK_NGX_D3D12_CreateFeature"));
		_evaluate = reinterpret_cast<EvaluateFeatureFn>(
			GetProcAddress(_module, "NVSDK_NGX_D3D12_EvaluateFeature"));
		_release = reinterpret_cast<ReleaseFeatureFn>(
			GetProcAddress(_module, "NVSDK_NGX_D3D12_ReleaseFeature"));
		_shutdown = reinterpret_cast<ShutdownFn>(
			GetProcAddress(_module, "NVSDK_NGX_D3D12_Shutdown1"));
		if (!_init || !_create || !_evaluate || !_release || !_shutdown) {
			return false;
		}
		return true;
	}

	bool submit() {
		if (FAILED(_list->Close())) {
			return false;
		}
		ID3D12CommandList *lists[] = { _list.Get() };
		_queue->ExecuteCommandLists(1, lists);
		const auto value = ++_fenceValue;
		if (FAILED(_queue->Signal(_fence.Get(), value))) {
			return false;
		}
		if (_fence->GetCompletedValue() < value) {
			if (FAILED(_fence->SetEventOnCompletion(value, _fenceEvent))) {
				return false;
			}
			if (WaitForSingleObject(_fenceEvent, 2000) != WAIT_OBJECT_0) {
				return false;
			}
		}
		return SUCCEEDED(_allocator->Reset()) && SUCCEEDED(_list->Reset(
			_allocator.Get(),
			nullptr));
	}

	bool transition(
			ID3D12Resource *resource,
			D3D12_RESOURCE_STATES before,
			D3D12_RESOURCE_STATES after) {
		if (!resource || before == after) {
			return true;
		}
		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = resource;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = before;
		barrier.Transition.StateAfter = after;
		_list->ResourceBarrier(1, &barrier);
		return true;
	}

	struct Texture {
		ComPtr<ID3D12Resource> gpu;
		ComPtr<ID3D12Resource> transfer;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
		D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
		int width = 0;
		int height = 0;
	};

	bool makeTexture(
			Texture &texture,
			int width,
			int height,
			DXGI_FORMAT format,
			D3D12_RESOURCE_FLAGS flags,
			bool readback) {
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = UINT64(width);
		desc.Height = UINT(height);
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Flags = flags;
		D3D12_HEAP_PROPERTIES heapDefault = {};
		heapDefault.Type = D3D12_HEAP_TYPE_DEFAULT;
		if (FAILED(_device->CreateCommittedResource(
				&heapDefault,
				D3D12_HEAP_FLAG_NONE,
				&desc,
				D3D12_RESOURCE_STATE_COMMON,
				nullptr,
				IID_PPV_ARGS(&texture.gpu)))) {
			return false;
		}
		UINT64 total = 0;
		_device->GetCopyableFootprints(
			&desc,
			0,
			1,
			0,
			&texture.footprint,
			nullptr,
			nullptr,
			&total);
		D3D12_HEAP_PROPERTIES heapCopy = {};
		heapCopy.Type = readback
			? D3D12_HEAP_TYPE_READBACK
			: D3D12_HEAP_TYPE_UPLOAD;
		D3D12_RESOURCE_DESC buffer = {};
		buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		buffer.Width = total;
		buffer.Height = 1;
		buffer.DepthOrArraySize = 1;
		buffer.MipLevels = 1;
		buffer.Format = DXGI_FORMAT_UNKNOWN;
		buffer.SampleDesc.Count = 1;
		buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(_device->CreateCommittedResource(
				&heapCopy,
				D3D12_HEAP_FLAG_NONE,
				&buffer,
				readback
					? D3D12_RESOURCE_STATE_COPY_DEST
					: D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS(&texture.transfer)))) {
			return false;
		}
		texture.state = D3D12_RESOURCE_STATE_COMMON;
		texture.width = width;
		texture.height = height;
		return true;
	}

	bool upload(Texture &texture, const void *pixels, int stride, int pixelBytes) {
		void *mapped = nullptr;
		if (FAILED(texture.transfer->Map(0, nullptr, &mapped))) {
			return false;
		}
		auto *dst = static_cast<unsigned char *>(mapped)
			+ texture.footprint.Offset;
		const auto *src = static_cast<const unsigned char *>(pixels);
		const auto rowPitch = int(texture.footprint.Footprint.RowPitch);
		const auto rowBytes = texture.width * pixelBytes;
		for (auto y = 0; y != texture.height; ++y) {
			std::memcpy(dst + (std::size_t(y) * rowPitch), src + (std::size_t(y) * stride), rowBytes);
		}
		texture.transfer->Unmap(0, nullptr);
		if (!transition(
				texture.gpu.Get(),
				texture.state,
				D3D12_RESOURCE_STATE_COPY_DEST)) {
			return false;
		}
		D3D12_TEXTURE_COPY_LOCATION source = {};
		source.pResource = texture.transfer.Get();
		source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		source.PlacedFootprint = texture.footprint;
		D3D12_TEXTURE_COPY_LOCATION dest = {};
		dest.pResource = texture.gpu.Get();
		dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		_list->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
		texture.state = D3D12_RESOURCE_STATE_COPY_DEST;
		return true;
	}

	void bindSize(int width, int height) {
		_parameters.setUInt("DLSSNR.Width", unsigned(width));
		_parameters.setUInt("DLSSNR.Height", unsigned(height));
		_parameters.setUInt("DLSSNR.InputWidth", unsigned(width));
		_parameters.setUInt("DLSSNR.InputHeight", unsigned(height));
		_parameters.setUInt("DLSSNR.OutputWidth", unsigned(width));
		_parameters.setUInt("DLSSNR.OutputHeight", unsigned(height));
		_parameters.setUInt("DLSSNR.Output.Width", unsigned(width));
		_parameters.setUInt("DLSSNR.Output.Height", unsigned(height));
		_parameters.setUInt("Width", unsigned(width));
		_parameters.setUInt("Height", unsigned(height));
		_parameters.setUInt("DLSSNR.Upscaling", 0);
		_parameters.setFloat("DLSSNR.Scale", 1.f);
		_parameters.setFloat("DLSSNR.ScalingRatio", 1.f);
		_parameters.setPointer(
			"DLSSNRComputeScalingRatioCallback",
			reinterpret_cast<void *>(&ScalingCallback));
		_parameters.setInt("DLSSNR.Hint.Render.Preset", 1);
		_parameters.setInt("PerfQualityValue", 1);
		_parameters.setUInt("CreationNodeMask", 1);
		_parameters.setUInt("VisibilityNodeMask", 1);
	}

	bool ensureFeature(int width, int height) {
		if (_handle && _width == width && _height == height) {
			return true;
		}
		if (_handle && _release) {
			GuardRelease(_release, _handle);
			_handle = nullptr;
		}
		_color = {};
		_output = {};
		_motion = {};
		_depth = {};
		if (!makeTexture(
				_color,
				width,
				height,
				DXGI_FORMAT_R8G8B8A8_UNORM,
				D3D12_RESOURCE_FLAG_NONE,
				false)
			|| !makeTexture(
				_output,
				width,
				height,
				DXGI_FORMAT_R8G8B8A8_UNORM,
				D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
				true)
			|| !makeTexture(
				_motion,
				width,
				height,
				DXGI_FORMAT_R16G16_FLOAT,
				D3D12_RESOURCE_FLAG_NONE,
				false)
			|| !makeTexture(
				_depth,
				width,
				height,
				DXGI_FORMAT_R32_FLOAT,
				D3D12_RESOURCE_FLAG_NONE,
				false)) {
			releaseDevice();
			return false;
		}
		const auto zeros = std::vector<unsigned char>(
			std::size_t(width) * height * 4);
		if (!upload(_motion, zeros.data(), width * 4, 4)
			|| !upload(_depth, zeros.data(), width * 4, 4)
			|| !transition(
				_motion.gpu.Get(),
				_motion.state,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
			|| !transition(
				_depth.gpu.Get(),
				_depth.state,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
			|| !submit()) {
			releaseDevice();
			return false;
		}
		_motion.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		_depth.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		bindSize(width, height);
		void *created = nullptr;
		const auto code = GuardCreate(
			_create,
			_list.Get(),
			&_parameters,
			&created);
		_handle = created;
		if (!submit() || code != kNgxSuccess || !created) {
			failCreate(code ? code : kNgxFail);
			releaseDevice();
			return false;
		}
		_width = width;
		_height = height;
		RtxNrBumpEpoch(_clock);
		return true;
	}

	void bindEvaluate(int width, int height, bool reset) {
		_parameters.setPointer("DLSSNR.Color", _color.gpu.Get());
		_parameters.setPointer("DLSSNR.Output", _output.gpu.Get());
		_parameters.setPointer("DLSSNR.MVec", _motion.gpu.Get());
		_parameters.setPointer("DLSSNR.Depth", _depth.gpu.Get());
		_parameters.setUInt("DLSSNR.ColorSubrectBaseX", 0);
		_parameters.setUInt("DLSSNR.ColorSubrectBaseY", 0);
		_parameters.setUInt("DLSSNR.ColorSubrectWidth", unsigned(width));
		_parameters.setUInt("DLSSNR.ColorSubrectHeight", unsigned(height));
		_parameters.setUInt("DLSSNR.OutputSubrectBaseX", 0);
		_parameters.setUInt("DLSSNR.OutputSubrectBaseY", 0);
		_parameters.setUInt("DLSSNR.OutputSubrectWidth", unsigned(width));
		_parameters.setUInt("DLSSNR.OutputSubrectHeight", unsigned(height));
		_parameters.setUInt("DLSSNR.MVecSubrectBaseX", 0);
		_parameters.setUInt("DLSSNR.MVecSubrectBaseY", 0);
		_parameters.setUInt("DLSSNR.MVecSubrectWidth", unsigned(width));
		_parameters.setUInt("DLSSNR.MVecSubrectHeight", unsigned(height));
		_parameters.setUInt("DLSSNR.DepthSubrectBaseX", 0);
		_parameters.setUInt("DLSSNR.DepthSubrectBaseY", 0);
		_parameters.setUInt("DLSSNR.DepthSubrectWidth", unsigned(width));
		_parameters.setUInt("DLSSNR.DepthSubrectHeight", unsigned(height));
		_parameters.setFloat("DLSSNR.MVecScaleX", 1.f);
		_parameters.setFloat("DLSSNR.MVecScaleY", 1.f);
		_parameters.setInt("DLSSNR.DepthInverted", 1);
		_parameters.setInt("DLSS.Indicator.Invert.X.Axis", 0);
		_parameters.setInt("DLSS.Indicator.Invert.Y.Axis", 0);
		_parameters.setInt("DLSSNR.Enabled", 1);
		_parameters.setInt("DLSSNR.Reset", reset ? 1 : 0);
		_parameters.setInt("DLSSNR.Style", 0);
		_parameters.setFloat("DLSSNR.Intensity", 1.f);
		_parameters.setFloat("DLSSNR.LocalToneStrength", 1.f);
		_parameters.setFloat("DLSSNR.LocalStructureStrength", 1.f);
		_parameters.setFloat("DLSSNR.SkinStructureStrength", 0.f);
		_parameters.setInt("DLSSNR.UseAutoMask", 1);
		_parameters.setInt("DLSSNR.UICorrection", 0);
		_parameters.setPointer(
			"DLSSNRComputeScalingRatioCallback",
			reinterpret_cast<void *>(&ScalingCallback));
	}

	[[nodiscard]] QImage evaluate(const QImage &source) {
		const auto width = source.width();
		const auto height = source.height();
		if (!ensureDevice() || !ensureFeature(width, height)) {
			return {};
		}
		auto packed = std::vector<unsigned char>(std::size_t(width) * height * 4);
		const auto sourceMax = SampleMax(
			source.constBits(),
			width,
			height,
			source.bytesPerLine());
		for (auto y = 0; y != height; ++y) {
			const auto *row = source.constBits() + (std::size_t(y) * source.bytesPerLine());
			auto *dst = packed.data() + (std::size_t(y) * width * 4);
			for (auto x = 0; x != width; ++x) {
				dst[x * 4 + 0] = row[x * 4 + 2];
				dst[x * 4 + 1] = row[x * 4 + 1];
				dst[x * 4 + 2] = row[x * 4 + 0];
				dst[x * 4 + 3] = 255;
			}
		}
		if (!upload(_color, packed.data(), width * 4, 4)
			|| !transition(
				_color.gpu.Get(),
				_color.state,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
			|| !submit()) {
			failCreate(kNgxFail);
			releaseDevice();
			return {};
		}
		_color.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		if (!transition(
				_output.gpu.Get(),
				_output.state,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) {
			failCreate(kNgxFail);
			releaseDevice();
			return {};
		}
		_output.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		const auto reset = RtxNrTakeReset(_clock);
		bindEvaluate(width, height, reset);
		const auto code = GuardEvaluate(
			_evaluate,
			_list.Get(),
			_handle,
			&_parameters);
		if (code != kNgxSuccess
			|| !transition(
				_output.gpu.Get(),
				_output.state,
				D3D12_RESOURCE_STATE_COPY_SOURCE)) {
			failCreate(code);
			releaseDevice();
			return {};
		}
		_output.state = D3D12_RESOURCE_STATE_COPY_SOURCE;
		D3D12_TEXTURE_COPY_LOCATION sourceLocation = {};
		sourceLocation.pResource = _output.gpu.Get();
		sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		D3D12_TEXTURE_COPY_LOCATION destLocation = {};
		destLocation.pResource = _output.transfer.Get();
		destLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		destLocation.PlacedFootprint = _output.footprint;
		_list->CopyTextureRegion(&destLocation, 0, 0, 0, &sourceLocation, nullptr);
		if (!submit()) {
			failCreate(kNgxFail);
			releaseDevice();
			return {};
		}
		void *mapped = nullptr;
		if (FAILED(_output.transfer->Map(0, nullptr, &mapped))) {
			failCreate(kNgxFail);
			releaseDevice();
			return {};
		}
		auto image = QImage(width, height, QImage::Format_ARGB32);
		const auto *src = static_cast<const unsigned char *>(mapped)
			+ _output.footprint.Offset;
		const auto rowPitch = int(_output.footprint.Footprint.RowPitch);
		for (auto y = 0; y != height; ++y) {
			const auto *row = src + (std::size_t(y) * rowPitch);
			auto *dst = image.bits() + (std::size_t(y) * image.bytesPerLine());
			for (auto x = 0; x != width; ++x) {
				dst[x * 4 + 0] = row[x * 4 + 2];
				dst[x * 4 + 1] = row[x * 4 + 1];
				dst[x * 4 + 2] = row[x * 4 + 0];
				dst[x * 4 + 3] = 255;
			}
		}
		const auto outputMax = SampleMax(
			image.constBits(),
			width,
			height,
			image.bytesPerLine());
		_output.transfer->Unmap(0, nullptr);
		if ((sourceMax > 16) && (outputMax == 0)) {
			failCreate(kNgxFail);
			releaseDevice();
			return {};
		}
		_failures = 0;
		return image;
	}

	void releaseDevice() {
		if (_handle && _release) {
			GuardRelease(_release, _handle);
		}
		_handle = nullptr;
		if (_inited && _shutdown && _device) {
			GuardShutdown(_shutdown, _device.Get());
		}
		_inited = false;
		if (_fenceEvent) {
			CloseHandle(_fenceEvent);
			_fenceEvent = nullptr;
		}
		_list.Reset();
		_allocator.Reset();
		_queue.Reset();
		_fence.Reset();
		_device.Reset();
		_color = {};
		_output = {};
		_motion = {};
		_depth = {};
		_width = _height = 0;
	}

	void releaseAll() {
		releaseDevice();
		_init = nullptr;
		_create = nullptr;
		_evaluate = nullptr;
		_release = nullptr;
		_shutdown = nullptr;
		if (_module) {
			FreeLibrary(_module);
			_module = nullptr;
		}
	}

	DWORD _thread = 0;
	bool _rtx = false;
	bool _rtxProbed = false;
	bool _versionOk = false;
	bool _blocked = false;
	bool _inited = false;
	bool _loggedActive = false;
	bool _loggedCaller = false;
	int _failures = 0;
	unsigned _logged = 0;
	RtxNrPathKind _kind = RtxNrPathKind::None;
	RtxNrTrust _trust = RtxNrTrust::Missing;
	int _versionParts[4] = {};
	std::wstring _dll;
	std::wstring _directory;
	RtxNrClock _clock;
	Cache _cache;
	Parameter _parameters;
	HMODULE _module = nullptr;
	InitExtFn _init = nullptr;
	CreateFeatureFn _create = nullptr;
	EvaluateFeatureFn _evaluate = nullptr;
	ReleaseFeatureFn _release = nullptr;
	ShutdownFn _shutdown = nullptr;
	ComPtr<ID3D12Device> _device;
	ComPtr<ID3D12CommandQueue> _queue;
	ComPtr<ID3D12CommandAllocator> _allocator;
	ComPtr<ID3D12GraphicsCommandList> _list;
	ComPtr<ID3D12Fence> _fence;
	HANDLE _fenceEvent = nullptr;
	UINT64 _fenceValue = 0;
	void *_handle = nullptr;
	Texture _color;
	Texture _output;
	Texture _motion;
	Texture _depth;
	int _width = 0;
	int _height = 0;
};

#endif // Q_OS_WIN

} // namespace

bool RtxDlssNrSettingVisible() {
#if defined Q_OS_WIN
	return true;
#else
	return false;
#endif
}

bool RtxDlssNrEnabled() {
	return Core::App().settings().readPref<bool>(
		kRtxDlssNrPref,
		kRtxDlssNrDefault);
}

void RtxDlssNrSetEnabled(bool enabled) {
	Core::App().settings().writePref<bool>(kRtxDlssNrPref, enabled);
	Core::App().saveSettingsDelayed();
#if defined Q_OS_WIN
	Engine::Instance().noteToggled(enabled);
#endif
	EnabledStream().fire_copy(enabled);
}

rpl::producer<bool> RtxDlssNrEnabledValue() {
	return rpl::single(
		RtxDlssNrEnabled()
	) | rpl::then(EnabledStream().events());
}

bool RtxDlssNrAvailable() {
#if defined Q_OS_WIN
	return Engine::Instance().available();
#else
	return false;
#endif
}

void RtxDlssNrNoteDiscontinuity() {
#if defined Q_OS_WIN
	Engine::Instance().noteDiscontinuity();
#endif
}

void RtxDlssNrNoteNewClip() {
#if defined Q_OS_WIN
	Engine::Instance().noteNewClip();
#endif
}

QImage RtxDlssNrImage(
		const FrameWithInfo &frame,
		bool fullscreen,
		int windowLongEdge) {
	if (!RtxDlssNrEnabled()) {
		return {};
	}
#if defined Q_OS_WIN
	return Engine::Instance().apply(frame, fullscreen, windowLongEdge);
#else
	return {};
#endif
}

} // namespace Media::Streaming

/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif // _WIN32

#include "media/streaming/media_streaming_rtx_fg.h"

#include "media/streaming/media_streaming_rtx_vsr_plan.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "logs.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>

#include <cmath>
#include <cstddef>
#include <cstring>

#if defined Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <dxgi1_2.h>

#ifdef small
#undef small
#endif // small

#ifndef LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
#define LOAD_LIBRARY_SEARCH_DEFAULT_DIRS 0x00001000
#endif
#ifndef LOAD_LIBRARY_SEARCH_USER_DIRS
#define LOAD_LIBRARY_SEARCH_USER_DIRS 0x00000400
#endif

#endif // Q_OS_WIN

namespace Media::Streaming {
namespace {

#if defined Q_OS_WIN

// NvCVImage ABI used by NVCVImage.dll. pixel data begins at offset 32.
struct NvCVImage {
	unsigned int width = 0;
	unsigned int height = 0;
	unsigned int pitch = 0;
	int pixelFormat = 0;
	int componentType = 0;
	unsigned char pixelBytes = 0;
	unsigned char componentBytes = 0;
	unsigned char numComponents = 0;
	unsigned char planar = 0;
	unsigned char gpuMem = 0;
	unsigned char colorspace = 0;
	unsigned char reserved[2] = {};
	void *pixels = nullptr;
	void *deletePtr = nullptr;
	void (*deleteProc)(void *) = nullptr;
	unsigned long long bufferBytes = 0;
};

static_assert(sizeof(void *) == 8);
static_assert(offsetof(NvCVImage, pixels) == 32);
static_assert(sizeof(NvCVImage) == 64);

constexpr auto kNvBgra = 7;
constexpr auto kNvU8 = 1;
constexpr auto kNvInterleaved = 0u;
constexpr auto kNvPinned = 2u;
constexpr auto kNvSuccess = 0;

using NvVFX_Handle = void *;
using CUstream = void *;
using NvStatus = int;

using CreateEffectFn = NvStatus (*)(const char *, NvVFX_Handle *);
using DestroyEffectFn = void (*)(NvVFX_Handle);
using SetU32Fn = NvStatus (*)(NvVFX_Handle, const char *, unsigned int);
using SetF32Fn = NvStatus (*)(NvVFX_Handle, const char *, float);
using SetStringFn = NvStatus (*)(NvVFX_Handle, const char *, const char *);
using SetImageFn = NvStatus (*)(NvVFX_Handle, const char *, NvCVImage *);
using SetStreamFn = NvStatus (*)(NvVFX_Handle, const char *, CUstream);
using LoadFn = NvStatus (*)(NvVFX_Handle);
using RunFn = NvStatus (*)(NvVFX_Handle, int);
using StreamCreateFn = NvStatus (*)(CUstream *);
using StreamDestroyFn = void (*)(CUstream);
using ImageAllocFn = NvStatus (*)(
	NvCVImage *,
	unsigned int,
	unsigned int,
	int,
	int,
	unsigned int,
	unsigned int,
	unsigned int);
using ImageDeallocFn = void (*)(NvCVImage *);

struct Runtime {
	HMODULE effects = nullptr;
	HMODULE image = nullptr;
	void *directory = nullptr;
	CreateEffectFn createEffect = nullptr;
	DestroyEffectFn destroyEffect = nullptr;
	SetU32Fn setU32 = nullptr;
	SetF32Fn setF32 = nullptr;
	SetStringFn setString = nullptr;
	SetImageFn setImage = nullptr;
	SetStreamFn setStream = nullptr;
	LoadFn load = nullptr;
	RunFn run = nullptr;
	StreamCreateFn createStream = nullptr;
	StreamDestroyFn destroyStream = nullptr;
	ImageAllocFn allocImage = nullptr;
	ImageDeallocFn deallocImage = nullptr;

	[[nodiscard]] bool ready() const {
		return createEffect
			&& destroyEffect
			&& setU32
			&& setF32
			&& setString
			&& setImage
			&& setStream
			&& load
			&& run
			&& createStream
			&& destroyStream
			&& allocImage
			&& deallocImage;
	}
};

struct LogState {
	std::uintptr_t clip = 0;
	unsigned skips = 0;
	bool active = false;
};

LogState &Logs() {
	static auto state = LogState();
	return state;
}

[[nodiscard]] bool NvidiaAdapterPresent() {
	const auto remote = (GetSystemMetrics(SM_REMOTESESSION) != 0);
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) {
		return false;
	}
	auto found = false;
	for (UINT index = 0; !found; ++index) {
		IDXGIAdapter1 *adapter = nullptr;
		const auto enumerate = factory->EnumAdapters1(index, &adapter);
		if (enumerate == DXGI_ERROR_NOT_FOUND) {
			break;
		}
		if (FAILED(enumerate) || !adapter) {
			if (adapter) {
				adapter->Release();
			}
			break;
		}
		DXGI_ADAPTER_DESC1 desc = {};
		const auto described = SUCCEEDED(adapter->GetDesc1(&desc));
		const auto software = described
			&& ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0);
		found = described
			&& RtxAdapterQualifies(desc.VendorId, software, remote);
		adapter->Release();
	}
	factory->Release();
	return found;
}

[[nodiscard]] bool RuntimeDirectory(const QString &path) {
	if (path.isEmpty()) {
		return false;
	}
	const auto directory = QDir(path);
	return QFileInfo(directory.filePath(u"NVVideoEffects.dll"_q)).isFile()
		&& QFileInfo(directory.filePath(u"NVCVImage.dll"_q)).isFile()
		&& QFileInfo(
			directory.filePath(u"nvVFXVideoFrameGeneration.dll"_q)).isFile();
}

[[nodiscard]] QString FindRuntimeDirectory() {
	const auto env = qEnvironmentVariable("FISHGRAM_VFX_RUNTIME");
	if (RuntimeDirectory(env)) {
		return QDir(env).absolutePath();
	}
	const auto exe = QCoreApplication::applicationDirPath();
	const auto beside = QDir(exe).filePath(u"vfx"_q);
	if (RuntimeDirectory(beside)) {
		return QDir(beside).absolutePath();
	}
	if (RuntimeDirectory(exe)) {
		return exe;
	}
	return {};
}

template <typename Function>
[[nodiscard]] Function LoadFnFrom(HMODULE module, const char *name) {
	return module
		? reinterpret_cast<Function>(GetProcAddress(module, name))
		: nullptr;
}

[[nodiscard]] bool LoadOne(const QString &path, HMODULE *module) {
	const auto wide = QDir::toNativeSeparators(path).toStdWString();
	*module = LoadLibraryExW(
		wide.c_str(),
		nullptr,
		LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
	if (!*module) {
		*module = LoadLibraryW(wide.c_str());
	}
	return *module != nullptr;
}

[[nodiscard]] bool BindRuntime(const QString &directory, Runtime *runtime) {
	const auto wide = QDir::toNativeSeparators(directory).toStdWString();
	using AddDirectory = void *(WINAPI *)(const wchar_t *);
	const auto add = reinterpret_cast<AddDirectory>(GetProcAddress(
		GetModuleHandleW(L"kernel32.dll"),
		"AddDllDirectory"));
	if (add) {
		runtime->directory = add(wide.c_str());
	} else {
		SetDllDirectoryW(wide.c_str());
	}
	const auto folder = QDir(directory);
	const auto load = [&](const QString &name, HMODULE *module) {
		return LoadOne(folder.filePath(name), module);
	};
	HMODULE cudart = nullptr;
	HMODULE ngx = nullptr;
	HMODULE feature = nullptr;
	if (!load(u"NVCVImage.dll"_q, &runtime->image)
		|| !load(u"cudart64_12.dll"_q, &cudart)
		|| !load(u"nvngxruntime.dll"_q, &ngx)
		|| !load(u"nvVFXVideoFrameGeneration.dll"_q, &feature)
		|| !load(u"NVVideoEffects.dll"_q, &runtime->effects)) {
		return false;
	}
	runtime->createEffect = LoadFnFrom<CreateEffectFn>(
		runtime->effects,
		"NvVFX_CreateEffect");
	runtime->destroyEffect = LoadFnFrom<DestroyEffectFn>(
		runtime->effects,
		"NvVFX_DestroyEffect");
	runtime->setU32 = LoadFnFrom<SetU32Fn>(runtime->effects, "NvVFX_SetU32");
	runtime->setF32 = LoadFnFrom<SetF32Fn>(runtime->effects, "NvVFX_SetF32");
	runtime->setString = LoadFnFrom<SetStringFn>(
		runtime->effects,
		"NvVFX_SetString");
	runtime->setImage = LoadFnFrom<SetImageFn>(
		runtime->effects,
		"NvVFX_SetImage");
	runtime->setStream = LoadFnFrom<SetStreamFn>(
		runtime->effects,
		"NvVFX_SetCudaStream");
	runtime->load = LoadFnFrom<LoadFn>(runtime->effects, "NvVFX_Load");
	runtime->run = LoadFnFrom<RunFn>(runtime->effects, "NvVFX_Run");
	runtime->createStream = LoadFnFrom<StreamCreateFn>(
		runtime->effects,
		"NvVFX_CudaStreamCreate");
	runtime->destroyStream = LoadFnFrom<StreamDestroyFn>(
		runtime->effects,
		"NvVFX_CudaStreamDestroy");
	runtime->allocImage = LoadFnFrom<ImageAllocFn>(
		runtime->image,
		"NvCVImage_Alloc");
	runtime->deallocImage = LoadFnFrom<ImageDeallocFn>(
		runtime->image,
		"NvCVImage_Dealloc");
	return runtime->ready();
}

class Generator {
public:
	static Generator &Instance() {
		static Generator generator;
		return generator;
	}

	[[nodiscard]] bool blocked() const {
		return RtxFgBlocked(_session);
	}

	[[nodiscard]] bool supported() {
		if (_supported == Support::Unknown) {
			_supported = probe() ? Support::Yes : Support::No;
			if (_supported == Support::No && !_loggedProbe) {
				_loggedProbe = true;
				if (_runtimeDirectory.isEmpty()) {
					LOG(("FG Error: NVIDIA Video Frame Generation runtime "
						"was not found, using normal playback."));
				} else {
					LOG(("FG Error: frame generation is unavailable, "
						"using normal playback."));
				}
			}
		}
		return _supported == Support::Yes;
	}

	void noteToggledOn() {
		_session = ResetRtxFgSession();
		releaseEffect();
		_cache = Cache();
		_loggedFailure = false;
		if (_supported == Support::No) {
			_supported = Support::Unknown;
			_loggedProbe = false;
		}
	}

	void noteClip(std::uintptr_t clip) {
		auto &logs = Logs();
		if (logs.clip == clip) {
			return;
		}
		logs.clip = clip;
		logs.skips = 0;
		logs.active = false;
	}

	void noteSkip(RtxFgReason reason, int width, int height) {
		if (reason == RtxFgReason::None
			|| reason == RtxFgReason::Off
			|| reason == RtxFgReason::Unsupported
			|| reason == RtxFgReason::Blocked) {
			return;
		}
		auto &logs = Logs();
		const auto bit = 1u << int(reason);
		if (logs.skips & bit) {
			return;
		}
		logs.skips |= bit;
		LOG(("FG Debug: skip reason=%1 size=%2x%3"
			).arg(QString::fromLatin1(RtxFgReasonName(reason))
			).arg(width
			).arg(height));
	}

	[[nodiscard]] QImage generate(
			const QImage &previous,
			const QImage &next,
			float timestep) {
		if (!onOwnerThread()
			|| RtxFgBlocked(_session)
			|| previous.isNull()
			|| next.isNull()
			|| previous.size() != next.size()
			|| timestep <= 0.f
			|| timestep >= 1.f) {
			return {};
		}
		if (!supported()) {
			return {};
		}
		const auto bucket = int(std::lround(timestep * 100.f));
		if (_cache.valid
			&& _cache.size == previous.size()
			&& _cache.bucket == bucket
			&& _cache.previousBits == previous.constBits()
			&& _cache.nextBits == next.constBits()
			&& !_cache.image.isNull()) {
			return _cache.image;
		}
		const auto prepared = ensureEffect(previous.size());
		if (prepared != RtxFgBackendResult::Ok) {
			finish(prepared);
			return {};
		}
		if (!upload(previous, &_previous)
			|| !upload(next, &_next)
			|| _runtime.setU32(_effect, "FrameMultiplier", 0) != kNvSuccess
			|| _runtime.setF32(_effect, "Timestep", timestep) != kNvSuccess
			|| _runtime.setU32(_effect, "ShotChange", 0) != kNvSuccess
			|| _runtime.setImage(_effect, "SrcImage0", &_previous) != kNvSuccess
			|| _runtime.setImage(_effect, "SrcImage1", &_next) != kNvSuccess
			|| _runtime.run(_effect, 0) != kNvSuccess) {
			finish(RtxFgBackendResult::Failed);
			return {};
		}
		auto image = download(previous);
		if (image.isNull()) {
			finish(RtxFgBackendResult::Failed);
			return {};
		}
		finish(RtxFgBackendResult::Ok);
		if (!Logs().active) {
			Logs().active = true;
			LOG(("FG Info: NVIDIA Video Frame Generation is active."));
		}
		_cache.valid = true;
		_cache.size = previous.size();
		_cache.bucket = bucket;
		_cache.previousBits = previous.constBits();
		_cache.nextBits = next.constBits();
		_cache.image = image;
		return image;
	}

private:
	enum class Support {
		Unknown,
		Yes,
		No,
	};
	struct Cache {
		bool valid = false;
		QSize size;
		int bucket = -1;
		const uchar *previousBits = nullptr;
		const uchar *nextBits = nullptr;
		QImage image;
	};

	Generator() = default;
	Generator(const Generator &) = delete;
	Generator &operator=(const Generator &) = delete;

	[[nodiscard]] bool onOwnerThread() {
		const auto now = GetCurrentThreadId();
		if (!_thread) {
			_thread = now;
			return true;
		}
		return _thread == now;
	}

	void finish(RtxFgBackendResult result) {
		const auto before = _session.state;
		_session = NoteRtxFgResult(_session, result);
		if (!_loggedFailure
			&& before == RtxFgSessionState::Ready
			&& RtxFgBlocked(_session)) {
			_loggedFailure = true;
			LOG(("FG Error: frame generation is unavailable, "
				"using normal playback."));
		}
	}

	void releaseEffect() {
		if (_runtime.deallocImage) {
			if (_previous.pixels || _previous.deletePtr) {
				_runtime.deallocImage(&_previous);
			}
			if (_next.pixels || _next.deletePtr) {
				_runtime.deallocImage(&_next);
			}
			if (_output.pixels || _output.deletePtr) {
				_runtime.deallocImage(&_output);
			}
		}
		_previous = NvCVImage();
		_next = NvCVImage();
		_output = NvCVImage();
		if (_effect && _runtime.destroyEffect) {
			_runtime.destroyEffect(_effect);
		}
		_effect = nullptr;
		_width = 0;
		_height = 0;
	}

	[[nodiscard]] bool probe() {
		if (!onOwnerThread() || !NvidiaAdapterPresent()) {
			return false;
		}
		_runtimeDirectory = FindRuntimeDirectory();
		if (_runtimeDirectory.isEmpty()) {
			return false;
		}
		if (!BindRuntime(_runtimeDirectory, &_runtime) || !_runtime.ready()) {
			return false;
		}
		if (_runtime.createStream(&_stream) != kNvSuccess || !_stream) {
			return false;
		}
		LOG(("FG Info: NVIDIA Video Frame Generation runtime is loaded."));
		return true;
	}

	[[nodiscard]] bool allocImage(NvCVImage *image, int width, int height) {
		*image = NvCVImage();
		return _runtime.allocImage(
				image,
				unsigned(width),
				unsigned(height),
				kNvBgra,
				kNvU8,
				kNvInterleaved,
				kNvPinned,
				32u) == kNvSuccess
			&& image->pixels
			&& int(image->width) == width
			&& int(image->height) == height
			&& int(image->pitch) >= (width * 4);
	}

	[[nodiscard]] RtxFgBackendResult ensureEffect(QSize size) {
		if (_effect && _width == size.width() && _height == size.height()) {
			return RtxFgBackendResult::Ok;
		}
		releaseEffect();
		if (!allocImage(&_previous, size.width(), size.height())
			|| !allocImage(&_next, size.width(), size.height())
			|| !allocImage(&_output, size.width(), size.height())) {
			releaseEffect();
			return RtxFgBackendResult::Failed;
		}
		const auto model = _runtimeDirectory.toUtf8();
		if (_runtime.createEffect("VideoFrameGeneration", &_effect) != kNvSuccess
			|| !_effect) {
			releaseEffect();
			return RtxFgBackendResult::Unsupported;
		}
		_runtime.setString(_effect, "ModelDir", model.constData());
		if (_runtime.setStream(_effect, "CudaStream", _stream) != kNvSuccess
			|| _runtime.setU32(_effect, "InputWidth", unsigned(size.width()))
				!= kNvSuccess
			|| _runtime.setU32(_effect, "InputHeight", unsigned(size.height()))
				!= kNvSuccess
			|| _runtime.setU32(_effect, "Mode", 1u) != kNvSuccess
			|| _runtime.setU32(
				_effect,
				"AutomaticShotChangeDetectionEnabled",
				1u) != kNvSuccess
			|| _runtime.load(_effect) != kNvSuccess
			|| _runtime.setImage(_effect, "DstImage0", &_output) != kNvSuccess
			|| _runtime.setU32(_effect, "FrameMultiplier", 0) != kNvSuccess) {
			releaseEffect();
			return RtxFgBackendResult::Unsupported;
		}
		_width = size.width();
		_height = size.height();
		return RtxFgBackendResult::Ok;
	}

	[[nodiscard]] static QImage straight(const QImage &image) {
		if (image.format() == QImage::Format_ARGB32
			|| image.format() == QImage::Format_RGB32
			|| image.format() == QImage::Format_ARGB32_Premultiplied) {
			return image;
		}
		return image.convertToFormat(QImage::Format_ARGB32);
	}

	[[nodiscard]] bool upload(const QImage &source, NvCVImage *dest) {
		const auto image = straight(source);
		if (image.isNull()
			|| image.width() != int(dest->width)
			|| image.height() != int(dest->height)
			|| image.bytesPerLine() < (image.width() * 4)) {
			return false;
		}
		const auto pitch = int(dest->pitch);
		for (auto y = 0; y != image.height(); ++y) {
			std::memcpy(
				static_cast<char *>(dest->pixels) + (std::size_t(y) * pitch),
				image.constScanLine(y),
				image.width() * 4);
		}
		return true;
	}

	[[nodiscard]] QImage download(const QImage &like) const {
		auto image = QImage(_width, _height, like.format());
		if (image.isNull()) {
			return {};
		}
		const auto pitch = int(_output.pitch);
		for (auto y = 0; y != _height; ++y) {
			std::memcpy(
				image.scanLine(y),
				static_cast<const char *>(_output.pixels)
					+ (std::size_t(y) * pitch),
				_width * 4);
		}
		image.setDevicePixelRatio(like.devicePixelRatio());
		return image;
	}

	Support _supported = Support::Unknown;
	RtxFgSession _session;
	Runtime _runtime;
	QString _runtimeDirectory;
	CUstream _stream = nullptr;
	NvVFX_Handle _effect = nullptr;
	NvCVImage _previous;
	NvCVImage _next;
	NvCVImage _output;
	int _width = 0;
	int _height = 0;
	DWORD _thread = 0;
	bool _loggedProbe = false;
	bool _loggedFailure = false;
	Cache _cache;

};

bool &EnabledLatch() {
	static auto enabled = false;
	return enabled;
}

#endif // Q_OS_WIN

} // namespace

bool RtxVideoFrameGenerationEnabled() {
	const auto enabled = Core::App().settings().readPref<bool>(
		kRtxVideoFrameGenerationPref,
		kRtxVideoFrameGenerationDefault);
#if defined Q_OS_WIN
	if (enabled && !EnabledLatch()) {
		Generator::Instance().noteToggledOn();
	}
	EnabledLatch() = enabled;
#endif // Q_OS_WIN
	return enabled;
}

bool RtxVideoFrameGenerationSupported() {
#if defined Q_OS_WIN
	return Generator::Instance().supported();
#else // Q_OS_WIN
	return false;
#endif // Q_OS_WIN
}

bool RtxFrameGenerationBlocked() {
#if defined Q_OS_WIN
	return Generator::Instance().blocked();
#else // Q_OS_WIN
	return false;
#endif // Q_OS_WIN
}

void RtxVideoFrameGenerationNoteToggled(bool enabled) {
	if (!enabled) {
#if defined Q_OS_WIN
		EnabledLatch() = false;
#endif // Q_OS_WIN
		return;
	}
#if defined Q_OS_WIN
	Generator::Instance().noteToggledOn();
	EnabledLatch() = true;
#endif // Q_OS_WIN
}

void RtxFrameGenerationNoteClip(std::uintptr_t clip) {
#if defined Q_OS_WIN
	Generator::Instance().noteClip(clip);
#else // Q_OS_WIN
	if (clip) {
		return;
	}
#endif // Q_OS_WIN
}

void RtxFrameGenerationNoteSkip(
		RtxFgReason reason,
		int width,
		int height) {
#if defined Q_OS_WIN
	Generator::Instance().noteSkip(reason, width, height);
#else // Q_OS_WIN
	if (reason == RtxFgReason::Off || !width || !height) {
		return;
	}
#endif // Q_OS_WIN
}

QImage RtxGeneratedFrame(
		const QImage &previous,
		const QImage &next,
		float timestep) {
	if (previous.isNull()
		|| next.isNull()
		|| timestep <= 0.f
		|| timestep >= 1.f) {
		return {};
	}
#if defined Q_OS_WIN
	return Generator::Instance().generate(previous, next, timestep);
#else // Q_OS_WIN
	return {};
#endif // Q_OS_WIN
}

} // namespace Media::Streaming

/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_rtx_vsr.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "logs.h"

#include "rpl/event_stream.h"
#include "rpl/variable.h"

#include <cmath>

#if defined Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <d3d11_1.h>
#include <dxgi1_6.h>

#ifdef small
#undef small
#endif // small
#endif // Q_OS_WIN

namespace Media::Streaming {
namespace {

struct PresentationNote {
	const char *swap = "unknown";
	int width = 0;
	int height = 0;
	float dpr = 0.f;
};

PresentationNote &Presentation() {
	static auto note = PresentationNote();
	return note;
}

struct DesktopColorNote {
	const char *hdr = "unknown";
	const char *cs = "unknown";
	int bits = 0;
	int width = 0;
	int height = 0;
	bool ready = false;
};

[[nodiscard]] const char *FrameFormatName(FrameFormat format) {
	switch (format) {
	case FrameFormat::None: return "none";
	case FrameFormat::ARGB32: return "argb32";
	case FrameFormat::YUV420: return "yuv420";
	case FrameFormat::NV12: return "nv12";
	case FrameFormat::NativeTexture: return "native";
	}
	return "other";
}

[[nodiscard]] const char *PathName(RtxVsrPath path) {
	switch (path) {
	case RtxVsrPath::Overlay: return "overlay";
	case RtxVsrPath::Track: return "track";
	case RtxVsrPath::Pip: return "pip";
	case RtxVsrPath::Rhi: return "rhi";
	}
	return "other";
}

[[nodiscard]] quint64 CurrentThread() {
#if defined Q_OS_WIN
	return quint64(GetCurrentThreadId());
#else
	return 0;
#endif
}

struct DebugGate {
	const char *reason = nullptr;
	quint64 video = 0;
	int sourceWidth = 0;
	int sourceHeight = 0;
	int outputWidth = 0;
	int outputHeight = 0;
	bool have = false;
};

DesktopColorNote DesktopColor() {
	static auto note = DesktopColorNote();
	if (note.ready) {
		return note;
	}
	note.ready = true;
#if defined Q_OS_WIN
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) {
		return note;
	}
	IDXGIAdapter1 *adapter = nullptr;
	const auto remote = (GetSystemMetrics(SM_REMOTESESSION) != 0);
	for (UINT index = 0; ; ++index) {
		if (adapter) {
			adapter->Release();
			adapter = nullptr;
		}
		const auto enumerate = factory->EnumAdapters1(index, &adapter);
		if (enumerate == DXGI_ERROR_NOT_FOUND) {
			break;
		}
		if (FAILED(enumerate) || !adapter) {
			break;
		}
		DXGI_ADAPTER_DESC1 desc = {};
		if (FAILED(adapter->GetDesc1(&desc))) {
			continue;
		}
		const auto software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
		if (!RtxAdapterQualifies(desc.VendorId, software, remote)) {
			continue;
		}
		IDXGIOutput *output = nullptr;
		if (FAILED(adapter->EnumOutputs(0, &output)) || !output) {
			break;
		}
		IDXGIOutput6 *output6 = nullptr;
		const auto queried = output->QueryInterface(IID_PPV_ARGS(&output6));
		if (SUCCEEDED(queried) && output6) {
			DXGI_OUTPUT_DESC1 desc1 = {};
			if (SUCCEEDED(output6->GetDesc1(&desc1))) {
				note.width = int(desc1.DesktopCoordinates.right
					- desc1.DesktopCoordinates.left);
				note.height = int(desc1.DesktopCoordinates.bottom
					- desc1.DesktopCoordinates.top);
				note.bits = int(desc1.BitsPerColor);
				switch (desc1.ColorSpace) {
				case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
					note.hdr = "sdr";
					note.cs = "g22-p709";
					break;
				case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
					note.hdr = "scrgb";
					note.cs = "g10-p709";
					break;
				case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
					note.hdr = "hdr10";
					note.cs = "pq-p2020";
					break;
				case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
					note.hdr = "hdr10";
					note.cs = "studio-pq-p2020";
					break;
				case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709:
					note.hdr = "sdr";
					note.cs = "studio-g22-p709";
					break;
				default:
					note.hdr = "other";
					note.cs = "other";
					break;
				}
			}
			output6->Release();
		}
		output->Release();
		break;
	}
	if (adapter) {
		adapter->Release();
	}
	factory->Release();
#endif // Q_OS_WIN
	return note;
}

void LogSkip(
		DebugGate &gate,
		const char *reason,
		RtxVsrSize source,
		QSize target,
		FrameFormat format,
		int avPixelFormat,
		RtxVsrPath path,
		quint64 videoTag,
		quint64 owner,
		const DesktopColorNote &desktop) {
	if (gate.have && gate.reason == reason && gate.video == videoTag) {
		return;
	}
	gate.have = true;
	gate.reason = reason;
	gate.video = videoTag;
	const auto &shown = Presentation();
	LOG(("VSR Debug: skip reason=%1 source=%2x%3 format=%4 pix=%5"
		" target=%6x%7 thread=%8 owner=%9 path=%10"
		" hdr=%11 swap=%12 cs=%13 bits=%14 desk=%15x%16"
		" shown=%17x%18 dpr=%19"
		).arg(reason
		).arg(source.width
		).arg(source.height
		).arg(FrameFormatName(format)
		).arg(avPixelFormat
		).arg(target.width()
		).arg(target.height()
		).arg(CurrentThread()
		).arg(owner
		).arg(PathName(path)
		).arg(desktop.hdr
		).arg(shown.swap
		).arg(desktop.cs
		).arg(desktop.bits
		).arg(desktop.width
		).arg(desktop.height
		).arg(shown.width
		).arg(shown.height
		).arg(double(shown.dpr), 0, 'f', 2));
}

void LogUpscale(
		DebugGate &gate,
		RtxVsrSize source,
		RtxVsrSize output,
		FrameFormat format,
		int avPixelFormat,
		RtxVsrPath path,
		quint64 videoTag,
		const DesktopColorNote &desktop) {
	if (gate.have
		&& gate.video == videoTag
		&& gate.sourceWidth == source.width
		&& gate.sourceHeight == source.height
		&& gate.outputWidth == output.width
		&& gate.outputHeight == output.height) {
		return;
	}
	gate.have = true;
	gate.video = videoTag;
	gate.reason = "upscaled";
	gate.sourceWidth = source.width;
	gate.sourceHeight = source.height;
	gate.outputWidth = output.width;
	gate.outputHeight = output.height;
	const auto &shown = Presentation();
	LOG(("VSR Debug: upscaled source=%1x%2 output=%3x%4 format=%5 pix=%6"
		" path=%7 hdr=%8 swap=%9 cs=%10 bits=%11 desk=%12x%13"
		" shown=%14x%15 dpr=%16"
		).arg(source.width
		).arg(source.height
		).arg(output.width
		).arg(output.height
		).arg(FrameFormatName(format)
		).arg(avPixelFormat
		).arg(PathName(path)
		).arg(desktop.hdr
		).arg(shown.swap
		).arg(desktop.cs
		).arg(desktop.bits
		).arg(desktop.width
		).arg(desktop.height
		).arg(shown.width
		).arg(shown.height
		).arg(double(shown.dpr), 0, 'f', 2));
}

#if defined Q_OS_WIN

// NVIDIA stream extension, version 1 method 2.
// https://chromium.googlesource.com/chromium/src/+/46f000a1/ui/gl/swap_chain_presenter.cc
const GUID kNvidiaPpeInterface = {
	0xD43CE1B3,
	0x1F4B,
	0x48AC,
	{ 0xBA, 0xEE, 0xC3, 0xC2, 0x53, 0x75, 0xE6, 0xF7 },
};

struct NvidiaStreamExtension {
	UINT version;
	UINT method;
	UINT enable;
};

static_assert(sizeof(NvidiaStreamExtension) == sizeof(UINT) * 3);

// NVIDIA TrueHDR (RTX Video HDR), version 4 method 3.
// https://chromium.googlesource.com/chromium/src/+/b2fae4c98404bec282f24cd7d8a63aced9070fda/ui/gl/swap_chain_presenter.cc
const GUID kNvidiaTrueHdrInterface = {
	0xFDD62BB4,
	0x620B,
	0x4FD7,
	{ 0x9A, 0xB3, 0x1E, 0x59, 0xD0, 0xD5, 0x44, 0xB3 },
};

struct NvidiaTrueHdrExtension {
	UINT version;
	UINT method;
	UINT flags;
};

static_assert(sizeof(NvidiaTrueHdrExtension) == sizeof(UINT) * 3);

#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

class Processor {
public:
	static Processor &Instance() {
		static Processor processor;
		return processor;
	}

	[[nodiscard]] bool supported() {
		return featureSupported(_vsrSupported);
	}

	[[nodiscard]] bool hdrSupported() {
		return featureSupported(_hdrSupported);
	}

	void noteToggledOn() {
		_session = ResetRtxVsrSession();
		_rejectAtOrAbove = 0;
		_rejectSource = {};
		_cache = Cache();
		_loggedFailure = false;
		_skipGate = {};
		_upscaleGate = {};
		if (_vsrSupported == Support::No) {
			_vsrSupported = Support::Unknown;
		}
	}

	void noteHdrToggledOn() {
		_hdrSession = ResetRtxVsrSession();
		_hdrLoggedActive = false;
		_hdrFailureLogged = false;
		_hdrSampled = false;
		_hdrSkip = {};
		if (_hdrSupported == Support::No) {
			_hdrSupported = Support::Unknown;
		}
		hidePopup(nullptr);
	}

	void releaseWindow(quintptr windowId) {
		hidePopup(reinterpret_cast<HWND>(windowId));
	}

	[[nodiscard]] bool displayIsHdr(quintptr windowId) {
		return windowOnHdrDisplay(reinterpret_cast<HWND>(windowId));
	}

	[[nodiscard]] QImage enhance(
			const FrameWithInfo &frame,
			const RtxEnhanceRequest &request,
			bool hdrEnabled,
			bool vsrEnabled) {
		if (hdrEnabled && presentHdr(frame, request)) {
			if (!vsrEnabled) {
				upscale(
					frame,
					request.targetPixels,
					request.path,
					request.videoTag);
			}
			return {};
		}
		return upscale(
			frame,
			request.targetPixels,
			request.path,
			request.videoTag);
	}

	[[nodiscard]] QImage upscale(
			const FrameWithInfo &frame,
			QSize targetPixels,
			RtxVsrPath path,
			quint64 videoTag) {
		if (!RtxVideoSuperResolutionEnabled()) {
			LogSkip(
				_skipGate,
				"disabled",
				{},
				targetPixels,
				frame.format,
				frame.avPixelFormat,
				path,
				videoTag,
				quint64(_thread),
				DesktopColor());
			return {};
		}
		auto source = extract(frame);
		source.size = RtxEvenSize(source.size);
		const auto skip = [&](const char *reason) {
			LogSkip(
				_skipGate,
				reason,
				source.size,
				targetPixels,
				frame.format,
				frame.avPixelFormat,
				path,
				videoTag,
				quint64(_thread),
				DesktopColor());
			return QImage();
		};
		if (!onOwnerThread()) {
			return skip("thread");
		}
		if (frame.alpha) {
			return skip("alpha");
		}
		if (frame.index < 0) {
			return skip("bad-index");
		}
		if (RtxVsrBlocked(_session)) {
			return skip("blocked");
		}
		if (targetPixels.isEmpty()) {
			return skip("empty-target");
		}
		if (!source.token || source.size.isEmpty()) {
			return skip("no-frame");
		}
		if (!(_rejectSource == source.size)) {
			_rejectAtOrAbove = 0;
			_rejectSource = source.size;
		}
		if (_cache.valid
			&& _cache.index == frame.index
			&& _cache.token == source.token
			&& _cache.targetWidth == targetPixels.width()
			&& _cache.targetHeight == targetPixels.height()) {
			if (_cache.image.isNull()) {
				return skip("cached-empty");
			}
			return _cache.image;
		}
		const auto sizeReason = RtxVsrRejectReasonFor(
			source.size,
			{ targetPixels.width(), targetPixels.height() });
		if (sizeReason != RtxVsrRejectReason::None) {
			return skip(RtxVsrRejectReasonName(sizeReason));
		}
		const auto outputs = RtxVsrOutputs(
			source.size,
			{ targetPixels.width(), targetPixels.height() });
		if (!outputs.count) {
			return skip("no-output");
		}
		if (!supported()) {
			return skip("unsupported");
		}

		auto image = QImage();
		auto failedEdge = 0;
		auto tried = 0;
		auto skipped = 0;
		for (auto i = 0; i != outputs.count; ++i) {
			const auto size = outputs.sizes[i];
			const auto edge = RtxLongEdge(size);
			if (_rejectAtOrAbove && edge >= _rejectAtOrAbove) {
				++skipped;
				continue;
			}
			++tried;
			const auto attempt = trySize(source, size);
			if (attempt.kind == AttemptKind::Done) {
				if (failedEdge) {
					_rejectAtOrAbove = failedEdge;
				}
				image = std::move(attempt.image);
				if (!_loggedActive) {
					_loggedActive = true;
					LOG(("VSR Info: NVIDIA RTX video super resolution is active."));
				}
				break;
			}
			if (attempt.kind == AttemptKind::Stop) {
				remember(frame, source.token, targetPixels, QImage());
				finish(RtxVsrBackendResult::Unsupported);
				return skip("driver-rejected");
			}
			failedEdge = edge;
		}
		if (image.isNull()) {
			if (!tried && skipped) {
				return skip("reject-latch");
			}
			finish(RtxVsrBackendResult::Failed);
			skip("backend-failed");
		} else {
			finish(RtxVsrBackendResult::Ok);
			LogUpscale(
				_upscaleGate,
				source.size,
				{
					image.width(),
					image.height(),
				},
				frame.format,
				frame.avPixelFormat,
				path,
				videoTag,
				DesktopColor());
		}
		remember(frame, source.token, targetPixels, image);
		return image;
	}

private:
	enum class Support {
		Unknown,
		Yes,
		No,
	};
	enum class InputKind {
		None,
		Nv12,
		Bgra,
	};
	enum class AttemptKind {
		Done,
		Smaller,
		Stop,
	};
	struct Source {
		RtxVsrSize size;
		const void *token = nullptr;
		int lumaMax = 0;
		InputKind kind = InputKind::None;
		bool planar = false;
		const FrameYUV *yuv = nullptr;
		QImage image;
	};
	struct Attempt {
		AttemptKind kind = AttemptKind::Smaller;
		QImage image;
	};
	struct Cache {
		bool valid = false;
		int index = -1;
		const void *token = nullptr;
		int targetWidth = 0;
		int targetHeight = 0;
		QImage image;
	};
	struct HdrSkip {
		const char *reason = nullptr;
		quint64 video = 0;
	};
	struct MapGuard {
		ID3D11DeviceContext *context = nullptr;
		ID3D11Resource *resource = nullptr;
		~MapGuard() {
			if (context && resource) {
				context->Unmap(resource, 0);
			}
		}
	};

	Processor() = default;
	Processor(const Processor &) = delete;
	Processor &operator=(const Processor &) = delete;
	~Processor() {
		destroyPopup();
	}

	[[nodiscard]] bool onOwnerThread() {
		const auto now = GetCurrentThreadId();
		if (!_thread) {
			_thread = now;
			return true;
		}
		return _thread == now;
	}

	void remember(
			const FrameWithInfo &frame,
			const void *token,
			QSize target,
			QImage image) {
		_cache.valid = true;
		_cache.index = frame.index;
		_cache.token = token;
		_cache.targetWidth = target.width();
		_cache.targetHeight = target.height();
		_cache.image = std::move(image);
	}

	void finish(RtxVsrBackendResult result) {
		const auto before = _session.state;
		_session = NoteRtxVsrResult(_session, result);
		if (!_loggedFailure
			&& before == RtxVsrSessionState::Ready
			&& RtxVsrBlocked(_session)) {
			_loggedFailure = true;
			LOG(("VSR Error: super resolution is unavailable, "
				"using normal playback."));
		}
	}

	void releaseProcessor() {
		_inputView.Reset();
		_outputView.Reset();
		_processor.Reset();
		_enumerator.Reset();
		_input.Reset();
		_inputStaging.Reset();
		_output.Reset();
		_outputStaging.Reset();
		_inWidth = _inHeight = _outWidth = _outHeight = 0;
		_kind = InputKind::None;
		_hdrOutput = false;
	}

	void dropDevice() {
		releaseProcessor();
		_videoContext1.Reset();
		_videoContext.Reset();
		_videoDevice.Reset();
		_context.Reset();
		_device.Reset();
		_vsrSupported = Support::Unknown;
		_hdrSupported = Support::Unknown;
	}

	[[nodiscard]] bool featureSupported(Support &slot) {
		if (slot == Support::Unknown) {
			if (!probe()) {
				dropDevice();
				_vsrSupported = Support::No;
				_hdrSupported = Support::No;
			}
		}
		return slot == Support::Yes;
	}

	template <typename T>
	struct ComPtr {
		T *pointer = nullptr;
		ComPtr() = default;
		ComPtr(const ComPtr &) = delete;
		ComPtr &operator=(const ComPtr &) = delete;
		~ComPtr() {
			Reset();
		}
		void Reset(T *value = nullptr) {
			if (pointer) {
				pointer->Release();
			}
			pointer = value;
		}
		[[nodiscard]] T *Get() const {
			return pointer;
		}
		[[nodiscard]] explicit operator bool() const {
			return pointer != nullptr;
		}
		T *operator->() const {
			return pointer;
		}
	};

	[[nodiscard]] bool ensureDevice() {
		if (_device && _videoDevice && _videoContext) {
			return true;
		}
		dropDevice();
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
			if (!described
				|| !RtxAdapterQualifies(desc.VendorId, software, remote)) {
				adapter->Release();
				continue;
			}
			found = adoptAdapter(adapter);
			adapter->Release();
		}
		factory->Release();
		return found;
	}

	[[nodiscard]] bool adoptAdapter(IDXGIAdapter1 *adapter) {
		ID3D11Device *device = nullptr;
		ID3D11DeviceContext *context = nullptr;
		const D3D_FEATURE_LEVEL levels[] = {
			D3D_FEATURE_LEVEL_11_1,
			D3D_FEATURE_LEVEL_11_0,
		};
		auto level = D3D_FEATURE_LEVEL_11_0;
		const auto created = D3D11CreateDevice(
			adapter,
			D3D_DRIVER_TYPE_UNKNOWN,
			nullptr,
			D3D11_CREATE_DEVICE_VIDEO_SUPPORT
				| D3D11_CREATE_DEVICE_BGRA_SUPPORT,
			levels,
			UINT(sizeof(levels) / sizeof(levels[0])),
			D3D11_SDK_VERSION,
			&device,
			&level,
			&context);
		if (FAILED(created) || !device || !context) {
			if (context) {
				context->Release();
			}
			if (device) {
				device->Release();
			}
			return false;
		}
		ID3D11VideoDevice *videoDevice = nullptr;
		ID3D11VideoContext *videoContext = nullptr;
		const auto videoHr = device->QueryInterface(
			IID_PPV_ARGS(&videoDevice));
		const auto contextHr = context->QueryInterface(
			IID_PPV_ARGS(&videoContext));
		if (FAILED(videoHr) || FAILED(contextHr) || !videoDevice || !videoContext) {
			if (videoContext) {
				videoContext->Release();
			}
			if (videoDevice) {
				videoDevice->Release();
			}
			context->Release();
			device->Release();
			return false;
		}
		ID3D11VideoContext1 *videoContext1 = nullptr;
		if (SUCCEEDED(videoContext->QueryInterface(
				IID_PPV_ARGS(&videoContext1)))
			&& videoContext1) {
			_videoContext1.Reset(videoContext1);
		} else if (videoContext1) {
			videoContext1->Release();
		}
		_device.Reset(device);
		_context.Reset(context);
		_videoDevice.Reset(videoDevice);
		_videoContext.Reset(videoContext);
		return true;
	}

	[[nodiscard]] bool probe() {
		if (!onOwnerThread() || !ensureDevice()) {
			return false;
		}
		D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
		content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
		content.InputFrameRate.Numerator = 30;
		content.InputFrameRate.Denominator = 1;
		content.InputWidth = 64;
		content.InputHeight = 64;
		content.OutputFrameRate.Numerator = 30;
		content.OutputFrameRate.Denominator = 1;
		content.OutputWidth = 128;
		content.OutputHeight = 128;
		content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
		ID3D11VideoProcessorEnumerator *enumerator = nullptr;
		auto hr = _videoDevice->CreateVideoProcessorEnumerator(
			&content,
			&enumerator);
		if (FAILED(hr) || !enumerator) {
			return false;
		}
		ID3D11VideoProcessor *processor = nullptr;
		hr = _videoDevice->CreateVideoProcessor(enumerator, 0, &processor);
		if (FAILED(hr) || !processor) {
			enumerator->Release();
			return false;
		}
		auto info = NvidiaStreamExtension{ 1u, 2u, 1u };
		hr = _videoContext->VideoProcessorSetStreamExtension(
			processor,
			0,
			&kNvidiaPpeInterface,
			UINT(sizeof(info)),
			&info);
		_vsrSupported = SUCCEEDED(hr) ? Support::Yes : Support::No;
		auto hdrFlag = UINT(0);
		const auto hdrHr = _videoContext->VideoProcessorGetStreamExtension(
			processor,
			0,
			&kNvidiaTrueHdrInterface,
			UINT(sizeof(hdrFlag)),
			&hdrFlag);
		_hdrSupported = (SUCCEEDED(hdrHr) && hdrFlag == 1u)
			? Support::Yes
			: Support::No;
		processor->Release();
		enumerator->Release();
		return true;
	}

	[[nodiscard]] bool createTexture(
			ID3D11Texture2D **texture,
			int width,
			int height,
			DXGI_FORMAT format,
			UINT bind,
			D3D11_USAGE usage,
			UINT cpuAccess) {
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = UINT(width);
		desc.Height = UINT(height);
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Usage = usage;
		desc.BindFlags = bind;
		desc.CPUAccessFlags = cpuAccess;
		return SUCCEEDED(_device->CreateTexture2D(&desc, nullptr, texture))
			&& (*texture != nullptr);
	}

	[[nodiscard]] bool ensureProcessor(
			const Source &source,
			RtxVsrSize output,
			bool hdr) {
		if (_processor
			&& _hdrOutput == hdr
			&& _kind == source.kind
			&& _inWidth == source.size.width
			&& _inHeight == source.size.height
			&& _outWidth == output.width
			&& _outHeight == output.height) {
			return true;
		}
		releaseProcessor();
		const auto inputFormat = (source.kind == InputKind::Nv12)
			? DXGI_FORMAT_NV12
			: DXGI_FORMAT_B8G8R8A8_UNORM;
		const auto outputFormat = hdr
			? DXGI_FORMAT_R16G16B16A16_FLOAT
			: DXGI_FORMAT_B8G8R8A8_UNORM;
		D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
		content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
		content.InputFrameRate.Numerator = 30;
		content.InputFrameRate.Denominator = 1;
		content.InputWidth = UINT(source.size.width);
		content.InputHeight = UINT(source.size.height);
		content.OutputFrameRate.Numerator = 30;
		content.OutputFrameRate.Denominator = 1;
		content.OutputWidth = UINT(output.width);
		content.OutputHeight = UINT(output.height);
		content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
		ID3D11VideoProcessorEnumerator *enumerator = nullptr;
		auto hr = _videoDevice->CreateVideoProcessorEnumerator(
			&content,
			&enumerator);
		if (FAILED(hr) || !enumerator) {
			return false;
		}
		auto inputFlags = UINT(0);
		auto outputFlags = UINT(0);
		const auto inputOk = SUCCEEDED(enumerator->CheckVideoProcessorFormat(
				inputFormat,
				&inputFlags))
			&& ((inputFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) != 0);
		const auto outputOk = SUCCEEDED(enumerator->CheckVideoProcessorFormat(
				outputFormat,
				&outputFlags))
			&& ((outputFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) != 0);
		if (!inputOk || !outputOk) {
			enumerator->Release();
			return false;
		}
		ID3D11VideoProcessor *processor = nullptr;
		hr = _videoDevice->CreateVideoProcessor(enumerator, 0, &processor);
		if (FAILED(hr) || !processor) {
			enumerator->Release();
			return false;
		}
		_enumerator.Reset(enumerator);
		_processor.Reset(processor);
		const auto inputBind = (source.kind == InputKind::Nv12)
			? UINT(D3D11_BIND_DECODER)
			: UINT(D3D11_BIND_RENDER_TARGET);
		ID3D11Texture2D *input = nullptr;
		ID3D11Texture2D *inputStaging = nullptr;
		ID3D11Texture2D *outputTexture = nullptr;
		ID3D11Texture2D *outputStaging = nullptr;
		const auto inputCreated = createTexture(
				&input,
				source.size.width,
				source.size.height,
				inputFormat,
				inputBind,
				D3D11_USAGE_DEFAULT,
				0)
			|| ((source.kind == InputKind::Nv12)
				&& createTexture(
					&input,
					source.size.width,
					source.size.height,
					inputFormat,
					D3D11_BIND_SHADER_RESOURCE,
					D3D11_USAGE_DEFAULT,
					0));
		const auto textures = inputCreated
			&& createTexture(
				&inputStaging,
				source.size.width,
				source.size.height,
				inputFormat,
				0,
				D3D11_USAGE_STAGING,
				D3D11_CPU_ACCESS_WRITE)
			&& createTexture(
				&outputTexture,
				output.width,
				output.height,
				outputFormat,
				D3D11_BIND_RENDER_TARGET,
				D3D11_USAGE_DEFAULT,
				0)
			&& createTexture(
				&outputStaging,
				output.width,
				output.height,
				outputFormat,
				0,
				D3D11_USAGE_STAGING,
				D3D11_CPU_ACCESS_READ);
		if (!textures) {
			if (outputStaging) {
				outputStaging->Release();
			}
			if (outputTexture) {
				outputTexture->Release();
			}
			if (inputStaging) {
				inputStaging->Release();
			}
			if (input) {
				input->Release();
			}
			releaseProcessor();
			return false;
		}
		D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc = {};
		inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
		ID3D11VideoProcessorInputView *inputView = nullptr;
		hr = _videoDevice->CreateVideoProcessorInputView(
			input,
			_enumerator.Get(),
			&inputDesc,
			&inputView);
		D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc = {};
		outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
		ID3D11VideoProcessorOutputView *outputView = nullptr;
		const auto outputHr = _videoDevice->CreateVideoProcessorOutputView(
			outputTexture,
			_enumerator.Get(),
			&outputDesc,
			&outputView);
		if (FAILED(hr) || FAILED(outputHr) || !inputView || !outputView) {
			if (outputView) {
				outputView->Release();
			}
			if (inputView) {
				inputView->Release();
			}
			outputStaging->Release();
			outputTexture->Release();
			inputStaging->Release();
			input->Release();
			releaseProcessor();
			return false;
		}
		_input.Reset(input);
		_inputStaging.Reset(inputStaging);
		_output.Reset(outputTexture);
		_outputStaging.Reset(outputStaging);
		_inputView.Reset(inputView);
		_outputView.Reset(outputView);
		_kind = source.kind;
		_hdrOutput = hdr;
		_inWidth = source.size.width;
		_inHeight = source.size.height;
		_outWidth = output.width;
		_outHeight = output.height;
		if (_videoContext1) {
			const auto yuvSpace = (_inHeight >= 720)
				? DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709
				: DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
			const auto inputSpace = (_kind == InputKind::Nv12)
				? yuvSpace
				: DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
			_videoContext1->VideoProcessorSetStreamColorSpace1(
				_processor.Get(),
				0,
				inputSpace);
			_videoContext1->VideoProcessorSetOutputColorSpace1(
				_processor.Get(),
				hdr
					? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
					: DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
		} else if (hdr) {
			releaseProcessor();
			return false;
		}
		RECT sourceRect = { 0, 0, _inWidth, _inHeight };
		RECT destRect = { 0, 0, _outWidth, _outHeight };
		_videoContext->VideoProcessorSetStreamFrameFormat(
			_processor.Get(),
			0,
			D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
		_videoContext->VideoProcessorSetStreamSourceRect(
			_processor.Get(),
			0,
			TRUE,
			&sourceRect);
		_videoContext->VideoProcessorSetStreamDestRect(
			_processor.Get(),
			0,
			TRUE,
			&destRect);
		_videoContext->VideoProcessorSetOutputTargetRect(
			_processor.Get(),
			TRUE,
			&destRect);
		return true;
	}

	[[nodiscard]] bool uploadNv12(const Source &source) {
		D3D11_MAPPED_SUBRESOURCE mapped = {};
		if (FAILED(_context->Map(
				_inputStaging.Get(),
				0,
				D3D11_MAP_WRITE,
				0,
				&mapped))
			|| !mapped.pData) {
			return false;
		}
		auto guard = MapGuard{ _context.Get(), _inputStaging.Get() };
		const auto stride = int(mapped.RowPitch);
		auto *dest = static_cast<unsigned char *>(mapped.pData);
		const auto yuv = source.yuv;
		auto wrote = false;
		if (!yuv || !yuv->y.data || !yuv->u.data) {
			wrote = false;
		} else if (!source.planar) {
			if (stride >= source.size.width
				&& yuv->y.stride >= source.size.width
				&& yuv->u.stride >= source.size.width
				&& yuv->size.width() >= source.size.width
				&& yuv->size.height() >= source.size.height) {
				for (auto row = 0; row != source.size.height; ++row) {
					std::memcpy(
						dest + (std::size_t(row) * stride),
						static_cast<const unsigned char *>(yuv->y.data)
							+ (std::size_t(row) * yuv->y.stride),
						source.size.width);
				}
				auto *uv = dest + (std::size_t(stride) * source.size.height);
				for (auto row = 0; row != (source.size.height / 2); ++row) {
					std::memcpy(
						uv + (std::size_t(row) * stride),
						static_cast<const unsigned char *>(yuv->u.data)
							+ (std::size_t(row) * yuv->u.stride),
						source.size.width);
				}
				wrote = true;
			}
		} else if (yuv->v.data) {
			wrote = PackNv12(
				static_cast<const unsigned char *>(yuv->y.data),
				yuv->y.stride,
				static_cast<const unsigned char *>(yuv->u.data),
				yuv->u.stride,
				static_cast<const unsigned char *>(yuv->v.data),
				yuv->v.stride,
				source.size.width,
				source.size.height,
				dest,
				stride);
		}
		guard.context = nullptr;
		_context->Unmap(_inputStaging.Get(), 0);
		if (!wrote) {
			return false;
		}
		_context->CopyResource(_input.Get(), _inputStaging.Get());
		return true;
	}

	[[nodiscard]] bool uploadBgra(const Source &source) {
		auto image = source.image;
		if (image.format() != QImage::Format_ARGB32
			&& image.format() != QImage::Format_ARGB32_Premultiplied
			&& image.format() != QImage::Format_RGB32) {
			image = image.convertToFormat(QImage::Format_ARGB32);
		}
		if (image.isNull()) {
			return false;
		}
		D3D11_MAPPED_SUBRESOURCE mapped = {};
		if (FAILED(_context->Map(
				_inputStaging.Get(),
				0,
				D3D11_MAP_WRITE,
				0,
				&mapped))
			|| !mapped.pData) {
			return false;
		}
		const auto stride = int(mapped.RowPitch);
		auto *dest = static_cast<unsigned char *>(mapped.pData);
		const auto widthBytes = source.size.width * 4;
		if (stride < widthBytes
			|| image.width() < source.size.width
			|| image.height() < source.size.height) {
			_context->Unmap(_inputStaging.Get(), 0);
			return false;
		}
		for (auto row = 0; row != source.size.height; ++row) {
			std::memcpy(
				dest + (std::size_t(row) * stride),
				image.constScanLine(row),
				widthBytes);
		}
		_context->Unmap(_inputStaging.Get(), 0);
		_context->CopyResource(_input.Get(), _inputStaging.Get());
		return true;
	}

	[[nodiscard]] QImage readback(int sourceLumaMax) {
		_context->CopyResource(_outputStaging.Get(), _output.Get());
		_context->Flush();
		D3D11_MAPPED_SUBRESOURCE mapped = {};
		if (FAILED(_context->Map(
				_outputStaging.Get(),
				0,
				D3D11_MAP_READ,
				0,
				&mapped))
			|| !mapped.pData) {
			return {};
		}
		auto image = QImage(
			_outWidth,
			_outHeight,
			QImage::Format_ARGB32_Premultiplied);
		if (image.isNull() || int(mapped.RowPitch) < (_outWidth * 4)) {
			_context->Unmap(_outputStaging.Get(), 0);
			return {};
		}
		const auto *src = static_cast<const unsigned char *>(mapped.pData);
		for (auto row = 0; row != _outHeight; ++row) {
			auto *dst = image.scanLine(row);
			std::memcpy(dst, src + (std::size_t(row) * mapped.RowPitch), _outWidth * 4);
			for (auto column = 0; column != _outWidth; ++column) {
				dst[(column * 4) + 3] = 255;
			}
		}
		_context->Unmap(_outputStaging.Get(), 0);
		const auto outputMax = RtxSampleMaxByte(
			image.constBits(),
			image.width(),
			image.height(),
			image.bytesPerLine(),
			4,
			3);
		if (RtxOutputRejectedAsBlank(sourceLumaMax, outputMax)) {
			return {};
		}
		return image;
	}

	[[nodiscard]] Attempt trySize(const Source &source, RtxVsrSize output) {
		if (!ensureDevice()) {
			return { AttemptKind::Stop };
		}
		if (deviceRemoved()) {
			dropDevice();
			return { AttemptKind::Stop };
		}
		if (!ensureProcessor(source, output, false)) {
			return { AttemptKind::Smaller };
		}
		const auto uploaded = (source.kind == InputKind::Nv12)
			? uploadNv12(source)
			: uploadBgra(source);
		if (!uploaded) {
			return { AttemptKind::Smaller };
		}
		auto info = NvidiaStreamExtension{ 1u, 2u, 1u };
		auto hr = _videoContext->VideoProcessorSetStreamExtension(
			_processor.Get(),
			0,
			&kNvidiaPpeInterface,
			UINT(sizeof(info)),
			&info);
		if (FAILED(hr)) {
			return { AttemptKind::Stop };
		}
		D3D11_VIDEO_PROCESSOR_STREAM stream = {};
		stream.Enable = TRUE;
		stream.pInputSurface = _inputView.Get();
		hr = _videoContext->VideoProcessorBlt(
			_processor.Get(),
			_outputView.Get(),
			0,
			1,
			&stream);
		if (FAILED(hr)) {
			if (hr == DXGI_ERROR_DEVICE_REMOVED
				|| hr == DXGI_ERROR_DEVICE_RESET) {
				dropDevice();
				return { AttemptKind::Stop };
			}
			info.enable = 0;
			_videoContext->VideoProcessorSetStreamExtension(
				_processor.Get(),
				0,
				&kNvidiaPpeInterface,
				UINT(sizeof(info)),
				&info);
			return { AttemptKind::Smaller };
		}
		auto image = readback(source.lumaMax);
		if (image.isNull()) {
			return { AttemptKind::Smaller };
		}
		return { AttemptKind::Done, std::move(image) };
	}

	[[nodiscard]] bool deviceRemoved() const {
		if (!_device) {
			return false;
		}
		const auto removed = _device->GetDeviceRemovedReason();
		return FAILED(removed);
	}

	[[nodiscard]] static Source extract(const FrameWithInfo &frame) {
		auto source = Source();
		if (frame.yuv
			&& frame.yuv->y.data
			&& frame.yuv->u.data
			&& !frame.yuv->size.isEmpty()
			&& ((frame.format == FrameFormat::NV12)
				|| (frame.format == FrameFormat::YUV420 && frame.yuv->v.data))) {
			source.kind = InputKind::Nv12;
			source.planar = (frame.format == FrameFormat::YUV420);
			source.size = {
				frame.yuv->size.width(),
				frame.yuv->size.height(),
			};
			source.token = frame.yuv->y.data;
			source.lumaMax = RtxSampleMaxByte(
				static_cast<const unsigned char *>(frame.yuv->y.data),
				source.size.width,
				source.size.height,
				frame.yuv->y.stride,
				1,
				1);
			source.yuv = frame.yuv;
			return source;
		}
		if (!frame.image.isNull()) {
			source.kind = InputKind::Bgra;
			source.size = {
				frame.image.width(),
				frame.image.height(),
			};
			source.token = frame.image.constBits();
			source.lumaMax = RtxSampleMaxByte(
				frame.image.constBits(),
				frame.image.width(),
				frame.image.height(),
				frame.image.bytesPerLine(),
				4,
				3);
			source.image = frame.image;
		}
		return source;
	}

	[[nodiscard]] bool windowOnHdrDisplay(HWND hwnd) {
		if (!hwnd || (GetSystemMetrics(SM_REMOTESESSION) != 0)) {
			return false;
		}
		const auto monitor = MonitorFromWindow(
			hwnd,
			MONITOR_DEFAULTTONEAREST);
		if (!monitor) {
			return false;
		}
		const auto now = GetTickCount64();
		if (monitor == _hdrMonitor && (now - _hdrMonitorTick) < 2000) {
			return _hdrMonitorOn;
		}
		_hdrMonitor = monitor;
		_hdrMonitorTick = now;
		_hdrMonitorOn = false;
		MONITORINFO info = {};
		info.cbSize = sizeof(info);
		if (!GetMonitorInfoW(monitor, &info)) {
			return false;
		}
		IDXGIFactory1 *factory = nullptr;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) {
			return false;
		}
		for (UINT adapterIndex = 0; !_hdrMonitorOn; ++adapterIndex) {
			IDXGIAdapter1 *adapter = nullptr;
			const auto enumerate = factory->EnumAdapters1(
				adapterIndex,
				&adapter);
			if (enumerate == DXGI_ERROR_NOT_FOUND) {
				break;
			}
			if (FAILED(enumerate) || !adapter) {
				break;
			}
			for (UINT outputIndex = 0; ; ++outputIndex) {
				IDXGIOutput *output = nullptr;
				const auto next = adapter->EnumOutputs(outputIndex, &output);
				if (next == DXGI_ERROR_NOT_FOUND) {
					break;
				}
				if (FAILED(next) || !output) {
					if (output) {
						output->Release();
					}
					break;
				}
				IDXGIOutput6 *output6 = nullptr;
				const auto queried = output->QueryInterface(
					IID_PPV_ARGS(&output6));
				output->Release();
				if (FAILED(queried) || !output6) {
					if (output6) {
						output6->Release();
					}
					continue;
				}
				DXGI_OUTPUT_DESC1 desc = {};
				const auto described = SUCCEEDED(output6->GetDesc1(&desc));
				output6->Release();
				if (!described) {
					continue;
				}
				const auto &box = desc.DesktopCoordinates;
				const RECT rect = {
					box.left,
					box.top,
					box.right,
					box.bottom,
				};
				RECT overlap = {};
				if (!IntersectRect(&overlap, &rect, &info.rcMonitor)) {
					continue;
				}
				const auto space = desc.ColorSpace;
				_hdrMonitorOn = (
					space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
					|| space == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020
					|| space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
				break;
			}
			adapter->Release();
		}
		factory->Release();
		return _hdrMonitorOn;
	}

	void logHdrSkip(
			const char *reason,
			quint64 video,
			RtxVsrSize source) {
		if (!reason || !std::strcmp(reason, "disabled")) {
			return;
		}
		if (_hdrSkip.reason == reason && _hdrSkip.video == video) {
			return;
		}
		_hdrSkip.reason = reason;
		_hdrSkip.video = video;
		LOG(("RTX HDR Debug: skip reason=%1 source=%2x%3"
			).arg(reason
			).arg(source.width
			).arg(source.height));
	}

	void finishHdr(RtxVsrBackendResult result) {
		const auto before = _hdrSession.state;
		_hdrSession = NoteRtxVsrResult(_hdrSession, result);
		if (!_hdrFailureLogged
			&& before == RtxVsrSessionState::Ready
			&& RtxVsrBlocked(_hdrSession)) {
			_hdrFailureLogged = true;
			LOG(("RTX HDR Error: video HDR is unavailable, "
				"using normal playback."));
		}
	}

	void destroyPopup() {
		if (_popup) {
			if (IsWindow(_popup)) {
				DestroyWindow(_popup);
			}
			_popup = nullptr;
		}
		_popupOwner = nullptr;
		_swap.Reset();
		_swapWidth = _swapHeight = 0;
	}

	void hidePopup(HWND owner) {
		if (!_popup || !IsWindow(_popup)) {
			_popup = nullptr;
			_popupOwner = nullptr;
			return;
		}
		if (owner && _popupOwner != owner) {
			return;
		}
		ShowWindow(_popup, SW_HIDE);
	}

	[[nodiscard]] bool ensurePopup(HWND owner, QRect screen) {
		if (!owner || screen.width() < 2 || screen.height() < 2) {
			return false;
		}
		if (_popup && !IsWindow(_popup)) {
			_popup = nullptr;
			_popupOwner = nullptr;
			_swap.Reset();
		}
		if (_popup && _popupOwner != owner) {
			destroyPopup();
		}
		if (!_popupClass) {
			WNDCLASSEXW windowClass = {};
			windowClass.cbSize = sizeof(windowClass);
			windowClass.lpfnWndProc = DefWindowProcW;
			windowClass.hInstance = GetModuleHandleW(nullptr);
			windowClass.lpszClassName = L"FishgramRtxHdr";
			_popupClass = RegisterClassExW(&windowClass);
			if (!_popupClass && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
				return false;
			}
			_popupClass = 1;
		}
		if (!_popup) {
			_popup = CreateWindowExW(
				WS_EX_NOACTIVATE
					| WS_EX_TOOLWINDOW
					| WS_EX_TRANSPARENT
					| WS_EX_NOREDIRECTIONBITMAP,
				L"FishgramRtxHdr",
				L"",
				WS_POPUP,
				screen.x(),
				screen.y(),
				screen.width(),
				screen.height(),
				owner,
				nullptr,
				GetModuleHandleW(nullptr),
				nullptr);
			if (!_popup) {
				return false;
			}
			_popupOwner = owner;
		}
		SetWindowPos(
			_popup,
			HWND_TOP,
			screen.x(),
			screen.y(),
			screen.width(),
			screen.height(),
			SWP_NOACTIVATE | SWP_NOZORDER);
		return true;
	}

	void applyHoles(QRect screen, const QRegion &holes) {
		if (!_popup) {
			return;
		}
		if (holes.isEmpty()) {
			SetWindowRgn(_popup, nullptr, TRUE);
			return;
		}
		auto region = CreateRectRgn(0, 0, screen.width(), screen.height());
		for (const auto &hole : holes) {
			const auto local = hole.translated(-screen.x(), -screen.y());
			const auto clipped = local.intersected(
				QRect(0, 0, screen.width(), screen.height()));
			if (clipped.isEmpty()) {
				continue;
			}
			auto holeRegion = CreateRectRgn(
				clipped.left(),
				clipped.top(),
				clipped.left() + clipped.width(),
				clipped.top() + clipped.height());
			CombineRgn(region, region, holeRegion, RGN_DIFF);
			DeleteObject(holeRegion);
		}
		SetWindowRgn(_popup, region, TRUE);
	}

	[[nodiscard]] bool ensureSwap(int width, int height) {
		if (width < 2 || height < 2 || !_popup || !_device) {
			return false;
		}
		if (_swap && _swapWidth == width && _swapHeight == height) {
			return true;
		}
		_swap.Reset();
		_swapWidth = _swapHeight = 0;
		IDXGIDevice *dxgi = nullptr;
		if (FAILED(_device->QueryInterface(IID_PPV_ARGS(&dxgi))) || !dxgi) {
			return false;
		}
		IDXGIAdapter *adapter = nullptr;
		const auto adapterHr = dxgi->GetAdapter(&adapter);
		dxgi->Release();
		if (FAILED(adapterHr) || !adapter) {
			if (adapter) {
				adapter->Release();
			}
			return false;
		}
		IDXGIFactory2 *factory = nullptr;
		const auto factoryHr = adapter->GetParent(IID_PPV_ARGS(&factory));
		adapter->Release();
		if (FAILED(factoryHr) || !factory) {
			if (factory) {
				factory->Release();
			}
			return false;
		}
		DXGI_SWAP_CHAIN_DESC1 desc = {};
		desc.Width = UINT(width);
		desc.Height = UINT(height);
		desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		desc.BufferCount = 2;
		desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		desc.Scaling = DXGI_SCALING_STRETCH;
		desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
		IDXGISwapChain1 *swap = nullptr;
		const auto created = factory->CreateSwapChainForHwnd(
			_device.Get(),
			_popup,
			&desc,
			nullptr,
			nullptr,
			&swap);
		factory->Release();
		if (FAILED(created) || !swap) {
			if (swap) {
				swap->Release();
			}
			return false;
		}
		IDXGISwapChain3 *swap3 = nullptr;
		auto colorOk = false;
		if (SUCCEEDED(swap->QueryInterface(IID_PPV_ARGS(&swap3))) && swap3) {
			auto support = UINT(0);
			const auto space = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
			if (SUCCEEDED(swap3->CheckColorSpaceSupport(space, &support))
				&& (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
				colorOk = SUCCEEDED(swap3->SetColorSpace1(space));
			}
			swap3->Release();
		}
		if (!colorOk) {
			swap->Release();
			return false;
		}
		_swap.Reset(swap);
		_swapWidth = width;
		_swapHeight = height;
		return true;
	}

	[[nodiscard]] bool setVsrExtension(bool enable) {
		auto info = NvidiaStreamExtension{ 1u, 2u, enable ? 1u : 0u };
		return SUCCEEDED(_videoContext->VideoProcessorSetStreamExtension(
			_processor.Get(),
			0,
			&kNvidiaPpeInterface,
			UINT(sizeof(info)),
			&info));
	}

	[[nodiscard]] bool setHdrExtension(bool enable) {
		auto info = NvidiaTrueHdrExtension{
			4u,
			3u,
			enable ? 1u : 0u,
		};
		return SUCCEEDED(_videoContext->VideoProcessorSetStreamExtension(
			_processor.Get(),
			0,
			&kNvidiaTrueHdrInterface,
			UINT(sizeof(info)),
			&info));
	}

	[[nodiscard]] bool hdrOutputBlank(int sourceLumaMax) {
		if (sourceLumaMax <= 16 || !_outputStaging || !_output) {
			return false;
		}
		_context->CopyResource(_outputStaging.Get(), _output.Get());
		_context->Flush();
		D3D11_MAPPED_SUBRESOURCE mapped = {};
		if (FAILED(_context->Map(
				_outputStaging.Get(),
				0,
				D3D11_MAP_READ,
				0,
				&mapped))
			|| !mapped.pData) {
			return false;
		}
		const auto *src = static_cast<const unsigned char *>(mapped.pData);
		const auto stepX = (_outWidth > 8) ? (_outWidth / 8) : 1;
		const auto stepY = (_outHeight > 8) ? (_outHeight / 8) : 1;
		auto nonzero = 0;
		for (auto y = 0; y < _outHeight && !nonzero; y += stepY) {
			const auto row = src + (std::size_t(y) * mapped.RowPitch);
			for (auto x = 0; x < _outWidth; x += stepX) {
				unsigned char pixel[8] = {};
				std::memcpy(pixel, row + (std::size_t(x) * 8), 6);
				if (pixel[0] || pixel[1]
					|| pixel[2] || pixel[3]
					|| pixel[4] || pixel[5]) {
					nonzero = 1;
					break;
				}
			}
		}
		_context->Unmap(_outputStaging.Get(), 0);
		return RtxOutputRejectedAsBlank(sourceLumaMax, nonzero);
	}

	[[nodiscard]] bool bltOutput() {
		D3D11_VIDEO_PROCESSOR_STREAM stream = {};
		stream.Enable = TRUE;
		stream.pInputSurface = _inputView.Get();
		const auto hr = _videoContext->VideoProcessorBlt(
			_processor.Get(),
			_outputView.Get(),
			0,
			1,
			&stream);
		if (FAILED(hr)) {
			if (hr == DXGI_ERROR_DEVICE_REMOVED
				|| hr == DXGI_ERROR_DEVICE_RESET) {
				dropDevice();
			}
			return false;
		}
		return true;
	}

	[[nodiscard]] bool copyToSwap() {
		if (!_swap) {
			return false;
		}
		ID3D11Texture2D *buffer = nullptr;
		if (FAILED(_swap->GetBuffer(0, IID_PPV_ARGS(&buffer))) || !buffer) {
			return false;
		}
		_context->CopyResource(buffer, _output.Get());
		buffer->Release();
		const auto presented = _swap->Present(0, 0);
		return SUCCEEDED(presented) || presented == DXGI_STATUS_OCCLUDED;
	}

	[[nodiscard]] static RtxVsrSize evenSize(RtxVsrSize size) {
		if (size.width > 0 && (size.width % 2)) {
			--size.width;
		}
		if (size.height > 0 && (size.height % 2)) {
			--size.height;
		}
		return size;
	}

	[[nodiscard]] bool presentHdr(
			const FrameWithInfo &frame,
			const RtxEnhanceRequest &request) {
		if (request.suspendHdr) {
			hidePopup(reinterpret_cast<HWND>(request.windowId));
			return false;
		}
		auto source = extract(frame);
		source.size = evenSize(source.size);
		const auto hwnd = reinterpret_cast<HWND>(request.windowId);
		const auto displayHdr = windowOnHdrDisplay(hwnd);
		const auto rotated = (request.rotation % 360) != 0;
		const auto reason = RtxHdrSkipReasonFor(
			true,
			hdrSupported(),
			RtxVsrBlocked(_hdrSession),
			displayHdr,
			frame.sourceHdr,
			rotated,
			frame.alpha);
		if (reason != RtxHdrSkipReason::None) {
			if (reason == RtxHdrSkipReason::Unsupported) {
				finishHdr(RtxVsrBackendResult::Unsupported);
			}
			logHdrSkip(
				RtxHdrSkipReasonName(reason),
				request.videoTag,
				source.size);
			hidePopup(hwnd);
			return false;
		}
		if (!onOwnerThread()) {
			logHdrSkip("thread", request.videoTag, source.size);
			return false;
		}
		if (frame.index < 0) {
			logHdrSkip("bad-index", request.videoTag, source.size);
			hidePopup(hwnd);
			return false;
		}
		if (!source.token
			|| source.size.width < 2
			|| source.size.height < 2
			|| request.videoScreenPx.width() < 2
			|| request.videoScreenPx.height() < 2) {
			logHdrSkip("no-frame", request.videoTag, source.size);
			hidePopup(hwnd);
			return false;
		}
		if (_inHdr) {
			return false;
		}
		_inHdr = true;
		struct Flag {
			bool &value;
			~Flag() {
				value = false;
			}
		} guard{ _inHdr };

		const auto outputs = RtxVsrOutputs(
			source.size,
			{
				request.targetPixels.width(),
				request.targetPixels.height(),
			});
		const auto vsr = !RtxVsrBlocked(_session)
			&& supported()
			&& (outputs.count > 0);
		auto output = vsr
			? outputs.sizes[0]
			: evenSize({
				request.videoScreenPx.width(),
				request.videoScreenPx.height(),
			});
		if (output.width < 2 || output.height < 2) {
			logHdrSkip("no-output", request.videoTag, source.size);
			hidePopup(hwnd);
			return false;
		}
		if (!(_hdrSampleSize == source.size)) {
			_hdrSampled = false;
			_hdrSampleSize = source.size;
		}
		if (!ensureDevice()) {
			finishHdr(RtxVsrBackendResult::Failed);
			logHdrSkip("device", request.videoTag, source.size);
			hidePopup(hwnd);
			return false;
		}
		if (deviceRemoved()) {
			dropDevice();
			finishHdr(RtxVsrBackendResult::Failed);
			logHdrSkip("device", request.videoTag, source.size);
			hidePopup(hwnd);
			return false;
		}
		if (!ensurePopup(hwnd, request.videoScreenPx)
			|| !ensureSwap(output.width, output.height)
			|| !ensureProcessor(source, output, true)) {
			finishHdr(RtxVsrBackendResult::Failed);
			logHdrSkip("present", request.videoTag, source.size);
			hidePopup(hwnd);
			return false;
		}
		const auto uploaded = (source.kind == InputKind::Nv12)
			? uploadNv12(source)
			: uploadBgra(source);
		if (!uploaded) {
			logHdrSkip("upload", request.videoTag, source.size);
			hidePopup(hwnd);
			return false;
		}
		auto usedVsr = false;
		if (vsr) {
			usedVsr = setVsrExtension(true);
		}
		if (!setHdrExtension(true)) {
			finishHdr(RtxVsrBackendResult::Unsupported);
			logHdrSkip("driver-rejected", request.videoTag, source.size);
			hidePopup(hwnd);
			return false;
		}
		if (!bltOutput()) {
			auto recovered = false;
			if (usedVsr && _processor) {
				setVsrExtension(false);
				setHdrExtension(true);
				recovered = bltOutput();
				usedVsr = false;
			}
			if (!recovered) {
				finishHdr(RtxVsrBackendResult::Failed);
				logHdrSkip("blt", request.videoTag, source.size);
				hidePopup(hwnd);
				return false;
			}
		}
		if (!_hdrSampled && source.lumaMax > 16) {
			if (hdrOutputBlank(source.lumaMax)) {
				finishHdr(RtxVsrBackendResult::Failed);
				logHdrSkip("blank", request.videoTag, source.size);
				hidePopup(hwnd);
				return false;
			}
			_hdrSampled = true;
		}
		if (!copyToSwap()) {
			finishHdr(RtxVsrBackendResult::Failed);
			logHdrSkip("present", request.videoTag, source.size);
			hidePopup(hwnd);
			return false;
		}
		applyHoles(request.videoScreenPx, request.controlHoles);
		ShowWindow(_popup, SW_SHOWNOACTIVATE);
		finishHdr(RtxVsrBackendResult::Ok);
		if (usedVsr) {
			finish(RtxVsrBackendResult::Ok);
			if (!_loggedActive) {
				_loggedActive = true;
				LOG(("VSR Info: NVIDIA RTX video super resolution is active."));
			}
		}
		if (!_hdrLoggedActive) {
			_hdrLoggedActive = true;
			LOG(("RTX HDR Info: NVIDIA RTX video HDR is active."));
		}
		return true;
	}

	Support _vsrSupported = Support::Unknown;
	Support _hdrSupported = Support::Unknown;
	RtxVsrSession _session;
	RtxVsrSession _hdrSession;
	int _rejectAtOrAbove = 0;
	RtxVsrSize _rejectSource;
	DWORD _thread = 0;
	DebugGate _skipGate;
	DebugGate _upscaleGate;
	bool _loggedActive = false;
	bool _loggedFailure = false;
	InputKind _kind = InputKind::None;
	int _inWidth = 0;
	int _inHeight = 0;
	int _outWidth = 0;
	int _outHeight = 0;
	ComPtr<ID3D11Device> _device;
	ComPtr<ID3D11DeviceContext> _context;
	ComPtr<ID3D11VideoDevice> _videoDevice;
	ComPtr<ID3D11VideoContext> _videoContext;
	ComPtr<ID3D11VideoContext1> _videoContext1;
	ComPtr<ID3D11VideoProcessorEnumerator> _enumerator;
	ComPtr<ID3D11VideoProcessor> _processor;
	ComPtr<ID3D11Texture2D> _input;
	ComPtr<ID3D11Texture2D> _inputStaging;
	ComPtr<ID3D11Texture2D> _output;
	ComPtr<ID3D11Texture2D> _outputStaging;
	ComPtr<ID3D11VideoProcessorInputView> _inputView;
	ComPtr<ID3D11VideoProcessorOutputView> _outputView;
	ComPtr<IDXGISwapChain1> _swap;
	Cache _cache;
	bool _hdrOutput = false;
	bool _hdrLoggedActive = false;
	bool _hdrFailureLogged = false;
	bool _hdrSampled = false;
	bool _inHdr = false;
	bool _hdrMonitorOn = false;
	RtxVsrSize _hdrSampleSize;
	HdrSkip _hdrSkip;
	HMONITOR _hdrMonitor = nullptr;
	ULONGLONG _hdrMonitorTick = 0;
	HWND _popup = nullptr;
	HWND _popupOwner = nullptr;
	int _popupClass = 0;
	int _swapWidth = 0;
	int _swapHeight = 0;

};

bool &EnabledLatch() {
	static auto enabled = false;
	return enabled;
}

bool &HdrEnabledLatch() {
	static auto enabled = false;
	return enabled;
}

#endif // Q_OS_WIN

rpl::variable<bool> &VideoSuperResolutionValue() {
	static auto value = rpl::variable<bool>(
		Core::App().settings().readPref<bool>(
			kRtxVideoSuperResolutionPref,
			kRtxVideoSuperResolutionDefault));
	return value;
}

} // namespace

bool RtxVideoSuperResolutionEnabled() {
	const auto enabled = Core::App().settings().readPref<bool>(
		kRtxVideoSuperResolutionPref,
		kRtxVideoSuperResolutionDefault);
#if defined Q_OS_WIN
	if (enabled && !EnabledLatch()) {
		Processor::Instance().noteToggledOn();
	}
	EnabledLatch() = enabled;
#endif // Q_OS_WIN
	return enabled;
}

rpl::producer<bool> RtxVideoSuperResolutionEnabledValue() {
	return VideoSuperResolutionValue().value();
}

void SetRtxVideoSuperResolutionEnabled(bool enabled) {
	if (RtxVideoSuperResolutionEnabled() == enabled) {
		return;
	}
	Core::App().settings().writePref<bool>(
		kRtxVideoSuperResolutionPref,
		enabled);
	Core::App().saveSettingsDelayed();
	RtxVideoSuperResolutionNoteToggled(enabled);
	VideoSuperResolutionValue() = enabled;
}

bool RtxVideoSuperResolutionSupported() {
#if defined Q_OS_WIN
	return Processor::Instance().supported();
#else // Q_OS_WIN
	return false;
#endif // Q_OS_WIN
}

void RtxVideoSuperResolutionNoteToggled(bool enabled) {
	if (!enabled) {
#if defined Q_OS_WIN
		EnabledLatch() = false;
#endif // Q_OS_WIN
		return;
	}
#if defined Q_OS_WIN
	Processor::Instance().noteToggledOn();
	EnabledLatch() = true;
#endif // Q_OS_WIN
}

void RtxNotePresentation(
		const char *swapFormat,
		int pixelWidth,
		int pixelHeight,
		float devicePixelRatio) {
	auto &note = Presentation();
	note.swap = swapFormat ? swapFormat : "unknown";
	note.width = pixelWidth;
	note.height = pixelHeight;
	note.dpr = devicePixelRatio;
}

QImage RtxSuperResolvedImage(
		const FrameWithInfo &frame,
		QSize targetPixels,
		RtxVsrPath path,
		quint64 videoTag) {
	static auto gate = DebugGate();
	const auto skip = [&](const char *reason) {
		LogSkip(
			gate,
			reason,
			{},
			targetPixels,
			frame.format,
			frame.avPixelFormat,
			path,
			videoTag,
			0,
			DesktopColor());
		return QImage();
	};
	if (frame.format == FrameFormat::None) {
		return skip("bad-format");
	}
#if defined Q_OS_WIN
	if (!RtxVideoSuperResolutionEnabled()) {
		return skip("disabled");
	}
	return Processor::Instance().upscale(
		frame,
		targetPixels,
		path,
		videoTag);
#else // Q_OS_WIN
	if (targetPixels.isEmpty()) {
		return skip("empty-target");
	}
	return skip("platform");
#endif // Q_OS_WIN
}

bool RtxVideoHdrEnabled() {
	const auto enabled = Core::App().settings().readPref<bool>(
		kRtxVideoHdrPref,
		kRtxVideoHdrDefault);
#if defined Q_OS_WIN
	if (enabled && !HdrEnabledLatch()) {
		Processor::Instance().noteHdrToggledOn();
	}
	HdrEnabledLatch() = enabled;
#endif // Q_OS_WIN
	return enabled;
}

bool RtxVideoHdrSupported() {
#if defined Q_OS_WIN
	return Processor::Instance().hdrSupported();
#else // Q_OS_WIN
	return false;
#endif // Q_OS_WIN
}

void RtxVideoHdrNoteToggled(bool enabled) {
	if (!enabled) {
#if defined Q_OS_WIN
		HdrEnabledLatch() = false;
		Processor::Instance().noteHdrToggledOn();
#endif // Q_OS_WIN
		return;
	}
#if defined Q_OS_WIN
	Processor::Instance().noteHdrToggledOn();
	HdrEnabledLatch() = true;
#endif // Q_OS_WIN
}

rpl::event_stream<bool> &HdrToggleStream() {
	static auto stream = rpl::event_stream<bool>();
	return stream;
}

rpl::producer<bool> RtxVideoHdrEnabledValue() {
	return rpl::single(
		RtxVideoHdrEnabled()
	) | rpl::then(HdrToggleStream().events());
}

void RtxVideoHdrSetEnabled(bool enabled) {
	if (enabled == RtxVideoHdrEnabled()) {
		return;
	}
	Core::App().settings().writePref<bool>(kRtxVideoHdrPref, enabled);
	Core::App().saveSettingsDelayed();
	RtxVideoHdrNoteToggled(enabled);
	HdrToggleStream().fire_copy(enabled);
}

bool RtxWindowOnHdrDisplay(quintptr windowId) {
#if defined Q_OS_WIN
	return Processor::Instance().displayIsHdr(windowId);
#else // Q_OS_WIN
	return windowId == 0 && false;
#endif // Q_OS_WIN
}

void RtxHdrReleaseWindow(quintptr windowId) {
#if defined Q_OS_WIN
	Processor::Instance().releaseWindow(windowId);
#else // Q_OS_WIN
	if (windowId) {
		return;
	}
#endif // Q_OS_WIN
}

QRect RtxScreenRect(
		quintptr windowId,
		QRect logicalInTopLevel,
		double devicePixelRatio) {
	const auto scale = devicePixelRatio > 0. ? devicePixelRatio : 1.;
	auto rect = QRect(
		int(std::lround(logicalInTopLevel.x() * scale)),
		int(std::lround(logicalInTopLevel.y() * scale)),
		std::max(int(std::lround(logicalInTopLevel.width() * scale)), 0),
		std::max(int(std::lround(logicalInTopLevel.height() * scale)), 0));
#if defined Q_OS_WIN
	POINT origin = {};
	if (windowId) {
		ClientToScreen(reinterpret_cast<HWND>(windowId), &origin);
	}
	rect.translate(origin.x, origin.y);
#else // Q_OS_WIN
	if (windowId) {
		rect.translate(0, 0);
	}
#endif // Q_OS_WIN
	return rect;
}

QImage RtxEnhancedImage(
		const FrameWithInfo &frame,
		const RtxEnhanceRequest &request) {
	if (frame.format == FrameFormat::None) {
		return {};
	}
#if defined Q_OS_WIN
	const auto hdr = RtxVideoHdrEnabled();
	const auto vsr = RtxVideoSuperResolutionEnabled();
	if (!hdr && !vsr) {
		return {};
	}
	if (vsr && request.targetPixels.isEmpty() && !hdr) {
		return {};
	}
	return Processor::Instance().enhance(frame, request, hdr, vsr);
#else // Q_OS_WIN
	if (request.targetPixels.isEmpty()
		|| !RtxVideoSuperResolutionEnabled()) {
		return {};
	}
	return RtxSuperResolvedImage(
		frame,
		request.targetPixels,
		request.path,
		request.videoTag);
#endif // Q_OS_WIN
}

} // namespace Media::Streaming

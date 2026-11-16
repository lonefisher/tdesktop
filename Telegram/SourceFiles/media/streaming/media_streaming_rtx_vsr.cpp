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

#if defined Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <d3d11_1.h>
#include <dxgi1_4.h>

#ifdef small
#undef small
#endif // small
#endif // Q_OS_WIN

namespace Media::Streaming {
namespace {

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

class Processor {
public:
	static Processor &Instance() {
		static Processor processor;
		return processor;
	}

	[[nodiscard]] bool supported() {
		if (_supported == Support::Unknown) {
			const auto ok = probe();
			if (!ok) {
				dropDevice();
			}
			_supported = ok ? Support::Yes : Support::No;
		}
		return _supported == Support::Yes;
	}

	void noteToggledOn() {
		_session = ResetRtxVsrSession();
		_rejectAtOrAbove = 0;
		_cache = Cache();
		_loggedFailure = false;
		if (_supported == Support::No) {
			_supported = Support::Unknown;
		}
	}

	[[nodiscard]] QImage upscale(
			const FrameWithInfo &frame,
			QSize targetPixels) {
		if (!onOwnerThread()
			|| frame.alpha
			|| frame.index < 0
			|| RtxVsrBlocked(_session)
			|| targetPixels.isEmpty()) {
			return {};
		}
		const auto source = extract(frame);
		if (!source.token) {
			return {};
		}
		if (_cache.valid
			&& _cache.index == frame.index
			&& _cache.token == source.token
			&& _cache.targetWidth == targetPixels.width()
			&& _cache.targetHeight == targetPixels.height()) {
			return _cache.image;
		}
		const auto outputs = RtxVsrOutputs(
			source.size,
			{ targetPixels.width(), targetPixels.height() });
		if (!outputs.count) {
			return {};
		}
		if (!supported()) {
			return {};
		}

		auto image = QImage();
		auto failedEdge = 0;
		for (auto i = 0; i != outputs.count; ++i) {
			const auto size = outputs.sizes[i];
			const auto edge = RtxLongEdge(size);
			if (_rejectAtOrAbove && edge >= _rejectAtOrAbove) {
				continue;
			}
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
				return {};
			}
			failedEdge = edge;
		}
		if (image.isNull()) {
			finish(RtxVsrBackendResult::Failed);
		} else {
			finish(RtxVsrBackendResult::Ok);
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
	}

	void dropDevice() {
		releaseProcessor();
		_videoContext1.Reset();
		_videoContext.Reset();
		_videoDevice.Reset();
		_context.Reset();
		_device.Reset();
		_supported = Support::Unknown;
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
			D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
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
		processor->Release();
		enumerator->Release();
		return SUCCEEDED(hr);
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
			RtxVsrSize output) {
		if (_processor
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
				DXGI_FORMAT_B8G8R8A8_UNORM,
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
				DXGI_FORMAT_B8G8R8A8_UNORM,
				D3D11_BIND_RENDER_TARGET,
				D3D11_USAGE_DEFAULT,
				0)
			&& createTexture(
				&outputStaging,
				output.width,
				output.height,
				DXGI_FORMAT_B8G8R8A8_UNORM,
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
				DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
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
			if (stride >= yuv->size.width()
				&& yuv->y.stride >= yuv->size.width()
				&& yuv->u.stride >= yuv->size.width()) {
				for (auto row = 0; row != yuv->size.height(); ++row) {
					std::memcpy(
						dest + (std::size_t(row) * stride),
						static_cast<const unsigned char *>(yuv->y.data)
							+ (std::size_t(row) * yuv->y.stride),
						yuv->size.width());
				}
				auto *uv = dest + (std::size_t(stride) * yuv->size.height());
				for (auto row = 0; row != (yuv->size.height() / 2); ++row) {
					std::memcpy(
						uv + (std::size_t(row) * stride),
						static_cast<const unsigned char *>(yuv->u.data)
							+ (std::size_t(row) * yuv->u.stride),
						yuv->size.width());
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
				yuv->size.width(),
				yuv->size.height(),
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
		const auto widthBytes = image.width() * 4;
		if (stride < widthBytes) {
			_context->Unmap(_inputStaging.Get(), 0);
			return false;
		}
		for (auto row = 0; row != image.height(); ++row) {
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
		if (!ensureProcessor(source, output)) {
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

	Support _supported = Support::Unknown;
	RtxVsrSession _session;
	int _rejectAtOrAbove = 0;
	DWORD _thread = 0;
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
	Cache _cache;

};

bool &EnabledLatch() {
	static auto enabled = false;
	return enabled;
}

#endif // Q_OS_WIN

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

QImage RtxSuperResolvedImage(
		const FrameWithInfo &frame,
		QSize targetPixels) {
	if (frame.format == FrameFormat::None || targetPixels.isEmpty()) {
		return {};
	}
#if defined Q_OS_WIN
	if (!RtxVideoSuperResolutionEnabled()) {
		return {};
	}
	return Processor::Instance().upscale(frame, targetPixels);
#else // Q_OS_WIN
	return {};
#endif // Q_OS_WIN
}

} // namespace Media::Streaming

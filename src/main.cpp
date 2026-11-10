#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <d3d11_4.h>
#include <dxgi1_3.h>
#include <d2d1_3.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <mmreg.h>
#include <winhttp.h>
#include <wincodec.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <complex>
#include <cstdint>
#include <cwctype>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;
namespace media = winrt::Windows::Media::Control;

namespace {

constexpr wchar_t kWindowClass[] = L"DynamicIslandWindow";
constexpr wchar_t kWindowTitle[] = L"Dynamic Island";
constexpr wchar_t kStartupValue[] = L"DynamicIsland";

constexpr float kCanvasWidth = 460.0f;
constexpr float kCanvasHeight = 225.0f;
constexpr float kCompactHeight = 42.0f;
constexpr float kExpandedWidth = 410.0f;
constexpr float kExpandedHeight = 166.0f;
constexpr float kMediaExpandedHeight = 190.0f;
constexpr float kWeatherExpandedHeight = 180.0f;
constexpr float kTopInset = 5.0f;

constexpr UINT_PTR kAnimationTimer = 1;
constexpr UINT_PTR kStatusTimer = 2;
constexpr UINT_PTR kVisualizerTimer = 3;
constexpr UINT kMediaChanged = WM_APP + 1;
constexpr UINT kWeatherChanged = WM_APP + 2;
constexpr UINT kOutsideMouseDown = WM_APP + 3;

constexpr UINT kMenuExpand = 1001;
constexpr UINT kMenuStartup = 1002;
constexpr UINT kMenuExit = 1003;

HWND gIslandWindow = nullptr;
HHOOK gOutsideClickHook = nullptr;

// Real OS build number (GetVersionEx lies without a matching manifest entry).
DWORD windowsBuildNumber() {
    static const DWORD build = [] {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        OSVERSIONINFOW info{};
        info.dwOSVersionInfoSize = sizeof(info);
        if (const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
            if (const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
                    GetProcAddress(ntdll, "RtlGetVersion"))) {
                if (rtlGetVersion(&info) == 0) return info.dwBuildNumber;
            }
        }
        return DWORD{0};
    }();
    return build;
}

// Per-process loopback capture (AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK)
// shipped in build 20348. Windows 10 22H2 is build 19045, so it needs a fallback.
bool supportsProcessLoopback() {
    return windowsBuildNumber() >= 20348;
}

LRESULT CALLBACK outsideClickHookProcedure(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION
        && (wParam == WM_LBUTTONDOWN
            || wParam == WM_RBUTTONDOWN
            || wParam == WM_MBUTTONDOWN)) {
        const auto* mouse = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        if (mouse && IsWindow(gIslandWindow)) {
            PostMessageW(
                gIslandWindow,
                kOutsideMouseDown,
                static_cast<WPARAM>(mouse->pt.x),
                static_cast<LPARAM>(mouse->pt.y));
        }
    }
    return CallNextHookEx(gOutsideClickHook, code, wParam, lParam);
}

void setOutsideClickTracking(bool enabled) {
    if (enabled && !gOutsideClickHook) {
        gOutsideClickHook = SetWindowsHookExW(
            WH_MOUSE_LL,
            outsideClickHookProcedure,
            GetModuleHandleW(nullptr),
            0);
    } else if (!enabled && gOutsideClickHook) {
        UnhookWindowsHookEx(gOutsideClickHook);
        gOutsideClickHook = nullptr;
    }
}

enum class MediaAction {
    None,
    TogglePlayback,
    Previous,
    Next,
};

enum class IslandTab {
    Media = 0,
    Calendar = 1,
    Weather = 2,
};

struct MediaSnapshot {
    bool available = false;
    bool playing = false;
    int64_t positionSeconds = 0;
    int64_t durationSeconds = 0;
    std::wstring title;
    std::wstring artist;
    uint32_t accentSeed = 0x705CF6u;
    uint64_t artworkKey = 0;
    std::shared_ptr<const std::vector<uint8_t>> artwork;

    bool operator==(const MediaSnapshot& other) const {
        return available == other.available
            && playing == other.playing
            && positionSeconds == other.positionSeconds
            && durationSeconds == other.durationSeconds
            && title == other.title
            && artist == other.artist
            && accentSeed == other.accentSeed
            && artworkKey == other.artworkKey;
    }
};

struct WeatherSnapshot {
    bool loading = true;
    bool available = false;
    bool metric = false;
    int temperature = 0;
    int feelsLike = 0;
    int high = 0;
    int low = 0;
    int humidity = 0;
    int windSpeed = 0;
    int weatherCode = 113;
    std::wstring condition;
    std::wstring location;
    std::wstring error;

    bool operator==(const WeatherSnapshot& other) const {
        return loading == other.loading
            && available == other.available
            && metric == other.metric
            && temperature == other.temperature
            && feelsLike == other.feelsLike
            && high == other.high
            && low == other.low
            && humidity == other.humidity
            && windSpeed == other.windSpeed
            && weatherCode == other.weatherCode
            && condition == other.condition
            && location == other.location
            && error == other.error;
    }
};

struct Spring {
    float value = 0.0f;
    float velocity = 0.0f;
    float target = 0.0f;

    bool step(float dt) {
        constexpr float stiffness = 390.0f;
        constexpr float damping = 31.0f;
        const float acceleration = stiffness * (target - value) - damping * velocity;
        velocity += acceleration * dt;
        value += velocity * dt;

        if (std::abs(target - value) < 0.025f && std::abs(velocity) < 0.025f) {
            value = target;
            velocity = 0.0f;
            return false;
        }
        return true;
    }
};

uint32_t hashText(const std::wstring& value) {
    uint32_t hash = 2166136261u;
    for (const wchar_t character : value) {
        hash ^= static_cast<uint32_t>(character);
        hash *= 16777619u;
    }
    return hash;
}

float clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

D2D1_COLOR_F colorFromHex(uint32_t value, float alpha = 1.0f) {
    return D2D1::ColorF(
        static_cast<float>((value >> 16) & 0xff) / 255.0f,
        static_cast<float>((value >> 8) & 0xff) / 255.0f,
        static_cast<float>(value & 0xff) / 255.0f,
        alpha);
}

D2D1_COLOR_F accentFromSeed(uint32_t seed, float lightnessOffset = 0.0f) {
    const float hue = static_cast<float>(seed % 360u) / 360.0f;
    const float saturation = 0.72f;
    const float lightness = std::clamp(0.57f + lightnessOffset, 0.0f, 1.0f);

    const auto hueToRgb = [](float p, float q, float t) {
        if (t < 0.0f) t += 1.0f;
        if (t > 1.0f) t -= 1.0f;
        if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
        if (t < 1.0f / 2.0f) return q;
        if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
        return p;
    };

    const float q = lightness < 0.5f
        ? lightness * (1.0f + saturation)
        : lightness + saturation - lightness * saturation;
    const float p = 2.0f * lightness - q;
    return D2D1::ColorF(
        hueToRgb(p, q, hue + 1.0f / 3.0f),
        hueToRgb(p, q, hue),
        hueToRgb(p, q, hue - 1.0f / 3.0f),
        1.0f);
}

class SpotifyAudioSession {
public:
    float peak() {
        std::scoped_lock lock(mutex_);
        ensureSessions();
        if (meters_.empty()) return -1.0f;

        float peakValue = 0.0f;
        bool readAny = false;
        for (const auto& meter : meters_) {
            float value = 0.0f;
            if (meter && SUCCEEDED(meter->GetPeakValue(&value))) {
                peakValue = std::max(peakValue, clamp01(value));
                readAny = true;
            }
        }
        if (!readAny) {
            meters_.clear();
            return -1.0f;
        }
        return peakValue;
    }

    float volume() {
        std::scoped_lock lock(mutex_);
        ensureSessions();
        for (const auto& control : volumes_) {
            float value = 0.0f;
            if (control && SUCCEEDED(control->GetMasterVolume(&value))) {
                return clamp01(value);
            }
        }
        return 0.0f;
    }

    bool muted() {
        std::scoped_lock lock(mutex_);
        ensureSessions();
        for (const auto& control : volumes_) {
            BOOL muted = FALSE;
            if (control && SUCCEEDED(control->GetMute(&muted))) {
                return muted != FALSE;
            }
        }
        return false;
    }

    void setVolume(float value) {
        std::scoped_lock lock(mutex_);
        ensureSessions();
        const float clamped = clamp01(value);
        for (const auto& control : volumes_) {
            if (control) {
                control->SetMasterVolume(clamped, nullptr);
            }
        }
    }

    void setMuted(bool muted) {
        std::scoped_lock lock(mutex_);
        ensureSessions();
        for (const auto& control : volumes_) {
            if (control) {
                control->SetMute(muted ? TRUE : FALSE, nullptr);
            }
        }
    }

    DWORD processId() {
        std::scoped_lock lock(mutex_);
        ensureSessions();
        return processId_;
    }

private:
    void ensureSessions() {
        const auto now = std::chrono::steady_clock::now();
        if (volumes_.empty() || now - lastScan_ > 5s) {
            scan();
            lastScan_ = now;
        }
    }

    static bool isSpotifyProcess(DWORD processId) {
        if (processId == 0) return false;
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (!process) return false;

        std::vector<wchar_t> path(1024);
        DWORD length = static_cast<DWORD>(path.size());
        const bool succeeded =
            QueryFullProcessImageNameW(process, 0, path.data(), &length) != FALSE;
        CloseHandle(process);
        if (!succeeded) return false;

        std::wstring processPath(path.data(), length);
        std::transform(
            processPath.begin(),
            processPath.end(),
            processPath.begin(),
            [](wchar_t character) { return static_cast<wchar_t>(towlower(character)); });
        return processPath.find(L"spotify.exe") != std::wstring::npos
            || processPath.find(L"spotifyab.spotifymusic") != std::wstring::npos;
    }

    void scan() {
        meters_.clear();
        volumes_.clear();
        processId_ = 0;

        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(
                __uuidof(MMDeviceEnumerator),
                nullptr,
                CLSCTX_ALL,
                IID_PPV_ARGS(&enumerator)))) {
            return;
        }

        ComPtr<IMMDevice> device;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device))) {
            return;
        }

        ComPtr<IAudioSessionManager2> sessionManager;
        if (FAILED(device->Activate(
                __uuidof(IAudioSessionManager2),
                CLSCTX_ALL,
                nullptr,
                reinterpret_cast<void**>(sessionManager.ReleaseAndGetAddressOf())))) {
            return;
        }

        ComPtr<IAudioSessionEnumerator> sessionEnumerator;
        if (FAILED(sessionManager->GetSessionEnumerator(&sessionEnumerator))) return;

        int count = 0;
        if (FAILED(sessionEnumerator->GetCount(&count))) return;
        for (int index = 0; index < count; ++index) {
            ComPtr<IAudioSessionControl> control;
            if (FAILED(sessionEnumerator->GetSession(index, &control))) continue;

            ComPtr<IAudioSessionControl2> control2;
            if (FAILED(control.As(&control2))) continue;
            DWORD processId = 0;
            if (FAILED(control2->GetProcessId(&processId))
                || !isSpotifyProcess(processId)) {
                continue;
            }

            if (processId_ == 0) {
                processId_ = processId;
            }
            ComPtr<IAudioMeterInformation> meter;
            if (SUCCEEDED(control.As(&meter))) {
                meters_.push_back(std::move(meter));
            }
            ComPtr<ISimpleAudioVolume> volume;
            if (SUCCEEDED(control.As(&volume))) {
                volumes_.push_back(std::move(volume));
            }
        }
    }

    std::vector<ComPtr<IAudioMeterInformation>> meters_;
    std::vector<ComPtr<ISimpleAudioVolume>> volumes_;
    std::chrono::steady_clock::time_point lastScan_{};
    DWORD processId_ = 0;
    std::mutex mutex_;
};

struct SpectrumSnapshot {
    bool active = false;
    std::array<float, 5> bands{};
};

class AudioActivationHandler final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::FtmBase,
          IActivateAudioInterfaceCompletionHandler> {
public:
    AudioActivationHandler()
        : completedEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    ~AudioActivationHandler() override {
        if (completedEvent_) {
            CloseHandle(completedEvent_);
        }
    }

    STDMETHODIMP ActivateCompleted(
        IActivateAudioInterfaceAsyncOperation* operation) override {
        ComPtr<IUnknown> activatedInterface;
        HRESULT activationResult = E_FAIL;
        const HRESULT operationResult = operation->GetActivateResult(
            &activationResult,
            activatedInterface.GetAddressOf());
        result_ = FAILED(operationResult) ? operationResult : activationResult;
        if (SUCCEEDED(result_)) {
            result_ = activatedInterface.As(&audioClient_);
        }
        if (completedEvent_) {
            SetEvent(completedEvent_);
        }
        return S_OK;
    }

    HRESULT waitForResult(
        std::stop_token stopToken,
        ComPtr<IAudioClient>& audioClient) {
        if (!completedEvent_) return HRESULT_FROM_WIN32(GetLastError());

        while (!stopToken.stop_requested()) {
            const DWORD waitResult = WaitForSingleObject(completedEvent_, 100);
            if (waitResult == WAIT_OBJECT_0) {
                if (SUCCEEDED(result_)) {
                    audioClient = audioClient_;
                }
                return result_;
            }
            if (waitResult == WAIT_FAILED) {
                return HRESULT_FROM_WIN32(GetLastError());
            }
        }
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }

private:
    HANDLE completedEvent_ = nullptr;
    HRESULT result_ = E_PENDING;
    ComPtr<IAudioClient> audioClient_;
};

class SpotifySpectrumCapture {
public:
    explicit SpotifySpectrumCapture(SpotifyAudioSession& spotifySession)
        : spotifySession_(spotifySession),
          thread_([this](std::stop_token stopToken) { run(stopToken); }) {}

    ~SpotifySpectrumCapture() {
        thread_.request_stop();
    }

    SpectrumSnapshot snapshot() const {
        std::scoped_lock lock(mutex_);
        return snapshot_;
    }

private:
    static constexpr size_t kFftSize = 1024;
    static constexpr size_t kFftHop = kFftSize / 2;
    static constexpr uint32_t kSampleRate = 44100;

    static void fft(std::array<std::complex<float>, kFftSize>& values) {
        for (size_t index = 1, reversed = 0; index < kFftSize; ++index) {
            size_t bit = kFftSize >> 1;
            for (; reversed & bit; bit >>= 1) {
                reversed ^= bit;
            }
            reversed ^= bit;
            if (index < reversed) {
                std::swap(values[index], values[reversed]);
            }
        }

        constexpr float pi = 3.14159265358979323846f;
        for (size_t length = 2; length <= kFftSize; length <<= 1) {
            const float angle = -2.0f * pi / static_cast<float>(length);
            const std::complex<float> root(std::cos(angle), std::sin(angle));
            for (size_t start = 0; start < kFftSize; start += length) {
                std::complex<float> factor(1.0f, 0.0f);
                for (size_t offset = 0; offset < length / 2; ++offset) {
                    const auto even = values[start + offset];
                    const auto odd =
                        values[start + offset + length / 2] * factor;
                    values[start + offset] = even + odd;
                    values[start + offset + length / 2] = even - odd;
                    factor *= root;
                }
            }
        }
    }

    void publish(bool active, const std::array<float, 5>& bands = {}) {
        std::scoped_lock lock(mutex_);
        snapshot_.active = active;
        snapshot_.bands = bands;
    }

    void analyzeWindow(const std::vector<float>& samples) {
        constexpr float pi = 3.14159265358979323846f;
        std::array<std::complex<float>, kFftSize> spectrum{};
        for (size_t index = 0; index < kFftSize; ++index) {
            const float window =
                0.5f
                - 0.5f * std::cos(
                    2.0f * pi * static_cast<float>(index)
                    / static_cast<float>(kFftSize - 1));
            spectrum[index] = samples[index] * window;
        }
        fft(spectrum);

        static constexpr std::array<std::pair<float, float>, 5> ranges{{
            {45.0f, 160.0f},
            {160.0f, 400.0f},
            {400.0f, 1000.0f},
            {1000.0f, 3000.0f},
            {3000.0f, 9000.0f},
        }};
        static constexpr std::array<float, 5> perceptualGain{
            1.00f, 1.08f, 1.18f, 1.30f, 1.42f};

        std::array<float, 5> next{};
        for (size_t band = 0; band < ranges.size(); ++band) {
            const size_t firstBin = std::max<size_t>(
                1,
                static_cast<size_t>(
                    std::floor(ranges[band].first * kFftSize / static_cast<float>(sampleRate_))));
            const size_t lastBin = std::min<size_t>(
                kFftSize / 2,
                static_cast<size_t>(
                    std::ceil(ranges[band].second * kFftSize / static_cast<float>(sampleRate_))));

            float energy = 0.0f;
            size_t binCount = 0;
            for (size_t bin = firstBin; bin <= lastBin; ++bin) {
                const float magnitude =
                    std::abs(spectrum[bin])
                    / (static_cast<float>(kFftSize) * 0.25f);
                energy += magnitude * magnitude;
                ++binCount;
            }

            const float rms =
                std::sqrt(energy / static_cast<float>(std::max<size_t>(1, binCount)))
                * perceptualGain[band];
            const float decibels = 20.0f * std::log10(std::max(rms, 0.00001f));
            const float level = clamp01((decibels + 62.0f) / 54.0f);
            const float shaped = std::pow(level, 0.72f);
            const float smoothing =
                shaped >= smoothedBands_[band] ? 0.58f : 0.20f;
            smoothedBands_[band] +=
                (shaped - smoothedBands_[band]) * smoothing;
            next[band] = smoothedBands_[band];
        }
        publish(true, next);
    }

    HRESULT activateProcessLoopback(
        DWORD processId,
        std::stop_token stopToken,
        ComPtr<IAudioClient>& audioClient) {
        AUDIOCLIENT_ACTIVATION_PARAMS activationParams{};
        activationParams.ActivationType =
            AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        activationParams.ProcessLoopbackParams.TargetProcessId = processId;
        activationParams.ProcessLoopbackParams.ProcessLoopbackMode =
            PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

        PROPVARIANT activateParams{};
        activateParams.vt = VT_BLOB;
        activateParams.blob.cbSize = sizeof(activationParams);
        activateParams.blob.pBlobData =
            reinterpret_cast<BYTE*>(&activationParams);

        const auto handler = Microsoft::WRL::Make<AudioActivationHandler>();
        if (!handler) return E_OUTOFMEMORY;

        ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
        const HRESULT startResult = ActivateAudioInterfaceAsync(
            VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
            __uuidof(IAudioClient),
            &activateParams,
            handler.Get(),
            operation.GetAddressOf());
        if (FAILED(startResult)) return startResult;
        return handler->waitForResult(stopToken, audioClient);
    }

    // Windows 10 fallback: loop back the whole default render endpoint. Other
    // apps' audio is mixed in, so frames are only analyzed while Spotify's own
    // session meter reports sound (see the gate in capture()).
    static HRESULT activateEndpointLoopback(ComPtr<IAudioClient>& audioClient) {
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT result = CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator));
        if (FAILED(result)) return result;

        ComPtr<IMMDevice> device;
        result = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
        if (FAILED(result)) return result;

        return device->Activate(
            __uuidof(IAudioClient),
            CLSCTX_ALL,
            nullptr,
            reinterpret_cast<void**>(audioClient.ReleaseAndGetAddressOf()));
    }

    enum class SampleKind { Int16, Int32, Float32 };

    static float readSample(const BYTE* frame, SampleKind kind, UINT32 channel) {
        switch (kind) {
        case SampleKind::Int16:
            return static_cast<float>(
                reinterpret_cast<const int16_t*>(frame)[channel]) / 32768.0f;
        case SampleKind::Int32:
            return static_cast<float>(
                reinterpret_cast<const int32_t*>(frame)[channel]) / 2147483648.0f;
        case SampleKind::Float32:
            return reinterpret_cast<const float*>(frame)[channel];
        }
        return 0.0f;
    }

    HRESULT capture(DWORD processId, std::stop_token stopToken) {
        const bool processLoopback = supportsProcessLoopback();
        ComPtr<IAudioClient> audioClient;
        HRESULT result = processLoopback
            ? activateProcessLoopback(processId, stopToken, audioClient)
            : activateEndpointLoopback(audioClient);
        if (FAILED(result)) return result;

        SampleKind sampleKind = SampleKind::Int16;
        UINT32 channels = 2;
        UINT32 frameBytes = 4;
        DWORD streamFlags =
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

        WAVEFORMATEX requestedFormat{};
        WAVEFORMATEX* mixFormat = nullptr;
        const WAVEFORMATEX* format = &requestedFormat;
        if (processLoopback) {
            // The process loopback device has no mix format; ask for 16-bit
            // stereo and let the engine convert.
            requestedFormat.wFormatTag = WAVE_FORMAT_PCM;
            requestedFormat.nChannels = 2;
            requestedFormat.nSamplesPerSec = kSampleRate;
            requestedFormat.wBitsPerSample = 16;
            requestedFormat.nBlockAlign =
                requestedFormat.nChannels * requestedFormat.wBitsPerSample / 8;
            requestedFormat.nAvgBytesPerSec =
                requestedFormat.nSamplesPerSec * requestedFormat.nBlockAlign;
            streamFlags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            sampleRate_ = kSampleRate;
        } else {
            // Endpoint loopback must use the engine's mix format as-is.
            result = audioClient->GetMixFormat(&mixFormat);
            if (FAILED(result)) return result;
            format = mixFormat;

            WORD tag = mixFormat->wFormatTag;
            if (tag == WAVE_FORMAT_EXTENSIBLE
                && mixFormat->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
                tag = static_cast<WORD>(
                    reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mixFormat)
                        ->SubFormat.Data1);
            }
            if (tag == WAVE_FORMAT_IEEE_FLOAT && mixFormat->wBitsPerSample == 32) {
                sampleKind = SampleKind::Float32;
            } else if (tag == WAVE_FORMAT_PCM && mixFormat->wBitsPerSample == 16) {
                sampleKind = SampleKind::Int16;
            } else if (tag == WAVE_FORMAT_PCM && mixFormat->wBitsPerSample == 32) {
                sampleKind = SampleKind::Int32;
            } else {
                CoTaskMemFree(mixFormat);
                return AUDCLNT_E_UNSUPPORTED_FORMAT;
            }
            channels = std::max<UINT32>(1, mixFormat->nChannels);
            sampleRate_ = mixFormat->nSamplesPerSec;
        }
        frameBytes = format->nBlockAlign;

        result = audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            streamFlags,
            0,
            0,
            format,
            nullptr);
        if (mixFormat) CoTaskMemFree(mixFormat);
        if (FAILED(result)) return result;

        ComPtr<IAudioCaptureClient> captureClient;
        result = audioClient->GetService(IID_PPV_ARGS(&captureClient));
        if (FAILED(result)) return result;

        const HANDLE sampleReady = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!sampleReady) return HRESULT_FROM_WIN32(GetLastError());
        result = audioClient->SetEventHandle(sampleReady);
        if (FAILED(result)) {
            CloseHandle(sampleReady);
            return result;
        }

        const HANDLE process =
            OpenProcess(SYNCHRONIZE, FALSE, processId);
        result = audioClient->Start();
        if (FAILED(result)) {
            if (process) CloseHandle(process);
            CloseHandle(sampleReady);
            return result;
        }

        std::vector<float> samples;
        samples.reserve(kFftSize * 2);
        bool captureFailed = false;
        bool spotifyAudible = processLoopback;
        auto lastGateCheck = std::chrono::steady_clock::time_point{};
        while (!stopToken.stop_requested()) {
            // Endpoint loopback may not signal while the device is idle, so
            // poll it more often instead of relying only on the event.
            const DWORD waitResult =
                WaitForSingleObject(sampleReady, processLoopback ? 100 : 20);
            if (waitResult == WAIT_FAILED) {
                captureFailed = true;
                break;
            }
            if (process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
                break;
            }
            if (processLoopback && waitResult != WAIT_OBJECT_0) continue;

            if (!processLoopback) {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastGateCheck > 60ms) {
                    lastGateCheck = now;
                    spotifyAudible = spotifySession_.peak() > 0.002f;
                }
            }

            UINT32 packetFrames = 0;
            while (SUCCEEDED(result = captureClient->GetNextPacketSize(
                       &packetFrames))
                   && packetFrames > 0) {
                BYTE* data = nullptr;
                UINT32 frameCount = 0;
                DWORD bufferFlags = 0;
                result = captureClient->GetBuffer(
                    &data,
                    &frameCount,
                    &bufferFlags,
                    nullptr,
                    nullptr);
                if (FAILED(result)) {
                    captureFailed = true;
                    break;
                }

                const bool silent =
                    !spotifyAudible
                    || (bufferFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                const UINT32 mixChannels = std::min<UINT32>(channels, 2);
                for (UINT32 frame = 0; frame < frameCount; ++frame) {
                    float mono = 0.0f;
                    if (!silent) {
                        const BYTE* frameData = data + frame * frameBytes;
                        for (UINT32 channel = 0; channel < mixChannels; ++channel) {
                            mono += readSample(frameData, sampleKind, channel);
                        }
                        mono /= static_cast<float>(mixChannels);
                    }
                    samples.push_back(mono);
                }
                captureClient->ReleaseBuffer(frameCount);

                while (samples.size() >= kFftSize) {
                    analyzeWindow(samples);
                    samples.erase(samples.begin(), samples.begin() + kFftHop);
                }
            }
            if (FAILED(result) || captureFailed) {
                captureFailed = true;
                break;
            }
        }

        audioClient->Stop();
        if (process) CloseHandle(process);
        CloseHandle(sampleReady);
        return captureFailed ? result : S_OK;
    }

    void run(std::stop_token stopToken) {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        while (!stopToken.stop_requested()) {
            const DWORD processId = spotifySession_.processId();
            if (processId == 0) {
                smoothedBands_.fill(0.0f);
                publish(false);
                for (int wait = 0; wait < 10 && !stopToken.stop_requested(); ++wait) {
                    std::this_thread::sleep_for(100ms);
                }
                continue;
            }

            const HRESULT captureResult = capture(processId, stopToken);
            smoothedBands_.fill(0.0f);
            publish(false);
            if (stopToken.stop_requested()) break;

            const auto retryDelay =
                FAILED(captureResult) ? 1500ms : 400ms;
            const int steps = static_cast<int>(retryDelay / 100ms);
            for (int wait = 0; wait < steps && !stopToken.stop_requested(); ++wait) {
                std::this_thread::sleep_for(100ms);
            }
        }
    }

    SpotifyAudioSession& spotifySession_;
    std::array<float, 5> smoothedBands_{};
    uint32_t sampleRate_ = kSampleRate;
    mutable std::mutex mutex_;
    SpectrumSnapshot snapshot_;
    std::jthread thread_;
};

class MediaController {
public:
    explicit MediaController(HWND owner) : owner_(owner) {
        thread_ = std::jthread([this](std::stop_token stopToken) {
            run(stopToken);
        });
    }

    ~MediaController() {
        thread_.request_stop();
    }

    MediaSnapshot snapshot() const {
        std::scoped_lock lock(mutex_);
        return snapshot_;
    }

    void request(MediaAction action) {
        action_.store(action, std::memory_order_release);
    }

private:
    static bool isSpotifySession(
        const media::GlobalSystemMediaTransportControlsSession& session) {
        try {
            std::wstring source = session.SourceAppUserModelId().c_str();
            std::transform(
                source.begin(),
                source.end(),
                source.begin(),
                [](wchar_t character) {
                    return static_cast<wchar_t>(towlower(character));
                });
            return source.find(L"spotify") != std::wstring::npos;
        } catch (...) {
            return false;
        }
    }

    static media::GlobalSystemMediaTransportControlsSession findSpotifySession(
        const media::GlobalSystemMediaTransportControlsSessionManager& manager) {
        for (const auto& session : manager.GetSessions()) {
            if (isSpotifySession(session)) return session;
        }
        return nullptr;
    }

    static std::shared_ptr<const std::vector<uint8_t>> readArtwork(
        const media::GlobalSystemMediaTransportControlsSessionMediaProperties& properties) {
        try {
            const auto thumbnail = properties.Thumbnail();
            if (!thumbnail) return {};
            const auto stream = thumbnail.OpenReadAsync().get();
            const uint64_t streamSize = std::min<uint64_t>(stream.Size(), 8 * 1024 * 1024);
            if (streamSize == 0) return {};

            winrt::Windows::Storage::Streams::DataReader reader(stream.GetInputStreamAt(0));
            const uint32_t loaded =
                reader.LoadAsync(static_cast<uint32_t>(streamSize)).get();
            if (loaded == 0) return {};

            auto bytes = std::make_shared<std::vector<uint8_t>>(loaded);
            reader.ReadBytes(winrt::array_view<uint8_t>(
                bytes->data(),
                bytes->data() + bytes->size()));
            return bytes;
        } catch (...) {
            return {};
        }
    }

    void publish(MediaSnapshot next) {
        bool changed = false;
        {
            std::scoped_lock lock(mutex_);
            if (!(snapshot_ == next)) {
                snapshot_ = std::move(next);
                changed = true;
            }
        }
        if (changed && IsWindow(owner_)) {
            PostMessageW(owner_, kMediaChanged, 0, 0);
        }
    }

    void run(std::stop_token stopToken) {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            const auto manager =
                media::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            uint64_t cachedArtworkKey = 0;
            std::shared_ptr<const std::vector<uint8_t>> cachedArtwork;

            while (!stopToken.stop_requested()) {
                MediaSnapshot next;
                const auto session = findSpotifySession(manager);

                if (session) {
                    next.available = true;
                    try {
                        const auto properties = session.TryGetMediaPropertiesAsync().get();
                        next.title = properties.Title().c_str();
                        next.artist = properties.Artist().c_str();
                        if (next.title.empty()) next.title = L"Spotify";
                        if (next.artist.empty()) next.artist = L"Ready to play";
                        next.accentSeed = hashText(next.title + next.artist);
                        next.artworkKey = static_cast<uint64_t>(next.accentSeed);
                        if (next.artworkKey != cachedArtworkKey) {
                            cachedArtwork = readArtwork(properties);
                            cachedArtworkKey = next.artworkKey;
                        }
                        next.artwork = cachedArtwork;
                    } catch (...) {
                        next.title = L"Spotify";
                        next.artist = L"Ready to play";
                    }

                    try {
                        next.playing = session.GetPlaybackInfo().PlaybackStatus()
                            == media::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                    } catch (...) {
                        next.playing = false;
                    }

                    try {
                        const auto timeline = session.GetTimelineProperties();
                        constexpr int64_t ticksPerSecond = 10'000'000;
                        const int64_t startTicks = timeline.StartTime().count();
                        const int64_t endTicks = timeline.EndTime().count();
                        const int64_t positionTicks = timeline.Position().count();
                        next.durationSeconds = std::max<int64_t>(
                            0,
                            (endTicks - startTicks) / ticksPerSecond);
                        next.positionSeconds = std::clamp<int64_t>(
                            (positionTicks - startTicks) / ticksPerSecond,
                            0,
                            next.durationSeconds);
                    } catch (...) {
                        next.positionSeconds = 0;
                        next.durationSeconds = 0;
                    }

                    const MediaAction requested =
                        action_.exchange(MediaAction::None, std::memory_order_acq_rel);
                    try {
                        switch (requested) {
                        case MediaAction::TogglePlayback:
                            if (next.playing) {
                                session.TryPauseAsync().get();
                            } else {
                                session.TryPlayAsync().get();
                            }
                            break;
                        case MediaAction::Previous:
                            session.TrySkipPreviousAsync().get();
                            break;
                        case MediaAction::Next:
                            session.TrySkipNextAsync().get();
                            break;
                        case MediaAction::None:
                            break;
                        }
                    } catch (...) {
                        // Some media sources expose a session but reject one or more controls.
                    }
                } else {
                    action_.store(MediaAction::None, std::memory_order_release);
                    cachedArtworkKey = 0;
                    cachedArtwork.reset();
                }

                publish(std::move(next));

                for (int tick = 0; tick < 6 && !stopToken.stop_requested(); ++tick) {
                    std::this_thread::sleep_for(100ms);
                }
            }
        } catch (...) {
            publish({});
        }
    }

    HWND owner_ = nullptr;
    mutable std::mutex mutex_;
    MediaSnapshot snapshot_;
    std::atomic<MediaAction> action_{MediaAction::None};
    std::jthread thread_;
};

std::wstring moduleFilePath(const wchar_t* fileName) {
    std::vector<wchar_t> path(512);
    DWORD length = 0;
    while (true) {
        length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return fileName;
        if (length < path.size() - 1) break;
        path.resize(path.size() * 2);
    }

    std::wstring directory(path.data(), length);
    const size_t separator = directory.find_last_of(L"\\/");
    if (separator != std::wstring::npos) {
        directory.resize(separator + 1);
    } else {
        directory.clear();
    }
    return directory + fileName;
}

std::wstring readWeatherSetting(const wchar_t* name, const wchar_t* fallback) {
    wchar_t value[256]{};
    GetPrivateProfileStringW(
        L"weather",
        name,
        fallback,
        value,
        ARRAYSIZE(value),
        moduleFilePath(L"settings.ini").c_str());
    std::wstring result = value;
    const auto first = result.find_first_not_of(L" \t\r\n");
    const auto last = result.find_last_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return fallback;
    result = result.substr(first, last - first + 1);
    return result;
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        count,
        nullptr,
        nullptr);
    return result;
}

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        count);
    return result;
}

std::wstring urlEncodePathComponent(const std::wstring& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    const std::string utf8 = wideToUtf8(value);
    std::string encoded;
    encoded.reserve(utf8.size() * 3);
    for (const unsigned char character : utf8) {
        if ((character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '-' || character == '_' || character == '.' || character == '~') {
            encoded.push_back(static_cast<char>(character));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[(character >> 4) & 0x0f]);
            encoded.push_back(hex[character & 0x0f]);
        }
    }
    return std::wstring(encoded.begin(), encoded.end());
}

std::string jsonStringValue(const std::string& json, const std::string& key) {
    const std::string token = "\"" + key + "\"";
    size_t position = json.find(token);
    if (position == std::string::npos) return {};
    position = json.find(':', position + token.size());
    if (position == std::string::npos) return {};
    position = json.find('"', position + 1);
    if (position == std::string::npos) return {};
    ++position;

    std::string result;
    while (position < json.size()) {
        const char character = json[position++];
        if (character == '"') break;
        if (character == '\\' && position < json.size()) {
            const char escaped = json[position++];
            switch (escaped) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            default: result.push_back('?'); break;
            }
        } else {
            result.push_back(character);
        }
    }
    return result;
}

int jsonIntegerValue(const std::string& json, const std::string& key, int fallback = 0) {
    const std::string value = jsonStringValue(json, key);
    if (value.empty()) return fallback;
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

class InternetHandle {
public:
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET handle) : handle_(handle) {}
    ~InternetHandle() {
        if (handle_) WinHttpCloseHandle(handle_);
    }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    operator HINTERNET() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

private:
    HINTERNET handle_ = nullptr;
};

bool downloadWeatherJson(const std::wstring& location, std::string& response) {
    InternetHandle session(WinHttpOpen(
        L"DynamicIsland/1.1",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session) return false;
    WinHttpSetTimeouts(session, 4000, 5000, 5000, 6000);

    InternetHandle connection(WinHttpConnect(
        session,
        L"wttr.in",
        INTERNET_DEFAULT_HTTPS_PORT,
        0));
    if (!connection) return false;

    const std::wstring path =
        location.empty() || _wcsicmp(location.c_str(), L"auto") == 0
            ? L"/?format=j1"
            : L"/" + urlEncodePathComponent(location) + L"?format=j1";
    InternetHandle request(WinHttpOpenRequest(
        connection,
        L"GET",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (!request) return false;

    static constexpr wchar_t headers[] =
        L"Accept: application/json\r\n"
        L"Cache-Control: no-cache\r\n";
    if (!WinHttpAddRequestHeaders(
            request,
            headers,
            static_cast<DWORD>(-1L),
            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
        return false;
    }
    if (!WinHttpSendRequest(
            request,
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0)
        || !WinHttpReceiveResponse(request, nullptr)) {
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX)
        || statusCode != 200) {
        return false;
    }

    response.clear();
    std::array<char, 8192> buffer{};
    while (response.size() < 1024 * 1024) {
        DWORD bytesRead = 0;
        if (!WinHttpReadData(
                request,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &bytesRead)) {
            return false;
        }
        if (bytesRead == 0) break;
        response.append(buffer.data(), bytesRead);
    }
    return !response.empty();
}

bool localeUsesMetricWeather() {
    wchar_t measurement[8]{};
    if (!GetLocaleInfoEx(
            LOCALE_NAME_USER_DEFAULT,
            LOCALE_IMEASURE,
            measurement,
            ARRAYSIZE(measurement))) {
        return false;
    }
    return measurement[0] != L'1';
}

class WeatherController {
public:
    explicit WeatherController(HWND owner) : owner_(owner) {
        thread_ = std::jthread([this](std::stop_token stopToken) {
            run(stopToken);
        });
    }

    ~WeatherController() {
        thread_.request_stop();
        wake_.notify_all();
    }

    WeatherSnapshot snapshot() const {
        std::scoped_lock lock(mutex_);
        return snapshot_;
    }

    void requestRefresh() {
        refreshRequested_.store(true, std::memory_order_release);
        wake_.notify_all();
    }

private:
    static WeatherSnapshot fetch() {
        WeatherSnapshot result;
        result.loading = false;

        const std::wstring locationSetting = readWeatherSetting(L"location", L"auto");
        std::wstring unitsSetting = readWeatherSetting(L"units", L"auto");
        std::transform(
            unitsSetting.begin(),
            unitsSetting.end(),
            unitsSetting.begin(),
            [](wchar_t character) { return static_cast<wchar_t>(towlower(character)); });
        result.metric = unitsSetting == L"celsius"
            || (unitsSetting == L"auto" && localeUsesMetricWeather());

        std::string json;
        if (!downloadWeatherJson(locationSetting, json)) {
            result.error = L"Check your connection or weather location.";
            return result;
        }

        const size_t currentStart = json.find("\"current_condition\"");
        const size_t nearestStart = json.find("\"nearest_area\"", currentStart);
        const size_t forecastStart = json.find("\"weather\"", nearestStart);
        if (currentStart == std::string::npos
            || nearestStart == std::string::npos
            || forecastStart == std::string::npos) {
            result.error = L"The weather service returned an unexpected response.";
            return result;
        }

        const std::string current = json.substr(currentStart, nearestStart - currentStart);
        const std::string nearest = json.substr(nearestStart, forecastStart - nearestStart);
        const std::string forecast = json.substr(forecastStart);
        const char* temperatureKey = result.metric ? "temp_C" : "temp_F";
        const char* feelsKey = result.metric ? "FeelsLikeC" : "FeelsLikeF";
        const char* highKey = result.metric ? "maxtempC" : "maxtempF";
        const char* lowKey = result.metric ? "mintempC" : "mintempF";
        const char* windKey = result.metric ? "windspeedKmph" : "windspeedMiles";

        result.temperature = jsonIntegerValue(current, temperatureKey);
        result.feelsLike = jsonIntegerValue(current, feelsKey);
        result.high = jsonIntegerValue(forecast, highKey);
        result.low = jsonIntegerValue(forecast, lowKey);
        result.humidity = jsonIntegerValue(current, "humidity");
        result.windSpeed = jsonIntegerValue(current, windKey);
        result.weatherCode = jsonIntegerValue(current, "weatherCode", 113);
        result.condition = utf8ToWide(jsonStringValue(current, "value"));
        result.location = utf8ToWide(jsonStringValue(nearest, "value"));
        if (result.condition.empty()) result.condition = L"Current conditions";
        if (result.location.empty()) {
            result.location =
                _wcsicmp(locationSetting.c_str(), L"auto") == 0
                    ? L"Local weather"
                    : locationSetting;
        }
        result.available = true;
        return result;
    }

    void publish(WeatherSnapshot next) {
        bool changed = false;
        {
            std::scoped_lock lock(mutex_);
            if (!(snapshot_ == next)) {
                snapshot_ = std::move(next);
                changed = true;
            }
        }
        if (changed && IsWindow(owner_)) {
            PostMessageW(owner_, kWeatherChanged, 0, 0);
        }
    }

    void run(std::stop_token stopToken) {
        while (!stopToken.stop_requested()) {
            publish(fetch());
            refreshRequested_.store(false, std::memory_order_release);

            std::unique_lock lock(waitMutex_);
            wake_.wait_for(
                lock,
                stopToken,
                15min,
                [this] { return refreshRequested_.load(std::memory_order_acquire); });
        }
    }

    HWND owner_ = nullptr;
    mutable std::mutex mutex_;
    std::mutex waitMutex_;
    std::condition_variable_any wake_;
    WeatherSnapshot snapshot_;
    std::atomic<bool> refreshRequested_{false};
    std::jthread thread_;
};

class DynamicIslandApp {
public:
    explicit DynamicIslandApp(HWND window)
        : window_(window),
          spotifySpectrumCapture_(spotifyAudioSession_),
          mediaController_(window),
          weatherController_(window) {
        width_.value = compactWidth();
        width_.target = width_.value;
        height_.value = kCompactHeight;
        height_.target = height_.value;
        hover_.value = 0.0f;
        hover_.target = 0.0f;
        tabIndicator_.value = 0.0f;
        tabIndicator_.target = 0.0f;
        contentOpacity_.value = 1.0f;
        contentOpacity_.target = 1.0f;
        SYSTEMTIME today{};
        GetLocalTime(&today);
        calendarYear_ = today.wYear;
        calendarMonth_ = today.wMonth;
        calendarSelectedDay_ = today.wDay;
        updateStatus();
    }

    ~DynamicIslandApp() {
        KillTimer(window_, kAnimationTimer);
        KillTimer(window_, kStatusTimer);
        KillTimer(window_, kVisualizerTimer);
        setOutsideClickTracking(false);
    }

    HRESULT initializeGraphics() {
        RECT client{};
        GetClientRect(window_, &client);
        pixelWidth_ = static_cast<UINT>(client.right - client.left);
        pixelHeight_ = static_cast<UINT>(client.bottom - client.top);

        UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
        deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL featureLevel{};
        static constexpr D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };

        HRESULT result = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            deviceFlags,
            levels,
            ARRAYSIZE(levels),
            D3D11_SDK_VERSION,
            &d3dDevice_,
            &featureLevel,
            &d3dContext_);
        if (FAILED(result)) {
            deviceFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
            result = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                deviceFlags,
                levels,
                ARRAYSIZE(levels),
                D3D11_SDK_VERSION,
                &d3dDevice_,
                &featureLevel,
                &d3dContext_);
        }
        if (FAILED(result)) return result;

        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(result = d3dDevice_.As(&dxgiDevice))) return result;

        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(result = dxgiDevice->GetAdapter(&adapter))) return result;

        ComPtr<IDXGIFactory2> dxgiFactory;
        if (FAILED(result = adapter->GetParent(IID_PPV_ARGS(&dxgiFactory)))) return result;

        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = pixelWidth_;
        description.Height = pixelHeight_;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.Stereo = FALSE;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

        if (FAILED(result = dxgiFactory->CreateSwapChainForComposition(
                d3dDevice_.Get(), &description, nullptr, &swapChain_))) {
            return result;
        }

        D2D1_FACTORY_OPTIONS factoryOptions{};
#ifdef _DEBUG
        factoryOptions.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
        if (FAILED(result = D2D1CreateFactory(
                D2D1_FACTORY_TYPE_SINGLE_THREADED,
                __uuidof(ID2D1Factory3),
                &factoryOptions,
                &d2dFactory_))) {
            return result;
        }

        if (FAILED(result = d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_))) {
            return result;
        }
        if (FAILED(result = d2dDevice_->CreateDeviceContext(
                D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext_))) {
            return result;
        }

        ComPtr<IDXGISurface> surface;
        if (FAILED(result = swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return result;

        const float dpi = static_cast<float>(GetDpiForWindow(window_));
        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(
                DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED),
            dpi,
            dpi);
        if (FAILED(result = d2dContext_->CreateBitmapFromDxgiSurface(
                surface.Get(), &properties, &targetBitmap_))) {
            return result;
        }
        d2dContext_->SetTarget(targetBitmap_.Get());
        d2dContext_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        d2dContext_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        if (FAILED(CoCreateInstance(
                CLSID_WICImagingFactory2,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&wicFactory_)))) {
            CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&wicFactory_));
        }

        if (FAILED(result = DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                &textFactory_))) {
            return result;
        }
        if (FAILED(result = createTextFormats())) return result;

        if (FAILED(result = DCompositionCreateDevice(
                dxgiDevice.Get(), __uuidof(IDCompositionDevice), &compositionDevice_))) {
            return result;
        }
        if (FAILED(result = compositionDevice_->CreateTargetForHwnd(
                window_, TRUE, &compositionTarget_))) {
            return result;
        }
        if (FAILED(result = compositionDevice_->CreateVisual(&compositionVisual_))) {
            return result;
        }
        if (FAILED(result = compositionVisual_->SetContent(swapChain_.Get()))) return result;
        if (FAILED(result = compositionTarget_->SetRoot(compositionVisual_.Get()))) return result;
        if (FAILED(result = compositionDevice_->Commit())) return result;

        SetTimer(window_, kStatusTimer, 500, nullptr);
        updateWindowRegion();
        render();
        return S_OK;
    }

    void onWindowMetricsChanged() {
        updateWindowRegion();
        render();
    }

    void onTimer(UINT_PTR timer) {
        if (timer == kAnimationTimer) {
            animate();
        } else if (timer == kVisualizerTimer) {
            updateVisualizerFrame();
        } else if (timer == kStatusTimer) {
            updateStatus();
            const float desiredWidth = expanded_ ? kExpandedWidth : compactWidth();
            if (std::abs(width_.target - desiredWidth) > 0.1f) {
                width_.target = desiredWidth;
                startAnimation();
            } else {
                render();
            }
        }
    }

    void onMediaChanged() {
        const float desiredWidth = expanded_ ? kExpandedWidth : compactWidth();
        if (std::abs(width_.target - desiredWidth) > 0.1f) {
            width_.target = desiredWidth;
            startAnimation();
        }
        updateVisualizerTimer();
        render();
    }

    void onWeatherChanged() {
        const float desiredWidth = expanded_ ? kExpandedWidth : compactWidth();
        if (std::abs(width_.target - desiredWidth) > 0.1f) {
            width_.target = desiredWidth;
            startAnimation();
        }
        render();
    }

    void onMouseMove(float x, float y) {
        const bool nowHovered = pointInIsland(x, y);
        if (nowHovered && !trackingMouse_) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window_, 0};
            TrackMouseEvent(&tracking);
            trackingMouse_ = true;
        }
        if (hovered_ != nowHovered) {
            hovered_ = nowHovered;
            hover_.target = hovered_ ? 1.0f : 0.0f;
            startAnimation();
        }
    }

    void onMouseLeave() {
        trackingMouse_ = false;
        hovered_ = false;
        hover_.target = 0.0f;
        startAnimation();
    }

    void onOutsideMouseDown(LONG screenX, LONG screenY) {
        if (!expanded_) return;
        POINT point{screenX, screenY};
        ScreenToClient(window_, &point);
        const float scale = 96.0f / static_cast<float>(GetDpiForWindow(window_));
        if (!pointInIsland(
                static_cast<float>(point.x) * scale,
                static_cast<float>(point.y) * scale)) {
            setExpanded(false);
        }
    }

    void onLeftButton(float x, float y) {
        if (!pointInIsland(x, y)) return;

        if (expanded_) {
            for (int index = 0; index < 3; ++index) {
                if (pointInRect(x, y, tabButtonRect(index))) {
                    const auto selectedTab = static_cast<IslandTab>(index);
                    if (activeTab_ != selectedTab) {
                        setActiveTab(selectedTab);
                    }
                    return;
                }
            }

            if (activeTab_ == IslandTab::Media) {
                if (pointInRect(x, y, previousButtonRect())) {
                    mediaController_.request(MediaAction::Previous);
                    return;
                }
                if (pointInRect(x, y, playButtonRect())) {
                    mediaController_.request(MediaAction::TogglePlayback);
                    return;
                }
                if (pointInRect(x, y, nextButtonRect())) {
                    mediaController_.request(MediaAction::Next);
                    return;
                }
                if (pointInRect(x, y, volumeButtonRect())) {
                    spotifyAudioSession_.setMuted(!muted_);
                    updateStatus();
                    render();
                    return;
                }
            } else if (activeTab_ == IslandTab::Calendar) {
                if (pointInRect(x, y, calendarPreviousRect().expanded(8.0f, 8.0f))) {
                    changeCalendarMonth(-1);
                    return;
                }
                if (pointInRect(x, y, calendarNextRect().expanded(8.0f, 8.0f))) {
                    changeCalendarMonth(1);
                    return;
                }
                const int selectedDay = calendarDayAtPoint(x, y);
                if (selectedDay > 0) {
                    calendarSelectedDay_ = selectedDay;
                    render();
                    return;
                }
            } else if (activeTab_ == IslandTab::Weather
                       && pointInRect(x, y, weatherRefreshRect().expanded(7.0f, 7.0f))) {
                weatherController_.requestRefresh();
                return;
            }
            // Expanded content is interactive; click outside the island to close.
            return;
        }
        setExpanded(true);
    }

    void onMouseWheel(short delta) {
        if (expanded_) {
            wheelAccumulator_ += static_cast<int>(delta);
            if (std::abs(wheelAccumulator_) < WHEEL_DELTA) return;

            const int current = static_cast<int>(activeTab_);
            const int direction = wheelAccumulator_ < 0 ? 1 : -1;
            wheelAccumulator_ = 0;
            const int next = std::clamp(current + direction, 0, 2);
            if (next != current) {
                setActiveTab(static_cast<IslandTab>(next));
            }
            return;
        }

        const float step = delta > 0 ? 0.035f : -0.035f;
        spotifyAudioSession_.setVolume(volume_ + step);
        updateStatus();
        render();
    }

    void setExpanded(bool expanded) {
        expanded_ = expanded;
        wheelAccumulator_ = 0;
        setOutsideClickTracking(expanded);
        width_.target = expanded ? kExpandedWidth : compactWidth();
        height_.target = expanded ? expandedHeight() : kCompactHeight;
        startAnimation();
    }

    void setActiveTab(IslandTab tab) {
        if (activeTab_ == tab) return;
        activeTab_ = tab;
        tabIndicator_.target = static_cast<float>(tab);
        if (expanded_) {
            height_.target = expandedHeight();
        }
        contentOpacity_.value = 0.0f;
        contentOpacity_.velocity = 0.0f;
        contentOpacity_.target = 1.0f;
        updateVisualizerTimer();
        startAnimation();
    }

    bool pointInIsland(float x, float y) const {
        const auto rect = islandRect();
        if (!pointInRect(x, y, rect)) return false;

        const float radius = cornerRadius();
        const float leftCenter = rect.left + radius;
        const float rightCenter = rect.right - radius;
        if (x >= leftCenter && x <= rightCenter) return true;
        const float centerX = x < leftCenter ? leftCenter : rightCenter;
        const float centerY = std::clamp(y, rect.top + radius, rect.bottom - radius);
        const float dx = x - centerX;
        const float dy = y - centerY;
        return dx * dx + dy * dy <= radius * radius;
    }

    void showContextMenu(POINT screenPoint) {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;

        AppendMenuW(menu, MF_STRING | (expanded_ ? MF_CHECKED : 0), kMenuExpand, L"Expanded");
        AppendMenuW(
            menu,
            MF_STRING | (isStartupEnabled() ? MF_CHECKED : 0),
            kMenuStartup,
            L"Run at startup");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");

        SetForegroundWindow(window_);
        const UINT command = TrackPopupMenu(
            menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
            screenPoint.x,
            screenPoint.y,
            0,
            window_,
            nullptr);
        DestroyMenu(menu);

        switch (command) {
        case kMenuExpand:
            setExpanded(!expanded_);
            break;
        case kMenuStartup:
            setStartupEnabled(!isStartupEnabled());
            break;
        case kMenuExit:
            DestroyWindow(window_);
            break;
        default:
            break;
        }
    }

    void render() {
        if (!d2dContext_ || rendering_) return;
        rendering_ = true;

        d2dContext_->BeginDraw();
        d2dContext_->Clear(D2D1::ColorF(0, 0.0f));

        const auto rect = islandRect();
        const float radius = cornerRadius();
        drawShadow(rect, radius);

        ComPtr<ID2D1SolidColorBrush> background;
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.018f, 0.020f, 0.024f, 0.985f),
            &background);
        d2dContext_->FillRoundedRectangle(
            D2D1::RoundedRect(rect, radius, radius),
            background.Get());

        ComPtr<ID2D1SolidColorBrush> border;
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.055f + hover_.value * 0.055f),
            &border);
        d2dContext_->DrawRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(
                    rect.left + 0.5f,
                    rect.top + 0.5f,
                    rect.right - 0.5f,
                    rect.bottom - 0.5f),
                radius,
                radius),
            border.Get(),
            1.0f);

        // Clip every transition frame to the current pill. Without this mask,
        // the 64 px expanded artwork can briefly protrude through a shrinking
        // 38 px compact island.
        ComPtr<ID2D1RoundedRectangleGeometry> contentMask;
        ComPtr<ID2D1Layer> contentLayer;
        const bool hasContentMask =
            SUCCEEDED(d2dFactory_->CreateRoundedRectangleGeometry(
                D2D1::RoundedRect(rect, radius, radius),
                &contentMask))
            && SUCCEEDED(d2dContext_->CreateLayer(&contentLayer));
        if (hasContentMask) {
            d2dContext_->PushLayer(
                D2D1::LayerParameters(
                    D2D1::InfiniteRect(),
                    contentMask.Get()),
                contentLayer.Get());
        }

        const float expansion =
            clamp01((height_.value - kCompactHeight) / (expandedHeight() - kCompactHeight));
        // Fade the two text/control layouts in separate windows while a single
        // album cover remains visible and morphs continuously between them.
        const float compactOpacity =
            1.0f - clamp01(expansion / 0.46f);
        const float expandedOpacity =
            clamp01((expansion - 0.54f) / 0.38f);
        if (compactOpacity > 0.001f) {
            drawCompact(compactOpacity);
        }
        if (expandedOpacity > 0.001f) {
            const float pageOpacity =
                expandedOpacity * clamp01(contentOpacity_.value);
            switch (activeTab_) {
            case IslandTab::Media:
                drawMediaPage(pageOpacity);
                break;
            case IslandTab::Calendar:
                drawCalendarPage(pageOpacity);
                break;
            case IslandTab::Weather:
                drawWeatherPage(pageOpacity);
                break;
            }
            drawPageIndicators(expandedOpacity);
        }

        if (activeTab_ == IslandTab::Media) {
            const auto snapshot = mediaController_.snapshot();
            if (snapshot.available) {
                const FloatRect compactArtwork{
                    rect.left + 6.0f,
                    rect.top + 5.0f,
                    rect.left + 38.0f,
                    rect.top + 37.0f,
                };
                const FloatRect expandedArtwork{
                    rect.left + 16.0f,
                    rect.top + 15.0f,
                    rect.left + 88.0f,
                    rect.top + 87.0f,
                };
                const auto interpolate = [expansion](float from, float to) {
                    return from + (to - from) * expansion;
                };
                drawAlbumArtwork(
                    FloatRect{
                        interpolate(compactArtwork.left, expandedArtwork.left),
                        interpolate(compactArtwork.top, expandedArtwork.top),
                        interpolate(compactArtwork.right, expandedArtwork.right),
                        interpolate(compactArtwork.bottom, expandedArtwork.bottom),
                    },
                    snapshot,
                    interpolate(16.0f, 17.0f),
                    1.0f);
            }
        }
        if (hasContentMask) {
            d2dContext_->PopLayer();
        }

        const HRESULT drawResult = d2dContext_->EndDraw();
        if (drawResult == D2DERR_RECREATE_TARGET) {
            targetBitmap_.Reset();
        } else if (SUCCEEDED(drawResult)) {
            swapChain_->Present(1, 0);
        }

        rendering_ = false;
    }

private:
    struct FloatRect : D2D1_RECT_F {
        FloatRect expanded(float horizontal, float vertical) const {
            return {
                left - horizontal,
                top - vertical,
                right + horizontal,
                bottom + vertical,
            };
        }
    };

    bool fontFamilyInstalled(const wchar_t* family) const {
        ComPtr<IDWriteFontCollection> fonts;
        if (FAILED(textFactory_->GetSystemFontCollection(&fonts, FALSE))) {
            return false;
        }
        UINT32 index = 0;
        BOOL exists = FALSE;
        return SUCCEEDED(fonts->FindFamilyName(family, &index, &exists))
            && exists != FALSE;
    }

    // Segoe UI Variable ships with Windows 11 only. Windows 10 falls back to
    // Segoe UI, which has the same metrics family and the semibold weight.
    void resolveFontFamilies() {
        displayFont_ = fontFamilyInstalled(L"Segoe UI Variable Display")
            ? L"Segoe UI Variable Display"
            : L"Segoe UI";
        textFont_ = fontFamilyInstalled(L"Segoe UI Variable Text")
            ? L"Segoe UI Variable Text"
            : L"Segoe UI";
    }

    HRESULT createTextFormats() {
        resolveFontFamilies();
        HRESULT result = textFactory_->CreateTextFormat(
            displayFont_.c_str(),
            nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            14.0f,
            L"en-us",
            &titleFormat_);
        if (FAILED(result)) return result;

        result = textFactory_->CreateTextFormat(
            displayFont_.c_str(),
            nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            16.5f,
            L"en-us",
            &mediaTitleFormat_);
        if (FAILED(result)) return result;

        result = textFactory_->CreateTextFormat(
            textFont_.c_str(),
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            11.5f,
            L"en-us",
            &captionFormat_);
        if (FAILED(result)) return result;

        result = textFactory_->CreateTextFormat(
            textFont_.c_str(),
            nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            10.5f,
            L"en-us",
            &smallFormat_);
        if (FAILED(result)) return result;

        result = textFactory_->CreateTextFormat(
            textFont_.c_str(),
            nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            10.5f,
            L"en-us",
            &tabFormat_);
        if (FAILED(result)) return result;
        tabFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        tabFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        result = textFactory_->CreateTextFormat(
            displayFont_.c_str(),
            nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            38.0f,
            L"en-us",
            &temperatureFormat_);
        if (FAILED(result)) return result;

        result = textFactory_->CreateTextFormat(
            displayFont_.c_str(),
            nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            42.0f,
            L"en-us",
            &weatherHeroFormat_);
        if (FAILED(result)) return result;
        weatherHeroFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        weatherHeroFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        result = textFactory_->CreateTextFormat(
            displayFont_.c_str(),
            nullptr,
            DWRITE_FONT_WEIGHT_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            46.0f,
            L"en-us",
            &dateNumberFormat_);
        if (FAILED(result)) return result;
        dateNumberFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        dateNumberFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        titleFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        mediaTitleFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        captionFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        smallFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        temperatureFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        weatherHeroFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return S_OK;
    }

    void startAnimation() {
        lastFrame_ = std::chrono::steady_clock::now();
        SetTimer(window_, kAnimationTimer, 8, nullptr);
    }

    void updateVisualizerTimer() {
        const auto snapshot = mediaController_.snapshot();
        if (activeTab_ == IslandTab::Media && snapshot.available && snapshot.playing) {
            SetTimer(window_, kVisualizerTimer, 16, nullptr);
        } else {
            KillTimer(window_, kVisualizerTimer);
            visualizerPeak_ = 0.0f;
            visualizerBands_.fill(0.0f);
            visualizerSpectrumActive_ = false;
        }
    }

    void updateVisualizerFrame() {
        const auto snapshot = mediaController_.snapshot();
        if (activeTab_ != IslandTab::Media || !snapshot.available || !snapshot.playing) {
            updateVisualizerTimer();
            render();
            return;
        }

        const auto spectrum = spotifySpectrumCapture_.snapshot();
        visualizerSpectrumActive_ = spectrum.active;
        if (spectrum.active) {
            visualizerBands_ = spectrum.bands;
            visualizerPeak_ = *std::max_element(
                visualizerBands_.begin(),
                visualizerBands_.end());
        } else {
            const float measuredPeak = spotifyAudioSession_.peak();
            const float target = measuredPeak >= 0.0f
                ? std::pow(clamp01(measuredPeak), 0.55f)
                : 0.22f + std::sin(visualizerPhase_ * 0.7f) * 0.06f;
            if (target >= visualizerPeak_) {
                visualizerPeak_ += (target - visualizerPeak_) * 0.48f;
            } else {
                visualizerPeak_ *= 0.86f;
            }
        }
        visualizerPhase_ += 0.16f + visualizerPeak_ * 0.13f;
        render();
    }

    void animate() {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::clamp(
            std::chrono::duration<float>(now - lastFrame_).count(),
            0.001f,
            0.033f);
        lastFrame_ = now;

        const float previousWidth = width_.value;
        const float previousHeight = height_.value;
        const bool widthMoving = width_.step(dt);
        const bool heightMoving = height_.step(dt);
        const bool hoverMoving = hover_.step(dt);
        const bool tabMoving = tabIndicator_.step(dt);
        const bool contentMoving = contentOpacity_.step(dt);
        if (std::abs(width_.value - previousWidth) > 0.001f
            || std::abs(height_.value - previousHeight) > 0.001f) {
            updateWindowRegion();
        }
        render();

        if (!widthMoving && !heightMoving && !hoverMoving && !tabMoving && !contentMoving) {
            KillTimer(window_, kAnimationTimer);
        }
    }

    float compactWidth() const {
        if (activeTab_ == IslandTab::Calendar) return 188.0f;
        if (activeTab_ == IslandTab::Weather) {
            return weatherController_.snapshot().available ? 216.0f : 176.0f;
        }
        return 150.0f;
    }

    float expandedHeight() const {
        if (activeTab_ == IslandTab::Media) return kMediaExpandedHeight;
        if (activeTab_ == IslandTab::Weather) return kWeatherExpandedHeight;
        return kExpandedHeight;
    }

    FloatRect islandRect() const {
        const float left = (kCanvasWidth - width_.value) * 0.5f;
        return {left, kTopInset, left + width_.value, kTopInset + height_.value};
    }

    float cornerRadius() const {
        const float expansion =
            clamp01((height_.value - kCompactHeight) / (expandedHeight() - kCompactHeight));
        return kCompactHeight * 0.5f + expansion * 10.0f;
    }

    void updateWindowRegion() {
        const float scale =
            static_cast<float>(GetDpiForWindow(window_)) / 96.0f;
        const auto rect = islandRect();
        const int left = static_cast<int>(std::floor(rect.left * scale)) - 1;
        const int top = static_cast<int>(std::floor(rect.top * scale)) - 1;
        const int right = static_cast<int>(std::ceil(rect.right * scale)) + 2;
        const int bottom = static_cast<int>(std::ceil(rect.bottom * scale)) + 2;
        const int diameter = std::max(
            2,
            static_cast<int>(std::ceil((cornerRadius() + 1.0f) * 2.0f * scale)));

        const HRGN region = CreateRoundRectRgn(
            left,
            top,
            right,
            bottom,
            diameter,
            diameter);
        if (!region) return;
        if (SetWindowRgn(window_, region, FALSE) == 0) {
            DeleteObject(region);
        }
    }

    static bool pointInRect(float x, float y, const D2D1_RECT_F& rect) {
        return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
    }

    FloatRect previousButtonRect() const {
        const auto rect = islandRect();
        return {
            rect.left + 83.0f,
            rect.top + 126.0f,
            rect.left + 139.0f,
            rect.top + 176.0f,
        };
    }

    FloatRect playButtonRect() const {
        const auto rect = islandRect();
        return {
            rect.left + 179.0f,
            rect.top + 123.0f,
            rect.left + 231.0f,
            rect.top + 179.0f,
        };
    }

    FloatRect nextButtonRect() const {
        const auto rect = islandRect();
        return {
            rect.left + 271.0f,
            rect.top + 126.0f,
            rect.left + 327.0f,
            rect.top + 176.0f,
        };
    }

    FloatRect volumeButtonRect() const {
        const auto rect = islandRect();
        return {
            rect.right - 76.0f,
            rect.top + 126.0f,
            rect.right - 28.0f,
            rect.top + 176.0f,
        };
    }

    FloatRect volumeTrackRect() const {
        const auto rect = islandRect();
        return {
            rect.left + 54.0f,
            rect.bottom - 24.5f,
            rect.right - 80.0f,
            rect.bottom - 20.5f,
        };
    }

    FloatRect tabButtonRect(int index) const {
        const auto rect = islandRect();
        constexpr float gap = 14.0f;
        const float centerX = rect.right - 12.0f;
        const float centerY =
            (rect.top + rect.bottom) * 0.5f
            + (static_cast<float>(index) - 1.0f) * gap;
        return {
            centerX - 8.0f,
            centerY - 7.0f,
            centerX + 8.0f,
            centerY + 7.0f,
        };
    }

    FloatRect calendarCardRect() const {
        const auto rect = islandRect();
        return {
            rect.left + 10.0f,
            rect.top + 10.0f,
            rect.left + 112.0f,
            rect.bottom - 10.0f,
        };
    }

    FloatRect calendarPreviousRect() const {
        const auto card = calendarCardRect();
        return {card.left + 5.0f, card.top + 7.0f, card.left + 25.0f, card.top + 28.0f};
    }

    FloatRect calendarNextRect() const {
        const auto card = calendarCardRect();
        return {card.right - 25.0f, card.top + 7.0f, card.right - 5.0f, card.top + 28.0f};
    }

    FloatRect weatherRefreshRect() const {
        const auto rect = islandRect();
        return {
            rect.right - 70.0f,
            rect.top + 12.0f,
            rect.right - 42.0f,
            rect.top + 40.0f,
        };
    }

    void drawShadow(const FloatRect& rect, float radius) {
        for (int layer = 5; layer >= 1; --layer) {
            const float spread = static_cast<float>(layer) * 1.8f;
            ComPtr<ID2D1SolidColorBrush> brush;
            d2dContext_->CreateSolidColorBrush(
                D2D1::ColorF(0.0f, 0.0f, 0.0f, (6 - layer) * 0.012f),
                &brush);
            d2dContext_->FillRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(
                        rect.left - spread,
                        rect.top - spread * 0.35f,
                        rect.right + spread,
                        rect.bottom + spread),
                    radius + spread,
                    radius + spread),
                brush.Get());
        }
    }

    void updateArtworkBitmap(const MediaSnapshot& snapshot) {
        if (snapshot.artworkKey == renderedArtworkKey_) return;
        renderedArtworkKey_ = snapshot.artworkKey;
        artworkBitmap_.Reset();
        if (!wicFactory_ || !snapshot.artwork || snapshot.artwork->empty()) return;

        ComPtr<IWICStream> stream;
        if (FAILED(wicFactory_->CreateStream(&stream))) return;
        if (FAILED(stream->InitializeFromMemory(
                const_cast<BYTE*>(snapshot.artwork->data()),
                static_cast<DWORD>(snapshot.artwork->size())))) {
            return;
        }

        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(wicFactory_->CreateDecoderFromStream(
                stream.Get(),
                nullptr,
                WICDecodeMetadataCacheOnLoad,
                &decoder))) {
            return;
        }
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame))) return;

        ComPtr<IWICFormatConverter> converter;
        if (FAILED(wicFactory_->CreateFormatConverter(&converter))) return;
        if (FAILED(converter->Initialize(
                frame.Get(),
                GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeMedianCut))) {
            return;
        }
        // On Windows 10, Spotify's media thumbnail is a 300x300 PNG with the
        // cover in the top-middle and a Spotify logo below it on a transparent
        // background. Crop to the cover so only the album art is drawn.
        ComPtr<IWICBitmapSource> source = converter;
        WICRect cover{};
        ComPtr<IWICBitmapClipper> clipper;
        if (findOpaqueCover(converter.Get(), cover)
            && SUCCEEDED(wicFactory_->CreateBitmapClipper(&clipper))
            && SUCCEEDED(clipper->Initialize(converter.Get(), &cover))) {
            source = clipper;
        }
        d2dContext_->CreateBitmapFromWicBitmap(
            source.Get(),
            nullptr,
            &artworkBitmap_);
    }

    // Finds the album cover inside an artwork image that has transparent
    // padding: the first contiguous band of mostly-opaque rows. Returns false
    // for fully opaque images, which are already just the cover.
    static bool findOpaqueCover(IWICBitmapSource* image, WICRect& cover) {
        UINT width = 0;
        UINT height = 0;
        if (FAILED(image->GetSize(&width, &height))
            || width < 16 || height < 16 || width > 4096 || height > 4096) {
            return false;
        }
        const UINT stride = width * 4;
        std::vector<BYTE> pixels(static_cast<size_t>(stride) * height);
        if (FAILED(image->CopyPixels(
                nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data()))) {
            return false;
        }

        constexpr BYTE kOpaque = 200;
        bool hasTransparency = false;
        int top = -1;
        int bottom = -1;
        UINT left = width;
        UINT right = 0;
        for (UINT y = 0; y < height; ++y) {
            const BYTE* row = pixels.data() + static_cast<size_t>(y) * stride;
            UINT opaque = 0;
            UINT first = width;
            UINT last = 0;
            for (UINT x = 0; x < width; ++x) {
                if (row[x * 4 + 3] > kOpaque) {
                    ++opaque;
                    first = std::min(first, x);
                    last = x;
                } else {
                    hasTransparency = true;
                }
            }
            const bool coverRow = opaque * 2 >= width;
            if (coverRow && bottom < 0) {
                if (top < 0) top = static_cast<int>(y);
                left = std::min(left, first);
                right = std::max(right, last);
            } else if (!coverRow && top >= 0 && bottom < 0) {
                bottom = static_cast<int>(y);
            }
        }
        if (!hasTransparency || top < 0) return false;
        if (bottom < 0) bottom = static_cast<int>(height);

        cover.X = static_cast<INT>(left);
        cover.Y = top;
        cover.Width = static_cast<INT>(right - left + 1);
        cover.Height = bottom - top;
        // Ignore tiny or barely-cropped results; those aren't a padded cover.
        return cover.Width >= 16 && cover.Height >= 16
            && (cover.Width < static_cast<INT>(width)
                || cover.Height < static_cast<INT>(height));
    }

    void drawAlbumArtwork(
        const FloatRect& destination,
        const MediaSnapshot& snapshot,
        float radius,
        float opacity) {
        updateArtworkBitmap(snapshot);
        if (artworkBitmap_) {
            const D2D1_SIZE_F bitmapSize = artworkBitmap_->GetSize();
            const float destinationWidth = destination.right - destination.left;
            const float destinationHeight = destination.bottom - destination.top;
            const float destinationAspect = destinationWidth / destinationHeight;
            const float bitmapAspect = bitmapSize.width / bitmapSize.height;
            D2D1_RECT_F source = D2D1::RectF(0, 0, bitmapSize.width, bitmapSize.height);
            if (bitmapAspect > destinationAspect) {
                const float croppedWidth = bitmapSize.height * destinationAspect;
                source.left = (bitmapSize.width - croppedWidth) * 0.5f;
                source.right = source.left + croppedWidth;
            } else {
                const float croppedHeight = bitmapSize.width / destinationAspect;
                source.top = (bitmapSize.height - croppedHeight) * 0.5f;
                source.bottom = source.top + croppedHeight;
            }

            ComPtr<ID2D1RoundedRectangleGeometry> mask;
            ComPtr<ID2D1Layer> layer;
            if (SUCCEEDED(d2dFactory_->CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(destination, radius, radius),
                    &mask))
                && SUCCEEDED(d2dContext_->CreateLayer(&layer))) {
                const D2D1_LAYER_PARAMETERS parameters = D2D1::LayerParameters(
                    D2D1::InfiniteRect(),
                    mask.Get(),
                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                    D2D1::IdentityMatrix(),
                    opacity);
                d2dContext_->PushLayer(parameters, layer.Get());
                d2dContext_->DrawBitmap(
                    artworkBitmap_.Get(),
                    destination,
                    1.0f,
                    D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                    source);
                d2dContext_->PopLayer();
                return;
            }
        }

        const D2D1_COLOR_F accent = accentFromSeed(snapshot.accentSeed);
        const D2D1_COLOR_F accentDark = accentFromSeed(snapshot.accentSeed + 31u, -0.17f);
        const D2D1_GRADIENT_STOP gradientStops[] = {
            {0.0f, D2D1::ColorF(accent.r, accent.g, accent.b, opacity)},
            {1.0f, D2D1::ColorF(accentDark.r, accentDark.g, accentDark.b, opacity)},
        };
        ComPtr<ID2D1GradientStopCollection> stops;
        ComPtr<ID2D1LinearGradientBrush> gradient;
        d2dContext_->CreateGradientStopCollection(
            gradientStops,
            ARRAYSIZE(gradientStops),
            &stops);
        d2dContext_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(destination.left, destination.top),
                D2D1::Point2F(destination.right, destination.bottom)),
            stops.Get(),
            &gradient);
        d2dContext_->FillRoundedRectangle(
            D2D1::RoundedRect(destination, radius, radius),
            gradient.Get());

        ComPtr<ID2D1SolidColorBrush> note;
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(1.0f, 1.0f, 1.0f, opacity * 0.9f),
            &note);
        drawMusicNote(
            D2D1::Point2F(
                (destination.left + destination.right) * 0.5f,
                (destination.top + destination.bottom) * 0.5f),
            note.Get(),
            opacity);
    }

    void drawVisualizer(
        const FloatRect& bounds,
        uint32_t accentSeed,
        float opacity) {
        constexpr int barCount = 5;
        constexpr float barWidth = 3.0f;
        constexpr float gap = 2.5f;
        const float totalWidth = barCount * barWidth + (barCount - 1) * gap;
        const float startX =
            bounds.left + ((bounds.right - bounds.left) - totalWidth) * 0.5f;
        const float centerY = (bounds.top + bounds.bottom) * 0.5f;
        const float maximumHeight = bounds.bottom - bounds.top;
        const D2D1_COLOR_F accent = accentFromSeed(accentSeed);

        for (int index = 0; index < barCount; ++index) {
            const float wave = 0.35f + 0.65f * std::abs(std::sin(
                visualizerPhase_ * (0.82f + index * 0.06f)
                + static_cast<float>(index) * 1.37f));
            const float level = visualizerSpectrumActive_
                ? visualizerBands_[static_cast<size_t>(index)]
                : visualizerPeak_ * wave;
            const float height = std::clamp(
                3.0f + (maximumHeight - 3.0f) * level,
                3.0f,
                maximumHeight);
            const float x = startX + static_cast<float>(index) * (barWidth + gap);
            ComPtr<ID2D1SolidColorBrush> brush;
            const float brightness = 0.82f + static_cast<float>(index) * 0.035f;
            d2dContext_->CreateSolidColorBrush(
                D2D1::ColorF(
                    std::min(accent.r * brightness, 1.0f),
                    std::min(accent.g * brightness, 1.0f),
                    std::min(accent.b * brightness, 1.0f),
                    opacity),
                &brush);
            d2dContext_->FillRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(
                        x,
                        centerY - height * 0.5f,
                        x + barWidth,
                        centerY + height * 0.5f),
                    1.5f,
                    1.5f),
                brush.Get());
        }
    }

    void drawCompact(float opacity) {
        const auto rect = islandRect();
        const auto mediaSnapshot = mediaController_.snapshot();
        const auto weatherSnapshot = weatherController_.snapshot();
        ComPtr<ID2D1SolidColorBrush> primary;
        ComPtr<ID2D1SolidColorBrush> secondary;
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.96f, 0.97f, 0.99f, opacity),
            &primary);
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.62f, 0.65f, 0.70f, opacity),
            &secondary);

        const float centerY = rect.top + kCompactHeight * 0.5f;
        float textLeft = rect.left + 16.0f;
        bool showAccent = false;
        D2D1_COLOR_F accent = colorFromHex(0x6F7BF7u);
        std::wstring label = timeText_;

        if (activeTab_ == IslandTab::Media) {
            if (mediaSnapshot.available) {
                label.clear();
                textLeft = rect.left + 42.0f;
            } else {
                showAccent = true;
                accent = colorFromHex(0x1ED760u);
                label.clear();
            }
        } else if (activeTab_ == IslandTab::Calendar) {
            showAccent = true;
            accent = colorFromHex(0x5B8CFFu);
            SYSTEMTIME today{};
            GetLocalTime(&today);
            wchar_t date[48]{};
            if (GetDateFormatEx(
                    LOCALE_NAME_USER_DEFAULT,
                    0,
                    &today,
                    L"ddd, MMM d",
                    date,
                    ARRAYSIZE(date),
                    nullptr)) {
                label = date;
            }
        } else if (activeTab_ == IslandTab::Weather) {
            showAccent = true;
            accent = colorFromHex(
                weatherSnapshot.weatherCode == 113 ? 0xFFB52Eu : 0x72A7FFu);
            if (weatherSnapshot.available) {
                label = std::format(
                    L"{}°  {}",
                    weatherSnapshot.temperature,
                    weatherSnapshot.condition);
            } else {
                label = weatherSnapshot.loading ? L"Loading weather…" : L"Weather unavailable";
            }
        }

        if (showAccent) {
            ComPtr<ID2D1SolidColorBrush> accentBrush;
            d2dContext_->CreateSolidColorBrush(
                D2D1::ColorF(accent.r, accent.g, accent.b, opacity),
                &accentBrush);
            d2dContext_->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(rect.left + 18.0f, centerY), 4.0f, 4.0f),
                accentBrush.Get());
            textLeft = rect.left + 30.0f;
        }

        const bool spotifyMode = activeTab_ == IslandTab::Media;
        const float reservedRight = spotifyMode ? 57.0f : 58.0f;
        if (!label.empty()) {
            d2dContext_->PushAxisAlignedClip(
                D2D1::RectF(textLeft, rect.top, rect.right - reservedRight, rect.bottom),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            d2dContext_->DrawTextW(
                label.c_str(),
                static_cast<UINT32>(label.size()),
                smallFormat_.Get(),
                D2D1::RectF(textLeft, centerY - 7.5f, rect.right - reservedRight, centerY + 8.0f),
                primary.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
            d2dContext_->PopAxisAlignedClip();
        }

        if (spotifyMode) {
            if (mediaSnapshot.available) {
                drawVisualizer(
                    FloatRect{
                        rect.right - 49.0f,
                        centerY - 10.0f,
                        rect.right - 8.0f,
                        centerY + 10.0f,
                    },
                    mediaSnapshot.accentSeed,
                    opacity);
            }
        } else {
            drawVolumeIcon(
                D2D1::Point2F(rect.right - 41.0f, centerY),
                secondary.Get(),
                opacity,
                muted_);

            const std::wstring volumeLabel = std::format(L"{:.0f}", volume_ * 100.0f);
            d2dContext_->DrawTextW(
                volumeLabel.c_str(),
                static_cast<UINT32>(volumeLabel.size()),
                smallFormat_.Get(),
                D2D1::RectF(rect.right - 29.0f, centerY - 7.5f, rect.right - 7.0f, centerY + 8.0f),
                secondary.Get());
        }
    }

    void drawPageIndicators(float opacity) {
        const auto rect = islandRect();
        ComPtr<ID2D1SolidColorBrush> inactive;
        ComPtr<ID2D1SolidColorBrush> activeGlow;
        ComPtr<ID2D1SolidColorBrush> active;
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.58f, 0.60f, 0.66f, 0.48f * opacity),
            &inactive);
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f * opacity),
            &activeGlow);
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.97f, 0.98f, 1.0f, opacity),
            &active);

        const float position = std::clamp(tabIndicator_.value, 0.0f, 2.0f);
        constexpr float gap = 14.0f;
        const float centerX = rect.right - 12.0f;
        const float centerY = (rect.top + rect.bottom) * 0.5f;
        for (int index = 0; index < 3; ++index) {
            const float y =
                centerY + (static_cast<float>(index) - 1.0f) * gap;
            d2dContext_->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(centerX, y), 2.0f, 2.0f),
                inactive.Get());
        }

        const float activeY = centerY + (position - 1.0f) * gap;
        d2dContext_->FillEllipse(
            D2D1::Ellipse(
                D2D1::Point2F(centerX, activeY),
                5.5f,
                5.5f),
            activeGlow.Get());
        d2dContext_->FillEllipse(
            D2D1::Ellipse(
                D2D1::Point2F(centerX, activeY),
                3.2f,
                3.2f),
            active.Get());
    }

    static int daysInMonth(int year, int month) {
        static constexpr int days[] = {
            31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
        };
        if (month != 2) return days[month - 1];
        const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        return leap ? 29 : 28;
    }

    int calendarDayAtPoint(float x, float y) const {
        const auto rect = islandRect();
        const float gridLeft = rect.left + 126.0f;
        const float gridRight = rect.right - 24.0f;
        const float gridTop = rect.top + 47.0f;
        constexpr float rowHeight = 17.2f;
        const float columnWidth = (gridRight - gridLeft) / 7.0f;
        if (x < gridLeft || x >= gridRight
            || y < gridTop || y >= gridTop + rowHeight * 6.0f) {
            return 0;
        }

        const int column = static_cast<int>((x - gridLeft) / columnWidth);
        const int row = static_cast<int>((y - gridTop) / rowHeight);
        const std::chrono::year_month_day firstDay{
            std::chrono::year{calendarYear_},
            std::chrono::month{static_cast<unsigned int>(calendarMonth_)},
            std::chrono::day{1}};
        const int firstWeekday = static_cast<int>(
            std::chrono::weekday{std::chrono::sys_days{firstDay}}.c_encoding());
        const int day = row * 7 + column - firstWeekday + 1;
        return day >= 1 && day <= daysInMonth(calendarYear_, calendarMonth_)
            ? day
            : 0;
    }

    void changeCalendarMonth(int delta) {
        calendarMonth_ += delta;
        if (calendarMonth_ < 1) {
            calendarMonth_ = 12;
            --calendarYear_;
        } else if (calendarMonth_ > 12) {
            calendarMonth_ = 1;
            ++calendarYear_;
        }
        calendarSelectedDay_ =
            std::min(calendarSelectedDay_, daysInMonth(calendarYear_, calendarMonth_));
        contentOpacity_.value = 0.25f;
        contentOpacity_.velocity = 0.0f;
        contentOpacity_.target = 1.0f;
        startAnimation();
    }

    void drawCalendarPage(float opacity) {
        const auto rect = islandRect();
        const auto cardRect = calendarCardRect();
        ComPtr<ID2D1SolidColorBrush> primary;
        ComPtr<ID2D1SolidColorBrush> secondary;
        ComPtr<ID2D1SolidColorBrush> muted;
        ComPtr<ID2D1SolidColorBrush> cardBrush;
        ComPtr<ID2D1SolidColorBrush> accent;
        ComPtr<ID2D1SolidColorBrush> navigation;
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.97f, 0.98f, 1.0f, opacity),
            &primary);
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.78f, 0.79f, 0.82f, opacity),
            &secondary);
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.37f, 0.38f, 0.42f, opacity),
            &muted);
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.045f * opacity),
            &cardBrush);
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.82f, 0.22f, 0.19f, opacity),
            &accent);
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.55f, 0.56f, 0.60f, 0.55f * opacity),
            &navigation);

        d2dContext_->FillRoundedRectangle(
            D2D1::RoundedRect(cardRect, 14.0f, 14.0f),
            cardBrush.Get());

        SYSTEMTIME selectedDate{};
        selectedDate.wYear = static_cast<WORD>(calendarYear_);
        selectedDate.wMonth = static_cast<WORD>(calendarMonth_);
        selectedDate.wDay = static_cast<WORD>(calendarSelectedDay_);
        wchar_t monthName[32]{};
        GetDateFormatEx(
            LOCALE_NAME_USER_DEFAULT,
            0,
            &selectedDate,
            L"MMMM",
            monthName,
            ARRAYSIZE(monthName),
            nullptr);
        std::wstring monthLabel = monthName;
        std::transform(
            monthLabel.begin(),
            monthLabel.end(),
            monthLabel.begin(),
            [](wchar_t character) {
                return static_cast<wchar_t>(towupper(character));
            });
        d2dContext_->DrawTextW(
            monthLabel.c_str(),
            static_cast<UINT32>(monthLabel.size()),
            tabFormat_.Get(),
            D2D1::RectF(
                cardRect.left + 18.0f,
                cardRect.top + 7.0f,
                cardRect.right - 18.0f,
                cardRect.top + 25.0f),
            accent.Get());

        const std::wstring yearLabel = std::to_wstring(calendarYear_);
        d2dContext_->DrawTextW(
            yearLabel.c_str(),
            static_cast<UINT32>(yearLabel.size()),
            tabFormat_.Get(),
            D2D1::RectF(
                cardRect.left + 18.0f,
                cardRect.top + 25.0f,
                cardRect.right - 18.0f,
                cardRect.top + 43.0f),
            muted.Get());

        const std::wstring selectedDay = std::to_wstring(calendarSelectedDay_);
        d2dContext_->DrawTextW(
            selectedDay.c_str(),
            static_cast<UINT32>(selectedDay.size()),
            dateNumberFormat_.Get(),
            D2D1::RectF(
                cardRect.left + 5.0f,
                cardRect.top + 42.0f,
                cardRect.right - 5.0f,
                cardRect.top + 99.0f),
            primary.Get());

        wchar_t weekdayName[32]{};
        GetDateFormatEx(
            LOCALE_NAME_USER_DEFAULT,
            0,
            &selectedDate,
            L"dddd",
            weekdayName,
            ARRAYSIZE(weekdayName),
            nullptr);
        const std::wstring weekdayLabel = weekdayName;
        d2dContext_->DrawTextW(
            weekdayLabel.c_str(),
            static_cast<UINT32>(weekdayLabel.size()),
            tabFormat_.Get(),
            D2D1::RectF(
                cardRect.left + 8.0f,
                cardRect.bottom - 28.0f,
                cardRect.right - 8.0f,
                cardRect.bottom - 8.0f),
            muted.Get());

        const auto previous = calendarPreviousRect();
        const auto next = calendarNextRect();
        const auto drawChevron = [&](const FloatRect& target, bool pointsRight) {
            const float cx = (target.left + target.right) * 0.5f;
            const float cy = (target.top + target.bottom) * 0.5f;
            const float direction = pointsRight ? 1.0f : -1.0f;
            d2dContext_->DrawLine(
                D2D1::Point2F(cx - direction * 2.0f, cy - 3.5f),
                D2D1::Point2F(cx + direction * 2.0f, cy),
                navigation.Get(),
                1.35f);
            d2dContext_->DrawLine(
                D2D1::Point2F(cx + direction * 2.0f, cy),
                D2D1::Point2F(cx - direction * 2.0f, cy + 3.5f),
                navigation.Get(),
                1.35f);
        };
        drawChevron(previous, false);
        drawChevron(next, true);

        static constexpr const wchar_t* weekdays[] = {
            L"S", L"M", L"T", L"W", L"T", L"F", L"S",
        };
        const float gridLeft = rect.left + 126.0f;
        const float gridRight = rect.right - 24.0f;
        const float columnWidth = (gridRight - gridLeft) / 7.0f;
        const float weekdayTop = rect.top + 18.0f;
        for (int column = 0; column < 7; ++column) {
            const FloatRect cell{
                gridLeft + columnWidth * static_cast<float>(column),
                weekdayTop,
                gridLeft + columnWidth * static_cast<float>(column + 1),
                weekdayTop + 17.0f,
            };
            d2dContext_->DrawTextW(
                weekdays[column],
                1,
                tabFormat_.Get(),
                cell,
                column == 0 || column == 6 ? accent.Get() : muted.Get());
        }

        const std::chrono::year_month_day firstDay{
            std::chrono::year{calendarYear_},
            std::chrono::month{static_cast<unsigned int>(calendarMonth_)},
            std::chrono::day{1}};
        const unsigned int firstWeekday =
            std::chrono::weekday{std::chrono::sys_days{firstDay}}.c_encoding();
        SYSTEMTIME today{};
        GetLocalTime(&today);
        const int count = daysInMonth(calendarYear_, calendarMonth_);
        constexpr float rowHeight = 17.2f;
        const float gridTop = rect.top + 47.0f;

        for (int day = 1; day <= count; ++day) {
            const int slot = static_cast<int>(firstWeekday) + day - 1;
            const int row = slot / 7;
            const int column = slot % 7;
            const float centerX =
                gridLeft + columnWidth * (static_cast<float>(column) + 0.5f);
            const float centerY =
                gridTop + rowHeight * (static_cast<float>(row) + 0.5f);
            const bool isToday =
                today.wYear == calendarYear_
                && today.wMonth == calendarMonth_
                && today.wDay == day;
            const bool isSelected = day == calendarSelectedDay_;
            if (isSelected) {
                d2dContext_->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(centerX, centerY), 10.5f, 9.2f),
                    accent.Get());
            } else if (isToday) {
                d2dContext_->DrawEllipse(
                    D2D1::Ellipse(D2D1::Point2F(centerX, centerY), 10.0f, 8.7f),
                    accent.Get(),
                    1.2f);
            }
            const std::wstring dayText = std::to_wstring(day);
            const bool weekend = column == 0 || column == 6;
            d2dContext_->DrawTextW(
                dayText.c_str(),
                static_cast<UINT32>(dayText.size()),
                tabFormat_.Get(),
                D2D1::RectF(
                    centerX - columnWidth * 0.5f,
                    centerY - 8.5f,
                    centerX + columnWidth * 0.5f,
                    centerY + 8.5f),
                isSelected ? primary.Get() : (weekend ? accent.Get() : secondary.Get()));
        }
    }

    static bool isRainCode(int code) {
        switch (code) {
        case 176: case 263: case 266: case 281: case 284:
        case 293: case 296: case 299: case 302: case 305: case 308:
        case 311: case 314: case 353: case 356: case 359:
            return true;
        default:
            return false;
        }
    }

    static bool isSnowCode(int code) {
        switch (code) {
        case 179: case 182: case 185: case 227: case 230:
        case 317: case 320: case 323: case 326: case 329: case 332:
        case 335: case 338: case 350: case 362: case 365:
        case 368: case 371: case 374: case 377: case 392: case 395:
            return true;
        default:
            return false;
        }
    }

    void drawWeatherIcon(D2D1_POINT_2F center, int code, float opacity) {
        const bool clear = code == 113;
        const bool partlyCloudy = code == 116;
        const bool rain = isRainCode(code);
        const bool snow = isSnowCode(code);
        const bool thunder = code == 200 || code == 386 || code == 389;
        const bool fog = code == 143 || code == 248 || code == 260;
        const bool cloud = !clear;

        ComPtr<ID2D1SolidColorBrush> sun;
        ComPtr<ID2D1SolidColorBrush> cloudBrush;
        ComPtr<ID2D1SolidColorBrush> precipitation;
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(1.0f, 0.69f, 0.14f, opacity),
            &sun);
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.72f, 0.78f, 0.88f, opacity),
            &cloudBrush);
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.34f, 0.64f, 1.0f, opacity),
            &precipitation);

        if (clear || partlyCloudy) {
            const D2D1_POINT_2F sunCenter = partlyCloudy
                ? D2D1::Point2F(center.x - 13.0f, center.y - 10.0f)
                : center;
            const float radius = clear ? 18.0f : 14.0f;
            d2dContext_->FillEllipse(
                D2D1::Ellipse(sunCenter, radius, radius),
                sun.Get());
            for (int ray = 0; ray < 8; ++ray) {
                const float angle = static_cast<float>(ray) * 3.14159265f / 4.0f;
                const float inner = radius + 5.0f;
                const float outer = radius + 10.0f;
                d2dContext_->DrawLine(
                    D2D1::Point2F(
                        sunCenter.x + std::cos(angle) * inner,
                        sunCenter.y + std::sin(angle) * inner),
                    D2D1::Point2F(
                        sunCenter.x + std::cos(angle) * outer,
                        sunCenter.y + std::sin(angle) * outer),
                    sun.Get(),
                    2.2f);
            }
        }

        if (cloud) {
            const float cloudY = center.y + (partlyCloudy ? 6.0f : -1.0f);
            d2dContext_->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(center.x - 15.0f, cloudY), 16.0f, 12.0f),
                cloudBrush.Get());
            d2dContext_->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(center.x + 1.0f, cloudY - 8.0f), 19.0f, 17.0f),
                cloudBrush.Get());
            d2dContext_->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(center.x + 19.0f, cloudY), 15.0f, 11.0f),
                cloudBrush.Get());
            d2dContext_->FillRectangle(
                D2D1::RectF(center.x - 16.0f, cloudY, center.x + 21.0f, cloudY + 11.0f),
                cloudBrush.Get());

            if (fog) {
                for (int line = 0; line < 3; ++line) {
                    const float y = cloudY + 17.0f + line * 6.0f;
                    d2dContext_->DrawLine(
                        D2D1::Point2F(center.x - 21.0f + line * 5.0f, y),
                        D2D1::Point2F(center.x + 20.0f, y),
                        precipitation.Get(),
                        2.0f);
                }
            } else if (rain || thunder) {
                for (int drop = -1; drop <= 1; ++drop) {
                    const float x = center.x + static_cast<float>(drop) * 14.0f;
                    d2dContext_->DrawLine(
                        D2D1::Point2F(x + 3.0f, cloudY + 16.0f),
                        D2D1::Point2F(x - 2.0f, cloudY + 25.0f),
                        precipitation.Get(),
                        2.5f);
                }
            } else if (snow) {
                for (int flake = -1; flake <= 1; ++flake) {
                    d2dContext_->FillEllipse(
                        D2D1::Ellipse(
                            D2D1::Point2F(
                                center.x + static_cast<float>(flake) * 14.0f,
                                cloudY + 22.0f + std::abs(flake) * 3.0f),
                            2.4f,
                            2.4f),
                        precipitation.Get());
                }
            }
        }
    }

    void drawWeatherPage(float opacity) {
        const auto rect = islandRect();
        const auto weather = weatherController_.snapshot();
        const D2D1_COLOR_F weatherAccent =
            colorFromHex(weather.weatherCode == 113 ? 0xFFB52Eu : 0x72A7FFu);

        ComPtr<ID2D1SolidColorBrush> primary;
        ComPtr<ID2D1SolidColorBrush> secondary;
        ComPtr<ID2D1SolidColorBrush> muted;
        ComPtr<ID2D1SolidColorBrush> heroCard;
        ComPtr<ID2D1SolidColorBrush> metricTray;
        ComPtr<ID2D1SolidColorBrush> accent;
        ComPtr<ID2D1SolidColorBrush> accentSurface;
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.97f, 0.98f, 1.0f, opacity),
            &primary);
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.62f, 0.65f, 0.70f, opacity),
            &secondary);
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.50f, 0.53f, 0.59f, opacity),
            &muted);
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(
                weatherAccent.r,
                weatherAccent.g,
                weatherAccent.b,
                0.10f * opacity),
            &heroCard);
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.065f * opacity),
            &metricTray);
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(
                weatherAccent.r,
                weatherAccent.g,
                weatherAccent.b,
                opacity),
            &accent);
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(
                weatherAccent.r,
                weatherAccent.g,
                weatherAccent.b,
                0.09f * opacity),
            &accentSurface);

        const auto refresh = weatherRefreshRect();
        d2dContext_->FillRoundedRectangle(
            D2D1::RoundedRect(refresh, 14.0f, 14.0f),
            metricTray.Get());
        const std::wstring refreshText = L"↻";
        d2dContext_->DrawTextW(
            refreshText.c_str(),
            1,
            tabFormat_.Get(),
            refresh,
            secondary.Get());

        const FloatRect hero{
            rect.left + 14.0f,
            rect.top + 14.0f,
            rect.left + 160.0f,
            rect.bottom - 12.0f,
        };
        d2dContext_->FillRoundedRectangle(
            D2D1::RoundedRect(hero, 20.0f, 20.0f),
            heroCard.Get());

        const std::wstring location =
            weather.available ? weather.location : L"Local weather";
        d2dContext_->FillEllipse(
            D2D1::Ellipse(
                D2D1::Point2F(hero.left + 16.0f, hero.top + 17.0f),
                3.0f,
                3.0f),
            accent.Get());
        d2dContext_->DrawTextW(
            location.c_str(),
            static_cast<UINT32>(location.size()),
            smallFormat_.Get(),
            D2D1::RectF(
                hero.left + 26.0f,
                hero.top + 9.0f,
                hero.right - 12.0f,
                hero.top + 29.0f),
            primary.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);

        if (!weather.available) {
            const std::wstring heading =
                weather.loading ? L"Loading local weather…" : L"Weather unavailable";
            const std::wstring detail =
                weather.loading ? L"Getting the latest conditions" : weather.error;
            d2dContext_->DrawTextW(
                heading.c_str(),
                static_cast<UINT32>(heading.size()),
                titleFormat_.Get(),
                D2D1::RectF(
                    rect.left + 175.0f,
                    rect.top + 62.0f,
                    rect.right - 30.0f,
                    rect.top + 86.0f),
                primary.Get());
            d2dContext_->DrawTextW(
                detail.c_str(),
                static_cast<UINT32>(detail.size()),
                captionFormat_.Get(),
                D2D1::RectF(
                    rect.left + 175.0f,
                    rect.top + 91.0f,
                    rect.right - 30.0f,
                    rect.top + 126.0f),
                secondary.Get());
            return;
        }

        const D2D1_POINT_2F iconCenter =
            D2D1::Point2F((hero.left + hero.right) * 0.5f, hero.top + 55.0f);
        d2dContext_->FillEllipse(
            D2D1::Ellipse(iconCenter, 32.0f, 32.0f),
            accentSurface.Get());
        drawWeatherIcon(
            iconCenter,
            weather.weatherCode,
            opacity);

        const std::wstring temperature = std::format(L"{}°", weather.temperature);
        d2dContext_->DrawTextW(
            temperature.c_str(),
            static_cast<UINT32>(temperature.size()),
            weatherHeroFormat_.Get(),
            D2D1::RectF(
                hero.left + 8.0f,
                hero.top + 82.0f,
                hero.right - 8.0f,
                hero.top + 126.0f),
            primary.Get());
        const std::wstring units = weather.metric ? L"C" : L"F";
        d2dContext_->DrawTextW(
            units.c_str(),
            static_cast<UINT32>(units.size()),
            smallFormat_.Get(),
            D2D1::RectF(
                hero.right - 34.0f,
                hero.top + 90.0f,
                hero.right - 12.0f,
                hero.top + 108.0f),
            secondary.Get());
        d2dContext_->DrawTextW(
            weather.condition.c_str(),
            static_cast<UINT32>(weather.condition.size()),
            tabFormat_.Get(),
            D2D1::RectF(
                hero.left + 12.0f,
                hero.bottom - 27.0f,
                hero.right - 12.0f,
                hero.bottom - 7.0f),
            primary.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);

        const std::wstring todayLabel = L"TODAY";
        d2dContext_->DrawTextW(
            todayLabel.c_str(),
            static_cast<UINT32>(todayLabel.size()),
            smallFormat_.Get(),
            D2D1::RectF(
                rect.left + 175.0f,
                rect.top + 18.0f,
                rect.right - 80.0f,
                rect.top + 36.0f),
            accent.Get());
        const std::wstring range =
            std::format(L"High {}°   Low {}°", weather.high, weather.low);
        d2dContext_->DrawTextW(
            range.c_str(),
            static_cast<UINT32>(range.size()),
            titleFormat_.Get(),
            D2D1::RectF(
                rect.left + 175.0f,
                rect.top + 39.0f,
                rect.right - 31.0f,
                rect.top + 61.0f),
            secondary.Get());

        const std::wstring values[] = {
            std::format(L"{}°", weather.feelsLike),
            std::format(L"{}%", weather.humidity),
            std::format(
                L"{} {}",
                weather.windSpeed,
                weather.metric ? L"km/h" : L"mph"),
        };
        static constexpr const wchar_t* labels[] = {
            L"Feels like", L"Humidity", L"Wind",
        };
        for (int index = 0; index < 3; ++index) {
            const float top =
                rect.top + 67.0f + static_cast<float>(index) * 38.0f;
            const FloatRect row{
                rect.left + 173.0f,
                top,
                rect.right - 29.0f,
                top + 31.0f,
            };
            d2dContext_->FillRoundedRectangle(
                D2D1::RoundedRect(row, 11.0f, 11.0f),
                metricTray.Get());
            d2dContext_->FillEllipse(
                D2D1::Ellipse(
                    D2D1::Point2F(row.left + 14.0f, (row.top + row.bottom) * 0.5f),
                    2.2f,
                    2.2f),
                accent.Get());
            d2dContext_->DrawTextW(
                labels[index],
                static_cast<UINT32>(wcslen(labels[index])),
                captionFormat_.Get(),
                D2D1::RectF(
                    row.left + 25.0f,
                    row.top + 7.0f,
                    row.right - 72.0f,
                    row.bottom - 5.0f),
                muted.Get());
            d2dContext_->DrawTextW(
                values[index].c_str(),
                static_cast<UINT32>(values[index].size()),
                tabFormat_.Get(),
                D2D1::RectF(
                    row.right - 70.0f,
                    row.top + 4.0f,
                    row.right - 8.0f,
                    row.bottom - 4.0f),
                primary.Get());
        }
    }

    static std::wstring formatMediaTime(int64_t seconds, bool negative = false) {
        seconds = std::max<int64_t>(0, seconds);
        const int64_t hours = seconds / 3600;
        const int64_t minutes = (seconds % 3600) / 60;
        const int64_t remainingSeconds = seconds % 60;
        const wchar_t* prefix = negative ? L"-" : L"";
        if (hours > 0) {
            return std::format(
                L"{}{}:{:02}:{:02}",
                prefix,
                hours,
                minutes,
                remainingSeconds);
        }
        return std::format(
            L"{}{}:{:02}",
            prefix,
            minutes,
            remainingSeconds);
    }

    void drawMediaPage(float opacity) {
        const auto rect = islandRect();
        const auto mediaSnapshot = mediaController_.snapshot();
        const uint32_t seed = mediaSnapshot.available
            ? mediaSnapshot.accentSeed
            : 0x705CF6u;
        const D2D1_COLOR_F accent = accentFromSeed(seed);

        ComPtr<ID2D1SolidColorBrush> primary;
        ComPtr<ID2D1SolidColorBrush> secondary;
        ComPtr<ID2D1SolidColorBrush> track;
        ComPtr<ID2D1SolidColorBrush> artworkFrame;
        ComPtr<ID2D1SolidColorBrush> volumeButton;
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.98f, 0.98f, 1.0f, opacity),
            &primary);
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(0.58f, 0.60f, 0.65f, opacity),
            &secondary);
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f * opacity),
            &track);
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(
                std::min(accent.r * 0.55f + 0.45f, 1.0f),
                std::min(accent.g * 0.55f + 0.45f, 1.0f),
                std::min(accent.b * 0.55f + 0.45f, 1.0f),
                0.18f * opacity),
            &artworkFrame);
        d2dContext_->CreateSolidColorBrush(
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.055f * opacity),
            &volumeButton);

        const FloatRect art{
            rect.left + 16.0f,
            rect.top + 15.0f,
            rect.left + 88.0f,
            rect.top + 87.0f,
        };
        d2dContext_->FillRoundedRectangle(
            D2D1::RoundedRect(art.expanded(1.5f, 1.5f), 18.5f, 18.5f),
            artworkFrame.Get());
        if (!mediaSnapshot.available) {
            drawAlbumArtwork(art, mediaSnapshot, 17.0f, opacity);
        }

        const std::wstring title = mediaSnapshot.available
            ? mediaSnapshot.title
            : L"Spotify isn’t playing";
        const std::wstring artist = mediaSnapshot.available
            ? mediaSnapshot.artist
            : L"Open Spotify and start a song";
        const float textLeft = art.right + 15.0f;
        const float textRight = rect.right - 92.0f;
        d2dContext_->PushAxisAlignedClip(
            D2D1::RectF(textLeft, rect.top + 20.0f, textRight, rect.top + 78.0f),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        d2dContext_->DrawTextW(
            title.c_str(),
            static_cast<UINT32>(title.size()),
            mediaTitleFormat_.Get(),
            D2D1::RectF(textLeft, rect.top + 25.0f, textRight, rect.top + 49.0f),
            primary.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
        d2dContext_->DrawTextW(
            artist.c_str(),
            static_cast<UINT32>(artist.size()),
            captionFormat_.Get(),
            D2D1::RectF(textLeft, rect.top + 52.0f, textRight, rect.top + 72.0f),
            secondary.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
        d2dContext_->PopAxisAlignedClip();

        drawVisualizer(
            FloatRect{
                rect.right - 79.0f,
                rect.top + 25.0f,
                rect.right - 33.0f,
                rect.top + 62.0f,
            },
            seed,
            opacity);

        const int64_t duration = mediaSnapshot.durationSeconds;
        const int64_t position = std::clamp<int64_t>(
            mediaSnapshot.positionSeconds,
            0,
            std::max<int64_t>(0, duration));
        const float progress = duration > 0
            ? clamp01(static_cast<float>(position) / static_cast<float>(duration))
            : 0.0f;
        const std::wstring elapsed = formatMediaTime(position);
        const std::wstring remaining = duration > 0
            ? formatMediaTime(duration - position, true)
            : L"--:--";
        const FloatRect progressTrack{
            rect.left + 49.0f,
            rect.top + 104.0f,
            rect.right - 72.0f,
            rect.top + 109.0f,
        };
        d2dContext_->DrawTextW(
            elapsed.c_str(),
            static_cast<UINT32>(elapsed.size()),
            tabFormat_.Get(),
            D2D1::RectF(
                rect.left + 13.0f,
                rect.top + 96.0f,
                rect.left + 47.0f,
                rect.top + 117.0f),
            secondary.Get());
        d2dContext_->DrawTextW(
            remaining.c_str(),
            static_cast<UINT32>(remaining.size()),
            tabFormat_.Get(),
            D2D1::RectF(
                rect.right - 70.0f,
                rect.top + 96.0f,
                rect.right - 29.0f,
                rect.top + 117.0f),
            secondary.Get());
        d2dContext_->FillRoundedRectangle(
            D2D1::RoundedRect(progressTrack, 2.5f, 2.5f),
            track.Get());
        const FloatRect progressFill{
            progressTrack.left,
            progressTrack.top,
            progressTrack.left
                + (progressTrack.right - progressTrack.left) * progress,
            progressTrack.bottom,
        };
        if (progress > 0.001f) {
            d2dContext_->FillRoundedRectangle(
                D2D1::RoundedRect(progressFill, 2.5f, 2.5f),
                primary.Get());
        }

        drawMediaButton(
            previousButtonRect(),
            MediaAction::Previous,
            false,
            primary.Get(),
            opacity);
        drawMediaButton(
            playButtonRect(),
            MediaAction::TogglePlayback,
            mediaSnapshot.playing,
            primary.Get(),
            opacity);
        drawMediaButton(
            nextButtonRect(),
            MediaAction::Next,
            false,
            primary.Get(),
            opacity);

        const auto volumeRect = volumeButtonRect();
        const D2D1_POINT_2F volumeCenter = D2D1::Point2F(
            (volumeRect.left + volumeRect.right) * 0.5f,
            (volumeRect.top + volumeRect.bottom) * 0.5f);
        d2dContext_->FillEllipse(
            D2D1::Ellipse(volumeCenter, 20.0f, 20.0f),
            volumeButton.Get());
        drawVolumeIcon(
            volumeCenter,
            primary.Get(),
            opacity,
            muted_);
    }

    void drawMediaButton(
        const FloatRect& rect,
        MediaAction action,
        bool playing,
        ID2D1Brush* brush,
        float opacity) {
        (void)opacity;
        const float cx = (rect.left + rect.right) * 0.5f;
        const float cy = (rect.top + rect.bottom) * 0.5f;

        if (action == MediaAction::TogglePlayback) {
            if (playing) {
                d2dContext_->FillRoundedRectangle(
                    D2D1::RoundedRect(
                        D2D1::RectF(cx - 8.0f, cy - 14.0f, cx - 2.0f, cy + 14.0f),
                        2.0f,
                        2.0f),
                    brush);
                d2dContext_->FillRoundedRectangle(
                    D2D1::RoundedRect(
                        D2D1::RectF(cx + 2.0f, cy - 14.0f, cx + 8.0f, cy + 14.0f),
                        2.0f,
                        2.0f),
                    brush);
            } else {
                const D2D1_POINT_2F points[] = {
                    D2D1::Point2F(cx - 7.0f, cy - 14.0f),
                    D2D1::Point2F(cx + 14.0f, cy),
                    D2D1::Point2F(cx - 7.0f, cy + 14.0f),
                };
                fillTriangle(points, brush);
            }
            return;
        }

        const bool previous = action == MediaAction::Previous;
        if (previous) {
            const D2D1_POINT_2F first[] = {
                D2D1::Point2F(cx - 15.0f, cy),
                D2D1::Point2F(cx - 1.0f, cy - 10.0f),
                D2D1::Point2F(cx - 1.0f, cy + 10.0f),
            };
            const D2D1_POINT_2F second[] = {
                D2D1::Point2F(cx - 1.0f, cy),
                D2D1::Point2F(cx + 13.0f, cy - 10.0f),
                D2D1::Point2F(cx + 13.0f, cy + 10.0f),
            };
            fillTriangle(first, brush);
            fillTriangle(second, brush);
        } else {
            const D2D1_POINT_2F first[] = {
                D2D1::Point2F(cx + 15.0f, cy),
                D2D1::Point2F(cx + 1.0f, cy - 10.0f),
                D2D1::Point2F(cx + 1.0f, cy + 10.0f),
            };
            const D2D1_POINT_2F second[] = {
                D2D1::Point2F(cx + 1.0f, cy),
                D2D1::Point2F(cx - 13.0f, cy - 10.0f),
                D2D1::Point2F(cx - 13.0f, cy + 10.0f),
            };
            fillTriangle(first, brush);
            fillTriangle(second, brush);
        }
    }

    void fillTriangle(const D2D1_POINT_2F (&points)[3], ID2D1Brush* brush) {
        ComPtr<ID2D1PathGeometry> geometry;
        if (FAILED(d2dFactory_->CreatePathGeometry(&geometry))) return;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(geometry->Open(&sink))) return;
        sink->BeginFigure(points[0], D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(points[1]);
        sink->AddLine(points[2]);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (SUCCEEDED(sink->Close())) {
            d2dContext_->FillGeometry(geometry.Get(), brush);
        }
    }

    void drawVolumeIcon(
        D2D1_POINT_2F center,
        ID2D1Brush* brush,
        float opacity,
        bool muted) {
        (void)opacity;
        d2dContext_->FillRectangle(
            D2D1::RectF(center.x - 7.0f, center.y - 3.0f, center.x - 3.0f, center.y + 3.0f),
            brush);

        ComPtr<ID2D1PathGeometry> geometry;
        if (SUCCEEDED(d2dFactory_->CreatePathGeometry(&geometry))) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(geometry->Open(&sink))) {
                sink->BeginFigure(
                    D2D1::Point2F(center.x - 3.0f, center.y - 3.0f),
                    D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(center.x + 2.0f, center.y - 7.0f));
                sink->AddLine(D2D1::Point2F(center.x + 2.0f, center.y + 7.0f));
                sink->AddLine(D2D1::Point2F(center.x - 3.0f, center.y + 3.0f));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                if (SUCCEEDED(sink->Close())) {
                    d2dContext_->FillGeometry(geometry.Get(), brush);
                }
            }
        }

        if (muted) {
            d2dContext_->DrawLine(
                D2D1::Point2F(center.x + 5.0f, center.y - 4.0f),
                D2D1::Point2F(center.x + 11.0f, center.y + 4.0f),
                brush,
                1.5f);
            d2dContext_->DrawLine(
                D2D1::Point2F(center.x + 11.0f, center.y - 4.0f),
                D2D1::Point2F(center.x + 5.0f, center.y + 4.0f),
                brush,
                1.5f);
        } else {
            ComPtr<ID2D1PathGeometry> wave;
            if (SUCCEEDED(d2dFactory_->CreatePathGeometry(&wave))) {
                ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(wave->Open(&sink))) {
                    sink->BeginFigure(
                        D2D1::Point2F(center.x + 5.0f, center.y - 5.0f),
                        D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddBezier(D2D1::BezierSegment(
                        D2D1::Point2F(center.x + 10.0f, center.y - 3.0f),
                        D2D1::Point2F(center.x + 10.0f, center.y + 3.0f),
                        D2D1::Point2F(center.x + 5.0f, center.y + 5.0f)));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    if (SUCCEEDED(sink->Close())) {
                        d2dContext_->DrawGeometry(wave.Get(), brush, 1.5f);
                    }
                }
            }
        }
    }

    void drawMusicNote(
        D2D1_POINT_2F center,
        ID2D1Brush* brush,
        float opacity) {
        (void)opacity;
        d2dContext_->DrawLine(
            D2D1::Point2F(center.x - 2.0f, center.y - 11.0f),
            D2D1::Point2F(center.x - 2.0f, center.y + 7.0f),
            brush,
            3.0f);
        d2dContext_->DrawLine(
            D2D1::Point2F(center.x - 1.0f, center.y - 10.0f),
            D2D1::Point2F(center.x + 10.0f, center.y - 7.0f),
            brush,
            3.0f);
        d2dContext_->DrawLine(
            D2D1::Point2F(center.x + 10.0f, center.y - 8.0f),
            D2D1::Point2F(center.x + 10.0f, center.y + 10.0f),
            brush,
            3.0f);
        d2dContext_->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(center.x - 6.0f, center.y + 8.0f), 5.0f, 4.0f),
            brush);
        d2dContext_->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(center.x + 6.0f, center.y + 11.0f), 5.0f, 4.0f),
            brush);
    }

    void updateStatus() {
        volume_ = spotifyAudioSession_.volume();
        muted_ = spotifyAudioSession_.muted();

        SYSTEM_POWER_STATUS power{};
        if (GetSystemPowerStatus(&power) && power.BatteryLifePercent != 255) {
            batteryPercent_ = static_cast<int>(power.BatteryLifePercent);
        } else {
            batteryPercent_ = -1;
        }

        SYSTEMTIME localTime{};
        GetLocalTime(&localTime);
        wchar_t buffer[32]{};
        if (GetTimeFormatEx(
                LOCALE_NAME_USER_DEFAULT,
                TIME_NOSECONDS,
                &localTime,
                nullptr,
                buffer,
                ARRAYSIZE(buffer))) {
            timeText_ = buffer;
        }
    }

    bool isStartupEnabled() const {
        HKEY key = nullptr;
        if (RegOpenKeyExW(
                HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                0,
                KEY_QUERY_VALUE,
                &key) != ERROR_SUCCESS) {
            return false;
        }
        const LSTATUS status =
            RegQueryValueExW(key, kStartupValue, nullptr, nullptr, nullptr, nullptr);
        RegCloseKey(key);
        return status == ERROR_SUCCESS;
    }

    void setStartupEnabled(bool enabled) const {
        HKEY key = nullptr;
        if (RegCreateKeyExW(
                HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                0,
                nullptr,
                0,
                KEY_SET_VALUE,
                nullptr,
                &key,
                nullptr) != ERROR_SUCCESS) {
            return;
        }

        if (enabled) {
            wchar_t executablePath[MAX_PATH]{};
            GetModuleFileNameW(nullptr, executablePath, ARRAYSIZE(executablePath));
            const std::wstring command = std::format(L"\"{}\"", executablePath);
            RegSetValueExW(
                key,
                kStartupValue,
                0,
                REG_SZ,
                reinterpret_cast<const BYTE*>(command.c_str()),
                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(key, kStartupValue);
        }
        RegCloseKey(key);
    }

    HWND window_ = nullptr;
    UINT pixelWidth_ = 0;
    UINT pixelHeight_ = 0;
    bool rendering_ = false;
    bool expanded_ = false;
    bool hovered_ = false;
    bool trackingMouse_ = false;
    bool muted_ = false;
    int batteryPercent_ = -1;
    float volume_ = 0.0f;
    float visualizerPeak_ = 0.0f;
    float visualizerPhase_ = 0.0f;
    std::array<float, 5> visualizerBands_{};
    bool visualizerSpectrumActive_ = false;
    std::wstring timeText_ = L"--:--";
    Spring width_;
    Spring height_;
    Spring hover_;
    Spring tabIndicator_;
    Spring contentOpacity_;
    IslandTab activeTab_ = IslandTab::Media;
    int calendarYear_ = 2026;
    int calendarMonth_ = 1;
    int calendarSelectedDay_ = 1;
    int wheelAccumulator_ = 0;
    std::chrono::steady_clock::time_point lastFrame_ = std::chrono::steady_clock::now();

    SpotifyAudioSession spotifyAudioSession_;
    SpotifySpectrumCapture spotifySpectrumCapture_;
    MediaController mediaController_;
    WeatherController weatherController_;

    ComPtr<ID3D11Device> d3dDevice_;
    ComPtr<ID3D11DeviceContext> d3dContext_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID2D1Factory3> d2dFactory_;
    ComPtr<ID2D1Device2> d2dDevice_;
    ComPtr<ID2D1DeviceContext2> d2dContext_;
    ComPtr<ID2D1Bitmap1> targetBitmap_;
    ComPtr<IWICImagingFactory> wicFactory_;
    ComPtr<ID2D1Bitmap1> artworkBitmap_;
    uint64_t renderedArtworkKey_ = 0;
    ComPtr<IDWriteFactory> textFactory_;
    std::wstring displayFont_ = L"Segoe UI";
    std::wstring textFont_ = L"Segoe UI";
    ComPtr<IDWriteTextFormat> titleFormat_;
    ComPtr<IDWriteTextFormat> mediaTitleFormat_;
    ComPtr<IDWriteTextFormat> captionFormat_;
    ComPtr<IDWriteTextFormat> smallFormat_;
    ComPtr<IDWriteTextFormat> tabFormat_;
    ComPtr<IDWriteTextFormat> temperatureFormat_;
    ComPtr<IDWriteTextFormat> weatherHeroFormat_;
    ComPtr<IDWriteTextFormat> dateNumberFormat_;
    ComPtr<IDCompositionDevice> compositionDevice_;
    ComPtr<IDCompositionTarget> compositionTarget_;
    ComPtr<IDCompositionVisual> compositionVisual_;
};

DynamicIslandApp* appFromWindow(HWND window) {
    return reinterpret_cast<DynamicIslandApp*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
}

void positionWindow(HWND window) {
    const UINT dpi = GetDpiForWindow(window);
    const int width = MulDiv(static_cast<int>(kCanvasWidth), dpi, 96);
    const int height = MulDiv(static_cast<int>(kCanvasHeight), dpi, 96);

    // The primary monitor's origin is always (0, 0), even when another display
    // sits to its left or above it.
    POINT reference{0, 0};
    HMONITOR monitor = MonitorFromPoint(reference, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    const int x = info.rcWork.left + (info.rcWork.right - info.rcWork.left - width) / 2;
    const int y = info.rcWork.top;

    SetWindowPos(
        window,
        HWND_TOPMOST,
        x,
        y,
        width,
        height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    DynamicIslandApp* app = appFromWindow(window);

    switch (message) {
    case WM_CREATE: {
        gIslandWindow = window;
        auto instance = std::make_unique<DynamicIslandApp>(window);
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(instance.get()));
        const HRESULT result = instance->initializeGraphics();
        if (FAILED(result)) {
            wchar_t error[160]{};
            swprintf_s(
                error,
                L"DirectComposition initialization failed (0x%08X).",
                static_cast<unsigned int>(result));
            MessageBoxW(window, error, kWindowTitle, MB_OK | MB_ICONERROR);
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            return -1;
        }
        instance.release();
        return 0;
    }
    case WM_TIMER:
        if (app) app->onTimer(wParam);
        return 0;
    case kMediaChanged:
        if (app) app->onMediaChanged();
        return 0;
    case kWeatherChanged:
        if (app) app->onWeatherChanged();
        return 0;
    case kOutsideMouseDown:
        if (app) {
            app->onOutsideMouseDown(
                static_cast<LONG>(static_cast<LONG_PTR>(wParam)),
                static_cast<LONG>(lParam));
        }
        return 0;
    case WM_MOUSEMOVE:
        if (app) {
            const float scale = 96.0f / static_cast<float>(GetDpiForWindow(window));
            app->onMouseMove(
                static_cast<float>(GET_X_LPARAM(lParam)) * scale,
                static_cast<float>(GET_Y_LPARAM(lParam)) * scale);
        }
        return 0;
    case WM_MOUSELEAVE:
        if (app) app->onMouseLeave();
        return 0;
    case WM_LBUTTONDOWN:
        if (app) {
            const float scale = 96.0f / static_cast<float>(GetDpiForWindow(window));
            app->onLeftButton(
                static_cast<float>(GET_X_LPARAM(lParam)) * scale,
                static_cast<float>(GET_Y_LPARAM(lParam)) * scale);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (app) app->onMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
    case WM_CONTEXTMENU:
        if (app) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (point.x == -1 && point.y == -1) {
                GetCursorPos(&point);
            }
            app->showContextMenu(point);
        }
        return 0;
    case WM_NCHITTEST:
        if (app) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(window, &point);
            const float scale = 96.0f / static_cast<float>(GetDpiForWindow(window));
            if (!app->pointInIsland(
                    static_cast<float>(point.x) * scale,
                    static_cast<float>(point.y) * scale)) {
                return HTTRANSPARENT;
            }
        }
        return HTCLIENT;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        positionWindow(window);
        if (app) app->onWindowMetricsChanged();
        return 0;
    case WM_DWMCOMPOSITIONCHANGED:
        if (app) app->render();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        if (app) app->render();
        return 0;
    }
    case WM_DESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        delete app;
        gIslandWindow = nullptr;
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const HANDLE singleInstance =
        CreateMutexW(nullptr, TRUE, L"Local\\DynamicIsland.Windows11.Singleton");
    if (!singleInstance || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (singleInstance) CloseHandle(singleInstance);
        return 0;
    }

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        CloseHandle(singleInstance);
        return 1;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClass;
    windowClass.style = CS_HREDRAW | CS_VREDRAW;

    if (!RegisterClassExW(&windowClass)) {
        CoUninitialize();
        CloseHandle(singleInstance);
        return 1;
    }

    const DWORD extendedStyle =
        WS_EX_TOPMOST
        | WS_EX_TOOLWINDOW
        | WS_EX_NOACTIVATE
        | WS_EX_NOREDIRECTIONBITMAP;
    HWND window = CreateWindowExW(
        extendedStyle,
        kWindowClass,
        kWindowTitle,
        WS_POPUP,
        0,
        0,
        static_cast<int>(kCanvasWidth),
        static_cast<int>(kCanvasHeight),
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!window) {
        CoUninitialize();
        CloseHandle(singleInstance);
        return 1;
    }

    positionWindow(window);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    CoUninitialize();
    CloseHandle(singleInstance);
    return static_cast<int>(message.wParam);
}

#include "tvo/audio/VoiceEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#ifdef TVO_PLATFORM_WINDOWS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <mmsystem.h>
#endif

namespace tvo {

namespace {

constexpr int kWireVoiceSampleRate = 16000;
constexpr int kWireVoiceFrameMs = 20;
constexpr int kWireVoiceSamplesPerFrame = kWireVoiceSampleRate * kWireVoiceFrameMs / 1000;
constexpr int kPreSpeechFrameCount = 6;
constexpr int kVadHangoverPumpFrames = 45;
constexpr int kPushToTalkTailPumpFrames = 12;
constexpr int kRemotePlaybackIntervalMs = 18;
constexpr std::size_t kMaxRemoteFramesPerPeer = 6;
constexpr std::size_t kMaxPendingPlaybackBuffers = 12;

void appendVoiceLog(const std::string& line) {
    std::error_code ignored;
    std::filesystem::create_directories("logs", ignored);
    std::ofstream out("logs/voice.log", std::ios::out | std::ios::app);
    if (out) {
        out << line << "\n";
    }
}

}  // namespace

#ifdef TVO_PLATFORM_WINDOWS
struct VoiceEngine::NativeAudioCapture {
    bool start(float inputGain) {
        inputGain_ = inputGain;
        if (openWaveIn(kWireVoiceSampleRate)) {
            return true;
        }

        for (int sampleRate : {48000, 44100}) {
            if (openWaveIn(sampleRate)) {
                return true;
            }
        }

        // WASAPI remains a last-resort meter-only fallback. The push-to-talk
        // voice path needs PCM frames, which waveIn supplies directly.
        if (openWasapiCapture()) {
            return true;
        }
        return false;
    }

    void stop() {
        closeWaveIn();

        if (audioClient_ != nullptr) {
            audioClient_->Stop();
        }
        if (captureClient_ != nullptr) {
            captureClient_->Release();
            captureClient_ = nullptr;
        }
        if (audioClient_ != nullptr) {
            audioClient_->Release();
            audioClient_ = nullptr;
        }
        if (device_ != nullptr) {
            device_->Release();
            device_ = nullptr;
        }
        if (mixFormat_ != nullptr) {
            CoTaskMemFree(mixFormat_);
            mixFormat_ = nullptr;
        }
        if (enumerator_ != nullptr) {
            enumerator_->Release();
            enumerator_ = nullptr;
        }
        if (comInitialized_) {
            CoUninitialize();
            comInitialized_ = false;
        }

        currentLevel_ = 0.0f;
    }

    float pumpLevel(std::array<float, kSpectrumBands>& spectrum) {
        if (captureClient_ != nullptr) {
            const float level = pumpWasapiLevel(spectrum);
            if (level >= 0.0f) {
                currentLevel_ = level;
                return currentLevel_;
            }
        }

        if (handle_ == nullptr) {
            spectrum.fill(0.0f);
            return 0.0f;
        }

        bool sawBuffer = false;
        float level = 0.0f;
        std::array<float, kSpectrumBands> packetSpectrum{};

        for (std::size_t i = 0; i < headers_.size(); ++i) {
            auto& header = headers_[i];
            if ((header.dwFlags & WHDR_DONE) == 0 || header.dwBytesRecorded == 0) {
                continue;
            }

            sawBuffer = true;
            std::array<float, kSpectrumBands> localSpectrum{};
            level = std::max(level, rmsToDisplayLevel(buffers_[i], header.dwBytesRecorded, localSpectrum));
            enqueueVoiceFrames(buffers_[i], header.dwBytesRecorded);
            for (std::size_t band = 0; band < packetSpectrum.size(); ++band) {
                packetSpectrum[band] = std::max(packetSpectrum[band], localSpectrum[band]);
            }
            header.dwBytesRecorded = 0;
            waveInAddBuffer(handle_, &header, sizeof(WAVEHDR));
        }

        if (sawBuffer) {
            currentLevel_ = level;
            lastSpectrum_ = packetSpectrum;
        } else {
            currentLevel_ *= 0.70f;
            if (currentLevel_ < 0.01f) {
                currentLevel_ = 0.0f;
            }
            for (float& band : lastSpectrum_) {
                band *= 0.70f;
                if (band < 0.01f) {
                    band = 0.0f;
                }
            }
        }

        spectrum = lastSpectrum_;
        return currentLevel_;
    }

    std::vector<std::vector<std::int16_t>> drainVoiceFrames() {
        std::vector<std::vector<std::int16_t>> frames;
        frames.swap(pendingVoiceFrames_);
        return frames;
    }

    ~NativeAudioCapture() {
        stop();
    }

private:
    bool openWasapiCapture() {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        comInitialized_ = SUCCEEDED(init);
        if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
            return false;
        }

        HRESULT hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(&enumerator_));
        if (FAILED(hr) || enumerator_ == nullptr) {
            return false;
        }

        hr = enumerator_->GetDefaultAudioEndpoint(eCapture, eCommunications, &device_);
        if (FAILED(hr) || device_ == nullptr) {
            hr = enumerator_->GetDefaultAudioEndpoint(eCapture, eConsole, &device_);
        }
        if (FAILED(hr) || device_ == nullptr) {
            return false;
        }

        hr = device_->Activate(
            __uuidof(IAudioClient),
            CLSCTX_ALL,
            nullptr,
            reinterpret_cast<void**>(&audioClient_));
        if (FAILED(hr) || audioClient_ == nullptr) {
            return false;
        }

        hr = audioClient_->GetMixFormat(&mixFormat_);
        if (FAILED(hr) || mixFormat_ == nullptr) {
            return false;
        }

        constexpr REFERENCE_TIME bufferDuration100ns = 1000000;  // 100 ms.
        hr = audioClient_->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            0,
            bufferDuration100ns,
            0,
            mixFormat_,
            nullptr);
        if (FAILED(hr)) {
            return false;
        }

        hr = audioClient_->GetService(
            __uuidof(IAudioCaptureClient),
            reinterpret_cast<void**>(&captureClient_));
        if (FAILED(hr) || captureClient_ == nullptr) {
            return false;
        }

        return SUCCEEDED(audioClient_->Start());
    }

    float pumpWasapiLevel(std::array<float, kSpectrumBands>& spectrum) {
        UINT32 packetFrames = 0;
        if (FAILED(captureClient_->GetNextPacketSize(&packetFrames))) {
            return -1.0f;
        }

        bool sawPacket = false;
        float maxLevel = 0.0f;
        std::array<float, kSpectrumBands> packetSpectrum{};

        while (packetFrames > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(captureClient_->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                return -1.0f;
            }

            sawPacket = true;
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && data != nullptr && frames > 0) {
                std::array<float, kSpectrumBands> localSpectrum{};
                maxLevel = std::max(maxLevel, samplesToDisplayLevel(data, frames, localSpectrum));
                for (std::size_t band = 0; band < packetSpectrum.size(); ++band) {
                    packetSpectrum[band] = std::max(packetSpectrum[band], localSpectrum[band]);
                }
            }

            captureClient_->ReleaseBuffer(frames);
            if (FAILED(captureClient_->GetNextPacketSize(&packetFrames))) {
                break;
            }
        }

        if (sawPacket) {
            lastSpectrum_ = packetSpectrum;
            spectrum = lastSpectrum_;
            return maxLevel;
        }

        currentLevel_ *= 0.70f;
        for (float& band : lastSpectrum_) {
            band *= 0.70f;
            if (band < 0.01f) {
                band = 0.0f;
            }
        }
        spectrum = lastSpectrum_;
        return currentLevel_ < 0.01f ? 0.0f : currentLevel_;
    }

    void computeSpectrumFromMono(
        const std::vector<float>& mono,
        int sampleRate,
        std::array<float, kSpectrumBands>& spectrum) const {
        spectrum.fill(0.0f);
        if (mono.size() < 32 || sampleRate <= 0) {
            return;
        }

        constexpr double pi = 3.14159265358979323846;
        const std::size_t n = std::min<std::size_t>(mono.size(), 1024);
        constexpr double minHz = 90.0;
        constexpr double maxHz = 5200.0;
        const double ratio = maxHz / minHz;

        for (std::size_t band = 0; band < spectrum.size(); ++band) {
            const double t = static_cast<double>(band) / static_cast<double>(spectrum.size() - 1);
            const double frequency = minHz * std::pow(ratio, t);
            const double step = 2.0 * pi * frequency / static_cast<double>(sampleRate);

            double re = 0.0;
            double im = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                const double window = 0.5 - 0.5 * std::cos(2.0 * pi * static_cast<double>(i) / static_cast<double>(n - 1));
                const double sample = static_cast<double>(mono[i]) * window;
                const double angle = step * static_cast<double>(i);
                re += sample * std::cos(angle);
                im -= sample * std::sin(angle);
            }

            const double magnitude = std::sqrt(re * re + im * im) / static_cast<double>(n);
            const double shaped = std::pow(std::max(0.0, magnitude - 0.0006), 0.35) * 6.2 * inputGain_;
            spectrum[band] = std::clamp(static_cast<float>(shaped), 0.0f, 1.0f);
        }
    }

    float samplesToDisplayLevel(
        const BYTE* data,
        UINT32 frames,
        std::array<float, kSpectrumBands>& spectrum) const {
        if (mixFormat_ == nullptr || frames == 0 || mixFormat_->nChannels == 0) {
            spectrum.fill(0.0f);
            return 0.0f;
        }

        const UINT32 sampleCount = frames * mixFormat_->nChannels;
        if (sampleCount == 0) {
            spectrum.fill(0.0f);
            return 0.0f;
        }

        const bool extensible = mixFormat_->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            mixFormat_->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        const auto* extensibleFormat = extensible
            ? reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mixFormat_)
            : nullptr;

        const bool isFloat = mixFormat_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
            (extensibleFormat != nullptr &&
             IsEqualGUID(extensibleFormat->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
        const bool isPcm = mixFormat_->wFormatTag == WAVE_FORMAT_PCM ||
            (extensibleFormat != nullptr &&
             IsEqualGUID(extensibleFormat->SubFormat, KSDATAFORMAT_SUBTYPE_PCM));

        std::vector<float> mono(frames, 0.0f);
        if (isFloat && mixFormat_->wBitsPerSample == 32) {
            const auto* samples = reinterpret_cast<const float*>(data);
            for (UINT32 frame = 0; frame < frames; ++frame) {
                double sum = 0.0;
                for (UINT32 channel = 0; channel < mixFormat_->nChannels; ++channel) {
                    sum += std::clamp(samples[frame * mixFormat_->nChannels + channel], -1.0f, 1.0f);
                }
                mono[frame] = static_cast<float>(sum / static_cast<double>(mixFormat_->nChannels));
            }
        } else if (isPcm && mixFormat_->wBitsPerSample == 16) {
            const auto* samples = reinterpret_cast<const std::int16_t*>(data);
            for (UINT32 frame = 0; frame < frames; ++frame) {
                double sum = 0.0;
                for (UINT32 channel = 0; channel < mixFormat_->nChannels; ++channel) {
                    sum += static_cast<double>(samples[frame * mixFormat_->nChannels + channel]) /
                        static_cast<double>(std::numeric_limits<std::int16_t>::max());
                }
                mono[frame] = static_cast<float>(sum / static_cast<double>(mixFormat_->nChannels));
            }
        } else if (isPcm && mixFormat_->wBitsPerSample == 24) {
            for (UINT32 frame = 0; frame < frames; ++frame) {
                double sum = 0.0;
                for (UINT32 channel = 0; channel < mixFormat_->nChannels; ++channel) {
                    const BYTE* sampleBytes = data + (frame * mixFormat_->nChannels + channel) * 3;
                    std::int32_t value = sampleBytes[0] |
                        (sampleBytes[1] << 8) |
                        (sampleBytes[2] << 16);
                    if ((value & 0x00800000) != 0) {
                        value |= static_cast<std::int32_t>(0xff000000);
                    }
                    sum += static_cast<double>(value) / 8388607.0;
                }
                mono[frame] = static_cast<float>(sum / static_cast<double>(mixFormat_->nChannels));
            }
        } else if (isPcm && mixFormat_->wBitsPerSample == 32) {
            const auto* samples = reinterpret_cast<const std::int32_t*>(data);
            for (UINT32 frame = 0; frame < frames; ++frame) {
                double sum = 0.0;
                for (UINT32 channel = 0; channel < mixFormat_->nChannels; ++channel) {
                    sum += static_cast<double>(samples[frame * mixFormat_->nChannels + channel]) / 2147483647.0;
                }
                mono[frame] = static_cast<float>(sum / static_cast<double>(mixFormat_->nChannels));
            }
        } else {
            spectrum.fill(0.0f);
            return 0.0f;
        }

        double sumSquares = 0.0;
        for (float sample : mono) {
            sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
        }

        computeSpectrumFromMono(mono, static_cast<int>(mixFormat_->nSamplesPerSec), spectrum);

        const float rms = static_cast<float>(std::sqrt(sumSquares / mono.size()));
        constexpr float noiseFloor = 0.0025f;
        if (rms <= noiseFloor) {
            spectrum.fill(0.0f);
            return 0.0f;
        }

        const float shaped = std::sqrt(std::clamp(rms - noiseFloor, 0.0f, 1.0f));
        return std::clamp(shaped * 5.0f * inputGain_, 0.0f, 1.0f);
    }

    bool openWaveIn(int sampleRate) {
        waveInSampleRate_ = sampleRate;

        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 1;
        format.nSamplesPerSec = static_cast<DWORD>(sampleRate);
        format.wBitsPerSample = 16;
        format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        if (waveInOpen(&handle_, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            handle_ = nullptr;
            return false;
        }

        const std::size_t samplesPerBuffer = static_cast<std::size_t>(sampleRate / 50);
        for (std::size_t i = 0; i < buffers_.size(); ++i) {
            buffers_[i].assign(samplesPerBuffer, 0);
            auto& header = headers_[i];
            header = WAVEHDR{};
            header.lpData = reinterpret_cast<LPSTR>(buffers_[i].data());
            header.dwBufferLength = static_cast<DWORD>(buffers_[i].size() * sizeof(std::int16_t));

            if (waveInPrepareHeader(handle_, &header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR ||
                waveInAddBuffer(handle_, &header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                stop();
                return false;
            }
        }

        if (waveInStart(handle_) != MMSYSERR_NOERROR) {
            stop();
            return false;
        }

        return true;
    }

    void closeWaveIn() {
        if (handle_ == nullptr) {
            return;
        }

        waveInReset(handle_);
        for (auto& header : headers_) {
            if ((header.dwFlags & WHDR_PREPARED) != 0) {
                waveInUnprepareHeader(handle_, &header, sizeof(WAVEHDR));
            }
        }
        waveInClose(handle_);
        handle_ = nullptr;
    }

    float rmsToDisplayLevel(
        const std::vector<std::int16_t>& samples,
        DWORD bytesRecorded,
        std::array<float, kSpectrumBands>& spectrum) const {
        const std::size_t sampleCount = std::min<std::size_t>(
            samples.size(), bytesRecorded / sizeof(std::int16_t));
        if (sampleCount == 0) {
            spectrum.fill(0.0f);
            return 0.0f;
        }

        double sumSquares = 0.0;
        std::vector<float> mono;
        mono.reserve(sampleCount);
        for (std::size_t i = 0; i < sampleCount; ++i) {
            const double sample = static_cast<double>(samples[i]) /
                static_cast<double>(std::numeric_limits<std::int16_t>::max());
            sumSquares += sample * sample;
            mono.push_back(static_cast<float>(sample));
        }

        computeSpectrumFromMono(mono, waveInSampleRate_, spectrum);

        const float rms = static_cast<float>(std::sqrt(sumSquares / sampleCount));
        constexpr float noiseFloor = 0.008f;
        if (rms <= noiseFloor) {
            spectrum.fill(0.0f);
            return 0.0f;
        }

        return std::clamp((rms - noiseFloor) * 8.0f * inputGain_, 0.0f, 1.0f);
    }

    void enqueueVoiceFrames(const std::vector<std::int16_t>& samples, DWORD bytesRecorded) {
        const std::size_t sampleCount = std::min<std::size_t>(
            samples.size(), bytesRecorded / sizeof(std::int16_t));
        if (sampleCount == 0) {
            return;
        }

        pcmAccumulator_.reserve(pcmAccumulator_.size() + sampleCount);
        for (std::size_t i = 0; i < sampleCount; ++i) {
            const int scaled = static_cast<int>(static_cast<float>(samples[i]) * inputGain_);
            pcmAccumulator_.push_back(static_cast<std::int16_t>(std::clamp(scaled, -32768, 32767)));
        }

        while (pcmAccumulator_.size() >= kWireVoiceSamplesPerFrame) {
            pendingVoiceFrames_.emplace_back(
                pcmAccumulator_.begin(),
                pcmAccumulator_.begin() + kWireVoiceSamplesPerFrame);
            pcmAccumulator_.erase(
                pcmAccumulator_.begin(),
                pcmAccumulator_.begin() + kWireVoiceSamplesPerFrame);
        }

        constexpr std::size_t maxBufferedSamples = kWireVoiceSamplesPerFrame * 10;
        if (pcmAccumulator_.size() > maxBufferedSamples) {
            pcmAccumulator_.erase(
                pcmAccumulator_.begin(),
                pcmAccumulator_.end() - maxBufferedSamples);
        }
        if (pendingVoiceFrames_.size() > 25) {
            pendingVoiceFrames_.erase(
                pendingVoiceFrames_.begin(),
                pendingVoiceFrames_.end() - 25);
        }
    }

    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* device_ = nullptr;
    IAudioClient* audioClient_ = nullptr;
    IAudioCaptureClient* captureClient_ = nullptr;
    WAVEFORMATEX* mixFormat_ = nullptr;
    bool comInitialized_ = false;
    HWAVEIN handle_ = nullptr;
    std::array<std::vector<std::int16_t>, 4> buffers_{};
    std::array<WAVEHDR, 4> headers_{};
    std::array<float, kSpectrumBands> lastSpectrum_{};
    std::vector<std::int16_t> pcmAccumulator_;
    std::vector<std::vector<std::int16_t>> pendingVoiceFrames_;
    int waveInSampleRate_ = 16000;
    float inputGain_ = 1.0f;
    float currentLevel_ = 0.0f;
};

struct VoiceEngine::NativeAudioPlayback {
    bool start(float outputGain) {
        outputGain_ = outputGain;
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 1;
        format.nSamplesPerSec = kWireVoiceSampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        return waveOutOpen(&handle_, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR;
    }

    void stop() {
        if (handle_ != nullptr) {
            waveOutReset(handle_);
        }
        cleanup(true);
        if (handle_ != nullptr) {
            waveOutClose(handle_);
            handle_ = nullptr;
        }
    }

    void play(const EncodedVoiceFrame& frame) {
        if (handle_ == nullptr || frame.payload.size() < sizeof(std::int16_t)) {
            return;
        }

        cleanup(false);
        if (pending_.size() >= kMaxPendingPlaybackBuffers) {
            waveOutReset(handle_);
            cleanup(true);
        }

        auto pending = std::make_unique<PendingBuffer>();
        const std::size_t samples = frame.payload.size() / sizeof(std::int16_t);
        pending->samples.resize(samples);
        const auto* input = reinterpret_cast<const std::int16_t*>(frame.payload.data());
        for (std::size_t i = 0; i < samples; ++i) {
            const int scaled = static_cast<int>(static_cast<float>(input[i]) * outputGain_);
            pending->samples[i] = static_cast<std::int16_t>(std::clamp(scaled, -32768, 32767));
        }

        pending->header.lpData = reinterpret_cast<LPSTR>(pending->samples.data());
        pending->header.dwBufferLength = static_cast<DWORD>(pending->samples.size() * sizeof(std::int16_t));

        if (waveOutPrepareHeader(handle_, &pending->header, sizeof(WAVEHDR)) == MMSYSERR_NOERROR &&
            waveOutWrite(handle_, &pending->header, sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
            pending_.push_back(std::move(pending));
        }

        if (pending_.size() > kMaxPendingPlaybackBuffers) {
            cleanup(true);
        }
    }

    ~NativeAudioPlayback() {
        stop();
    }

private:
    struct PendingBuffer {
        WAVEHDR header{};
        std::vector<std::int16_t> samples;
    };

    void cleanup(bool force) {
        if (handle_ == nullptr) {
            pending_.clear();
            return;
        }

        for (auto it = pending_.begin(); it != pending_.end();) {
            auto& pending = **it;
            if (force || (pending.header.dwFlags & WHDR_DONE) != 0) {
                waveOutUnprepareHeader(handle_, &pending.header, sizeof(WAVEHDR));
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }

    HWAVEOUT handle_ = nullptr;
    float outputGain_ = 1.0f;
    std::vector<std::unique_ptr<PendingBuffer>> pending_;
};
#else
struct VoiceEngine::NativeAudioCapture {
    bool start(float) {
        return false;
    }
    void stop() {}
    float pumpLevel(std::array<float, kSpectrumBands>& spectrum) {
        spectrum.fill(0.0f);
        return 0.0f;
    }
    std::vector<std::vector<std::int16_t>> drainVoiceFrames() {
        return {};
    }
};

struct VoiceEngine::NativeAudioPlayback {
    bool start(float) {
        return false;
    }
    void stop() {}
    void play(const EncodedVoiceFrame&) {}
};
#endif

VoiceEngine::~VoiceEngine() {
    stop();
}

void VoiceEngine::setSettings(AudioSettings settings) {
    settings_ = std::move(settings);
}

void VoiceEngine::setTransport(VoiceTransport* transport) noexcept {
    transport_ = transport;
}

void VoiceEngine::setMetricsCallback(MetricsCallback callback) {
    metricsCallback_ = std::move(callback);
}

bool VoiceEngine::start() {
    running_ = true;
    startedAt_ = Clock::now();
    sequence_ = 0;
    transmitHangoverFrames_ = 0;
    pushToTalkTailFrames_ = 0;
    receivedLogCounter_ = 0;
    pushToTalkActive_ = false;
    transmitting_ = false;
    preSpeechFrames_.clear();
    remoteAudio_.clear();
    lastRemoteMix_ = Clock::time_point{};
    metrics_ = VoiceMetrics{};
    micLogCounter_ = 0;
    std::error_code ignored;
    std::filesystem::create_directories("logs", ignored);
    micLog_.open("logs/mic-level.log", std::ios::out | std::ios::trunc);
    if (micLog_) {
        micLog_ << "mic level log: silence should stay near 0, speech should rise\n";
    }
    delete nativeCapture_;
    nativeCapture_ = new NativeAudioCapture();
    if (!nativeCapture_->start(settings_.inputGain)) {
        delete nativeCapture_;
        nativeCapture_ = nullptr;
    }
    delete nativePlayback_;
    nativePlayback_ = new NativeAudioPlayback();
    if (!nativePlayback_->start(settings_.outputGain)) {
        delete nativePlayback_;
        nativePlayback_ = nullptr;
    }
    return true;
}

void VoiceEngine::stop() {
    running_ = false;
    if (nativeCapture_) {
        nativeCapture_->stop();
        delete nativeCapture_;
        nativeCapture_ = nullptr;
    }
    if (nativePlayback_) {
        nativePlayback_->stop();
        delete nativePlayback_;
        nativePlayback_ = nullptr;
    }
    metrics_.micLevel = 0.0f;
    metrics_.spectrum.fill(0.0f);
    metrics_.vadActive = false;
    transmitHangoverFrames_ = 0;
    pushToTalkTailFrames_ = 0;
    transmitting_ = false;
    preSpeechFrames_.clear();
    remoteAudio_.clear();
    lastRemoteMix_ = Clock::time_point{};
    if (micLog_) {
        micLog_.flush();
        micLog_.close();
    }
}

void VoiceEngine::setMuted(bool muted) {
    metrics_.muted = muted;
    if (muted) {
        metrics_.micLevel = 0.0f;
        metrics_.spectrum.fill(0.0f);
        metrics_.vadActive = false;
    }
}

void VoiceEngine::setPushToTalkActive(bool active) {
    pushToTalkActive_ = active;
}

void VoiceEngine::pumpRemotePlayback() {
    if (nativePlayback_ == nullptr || remoteAudio_.empty()) {
        return;
    }

    const auto now = Clock::now();
    if (lastRemoteMix_ != Clock::time_point{} &&
        now - lastRemoteMix_ < std::chrono::milliseconds(kRemotePlaybackIntervalMs)) {
        return;
    }
    lastRemoteMix_ = now;

    std::vector<int> mixed(kWireVoiceSamplesPerFrame, 0);
    std::size_t activeFrames = 0;

    for (auto it = remoteAudio_.begin(); it != remoteAudio_.end();) {
        auto& state = it->second;
        if (state.frames.empty() && now - state.lastReceived > std::chrono::seconds(2)) {
            it = remoteAudio_.erase(it);
            continue;
        }

        if (!state.frames.empty()) {
            auto frame = std::move(state.frames.front());
            state.frames.pop_front();
            const std::size_t samples = std::min(frame.size(), mixed.size());
            for (std::size_t i = 0; i < samples; ++i) {
                mixed[i] += frame[i];
            }
            ++activeFrames;
        }
        ++it;
    }

    if (activeFrames == 0) {
        return;
    }

    const float scale = activeFrames > 1
        ? 1.0f / std::sqrt(static_cast<float>(activeFrames))
        : 1.0f;
    std::vector<std::int16_t> output(kWireVoiceSamplesPerFrame, 0);
    for (std::size_t i = 0; i < output.size(); ++i) {
        const int scaled = static_cast<int>(static_cast<float>(mixed[i]) * scale);
        output[i] = static_cast<std::int16_t>(std::clamp(scaled, -32768, 32767));
    }

    EncodedVoiceFrame mixedFrame;
    mixedFrame.payload.resize(output.size() * sizeof(std::int16_t));
    std::memcpy(mixedFrame.payload.data(), output.data(), mixedFrame.payload.size());
    nativePlayback_->play(mixedFrame);
}

void VoiceEngine::pump() {
    if (!running_) {
        return;
    }

    pumpRemotePlayback();

    if (metrics_.muted) {
        metrics_.micLevel = 0.0f;
        metrics_.spectrum.fill(0.0f);
        metrics_.vadActive = false;
        transmitHangoverFrames_ = 0;
        pushToTalkTailFrames_ = 0;
        transmitting_ = false;
        preSpeechFrames_.clear();
        if (metricsCallback_) {
            metricsCallback_(metrics_);
        }
        return;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - startedAt_).count();

    if (nativeCapture_ != nullptr) {
        metrics_.micLevel = nativeCapture_->pumpLevel(metrics_.spectrum);
    } else {
        metrics_.micLevel = 0.0f;
        metrics_.spectrum.fill(0.0f);
    }
    const bool aboveThreshold = metrics_.micLevel >= settings_.vadThreshold;
    bool shouldTransmit = false;
    if (settings_.pushToTalk) {
        if (pushToTalkActive_) {
            pushToTalkTailFrames_ = kPushToTalkTailPumpFrames;
        } else if (pushToTalkTailFrames_ > 0) {
            --pushToTalkTailFrames_;
        }
        transmitHangoverFrames_ = 0;
        shouldTransmit = pushToTalkActive_ || pushToTalkTailFrames_ > 0;
        metrics_.vadActive = shouldTransmit;
    } else {
        if (aboveThreshold) {
            transmitHangoverFrames_ = kVadHangoverPumpFrames;
        } else if (transmitHangoverFrames_ > 0) {
            --transmitHangoverFrames_;
        }
        pushToTalkTailFrames_ = 0;
        shouldTransmit = aboveThreshold || transmitHangoverFrames_ > 0;
        metrics_.vadActive = shouldTransmit;
    }
    if (micLog_ && (++micLogCounter_ % 10 == 0)) {
        micLog_ << "level=" << metrics_.micLevel
                << " vad=" << (metrics_.vadActive ? 1 : 0)
                << "\n";
        micLog_.flush();
    }

    std::vector<std::vector<std::int16_t>> capturedFrames;
    if (nativeCapture_ != nullptr) {
        capturedFrames = nativeCapture_->drainVoiceFrames();
    }

    auto sendPcmFrame = [this, elapsed](const std::vector<std::int16_t>& pcmFrame) {
        if (transport_ == nullptr || pcmFrame.empty()) {
            return;
        }

        EncodedVoiceFrame frame;
        frame.sequence = sequence_++;
        frame.captureUnixMs = elapsed;
        frame.payload.resize(pcmFrame.size() * sizeof(std::int16_t));
        std::memcpy(frame.payload.data(), pcmFrame.data(), frame.payload.size());
        transport_->sendVoiceFrame(frame);
        metrics_.encodedFrames += 1;
        if (metrics_.encodedFrames <= 3 || metrics_.encodedFrames % 50 == 0) {
            appendVoiceLog("sent voice frame seq=" + std::to_string(frame.sequence) +
                " bytes=" + std::to_string(frame.payload.size()));
        }
    };

    if (shouldTransmit && transport_ != nullptr) {
        if (!transmitting_) {
            for (const auto& pcmFrame : preSpeechFrames_) {
                sendPcmFrame(pcmFrame);
            }
            preSpeechFrames_.clear();
        }
        for (const auto& pcmFrame : capturedFrames) {
            sendPcmFrame(pcmFrame);
        }
        transmitting_ = true;
    } else if (nativeCapture_ != nullptr) {
        transmitting_ = false;
        for (auto& pcmFrame : capturedFrames) {
            preSpeechFrames_.push_back(std::move(pcmFrame));
            while (preSpeechFrames_.size() > kPreSpeechFrameCount) {
                preSpeechFrames_.pop_front();
            }
        }
    }

    if (metricsCallback_) {
        metricsCallback_(metrics_);
    }
}

bool VoiceEngine::running() const noexcept {
    return running_;
}

const VoiceMetrics& VoiceEngine::metrics() const noexcept {
    return metrics_;
}

void VoiceEngine::playRemoteFrame(const EncodedVoiceFrame& frame) {
    if (!running_ || nativePlayback_ == nullptr || frame.payload.empty()) {
        return;
    }
    if (frame.payload.size() < sizeof(std::int16_t)) {
        return;
    }

    const PeerId sourcePeerId = frame.sourcePeerId.empty() ? "_unknown" : frame.sourcePeerId;
    auto& remote = remoteAudio_[sourcePeerId];
    if (remote.hasLastSequence && frame.sequence <= remote.lastSequence) {
        metrics_.droppedFrames += 1;
        return;
    }
    remote.hasLastSequence = true;
    remote.lastSequence = frame.sequence;
    remote.lastReceived = Clock::now();

    const std::size_t samples = frame.payload.size() / sizeof(std::int16_t);
    std::vector<std::int16_t> pcm(samples);
    std::memcpy(pcm.data(), frame.payload.data(), pcm.size() * sizeof(std::int16_t));
    remote.frames.push_back(std::move(pcm));
    while (remote.frames.size() > kMaxRemoteFramesPerPeer) {
        remote.frames.pop_front();
        metrics_.droppedFrames += 1;
    }

    ++receivedLogCounter_;
    if (receivedLogCounter_ <= 3 || receivedLogCounter_ % 50 == 0) {
        appendVoiceLog("received voice frame seq=" + std::to_string(frame.sequence) +
            " bytes=" + std::to_string(frame.payload.size()) +
            " from=" + frame.sourcePeerId);
    }
}

}  // namespace tvo

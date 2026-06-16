#pragma once

#include "tvo/core/Types.h"

#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <vector>

namespace tvo {

struct EncodedVoiceFrame {
    PeerId sourcePeerId;
    std::uint32_t sequence = 0;
    std::int64_t captureUnixMs = 0;
    std::vector<std::uint8_t> payload;
};

struct VoiceMetrics {
    float micLevel = 0.0f;
    std::array<float, kSpectrumBands> spectrum{};
    bool vadActive = false;
    bool muted = false;
    int encodedFrames = 0;
    int droppedFrames = 0;
};

class VoiceTransport {
public:
    virtual ~VoiceTransport() = default;
    virtual void sendVoiceFrame(const EncodedVoiceFrame& frame) = 0;
};

class VoiceEngine {
public:
    using MetricsCallback = std::function<void(const VoiceMetrics&)>;

    ~VoiceEngine();

    void setSettings(AudioSettings settings);
    void setTransport(VoiceTransport* transport) noexcept;
    void setMetricsCallback(MetricsCallback callback);

    bool start();
    void stop();
    void setMuted(bool muted);
    void setPushToTalkActive(bool active);
    void pump();
    void playRemoteFrame(const EncodedVoiceFrame& frame);

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] const VoiceMetrics& metrics() const noexcept;

private:
    struct NativeAudioCapture;
    struct NativeAudioPlayback;

    AudioSettings settings_{};
    VoiceTransport* transport_ = nullptr;
    MetricsCallback metricsCallback_;
    VoiceMetrics metrics_{};
    std::ofstream micLog_;
    int micLogCounter_ = 0;
    NativeAudioCapture* nativeCapture_ = nullptr;
    NativeAudioPlayback* nativePlayback_ = nullptr;
    bool running_ = false;
    std::uint32_t sequence_ = 0;
    int transmitHangoverFrames_ = 0;
    int pushToTalkTailFrames_ = 0;
    int receivedLogCounter_ = 0;
    bool pushToTalkActive_ = false;
    bool transmitting_ = false;
    std::deque<std::vector<std::int16_t>> preSpeechFrames_;
    Clock::time_point startedAt_ = Clock::now();
};

}  // namespace tvo

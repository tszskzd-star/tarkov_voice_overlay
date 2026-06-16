#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tvo {

inline constexpr std::size_t kMaxPeersPerRoom = 5;
inline constexpr int kSampleRateHz = 48000;
inline constexpr int kFrameDurationMs = 20;
inline constexpr int kFrameSamples = kSampleRateHz / 1000 * kFrameDurationMs;
inline constexpr int kTargetLatencyMs = 100;
inline constexpr std::size_t kSpectrumBands = 20;

using PeerId = std::string;
using RoomId = std::string;
using Clock = std::chrono::steady_clock;

enum class ConnectionState {
    Offline,
    Connecting,
    Connected,
    Failed
};

enum class PeerMediaState {
    Silent,
    Speaking,
    Muted,
    Disconnected
};

struct RoomInfo {
    RoomId id;
    std::string name;
    std::string hostPeerId;
    std::string hostNick;
    std::size_t peerCount = 0;
    std::size_t maxPeers = kMaxPeersPerRoom;
    bool locked = false;
    bool lanOnly = false;
    std::int64_t createdAtUnixMs = 0;
    std::vector<PeerId> peerIds;
};

struct PeerStatus {
    PeerId id;
    std::string nick;
    int iconIndex = 0;
    PeerMediaState mediaState = PeerMediaState::Disconnected;
    bool muted = false;
    bool speaking = false;
    float micLevel = 0.0f;
    std::array<float, kSpectrumBands> spectrum{};
    int latencyMs = 0;
    Clock::time_point lastSeen = Clock::now();
};

struct AudioSettings {
    std::string inputDeviceName;
    std::string outputDeviceName;
    float vadThreshold = 0.02f;
    float inputGain = 1.0f;
    float outputGain = 1.0f;
    bool pushToTalk = false;
    int pushToTalkVirtualKey = 0x56;  // V
};

struct HotkeySettings {
    int muteVirtualKey = 0x4D;
    bool ctrl = true;
    bool shift = true;
    bool alt = false;
};

struct AppSettings {
    std::string nick = "Player";
    std::string coordinatorUrl = "ws://127.0.0.1:8080/ws";
    std::string roomPassword;
    int iconIndex = 0;
    AudioSettings audio;
    HotkeySettings muteHotkey;
};

}  // namespace tvo

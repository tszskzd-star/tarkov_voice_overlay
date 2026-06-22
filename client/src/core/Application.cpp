#include "tvo/core/Application.h"
#include "tvo/net/IceConfig.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace tvo {

namespace {

std::optional<int> readPositiveEnvMs(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }

    try {
        const int parsed = std::stoi(value);
        if (parsed > 0) {
            return parsed;
        }
    } catch (...) {
    }

    return std::nullopt;
}

bool envFlagEnabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && std::string(value) == "1";
}

std::string trim(std::string text) {
    auto notSpace = [](unsigned char c) { return c > ' '; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    return text;
}

std::filesystem::path savedAudioSettingsPath() {
    return std::filesystem::current_path() / "tvo_client_settings.ini";
}

void loadSavedAudioSettings(AudioSettings& audio) {
    std::ifstream in(savedAudioSettingsPath());
    if (!in) {
        return;
    }

    std::string line;
    while (std::getline(in, line)) {
        const auto split = line.find('=');
        if (split == std::string::npos) {
            continue;
        }

        const std::string key = trim(line.substr(0, split));
        const std::string value = trim(line.substr(split + 1));
        try {
            if (key == "inputDeviceName") {
                audio.inputDeviceName = value;
            } else if (key == "vadThreshold") {
                audio.vadThreshold = std::stof(value);
            } else if (key == "inputGain") {
                audio.inputGain = std::stof(value);
            } else if (key == "pushToTalk") {
                audio.pushToTalk = value == "1" || value == "true";
            } else if (key == "pushToTalkVirtualKey") {
                audio.pushToTalkVirtualKey = std::stoi(value);
            }
        } catch (...) {
        }
    }
}

void saveAudioSettings(const AudioSettings& audio) {
    std::ofstream out(savedAudioSettingsPath(), std::ios::out | std::ios::trunc);
    if (!out) {
        return;
    }

    out << "inputDeviceName=" << audio.inputDeviceName << "\n";
    out << "vadThreshold=" << audio.vadThreshold << "\n";
    out << "inputGain=" << audio.inputGain << "\n";
    out << "pushToTalk=" << (audio.pushToTalk ? 1 : 0) << "\n";
    out << "pushToTalkVirtualKey=" << audio.pushToTalkVirtualKey << "\n";
}

std::optional<int> readVirtualKeyEnv(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }

    const std::string text = value;
    if (text.size() == 1 && std::isalnum(static_cast<unsigned char>(text[0])) != 0) {
        return std::toupper(static_cast<unsigned char>(text[0]));
    }

    try {
        const int parsed = std::stoi(text, nullptr, 0);
        if (parsed > 0 && parsed < 256) {
            return parsed;
        }
    } catch (...) {
    }
    return std::nullopt;
}

bool shouldInitiateDirectVoiceOffer(const PeerId& localPeerId, const PeerId& remotePeerId) {
    return !localPeerId.empty() && !remotePeerId.empty() && localPeerId < remotePeerId;
}

bool relayFallbackDisabled() {
    return envFlagEnabled("TVO_DISABLE_RELAY_FALLBACK");
}

class HybridVoiceTransport final : public VoiceTransport {
public:
    HybridVoiceTransport(
        DirectVoiceTransport& directVoice,
        SignalingClient& signaling,
        VoiceSession& session,
        bool relayEnabled)
        : directVoice_(directVoice),
          signaling_(signaling),
          session_(session),
          relayEnabled_(relayEnabled) {}

    void sendVoiceFrame(const EncodedVoiceFrame& frame) override {
        directVoice_.sendVoiceFrame(frame);
        if (!relayEnabled_) {
            return;
        }

        const auto& room = session_.currentRoom();
        if (!room) {
            return;
        }

        bool needsRelay = false;
        for (const auto& peer : session_.peers()) {
            if (peer.id.empty() || peer.id == session_.localPeer().id) {
                continue;
            }
            if (!directVoice_.hasPeerConnection(peer.id)) {
                needsRelay = true;
                break;
            }
        }

        if (needsRelay) {
            signaling_.sendVoiceFrame(room->id, frame);
        }
    }

private:
    DirectVoiceTransport& directVoice_;
    SignalingClient& signaling_;
    VoiceSession& session_;
    bool relayEnabled_ = true;
};

}  // namespace

int Application::run() {
    settings_.nick = "Host";
    loadSavedAudioSettings(settings_.audio);
    if (const char* nick = std::getenv("TVO_NICK")) {
        settings_.nick = nick;
    }
    if (const char* coordinatorUrl = std::getenv("TVO_COORDINATOR_URL")) {
        settings_.coordinatorUrl = coordinatorUrl;
    }
    if (const char* password = std::getenv("TVO_ROOM_PASSWORD")) {
        settings_.roomPassword = password;
    }
    if (const char* vadThreshold = std::getenv("TVO_VAD_THRESHOLD")) {
        try {
            settings_.audio.vadThreshold = std::stof(vadThreshold);
        } catch (...) {
        }
    }
    if (const char* inputGain = std::getenv("TVO_INPUT_GAIN")) {
        try {
            settings_.audio.inputGain = std::stof(inputGain);
        } catch (...) {
        }
    }
    if (const char* inputDevice = std::getenv("TVO_INPUT_DEVICE")) {
        settings_.audio.inputDeviceName = inputDevice;
    }
    if (envFlagEnabled("TVO_PUSH_TO_TALK") || envFlagEnabled("TVO_PTT")) {
        settings_.audio.pushToTalk = true;
    }
    if (auto key = readVirtualKeyEnv("TVO_PTT_KEY")) {
        settings_.audio.pushToTalkVirtualKey = *key;
    } else if (auto key = readVirtualKeyEnv("TVO_PUSH_TO_TALK_KEY")) {
        settings_.audio.pushToTalkVirtualKey = *key;
    }

    if (!envFlagEnabled("TVO_SKIP_SETUP")) {
        SetupWindow setup;
        const SetupResult profile = setup.showModal(settings_);
        if (!profile.accepted) {
            return 0;
        }

        settings_.nick = profile.nick;
        settings_.iconIndex = profile.iconIndex;
        settings_.audio.vadThreshold = profile.vadThreshold;
        settings_.audio.inputGain = profile.inputGain;
        settings_.audio.pushToTalk = profile.pushToTalk;
        settings_.audio.pushToTalkVirtualKey = profile.pushToTalkVirtualKey;
    }

    passwordGate_.setPassword(settings_.roomPassword);

    std::cout << "Tarkov Voice Overlay core boot\n";
    std::cout << "Coordinator: " << settings_.coordinatorUrl << "\n";
    std::cout << "ICE servers: " << defaultIceServers().size() << "\n";

    signaling_.setEventCallback([this](const SignalingEvent& event) {
        handleSignalingEvent(event);
    });
    const bool signalingOnline = signaling_.connect(settings_.coordinatorUrl, settings_.nick);
    lan_.start(40770);

    RoomInfo activeRoom;
    bool inServerRoom = false;
    if (signalingOnline) {
        const auto rooms = signaling_.listRooms();
        for (const auto& candidate : rooms) {
            if (!candidate.lanOnly && candidate.peerCount < candidate.maxPeers) {
                if (signaling_.joinRoom(JoinRoomRequest{
                        .roomId = candidate.id,
                        .nick = settings_.nick,
                        .publicKey = "",
                        .passwordProof = ""})) {
                    activeRoom = candidate;
                    session_.joinRoom(activeRoom, signaling_.peerId(), settings_.nick);
                    inServerRoom = true;
                    break;
                }
            }
        }

        if (!inServerRoom) {
            auto room = signaling_.createRoom(CreateRoomRequest{
                .roomName = "Factory squad",
                .hostNick = settings_.nick,
                .locked = passwordGate_.hasPassword()});
            if (!room) {
                std::cerr << "Failed to create room on coordinator\n";
                return 2;
            }
            activeRoom = *room;
            session_.createRoom(activeRoom, signaling_.peerId(), settings_.nick);
            inServerRoom = true;
        }
    } else {
        std::cerr << "Coordinator unavailable, using LAN fallback\n";
        activeRoom = RoomInfo{
            .id = "lan_" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now().time_since_epoch()).count()),
            .name = "LAN squad",
            .hostPeerId = "peer_lan_" + settings_.nick,
            .hostNick = settings_.nick,
            .peerCount = 1,
            .maxPeers = kMaxPeersPerRoom,
            .locked = passwordGate_.hasPassword(),
            .lanOnly = true,
            .createdAtUnixMs = 0};
        session_.createRoom(activeRoom, activeRoom.hostPeerId, settings_.nick);
    }

    session_.setLocalIcon(settings_.iconIndex);
    lan_.announce(activeRoom);

    directVoice_.setSignalCallback([this](const PeerId& targetPeerId,
                                         const std::string& signalType,
                                         const std::string& payloadJson) {
        if (!session_.currentRoom()) {
            return;
        }

        signaling_.sendIce(IceMessage{
            .roomId = session_.currentRoom()->id,
            .targetPeerId = targetPeerId,
            .type = signalType,
            .payloadJson = payloadJson});
    });
    directVoice_.setFrameCallback([this](const EncodedVoiceFrame& frame) {
        voice_.playRemoteFrame(frame);
        session_.markPeerSpeaking(frame.sourcePeerId, 0.65f, 0);
    });
    if (!directVoice_.start(activeRoom.id, session_.localPeer().id)) {
        std::cerr << "Direct voice transport unavailable; relay fallback may be used\n";
    }

    HybridVoiceTransport voiceTransport{
        directVoice_,
        signaling_,
        session_,
        !relayFallbackDisabled()};
    voice_.setSettings(settings_.audio);
    voice_.setTransport(&voiceTransport);
    auto lastStatusBroadcast = Clock::time_point{};
    voice_.setMetricsCallback([this, &lastStatusBroadcast](const VoiceMetrics& metrics) {
        const bool pushToTalkHeld = !settings_.audio.pushToTalk ||
            hotkeys_.isKeyDown(settings_.audio.pushToTalkVirtualKey);
        auto visibleSpectrum = metrics.spectrum;
        if (!pushToTalkHeld) {
            visibleSpectrum.fill(0.0f);
        }
        session_.setLocalMicLevel(pushToTalkHeld ? metrics.micLevel : 0.0f);
        session_.setLocalSpectrum(visibleSpectrum);
        const auto now = Clock::now();
        if (lastStatusBroadcast == Clock::time_point{} ||
            now - lastStatusBroadcast >= std::chrono::milliseconds(150)) {
            lastStatusBroadcast = now;
            signaling_.broadcastStatus(session_.localPeer());
        }
    });
    voice_.start();

    overlay_.create();
    overlay_.show();
    tray_.create();
    if (envFlagEnabled("TVO_SHOW_SETTINGS_ON_START")) {
        tray_.requestSettings();
    }
    hotkeys_.registerMuteHotkey(settings_.muteHotkey, [this] {
        session_.toggleLocalMute();
        voice_.setMuted(session_.localPeer().muted);
        signaling_.broadcastStatus(session_.localPeer());
    });

    const auto smokeExitMs = readPositiveEnvMs("TVO_SMOKE_EXIT_MS");
    const auto appStarted = Clock::now();
    auto lastOverlayRender = Clock::time_point{};
    bool smokeMuteTriggered = false;

    while (!tray_.exitRequested() && (tray_.created() || !overlay_.closeRequested())) {
        if (smokeExitMs && !smokeMuteTriggered) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - appStarted).count();
            if (elapsed > *smokeExitMs / 2) {
                smokeMuteTriggered = true;
                hotkeys_.triggerMuteForSmokeTest();
            }
        }

        if (smokeExitMs) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - appStarted).count();
            if (elapsed >= *smokeExitMs) {
                break;
            }
        }

        hotkeys_.poll();
        if (tray_.consumeSettingsRequest()) {
            audioSettingsWindow_.show(settings_.audio, [this](const AudioSettings& audioSettings) {
                applyAudioSettings(audioSettings);
            });
        }
        signaling_.pump();
        lan_.pump();
        directVoice_.pump();
        voice_.setPushToTalkActive(
            !settings_.audio.pushToTalk ||
            hotkeys_.isKeyDown(settings_.audio.pushToTalkVirtualKey));
        voice_.pump();
        session_.tick();
        const auto now = Clock::now();
        if (lastOverlayRender == Clock::time_point{} ||
            now - lastOverlayRender >= std::chrono::milliseconds(33)) {
            lastOverlayRender = now;
            updateOverlay();
            overlay_.render();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    audioSettingsWindow_.close();
    tray_.destroy();
    voice_.stop();
    directVoice_.stop();
    signaling_.disconnect();
    hotkeys_.unregisterAll();
    return 0;
}

void Application::handleSignalingEvent(const SignalingEvent& event) {
    if (event.type == "joined_room") {
        for (const auto& peerId : event.room.peerIds) {
            if (peerId == session_.localPeer().id) {
                continue;
            }
            PeerStatus peer;
            peer.id = peerId;
            peer.nick = "Player";
            session_.addOrUpdatePeer(peer);
            directVoice_.ensurePeer(
                peerId,
                shouldInitiateDirectVoiceOffer(session_.localPeer().id, peerId));
        }
    } else if (event.type == "peer_status" && !event.peer.id.empty()) {
        session_.addOrUpdatePeer(event.peer);
    } else if (event.type == "peer_joined" && !event.peer.id.empty()) {
        session_.addOrUpdatePeer(event.peer);
        directVoice_.ensurePeer(
            event.peer.id,
            shouldInitiateDirectVoiceOffer(session_.localPeer().id, event.peer.id));
    } else if (event.type == "ice_offer" || event.type == "ice_answer" || event.type == "ice_candidate") {
        directVoice_.handleSignal(event.ice.sourcePeerId, event.ice.type, event.ice.payloadJson);
    } else if (event.type == "voice_frame" && !event.voiceFrame.payload.empty()) {
        if (!directVoice_.hasPeerConnection(event.voiceFrame.sourcePeerId)) {
            voice_.playRemoteFrame(event.voiceFrame);
            session_.markPeerSpeaking(event.voiceFrame.sourcePeerId, 0.55f, 0);
        }
    } else if (event.type == "peer_left" && !event.peer.id.empty()) {
        session_.removePeer(event.peer.id);
        directVoice_.removePeer(event.peer.id);
    } else if (event.type == "room_closed") {
        directVoice_.stop();
        session_.leaveRoom();
    } else if (event.type == "error") {
        std::cerr << "signaling error: " << event.error << "\n";
    }
}

void Application::applyAudioSettings(const AudioSettings& audioSettings) {
    const bool wasRunning = voice_.running();
    const bool wasMuted = session_.localPeer().muted;

    settings_.audio = audioSettings;
    saveAudioSettings(settings_.audio);
    voice_.setSettings(settings_.audio);

    if (wasRunning) {
        voice_.stop();
        voice_.setSettings(settings_.audio);
        voice_.start();
        voice_.setMuted(wasMuted);
    }

    signaling_.broadcastStatus(session_.localPeer());
}

void Application::updateOverlay() {
    overlay_.setRows(makeOverlayRows(session_.peers()));
}

}  // namespace tvo

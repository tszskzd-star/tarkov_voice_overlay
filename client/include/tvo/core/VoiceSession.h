#pragma once

#include "tvo/core/Types.h"

#include <map>
#include <optional>
#include <vector>

namespace tvo {

class VoiceSession {
public:
    bool createRoom(RoomInfo room, PeerId localPeerId, std::string localNick);
    bool joinRoom(RoomInfo room, PeerId localPeerId, std::string localNick);
    void leaveRoom();

    [[nodiscard]] bool inRoom() const noexcept;
    [[nodiscard]] const std::optional<RoomInfo>& currentRoom() const noexcept;
    [[nodiscard]] const PeerStatus& localPeer() const noexcept;
    [[nodiscard]] std::vector<PeerStatus> peers() const;
    [[nodiscard]] bool hasCapacityForPeer() const;

    void addOrUpdatePeer(PeerStatus peer);
    void removePeer(const PeerId& peerId);
    void setLocalMuted(bool muted);
    void setLocalIcon(int iconIndex);
    void toggleLocalMute();
    void setLocalMicLevel(float level);
    void markPeerSpeaking(const PeerId& peerId, float level, int latencyMs);
    void markPeerMuted(const PeerId& peerId, bool muted);
    void setLocalSpectrum(const std::array<float, kSpectrumBands>& spectrum);
    void tick(Clock::time_point now = Clock::now());

private:
    std::optional<RoomInfo> room_;
    PeerStatus localPeer_{};
    std::map<PeerId, PeerStatus> peers_;
};

}  // namespace tvo

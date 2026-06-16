#include "tvo/core/VoiceSession.h"

#include <algorithm>
#include <utility>

namespace tvo {

bool VoiceSession::createRoom(RoomInfo room, PeerId localPeerId, std::string localNick) {
    if (room.maxPeers == 0 || room.maxPeers > kMaxPeersPerRoom) {
        room.maxPeers = kMaxPeersPerRoom;
    }

    room.peerCount = 1;
    room.hostPeerId = localPeerId;
    room.hostNick = localNick;

    room_ = std::move(room);
    peers_.clear();
    localPeer_ = PeerStatus{
        .id = std::move(localPeerId),
        .nick = std::move(localNick),
        .iconIndex = 0,
        .mediaState = PeerMediaState::Silent,
        .muted = false,
        .speaking = false,
        .micLevel = 0.0f,
        .latencyMs = 0,
        .lastSeen = Clock::now()};
    return true;
}

bool VoiceSession::joinRoom(RoomInfo room, PeerId localPeerId, std::string localNick) {
    if (room.peerCount >= room.maxPeers) {
        return false;
    }

    room.peerCount += 1;
    room_ = std::move(room);
    peers_.clear();
    localPeer_ = PeerStatus{
        .id = std::move(localPeerId),
        .nick = std::move(localNick),
        .iconIndex = 0,
        .mediaState = PeerMediaState::Silent,
        .muted = false,
        .speaking = false,
        .micLevel = 0.0f,
        .latencyMs = 0,
        .lastSeen = Clock::now()};
    return true;
}

void VoiceSession::leaveRoom() {
    room_.reset();
    peers_.clear();
    localPeer_ = PeerStatus{};
}

bool VoiceSession::inRoom() const noexcept {
    return room_.has_value();
}

const std::optional<RoomInfo>& VoiceSession::currentRoom() const noexcept {
    return room_;
}

const PeerStatus& VoiceSession::localPeer() const noexcept {
    return localPeer_;
}

std::vector<PeerStatus> VoiceSession::peers() const {
    std::vector<PeerStatus> result;
    if (!localPeer_.id.empty()) {
        result.push_back(localPeer_);
    }
    for (const auto& [_, peer] : peers_) {
        result.push_back(peer);
    }
    return result;
}

bool VoiceSession::hasCapacityForPeer() const {
    if (!room_) {
        return false;
    }
    return peers_.size() + 1 < room_->maxPeers;
}

void VoiceSession::addOrUpdatePeer(PeerStatus peer) {
    if (peer.id.empty() || peer.id == localPeer_.id) {
        return;
    }

    peer.lastSeen = Clock::now();
    peer.mediaState = peer.muted ? PeerMediaState::Muted :
        (peer.speaking ? PeerMediaState::Speaking : PeerMediaState::Silent);
    peers_[peer.id] = std::move(peer);

    if (room_) {
        room_->peerCount = std::min(room_->maxPeers, peers_.size() + 1);
    }
}

void VoiceSession::removePeer(const PeerId& peerId) {
    peers_.erase(peerId);
    if (room_) {
        room_->peerCount = peers_.size() + 1;
    }
}

void VoiceSession::setLocalMuted(bool muted) {
    localPeer_.muted = muted;
    localPeer_.speaking = false;
    localPeer_.micLevel = muted ? 0.0f : localPeer_.micLevel;
    localPeer_.mediaState = muted ? PeerMediaState::Muted : PeerMediaState::Silent;
    localPeer_.lastSeen = Clock::now();
}

void VoiceSession::setLocalIcon(int iconIndex) {
    localPeer_.iconIndex = std::clamp(iconIndex, 0, 4);
    localPeer_.lastSeen = Clock::now();
}

void VoiceSession::toggleLocalMute() {
    setLocalMuted(!localPeer_.muted);
}

void VoiceSession::setLocalMicLevel(float level) {
    level = std::clamp(level, 0.0f, 1.0f);
    localPeer_.micLevel = localPeer_.muted ? 0.0f : level;
    localPeer_.speaking = !localPeer_.muted && level > 0.04f;
    localPeer_.mediaState = localPeer_.muted ? PeerMediaState::Muted :
        (localPeer_.speaking ? PeerMediaState::Speaking : PeerMediaState::Silent);
    localPeer_.lastSeen = Clock::now();
}

void VoiceSession::setLocalSpectrum(const std::array<float, kSpectrumBands>& spectrum) {
    localPeer_.spectrum = spectrum;
    if (localPeer_.muted) {
        localPeer_.spectrum.fill(0.0f);
    }
    localPeer_.lastSeen = Clock::now();
}

void VoiceSession::markPeerSpeaking(const PeerId& peerId, float level, int latencyMs) {
    auto it = peers_.find(peerId);
    if (it == peers_.end()) {
        return;
    }

    auto& peer = it->second;
    peer.micLevel = std::clamp(level, 0.0f, 1.0f);
    peer.speaking = !peer.muted && peer.micLevel > 0.04f;
    peer.mediaState = peer.muted ? PeerMediaState::Muted :
        (peer.speaking ? PeerMediaState::Speaking : PeerMediaState::Silent);
    peer.latencyMs = latencyMs;
    peer.lastSeen = Clock::now();
}

void VoiceSession::markPeerMuted(const PeerId& peerId, bool muted) {
    auto it = peers_.find(peerId);
    if (it == peers_.end()) {
        return;
    }

    auto& peer = it->second;
    peer.muted = muted;
    peer.speaking = false;
    peer.micLevel = muted ? 0.0f : peer.micLevel;
    peer.mediaState = muted ? PeerMediaState::Muted : PeerMediaState::Silent;
    peer.lastSeen = Clock::now();
}

void VoiceSession::tick(Clock::time_point now) {
    constexpr auto staleAfter = std::chrono::seconds(8);

    for (auto& [_, peer] : peers_) {
        if (now - peer.lastSeen > staleAfter) {
            peer.mediaState = PeerMediaState::Disconnected;
            peer.speaking = false;
            peer.micLevel = 0.0f;
        } else if (!peer.muted && !peer.speaking) {
            peer.micLevel = std::max(0.0f, peer.micLevel * 0.85f);
            for (float& band : peer.spectrum) {
                band = std::max(0.0f, band * 0.85f);
            }
        }
    }
}

}  // namespace tvo

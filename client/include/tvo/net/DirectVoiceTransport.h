#pragma once

#include "tvo/audio/VoiceEngine.h"
#include "tvo/core/Types.h"

#include <functional>
#include <memory>
#include <string>

namespace tvo {

class DirectVoiceTransport : public VoiceTransport {
public:
    using SignalCallback = std::function<void(
        const PeerId& targetPeerId,
        const std::string& signalType,
        const std::string& payloadJson)>;
    using FrameCallback = std::function<void(const EncodedVoiceFrame& frame)>;

    DirectVoiceTransport();
    ~DirectVoiceTransport() override;

    DirectVoiceTransport(const DirectVoiceTransport&) = delete;
    DirectVoiceTransport& operator=(const DirectVoiceTransport&) = delete;

    void setSignalCallback(SignalCallback callback);
    void setFrameCallback(FrameCallback callback);

    bool start(RoomId roomId, PeerId localPeerId);
    void stop();
    void pump();

    void ensurePeer(const PeerId& peerId, bool initiateOffer);
    void removePeer(const PeerId& peerId);
    void handleSignal(
        const PeerId& sourcePeerId,
        const std::string& signalType,
        const std::string& payloadJson);

    void sendVoiceFrame(const EncodedVoiceFrame& frame) override;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool hasPeerConnection(const PeerId& peerId) const;
    [[nodiscard]] bool hasAnyPeerConnection() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace tvo

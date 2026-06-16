#pragma once

#include "tvo/audio/VoiceEngine.h"
#include "tvo/core/Types.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace tvo {

struct CreateRoomRequest {
    std::string roomName;
    std::string hostNick;
    bool locked = false;
};

struct JoinRoomRequest {
    RoomId roomId;
    std::string nick;
    std::string publicKey;
    std::string passwordProof;
};

struct IceMessage {
    RoomId roomId;
    PeerId sourcePeerId;
    PeerId targetPeerId;
    std::string type;
    std::string payloadJson;
};

struct SignalingEvent {
    std::string type;
    RoomInfo room;
    PeerStatus peer;
    IceMessage ice;
    EncodedVoiceFrame voiceFrame;
    std::string error;
};

class SignalingClient {
public:
    using EventCallback = std::function<void(const SignalingEvent&)>;

    void setEventCallback(EventCallback callback);
    bool connect(std::string url, std::string nick);
    void disconnect();

    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] const PeerId& peerId() const noexcept;
    [[nodiscard]] std::vector<RoomInfo> listRooms() const;

    std::optional<RoomInfo> createRoom(const CreateRoomRequest& request);
    bool joinRoom(const JoinRoomRequest& request);
    void leaveRoom();
    void sendIce(const IceMessage& message);
    void sendVoiceFrame(const RoomId& roomId, const EncodedVoiceFrame& frame);
    void broadcastStatus(const PeerStatus& status);
    void pump();

private:
    void emit(SignalingEvent event) const;
    bool sendText(const std::string& text);
    void receiveLoop();
    void handleServerMessage(const std::string& text);
    void queueEvent(SignalingEvent event);
    void closeHandles();

    EventCallback callback_;
    std::atomic_bool connected_{false};
    std::atomic_bool stopRequested_{false};
    std::string url_;
    PeerId peerId_;
    std::string nick_;

    mutable std::mutex mutex_;
    std::mutex sendMutex_;
    std::condition_variable cv_;
    std::deque<SignalingEvent> events_;
    std::vector<RoomInfo> rooms_;
    std::optional<RoomId> currentRoomId_;
    std::optional<RoomInfo> pendingCreatedRoom_;
    std::optional<RoomInfo> pendingJoinedRoom_;
    std::optional<std::string> pendingError_;
    bool receivedHello_ = false;
    bool receivedRooms_ = false;
    std::thread receiverThread_;

    void* sessionHandle_ = nullptr;
    void* connectionHandle_ = nullptr;
    void* websocketHandle_ = nullptr;
};

}  // namespace tvo

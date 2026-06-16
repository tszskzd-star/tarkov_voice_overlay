#pragma once

#include "tvo/core/Types.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace tvo {

struct LanAnnouncement {
    RoomInfo room;
    std::string hostAddress;
    std::uint16_t signalingPort = 0;
};

class LanDiscovery {
public:
    using RoomsCallback = std::function<void(std::vector<LanAnnouncement>)>;

    void setRoomsCallback(RoomsCallback callback);
    bool start(std::uint16_t port);
    void stop();
    void announce(RoomInfo room);
    void pump();

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::vector<LanAnnouncement> rooms() const;

private:
    RoomsCallback callback_;
    bool running_ = false;
    std::uint16_t port_ = 0;
    std::vector<LanAnnouncement> rooms_;
};

}  // namespace tvo

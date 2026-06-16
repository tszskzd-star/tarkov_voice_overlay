#include "tvo/net/LanDiscovery.h"

#include <utility>

namespace tvo {

void LanDiscovery::setRoomsCallback(RoomsCallback callback) {
    callback_ = std::move(callback);
}

bool LanDiscovery::start(std::uint16_t port) {
    port_ = port;
    running_ = true;
    return true;
}

void LanDiscovery::stop() {
    running_ = false;
    rooms_.clear();
}

void LanDiscovery::announce(RoomInfo room) {
    if (!running_) {
        return;
    }

    room.lanOnly = true;
    rooms_.push_back(LanAnnouncement{
        .room = std::move(room),
        .hostAddress = "255.255.255.255",
        .signalingPort = port_});
}

void LanDiscovery::pump() {
    if (running_ && callback_) {
        callback_(rooms_);
    }
}

bool LanDiscovery::running() const noexcept {
    return running_;
}

std::vector<LanAnnouncement> LanDiscovery::rooms() const {
    return rooms_;
}

}  // namespace tvo


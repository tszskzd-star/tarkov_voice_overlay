#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tvo {

struct IceServer {
    std::string url;
    std::string username;
    std::string credential;
};

inline constexpr std::string_view kDefaultGoogleStun = "stun:stun.l.google.com:19302";

#ifndef TVO_METERED_TURN_URL
#define TVO_METERED_TURN_URL ""
#endif

#ifndef TVO_METERED_TURN_USERNAME
#define TVO_METERED_TURN_USERNAME ""
#endif

#ifndef TVO_METERED_TURN_CREDENTIAL
#define TVO_METERED_TURN_CREDENTIAL ""
#endif

inline std::vector<IceServer> defaultIceServers() {
    std::vector<IceServer> servers;
    servers.push_back(IceServer{std::string(kDefaultGoogleStun), {}, {}});

    std::string turnUrl = TVO_METERED_TURN_URL;
    if (!turnUrl.empty()) {
        servers.push_back(IceServer{
            std::move(turnUrl),
            TVO_METERED_TURN_USERNAME,
            TVO_METERED_TURN_CREDENTIAL});
    }

    return servers;
}

}  // namespace tvo

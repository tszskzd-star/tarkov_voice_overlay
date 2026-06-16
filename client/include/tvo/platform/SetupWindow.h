#pragma once

#include "tvo/core/Types.h"

#include <string>

namespace tvo {

struct SetupResult {
    bool accepted = false;
    std::string nick = "Player";
    int iconIndex = 0;
    float vadThreshold = 0.02f;
    float inputGain = 1.0f;
    bool pushToTalk = false;
    int pushToTalkVirtualKey = 0x56;
};

class SetupWindow {
public:
    SetupResult showModal(const AppSettings& defaults);
};

}  // namespace tvo
